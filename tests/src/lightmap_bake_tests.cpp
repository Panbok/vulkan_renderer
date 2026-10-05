#include "lightmap_bake_tests.h"

extern "C" {
#include "assets/vkr_lightmap_set.h"
#include "core/vkr_hash.h"
}
#include "bake/vkr_bake_lightmap.h"
#include "bake/vkr_bake_mesh_decode.h"
#include "mesh_cooked_tests.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

/* Packing, texel rasterization and the cooked-mesh path into them (ADR-087).
 * The oracles are geometric: rectangles never overlap and stay on their page,
 * a texel's world position lies on the surface its UVs came from, and every
 * texel is claimed once. Page composition keeps each rectangle's fill inside
 * it, the page encoding keeps HDR values above one, and a lightmap set file
 * round-trips while a corrupted payload or malformed table is rejected. */

namespace {

Mat4 scale_matrix(float32_t scale) {
  Mat4 m = mat4_identity();
  m.elements[0] = scale;
  m.elements[5] = scale;
  m.elements[10] = scale;
  return m;
}

VkrBakeLightmapInstance make_instance(uint32_t index, uint32_t width,
                                      uint32_t height, float32_t scale) {
  VkrBakeLightmapInstance instance;
  instance.source_instance_index = index;
  instance.world = scale_matrix(scale);
  instance.atlas_width = width;
  instance.atlas_height = height;
  instance.texels_per_unit = 8.0f;
  return instance;
}

bool rects_overlap(const VkrBakeLightmapRect &a, const VkrBakeLightmapRect &b) {
  return a.page == b.page && a.x < b.x + b.width && b.x < a.x + a.width &&
         a.y < b.y + b.height && b.y < a.y + a.height;
}

void test_pack_sizes_and_places_rects() {
  printf("  test_pack_sizes_and_places_rects...\n");
  std::vector<VkrBakeLightmapInstance> instances = {
      make_instance(0u, 64u, 64u, 1.0f),
      make_instance(3u, 64u, 64u, 2.0f), // twice the world size
      make_instance(1u, 20u, 12u, 1.0f),
      make_instance(2u, 0u, 0u, 1.0f),    // no atlas: unlit
      make_instance(4u, 600u, 300u, 1.0f) // larger than a page
  };
  VkrBakeLightmapLayout layout;
  assert(vkr_bake_lightmap_pack(instances, 256u, 8.0f, &layout));
  assert(layout.rect_by_instance.size() == 5u);
  assert(layout.rect_by_instance[2] == UINT32_MAX);
  const VkrBakeLightmapRect &unit = layout.rects[layout.rect_by_instance[0]];
  const VkrBakeLightmapRect &doubled = layout.rects[layout.rect_by_instance[3]];
  const VkrBakeLightmapRect &small = layout.rects[layout.rect_by_instance[1]];
  const VkrBakeLightmapRect &large = layout.rects[layout.rect_by_instance[4]];
  assert(unit.width == 64u && unit.height == 64u);
  assert(doubled.width == 128u && doubled.height == 128u);
  assert(small.width == 20u && small.height == 12u);
  assert(large.width == 256u && large.height == 128u);
  for (size_t i = 0; i < layout.rects.size(); ++i) {
    const VkrBakeLightmapRect &rect = layout.rects[i];
    assert(rect.page < layout.page_count);
    assert(rect.x + rect.width <= layout.page_size);
    assert(rect.y + rect.height <= layout.page_size);
    for (size_t j = i + 1u; j < layout.rects.size(); ++j)
      assert(!rects_overlap(rect, layout.rects[j]));
  }
  VkrBakeLightmapLayout rejected;
  assert(!vkr_bake_lightmap_pack(instances, 250u, 8.0f, &rejected));
  assert(!vkr_bake_lightmap_pack(instances, 256u, 0.0f, &rejected));
  printf("  test_pack_sizes_and_places_rects PASSED\n");
}

VkrBakeVertex quad_vertex(float32_t x, float32_t y) {
  VkrBakeVertex vertex = {};
  vertex.position = vec3_new(2.0f * x, 3.0f * y, 1.0f);
  vertex.normal = vec3_new(0.0f, 0.0f, 1.0f);
  vertex.lightmap_uv = vec2_new(x, y);
  return vertex;
}

void test_rasterize_covers_a_quad_once() {
  printf("  test_rasterize_covers_a_quad_once...\n");
  /* Two triangles cover instance 0's whole 8x8 rectangle; instance 1 has no
     rectangle and must not produce texels. */
  VkrBakeTriangle triangles[3] = {};
  triangles[0].vertex[0] = quad_vertex(0.0f, 0.0f);
  triangles[0].vertex[1] = quad_vertex(1.0f, 0.0f);
  triangles[0].vertex[2] = quad_vertex(1.0f, 1.0f);
  triangles[1].vertex[0] = quad_vertex(0.0f, 0.0f);
  triangles[1].vertex[1] = quad_vertex(1.0f, 1.0f);
  triangles[1].vertex[2] = quad_vertex(0.0f, 1.0f);
  triangles[2] = triangles[0];
  triangles[2].source_instance_index = 1u;
  std::vector<VkrBakeLightmapInstance> instances = {
      make_instance(0u, 8u, 8u, 1.0f)};
  VkrBakeLightmapLayout layout;
  assert(vkr_bake_lightmap_pack(instances, 64u, 8.0f, &layout));
  std::vector<VkrBakeLightmapTexel> texels;
  assert(vkr_bake_lightmap_rasterize_page(triangles, 3u, layout, 0u, &texels));
  assert(texels.size() == 64u);
  const VkrBakeLightmapRect &rect = layout.rects[0];
  for (size_t i = 0; i < texels.size(); ++i) {
    const VkrBakeLightmapTexel &texel = texels[i];
    assert(texel.triangle_index < 2u);
    const float32_t u = ((float32_t)(texel.x - rect.x) + 0.5f) / 8.0f;
    const float32_t v = ((float32_t)(texel.y - rect.y) + 0.5f) / 8.0f;
    assert(fabsf(texel.position.x - 2.0f * u) < 1.0e-5f);
    assert(fabsf(texel.position.y - 3.0f * v) < 1.0e-5f);
    assert(fabsf(texel.position.z - 1.0f) < 1.0e-6f);
    assert(fabsf(texel.normal.z - 1.0f) < 1.0e-6f);
    if (i > 0u) {
      const VkrBakeLightmapTexel &previous = texels[i - 1u];
      assert(previous.y < texel.y ||
             (previous.y == texel.y && previous.x < texel.x));
    }
  }
  printf("  test_rasterize_covers_a_quad_once PASSED\n");
}

struct DecodedCube {
  std::vector<VkrBakeTriangle> triangles;
  std::vector<VkrBakeLightmapInstance> instances;
};

bool8_t collect_triangle(void *user, const VkrBakeTriangle *triangle,
                         String8 material_path) {
  (void)material_path;
  static_cast<DecodedCube *>(user)->triangles.push_back(*triangle);
  return true_v;
}

bool8_t collect_instance(void *user, const VkrBakeMeshInstance *instance) {
  VkrBakeLightmapInstance lightmap;
  lightmap.source_instance_index = instance->source_instance_index;
  lightmap.world = instance->world;
  lightmap.atlas_width = instance->atlas_width;
  lightmap.atlas_height = instance->atlas_height;
  lightmap.texels_per_unit = instance->texels_per_unit;
  static_cast<DecodedCube *>(user)->instances.push_back(lightmap);
  return true_v;
}

void test_cooked_cube_texels_lie_on_its_faces() {
  printf("  test_cooked_cube_texels_lie_on_its_faces...\n");
  uint64_t size = 0u;
  uint8_t *bytes = test_cook_lightmap_cube(16.0f, &size);
  DecodedCube cube;
  const VkrBakeMeshDecodeCallbacks callbacks = {
      .emit_triangle = collect_triangle,
      .emit_light = nullptr,
      .emit_instance = collect_instance,
  };
  uint32_t next_instance = 0u;
  assert(vkr_bake_mesh_decode(bytes, size, mat4_identity(), &next_instance,
                              &callbacks, &cube));
  free(bytes);
  assert(cube.instances.size() == 2u && cube.triangles.size() == 12u);
  for (const VkrBakeLightmapInstance &instance : cube.instances) {
    assert(instance.atlas_width > 0u && instance.atlas_height > 0u);
    assert(instance.texels_per_unit == 16.0f);
  }

  VkrBakeLightmapLayout layout;
  assert(vkr_bake_lightmap_pack(cube.instances, 256u, 16.0f, &layout));
  size_t texel_count = 0u;
  size_t rect_area = 0u;
  uint32_t faces_seen = 0u;
  for (const VkrBakeLightmapRect &rect : layout.rects)
    rect_area += (size_t)rect.width * rect.height;
  for (uint32_t page = 0u; page < layout.page_count; ++page) {
    std::vector<VkrBakeLightmapTexel> texels;
    assert(vkr_bake_lightmap_rasterize_page(cube.triangles.data(),
                                            (uint32_t)cube.triangles.size(),
                                            layout, page, &texels));
    for (const VkrBakeLightmapTexel &texel : texels) {
      /* On a face: one coordinate is at +-0.5 and the normal points along
         that axis, outward. */
      const float32_t p[3] = {texel.position.x, texel.position.y,
                              texel.position.z};
      const float32_t n[3] = {texel.normal.x, texel.normal.y, texel.normal.z};
      uint32_t axis = 0u;
      for (uint32_t a = 1u; a < 3u; ++a)
        if (fabsf(n[a]) > fabsf(n[axis]))
          axis = a;
      assert(fabsf(fabsf(n[axis]) - 1.0f) < 1.0e-3f);
      assert(fabsf(p[axis] - 0.5f * n[axis]) < 1.0e-3f);
      for (uint32_t a = 0u; a < 3u; ++a)
        assert(fabsf(p[a]) <= 0.5f + 1.0e-3f);
      faces_seen |= 1u << (2u * axis + (n[axis] < 0.0f ? 1u : 0u));
    }
    texel_count += texels.size();
  }
  /* Every face is lit by at least the requested 16 texels per unit (1,536
     texel centers over six unit faces); xatlas may size charts above it but
     never past their rectangles. */
  assert(faces_seen == 0x3fu);
  assert(texel_count >= 1536u && texel_count <= rect_area);
  printf("  test_cooked_cube_texels_lie_on_its_faces PASSED (%zu texels)\n",
         texel_count);
}

/* A few small instances share one page sized to them instead of a full
   page; a rectangle wider than every smaller page keeps the full size rather
   than being scaled down. */
void test_pack_fitted_shrinks_small_scenes() {
  const std::vector<VkrBakeLightmapInstance> small = {
      make_instance(0u, 60u, 40u, 1.0f), make_instance(1u, 30u, 30u, 1.0f)};
  VkrBakeLightmapLayout layout;
  assert(vkr_bake_lightmap_pack_fitted(small, 4096u, 8.0f, &layout));
  assert(layout.page_count == 1u && layout.page_size == 256u);
  assert(layout.rects[layout.rect_by_instance[0]].width == 60u);

  const std::vector<VkrBakeLightmapInstance> wide = {
      make_instance(0u, 3000u, 8u, 1.0f)};
  assert(vkr_bake_lightmap_pack_fitted(wide, 4096u, 8.0f, &layout));
  assert(layout.page_count == 1u && layout.page_size == 4096u);
  assert(layout.rects[0].width == 3000u);
  printf("  test_pack_fitted_shrinks_small_scenes PASSED\n");
}

/* Two adjacent rectangles with one covered texel each: dilation and the
   rectangle mean fill each rectangle with its own value, nothing crosses the
   shared edge, and texels outside both stay zero. */
void test_compose_fills_each_rect_alone() {
  VkrBakeLightmapLayout layout;
  layout.page_size = 8u;
  layout.page_count = 1u;
  layout.rects = {{0u, 0u, 0u, 0u, 4u, 4u}, {1u, 0u, 4u, 0u, 4u, 4u}};
  layout.rect_by_instance = {0u, 1u};
  VkrBakeLightmapTexel left;
  left.x = 1u;
  left.y = 1u;
  VkrBakeLightmapTexel right;
  right.x = 6u;
  right.y = 2u;
  const std::vector<VkrBakeLightmapTexel> texels = {left, right};
  const std::vector<Vec3> values = {vec3_new(2.0f, 0.0f, 0.0f),
                                    vec3_new(0.0f, 0.0f, 3.0f)};
  std::vector<float32_t> rgba;
  assert(vkr_bake_lightmap_compose_page(layout, 0u, texels, values, 1u, &rgba));
  assert(rgba.size() == 8u * 8u * 4u);
  for (uint32_t y = 0u; y < 8u; ++y) {
    for (uint32_t x = 0u; x < 8u; ++x) {
      const float32_t *texel = &rgba[4u * (y * 8u + x)];
      Vec3 expected = vec3_zero();
      if (y < 4u) {
        expected = x < 4u ? values[0] : values[1];
      }
      assert(fabsf(texel[0] - expected.x) < 1.0e-6f);
      assert(fabsf(texel[1] - expected.y) < 1.0e-6f);
      assert(fabsf(texel[2] - expected.z) < 1.0e-6f);
      assert(texel[3] == 1.0f);
    }
  }
  printf("  test_compose_fills_each_rect_alone PASSED\n");
}

/* A smooth ramp from 0.25 to 40 survives the ASTC 4x4 HDR round trip within
   a few percent; an LDR profile would clamp everything above one. */
void test_astc_hdr_round_trip_keeps_range() {
  const uint32_t size = 16u;
  std::vector<float32_t> rgba(size * size * 4u);
  for (uint32_t y = 0u; y < size; ++y) {
    for (uint32_t x = 0u; x < size; ++x) {
      const float32_t t = (float32_t)(y * size + x) / (size * size - 1u);
      const float32_t value = 0.25f * powf(160.0f, t);
      float32_t *texel = &rgba[4u * (y * size + x)];
      texel[0] = value;
      texel[1] = 0.5f * value;
      texel[2] = 0.25f * value;
      texel[3] = 1.0f;
    }
  }
  std::vector<uint8_t> blocks;
  assert(vkr_bake_lightmap_encode_astc_hdr(rgba, size, 10.0f, 2u, &blocks));
  assert(blocks.size() == (size / 4u) * (size / 4u) * 16u);
  std::vector<float32_t> decoded;
  assert(vkr_bake_lightmap_decode_astc_hdr(blocks, size, &decoded));
  float32_t largest = 0.0f;
  for (size_t i = 0u; i < rgba.size(); ++i) {
    if (i % 4u == 3u) {
      continue;
    }
    const float32_t error = fabsf(decoded[i] - rgba[i]) / rgba[i];
    largest = fmaxf(largest, error);
  }
  assert(decoded[4u * (size * size - 1u)] > 30.0f);
  assert(largest < 0.05f);
  printf("  test_astc_hdr_round_trip_keeps_range PASSED (max error %.4f)\n",
         largest);
}

/* Writes a two-layer, one-page set with distinct payload bytes the way a
   producer streams it: payload CRC first, prefix last. */
std::vector<uint8_t> write_lightmap_set(const VkrLightmapSet &set) {
  uint64_t payload_offset = 0u;
  uint64_t file_size = 0u;
  assert(vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  std::vector<uint8_t> file(file_size, 0u);
  for (uint64_t i = payload_offset; i < file_size; ++i) {
    file[i] = (uint8_t)(i * 31u + 7u);
  }
  const uint32_t payload_crc =
      vkr_crc32(file.data() + payload_offset, file_size - payload_offset);
  assert(vkr_lightmap_set_write_prefix(&set, payload_crc, file.data(),
                                       payload_offset));
  return file;
}

void test_lightmap_set_round_trip_and_rejects() {
  const VkrLightmapLayer layers[] = {
      {VKR_LIGHTMAP_LAYER_SUN_KEY, 0u, vec3_new(0.0f, 0.6f, 0.8f)},
      {VKR_LIGHTMAP_LAYER_LAMP_GROUP, 0u, vec3_zero()}};
  const VkrLightmapInstance instances[] = {
      {{0u}, 2u, 0u, 0u, 0u, 0u, 8u, 4u},
      {{0u}, 2u, 3u, 0u, 8u, 0u, 4u, 4u},
      {{0xabu, 0x01u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0x7fu},
       5u,
       0u,
       0u,
       0u,
       4u,
       12u,
       12u}};
  VkrLightmapSet set = {};
  set.page_size = 16u;
  set.page_count = 1u;
  set.layer_count = 2u;
  set.instance_count = 3u;
  set.texels_per_unit = 8.0f;
  set.layers = layers;
  set.instances = instances;
  const std::vector<uint8_t> file = write_lightmap_set(set);
  assert(file.size() % 4u == 0u);

  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrLightmapSet decoded = {};
  assert(vkr_lightmap_set_decode(file.data(), file.size(), arena, &decoded));
  assert(decoded.page_size == 16u && decoded.page_count == 1u);
  assert(decoded.layer_count == 2u && decoded.instance_count == 3u);
  assert(decoded.layers[0].kind == VKR_LIGHTMAP_LAYER_SUN_KEY);
  assert(decoded.layers[0].sun_direction.z == 0.8f);
  assert(decoded.instances[2].entity_index == 5u);
  assert(decoded.instances[2].width == 12u);
  assert(decoded.instances[2].document_id[0] == 0xabu &&
         decoded.instances[2].document_id[15] == 0x7fu);
  const uint64_t page_bytes = vkr_lightmap_set_page_bytes(16u);
  assert(page_bytes == 16u * 16u);
  assert(decoded.payload + 2u * page_bytes == file.data() + file.size());

  std::vector<uint8_t> corrupt = file;
  corrupt[corrupt.size() - 1u] ^= 1u;
  assert(!vkr_lightmap_set_decode(corrupt.data(), corrupt.size(), arena,
                                  &decoded));
  assert(
      !vkr_lightmap_set_decode(file.data(), file.size() - 4u, arena, &decoded));

  VkrLightmapInstance unaligned[] = {instances[0], instances[1], instances[2]};
  unaligned[1].x = 6u;
  set.instances = unaligned;
  uint64_t payload_offset = 0u;
  uint64_t file_size = 0u;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  const VkrLightmapInstance unsorted[] = {instances[1], instances[0],
                                          instances[2]};
  set.instances = unsorted;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  arena_destroy(arena);
  printf("  test_lightmap_set_round_trip_and_rejects PASSED\n");
}

} // namespace

bool32_t run_lightmap_bake_tests(void) {
  printf("--- Starting Lightmap Bake Tests ---\n");
  test_pack_sizes_and_places_rects();
  test_rasterize_covers_a_quad_once();
  test_cooked_cube_texels_lie_on_its_faces();
  test_pack_fitted_shrinks_small_scenes();
  test_compose_fills_each_rect_alone();
  test_astc_hdr_round_trip_keeps_range();
  test_lightmap_set_round_trip_and_rejects();
  printf("--- Lightmap Bake Tests Completed ---\n");
  return true_v;
}

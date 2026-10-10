#include "lightmap_bake_tests.h"

extern "C" {
#include "assets/vkr_diffuse_volume.h"
#include "assets/vkr_lightmap_set.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
}
#include "bake/vkr_bake_bricks.h"
#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_integrator.h"
#include "bake/vkr_bake_lightmap.h"
#include "bake/vkr_bake_material.h"
#include "bake/vkr_bake_mesh_decode.h"
#include "bake/vkr_bake_scene.h"
#include "bake/vkr_bake_sh.h"
#include "mesh_cooked_tests.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

/* Packing, texel rasterization and the cooked-mesh path into them (ADR-087).
 * The oracles are geometric: rectangles never overlap and stay on their page,
 * a texel's world position lies on the surface its UVs came from, and every
 * texel is claimed once. Page composition keeps each rectangle's fill inside
 * it, the page encoding keeps HDR values above one, and a lightmap set file
 * round-trips while a corrupted payload or malformed table is rejected. A
 * scene's blockout stairs load as bake geometry. A sparse diffuse volume
 * round-trips and rejects inconsistent bricks, its probe visibility is the
 * same for any worker count and leaves no valid probe inside a wall, and a
 * Lambert furnace keeps its analytic radiance through the L1 projection. */

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
  const VkrBakeLightmapRect &small_rect =
      layout.rects[layout.rect_by_instance[1]];
  const VkrBakeLightmapRect &large = layout.rects[layout.rect_by_instance[4]];
  assert(unit.width == 64u && unit.height == 64u);
  assert(doubled.width == 128u && doubled.height == 128u);
  assert(small_rect.width == 20u && small_rect.height == 12u);
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
    /* One texel of the 2 x 3 quad's 8 x 8 is 0.25 wide and 0.375 high, on
       either triangle. */
    assert(fabsf(texel.step_x.x - 0.25f) < 1.0e-5f);
    assert(fabsf(texel.step_x.y) < 1.0e-5f && fabsf(texel.step_x.z) < 1.0e-5f);
    assert(fabsf(texel.step_y.y - 0.375f) < 1.0e-5f);
    assert(fabsf(texel.step_y.x) < 1.0e-5f && fabsf(texel.step_y.z) < 1.0e-5f);
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
  const std::vector<VkrBakeLightmapInstance> small_instances = {
      make_instance(0u, 60u, 40u, 1.0f), make_instance(1u, 30u, 30u, 1.0f)};
  VkrBakeLightmapLayout layout;
  assert(vkr_bake_lightmap_pack_fitted(small_instances, 4096u, 8.0f, &layout));
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
   rectangle mean fill each rectangle with its own value and occlusion,
   nothing crosses the shared edge, and texels outside both stay zero with
   alpha one. Without occlusion every alpha is one. */
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
  const std::vector<float32_t> occlusion = {0.25f, 0.75f};
  for (uint32_t with_occlusion = 0u; with_occlusion < 2u; ++with_occlusion) {
    std::vector<float32_t> rgba;
    assert(vkr_bake_lightmap_compose_page(layout, 0u, texels, values,
                                          with_occlusion ? &occlusion : nullptr,
                                          1u, &rgba));
    assert(rgba.size() == 8u * 8u * 4u);
    for (uint32_t y = 0u; y < 8u; ++y) {
      for (uint32_t x = 0u; x < 8u; ++x) {
        const float32_t *texel = &rgba[4u * (y * 8u + x)];
        Vec3 expected = vec3_zero();
        float32_t expected_alpha = 1.0f;
        if (y < 4u) {
          expected = x < 4u ? values[0] : values[1];
          if (with_occlusion) {
            expected_alpha = x < 4u ? occlusion[0] : occlusion[1];
          }
        }
        assert(fabsf(texel[0] - expected.x) < 1.0e-6f);
        assert(fabsf(texel[1] - expected.y) < 1.0e-6f);
        assert(fabsf(texel[2] - expected.z) < 1.0e-6f);
        assert(fabsf(texel[3] - expected_alpha) < 1.0e-6f);
      }
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

/* Writes a one-page set with distinct payload bytes the way a producer
   streams it: payload CRC first, prefix last. */
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
  const VkrLightLayer layers[] = {
      {VKR_LIGHT_LAYER_SUN_KEY, 0u, vec3_new(0.0f, 0.6f, 0.8f), ""},
      {VKR_LIGHT_LAYER_LAMP_GROUP, 0u, vec3_zero(), "default"},
      {VKR_LIGHT_LAYER_LAMP_GROUP, 1u, vec3_zero(), "street_lamps"}};
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
  /* Every layer's ASTC irradiance, and the lamp groups' RGB9E5 for the
     desktop pipeline. */
  const VkrLightmapPlane planes[] = {
      {0u, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR},
      {1u, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR},
      {1u, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_RGB9E5},
      {2u, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR},
      {2u, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_RGB9E5}};
  VkrLightmapSet set = {};
  set.page_size = 16u;
  set.page_count = 1u;
  set.layer_count = 3u;
  set.plane_count = 5u;
  set.instance_count = 3u;
  set.texels_per_unit = 8.0f;
  set.layers = layers;
  set.planes = planes;
  set.instances = instances;
  const std::vector<uint8_t> file = write_lightmap_set(set);
  assert(file.size() % 4u == 0u);

  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrLightmapSet decoded = {};
  assert(vkr_lightmap_set_decode(file.data(), file.size(), arena, &decoded));
  assert(decoded.page_size == 16u && decoded.page_count == 1u);
  assert(decoded.layer_count == 3u && decoded.instance_count == 3u);
  assert(decoded.layers[0].kind == VKR_LIGHT_LAYER_SUN_KEY);
  assert(decoded.layers[0].sun_direction.z == 0.8f);
  assert(decoded.layers[0].name[0] == '\0');
  assert(strcmp(decoded.layers[2].name, "street_lamps") == 0);
  assert(decoded.instances[2].entity_index == 5u);
  assert(decoded.instances[2].width == 12u);
  assert(decoded.instances[2].document_id[0] == 0xabu &&
         decoded.instances[2].document_id[15] == 0x7fu);
  assert(decoded.plane_count == 5u &&
         decoded.planes[4].format == VKR_LIGHTMAP_FORMAT_RGB9E5);
  const uint64_t astc_bytes =
      vkr_lightmap_set_plane_bytes(VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR, 16u);
  const uint64_t rgb9e5_bytes =
      vkr_lightmap_set_plane_bytes(VKR_LIGHTMAP_FORMAT_RGB9E5, 16u);
  assert(astc_bytes == 16u * 16u && rgb9e5_bytes == 16u * 16u * 4u);
  assert(vkr_lightmap_set_page_stride(&decoded) ==
         3u * astc_bytes + 2u * rgb9e5_bytes);
  assert(decoded.payload + vkr_lightmap_set_page_stride(&decoded) ==
         file.data() + file.size());
  assert(vkr_lightmap_set_plane_offset(&decoded, 0u, 4u) ==
         2u * astc_bytes + rgb9e5_bytes + astc_bytes);
  /* The tiled pipeline finds every layer's ASTC plane, the desktop one only
     the lamp groups' RGB9E5 planes. */
  assert(vkr_lightmap_set_find_plane(
             &decoded, 0u, VKR_LIGHTMAP_PLANE_IRRADIANCE, false_v) == 0u);
  assert(vkr_lightmap_set_find_plane(&decoded, 0u,
                                     VKR_LIGHTMAP_PLANE_IRRADIANCE,
                                     true_v) == UINT32_MAX);
  assert(vkr_lightmap_set_find_plane(
             &decoded, 2u, VKR_LIGHTMAP_PLANE_IRRADIANCE, true_v) == 4u);

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
  set.instances = instances;

  /* The runtime finds a lamp group by name, so each lamp group has a
     distinct one and a sun key has none. */
  VkrLightLayer named[] = {layers[0], layers[1], layers[2]};
  strcpy(named[2].name, "default");
  set.layers = named;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  named[2].name[0] = '\0';
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  strcpy(named[2].name, "street lamps");
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  strcpy(named[2].name, "street_lamps");
  strcpy(named[0].name, "sun");
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  set.layers = layers;

  /* Desktop planes belong to lamp groups; planes are in (layer, kind,
     format) order with one tiled and one desktop encoding of a kind; a
     direction is never stored as HDR irradiance. */
  VkrLightmapPlane bad[] = {planes[0], planes[1], planes[2], planes[3],
                            planes[4]};
  bad[0].format = VKR_LIGHTMAP_FORMAT_RGB9E5;
  set.planes = bad;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  bad[0] = planes[0];
  bad[1] = planes[2];
  bad[2] = planes[1];
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  bad[1] = planes[1];
  bad[2] = planes[2];
  bad[2].format = VKR_LIGHTMAP_FORMAT_BC6H;
  bad[1].format = VKR_LIGHTMAP_FORMAT_RGB9E5;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  bad[1] = planes[1];
  bad[2] = planes[2];
  bad[4].kind = VKR_LIGHTMAP_PLANE_DIRECTION;
  bad[4].format = VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  set.planes = planes;
  assert(vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  arena_destroy(arena);
  printf("  test_lightmap_set_round_trip_and_rejects PASSED\n");
}

/* Lamp-direct pages (ADR-108): a lamp group's lamp-direct plane lives on its
   own pages after every other page, each instance's rectangle on them
   survives a round trip, and a set rejects a lamp-direct plane without its
   pages, pages without a plane, a plane on a sun key and a rectangle off
   its page. */
void test_lightmap_set_lamp_direct_round_trip_and_rejects() {
  const VkrLightLayer layers[] = {
      {VKR_LIGHT_LAYER_SUN_KEY, 0u, vec3_new(0.0f, 0.6f, 0.8f), ""},
      {VKR_LIGHT_LAYER_LAMP_GROUP, 0u, vec3_zero(), "default"}};
  const VkrLightmapInstance instances[] = {{{0u}, 1u, 0u, 0u, 0u, 0u, 8u, 8u},
                                           {{0u}, 2u, 0u, 0u, 8u, 0u, 8u, 8u}};
  const VkrLightmapPlane planes[] = {
      {0u, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR},
      {1u, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR},
      {1u, VKR_LIGHTMAP_PLANE_LAMP_DIRECT, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR}};
  const VkrLightmapDirectRect direct[] = {{0u, 0u, 0u, 16u, 16u},
                                          {1u, 0u, 0u, 16u, 12u}};
  VkrLightmapSet set = {};
  set.page_size = 16u;
  set.page_count = 1u;
  set.layer_count = 2u;
  set.plane_count = 3u;
  set.instance_count = 2u;
  set.texels_per_unit = 8.0f;
  set.layers = layers;
  set.planes = planes;
  set.instances = instances;
  set.direct_page_count = 2u;
  set.direct_texels_per_unit = 16.0f;
  set.direct_rects = direct;
  const std::vector<uint8_t> file = write_lightmap_set(set);

  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrLightmapSet decoded = {};
  assert(vkr_lightmap_set_decode(file.data(), file.size(), arena, &decoded));
  assert(decoded.direct_page_count == 2u &&
         decoded.direct_texels_per_unit == 16.0f);
  assert(decoded.direct_rects[1].page == 1u &&
         decoded.direct_rects[1].height == 12u);
  const uint64_t astc_bytes =
      vkr_lightmap_set_plane_bytes(VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR, 16u);
  assert(vkr_lightmap_set_page_stride(&decoded) == 2u * astc_bytes);
  assert(vkr_lightmap_set_direct_page_stride(&decoded) == astc_bytes);
  /* Lamp-direct page 1 follows the page and lamp-direct page 0. */
  assert(vkr_lightmap_set_plane_offset(&decoded, 1u, 2u) == 3u * astc_bytes);
  assert(decoded.payload + 4u * astc_bytes == file.data() + file.size());
  assert(vkr_lightmap_set_find_plane(
             &decoded, 1u, VKR_LIGHTMAP_PLANE_LAMP_DIRECT, false_v) == 2u);

  uint64_t payload_offset = 0u;
  uint64_t file_size = 0u;
  set.direct_page_count = 0u;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  set.direct_page_count = 2u;
  set.plane_count = 2u;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  set.plane_count = 3u;
  VkrLightmapPlane sun_direct[] = {planes[0], planes[1], planes[2]};
  sun_direct[1] = {0u, VKR_LIGHTMAP_PLANE_LAMP_DIRECT,
                   VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR};
  sun_direct[2] = planes[1];
  set.planes = sun_direct;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  set.planes = planes;
  VkrLightmapDirectRect off_page[] = {direct[0], direct[1]};
  off_page[1].page = 2u;
  set.direct_rects = off_page;
  assert(!vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  /* An instance no lamp reaches has an all-zero rectangle. */
  off_page[1] = {0u, 0u, 0u, 0u, 0u};
  assert(vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  set.direct_rects = direct;
  assert(vkr_lightmap_set_layout(&set, &payload_offset, &file_size));
  arena_destroy(arena);
  printf("  test_lightmap_set_lamp_direct_round_trip_and_rejects PASSED\n");
}

/* RGB9E5 keeps exact powers of two and values on its mantissa grid, rounds
   others to within half a step of the brightest channel's exponent, clamps
   past 65408 and stores negatives and NaN as zero. */
void test_rgb9e5_round_trip() {
  float32_t rgb[3];
  vkr_rgb9e5_to_float3(vkr_rgb9e5_from_float3(1.0f, 0.5f, 0.25f), rgb);
  assert(rgb[0] == 1.0f && rgb[1] == 0.5f && rgb[2] == 0.25f);
  vkr_rgb9e5_to_float3(vkr_rgb9e5_from_float3(1.0e6f, -2.0f, NAN), rgb);
  assert(rgb[0] == 65408.0f && rgb[1] == 0.0f && rgb[2] == 0.0f);
  vkr_rgb9e5_to_float3(vkr_rgb9e5_from_float3(0.0f, 0.0f, 0.0f), rgb);
  assert(rgb[0] == 0.0f && rgb[1] == 0.0f && rgb[2] == 0.0f);
  uint32_t state = 12345u;
  for (uint32_t i = 0u; i < 10000u; ++i) {
    float32_t source[3];
    for (uint32_t c = 0u; c < 3u; ++c) {
      state = state * 1664525u + 1013904223u;
      source[c] = ldexpf((float32_t)(state >> 8) / 16777216.0f,
                         (int32_t)(state % 24u) - 12);
    }
    vkr_rgb9e5_to_float3(
        vkr_rgb9e5_from_float3(source[0], source[1], source[2]), rgb);
    const float32_t brightest = fmaxf(source[0], fmaxf(source[1], source[2]));
    for (uint32_t c = 0u; c < 3u; ++c) {
      assert(fabsf(rgb[c] - source[c]) <= brightest / 512.0f);
    }
  }
  printf("  test_rgb9e5_round_trip PASSED\n");
}

/* Sun-key weights follow the sun around the daily circle: a sun on a key
   weighs that key alone, a sun between two neighbouring keys splits by angle,
   no sun weighs key 0, and lamp-group weights are the caller's. */
void test_light_layer_sun_weights() {
  VkrLightLayer layers[9] = {};
  for (uint32_t k = 0u; k < 8u; ++k) {
    const float32_t angle = (float32_t)k * 0.78539816339f;
    layers[k].kind = VKR_LIGHT_LAYER_SUN_KEY;
    layers[k].index = k;
    layers[k].sun_direction = vec3_new(cosf(angle), 0.0f, sinf(angle));
  }
  layers[8].kind = VKR_LIGHT_LAYER_LAMP_GROUP;
  strcpy(layers[8].name, "default");
  assert(vkr_light_layers_valid(layers, 9u));
  float32_t weights[9];
  weights[8] = -1.0f;
  vkr_light_layers_sun_weights(layers, 9u, layers[2].sun_direction, weights);
  for (uint32_t k = 0u; k < 8u; ++k) {
    assert(fabsf(weights[k] - (k == 2u ? 1.0f : 0.0f)) < 1.0e-5f);
  }
  assert(weights[8] == -1.0f);
  const float32_t between = 6.5f * 0.78539816339f;
  vkr_light_layers_sun_weights(
      layers, 9u, vec3_new(2.0f * cosf(between), 0.0f, 2.0f * sinf(between)),
      weights);
  assert(fabsf(weights[6] - 0.5f) < 1.0e-4f &&
         fabsf(weights[7] - 0.5f) < 1.0e-4f);
  /* Across key 0: between keys 7 and 0. */
  const float32_t wrap = 7.75f * 0.78539816339f;
  vkr_light_layers_sun_weights(layers, 9u,
                               vec3_new(cosf(wrap), 0.0f, sinf(wrap)), weights);
  assert(fabsf(weights[7] - 0.25f) < 1.0e-4f &&
         fabsf(weights[0] - 0.75f) < 1.0e-4f);
  vkr_light_layers_sun_weights(layers, 9u, vec3_zero(), weights);
  assert(weights[0] == 1.0f && weights[1] == 0.0f && weights[8] == -1.0f);
  printf("  test_light_layer_sun_weights PASSED\n");
}

void store_half_le(uint8_t *dst, float32_t value) {
  const uint16_t half = vkr_float32_to_float16(value);
  dst[0] = (uint8_t)(half & 0xffu);
  dst[1] = (uint8_t)(half >> 8u);
}

/* A two-level DVOL v3 volume: one level-1 brick over a 3 x 3 x 3 grid with
   one corner entry refined by a level-0 brick. It round-trips byte for byte
   with its layer names, and the decoder refuses an entry naming a brick of
   another level, an offset past half the probe spacing, a validity that is
   neither zero nor one, a scale that is not a power of two, a corrupted
   payload and an invalid layer table. */
void test_diffuse_volume_v3_round_trip_and_rejects() {
  printf("  test_diffuse_volume_v3_round_trip_and_rejects...\n");
  VkrLightLayer layers[2] = {};
  layers[0].kind = VKR_LIGHT_LAYER_SUN_KEY;
  layers[0].sun_direction = vec3_new(0.0f, 1.0f, 0.0f);
  layers[1].kind = VKR_LIGHT_LAYER_LAMP_GROUP;
  strcpy(layers[1].name, "street");
  const VkrDiffuseVolumeBrick bricks[2] = {{{0u, 0u, 0u}, 1u},
                                           {{0u, 0u, 0u}, 0u}};
  std::vector<uint8_t> entries(27u * sizeof(uint32_t));
  for (uint32_t i = 0u; i < 27u; ++i) {
    const uint32_t entry =
        i == 0u ? 1u : (1u << VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT) | 0u;
    vkr_store_le_u32(entries.data() + i * sizeof(uint32_t), entry);
  }
  const uint32_t probe_count = 2u * VKR_DIFFUSE_VOLUME_BRICK_PROBES;
  std::vector<uint8_t> aux(probe_count * VKR_DIFFUSE_VOLUME_AUX_BYTES);
  std::vector<uint8_t> moments(probe_count * VKR_DIFFUSE_VOLUME_MOMENT_BYTES);
  /* Two layers, then the lamp group's direct band. */
  std::vector<uint8_t> sh(probe_count * 3u * VKR_DIFFUSE_VOLUME_SH_BYTES);
  for (uint32_t p = 0u; p < probe_count; ++p) {
    uint8_t *record = aux.data() + p * VKR_DIFFUSE_VOLUME_AUX_BYTES;
    store_half_le(record, 0.25f);
    store_half_le(record + 2u, -0.125f);
    store_half_le(record + 4u, 0.0f);
    store_half_le(record + 6u, p % 7u == 0u ? 0.0f : 1.0f);
    for (uint32_t t = 0u; t < VKR_DIFFUSE_VOLUME_MOMENT_TEXELS; ++t) {
      uint8_t *texel =
          moments.data() + p * VKR_DIFFUSE_VOLUME_MOMENT_BYTES + t * 4u;
      store_half_le(texel, 1.5f);
      store_half_le(texel + 2u, 2.5f);
    }
  }
  for (uint32_t i = 0u; i < probe_count * 3u * 12u; ++i) {
    store_half_le(sh.data() + i * 2u, (float32_t)(i % 97u) * 0.5f - 10.0f);
  }
  VkrDiffuseVolume volume = {};
  volume.origin = vec3_new(-1.0f, 0.0f, 2.0f);
  volume.spacing = 1.0f;
  volume.sh_scale = 4.0f;
  volume.level_count = 2u;
  volume.dimensions[0] = volume.dimensions[1] = volume.dimensions[2] = 3u;
  volume.entries = entries.data();
  volume.entry_count = 27u;
  volume.bricks = bricks;
  volume.brick_count = 2u;
  volume.probe_count = probe_count;
  volume.probe_aux = aux.data();
  volume.moments = moments.data();
  volume.layer_sh = sh.data();
  volume.layers = layers;
  volume.layer_count = 2u;
  volume.lamp_direct_count = 1u;

  Arena *arena = arena_create(MB(4), MB(1));
  assert(arena);
  const uint8_t *bytes = NULL;
  uint64_t size = 0u;
  bool8_t ok = vkr_diffuse_volume_encode(&volume, arena, &bytes, &size);
  assert(ok);
  VkrDiffuseVolume decoded = {};
  ok = vkr_diffuse_volume_decode(bytes, size, arena, &decoded);
  assert(ok);
  assert(decoded.layer_count == 2u && decoded.brick_count == 2u &&
         decoded.probe_count == probe_count && decoded.sh_scale == 4.0f &&
         decoded.lamp_direct_count == 1u &&
         vkr_diffuse_volume_sh_band_count(&decoded) == 3u);
  assert(strcmp(decoded.layers[1].name, "street") == 0);
  assert(decoded.bricks[0].level == 1u && decoded.bricks[1].level == 0u);
  assert(vkr_diffuse_volume_entry(&decoded, 0u) == 1u);
  assert(memcmp(decoded.layer_sh, sh.data(), sh.size()) == 0);
  const uint8_t *again = NULL;
  uint64_t again_size = 0u;
  ok = vkr_diffuse_volume_encode(&decoded, arena, &again, &again_size);
  assert(ok && again_size == size && memcmp(again, bytes, size) == 0);

  std::vector<uint8_t> corrupt(bytes, bytes + size);
  corrupt[size - 1u] ^= 0x40u;
  assert(!vkr_diffuse_volume_decode(corrupt.data(), size, arena, &decoded));

  /* Entry 0 names brick 1 (level 0) as if it were level 1. */
  vkr_store_le_u32(entries.data(),
                   (1u << VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT) | 1u);
  assert(!vkr_diffuse_volume_encode(&volume, arena, &bytes, &size));
  vkr_store_le_u32(entries.data(), 1u);
  /* A level-0 probe moved 0.75 m at 1 m spacing. */
  store_half_le(aux.data() + 64u * VKR_DIFFUSE_VOLUME_AUX_BYTES, 0.75f);
  assert(!vkr_diffuse_volume_encode(&volume, arena, &bytes, &size));
  store_half_le(aux.data() + 64u * VKR_DIFFUSE_VOLUME_AUX_BYTES, 0.25f);
  store_half_le(aux.data() + 6u, 0.5f);
  assert(!vkr_diffuse_volume_encode(&volume, arena, &bytes, &size));
  store_half_le(aux.data() + 6u, 0.0f);
  volume.sh_scale = 3.0f;
  assert(!vkr_diffuse_volume_encode(&volume, arena, &bytes, &size));
  volume.sh_scale = 4.0f;
  /* One lamp group cannot have two direct bands. */
  volume.lamp_direct_count = 2u;
  assert(!vkr_diffuse_volume_encode(&volume, arena, &bytes, &size));
  volume.lamp_direct_count = 1u;
  strcpy(layers[1].name, "two words");
  assert(!vkr_diffuse_volume_encode(&volume, arena, &bytes, &size));
  (void)ok;
  arena_destroy(arena);
  printf("  test_diffuse_volume_v3_round_trip_and_rejects PASSED\n");
}

/* Outlier rejection replaces a lone firefly on a smooth surface with its
   neighbors' mean, leaves a gradient alone, and does not take a texel that
   is next to it on the page but on another surface as a neighbor; smoothing
   does not mix the two surfaces either. */
void test_lightmap_denoise_stays_on_surfaces() {
  printf("  test_lightmap_denoise_stays_on_surfaces...\n");
  /* An 8x8 floor at 8 texels per unit, and one wall texel at (7, 0) that the
     atlas packed next to it but which lies 3 units away facing sideways. */
  std::vector<VkrBakeLightmapTexel> texels;
  std::vector<Vec3> values;
  for (uint32_t y = 0u; y < 8u; ++y) {
    for (uint32_t x = 0u; x < 8u; ++x) {
      VkrBakeLightmapTexel texel;
      texel.x = x;
      texel.y = y;
      texel.position =
          vec3_new(0.125f * (float32_t)x, 0.0f, 0.125f * (float32_t)y);
      texel.normal = vec3_new(0.0f, 1.0f, 0.0f);
      if (x == 7u && y == 0u) {
        texel.position = vec3_new(3.0f, 1.0f, 0.0f);
        texel.normal = vec3_new(1.0f, 0.0f, 0.0f);
      }
      texels.push_back(texel);
      /* A gradient that rises 50% per texel: steep, yet no texel exceeds
         twice its brightest neighbor. */
      const float32_t gray = powf(1.5f, (float32_t)x);
      values.push_back(vec3_new(gray, gray, gray));
    }
  }
  values[4u * 8u + 3u] = vec3_scale(values[4u * 8u + 3u], 10.0f);
  values[0u * 8u + 7u] = vec3_new(1000.0f, 1000.0f, 1000.0f);
  VkrBakeLightmapNeighbors neighbors;
  assert(vkr_bake_lightmap_neighbors(8u, texels, 0.5f, &neighbors));
  /* The wall texel has no neighbor, and the floor texel next to it on the
     page does not count it. */
  assert(neighbors.first[7u + 1u] == neighbors.first[7u]);
  for (uint32_t n = neighbors.first[6u]; n < neighbors.first[7u]; ++n) {
    assert(neighbors.indices[n] != 7u);
  }
  const std::vector<Vec3> before = values;
  const VkrBakeLightmapOutliers outliers =
      vkr_bake_lightmap_reject_outliers(neighbors, 2.0f, 3u, true, &values);
  assert(outliers.texels == 1u);
  assert(outliers.energy_fraction > 0.0 && outliers.energy_fraction < 1.0);
  const float32_t replaced = values[4u * 8u + 3u].x;
  assert(replaced > before[4u * 8u + 2u].x &&
         replaced < before[4u * 8u + 4u].x);
  for (size_t i = 0u; i < values.size(); ++i) {
    if (i != 4u * 8u + 3u) {
      assert(values[i].x == before[i].x);
    }
  }

  /* Smoothing keeps a constant floor constant beside the bright wall texel,
     which is not its neighbor, and leaves the lone wall texel as it was. */
  for (size_t i = 0u; i < values.size(); ++i) {
    values[i] = i == 7u ? vec3_new(1000.0f, 1000.0f, 1000.0f)
                        : vec3_new(2.0f, 2.0f, 2.0f);
  }
  assert(vkr_bake_lightmap_smooth(neighbors, texels, 2u, &values));
  for (size_t i = 0u; i < values.size(); ++i) {
    assert(fabsf(values[i].x - (i == 7u ? 1000.0f : 2.0f)) < 1.0e-4f);
  }
  printf("  test_lightmap_denoise_stays_on_surfaces PASSED\n");
}

/* Deterministic noise in [-1, 1] per texel. */
float32_t texel_noise(uint32_t x, uint32_t y) {
  uint32_t h = x * 0x9e3779b9u ^ (y + 0x7f4a7c15u) * 0x85ebca6bu;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  return (float32_t)(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

float32_t mean_and_deviation(const std::vector<Vec3> &values,
                             const std::vector<size_t> &indices,
                             float32_t *out_deviation) {
  float64_t sum = 0.0;
  for (size_t i : indices) {
    sum += values[i].x;
  }
  const float64_t mean = sum / (float64_t)indices.size();
  float64_t squares = 0.0;
  for (size_t i : indices) {
    squares += (values[i].x - mean) * (values[i].x - mean);
  }
  *out_deviation = (float32_t)sqrt(squares / (float64_t)indices.size());
  return (float32_t)mean;
}

/* The indirect-light denoiser averages noise out on a flat surface, keeps a
   floor and the wall it meets apart where their normals differ, does not
   mix in a chart of the same instance that the atlas packed next to the
   floor but that lies elsewhere in the world, and keeps the energy of rare
   bright texels instead of rejecting them from their darker neighbors. */
void test_lightmap_denoise_atrous_keeps_edges() {
  printf("  test_lightmap_denoise_atrous_keeps_edges...\n");
  /* Columns 0-13: a floor (normal +y) lit 1 +- 0.5; columns 14-23: the
     wall it meets at x = 1.75 (normal -x) lit 10 +- 5; columns 24-31: a
     second floor chart of the same instance 5 units away, lit 50. Eight
     texels per unit. */
  const uint32_t size = 32u;
  std::vector<VkrBakeLightmapTexel> texels;
  std::vector<Vec3> values;
  std::vector<float32_t> variance;
  std::vector<size_t> floor_inside;
  std::vector<size_t> floor_edge;
  std::vector<size_t> wall_edge;
  for (uint32_t y = 0u; y < size; ++y) {
    for (uint32_t x = 0u; x < size; ++x) {
      VkrBakeLightmapTexel texel;
      texel.x = x;
      texel.y = y;
      const float32_t z = 0.125f * (float32_t)y + 0.0625f;
      float32_t light = 0.0f;
      float32_t amplitude = 0.0f;
      if (x < 14u) {
        texel.position = vec3_new(0.125f * (float32_t)x + 0.0625f, 0.0f, z);
        texel.normal = vec3_new(0.0f, 1.0f, 0.0f);
        light = 1.0f;
        amplitude = 0.5f;
        if (x >= 4u && x < 10u && y >= 8u && y < 24u) {
          floor_inside.push_back(texels.size());
        }
        if (x == 13u && y >= 4u && y < 28u) {
          floor_edge.push_back(texels.size());
        }
      } else if (x < 24u) {
        texel.position =
            vec3_new(1.75f, 0.125f * (float32_t)(x - 14u) + 0.0625f, z);
        texel.normal = vec3_new(-1.0f, 0.0f, 0.0f);
        light = 10.0f;
        amplitude = 5.0f;
        if (x == 14u && y >= 4u && y < 28u) {
          wall_edge.push_back(texels.size());
        }
      } else {
        texel.position =
            vec3_new(5.0f + 0.125f * (float32_t)(x - 24u), 0.0f, z);
        texel.normal = vec3_new(0.0f, 1.0f, 0.0f);
        light = 50.0f;
      }
      const float32_t value = light + amplitude * texel_noise(x, y);
      texels.push_back(texel);
      values.push_back(vec3_new(value, value, value));
      /* Uniform noise of amplitude a has variance a^2 / 3; this is the
         variance of the texel's mean, as the gather reports it. */
      variance.push_back(amplitude * amplitude / 3.0f);
    }
  }
  float32_t noisy_deviation = 0.0f;
  const float32_t noisy_mean =
      mean_and_deviation(values, floor_inside, &noisy_deviation);
  assert(noisy_deviation > 0.2f);

  VkrBakeLightmapDenoiseSettings settings;
  settings.iterations = 4u;
  settings.texels_per_unit = 8.0f;
  settings.threads = 2u;
  /* Geometry alone and with luminance weights. */
  for (uint32_t mode = 0u; mode < 2u; ++mode) {
    std::vector<Vec3> result = values;
    assert(vkr_bake_lightmap_denoise(size, texels, nullptr,
                                     mode == 0u ? nullptr : &variance, settings,
                                     &result));
    float32_t deviation = 0.0f;
    const float32_t mean = mean_and_deviation(result, floor_inside, &deviation);
    assert(deviation < noisy_deviation / 4.0f);
    assert(fabsf(mean - noisy_mean) < 0.05f);
    /* A floor texel beside the wall keeps floor light, and a wall texel
       beside the floor keeps wall light. */
    for (size_t i : floor_edge) {
      assert(result[i].x < 1.6f);
    }
    for (size_t i : wall_edge) {
      assert(result[i].x > 7.0f);
    }
    /* The far chart neither leaks into the wall beside it on the page nor
       takes its light. */
    for (size_t i = 0u; i < texels.size(); ++i) {
      if (texels[i].x == 23u) {
        assert(result[i].x < 16.0f);
      }
      if (texels[i].x == 24u) {
        assert(fabsf(result[i].x - 50.0f) < 1.0e-3f);
      }
    }
  }

  /* A flat 16x16 patch where every 16th texel caught one bright path among
     64 samples (mean 17 instead of 1): such a texel reports a variance of
     its mean near 16^2, the others near zero. The mean keeps 85% of the
     energy (89.5% measured); a tolerance from the center's noise alone,
     which turns the bright texels away from their dark neighbors, kept
     75%. */
  std::vector<VkrBakeLightmapTexel> patch;
  std::vector<Vec3> patch_values;
  std::vector<float32_t> patch_variance;
  std::vector<size_t> all;
  for (uint32_t y = 0u; y < 16u; ++y) {
    for (uint32_t x = 0u; x < 16u; ++x) {
      VkrBakeLightmapTexel texel;
      texel.x = x;
      texel.y = y;
      texel.position =
          vec3_new(0.125f * (float32_t)x, 0.0f, 0.125f * (float32_t)y);
      texel.normal = vec3_new(0.0f, 1.0f, 0.0f);
      const bool bright = (x % 4u == 1u) && (y % 4u == 2u);
      const float32_t value = bright ? 17.0f : 1.0f;
      all.push_back(patch.size());
      patch.push_back(texel);
      patch_values.push_back(vec3_new(value, value, value));
      patch_variance.push_back(bright ? 256.0f : 0.001f);
    }
  }
  float32_t unused = 0.0f;
  const float32_t energy = mean_and_deviation(patch_values, all, &unused);
  assert(vkr_bake_lightmap_denoise(16u, patch, nullptr, &patch_variance,
                                   settings, &patch_values));
  const float32_t kept = mean_and_deviation(patch_values, all, &unused);
  assert(kept > 0.85f * energy && kept < 1.05f * energy);
  printf("  test_lightmap_denoise_atrous_keeps_edges PASSED\n");
}

/* Texels buried in a solid take their light from the valid texels of the
   same surface ring by ring, so a dark strip under a wall disappears; a
   texel with no valid texel on its surface keeps its value. */
void test_lightmap_fill_buried_texels() {
  printf("  test_lightmap_fill_buried_texels...\n");
  /* An 8x8 floor whose columns 3-5 lie under a wall, lit 2 to its left and
     4 to its right, and one lone wall texel at (7, 7) that is buried too. */
  std::vector<VkrBakeLightmapTexel> texels;
  std::vector<Vec3> values;
  std::vector<float32_t> occlusion;
  std::vector<uint8_t> valid;
  for (uint32_t y = 0u; y < 8u; ++y) {
    for (uint32_t x = 0u; x < 8u; ++x) {
      VkrBakeLightmapTexel texel;
      texel.x = x;
      texel.y = y;
      texel.position =
          vec3_new(0.125f * (float32_t)x, 0.0f, 0.125f * (float32_t)y);
      texel.normal = vec3_new(0.0f, 1.0f, 0.0f);
      const bool lone = x == 7u && y == 7u;
      if (lone) {
        texel.position = vec3_new(4.0f, 1.0f, 0.0f);
        texel.normal = vec3_new(1.0f, 0.0f, 0.0f);
      }
      const bool buried = lone || (x >= 3u && x <= 5u);
      const float32_t light = buried ? 0.0f : (x < 3u ? 2.0f : 4.0f);
      texels.push_back(texel);
      values.push_back(vec3_new(light, light, light));
      occlusion.push_back(buried ? 0.0f : 1.0f);
      valid.push_back(buried ? 0u : 1u);
    }
  }
  VkrBakeLightmapNeighbors neighbors;
  assert(vkr_bake_lightmap_neighbors(8u, texels, 0.5f, &neighbors));
  VkrBakeLightmapFill fill;
  assert(vkr_bake_lightmap_plan_fill(neighbors, valid, &fill));
  assert(fill.texels.size() == 24u);
  assert(fill.unfilled == 1u);
  vkr_bake_lightmap_apply_fill(fill, &values);
  vkr_bake_lightmap_apply_fill(fill, &occlusion);
  for (size_t i = 0u; i < texels.size(); ++i) {
    const uint32_t x = texels[i].x;
    const uint32_t y = texels[i].y;
    if (x == 7u && y == 7u) {
      assert(values[i].x == 0.0f && occlusion[i] == 0.0f);
      continue;
    }
    /* The strip's edges copy their side; its middle averages both. */
    const float32_t expected = x <= 3u ? 2.0f : (x >= 5u ? 4.0f : 3.0f);
    assert(fabsf(values[i].x - expected) < 1.0e-5f);
    assert(fabsf(occlusion[i] - 1.0f) < 1.0e-6f);
  }
  printf("  test_lightmap_fill_buried_texels PASSED\n");
}

/* The bake reads the material files the glTF importer writes, including the
   roughness bound it records where it folds the factor into a texture; the
   loader rejects keys it does not know. */
void test_bake_material_accepts_roughness_bound() {
  const char *path = PROJECT_SOURCE_DIR "tests/tmp/lightmap_bake_rough.mt";
  FILE *file = fopen(path, "wb");
  assert(file);
  fputs("type=pbr\nbase_color=0.5,0.5,0.5,1\nroughness=1\n"
        "roughness_max=0.25\n",
        file);
  fclose(file);
  Arena *arena = arena_create(KB(256), KB(256));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrBakeTextureStore *store = vkr_bake_texture_store_create(&allocator);
  assert(store);
  VkrBakeMaterial material = {};
  VkrBakeMaterialError error = VKR_BAKE_MATERIAL_ERROR_NONE;
  assert(vkr_bake_material_load(store, path, &material, &error));
  assert(error == VKR_BAKE_MATERIAL_ERROR_NONE);
  vkr_bake_texture_store_release(store);
  arena_destroy(arena);
  remove(path);
}

void write_text_file(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  assert(file);
  fputs(text, file);
  fclose(file);
}

/* A level's blockout stairs (ADR-084) are bake geometry: a `blockout`
   component whose own "shape" field names the shape kind loads, and its four
   steps become four 1 x 0.25 x 0.5 boxes (48 triangles) inside the stairs'
   world box, with no lightmap instance since shapes take none. A malformed
   brush face fails the load with a diagnostic naming its entity. */
void test_bake_scene_builds_blockout_stairs() {
  FilePath directory = {};
  directory.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp");
  directory.type = FILE_PATH_TYPE_ABSOLUTE;
  assert(file_create_directory(&directory));
  const char *material = PROJECT_SOURCE_DIR "tests/tmp/bake_scene_stairs.mt";
  const char *path = PROJECT_SOURCE_DIR "tests/tmp/bake_scene_stairs.json";
  write_text_file(material, "type=pbr\nbase_color=0.5,0.5,0.5,1\n");
  const std::string stairs =
      std::string("{\"version\": 2, \"entities\": [{\"id\": "
                  "\"5620aaab-efb9-4e0e-b03d-ca02384458b2\", \"name\": "
                  "\"stairs\", \"parent\": null, \"transform\": {\"pos\": "
                  "[10, 0, 0], \"rot\": [0, 0, 0, 1], \"scale\": [1, 1, 1]}, "
                  "\"components\": {\"blockout\": {\"shape\": \"Stairs\", "
                  "\"stairs\": \"Straight\", \"height\": 1, \"width\": 1, "
                  "\"length\": 2, \"step_height\": 0.25, \"thickness\": 0, "
                  "\"material\": \"") +
      material + "\"}}}]}";
  write_text_file(path, stairs.c_str());

  Arena *arena = arena_create(MB(4), MB(4));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  {
    VkrBakeScene scene(&allocator);
    VkrBakeSceneError error = VkrBakeSceneError::None;
    assert(vkr_bake_scene_load(&scene, path, &error));
    assert(error == VkrBakeSceneError::None && scene.diagnostic.empty());
    assert(scene.triangles.size() == 48u);
    assert(scene.lightmap_instances.empty());
    for (const VkrBakeTriangle &triangle : scene.triangles) {
      for (const VkrBakeVertex &vertex : triangle.vertex) {
        assert(vertex.position.x >= 9.5f - 1.0e-4f &&
               vertex.position.x <= 10.5f + 1.0e-4f);
        assert(vertex.position.y >= -1.0e-4f &&
               vertex.position.y <= 1.0f + 1.0e-4f);
        assert(vertex.position.z >= -1.0e-4f &&
               vertex.position.z <= 2.0f + 1.0e-4f);
      }
    }

    write_text_file(path, "{\"version\": 2, \"entities\": [{\"name\": "
                          "\"bad_face\", \"parent\": null, \"components\": "
                          "{\"brush_face\": {\"distance\": 1}}}]}");
    assert(!vkr_bake_scene_load(&scene, path, &error));
    assert(error == VkrBakeSceneError::Parse);
    assert(scene.diagnostic.find("bad_face") != std::string::npos);
    assert(scene.triangles.empty());
  }
  arena_destroy(arena);
  remove(path);
  remove(material);
  printf("  test_bake_scene_builds_blockout_stairs PASSED\n");
}

/* The scene entities of a unit box brush centred at `x`: the brush, then its
   six brush_face children. */
std::string box_brush_entities(const char *name, int32_t parent, int32_t first,
                               float32_t x, const char *material) {
  const std::string parent_text =
      parent < 0 ? std::string("null") : std::to_string(parent);
  std::string text = std::string("{\"name\": \"") + name +
                     "\", \"parent\": " + parent_text +
                     ", \"transform\": {\"pos\": [" + std::to_string(x) +
                     ", 0, 0], \"rot\": [0, 0, 0, 1], \"scale\": [1, 1, 1]}, "
                     "\"components\": {\"brush\": {\"role\": \"solid\"}}}";
  static const char *const normals[6] = {"[1, 0, 0]", "[-1, 0, 0]",
                                         "[0, 1, 0]", "[0, -1, 0]",
                                         "[0, 0, 1]", "[0, 0, -1]"};
  for (const char *normal : normals) {
    text += std::string(", {\"parent\": ") + std::to_string(first) +
            ", \"components\": {\"brush_face\": {\"normal\": " + normal +
            ", \"distance\": 0.5, \"material\": \"" + material + "\"}}}";
  }
  return text;
}

/* A brush or blockout shape a mover moves stays out of the bake (ADR-088):
   below a mover, through a group without one, a box brush and stairs add no
   triangle and no lightmap instance, while the static box brush beside them
   adds its 12 triangles within its own box and its one lightmap instance. */
void test_bake_scene_leaves_out_moving_brushes() {
  FilePath directory = {};
  directory.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp");
  directory.type = FILE_PATH_TYPE_ABSOLUTE;
  assert(file_create_directory(&directory));
  const char *material = PROJECT_SOURCE_DIR "tests/tmp/bake_scene_mover.mt";
  const char *path = PROJECT_SOURCE_DIR "tests/tmp/bake_scene_mover.json";
  write_text_file(material, "type=pbr\nbase_color=0.5,0.5,0.5,1\n");
  /* 0-6 the static wall, 7 the door's mover, 8 a group under it, 9-15 the
     door brush under the group, 16 stairs under the mover. */
  const std::string scene_text =
      "{\"version\": 2, \"entities\": [" +
      box_brush_entities("wall", -1, 0, 0.0f, material) +
      ", {\"name\": \"door_mover\", \"parent\": null, \"transform\": "
      "{\"pos\": [5, 0, 0], \"rot\": [0, 0, 0, 1], \"scale\": [1, 1, 1]}, "
      "\"components\": {\"mover\": {\"direction\": [0, 1, 0]}}}"
      ", {\"name\": \"door_group\", \"parent\": 7}, " +
      box_brush_entities("door", 8, 9, 0.0f, material) +
      ", {\"name\": \"door_stairs\", \"parent\": 7, \"components\": "
      "{\"blockout\": {\"shape\": \"Stairs\", \"stairs\": \"Straight\", "
      "\"height\": 1, \"width\": 1, \"length\": 2, \"step_height\": 0.25, "
      "\"thickness\": 0, \"material\": \"" +
      material + "\"}}}]}";
  write_text_file(path, scene_text.c_str());

  Arena *arena = arena_create(MB(4), MB(4));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  {
    VkrBakeScene scene(&allocator);
    VkrBakeSceneError error = VkrBakeSceneError::None;
    assert(vkr_bake_scene_load(&scene, path, &error));
    assert(error == VkrBakeSceneError::None && scene.diagnostic.empty());
    assert(scene.triangles.size() == 12u);
    for (const VkrBakeTriangle &triangle : scene.triangles) {
      for (const VkrBakeVertex &vertex : triangle.vertex) {
        assert(fabsf(vertex.position.x) <= 0.5f + 1.0e-4f);
        assert(fabsf(vertex.position.y) <= 0.5f + 1.0e-4f);
        assert(fabsf(vertex.position.z) <= 0.5f + 1.0e-4f);
      }
    }
    assert(scene.lightmap_instances.size() == 1u);
    assert(scene.lightmap_instances[0].entity_index == 0u);
  }
  arena_destroy(arena);
  remove(path);
  remove(material);
  printf("  test_bake_scene_leaves_out_moving_brushes PASSED\n");
}

/* Appends the twelve triangles of an axis-aligned box, wound so that every
   geometric normal faces out of the box. */
void append_box(std::vector<VkrBakeTriangle> *triangles, Vec3 min, Vec3 max) {
  /* cross(u, v) is +axis, so the max side keeps the u-then-v corner order
     and the min side reverses it. */
  const uint32_t order[2][6] = {{0u, 2u, 1u, 0u, 3u, 2u},
                                {0u, 1u, 2u, 0u, 2u, 3u}};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const uint32_t u = (axis + 1u) % 3u;
    const uint32_t v = (axis + 2u) % 3u;
    for (uint32_t side = 0u; side < 2u; ++side) {
      Vec3 corners[4];
      for (uint32_t corner = 0u; corner < 4u; ++corner) {
        const bool8_t high_u = corner == 1u || corner == 2u;
        const bool8_t high_v = corner >= 2u;
        corners[corner] = vec3_zero();
        corners[corner].elements[axis] =
            side ? max.elements[axis] : min.elements[axis];
        corners[corner].elements[u] =
            high_u ? max.elements[u] : min.elements[u];
        corners[corner].elements[v] =
            high_v ? max.elements[v] : min.elements[v];
      }

      Vec3 normal = vec3_zero();
      normal.elements[axis] = side ? 1.0f : -1.0f;
      for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        VkrBakeTriangle face = {};
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
          face.vertex[vertex].position =
              corners[order[side][triangle * 3u + vertex]];
          face.vertex[vertex].normal = normal;
        }
        triangles->push_back(face);
      }
    }
  }
}

/* Four 12 m rooms in a row, 4 m tall behind 0.2 m walls, stand for an
   indoor level; a solid 2 m column stands in the first room. */
std::vector<VkrBakeTriangle> four_rooms() {
  const float32_t wall = 0.2f;
  const float32_t length = 48.0f;
  const float32_t height = 4.0f;
  const float32_t depth = 12.0f;
  std::vector<VkrBakeTriangle> triangles;
  append_box(&triangles, vec3_new(0.0f, 0.0f, 0.0f),
             vec3_new(length, wall, depth));
  append_box(&triangles, vec3_new(0.0f, height - wall, 0.0f),
             vec3_new(length, height, depth));
  append_box(&triangles, vec3_new(0.0f, 0.0f, 0.0f),
             vec3_new(length, height, wall));
  append_box(&triangles, vec3_new(0.0f, 0.0f, depth - wall),
             vec3_new(length, height, depth));
  for (uint32_t i = 0u; i <= 4u; ++i) {
    const float32_t x = 12.0f * (float32_t)i;
    const float32_t low = i == 0u ? 0.0f : x - 0.5f * wall;
    const float32_t high = i == 4u ? length : x + 0.5f * wall;
    append_box(&triangles, vec3_new(low, 0.0f, 0.0f),
               vec3_new(high, height, depth));
  }
  append_box(&triangles, vec3_new(4.0f, wall, 4.0f),
             vec3_new(6.0f, height - wall, 6.0f));
  return triangles;
}

/* Whether a point lies strictly inside one of the four rooms' solid slabs
   or the column. */
bool8_t inside_four_rooms_solid(Vec3 p) {
  const float32_t wall = 0.2f;
  if (p.x > 4.0f && p.x < 6.0f && p.z > 4.0f && p.z < 6.0f && p.y > 0.0f &&
      p.y < 4.0f) {
    return true_v;
  }
  if (p.x <= 0.0f || p.x >= 48.0f || p.y <= 0.0f || p.y >= 4.0f ||
      p.z <= 0.0f || p.z >= 12.0f) {
    return false_v;
  }
  if (p.y < wall || p.y > 4.0f - wall || p.z < wall || p.z > 12.0f - wall) {
    return true_v;
  }
  for (uint32_t i = 0u; i <= 4u; ++i) {
    if (fabsf(p.x - 12.0f * (float32_t)i) < 0.5f * wall) {
      return true_v;
    }
  }
  return false_v;
}

/* Bricks over the four rooms cover every indirection entry, refine near
   the walls, and keep coarse bricks well above them. Probe visibility is
   the same with one worker as with three, every probe in room air stays
   valid and unmoved, no valid probe ends up inside a wall slab or the
   column, the column's inner probes, half a meter deep, are invalid, and
   dropping bricks without a valid probe leaves every entry naming a stored
   brick. */
void test_sparse_bricks_place_and_relocate() {
  printf("  test_sparse_bricks_place_and_relocate...\n");
  std::vector<VkrBakeTriangle> triangles = four_rooms();
  Arena *arena = arena_create(MB(64), MB(1));
  VkrBakeBvh bvh = {};
  bool8_t ok = vkr_bake_bvh_build(
      {triangles.data(), (uint32_t)triangles.size()}, arena, &bvh);
  assert(ok);
  const VkrBakeBrickMaterial materials[1] = {{true_v, false_v}};

  VkrBakeBrickDesc desc = {};
  desc.bounds = bvh.nodes[0].bounds;
  desc.bounds.max.y += 30.0f;
  desc.spacing = 1.0f;
  desc.level_count = 2u;
  desc.margin_spans = 1.0f;
  VkrBakeBrickLayout layout = {};
  ok = vkr_bake_bricks_place(&bvh, materials, 1u, desc, arena, &layout);
  assert(ok);
  uint32_t level_bricks[2] = {0u, 0u};
  for (uint32_t b = 0u; b < layout.brick_count; ++b) {
    ++level_bricks[layout.bricks[b].level];
  }
  assert(level_bricks[0] > 0u && level_bricks[1] > 0u);
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    assert(layout.dimensions[axis] % 3u == 0u);
  }
  for (uint32_t i = 0u; i < layout.entry_count; ++i) {
    assert(layout.entries[i] != VKR_DIFFUSE_VOLUME_ENTRY_EMPTY);
  }
  /* The air near the top of the bounds is far from every surface. */
  const uint32_t top =
      layout.entries[layout.dimensions[0] * (layout.dimensions[1] - 1u)];
  assert((top >> VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT) == 1u);

  std::vector<VkrBakeBrickVisibility> serial(layout.probe_count);
  std::vector<VkrBakeBrickVisibility> parallel(layout.probe_count);
  ok = vkr_bake_bricks_visibility(&bvh, materials, 1u, &layout, 1u,
                                  serial.data()) &&
       vkr_bake_bricks_visibility(&bvh, materials, 1u, &layout, 3u,
                                  parallel.data());
  assert(ok);
  assert(memcmp(serial.data(), parallel.data(),
                serial.size() * sizeof(VkrBakeBrickVisibility)) == 0);
  uint32_t air = 0u;
  uint32_t invalid = 0u;
  for (uint32_t p = 0u; p < layout.probe_count; ++p) {
    const Vec3 lattice = vkr_bake_bricks_probe_position(&layout, p);
    const Vec3 final_position = vec3_add(lattice, serial[p].offset);
    if (serial[p].valid) {
      assert(!inside_four_rooms_solid(final_position));
    } else {
      ++invalid;
    }
    /* Half a meter inside the column, past the reach of relocation. */
    if (lattice.x > 4.4f && lattice.x < 5.6f && lattice.z > 4.4f &&
        lattice.z < 5.6f && lattice.y > 0.7f && lattice.y < 3.3f) {
      assert(!serial[p].valid);
    }
    /* Well inside a room's air, more than half a meter from every
       surface. */
    const float32_t room_x = fmodf(lattice.x, 12.0f);
    const bool8_t near_column = lattice.x > 3.4f && lattice.x < 6.6f &&
                                lattice.z > 3.4f && lattice.z < 6.6f;
    if (lattice.y > 0.7f && lattice.y < 3.3f && lattice.z > 0.7f &&
        lattice.z < 11.3f && lattice.x > 0.0f && lattice.x < 48.0f &&
        room_x > 0.6f && room_x < 11.4f && !near_column) {
      ++air;
      assert(serial[p].valid);
      assert(serial[p].offset.x == 0.0f && serial[p].offset.y == 0.0f &&
             serial[p].offset.z == 0.0f);
    }
  }
  assert(air > 0u && invalid > 0u);

  const uint32_t placed = layout.brick_count;
  vkr_bake_bricks_drop_invalid(&layout, serial.data());
  assert(layout.brick_count > 0u && layout.brick_count <= placed);
  for (uint32_t i = 0u; i < layout.entry_count; ++i) {
    const uint32_t entry = layout.entries[i];
    assert(entry == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY ||
           (entry & VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK) < layout.brick_count);
  }
  (void)ok;
  (void)top;
  (void)placed;
  (void)air;
  (void)invalid;
  arena_destroy(arena);
  printf("  test_sparse_bricks_place_and_relocate PASSED\n");
}

/* A closed box of double-sided Lambert surfaces with emission 1 and albedo
   0.5 sends radiance sum(0.5^k, k < depth) from every direction: 1 at depth
   1 and 1.75 at depth 3. Projected to L1, the constant term is that radiance
   and the linear terms vanish. */
void test_l1_lambert_furnace() {
  printf("  test_l1_lambert_furnace...\n");
  std::vector<VkrBakeTriangle> triangles;
  append_box(&triangles, vec3_new(-1.0f, -1.0f, -1.0f),
             vec3_new(1.0f, 1.0f, 1.0f));
  /* Base color multiplies the vertex color, which append_box leaves
     black. */
  for (VkrBakeTriangle &triangle : triangles) {
    for (VkrBakeVertex &vertex : triangle.vertex) {
      vertex.color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
    }
  }
  Arena *arena = arena_create(MB(16), MB(1));
  VkrBakeBvh bvh = {};
  bool8_t ok = vkr_bake_bvh_build(
      {triangles.data(), (uint32_t)triangles.size()}, arena, &bvh);
  assert(ok);
  VkrBakeMaterial material = {};
  material.alpha_mode = VKR_BAKE_MATERIAL_ALPHA_OPAQUE;
  material.double_sided = true_v;
  material.base_color = vec4_new(0.5f, 0.5f, 0.5f, 1.0f);
  material.roughness = 1.0f;
  material.normal_scale = 1.0f;
  material.occlusion_strength = 1.0f;
  material.emissive_factor = vec3_new(1.0f, 1.0f, 1.0f);
  material.ior = 1.5f;
  material.attenuation_color = vec3_new(1.0f, 1.0f, 1.0f);
  material.attenuation_distance = INFINITY;
  material.diffuse_transmission_color = vec3_new(1.0f, 1.0f, 1.0f);

  const uint32_t face_size = 4u;
  const float32_t expected[2] = {1.0f, 1.75f};
  const uint32_t depths[2] = {1u, 3u};
  for (uint32_t d = 0u; d < 2u; ++d) {
    VkrBakeIntegratorSettings settings = {};
    settings.scene.bvh = &bvh;
    settings.scene.materials = &material;
    settings.scene.material_count = 1u;
    settings.max_depth = depths[d];
    settings.max_transparent_layers =
        VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS;
    settings.ray_epsilon = 0.00001f;
    VkrBakeIntegrator integrator = {};
    VkrBakeIntegratorError error = {};
    ok = vkr_bake_integrator_init(&settings, &integrator, &error);
    assert(ok);
    std::vector<Vec3> radiance(6u * face_size * face_size);
    for (uint32_t face = 0u; face < 6u; ++face)
      for (uint32_t y = 0u; y < face_size; ++y)
        for (uint32_t x = 0u; x < face_size; ++x) {
          const Vec3 direction = vkr_bake_cube_direction(
              face, 2.0f * (x + 0.5f) / face_size - 1.0f,
              2.0f * (y + 0.5f) / face_size - 1.0f);
          VkrBakeIntegratorResult traced = {};
          ok = vkr_bake_integrator_trace(&integrator, vec3_zero(), direction,
                                         (face * 64u + y * 8u + x) + 1u,
                                         &traced, &error);
          assert(ok);
          radiance[(face * face_size + y) * face_size + x] = traced.radiance;
        }
    float32_t sh[3][4];
    ok = vkr_bake_sh_project_l1(radiance.data(), face_size, 0.0f, sh);
    assert(ok);
    for (uint32_t c = 0u; c < 3u; ++c) {
      assert(fabsf(sh[c][3] - expected[d]) < 1.0e-4f);
      for (uint32_t k = 0u; k < 3u; ++k) {
        assert(fabsf(sh[c][k]) < 1.0e-4f);
      }
    }
  }
  (void)ok;
  (void)expected;
  arena_destroy(arena);
  printf("  test_l1_lambert_furnace PASSED\n");
}

/* A point lamp 2 m above a probe projects its direct light, I / d^2 from
   straight above, into exactly constant E / (4 pi) and linear y E / (2 pi).
   A 3 cm socket box around the lamp lies inside the runtime's local-shadow
   near clip and does not shadow it, as a Bistro lantern's bulb mesh must
   not; a wall between them does. */
void test_point_lamp_direct_l1() {
  printf("  test_point_lamp_direct_l1...\n");
  const Vec3 lamp = vec3_new(0.0f, 2.0f, 0.0f);
  std::vector<VkrBakeTriangle> triangles;
  append_box(&triangles, vec3_new(-0.03f, 1.97f, -0.03f),
             vec3_new(0.03f, 2.03f, 0.03f));
  Arena *arena = arena_create(MB(16), MB(1));
  VkrBakeMaterial material = {};
  material.alpha_mode = VKR_BAKE_MATERIAL_ALPHA_OPAQUE;
  material.double_sided = true_v;
  material.base_color = vec4_new(0.5f, 0.5f, 0.5f, 1.0f);
  material.roughness = 1.0f;
  material.normal_scale = 1.0f;
  material.occlusion_strength = 1.0f;
  material.ior = 1.5f;
  material.attenuation_color = vec3_new(1.0f, 1.0f, 1.0f);
  material.attenuation_distance = INFINITY;
  material.diffuse_transmission_color = vec3_new(1.0f, 1.0f, 1.0f);
  VkrBakeSceneLight light = {};
  light.kind = VkrBakeSceneLightKind::Point;
  light.position = lamp;
  light.color = vec3_new(1.0f, 0.5f, 0.25f);
  light.intensity = 8.0f;
  light.enabled = true_v;

  const float32_t pi = 3.14159265358979f;
  const float32_t irradiance = light.intensity / 4.0f;
  for (uint32_t blocked = 0u; blocked < 2u; ++blocked) {
    if (blocked) {
      append_box(&triangles, vec3_new(-1.0f, 0.9f, -1.0f),
                 vec3_new(1.0f, 1.1f, 1.0f));
    }
    VkrBakeBvh bvh = {};
    bool8_t ok = vkr_bake_bvh_build(
        {triangles.data(), (uint32_t)triangles.size()}, arena, &bvh);
    assert(ok);
    VkrBakeIntegratorSettings settings = {};
    settings.scene.bvh = &bvh;
    settings.scene.materials = &material;
    settings.scene.material_count = 1u;
    settings.scene.lights = &light;
    settings.scene.light_count = 1u;
    settings.max_depth = 1u;
    settings.max_transparent_layers =
        VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS;
    settings.ray_epsilon = 0.00001f;
    VkrBakeIntegrator integrator = {};
    VkrBakeIntegratorError error = {};
    ok = vkr_bake_integrator_init(&settings, &integrator, &error);
    assert(ok);
    float32_t sh[3][4] = {};
    ok = vkr_bake_integrator_direct_l1(&integrator, vec3_zero(), 1u, 16u, sh,
                                       &error);
    assert(ok);
    for (uint32_t c = 0u; c < 3u; ++c) {
      const float32_t e = blocked ? 0.0f : irradiance * light.color.elements[c];
      assert(fabsf(sh[c][3] - e / (4.0f * pi)) < 1.0e-5f);
      assert(fabsf(sh[c][1] - e / (2.0f * pi)) < 1.0e-5f);
      assert(fabsf(sh[c][0]) < 1.0e-6f && fabsf(sh[c][2]) < 1.0e-6f);
    }
    (void)ok;
  }
  arena_destroy(arena);
  printf("  test_point_lamp_direct_l1 PASSED\n");
}

} // namespace

/* A lamp layer's direction page: a covered texel's direction becomes
   0.5 * normalize(d) + 0.5 with its length as alpha, clamped to one; a texel
   with no direction is the neutral (0.5, 0.5, 0.5, 0); the rectangle's
   uncovered texels take its covered texels' mean as irradiance does, and
   texels outside every rectangle or in a rectangle nothing covers are
   neutral, not compose_page's black. */
void test_compose_direction_page_encodes_and_stays_neutral() {
  VkrBakeLightmapLayout layout;
  layout.page_size = 8u;
  layout.page_count = 1u;
  /* The second rectangle holds no covered texel. */
  layout.rects = {{0u, 0u, 0u, 0u, 4u, 4u}, {1u, 0u, 4u, 4u, 4u, 4u}};
  layout.rect_by_instance = {0u, 1u};
  std::vector<VkrBakeLightmapTexel> texels(3u);
  texels[0].x = 1u;
  texels[0].y = 1u;
  texels[1].x = 2u;
  texels[1].y = 1u;
  texels[2].x = 1u;
  texels[2].y = 2u;
  const std::vector<Vec3> directions = {vec3_new(0.0f, 0.6f, 0.0f), vec3_zero(),
                                        vec3_new(3.0f, 0.0f, 0.0f)};
  std::vector<float32_t> rgba;
  assert(vkr_bake_lightmap_compose_direction_page(layout, 0u, texels,
                                                  directions, 1u, &rgba));
  assert(rgba.size() == 8u * 8u * 4u);
  const float32_t expected[3][4] = {{0.5f, 1.0f, 0.5f, 0.6f},
                                    {0.5f, 0.5f, 0.5f, 0.0f},
                                    {1.0f, 0.5f, 0.5f, 1.0f}};
  for (uint32_t i = 0u; i < 3u; ++i) {
    const float32_t *texel = &rgba[4u * (texels[i].y * 8u + texels[i].x)];
    for (uint32_t c = 0u; c < 4u; ++c) {
      assert(fabsf(texel[c] - expected[i][c]) < 1.0e-6f);
    }
  }
  /* (3, 3) has no covered or first-ring neighbor, so it takes the mean of
     the covered texels. */
  const float32_t *corner = &rgba[4u * (3u * 8u + 3u)];
  for (uint32_t c = 0u; c < 4u; ++c) {
    const float32_t mean =
        (expected[0][c] + expected[1][c] + expected[2][c]) / 3.0f;
    assert(fabsf(corner[c] - mean) < 1.0e-6f);
  }
  for (uint32_t y = 0u; y < 8u; ++y) {
    for (uint32_t x = 0u; x < 8u; ++x) {
      if (x < 4u && y < 4u) {
        continue;
      }
      const float32_t *texel = &rgba[4u * (y * 8u + x)];
      assert(texel[0] == 0.5f && texel[1] == 0.5f && texel[2] == 0.5f &&
             texel[3] == 0.0f);
    }
  }
  assert(!vkr_bake_lightmap_compose_direction_page(
      layout, 0u, texels, std::vector<Vec3>(2u), 1u, &rgba));
  printf("  test_compose_direction_page_encodes_and_stays_neutral PASSED\n");
}

bool32_t run_lightmap_bake_tests(void) {
  printf("--- Starting Lightmap Bake Tests ---\n");
  test_pack_sizes_and_places_rects();
  test_rasterize_covers_a_quad_once();
  test_cooked_cube_texels_lie_on_its_faces();
  test_pack_fitted_shrinks_small_scenes();
  test_compose_fills_each_rect_alone();
  test_astc_hdr_round_trip_keeps_range();
  test_lightmap_set_round_trip_and_rejects();
  test_lightmap_set_lamp_direct_round_trip_and_rejects();
  test_rgb9e5_round_trip();
  test_light_layer_sun_weights();
  test_diffuse_volume_v3_round_trip_and_rejects();
  test_lightmap_denoise_stays_on_surfaces();
  test_lightmap_denoise_atrous_keeps_edges();
  test_lightmap_fill_buried_texels();
  test_bake_material_accepts_roughness_bound();
  test_bake_scene_builds_blockout_stairs();
  test_bake_scene_leaves_out_moving_brushes();
  test_sparse_bricks_place_and_relocate();
  test_l1_lambert_furnace();
  test_point_lamp_direct_l1();
  test_compose_direction_page_encodes_and_stays_neutral();
  printf("--- Lightmap Bake Tests Completed ---\n");
  return true_v;
}

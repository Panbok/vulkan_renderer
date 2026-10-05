#include "lightmap_bake_tests.h"

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
 * texel is claimed once. */

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

} // namespace

bool32_t run_lightmap_bake_tests(void) {
  printf("--- Starting Lightmap Bake Tests ---\n");
  test_pack_sizes_and_places_rects();
  test_rasterize_covers_a_quad_once();
  test_cooked_cube_texels_lie_on_its_faces();
  printf("--- Lightmap Bake Tests Completed ---\n");
  return true_v;
}

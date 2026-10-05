#include "mesh_lightmap_uv_tests.h"

#include "assets/vkr_mesh_lightmap_uv.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestLightmapMesh {
  float positions[512 * 3];
  float normals[512 * 3];
  uint32_t indices[1024 * 3];
  uint32_t vertex_count;
  uint32_t index_count;
} TestLightmapMesh;

typedef struct TestLightmapResult {
  VkrMeshLightmapUvInfo info;
  uint32_t *source_vertices;
  float *uv2;
  uint32_t *indices;
} TestLightmapResult;

static void test_lightmap_add_vertex(TestLightmapMesh *mesh, float x, float y,
                                     float z, float nx, float ny, float nz) {
  const uint32_t i = mesh->vertex_count++;
  assert(i < 512u);
  mesh->positions[3u * i + 0u] = x;
  mesh->positions[3u * i + 1u] = y;
  mesh->positions[3u * i + 2u] = z;
  mesh->normals[3u * i + 0u] = nx;
  mesh->normals[3u * i + 1u] = ny;
  mesh->normals[3u * i + 2u] = nz;
}

static void test_lightmap_add_triangle(TestLightmapMesh *mesh, uint32_t a,
                                       uint32_t b, uint32_t c) {
  assert(mesh->index_count + 3u <= 1024u * 3u);
  mesh->indices[mesh->index_count++] = a;
  mesh->indices[mesh->index_count++] = b;
  mesh->indices[mesh->index_count++] = c;
}

/* A unit cube with four vertices per face, as an importer emits it. */
static void test_lightmap_build_cube(TestLightmapMesh *mesh) {
  memset(mesh, 0, sizeof(*mesh));
  const float n[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                         {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  for (uint32_t face = 0u; face < 6u; ++face) {
    float u[3] = {n[face][1], n[face][2], n[face][0]};
    float v[3] = {n[face][2], n[face][0], n[face][1]};
    const uint32_t base = mesh->vertex_count;
    for (uint32_t corner = 0u; corner < 4u; ++corner) {
      const float su = (corner == 1u || corner == 2u) ? 0.5f : -0.5f;
      const float sv = (corner >= 2u) ? 0.5f : -0.5f;
      test_lightmap_add_vertex(mesh, 0.5f * n[face][0] + su * u[0] + sv * v[0],
                               0.5f * n[face][1] + su * u[1] + sv * v[1],
                               0.5f * n[face][2] + su * u[2] + sv * v[2],
                               n[face][0], n[face][1], n[face][2]);
    }
    test_lightmap_add_triangle(mesh, base, base + 1u, base + 2u);
    test_lightmap_add_triangle(mesh, base, base + 2u, base + 3u);
  }
}

/* A 10x10 grid, 4 units on a side, with a bump so it is not developable
   into one flat chart. */
static void test_lightmap_build_bumped_plane(TestLightmapMesh *mesh) {
  memset(mesh, 0, sizeof(*mesh));
  const uint32_t cells = 10u;
  for (uint32_t y = 0u; y <= cells; ++y) {
    for (uint32_t x = 0u; x <= cells; ++x) {
      const float px = 4.0f * (float)x / (float)cells - 2.0f;
      const float pz = 4.0f * (float)y / (float)cells - 2.0f;
      const float height = 1.5f * expf(-(px * px + pz * pz));
      test_lightmap_add_vertex(mesh, px, height, pz, 0.0f, 1.0f, 0.0f);
    }
  }
  for (uint32_t y = 0u; y < cells; ++y) {
    for (uint32_t x = 0u; x < cells; ++x) {
      const uint32_t a = y * (cells + 1u) + x;
      test_lightmap_add_triangle(mesh, a, a + cells + 1u, a + 1u);
      test_lightmap_add_triangle(mesh, a + 1u, a + cells + 1u, a + cells + 2u);
    }
  }
}

static VkrMeshLightmapUvStatus
test_lightmap_generate(const TestLightmapMesh *mesh, float texels_per_unit,
                       uint32_t max_size, TestLightmapResult *out) {
  memset(out, 0, sizeof(*out));
  const VkrMeshLightmapUvInput input = {
      .positions = mesh->positions,
      .position_stride = 3u * sizeof(float),
      .normals = mesh->normals,
      .normal_stride = 3u * sizeof(float),
      .indices = mesh->indices,
      .index_count = mesh->index_count,
      .vertex_count = mesh->vertex_count,
      .texels_per_unit = texels_per_unit,
      .padding = 2u,
      .max_size = max_size,
  };
  VkrMeshLightmapUvAtlas *atlas = NULL;
  const VkrMeshLightmapUvStatus status =
      vkr_mesh_lightmap_uv_generate(&input, &atlas, &out->info);
  if (status != VKR_MESH_LIGHTMAP_UV_OK)
    return status;
  out->source_vertices = malloc(out->info.vertex_count * sizeof(uint32_t));
  out->uv2 = malloc(out->info.vertex_count * 2u * sizeof(float));
  out->indices = malloc(out->info.index_count * sizeof(uint32_t));
  assert(out->source_vertices && out->uv2 && out->indices);
  vkr_mesh_lightmap_uv_copy(atlas, out->source_vertices, out->uv2,
                            out->indices);
  vkr_mesh_lightmap_uv_destroy(atlas);
  return status;
}

static void test_lightmap_release(TestLightmapResult *result) {
  free(result->source_vertices);
  free(result->uv2);
  free(result->indices);
  memset(result, 0, sizeof(*result));
}

static float test_lightmap_edge(const float *a, const float *b,
                                const float *p) {
  return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
}

/* Checks the properties the cook and bake rely on; returns covered texels. */
static uint32_t test_lightmap_check(const TestLightmapMesh *mesh,
                                    const TestLightmapResult *result) {
  const VkrMeshLightmapUvInfo *info = &result->info;
  assert(info->index_count == mesh->index_count);
  assert(info->vertex_count >= mesh->vertex_count);
  assert(info->chart_count >= 1u);

  /* Face order and corner order survive: every output corner copies the
     input corner at the same position in the index list. */
  for (uint32_t i = 0u; i < info->index_count; ++i) {
    const uint32_t output = result->indices[i];
    assert(output < info->vertex_count);
    assert(result->source_vertices[output] == mesh->indices[i]);
  }
  for (uint32_t i = 0u; i < info->vertex_count; ++i) {
    assert(result->uv2[2u * i] >= 0.0f && result->uv2[2u * i] <= 1.0f);
    assert(result->uv2[2u * i + 1u] >= 0.0f &&
           result->uv2[2u * i + 1u] <= 1.0f);
  }

  /* Every triangle keeps a positive chart area, and no texel center lies
     strictly inside two triangles. */
  uint8_t *coverage = calloc((size_t)info->width * info->height, 1u);
  assert(coverage);
  uint32_t covered = 0u;
  for (uint32_t t = 0u; t < info->index_count; t += 3u) {
    float corners[3][2];
    for (uint32_t c = 0u; c < 3u; ++c) {
      const uint32_t v = result->indices[t + c];
      corners[c][0] = result->uv2[2u * v] * (float)info->width;
      corners[c][1] = result->uv2[2u * v + 1u] * (float)info->height;
    }
    const float area = test_lightmap_edge(corners[0], corners[1], corners[2]);
    assert(fabsf(area) > 1e-6f);
    const float sign = area > 0.0f ? 1.0f : -1.0f;
    for (uint32_t y = 0u; y < info->height; ++y) {
      for (uint32_t x = 0u; x < info->width; ++x) {
        const float p[2] = {(float)x + 0.5f, (float)y + 0.5f};
        const float e0 = sign * test_lightmap_edge(corners[0], corners[1], p);
        const float e1 = sign * test_lightmap_edge(corners[1], corners[2], p);
        const float e2 = sign * test_lightmap_edge(corners[2], corners[0], p);
        if (e0 > 1e-4f && e1 > 1e-4f && e2 > 1e-4f) {
          uint8_t *cell = &coverage[(size_t)y * info->width + x];
          assert(*cell == 0u);
          *cell = 1u;
          ++covered;
        }
      }
    }
  }
  free(coverage);
  return covered;
}

static void test_lightmap_uv_cube_and_plane(void) {
  printf("  test_lightmap_uv_cube_and_plane...\n");
  static TestLightmapMesh mesh;
  test_lightmap_build_cube(&mesh);
  TestLightmapResult cube = {0};
  assert(test_lightmap_generate(&mesh, 32.0f, 4096u, &cube) ==
         VKR_MESH_LIGHTMAP_UV_OK);
  const uint32_t cube_covered = test_lightmap_check(&mesh, &cube);
  /* Six 32x32-texel faces cover about 6,144 texel centers. */
  assert(cube_covered > 4000u);
  test_lightmap_release(&cube);

  test_lightmap_build_bumped_plane(&mesh);
  TestLightmapResult plane = {0};
  assert(test_lightmap_generate(&mesh, 16.0f, 4096u, &plane) ==
         VKR_MESH_LIGHTMAP_UV_OK);
  assert(test_lightmap_check(&mesh, &plane) > 3000u);
  test_lightmap_release(&plane);
  printf("  test_lightmap_uv_cube_and_plane PASSED\n");
}

static void test_lightmap_uv_is_deterministic(void) {
  printf("  test_lightmap_uv_is_deterministic...\n");
  static TestLightmapMesh mesh;
  test_lightmap_build_bumped_plane(&mesh);
  TestLightmapResult first = {0};
  TestLightmapResult second = {0};
  assert(test_lightmap_generate(&mesh, 16.0f, 4096u, &first) ==
         VKR_MESH_LIGHTMAP_UV_OK);
  assert(test_lightmap_generate(&mesh, 16.0f, 4096u, &second) ==
         VKR_MESH_LIGHTMAP_UV_OK);
  assert(memcmp(&first.info, &second.info, sizeof(first.info)) == 0);
  assert(memcmp(first.source_vertices, second.source_vertices,
                first.info.vertex_count * sizeof(uint32_t)) == 0);
  assert(memcmp(first.uv2, second.uv2,
                first.info.vertex_count * 2u * sizeof(float)) == 0);
  assert(memcmp(first.indices, second.indices,
                first.info.index_count * sizeof(uint32_t)) == 0);
  test_lightmap_release(&first);
  test_lightmap_release(&second);
  printf("  test_lightmap_uv_is_deterministic PASSED\n");
}

static void test_lightmap_uv_rejects_bad_input_and_oversize(void) {
  printf("  test_lightmap_uv_rejects_bad_input_and_oversize...\n");
  static TestLightmapMesh mesh;
  test_lightmap_build_cube(&mesh);
  TestLightmapResult result = {0};
  assert(test_lightmap_generate(&mesh, 32.0f, 16u, &result) ==
         VKR_MESH_LIGHTMAP_UV_TOO_LARGE);
  assert(result.info.width > 16u || result.info.height > 16u);
  assert(test_lightmap_generate(&mesh, 0.0f, 4096u, &result) ==
         VKR_MESH_LIGHTMAP_UV_INVALID_INPUT);
  mesh.index_count = 4u;
  assert(test_lightmap_generate(&mesh, 32.0f, 4096u, &result) ==
         VKR_MESH_LIGHTMAP_UV_INVALID_INPUT);
  printf("  test_lightmap_uv_rejects_bad_input_and_oversize PASSED\n");
}

bool32_t run_mesh_lightmap_uv_tests(void) {
  printf("--- Starting Mesh Lightmap UV Tests ---\n");
  test_lightmap_uv_cube_and_plane();
  test_lightmap_uv_is_deterministic();
  test_lightmap_uv_rejects_bad_input_and_oversize();
  printf("--- Mesh Lightmap UV Tests Completed ---\n");
  return true_v;
}

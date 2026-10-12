#include "mesh_raycast_tests.h"

#include "assets/vkr_mesh_encode.h"
#include "math/vkr_transform.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_mesh_raycast.h"
#include "renderer/systems/vkr_texture_system.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A native publisher table: every publication succeeds at once. */
typedef struct RaycastTestContext {
  Arena *arena;
  VkrDMemory cache_memory;
  VkrAllocator cache_allocator;
  VkrAssetPublisher publisher;
  VkrTextureSystem textures;
  VkrMaterialSystem materials;
  VkrGeometrySystem geometry;
  VkrMeshManager meshes;
  /* A one-triangle in-memory mesh result the instances share; its triangle
     rises along X: (0,0,0), (1,0,1), (0,1,0), the plane z = x. */
  VkrVertex3d vertices[3];
  uint32_t indices[3];
  VkrMeshLoaderSubset subset;
  VkrMeshLoaderResult mesh;
} RaycastTestContext;

static const VkrPackedVec3 s_slope_positions[3] = {
    {0.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 1.0f},
    {0.0f, 1.0f, 0.0f},
};

static const uint32_t s_slope_indices[3] = {0u, 1u, 2u};

static bool8_t raycast_test_upload_available(void *state, uint64_t bytes) {
  (void)state;
  return bytes != 0u;
}

static VkrRendererError
raycast_test_publish_texture(void *state, VkrTextureHandle handle,
                             const struct VkrTexturePreparedLoad *texture) {
  (void)state;
  (void)handle;
  (void)texture;
  return VKR_RENDERER_ERROR_NONE;
}

static bool8_t
raycast_test_publish_writable_texture(void *state, VkrTextureHandle handle,
                                      const VkrTextureDescription *texture) {
  (void)state;
  (void)handle;
  (void)texture;
  return true_v;
}

static bool8_t raycast_test_unpublish_texture(void *state,
                                              VkrTextureHandle handle) {
  (void)state;
  (void)handle;
  return true_v;
}

static bool8_t
raycast_test_publish_material(void *state, VkrMaterialHandle handle,
                              const struct VkrMaterial *material) {
  (void)state;
  (void)handle;
  (void)material;
  return true_v;
}

static bool8_t raycast_test_unpublish_material(void *state,
                                               VkrMaterialHandle handle) {
  (void)state;
  (void)handle;
  return true_v;
}

static bool8_t
raycast_test_publish_geometry(void *state, VkrGeometryHandle handle,
                              const struct VkrGeometryConfig *geometry) {
  (void)state;
  (void)handle;
  (void)geometry;
  return true_v;
}

static bool8_t raycast_test_unpublish_geometry(void *state,
                                               VkrGeometryHandle handle) {
  (void)state;
  (void)handle;
  return true_v;
}

static void raycast_test_init(RaycastTestContext *ctx) {
  MemZero(ctx, sizeof(*ctx));
  ctx->arena = arena_create(MB(8), MB(8));
  assert(ctx->arena != NULL);
  assert(vkr_dmemory_create(MB(1), MB(16), &ctx->cache_memory));
  ctx->cache_allocator = (VkrAllocator){.ctx = &ctx->cache_memory};
  vkr_dmemory_allocator_create(&ctx->cache_allocator);
  ctx->publisher = (VkrAssetPublisher){
      .texture_upload_available = raycast_test_upload_available,
      .publish_texture = raycast_test_publish_texture,
      .publish_writable_texture = raycast_test_publish_writable_texture,
      .unpublish_texture = raycast_test_unpublish_texture,
      .publish_material = raycast_test_publish_material,
      .unpublish_material = raycast_test_unpublish_material,
      .publish_geometry = raycast_test_publish_geometry,
      .unpublish_geometry = raycast_test_unpublish_geometry,
  };

  const VkrDeviceInformation device_info = {
      .supports_texture_bc7 = true_v,
      .supports_texture_bc5 = true_v,
  };
  const VkrTextureSystemConfig texture_config = {
      .max_texture_count = 64,
      .asset_publisher = &ctx->publisher,
  };
  assert(vkr_texture_system_init(&device_info, &texture_config, NULL,
                                 &ctx->textures));
  const VkrMaterialSystemConfig material_config = {
      .max_material_count = 64,
      .asset_publisher = &ctx->publisher,
  };
  assert(vkr_material_system_init(&ctx->materials, ctx->arena, &ctx->textures,
                                  &material_config));
  const VkrGeometrySystemConfig geometry_config = {
      .max_geometries = 64,
      .asset_publisher = &ctx->publisher,
  };
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_geometry_system_init(&ctx->geometry, &geometry_config, &error));
  const VkrMeshManagerConfig mesh_config = {.max_mesh_count = 16};
  assert(vkr_mesh_manager_init(&ctx->meshes, &ctx->geometry, &ctx->materials,
                               &mesh_config));

  for (uint32_t i = 0u; i < 3u; ++i) {
    ctx->vertices[i].position = s_slope_positions[i];
    ctx->vertices[i].normal = (VkrPackedVec3){-0.70710678f, 0.0f, 0.70710678f};
    ctx->vertices[i].colour = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};
    ctx->vertices[i].tangent = (Vec4){0.70710678f, 0.0f, 0.70710678f, 1.0f};
    ctx->indices[i] = s_slope_indices[i];
  }
  ctx->subset = (VkrMeshLoaderSubset){
      .geometry_config =
          {
              .vertex_size = sizeof(VkrVertex3d),
              .vertex_count = 3u,
              .vertices = ctx->vertices,
              .index_size = sizeof(uint32_t),
              .index_count = 3u,
              .indices = ctx->indices,
              .center = vec3_new(0.5f, 0.5f, 0.5f),
              .min_extents = vec3_new(-0.5f, -0.5f, -0.5f),
              .max_extents = vec3_new(0.5f, 0.5f, 0.5f),
          },
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
  };
  ctx->mesh = (VkrMeshLoaderResult){
      .source_path = string8_lit("tests/raycast_test_mesh"),
      .subsets = {.length = 1u, .capacity = 1u, .data = &ctx->subset},
  };
}

static void raycast_test_shutdown(RaycastTestContext *ctx) {
  vkr_mesh_manager_shutdown(&ctx->meshes);
  vkr_geometry_system_shutdown(&ctx->geometry);
  vkr_material_system_shutdown(&ctx->materials);
  vkr_texture_system_shutdown(&ctx->textures);
  vkr_dmemory_allocator_destroy(&ctx->cache_allocator);
  arena_destroy(ctx->arena);
}

/* An instance of `result` loaded as `mesh_path` (source mesh
   `source_plus_one`, zero for none), placed by `transform`. */
static VkrMeshInstanceHandle
raycast_test_instance_of(RaycastTestContext *ctx, VkrMeshLoaderResult *result,
                         const char *mesh_path, uint32_t source_plus_one,
                         VkrTransform transform) {
  const VkrMeshLoadDesc desc = {
      .mesh_path = string8_create_from_cstr((const uint8_t *)mesh_path,
                                            strlen(mesh_path)),
      .transform = transform,
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .source_mesh_index_plus_one = source_plus_one,
  };
  const VkrResourceHandleInfo info = {
      .type = VKR_RESOURCE_TYPE_MESH,
      .as.mesh = result,
  };
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrMeshInstanceHandle handle =
      vkr_mesh_manager_create_instance_from_resource(&ctx->meshes, &desc, &info,
                                                     1u, true_v, &error);
  assert(handle.id != 0u && error == VKR_RENDERER_ERROR_NONE);
  const VkrMeshInstance *instance =
      vkr_mesh_manager_get_instance(&ctx->meshes, handle);
  assert(instance->loading_state == VKR_MESH_LOADING_STATE_LOADED);
  return handle;
}

/* An instance of the sloped in-memory triangle under `mesh_path`. */
static VkrMeshInstanceHandle raycast_test_instance(RaycastTestContext *ctx,
                                                   const char *mesh_path,
                                                   VkrTransform transform) {
  return raycast_test_instance_of(ctx, &ctx->mesh, mesh_path, 0u, transform);
}

/* Seeds the sloped triangle for the asset `instance` draws. */
static void raycast_test_seed(RaycastTestContext *ctx,
                              VkrMeshRaycastCache *cache,
                              VkrMeshInstanceHandle instance) {
  const VkrMeshInstance *mesh_instance =
      vkr_mesh_manager_get_instance(&ctx->meshes, instance);
  assert(mesh_instance != NULL);
  assert(vkr_mesh_raycast_cache_insert_triangles(
      cache, &ctx->meshes, mesh_instance->asset, s_slope_positions,
      ArrayCount(s_slope_positions), s_slope_indices,
      ArrayCount(s_slope_indices)));
}

static void raycast_test_near(Vec3 actual, Vec3 expected, float32_t epsilon) {
  assert(fabsf(actual.x - expected.x) <= epsilon);
  assert(fabsf(actual.y - expected.y) <= epsilon);
  assert(fabsf(actual.z - expected.z) <= epsilon);
}

/* A segment through the triangle hits it from either side with the normal
   facing the segment; one that passes beside it or stops short misses. */
static void test_raycast_hits_triangle(void) {
  RaycastTestContext ctx;
  raycast_test_init(&ctx);
  VkrMeshRaycastCache *cache =
      vkr_mesh_raycast_cache_create(&ctx.cache_allocator, MB(1));
  assert(cache != NULL);
  const VkrMeshInstanceHandle handle = raycast_test_instance(
      &ctx, "tests/raycast_test_mesh", vkr_transform_identity());
  raycast_test_seed(&ctx, cache, handle);

  /* Down onto the plane z = x at (0.25, 0.25): it lies at z = 0.25. */
  VkrMeshRaycastHit hit = {0};
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, handle,
                                   vec3_new(0.25f, 0.25f, 2.0f),
                                   vec3_new(0.0f, 0.0f, -4.0f), &hit));
  raycast_test_near(hit.position, vec3_new(0.25f, 0.25f, 0.25f), 1e-5f);
  assert(fabsf(hit.fraction - 0.4375f) <= 1e-5f);
  raycast_test_near(hit.normal, vec3_new(-0.70710678f, 0.0f, 0.70710678f),
                    1e-5f);

  /* Up from below: the same point, the other face. */
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, handle,
                                   vec3_new(0.25f, 0.25f, -2.0f),
                                   vec3_new(0.0f, 0.0f, 4.0f), &hit));
  raycast_test_near(hit.position, vec3_new(0.25f, 0.25f, 0.25f), 1e-5f);
  assert(fabsf(hit.fraction - 0.5625f) <= 1e-5f);
  raycast_test_near(hit.normal, vec3_new(0.70710678f, 0.0f, -0.70710678f),
                    1e-5f);

  /* Beside the hypotenuse, and short of the plane. */
  assert(!vkr_mesh_raycast_instance(cache, &ctx.meshes, handle,
                                    vec3_new(0.6f, 0.6f, 2.0f),
                                    vec3_new(0.0f, 0.0f, -4.0f), &hit));
  assert(!vkr_mesh_raycast_instance(cache, &ctx.meshes, handle,
                                    vec3_new(0.25f, 0.25f, 2.0f),
                                    vec3_new(0.0f, 0.0f, -1.0f), &hit));

  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, handle));
  vkr_mesh_raycast_cache_destroy(cache);
  raycast_test_shutdown(&ctx);
  printf("  test_raycast_hits_triangle PASSED\n");
}

/* A translated instance scaled twice along X hits where its model puts the
   triangle, with the normal of the stretched plane (inverse transpose), not
   the model-transformed normal. */
static void test_raycast_transformed_instance(void) {
  RaycastTestContext ctx;
  raycast_test_init(&ctx);
  VkrMeshRaycastCache *cache =
      vkr_mesh_raycast_cache_create(&ctx.cache_allocator, MB(1));
  assert(cache != NULL);
  const VkrTransform transform =
      vkr_transform_new(vec3_new(10.0f, 0.0f, 0.0f), vkr_quat_identity(),
                        vec3_new(2.0f, 1.0f, 1.0f));
  const VkrMeshInstanceHandle handle =
      raycast_test_instance(&ctx, "tests/raycast_test_mesh", transform);
  raycast_test_seed(&ctx, cache, handle);

  /* World plane: z = (x - 10) / 2, so at x = 11 it lies at z = 0.5. */
  VkrMeshRaycastHit hit = {0};
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, handle,
                                   vec3_new(11.0f, 0.25f, 5.0f),
                                   vec3_new(0.0f, 0.0f, -10.0f), &hit));
  raycast_test_near(hit.position, vec3_new(11.0f, 0.25f, 0.5f), 1e-4f);
  assert(fabsf(hit.fraction - 0.45f) <= 1e-5f);
  raycast_test_near(hit.normal, vec3_new(-0.4472136f, 0.0f, 0.8944272f), 1e-5f);

  /* The untransformed triangle's place is empty now. */
  assert(!vkr_mesh_raycast_instance(cache, &ctx.meshes, handle,
                                    vec3_new(0.25f, 0.25f, 2.0f),
                                    vec3_new(0.0f, 0.0f, -4.0f), &hit));

  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, handle));
  vkr_mesh_raycast_cache_destroy(cache);
  raycast_test_shutdown(&ctx);
  printf("  test_raycast_transformed_instance PASSED\n");
}

/* A destroyed instance misses; a new asset generation in the same slot does
   not reuse the old generation's triangles and, with no cooked file behind
   it, misses too. */
static void test_raycast_stale_instance(void) {
  RaycastTestContext ctx;
  raycast_test_init(&ctx);
  VkrMeshRaycastCache *cache =
      vkr_mesh_raycast_cache_create(&ctx.cache_allocator, MB(1));
  assert(cache != NULL);
  const VkrMeshInstanceHandle handle = raycast_test_instance(
      &ctx, "tests/raycast_test_mesh", vkr_transform_identity());
  raycast_test_seed(&ctx, cache, handle);
  const VkrMeshAssetHandle old_asset =
      vkr_mesh_manager_get_instance(&ctx.meshes, handle)->asset;
  const Vec3 origin = vec3_new(0.25f, 0.25f, 2.0f);
  const Vec3 delta = vec3_new(0.0f, 0.0f, -4.0f);
  VkrMeshRaycastHit hit = {0};
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, handle, origin, delta,
                                   &hit));

  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, handle));
  assert(!vkr_mesh_raycast_instance(cache, &ctx.meshes, handle, origin, delta,
                                    &hit));

  const VkrMeshInstanceHandle again = raycast_test_instance(
      &ctx, "tests/raycast_test_mesh", vkr_transform_identity());
  const VkrMeshAssetHandle new_asset =
      vkr_mesh_manager_get_instance(&ctx.meshes, again)->asset;
  assert(new_asset.generation != old_asset.generation);
  assert(!vkr_mesh_raycast_instance(cache, &ctx.meshes, again, origin, delta,
                                    &hit));

  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, again));
  vkr_mesh_raycast_cache_destroy(cache);
  raycast_test_shutdown(&ctx);
  printf("  test_raycast_stale_instance PASSED\n");
}

/* With room for two assets, caching a third evicts the least recently used
   one, not the first cached. */
static void test_raycast_evicts_least_recent(void) {
  RaycastTestContext ctx;
  raycast_test_init(&ctx);
  /* One triangle takes 3 positions, 3 indices and 1 node: 80 bytes. */
  VkrMeshRaycastCache *cache =
      vkr_mesh_raycast_cache_create(&ctx.cache_allocator, 170u);
  assert(cache != NULL);
  const VkrMeshInstanceHandle first =
      raycast_test_instance(&ctx, "tests/raycast_a", vkr_transform_identity());
  const VkrMeshInstanceHandle second =
      raycast_test_instance(&ctx, "tests/raycast_b", vkr_transform_identity());
  const VkrMeshInstanceHandle third =
      raycast_test_instance(&ctx, "tests/raycast_c", vkr_transform_identity());
  const Vec3 origin = vec3_new(0.25f, 0.25f, 2.0f);
  const Vec3 delta = vec3_new(0.0f, 0.0f, -4.0f);
  VkrMeshRaycastHit hit = {0};

  raycast_test_seed(&ctx, cache, first);
  raycast_test_seed(&ctx, cache, second);
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, first, origin, delta,
                                   &hit));
  raycast_test_seed(&ctx, cache, third);
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, first, origin, delta,
                                   &hit));
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, third, origin, delta,
                                   &hit));
  /* Evicted, and with no cooked file to read back. */
  assert(!vkr_mesh_raycast_instance(cache, &ctx.meshes, second, origin, delta,
                                    &hit));

  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, third));
  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, second));
  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, first));
  vkr_mesh_raycast_cache_destroy(cache);
  raycast_test_shutdown(&ctx);
  printf("  test_raycast_evicts_least_recent PASSED\n");
}

/* Two flat 2x2 quads, source mesh 0 at z = 0 and source mesh 1 at z = 2,
   one range each, in one cooked artifact. */
typedef struct RaycastTestTwoQuads {
  VkrVertex3d vertices[8];
  uint32_t indices[12];
  VkrGeometryUploadRange ranges[2];
  VkrMeshSourceMesh source_meshes[2];
  VkrMeshSourceNode nodes[2];
} RaycastTestTwoQuads;

static void raycast_test_two_quads_build(RaycastTestTwoQuads *quads) {
  const float32_t corners[4][2] = {
      {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
  for (uint32_t mesh = 0u; mesh < 2u; ++mesh) {
    const float32_t z = 2.0f * (float32_t)mesh;
    for (uint32_t corner = 0u; corner < 4u; ++corner) {
      quads->vertices[mesh * 4u + corner] = (VkrVertex3d){
          .position = {corners[corner][0], corners[corner][1], z},
          .normal = {0.0f, 0.0f, 1.0f},
          .texcoord = {0.5f * (corners[corner][0] + 1.0f),
                       0.5f * (corners[corner][1] + 1.0f)},
          .colour = {1.0f, 1.0f, 1.0f, 1.0f},
          .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
      };
    }
    const uint32_t base = mesh * 4u;
    const uint32_t quad_indices[6] = {base, base + 1u, base + 2u,
                                      base, base + 2u, base + 3u};
    MemCopy(&quads->indices[mesh * 6u], quad_indices, sizeof(quad_indices));
    quads->ranges[mesh] = (VkrGeometryUploadRange){
        .range_id = mesh,
        .first_index = 6u * mesh,
        .index_count = 6u,
        .center = vec3_new(0.0f, 0.0f, z),
        .min_extents = vec3_new(-1.0f, -1.0f, 0.0f),
        .max_extents = vec3_new(1.0f, 1.0f, 0.0f),
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
    };
    quads->source_meshes[mesh] = (VkrMeshSourceMesh){
        .source_mesh_index = mesh,
        .first_range = mesh,
        .range_count = 1u,
    };
    quads->nodes[mesh] = (VkrMeshSourceNode){
        .name = string8_lit("quad"),
        .local = mat4_identity(),
        .parent = UINT32_MAX,
        .mesh = mesh,
        .mesh_variant = mesh,
        .camera = UINT32_MAX,
        .skin = UINT32_MAX,
        .light = UINT32_MAX,
        .in_scene = true_v,
    };
  }
}

/* Each asset of a two-mesh cooked file reads its own source mesh only: a
   segment rising through both quads hits the lower quad on source mesh 0
   and the upper one on source mesh 1. The file decodes once for both, and
   again only after trim released it. */
static void test_raycast_decodes_cooked_source_mesh(void) {
  static const char dependency_path[] = "build/vkr_mesh_raycast_quads.bin";
  static const char cooked_path[] = "build/vkr_mesh_raycast_quads.vkb";
  FILE *dependency = fopen(dependency_path, "wb");
  assert(dependency != NULL);
  assert(fwrite("quads\n", 1u, 6u, dependency) == 6u);
  assert(fclose(dependency) == 0);

  RaycastTestContext ctx;
  raycast_test_init(&ctx);
  Arena *scratch_arena = arena_create(MB(16), MB(1));
  assert(scratch_arena != NULL);
  VkrAllocator scratch = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&scratch));

  static RaycastTestTwoQuads quads;
  raycast_test_two_quads_build(&quads);
  const String8 dependency_string = string8_lit(dependency_path);
  const VkrMeshCookedEncodeInfo info = {
      .source = {.nodes = {.data = quads.nodes, .length = 2u},
                 .meshes = {.data = quads.source_meshes, .length = 2u}},
      .source_path = dependency_string,
      .dependency_paths = &dependency_string,
      .dependency_count = 1u,
      .mesh_buffer = {.vertex_size = sizeof(VkrVertex3d),
                      .vertex_count = 8u,
                      .vertices = quads.vertices,
                      .index_size = sizeof(uint32_t),
                      .index_count = 12u,
                      .indices = quads.indices},
      .ranges = quads.ranges,
      .range_count = 2u,
      .budgets = vkr_packed_geometry_default_budgets(),
  };
  uint8_t *cooked = NULL;
  uint64_t cooked_size = 0u;
  assert(vkr_mesh_cooked_encode(&scratch, &info, &cooked, &cooked_size));
  FILE *file = fopen(cooked_path, "wb");
  assert(file != NULL);
  assert(fwrite(cooked, 1u, cooked_size, file) == cooked_size);
  assert(fclose(file) == 0);

  /* The instances need a loaded result; the ray cache reads the file again. */
  VkrArenaPool pool = {0};
  assert(vkr_arena_pool_create(MB(1), 1, &scratch, &pool));
  VkrMeshLoaderContext loader_context = {
      .arena_pool = &pool,
      .geometry_system = &ctx.geometry,
      .material_system = &ctx.materials,
  };
  VkrResourceLoader loader = vkr_mesh_loader_create(&loader_context);
  VkrResourceHandleInfo loaded = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(loader.load(&loader, string8_lit(cooked_path), &scratch, &loaded,
                     &error));
  VkrMeshLoaderResult *result = loaded.as.mesh;
  assert(result != NULL && result->source.meshes.length == 2u);

  const VkrMeshInstanceHandle lower = raycast_test_instance_of(
      &ctx, result, cooked_path, 1u, vkr_transform_identity());
  const VkrMeshInstanceHandle upper = raycast_test_instance_of(
      &ctx, result, cooked_path, 2u, vkr_transform_identity());

  VkrMeshRaycastCache *cache =
      vkr_mesh_raycast_cache_create(&ctx.cache_allocator, MB(1));
  assert(cache != NULL);
  const Vec3 origin = vec3_new(0.2f, 0.3f, -5.0f);
  const Vec3 delta = vec3_new(0.0f, 0.0f, 10.0f);
  VkrMeshRaycastHit hit = {0};
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, lower, origin, delta,
                                   &hit));
  raycast_test_near(hit.position, vec3_new(0.2f, 0.3f, 0.0f), 1e-3f);
  assert(fabsf(hit.fraction - 0.5f) <= 1e-4f);
  raycast_test_near(hit.normal, vec3_new(0.0f, 0.0f, -1.0f), 1e-5f);

  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, upper, origin, delta,
                                   &hit));
  raycast_test_near(hit.position, vec3_new(0.2f, 0.3f, 2.0f), 1e-3f);
  assert(fabsf(hit.fraction - 0.7f) <= 1e-4f);

  /* Past the quad's edge the segment misses. */
  assert(!vkr_mesh_raycast_instance(cache, &ctx.meshes, upper,
                                    vec3_new(1.5f, 0.0f, -5.0f), delta, &hit));

  /* The second asset extracted from the held file without decoding it. */
  assert(vkr_mesh_raycast_cache_decode_count(cache) == 1u);

  /* Trims nine seconds apart keep the held file: a new generation of the
     lower asset extracts from it again. */
  vkr_mesh_raycast_cache_trim(cache, 100.0);
  vkr_mesh_raycast_cache_trim(cache, 109.0);
  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, lower));
  const VkrMeshInstanceHandle lower_again = raycast_test_instance_of(
      &ctx, result, cooked_path, 1u, vkr_transform_identity());
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, lower_again, origin,
                                   delta, &hit));
  assert(fabsf(hit.fraction - 0.5f) <= 1e-4f);
  assert(vkr_mesh_raycast_cache_decode_count(cache) == 1u);

  /* That use restarts the idle time at the next trim; more than ten unused
     seconds after it release the file, so the next asset decodes again. */
  vkr_mesh_raycast_cache_trim(cache, 115.0);
  vkr_mesh_raycast_cache_trim(cache, 124.0);
  assert(vkr_mesh_raycast_cache_decode_count(cache) == 1u);
  vkr_mesh_raycast_cache_trim(cache, 125.5);
  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, upper));
  const VkrMeshInstanceHandle upper_again = raycast_test_instance_of(
      &ctx, result, cooked_path, 2u, vkr_transform_identity());
  assert(vkr_mesh_raycast_instance(cache, &ctx.meshes, upper_again, origin,
                                   delta, &hit));
  assert(fabsf(hit.fraction - 0.7f) <= 1e-4f);
  assert(vkr_mesh_raycast_cache_decode_count(cache) == 2u);

  /* Destroy releases the held file and the cached triangles. */
  vkr_mesh_raycast_cache_destroy(cache);
  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, upper_again));
  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, lower_again));
  loader.unload(&loader, &loaded, string8_lit(cooked_path));
  vkr_arena_pool_destroy(&scratch, &pool);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(scratch_arena);
  raycast_test_shutdown(&ctx);
  assert(remove(cooked_path) == 0);
  assert(remove(dependency_path) == 0);
  printf("  test_raycast_decodes_cooked_source_mesh PASSED\n");
}

bool32_t run_mesh_raycast_tests(void) {
  printf("--- Starting Mesh Raycast Tests ---\n");
  test_raycast_hits_triangle();
  test_raycast_transformed_instance();
  test_raycast_stale_instance();
  test_raycast_evicts_least_recent();
  test_raycast_decodes_cooked_source_mesh();
  printf("--- Mesh Raycast Tests Completed ---\n");
  return true_v;
}

#include "mesh_material_override_tests.h"

#include "math/vkr_transform.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "vkr_frame_input.h"

#include <assert.h>
#include <stdio.h>

/* A native publisher table: every publication succeeds at once. */
typedef struct OverrideTestContext {
  Arena *arena;
  VkrAssetPublisher publisher;
  VkrTextureSystem textures;
  VkrMaterialSystem materials;
  VkrGeometrySystem geometry;
  VkrMeshManager meshes;
  /* The asset's two submesh materials and two override materials. */
  VkrMaterialHandle stone;
  VkrMaterialHandle brick;
  VkrMaterialHandle red;
  VkrMaterialHandle blue;
  /* A two-submesh mesh result the instances share. */
  VkrVertex3d vertices[3];
  uint32_t indices[3];
  VkrMeshLoaderSubset subsets[2];
  VkrMeshLoaderResult mesh;
} OverrideTestContext;

static bool8_t override_test_upload_available(void *state, uint64_t bytes) {
  (void)state;
  return bytes != 0u;
}

static VkrRendererError
override_test_publish_texture(void *state, VkrTextureHandle handle,
                              const struct VkrTexturePreparedLoad *texture) {
  (void)state;
  (void)handle;
  (void)texture;
  return VKR_RENDERER_ERROR_NONE;
}

static bool8_t
override_test_publish_writable_texture(void *state, VkrTextureHandle handle,
                                       const VkrTextureDescription *texture) {
  (void)state;
  (void)handle;
  (void)texture;
  return true_v;
}

static bool8_t override_test_unpublish_texture(void *state,
                                               VkrTextureHandle handle) {
  (void)state;
  (void)handle;
  return true_v;
}

static bool8_t
override_test_publish_material(void *state, VkrMaterialHandle handle,
                               const struct VkrMaterial *material) {
  (void)state;
  (void)handle;
  (void)material;
  return true_v;
}

static bool8_t override_test_unpublish_material(void *state,
                                                VkrMaterialHandle handle) {
  (void)state;
  (void)handle;
  return true_v;
}

static bool8_t
override_test_publish_geometry(void *state, VkrGeometryHandle handle,
                               const struct VkrGeometryConfig *geometry) {
  (void)state;
  (void)handle;
  (void)geometry;
  return true_v;
}

static bool8_t override_test_unpublish_geometry(void *state,
                                                VkrGeometryHandle handle) {
  (void)state;
  (void)handle;
  return true_v;
}

/* References the material system counts for `handle`. */
static uint32_t override_test_refs(OverrideTestContext *ctx,
                                   VkrMaterialHandle handle) {
  const VkrMaterial *material =
      vkr_material_system_get_by_handle(&ctx->materials, handle);
  assert(material != NULL && material->name != NULL);
  const VkrMaterialEntry *entry = vkr_hash_table_get_VkrMaterialEntry(
      &ctx->materials.material_by_name, material->name);
  assert(entry != NULL);
  return entry->ref_count;
}

static bool8_t override_test_same(VkrMaterialHandle a, VkrMaterialHandle b) {
  return a.id == b.id && a.generation == b.generation;
}

static VkrMaterialHandle override_test_colored(OverrideTestContext *ctx,
                                               const char *name, Vec4 color) {
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrMaterialHandle handle =
      vkr_material_system_create_colored(&ctx->materials, name, color, &error);
  assert(handle.id != 0u && error == VKR_RENDERER_ERROR_NONE);
  return handle;
}

static void override_test_init(OverrideTestContext *ctx) {
  MemZero(ctx, sizeof(*ctx));
  ctx->arena = arena_create(MB(8), MB(8));
  assert(ctx->arena != NULL);
  ctx->publisher = (VkrAssetPublisher){
      .texture_upload_available = override_test_upload_available,
      .publish_texture = override_test_publish_texture,
      .publish_writable_texture = override_test_publish_writable_texture,
      .unpublish_texture = override_test_unpublish_texture,
      .publish_material = override_test_publish_material,
      .unpublish_material = override_test_unpublish_material,
      .publish_geometry = override_test_publish_geometry,
      .unpublish_geometry = override_test_unpublish_geometry,
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

  ctx->stone = override_test_colored(ctx, "override_test_stone",
                                     (Vec4){0.5f, 0.5f, 0.5f, 1.0f});
  ctx->brick = override_test_colored(ctx, "override_test_brick",
                                     (Vec4){0.6f, 0.3f, 0.2f, 1.0f});
  ctx->red = override_test_colored(ctx, "override_test_red",
                                   (Vec4){1.0f, 0.0f, 0.0f, 1.0f});
  ctx->blue = override_test_colored(ctx, "override_test_blue",
                                    (Vec4){0.0f, 0.0f, 1.0f, 1.0f});

  /* One triangle per submesh, each with its own material. */
  ctx->vertices[0].position = (VkrPackedVec3){0.0f, 0.0f, 0.0f};
  ctx->vertices[1].position = (VkrPackedVec3){1.0f, 0.0f, 0.0f};
  ctx->vertices[2].position = (VkrPackedVec3){0.0f, 1.0f, 0.0f};
  for (uint32_t i = 0u; i < 3u; ++i) {
    ctx->vertices[i].normal = (VkrPackedVec3){0.0f, 0.0f, 1.0f};
    ctx->vertices[i].colour = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};
    ctx->vertices[i].tangent = (Vec4){1.0f, 0.0f, 0.0f, 1.0f};
    ctx->indices[i] = i;
  }
  const VkrMaterialHandle submesh_materials[2] = {ctx->stone, ctx->brick};
  for (uint32_t i = 0u; i < 2u; ++i) {
    ctx->subsets[i] = (VkrMeshLoaderSubset){
        .geometry_config =
            {
                .vertex_size = sizeof(VkrVertex3d),
                .vertex_count = 3u,
                .vertices = ctx->vertices,
                .index_size = sizeof(uint32_t),
                .index_count = 3u,
                .indices = ctx->indices,
                .center = vec3_new(0.5f, 0.5f, 0.0f),
                .min_extents = vec3_new(-0.5f, -0.5f, 0.0f),
                .max_extents = vec3_new(0.5f, 0.5f, 0.0f),
            },
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
        .material_handle = submesh_materials[i],
    };
  }
  ctx->mesh = (VkrMeshLoaderResult){
      .source_path = string8_lit("tests/override_test_mesh"),
      .subsets = {.length = 2u, .capacity = 2u, .data = ctx->subsets},
  };
}

static void override_test_shutdown(OverrideTestContext *ctx) {
  vkr_mesh_manager_shutdown(&ctx->meshes);
  vkr_geometry_system_shutdown(&ctx->geometry);
  vkr_material_system_shutdown(&ctx->materials);
  vkr_texture_system_shutdown(&ctx->textures);
  arena_destroy(ctx->arena);
}

/* An instance of the shared two-submesh asset at `position`. */
static VkrMeshInstanceHandle override_test_instance(OverrideTestContext *ctx,
                                                    Vec3 position) {
  const VkrMeshLoadDesc desc = {
      .mesh_path = string8_lit("tests/override_test_mesh"),
      .transform = vkr_transform_new(position, vkr_quat_identity(), vec3_one()),
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
  };
  const VkrResourceHandleInfo info = {
      .type = VKR_RESOURCE_TYPE_MESH,
      .as.mesh = &ctx->mesh,
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

/* The material submesh `submesh` of `handle` draws. */
static VkrMaterialHandle override_test_drawn(OverrideTestContext *ctx,
                                             VkrMeshInstanceHandle handle,
                                             uint32_t submesh) {
  VkrMeshInstance *instance =
      vkr_mesh_manager_get_instance(&ctx->meshes, handle);
  const VkrMeshAsset *asset =
      vkr_mesh_manager_get_live_asset(&ctx->meshes, instance->asset);
  assert(submesh < asset->submeshes.length);
  return vkr_mesh_instance_submesh_material(
      instance, submesh, asset->submeshes.data[submesh].material);
}

/* An override reaches one instance and its submesh only; the instance's
   references return when the overrides clear and when it is destroyed. */
static void test_override_is_per_instance(void) {
  OverrideTestContext ctx;
  override_test_init(&ctx);

  const VkrMeshInstanceHandle first =
      override_test_instance(&ctx, vec3_new(0.0f, 0.0f, 0.0f));
  const VkrMeshInstanceHandle second =
      override_test_instance(&ctx, vec3_new(4.0f, 0.0f, 0.0f));
  const uint32_t red_refs = override_test_refs(&ctx, ctx.red);
  const uint32_t stone_refs = override_test_refs(&ctx, ctx.stone);
  const uint32_t brick_refs = override_test_refs(&ctx, ctx.brick);

  /* Submesh 1 of the first instance draws red; submesh 0 keeps stone. */
  const VkrMaterialHandle overrides[2] = {{0}, ctx.red};
  assert(vkr_mesh_manager_instance_set_materials(&ctx.meshes, first, overrides,
                                                 2u));
  assert(override_test_same(override_test_drawn(&ctx, first, 0u), ctx.stone));
  assert(override_test_same(override_test_drawn(&ctx, first, 1u), ctx.red));
  assert(override_test_same(override_test_drawn(&ctx, second, 0u), ctx.stone));
  assert(override_test_same(override_test_drawn(&ctx, second, 1u), ctx.brick));
  assert(override_test_refs(&ctx, ctx.red) == red_refs + 1u);
  assert(override_test_refs(&ctx, ctx.brick) == brick_refs);

  /* Replacing red by stone moves the reference; red returns to its count. */
  const VkrMaterialHandle stone_on_both[2] = {ctx.stone, ctx.stone};
  assert(vkr_mesh_manager_instance_set_materials(&ctx.meshes, first,
                                                 stone_on_both, 2u));
  assert(override_test_same(override_test_drawn(&ctx, first, 1u), ctx.stone));
  assert(override_test_refs(&ctx, ctx.red) == red_refs);
  assert(override_test_refs(&ctx, ctx.stone) == stone_refs + 2u);

  /* Empty entries clear the overrides. */
  const VkrMaterialHandle empty[VKR_MESH_MATERIAL_OVERRIDE_MAX] = {{0}};
  assert(vkr_mesh_manager_instance_set_materials(&ctx.meshes, first, empty,
                                                 ArrayCount(empty)));
  assert(override_test_same(override_test_drawn(&ctx, first, 1u), ctx.brick));
  assert(override_test_refs(&ctx, ctx.stone) == stone_refs);

  /* Too many entries and stale handles change nothing. */
  const uint32_t blue_refs = override_test_refs(&ctx, ctx.blue);
  const VkrMaterialHandle too_many[VKR_MESH_MATERIAL_OVERRIDE_MAX + 1u] = {
      ctx.blue};
  assert(!vkr_mesh_manager_instance_set_materials(&ctx.meshes, first, too_many,
                                                  ArrayCount(too_many)));
  assert(override_test_refs(&ctx, ctx.blue) == blue_refs);
  assert(override_test_same(override_test_drawn(&ctx, first, 0u), ctx.stone));

  /* Destroying an instance that holds overrides drops its references. */
  const VkrMaterialHandle blue_first[1] = {ctx.blue};
  assert(vkr_mesh_manager_instance_set_materials(&ctx.meshes, second,
                                                 blue_first, 1u));
  assert(override_test_refs(&ctx, ctx.blue) == blue_refs + 1u);
  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, second));
  assert(override_test_refs(&ctx, ctx.blue) == blue_refs);
  assert(!vkr_mesh_manager_instance_set_materials(&ctx.meshes, second,
                                                  blue_first, 1u));
  assert(override_test_refs(&ctx, ctx.blue) == blue_refs);

  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, first));
  override_test_shutdown(&ctx);
}

/* A static caster's override change advances the static generation with a
   change that reaches the instance and not distant retained shadows; a
   dynamic caster's advances only the dynamic generation. */
static void test_override_invalidates_by_mobility(void) {
  OverrideTestContext ctx;
  override_test_init(&ctx);

  const Vec3 position = vec3_new(10.0f, 0.0f, 0.0f);
  const VkrMeshInstanceHandle placed = override_test_instance(&ctx, position);
  assert(vkr_mesh_manager_instance_set_shadow_mobility(
      &ctx.meshes, placed, VKR_SHADOW_CASTER_MOBILITY_STATIC));
  const VkrMeshManagerGenerations before = ctx.meshes.generations;

  const VkrMaterialHandle red_first[1] = {ctx.red};
  assert(vkr_mesh_manager_instance_set_materials(&ctx.meshes, placed, red_first,
                                                 1u));
  assert(ctx.meshes.generations.static_content > before.static_content);
  assert(ctx.meshes.generations.dynamic_content == before.dynamic_content);
  assert(ctx.meshes.generations.topology == before.topology);

  VkrStaticChange changes[VKR_MESH_STATIC_CHANGE_MAX];
  uint64_t floor = 0u;
  const uint32_t change_count =
      vkr_mesh_manager_static_changes(&ctx.meshes, changes, &floor);
  const VkrWorldPassPayload world = {
      .static_generation = ctx.meshes.generations.static_content,
      .static_changes = changes,
      .static_change_count = change_count,
      .static_change_floor = floor,
  };
  assert(vkr_world_static_changes_reach(
      &world, before.static_content,
      vec3_add(position, vec3_new(0.5f, 0.5f, 0.0f)), 0.1f));
  assert(!vkr_world_static_changes_reach(&world, before.static_content,
                                         vec3_new(-100.0f, 0.0f, 0.0f), 1.0f));

  /* Setting the same overrides again is no change. */
  const VkrMeshManagerGenerations applied = ctx.meshes.generations;
  assert(vkr_mesh_manager_instance_set_materials(&ctx.meshes, placed, red_first,
                                                 1u));
  assert(ctx.meshes.generations.static_content == applied.static_content);

  assert(vkr_mesh_manager_instance_set_shadow_mobility(
      &ctx.meshes, placed, VKR_SHADOW_CASTER_MOBILITY_DYNAMIC));
  const VkrMeshManagerGenerations dynamic = ctx.meshes.generations;
  assert(
      vkr_mesh_manager_instance_set_materials(&ctx.meshes, placed, NULL, 0u));
  assert(ctx.meshes.generations.dynamic_content > dynamic.dynamic_content);
  assert(ctx.meshes.generations.static_content == dynamic.static_content);

  assert(vkr_mesh_manager_destroy_instance(&ctx.meshes, placed));
  override_test_shutdown(&ctx);
}

bool32_t run_mesh_material_override_tests(void) {
  printf("--- Starting Mesh Material Override Tests ---\n");
  test_override_is_per_instance();
  test_override_invalidates_by_mobility();
  printf("--- Mesh Material Override Tests Completed ---\n");
  return true_v;
}

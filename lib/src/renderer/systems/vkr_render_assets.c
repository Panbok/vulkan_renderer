#include "renderer/systems/vkr_render_assets.h"

#include "core/logger.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/vkr_ibl_math.h"

#define VKR_MESH_LOADER_ASYNC_DMEMORY_INITIAL MB(2)
#define VKR_MESH_LOADER_ASYNC_DMEMORY_RESERVE MB(32)
#define VKR_SCENE_LOADER_ASYNC_DMEMORY_INITIAL MB(8)
#define VKR_SCENE_LOADER_ASYNC_DMEMORY_RESERVE MB(256)

bool8_t vkr_render_assets_initialize(
    VkrRenderAssets *assets, const VkrAssetPublisher *publisher,
    const VkrDeviceInformation *device_info, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers) {
  if (!assets || !publisher || !device_info)
    return false_v;
  MemZero(assets, sizeof(*assets));
  assets->asset_publisher = publisher;
  assets->job_system = job_system;
  assets->arena = arena_create(MB(64));
  if (!assets->arena)
    return false_v;
  assets->allocator = (VkrAllocator){.ctx = assets->arena};
  if (!vkr_allocator_arena(&assets->allocator))
    return false_v;
  assets->scratch_arena = arena_create(MB(32), MB(1));
  if (!assets->scratch_arena)
    return false_v;
  assets->scratch_allocator = (VkrAllocator){.ctx = assets->scratch_arena};
  if (!vkr_allocator_arena(&assets->scratch_allocator))
    return false_v;
  if (metrics_producers) {
    assets->hdr_decode_metrics = metrics_producers->hdr_decode;
    assets->ibl_conversion_metrics = metrics_producers->ibl_conversion;
    assets->ibl_convolution_metrics = metrics_producers->ibl_convolution;
  }
  log_debug("Initializing resource registry");
  if (!vkr_resource_system_init(&assets->allocator, job_system,
                                metrics_producers)) {
    log_error("Resource registry initialization failed");
    return false_v;
  }
  assets->resource_system_initialized = true_v;
  log_debug("Initializing geometry assets");

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrGeometrySystemConfig geometry_config = {
      .max_geometries = 16384,
      .asset_publisher = assets->asset_publisher,
  };
  if (!vkr_geometry_system_init(&assets->geometry_system, &geometry_config,
                                &error)) {
    log_error("Geometry assets initialization failed");
    return false_v;
  }
  log_debug("Initializing texture assets");
  VkrTextureSystemConfig texture_config = {
      .max_texture_count = 16384,
      .asset_publisher = assets->asset_publisher,
  };
  if (!vkr_texture_system_init(device_info, &texture_config, job_system,
                               &assets->texture_system)) {
    log_error("Texture assets initialization failed");
    return false_v;
  }
  log_debug("Initializing material assets");
  assets->texture_system.hdr_decode_metrics = assets->hdr_decode_metrics;
  VkrMaterialSystemConfig material_config = {
      .max_material_count = 8192,
      .asset_publisher = assets->asset_publisher,
  };
  if (!vkr_material_system_init(&assets->material_system, assets->arena,
                                &assets->texture_system, &material_config)) {
    log_error("Material assets initialization failed");
    return false_v;
  }
  VkrMeshManagerConfig mesh_config = {.max_mesh_count = 16384};
  if (!vkr_mesh_manager_init(&assets->mesh_manager, &assets->geometry_system,
                             &assets->material_system, &mesh_config)) {
    return false_v;
  }

  const uint32_t pool_chunk_count =
      job_system ? job_system->worker_count + 4 : 8;
  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &assets->allocator,
                             &assets->mesh_arena_pool)) {
    return false_v;
  }
  assets->mesh_loader =
      (VkrMeshLoaderContext){.geometry_system = &assets->geometry_system,
                             .material_system = &assets->material_system,
                             .mesh_manager = &assets->mesh_manager,
                             .job_system = job_system,
                             .arena_pool = &assets->mesh_arena_pool};
  assets->mesh_loader.allocator.ctx = assets->arena;
  vkr_allocator_arena(&assets->mesh_loader.allocator);
  if (!vkr_dmemory_create(VKR_MESH_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_MESH_LOADER_ASYNC_DMEMORY_RESERVE,
                          &assets->mesh_loader.async_memory)) {
    return false_v;
  }
  assets->mesh_loader.async_allocator =
      (VkrAllocator){.ctx = &assets->mesh_loader.async_memory};
  vkr_dmemory_allocator_create(&assets->mesh_loader.async_allocator);
  if (!vkr_mutex_create(&assets->allocator, &assets->mesh_loader.async_mutex)) {
    return false_v;
  }
  if (!vkr_dmemory_create(VKR_SCENE_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_SCENE_LOADER_ASYNC_DMEMORY_RESERVE,
                          &assets->scene_async_memory)) {
    return false_v;
  }
  assets->scene_async_allocator =
      (VkrAllocator){.ctx = &assets->scene_async_memory};
  vkr_dmemory_allocator_create(&assets->scene_async_allocator);
  if (!vkr_mutex_create(&assets->allocator, &assets->scene_async_mutex)) {
    return false_v;
  }
  assets->mesh_manager.loader_context = &assets->mesh_loader;

  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &assets->allocator,
                             &assets->bitmap_font_arena_pool) ||
      !vkr_arena_pool_create(MB(6), pool_chunk_count, &assets->allocator,
                             &assets->system_font_arena_pool) ||
      !vkr_arena_pool_create(MB(6), pool_chunk_count, &assets->allocator,
                             &assets->mtsdf_font_arena_pool)) {
    return false_v;
  }
  assets->bitmap_font_loader = (VkrBitmapFontLoaderContext){
      .job_system = job_system, .arena_pool = &assets->bitmap_font_arena_pool};
  assets->system_font_loader = (VkrSystemFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &assets->system_font_arena_pool,
      .texture_system = &assets->texture_system,
  };
  assets->mtsdf_font_loader = (VkrMtsdfFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &assets->mtsdf_font_arena_pool,
      .texture_system = &assets->texture_system,
  };

  if (!vkr_resource_system_register_loader((void *)&assets->texture_system,
                                           vkr_texture_loader_create()))
    return false_v;
  if (!vkr_resource_system_register_loader((void *)&assets->material_system,
                                           vkr_material_loader_create()))
    return false_v;
  if (!vkr_resource_system_register_loader(
          (void *)&assets->mesh_loader,
          vkr_mesh_loader_create(&assets->mesh_loader)))
    return false_v;
  if (!vkr_resource_system_register_loader(
          (void *)&assets->bitmap_font_loader,
          vkr_bitmap_font_loader_create(&assets->bitmap_font_loader)))
    return false_v;
  if (!vkr_resource_system_register_loader(
          (void *)&assets->system_font_loader,
          vkr_system_font_loader_create(&assets->system_font_loader)))
    return false_v;
  if (!vkr_resource_system_register_loader(
          (void *)&assets->mtsdf_font_loader,
          vkr_mtsdf_font_loader_create(&assets->mtsdf_font_loader)))
    return false_v;
  if (!vkr_resource_system_register_loader(
          (void *)&assets->mtsdf_font_loader,
          vkr_cooked_font_loader_create(&assets->mtsdf_font_loader)))
    return false_v;
  if (!vkr_resource_system_register_loader((void *)assets,
                                           vkr_scene_loader_create()))
    return false_v;

  VkrFontSystemConfig font_config = {
      .max_system_font_count = 16,
      .max_bitmap_font_count = 16,
      .max_mtsdf_font_count = 16,
  };
  if (!vkr_font_system_init(&assets->font_system, &font_config, &error) ||
      !vkr_world_resources_init(assets, &assets->world_resources)) {
    return false_v;
  }
  return true_v;
}

void vkr_render_assets_shutdown(VkrRenderAssets *assets) {
  if (!assets)
    return;
  /* Workers and GPU have been drained by the caller. Return queued payloads
     before invalidating their loader contexts or independently freed storage.
   */
  if (assets->resource_system_initialized)
    vkr_resource_system_quiesce();
  if (assets->world_resources.initialized)
    vkr_world_resources_shutdown(assets, &assets->world_resources);
  if (assets->mesh_manager.arena)
    vkr_mesh_manager_shutdown(&assets->mesh_manager);
  vkr_font_system_shutdown(&assets->font_system);
  vkr_material_system_shutdown(&assets->material_system);
  vkr_geometry_system_shutdown(&assets->geometry_system);
  if (assets->texture_system.arena)
    vkr_texture_system_shutdown(&assets->texture_system);
  /* Font/material/texture teardown still queries registered loader metadata. */
  if (assets->resource_system_initialized)
    vkr_resource_system_shutdown();
  if (assets->mesh_arena_pool.initialized)
    vkr_arena_pool_destroy(&assets->allocator, &assets->mesh_arena_pool);
  if (assets->bitmap_font_arena_pool.initialized)
    vkr_arena_pool_destroy(&assets->allocator, &assets->bitmap_font_arena_pool);
  if (assets->system_font_arena_pool.initialized)
    vkr_arena_pool_destroy(&assets->allocator, &assets->system_font_arena_pool);
  if (assets->mtsdf_font_arena_pool.initialized)
    vkr_arena_pool_destroy(&assets->allocator, &assets->mtsdf_font_arena_pool);
  if (assets->scene_async_mutex)
    vkr_mutex_destroy(&assets->allocator, &assets->scene_async_mutex);
  if (assets->scene_async_allocator.ctx)
    vkr_dmemory_allocator_destroy(&assets->scene_async_allocator);
  if (assets->mesh_loader.async_mutex)
    vkr_mutex_destroy(&assets->allocator, &assets->mesh_loader.async_mutex);
  if (assets->mesh_loader.async_allocator.ctx)
    vkr_dmemory_allocator_destroy(&assets->mesh_loader.async_allocator);
  vkr_allocator_release_global_accounting(&assets->mesh_loader.allocator);
  vkr_allocator_release_global_accounting(&assets->allocator);
  vkr_allocator_release_global_accounting(&assets->scratch_allocator);
  arena_destroy(assets->scratch_arena);
  arena_destroy(assets->arena);
  MemZero(assets, sizeof(*assets));
}
bool8_t vkr_render_assets_texture_pressure_budget(
    const VkrDeviceMemoryStats *stats, bool8_t pressure_active,
    uint64_t *out_budget, bool8_t *out_pressure_active) {
  if (!stats || !out_budget || !out_pressure_active ||
      !stats->heap_usage_valid || stats->heap_count == 0u) {
    return false_v;
  }
  uint64_t usage = 0u;
  uint64_t budget = 0u;
  for (uint32_t i = 0u; i < stats->heap_count; ++i) {
    if (UINT64_MAX - usage < stats->heap_usage_bytes[i] ||
        UINT64_MAX - budget < stats->heap_budget_bytes[i]) {
      return false_v;
    }
    usage += stats->heap_usage_bytes[i];
    budget += stats->heap_budget_bytes[i];
  }
  if (budget == 0u) {
    return false_v;
  }
  const uint64_t projected_usage =
      stats->pending_texture_upload_bytes > budget - Min(usage, budget)
          ? budget
          : usage + stats->pending_texture_upload_bytes;
  if (projected_usage >= budget - budget / 10u) {
    const uint64_t texture_bytes =
        stats->owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes;
    const uint64_t non_texture_bytes =
        usage > texture_bytes ? usage - texture_bytes : 0u;
    const uint64_t target_usage = budget - budget / 5u;
    *out_budget = target_usage > non_texture_bytes
                      ? target_usage - non_texture_bytes
                      : 0u;
    *out_pressure_active = true_v;
    return true_v;
  }
  if (pressure_active && usage <= budget - budget / 4u) {
    *out_budget = UINT64_MAX;
    *out_pressure_active = false_v;
    return true_v;
  }
  return false_v;
}

bool8_t vkr_render_assets_pump(VkrRenderAssets *assets,
                               VkrResourceSubmissionState submission,
                               const VkrDeviceMemoryStats *device_memory) {
  if (!assets || !assets->resource_system_initialized)
    return false_v;
  if (device_memory &&
      !assets->material_system.texture_stream_budget_user_configured) {
    VkrDeviceMemoryStats stats = *device_memory;
    stats.owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes =
        assets->material_system.texture_stream_resident_bytes;
    uint64_t texture_budget = 0u;
    bool8_t pressure_active = assets->texture_pressure_active;
    if (vkr_render_assets_texture_pressure_budget(
            &stats, assets->texture_pressure_active, &texture_budget,
            &pressure_active)) {
      vkr_material_system_set_automatic_texture_residency_budget(
          &assets->material_system, texture_budget);
      assets->texture_pressure_active = pressure_active;
    }
  }
  const VkrAssetPublisher *publisher = assets->asset_publisher;
  const bool8_t batching = publisher->begin_texture_upload_batch != NULL;
  if (batching != (publisher->end_texture_upload_batch != NULL) ||
      (batching && !publisher->begin_texture_upload_batch(publisher->state))) {
    log_error("Asset texture upload batch initialization failed");
    return false_v;
  }
  vkr_resource_system_pump(submission, NULL);
  if (batching && !publisher->end_texture_upload_batch(publisher->state)) {
    log_error("Asset texture upload batch submission failed");
    return false_v;
  }
  vkr_material_system_pump_texture_streams(&assets->material_system, 32u);
  vkr_mesh_manager_pump_async(&assets->mesh_manager);
  return true_v;
}

uint32_t vkr_render_assets_ibl_sh_slot(const VkrRenderAssets *assets,
                                       VkrTextureHandle source) {
  const VkrAssetPublisher *publisher = assets ? assets->asset_publisher : NULL;
  return publisher && publisher->ibl_sh_slot
             ? publisher->ibl_sh_slot(publisher->state, source)
             : VKR_SH_SLOT_BLACK;
}

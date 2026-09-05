#pragma once

#include "core/vkr_threads.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/loaders/bitmap_font_loader.h"
#include "renderer/resources/loaders/cooked_font_loader.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/resources/loaders/mtsdf_font_loader.h"
#include "renderer/resources/loaders/system_font_loader.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_renderer_metrics.h"

/* Application-owned published assets. The publisher and job system are borrowed
 * and must outlive this object. Do not move it after initialization: loaders
 * and systems borrow its fields. Scratch views last until their explicit scope
 * ends. */
typedef struct VkrRenderAssets {
  Arena *arena;
  VkrAllocator allocator;
  Arena *scratch_arena;
  VkrAllocator scratch_allocator;
  const VkrAssetPublisher *asset_publisher;
  VkrJobSystem *job_system;
  bool8_t resource_system_initialized;
  VkrGeometrySystem geometry_system;
  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;
  VkrMeshManager mesh_manager;
  VkrFontSystem font_system;
  VkrWorldResources world_resources;
  VkrMeshLoaderContext mesh_loader;
  VkrArenaPool mesh_arena_pool;
  VkrDMemory scene_async_memory;
  VkrAllocator scene_async_allocator;
  VkrMutex scene_async_mutex;
  VkrBitmapFontLoaderContext bitmap_font_loader;
  VkrArenaPool bitmap_font_arena_pool;
  VkrSystemFontLoaderContext system_font_loader;
  VkrArenaPool system_font_arena_pool;
  VkrMtsdfFontLoaderContext mtsdf_font_loader;
  VkrArenaPool mtsdf_font_arena_pool;
  VkrMetricEventProducer hdr_decode_metrics;
  VkrMetricEventProducer ibl_conversion_metrics;
  VkrMetricEventProducer ibl_convolution_metrics;
  bool8_t texture_pressure_active;
} VkrRenderAssets;

/* On failure, partial ownership remains in assets for shutdown after the caller
 * joins workers and proves GPU idle. The native publisher stays alive
 * throughout. */
bool8_t vkr_render_assets_initialize(
    VkrRenderAssets *assets, const VkrAssetPublisher *publisher,
    const VkrDeviceInformation *device_info, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers);

/* Join workers and wait for GPU completion before calling. Scene/UI users must
 * already have released their borrowed assets. Safe after partial
 * initialization. */
void vkr_render_assets_shutdown(VkrRenderAssets *assets);

bool8_t vkr_render_assets_pump(VkrRenderAssets *assets,
                               VkrResourceSubmissionState submission,
                               const VkrDeviceMemoryStats *device_memory);
uint32_t vkr_render_assets_ibl_sh_slot(const VkrRenderAssets *assets,
                                       VkrTextureHandle source);

/* Pure pressure/hysteresis policy; false means keep the current budget. */
bool8_t vkr_render_assets_texture_pressure_budget(
    const VkrDeviceMemoryStats *stats, bool8_t pressure_active,
    uint64_t *out_budget, bool8_t *out_pressure_active);

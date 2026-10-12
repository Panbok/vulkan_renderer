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
#include "renderer/systems/vkr_mesh_raycast.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "vkr_renderer_metrics.h"

/* Application-owned published assets. The publisher and job system are borrowed
 * and must outlive this object. Do not move it after initialization: loaders
 * and systems borrow its fields. Scratch views last until their explicit scope
 * ends. */

/* The prefix of a managed asset reference, "asset:<scope>/<id>" (ADR-076):
 * the project asset a scene names without a revision path. */
#define VKR_ASSET_REF_PREFIX "asset:"
/* Bytes of a reference with its terminator: "asset:editor/" and a UUID fit. */
#define VKR_ASSET_REF_CAPACITY 64u

/* The current artifact path of one managed asset reference; the table owns
 * `path` (and the entry's key) in its own heap. */
typedef struct VkrAssetRefPath {
  char *path;
  uint64_t size;
} VkrAssetRefPath;
VkrHashTable(VkrAssetRefPath);

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
  /* CPU triangles of drawn meshes for ray tests where no collision exists,
     created on first use (vkr_render_assets_mesh_raycast). Main thread
     only. */
  VkrMeshRaycastCache *mesh_raycast;
  /* The cache's own heap: it frees one asset's triangles at a time. */
  VkrDMemory mesh_raycast_memory;
  VkrAllocator mesh_raycast_allocator;
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
  VkrMetricEventProducer ibl_convolution_metrics;
  bool8_t texture_pressure_active;
  /* Completed publications that can change an admitted shadow caster
     (VkrWorldPassPayload.caster_publication_generation). Main thread only. */
  uint64_t caster_publication_generation;
  /* Container world ids requested for scene paths (ADR-076); the scene loader
     creates a requested path's scene with that id and picking range. Main
     thread only. */
  struct {
    char path[1024];
    uint16_t world_id;
  } scene_container_requests[8];
  /* Managed asset references to their current artifact paths: scene loads
     publish their `asset_refs` and the editor publishes a rebuilt asset's
     new revision. An entry stays until shutdown, so the table is bounded by
     the distinct references loaded. Guarded by asset_ref_mutex, because
     scene loads publish from loader workers. */
  VkrDMemory asset_ref_memory;
  VkrAllocator asset_ref_allocator;
  VkrMutex asset_ref_mutex;
  VkrHashTable_VkrAssetRefPath asset_refs;
} VkrRenderAssets;

/* CPU triangle memory the mesh ray cache keeps before it evicts. */
#define VKR_RENDER_ASSETS_MESH_RAYCAST_BUDGET (128ull * 1024ull * 1024ull)

/* The mesh ray cache, created on first use; NULL when it cannot be. */
VkrMeshRaycastCache *vkr_render_assets_mesh_raycast(VkrRenderAssets *assets);

/* Whether `value` is a managed asset reference ("asset:<scope>/<id>"). */
bool8_t vkr_asset_ref_is(String8 value);
/* Points `ref` at artifact `path`, replacing an earlier path; sets
 * `*out_changed` (optional) when the path differs from the published one.
 * Any thread; false when `ref` is not a reference or storage runs out. */
bool8_t vkr_render_assets_publish_asset_ref(VkrRenderAssets *assets,
                                            String8 ref, String8 path,
                                            bool8_t *out_changed);
/* Whether a load, listing or rebuild published `ref`. */
bool8_t vkr_render_assets_asset_ref_known(VkrRenderAssets *assets, String8 ref);
/* Writes the current path of `ref` with a terminator into `out`; false when
 * no load or rebuild published it, or `capacity` cannot hold it. */
bool8_t vkr_render_assets_resolve_asset_ref(VkrRenderAssets *assets,
                                            String8 ref, char *out,
                                            uint64_t capacity);

/* On failure, partial ownership remains in assets for shutdown after the caller
 * joins workers and proves GPU idle. The native publisher stays alive
 * throughout. */
bool8_t vkr_render_assets_initialize(
    VkrRenderAssets *assets, const VkrAssetPublisher *publisher,
    const VkrDeviceInformation *device_info, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers,
    const char *bootstrap_font_directory, uint32_t texture_max_load_dimension);

/* Join workers and wait for GPU completion before calling. Scene/UI users must
 * already have released their borrowed assets. Safe after partial
 * initialization. */
void vkr_render_assets_shutdown(VkrRenderAssets *assets);

/* Refresh at a sampled frame boundary or after committed Scene memory relief.
 */
void vkr_render_assets_refresh_texture_residency_budget(
    VkrRenderAssets *assets, const VkrDeviceMemoryStats *device_memory);

/* Publish before sampling memory and applying ready material textures, so a
 * newly charged texture heap cannot be rejected by an older allowance. */
bool8_t
vkr_render_assets_pump_publications(VkrRenderAssets *assets,
                                    VkrResourceSubmissionState submission);
uint32_t vkr_render_assets_ibl_sh_slot(const VkrRenderAssets *assets,
                                       VkrTextureHandle source);

/* Pure pressure/hysteresis policy; false means keep the current budget.
 * With texture heap accounting, out_budget is always the finite capacity
 * allowance; out_pressure_active selects whether it is enforced. */
bool8_t vkr_render_assets_texture_pressure_budget(
    const VkrDeviceMemoryStats *stats, bool8_t pressure_active,
    uint64_t *out_budget, bool8_t *out_pressure_active);

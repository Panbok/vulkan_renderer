#pragma once

#include "core/event.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/loaders/bitmap_font_loader.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/resources/loaders/mtsdf_font_loader.h"
#include "renderer/resources/loaders/system_font_loader.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/systems/vkr_skybox_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_render_graph.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vkr_renderer_impl.h"

typedef struct VkrMetalPacketRenderer VkrMetalPacketRenderer;
typedef struct VkrBindlessVulkanRenderer VkrBindlessVulkanRenderer;

/**
 * @brief Per-frame batching statistics for the world render path.
 *
 * Counts are reset at frame begin and updated by the world view after draw
 * collection. `draws_issued` counts logical indexed commands after CPU
 * instancing; `draw_calls_issued` counts actual direct/indirect API calls.
 */
typedef struct VkrWorldBatchMetrics {
  uint32_t draws_collected;
  uint32_t opaque_draws;
  uint32_t transmission_draws;
  uint32_t transparent_draws;
  uint32_t opaque_batches;
  /** Logical indexed commands represented by direct or indirect submission. */
  uint32_t draws_issued;
  /** Actual vkCmdDrawIndexed/vkCmdDrawIndexedIndirect calls recorded. */
  uint32_t draw_calls_issued;
  uint32_t batches_created;
  uint32_t draws_merged;
  /** Logical commands carried by multi-draw-indirect calls. */
  uint32_t indirect_draws_issued;
  /** Actual multi-draw-indirect calls recorded. */
  uint32_t indirect_calls_issued;
  float32_t avg_batch_size;
  uint32_t max_batch_size;
  uint32_t gpu_candidate_count;
  uint32_t gpu_candidate_capacity;
  uint32_t gpu_candidate_overflow_fallbacks;
  uint32_t gpu_visible_count;
  uint32_t gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t gpu_compaction_overflow_count;
  uint32_t gpu_resolve_invalid_count;
  bool8_t gpu_diagnostics_valid;
  VkrGeometryMegabufferMetrics geometry_megabuffer;
} VkrWorldBatchMetrics;

/**
 * @brief Per-frame shadow pass statistics, indexed by cascade.
 *
 * descriptor_binds_set1 counts alpha-tested material set binds; the opaque
 * shadow pipeline uses no set 1. Draw/batch counts are split by opaque vs
 * alpha-tested pipelines.
 */
typedef struct VkrShadowMetrics {
  uint32_t shadow_draw_calls_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_draw_calls_alpha[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_descriptor_binds_set1[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_batches_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_batches_alpha[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_indirect_draws_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_indirect_calls_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
} VkrShadowMetrics;

/**
 * @brief Aggregate per-frame renderer metrics.
 *
 * These values are reset at frame begin and consumed by UI/debug tooling.
 */
typedef struct VkrRendererFrameMetrics {
  VkrWorldBatchMetrics world;
  VkrShadowMetrics shadow;
  uint64_t backend_present_ns;
  bool8_t backend_present_valid;
} VkrRendererFrameMetrics;

struct s_RendererFrontend {
  Arena *arena;
  VkrAllocator allocator;
  Arena *scratch_arena;
  VkrAllocator scratch_allocator;
  // VkrDMemory dmemory;
  // VkrAllocator dmemory_allocator;

  VkrWindow *window;
  VkrPresentTargetConfig present_target;
  EventManager *event_manager;
  VkrRendererBackendType backend_type;
  VkrRendererImpl impl;
  VkrMetalPacketRenderer *metal_renderer;
  VkrBindlessVulkanRenderer *bindless_vulkan_renderer;
  VkrAssetPublisher asset_publisher;
  VkrRendererImplSubmitResult timing_result;
  uint64_t timing_last_completed_submit_value;
  bool8_t timing_completed_ready;
  bool8_t supports_multi_draw_indirect;
  bool8_t supports_draw_indirect_first_instance;
  VkrRendererBootMetrics boot_metrics;
  VkrSubsystemPlan subsystem_plan;
  VkrMetricEventProducer hdr_decode_metrics;
  VkrMetricEventProducer ibl_conversion_metrics;
  VkrMetricEventProducer ibl_convolution_metrics;

  // High-level renderer subsystems and state (now accessible)
  VkrGeometrySystem geometry_system;
  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;
  VkrDMemory render_graph_dmemory;
  VkrAllocator render_graph_allocator;
  VkrFontSystem font_system;
  VkrGizmoSystem gizmo_system;
  VkrLightingSystem lighting_system;
  VkrShadowSystem shadow_system;
  VkrEditorViewportResources editor_viewport;
  VkrWorldResources world_resources;
  VkrUiSystem ui_system;
  VkrSkyboxSystem skybox_system;

  // Active scene for lighting and other ECS-driven systems
  VkrScene *active_scene;

  // Camera moved into frontend
  VkrCameraSystem camera_system;
  VkrCameraHandle active_camera;
  VkrCameraController camera_controller;

  // Meshes
  VkrMeshManager mesh_manager;
  VkrMeshLoaderContext mesh_loader;
  VkrArenaPool mesh_arena_pool;
  VkrDMemory scene_async_memory;
  VkrAllocator scene_async_allocator;
  VkrMutex scene_async_mutex;

  // Bitmap fonts
  VkrBitmapFontLoaderContext bitmap_font_loader;
  VkrArenaPool bitmap_font_arena_pool;

  // System fonts
  VkrSystemFontLoaderContext system_font_loader;
  VkrArenaPool system_font_arena_pool;

  // MTSDF fonts
  VkrMtsdfFontLoaderContext mtsdf_font_loader;
  VkrArenaPool mtsdf_font_arena_pool;

  // Picking system
  VkrPickingContext picking;

  // Cached global material state for both world and UI
  VkrGlobalMaterialState globals;

  // Per-frame render statistics for UI/debug use.
  VkrRendererFrameMetrics frame_metrics;

  // Debug visualization mode for CSM sampling in the world shader:
  // 0=off, 1=cascades, 2=shadow factor, 3=shadow map depth.
  uint32_t shadow_debug_mode;

  // Window size tracking and thread safety for resize events
  uint32_t last_window_width;
  uint32_t last_window_height;
  VkrMutex rf_mutex;

  bool32_t frame_active;
  uint64_t frame_number;

  uint64_t target_frame_rate;

  VkrAtomicUint64 pending_resize_mailbox;
};

typedef struct s_RendererFrontend RendererFrontend;

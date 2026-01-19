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
#include "renderer/systems/vkr_font_system.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_view_system.h"
#include "renderer/vkr_indirect_draw.h"
#include "renderer/vkr_instance_buffer.h"
#include "renderer/vkr_renderer.h"

/**
 * @brief Per-frame batching statistics for the world render path.
 *
 * Counts are reset at frame begin and updated by the world view after draw
 * collection. draws_issued counts draw calls actually submitted (opaque
 * batches + transparent draws).
 */
typedef struct VkrWorldBatchMetrics {
  uint32_t draws_collected;
  uint32_t opaque_draws;
  uint32_t transparent_draws;
  uint32_t opaque_batches;
  uint32_t draws_issued;
  uint32_t batches_created;
  uint32_t draws_merged;
  uint32_t indirect_draws_issued;
  float32_t avg_batch_size;
  uint32_t max_batch_size;
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
} VkrShadowMetrics;

/**
 * @brief Aggregate per-frame renderer metrics.
 *
 * These values are reset at frame begin and consumed by UI/debug tooling.
 */
typedef struct VkrRendererFrameMetrics {
  VkrWorldBatchMetrics world;
  VkrShadowMetrics shadow;
} VkrRendererFrameMetrics;

struct s_RendererFrontend {
  Arena *arena;
  VkrAllocator allocator;
  Arena *scratch_arena;
  VkrAllocator scratch_allocator;
  // VkrDMemory dmemory;
  // VkrAllocator dmemory_allocator;

  VkrWindow *window;
  EventManager *event_manager;
  void *backend_state;
  VkrRendererBackendType backend_type;
  VkrRendererBackendInterface backend;
  bool8_t supports_multi_draw_indirect;
  bool8_t supports_draw_indirect_first_instance;

  // High-level renderer subsystems and state (now accessible)
  VkrPipelineRegistry pipeline_registry;
  VkrShaderSystem shader_system;
  VkrGeometrySystem geometry_system;
  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;
  VkrViewSystem view_system;
  VkrFontSystem font_system;
  VkrGizmoSystem gizmo_system;
  VkrLightingSystem lighting_system;

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

  // Instance data streaming
  VkrInstanceBufferPool instance_buffer_pool;
  VkrIndirectDrawSystem indirect_draw_system;

  // Bitmap fonts
  VkrBitmapFontLoaderContext bitmap_font_loader;
  VkrArenaPool bitmap_font_arena_pool;

  // System fonts
  VkrSystemFontLoaderContext system_font_loader;
  VkrArenaPool system_font_arena_pool;

  // MTSDF fonts
  VkrMtsdfFontLoaderContext mtsdf_font_loader;
  VkrArenaPool mtsdf_font_arena_pool;

  VkrLayerHandle world_layer;
  VkrLayerHandle skybox_layer;
  VkrLayerHandle ui_layer;
  VkrLayerHandle editor_layer;
  VkrLayerHandle shadow_layer;
  VkrTextureHandle *offscreen_color_handles;
  uint32_t offscreen_color_handle_count;

  // Picking system
  VkrPickingContext picking;

  // Per-draw state
  VkrShaderStateObject draw_state;

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

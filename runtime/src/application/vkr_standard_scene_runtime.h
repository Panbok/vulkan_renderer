#pragma once

#include "application/vkr_application_host.h"
#include "application/vkr_application_metrics.h"
#include "core/vkr_subsystem_plan.h"

#include "core/logger.h"
#include "core/ui/vkr_ui_dock.h"
#include "core/vkr_atomic.h"
#include "core/vkr_job_system.h"
#include "core/vkr_metrics.h"
#include "defines.h"
#include "math/vec.h"
#include "math/vkr_frustum.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_frame.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/systems/vkr_skybox_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "vkr_frame_input.h"
#include "vkr_renderer.h"
#include "vkr_renderer_internal.h"
#include "vkr_renderer_metrics.h"
#include "vkr_visibility.h"

/**
 * @brief Editor viewport state owned by the application.
 */
typedef struct VkrStandardSceneRuntimeEditorViewport {
  bool8_t enabled;
  bool8_t scene_only;
  bool8_t simulation_running;
  bool8_t scene_rendering_stopped;
  VkrRendererError scene_error;
  float64_t simulation_time;
  VkrViewportFitMode fit_mode;
  float32_t render_scale;
  uint32_t rendered_width;
  uint32_t rendered_height;
  uint32_t output_width;
  uint32_t output_height;
  uint32_t last_target_width;
  uint32_t last_target_height;
  VkrUiDockTree dock;
  VkrUiDockInputCapture dock_capture;
} VkrStandardSceneRuntimeEditorViewport;

#define VKR_STANDARD_SCENE_RUNTIME_MAX_PENDING_TEXT_UPDATES 32

typedef struct VkrStandardSceneRuntimeTextUpdate {
  uint32_t text_id;
  String8 content;
  bool8_t has_transform;
  VkrTransform transform;
} VkrStandardSceneRuntimeTextUpdate;

/**
 * @brief Configuration settings for creating an application instance.
 * This structure is passed to `vkr_standard_scene_runtime_create()` to specify
 * initial properties of the application, such as window characteristics and
 * resource sizes.
 */
typedef struct VkrStandardSceneRuntimeConfig {
  const char *title;          /**< The title of the application window. */
  int32_t x;                  /**< The initial x-coordinate of the window. */
  int32_t y;                  /**< The initial y-coordinate of the window. */
  uint32_t width;             /**< The initial width of the window. */
  uint32_t height;            /**< The initial height of the window. */
  uint64_t target_frame_rate; /**< The desired target frame rate for the
                                 application loop (e.g., 60 FPS). */

  uint64_t app_arena_size; /**< The size of the main application arena, used for
                              general game/application allocations. */
  VkrDeviceRequirements device_requirements; /**< The device requirements for
                                             the application. */
  VkrMetricsConfig metrics_config; /**< Runtime instrumentation policy. */
  /** Harness controls. Zero values preserve the interactive application. */
  float64_t fixed_delta_seconds;
  bool8_t disable_camera_controller;
  bool8_t window_hidden;
  bool8_t disable_skybox;
  /** Coarse renderer selection; zero-initialized preserves Vulkan. */
  VkrRendererBackendType renderer_backend;
  VkrPresentTargetConfig present_target;
  VkrPresentMode requested_present_mode;
  VkrDisplayOutputMode display_output_mode;
  /** Internal Scene resolution relative to its presentation extent. Zero
   * selects 1.0. Metal currently supports values in (0, 1]. */
  float32_t render_scale;
  /** Cold reconstruction path; zero preserves spatial sampling. */
  VkrUpscaleMode upscale_mode;
  /** Completion-driven policy; valid only for MetalFX temporal mode. */
  VkrDynamicResolutionConfig dynamic_resolution;
  bool8_t capture_enabled;
  const char *bootstrap_font_directory;
  uint32_t capture_ring_capacity;
  uint64_t capture_max_batch_bytes;
  /** Boot intent only: `profile`, `requested_mask`, and `excluded_mask` are
      read and the closure is recomputed. Zero-initialized means full boot. */
  VkrSubsystemPlan subsystem_plan;
} VkrStandardSceneRuntimeConfig;

typedef struct VkrStandardSceneRuntimeMetricIds {
  // Catalog v1 boundaries: wall spans one active loop iteration including
  // limiter sleep; work spans that iteration through draw completion; update,
  // prepare, submit, and sleep wrap only their correspondingly named calls.
  // Backend present is separately nested inside submit by the renderer adapter.
  VkrMetricId frame_wall;
  VkrMetricId frame_work;
  VkrMetricId update;
  VkrMetricId render_prepare;
  VkrMetricId render_submit;
  VkrMetricId limiter_sleep;
  // Nested inside update and frame_work respectively. Both are frontend work
  // that runs before any backend sees the packet, so they belong here rather
  // than in the renderer catalog.
  VkrMetricId shadow_update;
  VkrMetricId world_payload_build;
} VkrStandardSceneRuntimeMetricIds;

struct VkrStandardSceneRuntime;

typedef struct VkrStandardSceneRuntimeCallbacks {
  void *state;
  void (*update)(void *state, struct VkrStandardSceneRuntime *runtime,
                 float64_t delta_seconds);
  /** Event data is borrowed until this callback returns. */
  bool8_t (*event)(void *state, Event *event);
} VkrStandardSceneRuntimeCallbacks;

/**
 * @brief Main structure representing the application.
 * Encapsulates all core components, state, and resources needed for the
 * application to run.
 */
/* Do not move after creation. Config is borrowed through shutdown; assets,
 * frame storage and the host are owned here and released in dependency order.
 */
typedef struct VkrStandardSceneRuntime {
  Arena *app_arena; /**< Main memory arena for general application use (e.g.,
                       game entities, state). */
  Arena *log_arena; /**< Memory arena dedicated to the logging system. */
  VkrAllocator app_allocator; /**< Allocator backed by `app_arena` for thread
                                 primitives and other systems. */
  Arena *metrics_arena;
  VkrAllocator metrics_allocator;
  VkrMetrics *metrics;
  VkrStandardSceneRuntimeMetricIds metric_ids;
  VkrRendererMetrics renderer_metrics;
  VkrApplicationHost host;
  VkrStandardSceneRuntimeConfig *config; /**< Pointer to the configuration used
                                to create this application instance. */
  VkrRenderer renderer;
  VkrAtomicUint64 pending_resize_mailbox;
  uint64_t last_target_generation;
  uint64_t texture_memory_sample_frame;
  VkrRenderAssets assets;
  Arena *frame_arena;
  VkrAllocator frame_allocator;
  VkrGizmoSystem gizmo_system;
  VkrLightingSystem lighting_system;
  VkrShadowSystem shadow_system;
  VkrUiSystem ui_system;
  /** Resolve UI anchors against the camera and viewport used by this packet. */
  void (*project_ui)(struct VkrStandardSceneRuntime *,
                     const VkrViewportMapping *);
  VkrSkyboxSystem skybox_system;
  VkrScene *active_scene;
  uint64_t scene_generation;
  VkrCameraSystem camera_system;
  VkrCameraHandle active_camera;
  VkrCameraController camera_controller;
  VkrPickingContext picking;
  VkrFrameGlobals globals;
  VkrSubsystemPlan subsystem_plan;
  uint32_t shadow_debug_mode;
  bool8_t transmission_depth_diagnostic_enabled;
  uint32_t ibl_probe_limit;
  /* Optional user quality gates; zero preserves scene-authored rendering. */
  bool8_t disable_directional_shadows;
  bool8_t disable_local_shadows;
  bool8_t disable_soft_shadows;
  bool8_t disable_fog;
  bool8_t disable_volumetric_fog;
  bool8_t disable_subsurface_scattering;

  /** Last frame's frustum-culling counters, produced by payload construction.
   */
  VkrVisibilityStats visibility_stats;

  VkrJobSystem job_system; /**< Engine-wide job system. */

  VkrStandardSceneRuntimeTextUpdate
      world_text_updates[VKR_STANDARD_SCENE_RUNTIME_MAX_PENDING_TEXT_UPDATES];
  uint32_t world_text_update_count;

  /* Bounded Scene memory recovery applies to app and editor presentation. */
  float32_t scene_output_scale;
  uint64_t scene_memory_relief_generation;
  VkrStandardSceneRuntimeEditorViewport editor_viewport;
  VkrUiInputCapture ui_capture;
  const VkrCaptureBatchRequest *capture_request;
  VkrRendererError last_renderer_error;
  VkrStandardSceneRuntimeCallbacks callbacks;
  /* GPU timing policy is owned by `metrics->config`. */
} VkrStandardSceneRuntime;

/**
 * @brief True when the application owns a window.
 *
 * An offscreen application creates no window, so it has no surface, input
 * state, or gamepads to poll, update, or destroy.
 */
vkr_internal INLINE bool8_t vkr_standard_scene_runtime_is_windowed(
    const VkrStandardSceneRuntime *application) {
  return vkr_application_host_is_windowed(&application->host);
}

bool8_t vkr_standard_scene_runtime_editor_scene_rendering_stopped(
    const VkrStandardSceneRuntime *runtime);
bool8_t vkr_standard_scene_runtime_editor_viewport_mapping(
    VkrStandardSceneRuntime *runtime, uint32_t window_width,
    uint32_t window_height, VkrViewportMapping *out_mapping);
void vkr_standard_scene_runtime_set_callbacks(
    VkrStandardSceneRuntime *runtime,
    const VkrStandardSceneRuntimeCallbacks *callbacks);
bool8_t
vkr_standard_scene_runtime_create(VkrStandardSceneRuntime *runtime,
                                  VkrStandardSceneRuntimeConfig *config);
void vkr_standard_scene_runtime_run(VkrStandardSceneRuntime *runtime);
void vkr_standard_scene_runtime_stop(VkrStandardSceneRuntime *runtime);
void vkr_standard_scene_runtime_resume(VkrStandardSceneRuntime *runtime);
void vkr_standard_scene_runtime_close(VkrStandardSceneRuntime *runtime);
void vkr_standard_scene_runtime_shutdown(VkrStandardSceneRuntime *runtime);

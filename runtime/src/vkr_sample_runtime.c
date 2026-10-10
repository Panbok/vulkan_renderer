#include "vkr_sample_runtime.h"
#include "core/vkr_content_codec.h"
#include "core/vkr_json.h"
#include "core/vkr_subsystem_plan.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "vkr_sample_runtime_config.h"

#include "application/vkr_standard_scene_runtime.h"
#include "core/event.h"
#include "core/input.h"
#include "core/logger.h"
#include "core/vkr_clock.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_quat.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/resources/ui/vkr_ui_text.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_model.h"
#include "renderer/systems/vkr_scene_partition.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "script/vkr_script_host.h"
#include "vkr_renderer.h"
#include <math.h>
#include <stdio.h>

#define VKR_FPS_UPDATE_INTERVAL 0.25
#define VKR_MEMORY_UPDATE_INTERVAL 1.0
#define VKR_FPS_DELTA_MIN 0.000001
#define VKR_WORLD_TIME_UPDATE_INTERVAL 0.25
/* Deadline for a headless run that sets no VKR_AUTOCLOSE_SECONDS. */
#define VKR_SAMPLE_HEADLESS_AUTOCLOSE_SECONDS 600.0
/* Milliseconds of cell loads one frame may spend (ADR-086). */
#define VKR_SAMPLE_PARTITION_BUDGET_MS 2.0
/* Cells one partition.load may load at once. */
#define VKR_SAMPLE_PARTITION_REQUEST_MAX 1024u
/* Origin rebase (ADR-086): during Play, a camera this many metres from the
   origin along X or Z moves every container back by whole steps. */
#define VKR_SAMPLE_REBASE_DISTANCE 4096.0f
#define VKR_SAMPLE_REBASE_STEP 1024.0f

static VkrSampleRuntimePreferences
sample_preferences_snapshot(VkrStandardSceneRuntime *application);
static VkrSampleSceneRecall
sample_recall_snapshot(VkrStandardSceneRuntime *application);
static void
sample_editor_state_apply(VkrStandardSceneRuntime *application,
                          const VkrSampleEditorStateRequest *request);

typedef struct FilterModeEntry {
  VkrFilter min_filter;
  VkrFilter mag_filter;
  VkrMipFilter mip_filter;
  bool8_t anisotropy;
  const char *label;
} FilterModeEntry;

vkr_global const FilterModeEntry FILTER_MODES[] = {
    {VKR_FILTER_NEAREST, VKR_FILTER_NEAREST, VKR_MIP_FILTER_NONE, false_v,
     "No filtering (point, base level)"},
    {VKR_FILTER_NEAREST, VKR_FILTER_NEAREST, VKR_MIP_FILTER_NEAREST, false_v,
     "Nearest"},
    {VKR_FILTER_LINEAR, VKR_FILTER_LINEAR, VKR_MIP_FILTER_NEAREST, false_v,
     "Linear"},
    {VKR_FILTER_LINEAR, VKR_FILTER_LINEAR, VKR_MIP_FILTER_NONE, false_v,
     "Bilinear"},
    {VKR_FILTER_LINEAR, VKR_FILTER_LINEAR, VKR_MIP_FILTER_LINEAR, false_v,
     "Trilinear"},
    {VKR_FILTER_LINEAR, VKR_FILTER_LINEAR, VKR_MIP_FILTER_LINEAR, true_v,
     "Anisotropic"},
};

/**
 * @brief Persistent gizmo drag state between pick and drag frames.
 */
/* Most entities a move drag carries beside its own. */
#define GIZMO_COMPANION_MAX 16u
_Static_assert(GIZMO_COMPANION_MAX + 1u <= VKR_SAMPLE_GIZMO_EDIT_MAX,
               "a gizmo report holds the dragged entity and its companions");

typedef struct GizmoDragState {
  bool8_t active;
  bool8_t pending_pick;
  bool8_t pending_select;
  VkrGizmoMode mode;
  VkrGizmoHandle handle;
  VkrEntityId entity;
  Mat4 parent_inverse;
  VkrQuat parent_rotation;
  Vec3 pick_ray_origin;
  Vec3 pick_ray_direction;
  Vec3 axis;
  Vec3 plane_normal;
  Vec3 start_world_position;
  Vec3 start_hit;
  Vec3 start_scale;
  VkrQuat start_rotation;
  float32_t start_radius;
  /* Axis handles: the start hit's signed distance along `axis`. */
  float32_t start_distance;
  /* The UI client's move snap has not run for this drag yet. */
  bool8_t snap_pending;
  /* Entities a move carries along, with their local positions when it
     began and their parents' inverse world matrices. */
  uint32_t companion_count;
  VkrEntityId companions[GIZMO_COMPANION_MAX];
  Vec3 companion_start[GIZMO_COMPANION_MAX];
  Mat4 companion_parent_inverse[GIZMO_COMPANION_MAX];
  bool8_t uses_text_pivot;
  Vec3 text_pivot_local;
  Vec2 pick_position;
  bool8_t released;
  bool8_t release_has_target_coords;
  Vec2 release_position;
} GizmoDragState;

#define VKR_APPLICATION_UI_TEXT_CAPACITY 32768u

typedef struct ApplicationUiText {
  uint8_t data[VKR_APPLICATION_UI_TEXT_CAPACITY];
  uint32_t length;
} ApplicationUiText;

/* A library reload whose copy and open run on a worker (ADR-079). The job
 * borrows this slot of `state` until `done`; the frame thread then commits
 * it. */
typedef struct SampleScriptPending {
  VkrScriptPrepared prepared;
  VkrAtomicBool done;
  bool8_t active;
  /* A later request for the same library, started once this one commits. */
  bool8_t queued;
  VkrSampleScriptLoad next;
} SampleScriptPending;

typedef struct State {
  /* Script modules and the session of the active scene (ADR-079). */
  VkrScriptHost scripts;
  /* The frame's scene edit while script loads apply, so a reload that moves
     a component to new fields converts its bytes too. */
  VkrSceneEditValues *script_frame_edit;
  SampleScriptPending script_pending[VKR_SAMPLE_SCRIPT_LOAD_MAX];
  /* Last presentation the modules published; its HUD replaces the camera
     text while they run. */
  VkrScriptView script_view;
  /* Outcomes of the latest library loads the UI requested. */
  VkrSampleScriptResult script_results[VKR_SAMPLE_SCRIPT_LOAD_MAX];
  uint32_t script_result_count;
  uint64_t script_result_serial;
  Vec3 editor_camera_position;
  float32_t editor_camera_yaw;
  float32_t editor_camera_pitch;
  bool8_t script_camera_active;
  bool8_t gameplay_enabled;
  /* One session start per reset boundary; Reset and scene changes clear it. */
  bool8_t script_start_attempted;
  InputState *input_state;
  bool8_t scene_keyboard_focus;
  /* This frame's Escape released the flying camera or cancelled a gizmo
     pick or edit, so the editor does not act on it too. */
  bool8_t escape_taken;
  VkrSampleViewState view_state;
  /* The editor's last applied Hide or Isolate (VkrSampleHideRequest). */
  VkrSampleHideRequest hidden;
  VkrCamera perspective_camera;
  bool8_t perspective_camera_saved;
  /* Reused application-owned event buffer; outside a session, draining
     keeps the unconsumed sensor queue from exhausting capacity. */
  VkrPhysicsSensorEvent
      physics_sensor_events[VKR_SCENE_PHYSICS_MAX_BODIES *
                            VKR_PHYSICS_SENSOR_EVENTS_PER_BODY];
  uint32_t physics_sensor_event_count;
  /* The latest edit batch or group revert outcome the UI reads. */
  VkrSampleEditBatchResult edit_batch_result;
  /* The gizmo drag recorded since the last build; the next build reads it. */
  VkrSampleGizmoEdit gizmo_recorded;
  /* The latest entity IO request's outcome. */
  VkrSampleIoResult io_result;
  /* One window capture the UI asked for: the renderer request while it
     renders, then the poll result lent to one build until release. */
  VkrCaptureItemRequest capture_item;
  VkrCaptureBatchRequest capture_batch;
  VkrCaptureRequestId capture_serial;
  uint64_t capture_token;
  bool8_t capture_pending;
  VkrSampleCaptureReady capture_ready;
  bool8_t capture_ready_valid;

  Arena *app_arena;
  Arena *event_arena;
  Arena *stats_arena;

  uint32_t filter_mode_index;
  bool8_t anisotropy_supported;
  float64_t max_sampler_anisotropy;

  EventManager *event_manager; // For dispatching events

  /* Runtime-owned caches; UI borrows views through its build callback. */
  char hardware_text[512];
  char system_text[768];
  ApplicationUiText fps_text;
  ApplicationUiText left_text;
  ApplicationUiText memory_text;
  ApplicationUiText metrics_text;
  VkrSampleUiClient ui;
  VkrGraphicsSettingsState graphics;
  VkrGraphicsSettings graphics_started;
  /* Texture limit of the scene being loaded or open, zero for full; the
     Graphics texture resolution also caps material textures. */
  uint32_t scene_texture_extent;
  /* The renderer took the loaded presentation and scale settings. */
  bool8_t graphics_present_live;
  bool8_t graphics_scale_live;
  char graphics_path[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  char graphics_message[160];
  bool8_t graphics_dirty;
  float64_t graphics_changed_at;
  VkrSceneEditState edits;
  /* Every container's undo journal and overlay loads draw from this pool,
     not the UI's retained one, so a long build never starves the UI; each
     journal bounds its own share (VKR_SCENE_EDIT_HISTORY_BYTES). */
  VkrDMemory edit_memory;
  VkrAllocator edit_allocator;
  /* Root physics world every loaded scene shares (ADR-076); created with the
     first attached scene and destroyed after the last one unloads. */
  VkrScenePhysicsSet *physics_set;
  /* Root World container (ADR-076): its document, sidecar and journal. */
  VkrSceneHandle world_handle;
  /* The document loads through the resource system like an added scene, so
     models in the World stream instead of blocking the frame; pending until
     it resolves. A missing document starts an empty World directly. */
  VkrResourceHandleInfo world_resource;
  bool8_t world_pending;
  /* Opened alone, the World frames the camera once it activates; a reload of
     the same document keeps the view. */
  bool8_t world_frame_camera;
  VkrSceneEditState world_edits;
  char world_path[1024];
  char world_sidecar[1024];
  char world_status[192];
  /* Additive scene containers (ADR-076): slot i owns world id i + 1 and
     picking range i + 1. */
  VkrSceneHandle additive_handles[VKR_SCENE_ADDITIVE_MAX];
  /* Resource loads backing additive slots; pending until they resolve. */
  VkrResourceHandleInfo additive_resources[VKR_SCENE_ADDITIVE_MAX];
  bool8_t additive_pending[VKR_SCENE_ADDITIVE_MAX];
  VkrSceneEditState additive_edits[VKR_SCENE_ADDITIVE_MAX];
  char additive_paths[VKR_SCENE_ADDITIVE_MAX][1024];
  char additive_sidecars[VKR_SCENE_ADDITIVE_MAX][1024];
  String8 scene_path;
  char scene_path_storage[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  char sidecar_path[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  /* Mesh assets of the primary scene the last switch closed, which stay
     loaded so switching back skips loading them (ADR-089), and the set
     before it, kept until the next scene to open is known and, when it is
     that set's scene, until it opens. */
  VkrMeshAssetHold warm_hold;
  char warm_path[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  VkrMeshAssetHold opening_hold;
  char opening_path[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  /* World partition (ADR-086): the journal revision and scene structure the
     primary scene's cells were last tracked at. */
  uint64_t partition_revision;
  uint64_t partition_structure;
  /* The proxies directory's modification time plus one, zero while it is
     missing: a change means a bake may have built proxies for cells that
     had none. */
  uint64_t partition_proxy_stamp;
  char physics_asset_root[1024];
  char scene_status[512];
  bool8_t modal;
  VkrSceneEditValues gizmo_before;
  VkrEntityId gizmo_edit_entity;
  uint64_t gizmo_collider_id;
  VkrEntityId picked_collider;
  bool8_t gizmo_edit_pending;
  VkrClock fps_update_clock;
  VkrClock memory_update_clock;
  float64_t fps_accumulated_time;
  uint32_t fps_frame_count;
  float64_t current_fps;
  float64_t current_frametime;

  uint32_t world_text_id;
  VkrClock world_text_update_clock;

  // Picking demo state
  ApplicationUiText picked_object_text;
  uint32_t last_picked_object_id;
  VkrEntityId selected_entity;
  bool8_t has_selection;

  // Gizmo interaction state
  GizmoDragState gizmo_drag;
  bool8_t gizmo_hover_pending;
  VkrGizmoHandle gizmo_hot_handle;
  uint64_t pick_scene_generation;
  VkrEntityId pick_selected_entity;

  bool8_t free_camera_use_gamepad;
  bool8_t free_camera_held;
  /* A right press in the Scene that may end as a click: where and when it
     began and how far the pointer moved while it flew the camera. */
  bool8_t context_armed;
  int32_t context_press_x;
  int32_t context_press_y;
  float64_t context_press_time;
  int32_t context_motion;
  /* The click ended, or the UI asked; its pick runs when the picker is
     free, for this purpose. */
  bool8_t context_click_pending;
  /* A grid fit waits for its GPU pick: the camera's inverse view-projection
     and the pixel's NDC at the request turn the depth into a point. */
  bool8_t grid_fit_pending;
  float64_t grid_fit_started;
  Mat4 grid_fit_inverse_view_projection;
  Vec2 grid_fit_ndc;
  char grid_status[96];
  VkrSamplePickPurpose context_purpose;
  /* The pending pick answers a context click, for this purpose. */
  bool8_t context_pick;
  VkrSamplePickPurpose context_pick_purpose;
  /* Published to the UI once: open the object menu for this entity, or the
     creation menu when none, at this window pixel. */
  bool8_t context_ready;
  VkrEntityId context_entity;
  Vec2 context_position_px;
  VkrSamplePickPurpose context_ready_purpose;

  // Scene system demo
  VkrResourceHandleInfo scene_resource;
  bool8_t scene_load_terminal_logged;
  bool8_t scene_load_stats_baseline_valid;
  VkrAllocatorStatistics scene_load_stats_baseline;
  bool8_t scene_memory_verbose;
  bool8_t scene_load_timer_active;
  float64_t scene_load_start_time_seconds;

  // Optional automation-only runtime cap used for non-interactive verification.
  bool8_t auto_close_enabled;
  float64_t auto_close_after_seconds;
  /** Periodic GPU/allocator telemetry dump; see VKR_METRICS_INTERVAL_SECONDS.
   */
  bool8_t metrics_dump_enabled;
  float64_t metrics_dump_interval_seconds;
  VkrClock metrics_dump_clock;
  uint32_t metrics_dump_index;
  bool8_t auto_close_requested;

  // Upload wait telemetry for async streaming validation runs.
  bool8_t assert_no_upload_waits;
  bool8_t upload_wait_violation_seen;
  uint64_t upload_wait_fence_total;
  uint64_t upload_wait_queue_idle_total;
  uint64_t upload_wait_device_idle_total;
  uint64_t upload_wait_last_publication_serial;
  bool8_t upload_wait_evidence_incomplete;

  // HUD frame-time window, averaged from published metrics rather than from
  // a second wall clock.
  uint64_t hud_last_publication_serial;
  float64_t hud_frametime_sum;
  uint64_t hud_frame_samples;

  // Runtime IBL validation controls.
  uint32_t ibl_validation_mode;
  float32_t ibl_validation_scalar;
  VkrScene *ibl_validation_scene;
  bool8_t ibl_validation_defaults_captured;
  bool8_t ibl_validation_base_enabled;
  float32_t ibl_validation_base_intensity;
  float32_t ibl_validation_base_diffuse_intensity;
  float32_t ibl_validation_base_specular_intensity;
} State;

vkr_global State *state = NULL;

static uint32_t sample_additive_slot(VkrEntityId entity);
static void sample_scripts_stop(VkrStandardSceneRuntime *application);
static void sample_partition_open(VkrScene *scene);
static void sample_partition_session(VkrStandardSceneRuntime *application,
                                     bool8_t begin);
static bool8_t sample_origin_rebased(VkrStandardSceneRuntime *application);
static void sample_script_request(VkrStandardSceneRuntime *application,
                                  const VkrSampleScriptRequest *request,
                                  VkrSceneEditValues *frame_edit);
static void sample_physics_attach(VkrStandardSceneRuntime *application,
                                  VkrScene *scene, bool8_t driver);
vkr_internal void sample_additive_poll(VkrStandardSceneRuntime *application);
vkr_internal void sample_world_update(VkrStandardSceneRuntime *application,
                                      bool8_t advance);
static bool8_t sample_additive_dirty(uint32_t slot);
vkr_internal void sample_additive_remove(VkrStandardSceneRuntime *application,
                                         uint32_t slot);

static void sample_graphics_apply_live(VkrStandardSceneRuntime *application,
                                       const VkrGraphicsSettings *settings) {
  /* Quality gates only; the scene's post_process component owns grading and
     effect strengths (ADR-076). */
  VkrFrameGlobals *globals = &application->globals;
  globals->bloom_enabled = settings->bloom;
  globals->gtao_enabled = settings->ambient_occlusion;
  globals->ssr_enabled = settings->screen_space_reflections;
  globals->ssgi_enabled = settings->screen_space_gi;
  globals->dof_enabled = settings->depth_of_field;
  globals->motion_blur_enabled = settings->motion_blur;
  application->disable_directional_shadows = settings->shadow_quality == 0;
  application->disable_local_shadows =
      settings->shadow_quality == 0 || !settings->local_shadows;
  application->disable_soft_shadows = !settings->soft_shadows;
  application->disable_fog = !settings->fog;
  application->disable_volumetric_fog =
      !settings->fog || !settings->volumetric_fog;
  application->disable_subsurface_scattering = !settings->subsurface_scattering;
  application->ibl_probe_limit = settings->reflection_probes ? UINT32_MAX : 0;
  application->host.window.input_state.invert_look_y = settings->invert_mouse_y;
  /* Ultra's extra local-shadow filtering was measured affordable only on the
   * Vulkan desktop host (ADR-019); Metal keeps High's budget. */
  application->shadow_config =
      settings->shadow_quality == 1 ? VKR_SHADOW_CONFIG_BALANCED
      : settings->shadow_quality == 3 && application->renderer.backend_type ==
                                             VKR_RENDERER_BACKEND_TYPE_VULKAN
          ? vkr_shadow_config_ultra()
          : VKR_SHADOW_CONFIG_HIGH;
  /* Contact shadows follow their own setting on both backends, so Metal can
     enable them under High's local-shadow budget. */
  application->shadow_config.local_shadow_contact = settings->contact_shadows;
  /* Soft shadows also covers local lights' authored source radii. */
  application->shadow_config.local_shadow_soft = settings->soft_shadows;
  application->shadow_config.far_cascade_evsm = settings->filtered_far_shadows;
  const bool8_t temporal =
      application->renderer.upscale_mode != VKR_UPSCALE_MODE_SPATIAL ||
      settings->anti_aliasing;
  vkr_renderer_set_temporal_enabled(&application->renderer, temporal);
  application->host.config.target_frame_rate = settings->frame_limit;
}

/* Display settings the renderer switches between frames: vertical sync,
   screen percentage and dynamic resolution, whose percentage caps the
   controller. Applied ones become the started values, so they need no
   restart; the first frame applies the loaded settings. */
static void sample_graphics_apply_display(VkrStandardSceneRuntime *application,
                                          const VkrGraphicsSettings *settings) {
  VkrRenderer *renderer = &application->renderer;
  if (!state->graphics_present_live ||
      settings->vsync != state->graphics_started.vsync) {
    if (vkr_renderer_set_present_mode(
            renderer, settings->vsync ? VKR_PRESENT_MODE_FIFO
                                      : VKR_PRESENT_MODE_IMMEDIATE) ==
        VKR_RENDERER_ERROR_NONE) {
      state->graphics_started.vsync = settings->vsync;
    }
    state->graphics_present_live = true_v;
  }
  /* The window was created at the started density; a change resizes the
     drawable, and the swapchain follows the resize. */
  if (settings->high_dpi != state->graphics_started.high_dpi &&
      vkr_application_host_is_windowed(&application->host) &&
      vkr_window_set_high_dpi(&application->host.window, settings->high_dpi)) {
    state->graphics_started.high_dpi = settings->high_dpi;
  }
  float32_t live_min = 1.0f;
  float32_t live_max = 1.0f;
  vkr_renderer_render_scale_range(renderer, &live_min, &live_max);
  const bool8_t dynamic = settings->dynamic_resolution &&
                          state->graphics.dynamic_resolution_available;
  if ((!state->graphics_scale_live ||
       settings->render_scale != state->graphics_started.render_scale ||
       settings->dynamic_resolution !=
           state->graphics_started.dynamic_resolution) &&
      settings->temporal_upscaling ==
          state->graphics_started.temporal_upscaling &&
      settings->render_scale >= live_min - 1e-4f &&
      settings->render_scale <= live_max + 1e-4f &&
      (!dynamic || vkr_renderer_dynamic_resolution_switchable(renderer)) &&
      vkr_renderer_set_render_scale(renderer, settings->render_scale,
                                    dynamic) == VKR_RENDERER_ERROR_NONE) {
    state->graphics_started.render_scale = settings->render_scale;
    state->graphics_started.dynamic_resolution = settings->dynamic_resolution;
    state->graphics_scale_live = true_v;
  }
}

static void sample_graphics_request(VkrStandardSceneRuntime *application,
                                    const VkrGraphicsSettingsRequest *request) {
  if (!request->apply && !request->reset_defaults)
    return;
  VkrGraphicsSettings settings =
      request->reset_defaults
          ? vkr_graphics_settings_defaults(application->renderer.backend_type)
          : request->settings;
  if (!vkr_graphics_settings_valid(&settings)) {
    snprintf(state->graphics_message, sizeof(state->graphics_message),
             "These settings could not be applied.");
    return;
  }
  const VkrGraphicsSettings old = state->graphics.settings;
  const bool8_t lighting_changed =
      old.anti_aliasing != settings.anti_aliasing ||
      old.shadow_quality != settings.shadow_quality ||
      old.soft_shadows != settings.soft_shadows ||
      old.filtered_far_shadows != settings.filtered_far_shadows ||
      old.local_shadows != settings.local_shadows ||
      old.contact_shadows != settings.contact_shadows ||
      old.ambient_occlusion != settings.ambient_occlusion ||
      old.screen_space_reflections != settings.screen_space_reflections ||
      old.screen_space_gi != settings.screen_space_gi ||
      old.reflection_probes != settings.reflection_probes ||
      old.subsurface_scattering != settings.subsurface_scattering ||
      old.fog != settings.fog || old.volumetric_fog != settings.volumetric_fog;
  state->graphics.settings = settings;
  sample_graphics_apply_display(application, &settings);
  state->graphics.restart_required = vkr_graphics_settings_restart_required(
      &settings, &state->graphics_started);
  state->graphics_message[0] = '\0';
  state->graphics_dirty = true_v;
  state->graphics_changed_at = vkr_platform_get_absolute_time();
  sample_graphics_apply_live(application, &settings);
  if (lighting_changed) {
    vkr_standard_scene_runtime_invalidate_shadow_fit(application);
    vkr_renderer_invalidate_temporal_history(&application->renderer);
  }
}

static void sample_graphics_save(void) {
  if (!state->graphics_dirty)
    return;
  if (!vkr_graphics_settings_save(state->graphics_path,
                                  &state->graphics.settings)) {
    snprintf(state->graphics_message, sizeof(state->graphics_message),
             "Settings are applied, but could not be saved.");
    log_warn("Unable to save Graphics settings to %s", state->graphics_path);
    return;
  }
  state->graphics_dirty = false_v;
}

vkr_internal bool8_t vkr_standard_scene_runtime_metric_duration_seconds(
    const VkrMetricsFrame *frame, VkrMetricId id, float64_t *out_value) {
  uint64_t mean_ns = 0;
  if (!out_value ||
      !vkr_metrics_frame_read_duration_mean_ns(frame, id, &mean_ns)) {
    return false_v;
  }
  *out_value = (float64_t)mean_ns / 1000000000.0;
  return true_v;
}

/**
 * @brief Folds the newest published frame time into the HUD's display window.
 *
 * The HUD reports an average over its refresh interval, so it accumulates
 * published per-frame values rather than displaying whichever single frame
 * happened to be current when the interval elapsed. Gating on the publication
 * serial keeps a repeated or dropped publication from being counted twice.
 */
vkr_internal void vkr_standard_scene_runtime_accumulate_frame_time(
    VkrStandardSceneRuntime *application) {
  VkrMetricsSnapshotView snapshot = {0};
  if (!vkr_metrics_snapshot_acquire(application->metrics, &snapshot)) {
    return;
  }
  float64_t frame_seconds = 0.0;
  if (snapshot.publication_serial != state->hud_last_publication_serial &&
      vkr_standard_scene_runtime_metric_duration_seconds(
          snapshot.frame, application->metric_ids.frame_wall, &frame_seconds)) {
    state->hud_last_publication_serial = snapshot.publication_serial;
    state->hud_frametime_sum += frame_seconds;
    state->hud_frame_samples++;
  }
  vkr_metrics_snapshot_release(application->metrics, &snapshot);
}

vkr_internal const char *
vkr_standard_scene_runtime_ibl_validation_mode_label(uint32_t mode) {
  switch (mode) {
  case 1u:
    return "IBL off";
  case 2u:
    return "IBL diffuse only";
  case 3u:
    return "IBL specular only";
  case 0u:
  default:
    return "Scene IBL";
  }
}

vkr_internal void vkr_standard_scene_runtime_update_ibl_validation_controls(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  VkrScene *scene = application->active_scene;
  if (!scene) {
    state->ibl_validation_scene = NULL;
    state->ibl_validation_defaults_captured = false_v;
    return;
  }

  /* F8 to F10 override the sky light only while a mode or scale is chosen;
     otherwise the scene's own values, edits and the enable a later
     atmosphere bake makes included, stand. Ending an override restores the
     values it replaced. F9 and F10 step by 0.1, so 1.0 compares loosely. */
  const bool8_t override = state->ibl_validation_mode != 0u ||
                           fabsf(state->ibl_validation_scalar - 1.0f) > 1e-3f;
  if (!override) {
    if (state->ibl_validation_defaults_captured &&
        state->ibl_validation_scene == scene) {
      scene->environment.enabled = state->ibl_validation_base_enabled;
      scene->environment.intensity = state->ibl_validation_base_intensity;
      scene->environment.diffuse_intensity =
          state->ibl_validation_base_diffuse_intensity;
      scene->environment.specular_intensity =
          state->ibl_validation_base_specular_intensity;
    }
    state->ibl_validation_defaults_captured = false_v;
    return;
  }

  if (state->ibl_validation_scene != scene ||
      !state->ibl_validation_defaults_captured) {
    state->ibl_validation_scene = scene;
    state->ibl_validation_defaults_captured = true_v;
    state->ibl_validation_base_enabled = scene->environment.enabled;
    state->ibl_validation_base_intensity = scene->environment.intensity;
    state->ibl_validation_base_diffuse_intensity =
        scene->environment.diffuse_intensity;
    state->ibl_validation_base_specular_intensity =
        scene->environment.specular_intensity;
  }

  scene->environment.enabled = state->ibl_validation_base_enabled;
  switch (state->ibl_validation_mode) {
  case 1u: // Off
    scene->environment.intensity = 0.0f;
    scene->environment.diffuse_intensity = 0.0f;
    scene->environment.specular_intensity = 0.0f;
    break;
  case 2u: // Diffuse only
    scene->environment.intensity =
        state->ibl_validation_base_intensity * state->ibl_validation_scalar;
    scene->environment.diffuse_intensity =
        state->ibl_validation_base_diffuse_intensity *
        state->ibl_validation_scalar;
    scene->environment.specular_intensity = 0.0f;
    break;
  case 3u: // Specular only
    scene->environment.intensity =
        state->ibl_validation_base_intensity * state->ibl_validation_scalar;
    scene->environment.diffuse_intensity = 0.0f;
    scene->environment.specular_intensity =
        state->ibl_validation_base_specular_intensity *
        state->ibl_validation_scalar;
    break;
  case 0u: // Scene default
  default:
    scene->environment.intensity =
        state->ibl_validation_base_intensity * state->ibl_validation_scalar;
    scene->environment.diffuse_intensity =
        state->ibl_validation_base_diffuse_intensity *
        state->ibl_validation_scalar;
    scene->environment.specular_intensity =
        state->ibl_validation_base_specular_intensity *
        state->ibl_validation_scalar;
    break;
  }
}

vkr_internal bool8_t vkr_standard_scene_runtime_capture_backend_allocator_stats(
    VkrStandardSceneRuntime *application, VkrAllocatorStatistics *out_stats) {
  if (!application || !out_stats) {
    return false_v;
  }

  VkrAllocator *backend_allocator =
      vkr_renderer_get_backend_allocator(&application->renderer);
  if (!backend_allocator) {
    return false_v;
  }

  *out_stats = vkr_allocator_get_statistics(backend_allocator);
  return true_v;
}

vkr_internal int64_t vkr_standard_scene_runtime_stat_delta(uint64_t current,
                                                           uint64_t baseline) {
  if (current >= baseline) {
    return (int64_t)(current - baseline);
  }
  return -(int64_t)(baseline - current);
}

vkr_internal const char *
vkr_standard_scene_runtime_allocator_tag_name(VkrAllocatorMemoryTag tag) {
  switch (tag) {
  case VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN:
    return "UNKNOWN";
  case VKR_ALLOCATOR_MEMORY_TAG_ARRAY:
    return "ARRAY";
  case VKR_ALLOCATOR_MEMORY_TAG_STRING:
    return "STRING";
  case VKR_ALLOCATOR_MEMORY_TAG_VECTOR:
    return "VECTOR";
  case VKR_ALLOCATOR_MEMORY_TAG_QUEUE:
    return "QUEUE";
  case VKR_ALLOCATOR_MEMORY_TAG_STRUCT:
    return "STRUCT";
  case VKR_ALLOCATOR_MEMORY_TAG_BUFFER:
    return "BUFFER";
  case VKR_ALLOCATOR_MEMORY_TAG_RENDERER:
    return "RENDERER";
  case VKR_ALLOCATOR_MEMORY_TAG_FILE:
    return "FILE";
  case VKR_ALLOCATOR_MEMORY_TAG_TEXTURE:
    return "TEXTURE";
  case VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE:
    return "HASH_TABLE";
  case VKR_ALLOCATOR_MEMORY_TAG_FREELIST:
    return "FREELIST";
  case VKR_ALLOCATOR_MEMORY_TAG_VULKAN:
    return "VULKAN";
  case VKR_ALLOCATOR_MEMORY_TAG_GPU:
    return "GPU";
  default:
    return "UNKNOWN";
  }
}

vkr_internal double vkr_standard_scene_runtime_bytes_to_mb(uint64_t bytes) {
  return (double)bytes / (double)MB(1);
}

vkr_internal double
vkr_standard_scene_runtime_delta_bytes_to_mb(int64_t bytes) {
  return (double)bytes / (double)MB(1);
}
vkr_internal const char *vkr_standard_scene_runtime_gpu_allocation_owner_name(
    VkrGpuAllocationOwner owner) {
  switch (owner) {
  case VKR_GPU_ALLOCATION_OWNER_MESH:
    return "mesh";
  case VKR_GPU_ALLOCATION_OWNER_TEXTURE:
    return "texture";
  case VKR_GPU_ALLOCATION_OWNER_FONT:
    return "font";
  case VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH:
    return "render_graph";
  case VKR_GPU_ALLOCATION_OWNER_SHADER:
    return "shader";
  case VKR_GPU_ALLOCATION_OWNER_INSTANCE:
    return "instance";
  case VKR_GPU_ALLOCATION_OWNER_INDIRECT:
    return "indirect";
  case VKR_GPU_ALLOCATION_OWNER_STAGING:
    return "staging";
  case VKR_GPU_ALLOCATION_OWNER_READBACK:
    return "readback";
  case VKR_GPU_ALLOCATION_OWNER_SWAPCHAIN:
    return "swapchain";
  case VKR_GPU_ALLOCATION_OWNER_UNKNOWN:
  default:
    return "unknown";
  }
}

vkr_internal bool8_t vkr_standard_scene_runtime_memory_text_append(
    char **write, size_t *remaining, const char *text) {
  if (!write || !*write || !remaining || !text)
    return false_v;
  const size_t length = strlen(text);
  if (length >= *remaining)
    return false_v;
  MemCopy(*write, text, length);
  *write += length;
  *remaining -= length;
  **write = '\0';
  return true_v;
}

vkr_internal bool8_t vkr_standard_scene_runtime_memory_text_append_size(
    char **write, size_t *remaining, const char *label, uint64_t bytes) {
  if (!write || !*write || !remaining || !label)
    return false_v;
  const size_t length =
      vkr_allocator_format_size_to_buffer(*write, *remaining, label, bytes);
  if (!length || length >= *remaining)
    return false_v;
  *write += length;
  *remaining -= length;
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_log_backend_allocator_breakdown(
    const char *label, const VkrAllocatorStatistics *stats) {
  if (!label || !stats) {
    return;
  }

  char stats_buffer[2048];
  char *write = stats_buffer;
  size_t remaining = sizeof(stats_buffer);
  for (uint32_t tag = 0; tag < VKR_ALLOCATOR_MEMORY_TAG_MAX && remaining > 1;
       ++tag) {
    size_t line_len = vkr_allocator_format_size_to_buffer(
        write, remaining,
        vkr_standard_scene_runtime_allocator_tag_name(
            (VkrAllocatorMemoryTag)tag),
        stats->tagged_allocs[tag]);
    if (line_len == 0 || line_len >= remaining) {
      break;
    }
    write += line_len;
    remaining -= line_len;
  }
  *write = '\0';

  log_debug("Vulkan backend %s stats:\n%s", label, stats_buffer);
}

/**
 * @brief Logs physical device-memory and logical resource-owner telemetry.
 */
vkr_internal void vkr_standard_scene_runtime_log_device_memory_stats(
    VkrStandardSceneRuntime *application, const char *label) {
  if (!application || !label) {
    return;
  }

  VkrDeviceMemoryStats stats = {0};
  if (!vkr_renderer_get_device_memory_stats(&application->renderer, &stats)) {
    return;
  }

  log_info("GPU_MEM label=%s physical_allocations=live:%llu peak:%llu "
           "total:%llu limit:%llu committed=live:%.3fMB peak:%.3fMB exact=%s",
           label, (unsigned long long)stats.live_allocation_count,
           (unsigned long long)stats.peak_allocation_count,
           (unsigned long long)stats.total_allocation_count,
           (unsigned long long)stats.max_allocation_count,
           vkr_standard_scene_runtime_bytes_to_mb(stats.live_bytes),
           vkr_standard_scene_runtime_bytes_to_mb(stats.peak_bytes),
           stats.live_totals_exact ? "yes" : "no");
  for (uint32_t owner = 0; owner < VKR_GPU_ALLOCATION_OWNER_COUNT; ++owner) {
    const VkrGpuAllocationOwnerTotals *totals = &stats.owners[owner];
    if (!totals->live_allocation_count && !totals->total_allocation_count)
      continue;
    log_info("GPU_MEM   owner=%s allocations=live:%llu peak:%llu total:%llu "
             "bytes=live:%.3fMB peak:%.3fMB total:%.3fMB",
             vkr_standard_scene_runtime_gpu_allocation_owner_name(
                 (VkrGpuAllocationOwner)owner),
             (unsigned long long)totals->live_allocation_count,
             (unsigned long long)totals->peak_allocation_count,
             (unsigned long long)totals->total_allocation_count,
             vkr_standard_scene_runtime_bytes_to_mb(totals->live_bytes),
             vkr_standard_scene_runtime_bytes_to_mb(totals->peak_bytes),
             vkr_standard_scene_runtime_bytes_to_mb(totals->total_bytes));
  }

  for (uint32_t i = 0; i < stats.memory_type_count; ++i) {
    if (stats.live_count_by_type[i] == 0) {
      continue;
    }
    log_info(
        "GPU_MEM   type=%u heap=%u flags=0x%x count=%llu bytes=%.3fMB", i,
        stats.heap_index_by_type[i], stats.property_flags_by_type[i],
        (unsigned long long)stats.live_count_by_type[i],
        vkr_standard_scene_runtime_bytes_to_mb(stats.live_bytes_by_type[i]));
  }

  for (uint32_t i = 0; i < stats.heap_count; ++i) {
    if (stats.heap_usage_valid) {
      log_info(
          "GPU_MEM   heap=%u size=%.3fMB usage=%.3fMB budget=%.3fMB", i,
          vkr_standard_scene_runtime_bytes_to_mb(stats.heap_size_bytes[i]),
          vkr_standard_scene_runtime_bytes_to_mb(stats.heap_usage_bytes[i]),
          vkr_standard_scene_runtime_bytes_to_mb(stats.heap_budget_bytes[i]));
    } else {
      log_info(
          "GPU_MEM   heap=%u size=%.3fMB (usage unavailable)", i,
          vkr_standard_scene_runtime_bytes_to_mb(stats.heap_size_bytes[i]));
    }
  }
}

/**
 * @brief Emits one periodic telemetry sample.
 *
 * Labelled with a monotonically increasing index and the elapsed time so a
 * capture can be read as a series -- allocation counts settling after a scene
 * load look different from allocation counts that keep climbing, and only the
 * series distinguishes them.
 */
vkr_internal void vkr_standard_scene_runtime_dump_periodic_metrics(
    VkrStandardSceneRuntime *application) {
  if (!application || !state || !state->metrics_dump_enabled) {
    return;
  }
  if (!vkr_clock_interval_elapsed(&state->metrics_dump_clock,
                                  state->metrics_dump_interval_seconds)) {
    return;
  }

  char label[64];
  snprintf(label, sizeof(label), "tick%u@%.1fs", state->metrics_dump_index,
           application->host.clock.elapsed);
  state->metrics_dump_index++;

  vkr_standard_scene_runtime_log_device_memory_stats(application, label);

  VkrAllocatorStatistics stats = {0};
  if (vkr_standard_scene_runtime_capture_backend_allocator_stats(application,
                                                                 &stats)) {
    log_info(
        "CPU_ALLOC label=%s total=%.3fMB renderer=%.3fMB array=%.3fMB "
        "string=%.3fMB file=%.3fMB vulkan_state=%.3fMB texture_temp=%.3fMB",
        label, vkr_standard_scene_runtime_bytes_to_mb(stats.total_allocated),
        vkr_standard_scene_runtime_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_RENDERER]),
        vkr_standard_scene_runtime_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]),
        vkr_standard_scene_runtime_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]),
        vkr_standard_scene_runtime_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_FILE]),
        vkr_standard_scene_runtime_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_VULKAN]),
        vkr_standard_scene_runtime_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_TEXTURE]));
  }
}

vkr_internal void vkr_standard_scene_runtime_log_backend_allocator_stats(
    VkrStandardSceneRuntime *application, const char *label,
    const VkrAllocatorStatistics *baseline_stats) {
  if (!application || !label) {
    return;
  }

  VkrAllocatorStatistics stats = {0};
  if (!vkr_standard_scene_runtime_capture_backend_allocator_stats(application,
                                                                  &stats)) {
    return;
  }

  const uint64_t total_bytes = stats.total_allocated;
  const uint64_t renderer_bytes =
      stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_RENDERER];
  const uint64_t vulkan_state_bytes =
      stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_VULKAN];
  const uint64_t texture_temp_bytes =
      stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_TEXTURE];

  if (!baseline_stats) {
    log_debug("SCENE_CPU_ALLOC label=%s total=%.3fMB renderer=%.3fMB "
              "vulkan_state=%.3fMB texture_temp=%.3fMB",
              label, vkr_standard_scene_runtime_bytes_to_mb(total_bytes),
              vkr_standard_scene_runtime_bytes_to_mb(renderer_bytes),
              vkr_standard_scene_runtime_bytes_to_mb(vulkan_state_bytes),
              vkr_standard_scene_runtime_bytes_to_mb(texture_temp_bytes));
    if (state && state->scene_memory_verbose) {
      vkr_standard_scene_runtime_log_backend_allocator_breakdown(label, &stats);
    }
    return;
  }

  const int64_t delta_total = vkr_standard_scene_runtime_stat_delta(
      total_bytes, baseline_stats->total_allocated);
  const int64_t delta_renderer = vkr_standard_scene_runtime_stat_delta(
      renderer_bytes,
      baseline_stats->tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_RENDERER]);
  const int64_t delta_vulkan_state = vkr_standard_scene_runtime_stat_delta(
      vulkan_state_bytes,
      baseline_stats->tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_VULKAN]);
  const int64_t delta_texture_temp = vkr_standard_scene_runtime_stat_delta(
      texture_temp_bytes,
      baseline_stats->tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_TEXTURE]);

  log_debug("SCENE_CPU_ALLOC label=%s total=%.3fMB renderer=%.3fMB "
            "vulkan_state=%.3fMB texture_temp=%.3fMB delta_total=%+.3fMB "
            "delta_renderer=%+.3fMB delta_vulkan_state=%+.3fMB "
            "delta_texture_temp=%+.3fMB",
            label, vkr_standard_scene_runtime_bytes_to_mb(total_bytes),
            vkr_standard_scene_runtime_bytes_to_mb(renderer_bytes),
            vkr_standard_scene_runtime_bytes_to_mb(vulkan_state_bytes),
            vkr_standard_scene_runtime_bytes_to_mb(texture_temp_bytes),
            vkr_standard_scene_runtime_delta_bytes_to_mb(delta_total),
            vkr_standard_scene_runtime_delta_bytes_to_mb(delta_renderer),
            vkr_standard_scene_runtime_delta_bytes_to_mb(delta_vulkan_state),
            vkr_standard_scene_runtime_delta_bytes_to_mb(delta_texture_temp));
  if (state && state->scene_memory_verbose) {
    vkr_standard_scene_runtime_log_backend_allocator_breakdown(label, &stats);
  }
}

vkr_internal float64_t
vkr_standard_scene_runtime_consume_scene_load_elapsed_seconds(
    VkrStandardSceneRuntime *application) {
  if (!application || !state || !state->scene_load_timer_active) {
    return -1.0;
  }

  float64_t elapsed =
      application->host.clock.elapsed - state->scene_load_start_time_seconds;
  if (elapsed < 0.0) {
    elapsed = 0.0;
  }

  state->scene_load_timer_active = false_v;
  return elapsed;
}

vkr_internal void
vkr_standard_scene_runtime_ui_text_set(ApplicationUiText *destination,
                                       String8 content) {
  if (!destination || (!content.str && content.length > 0u)) {
    return;
  }
  const uint32_t length =
      (uint32_t)Min(content.length, VKR_APPLICATION_UI_TEXT_CAPACITY - 1u);
  if (length > 0u)
    MemCopy(destination->data, content.str, length);
  destination->data[length] = 0u;
  destination->length = length;
}

vkr_internal String8
vkr_standard_scene_runtime_ui_text_view(const ApplicationUiText *text) {
  return text ? (String8){.str = (uint8_t *)text->data, .length = text->length}
              : (String8){0};
}

vkr_internal void vkr_standard_scene_runtime_queue_world_text_update(
    VkrStandardSceneRuntime *application, uint32_t text_id, String8 content,
    const VkrTransform *transform) {
  if (!application || text_id == VKR_INVALID_ID) {
    return;
  }

  for (uint32_t i = 0; i < application->world_text_update_count; ++i) {
    VkrStandardSceneRuntimeTextUpdate *slot =
        &application->world_text_updates[i];
    if (slot->text_id == text_id) {
      if (content.length > 0 || content.str) {
        slot->content = content;
      }
      if (transform) {
        slot->transform = *transform;
        slot->has_transform = true_v;
      }
      return;
    }
  }

  if (application->world_text_update_count >=
      VKR_STANDARD_SCENE_RUNTIME_MAX_PENDING_TEXT_UPDATES) {
    log_warn("World text update queue full; dropping text %u", text_id);
    return;
  }

  VkrStandardSceneRuntimeTextUpdate update = {
      .text_id = text_id,
      .content = content,
      .has_transform = false_v,
  };
  if (transform) {
    update.transform = *transform;
    update.has_transform = true_v;
  }

  application->world_text_updates[application->world_text_update_count++] =
      update;
}

/**
 * @brief Viewport mapping info for pointer-driven world interactions.
 */
typedef struct VkrViewportHitInfo {
  // Normalized displayed image coordinates, independent of render scale.
  Vec2 position;
  uint32_t target_x;
  uint32_t target_y;
  uint32_t target_width;
  uint32_t target_height;
  bool8_t has_target_coords;
} VkrViewportHitInfo;

/**
 * @brief Compute viewport mapping info for world picking and gizmo rays.
 */
vkr_internal VkrViewportHitInfo
vkr_standard_scene_runtime_get_viewport_hit_info(
    VkrStandardSceneRuntime *application, int32_t mouse_x, int32_t mouse_y) {
  VkrViewportHitInfo info = {0};
  if (!application || !state) {
    return info;
  }

  if (application->editor_viewport.enabled &&
      vkr_subsystem_plan_includes(&application->subsystem_plan,
                                  VKR_RENDERER_SUBSYSTEM_EDITOR)) {
    /* The UI's target, as the UI frame's mapping uses: a headless editor has
       no window, so its window size is zero. */
    VkrViewportMapping mapping = {0};
    if (vkr_standard_scene_runtime_editor_viewport_mapping(
            application, application->ui_system.target_width,
            application->ui_system.target_height, &mapping)) {
      info.target_width = mapping.target_width;
      info.target_height = mapping.target_height;
      if (vkr_viewport_mapping_window_to_target_pixel(
              &mapping, mouse_x, mouse_y, &info.target_x, &info.target_y)) {
        info.position = (Vec2){
            ((float32_t)mouse_x + 0.5f - mapping.image_rect_px.x) /
                mapping.image_rect_px.z,
            ((float32_t)mouse_y + 0.5f - mapping.image_rect_px.y) /
                mapping.image_rect_px.w,
        };
        info.has_target_coords = true_v;
      }
    }
  } else {
    VkrWindowPixelSize window_size =
        vkr_window_get_pixel_size(&application->host.window);
    info.target_width = window_size.width;
    info.target_height = window_size.height;
    if (mouse_x >= 0 && mouse_y >= 0 && (uint32_t)mouse_x < window_size.width &&
        (uint32_t)mouse_y < window_size.height) {
      info.position = (Vec2){
          ((float32_t)mouse_x + 0.5f) / window_size.width,
          ((float32_t)mouse_y + 0.5f) / window_size.height,
      };
      info.target_x = (uint32_t)mouse_x;
      info.target_y = (uint32_t)mouse_y;
      info.has_target_coords = true_v;
    }
  }

  return info;
}

/**
 * @brief Build a world-space ray from normalized displayed image coordinates.
 */
vkr_internal bool8_t vkr_standard_scene_runtime_build_view_ray(
    VkrCamera *camera, Vec2 position, Vec3 *out_origin, Vec3 *out_dir) {
  if (!camera || !out_origin || !out_dir) {
    return false_v;
  }

  vkr_camera_system_update(camera);

  Mat4 view = vkr_camera_system_get_view_matrix(camera);
  Mat4 projection = vkr_camera_system_get_projection_matrix(camera);
  Mat4 inv_vp = mat4_inverse(mat4_mul(projection, view));

  const float32_t ndc_x = position.x * 2.0f - 1.0f;
  const float32_t ndc_y = position.y * 2.0f - 1.0f;

  Vec4 near_clip = vec4_new(ndc_x, ndc_y, 0.0f, 1.0f);
  Vec4 far_clip = vec4_new(ndc_x, ndc_y, 1.0f, 1.0f);

  Vec4 near_world = mat4_mul_vec4(inv_vp, near_clip);
  Vec4 far_world = mat4_mul_vec4(inv_vp, far_clip);

  if (vkr_abs_f32(near_world.w) < VKR_FLOAT_EPSILON ||
      vkr_abs_f32(far_world.w) < VKR_FLOAT_EPSILON) {
    return false_v;
  }

  Vec3 near_pos =
      vec3_new(near_world.x / near_world.w, near_world.y / near_world.w,
               near_world.z / near_world.w);
  Vec3 far_pos = vec3_new(far_world.x / far_world.w, far_world.y / far_world.w,
                          far_world.z / far_world.w);
  Vec3 dir = vec3_sub(far_pos, near_pos);
  if (vec3_length_squared(dir) < VKR_FLOAT_EPSILON) {
    return false_v;
  }

  *out_origin = near_pos;
  *out_dir = vec3_normalize(dir);
  return true_v;
}

/**
 * @brief Intersect a ray with a plane.
 *
 * Returns false when the ray is parallel to the plane (no stable hit point).
 */
vkr_internal bool8_t vkr_standard_scene_runtime_ray_plane_intersect(
    Vec3 ray_origin, Vec3 ray_dir, Vec3 plane_point, Vec3 plane_normal,
    Vec3 *out_point) {
  if (!out_point) {
    return false_v;
  }

  float32_t denom = vec3_dot(ray_dir, plane_normal);
  if (vkr_abs_f32(denom) < VKR_FLOAT_EPSILON) {
    return false_v;
  }

  float32_t t =
      vec3_dot(vec3_sub(plane_point, ray_origin), plane_normal) / denom;
  if (t < 0.0f) {
    return false_v;
  }

  *out_point = vec3_add(ray_origin, vec3_scale(ray_dir, t));
  return true_v;
}

/**
 * @brief Pick a plane normal for axis dragging that stays stable near edge-on
 * views.
 */
vkr_internal Vec3 vkr_standard_scene_runtime_gizmo_axis_plane_normal(
    const VkrCamera *camera, Vec3 axis) {
  Vec3 view_dir = vec3_normalize(camera->forward);
  float32_t view_axis = vec3_dot(view_dir, axis);
  Vec3 normal = vec3_sub(view_dir, vec3_scale(axis, view_axis));
  if (vec3_length_squared(normal) < VKR_FLOAT_EPSILON) {
    normal = vec3_cross(axis, camera->up);
    if (vec3_length_squared(normal) < VKR_FLOAT_EPSILON) {
      normal = vec3_cross(axis, camera->right);
    }
  }
  return vec3_normalize(normal);
}

vkr_internal void vkr_standard_scene_runtime_clear_gizmo_handles(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  vkr_gizmo_system_set_active_handle(&application->gizmo_system,
                                     VKR_GIZMO_HANDLE_NONE);
  vkr_gizmo_system_set_hot_handle(&application->gizmo_system,
                                  VKR_GIZMO_HANDLE_NONE);
  state->gizmo_hot_handle = VKR_GIZMO_HANDLE_NONE;
}

vkr_internal void vkr_standard_scene_runtime_cancel_gizmo_pick(
    VkrStandardSceneRuntime *application) {
  vkr_picking_cancel(&application->picking);
  state->context_pick = false_v;
  state->gizmo_hover_pending = false_v;
  state->gizmo_drag.pending_pick = false_v;
  state->gizmo_drag.pending_select = false_v;
  state->gizmo_drag.released = false_v;
  state->gizmo_drag.release_has_target_coords = false_v;
}

vkr_internal void vkr_standard_scene_runtime_clear_gizmo_selection(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  vkr_standard_scene_runtime_cancel_gizmo_pick(application);
  state->gizmo_drag.active = false_v;
  state->gizmo_edit_pending = false_v;
  state->selected_entity = VKR_ENTITY_ID_INVALID;
  state->has_selection = false_v;
  vkr_gizmo_system_clear_target(&application->gizmo_system);
  state->gizmo_hot_handle = VKR_GIZMO_HANDLE_NONE;
  state->gizmo_drag.uses_text_pivot = false_v;
}

/* The entity a picked world text slot belongs to, when it lives in
   `scene`. */
vkr_internal bool8_t vkr_standard_scene_runtime_world_text_entity_from_id(
    VkrStandardSceneRuntime *application, VkrScene *scene, uint32_t text_id,
    VkrEntityId *out_entity) {
  const VkrWorldResources *resources = &application->assets.world_resources;
  if (!scene || !scene->world || !out_entity ||
      text_id >= resources->text_slots.length ||
      !resources->text_slots.data[text_id].active) {
    return false_v;
  }
  const VkrEntityId owner = resources->text_slots.data[text_id].owner;
  const SceneText3D *text = vkr_entity_is_alive(scene->world, owner)
                                ? vkr_scene_get_text3d(scene, owner)
                                : NULL;
  if (!text || text->text_index != text_id) {
    return false_v;
  }
  *out_entity = owner;
  return true_v;
}

/**
 * @brief Computes the centered local pivot for a text3d quad.
 */
vkr_internal bool8_t vkr_standard_scene_runtime_text_pivot_local(
    SceneText3D *text, Vec3 *out_local) {
  if (!text || !out_local) {
    return false_v;
  }

  *out_local =
      vec3_new(text->world_width * 0.5f, text->world_height * 0.5f, 0.0f);
  return true_v;
}

/**
 * @brief Transforms a local text pivot into world space.
 */
vkr_internal Vec3 vkr_standard_scene_runtime_text_pivot_world(
    const SceneTransform *transform, Vec3 pivot_local) {
  Mat4 world = transform->world;
  Vec4 pivot_world = mat4_mul_vec4(
      world, vec4_new(pivot_local.x, pivot_local.y, pivot_local.z, 1.0f));
  return vec3_new(pivot_world.x, pivot_world.y, pivot_world.z);
}

/**
 * @brief Computes local origin needed to keep a pivot fixed in world space.
 *
 * Uses parent space so child transforms preserve the pivot under hierarchy.
 */
vkr_internal Vec3 vkr_standard_scene_runtime_text_origin_from_pivot(
    VkrScene *scene, const SceneTransform *transform, Vec3 pivot_world,
    Vec3 pivot_local, Vec3 scale, VkrQuat rotation) {
  Mat4 parent_world = mat4_identity();
  if (transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64) {
    SceneTransform *parent_transform =
        vkr_scene_get_transform(scene, transform->parent);
    if (parent_transform) {
      parent_world = parent_transform->world;
    }
  }

  Mat4 parent_inv = mat4_inverse_affine(parent_world);
  Vec4 pivot_local_pos = mat4_mul_vec4(
      parent_inv, vec4_new(pivot_world.x, pivot_world.y, pivot_world.z, 1.0f));
  Vec3 pivot_parent =
      vec3_new(pivot_local_pos.x, pivot_local_pos.y, pivot_local_pos.z);

  Vec3 scaled_offset = vec3_mul(scale, pivot_local);
  Vec3 rotated_offset = vkr_quat_rotate_vec3(rotation, scaled_offset);
  return vec3_sub(pivot_parent, rotated_offset);
}

vkr_internal void vkr_standard_scene_runtime_sync_world_text_transform(
    VkrStandardSceneRuntime *application, VkrScene *scene, VkrEntityId entity) {
  if (!application || !scene || !scene->world) {
    return;
  }

  SceneText3D *text = vkr_scene_get_text3d(scene, entity);
  if (!text) {
    return;
  }

  SceneTransform *transform = vkr_scene_get_transform(scene, entity);
  if (!transform) {
    return;
  }

  VkrTransform text_transform = vkr_transform_from_position_scale_rotation(
      transform->position, transform->scale, transform->rotation);

  vkr_standard_scene_runtime_queue_world_text_update(
      application, text->text_index, (String8){0}, &text_transform);
}

/* The loaded container holding `entity`, by its world id (ADR-076): the
 * primary scene, the root World or an added scene; and its edit journal. */
static VkrScene *sample_entity_container(VkrStandardSceneRuntime *application,
                                         VkrEntityId entity,
                                         VkrSceneEditState **out_edits) {
  VkrScene *scene = NULL;
  VkrSceneEditState *edits = NULL;
  const uint32_t world = entity.parts.world;
  if (world == VKR_SCENE_WORLD_ROOT_ID) {
    scene = application->world_scene;
    edits = &state->world_edits;
  } else if (world == 0u) {
    scene = application->active_scene;
    edits = &state->edits;
  } else if (world <= VKR_SCENE_ADDITIVE_MAX &&
             state->additive_handles[world - 1u]) {
    scene = vkr_scene_handle_get_scene(state->additive_handles[world - 1u]);
    edits = &state->additive_edits[world - 1u];
  }
  if (out_edits) {
    *out_edits = scene ? edits : NULL;
  }
  return scene;
}

/* The container Play simulates: the primary scene once it is ready, else the
 * root World when no scene is open or loading (ADR-079). */
static VkrScene *sample_simulated_scene(VkrStandardSceneRuntime *application,
                                        VkrSceneHandle *out_handle) {
  VkrSceneHandle handle = NULL;
  if (state->scene_resource.as.scene && application->active_scene) {
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (vkr_resource_system_get_state(&state->scene_resource, &error) ==
        VKR_RESOURCE_LOAD_STATE_READY) {
      handle = state->scene_resource.as.scene;
    }
  } else if (!state->scene_resource.as.scene &&
             !state->scene_resource.request_id && state->world_handle &&
             !state->world_pending) {
    handle = state->world_handle;
  }
  if (out_handle) {
    *out_handle = handle;
  }
  return handle ? vkr_scene_handle_get_scene(handle) : NULL;
}

/* Material textures load at the tighter of the live Graphics texture
 * resolution and the scene's texture limit. */
vkr_internal void
sample_apply_texture_limit(VkrStandardSceneRuntime *application) {
  const uint32_t graphics =
      vkr_graphics_settings_texture_max_dimension(&state->graphics.settings);
  const uint32_t scene_extent = state->scene_texture_extent;
  uint32_t limit = graphics ? graphics : scene_extent;
  if (graphics && scene_extent) {
    limit = Min(graphics, scene_extent);
  }
  vkr_material_system_set_texture_extent_limit(
      &application->assets.material_system, limit);
}

/* The scene's limit follows the container Play would simulate. While a scene
 * loads, the limit its sidecar held when the load began stays in effect. */
vkr_internal void
sample_sync_texture_limit(VkrStandardSceneRuntime *application) {
  const VkrScene *scene = sample_simulated_scene(application, NULL);
  if (scene) {
    state->scene_texture_extent = scene->settings.texture_max_extent;
  }
  sample_apply_texture_limit(application);
}

static void gizmo_companions_move(VkrScene *scene, Vec3 offset);

vkr_internal bool8_t vkr_standard_scene_runtime_restore_gizmo_edit(
    VkrStandardSceneRuntime *application) {
  VkrSceneEditState *edits = NULL;
  VkrScene *scene =
      sample_entity_container(application, state->gizmo_edit_entity, &edits);
  if (!scene) {
    return false_v;
  }
  if (state->gizmo_before.fields & VKR_SCENE_EDIT_PHYSICS) {
    const char *error = NULL;
    if (!vkr_scene_physics_apply(scene, state->gizmo_edit_entity,
                                 &state->gizmo_before.physics, &error)) {
      snprintf(edits->status, sizeof(edits->status),
               "Could not restore collider drag: %s",
               error ? error : "allocation failed");
      return false_v;
    }
  } else {
    if (!vkr_scene_set_transform(
            scene, state->gizmo_drag.entity, state->gizmo_before.position,
            state->gizmo_before.rotation, state->gizmo_before.scale)) {
      snprintf(edits->status, sizeof(edits->status),
               "Could not restore the authored transform.");
      return false_v;
    }
    vkr_standard_scene_runtime_sync_world_text_transform(
        application, scene, state->gizmo_drag.entity);
    gizmo_companions_move(scene, vec3_zero());
  }
  return true_v;
}

vkr_internal bool8_t vkr_standard_scene_runtime_apply_gizmo_pose(
    VkrStandardSceneRuntime *application, Vec3 position, VkrQuat rotation,
    Vec3 scale) {
  VkrSceneEditState *edits = NULL;
  VkrScene *scene =
      sample_entity_container(application, state->gizmo_drag.entity, &edits);
  if (!scene) {
    return false_v;
  }
  if (state->gizmo_before.fields & VKR_SCENE_EDIT_PHYSICS) {
    VkrScenePhysicsSnapshot snapshot = state->gizmo_before.physics;
    for (uint32_t i = 0; i < snapshot.collider_count; ++i) {
      VkrSceneColliderConfig *collider = &snapshot.colliders[i];
      if (collider->authored_id != state->gizmo_collider_id) {
        continue;
      }
      collider->position = position;
      collider->rotation = rotation;
      collider->scale = scale;
      const char *error = NULL;
      if (!vkr_scene_physics_apply(scene, state->gizmo_edit_entity, &snapshot,
                                   &error)) {
        snprintf(edits->status, sizeof(edits->status), "%s",
                 error ? error : "Collider edit failed.");
        return false_v;
      }
      return true_v;
    }
    return false_v;
  }
  const SceneTransform *transform =
      vkr_scene_get_transform(scene, state->gizmo_drag.entity);
  const char *error = NULL;
  if (!transform ||
      !vkr_scene_physics_transform_validate(scene, state->gizmo_drag.entity,
                                            position, rotation, scale,
                                            transform->parent, &error) ||
      !vkr_scene_set_transform(scene, state->gizmo_drag.entity, position,
                               rotation, scale)) {
    snprintf(edits->status, sizeof(edits->status), "%s",
             error ? error : "Transform edit failed.");
    return false_v;
  }
  vkr_standard_scene_runtime_sync_world_text_transform(
      application, scene, state->gizmo_drag.entity);
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_cancel_gizmo_edit(
    VkrStandardSceneRuntime *application) {
  VkrScene *scene =
      sample_entity_container(application, state->gizmo_edit_entity, NULL);
  if (state->gizmo_edit_pending && scene &&
      !vkr_standard_scene_runtime_restore_gizmo_edit(application)) {
    state->gizmo_drag.active = false_v;
    vkr_standard_scene_runtime_clear_gizmo_handles(application);
    return;
  }
  state->gizmo_edit_pending = false_v;
  state->gizmo_drag.active = false_v;
  state->gizmo_drag.handle = VKR_GIZMO_HANDLE_NONE;
  vkr_standard_scene_runtime_clear_gizmo_handles(application);
}

vkr_internal bool8_t vkr_standard_scene_runtime_request_picking(
    VkrStandardSceneRuntime *application, VkrPickingContext *picking,
    const VkrViewportHitInfo *viewport_info) {
  if (!application || !picking || !viewport_info ||
      !viewport_info->has_target_coords || viewport_info->target_width == 0 ||
      viewport_info->target_height == 0) {
    return false_v;
  }

  if (picking->width != viewport_info->target_width ||
      picking->height != viewport_info->target_height) {
    vkr_picking_resize(picking, viewport_info->target_width,
                       viewport_info->target_height);
  }

  if (viewport_info->target_x >= picking->width ||
      viewport_info->target_y >= picking->height) {
    return false_v;
  }

  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!vkr_standard_scene_runtime_build_view_ray(
          camera, viewport_info->position, &state->gizmo_drag.pick_ray_origin,
          &state->gizmo_drag.pick_ray_direction))
    return false_v;
  state->picked_collider = VKR_ENTITY_ID_INVALID;
  if (state->view_state.collision_display && application->active_scene) {
    VkrPhysicsRayHit hit = {0};
    const Vec3 displacement =
        vec3_scale(state->gizmo_drag.pick_ray_direction, camera->far_clip);
    if (vkr_scene_physics_raycast(application->active_scene,
                                  state->gizmo_drag.pick_ray_origin,
                                  displacement, &hit)) {
      const VkrEntityId owner = {.u64 = hit.entity_id};
      const VkrEntityId child = {.u64 = hit.collider_entity_id};
      const VkrEntityId selected_owner = vkr_scene_physics_owner(
          application->active_scene, state->selected_entity);
      if ((state->view_state.collision_display == 2 ||
           owner.u64 == selected_owner.u64) &&
          vkr_scene_entity_alive(application->active_scene, child) &&
          vkr_scene_physics_owner(application->active_scene, child).u64 ==
              owner.u64) {
        state->picked_collider = child;
      }
    }
  }
  state->pick_scene_generation = application->scene_generation;
  state->pick_selected_entity = state->selected_entity;
  vkr_picking_request(picking, viewport_info->target_x,
                      viewport_info->target_y);
  return true_v;
}

vkr_internal bool8_t vkr_standard_scene_runtime_gizmo_parent_frame(
    VkrScene *scene, const SceneTransform *transform, Mat4 *out_inverse,
    VkrQuat *out_rotation) {
  if (!transform || !transform->trs_editable ||
      fabsf(transform->scale.x) < 1e-6f || fabsf(transform->scale.y) < 1e-6f ||
      fabsf(transform->scale.z) < 1e-6f)
    return false_v;
  *out_inverse = mat4_identity();
  *out_rotation = vkr_quat_identity();
  VkrEntityId ancestor = transform->parent;
  if (ancestor.u64 != VKR_ENTITY_ID_INVALID.u64) {
    const SceneTransform *parent = vkr_scene_get_transform(scene, ancestor);
    if (!parent)
      return false_v;
    const Mat4 world = parent->world;
    const Vec3 x =
        vec3_new(world.elements[0], world.elements[1], world.elements[2]);
    const Vec3 y =
        vec3_new(world.elements[4], world.elements[5], world.elements[6]);
    const Vec3 z =
        vec3_new(world.elements[8], world.elements[9], world.elements[10]);
    const float32_t determinant = vec3_dot(x, vec3_cross(y, z));
    if (!isfinite(determinant) || fabsf(determinant) < 1e-6f)
      return false_v;
    *out_inverse = mat4_inverse_affine(world);
  }
  while (ancestor.u64 != VKR_ENTITY_ID_INVALID.u64) {
    const SceneTransform *parent = vkr_scene_get_transform(scene, ancestor);
    if (!parent || !parent->trs_editable)
      return false_v;
    *out_rotation = vkr_quat_mul(parent->rotation, *out_rotation);
    ancestor = parent->parent;
  }
  *out_rotation = vkr_quat_normalize(*out_rotation);
  return true_v;
}

/* Asks the UI client which entities a move carries along, keeping those of
   `scene` with editable transforms that are not physics-owned. */
static void gizmo_companions_begin(VkrStandardSceneRuntime *application,
                                   VkrScene *scene, VkrGizmoMode mode) {
  state->gizmo_drag.companion_count = 0u;
  if (mode != VKR_GIZMO_MODE_TRANSLATE || !state->ui.move_companions ||
      (state->gizmo_before.fields & VKR_SCENE_EDIT_PHYSICS)) {
    return;
  }
  VkrEntityId found[GIZMO_COMPANION_MAX];
  const uint32_t count =
      Min(state->ui.move_companions(state->ui.state, state->gizmo_drag.entity,
                                    found, GIZMO_COMPANION_MAX),
          GIZMO_COMPANION_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    VkrScene *owner = sample_entity_container(application, found[i], NULL);
    const SceneTransform *transform =
        owner == scene ? vkr_scene_get_transform(scene, found[i]) : NULL;
    if (!transform || !transform->trs_editable ||
        found[i].u64 == state->gizmo_drag.entity.u64 ||
        vkr_scene_physics_owner(scene, found[i]).u64) {
      continue;
    }
    const SceneTransform *parent =
        transform->parent.u64
            ? vkr_scene_get_transform(scene, transform->parent)
            : NULL;
    const uint32_t slot = state->gizmo_drag.companion_count++;
    state->gizmo_drag.companions[slot] = found[i];
    state->gizmo_drag.companion_start[slot] = transform->position;
    state->gizmo_drag.companion_parent_inverse[slot] =
        parent ? mat4_inverse_affine(parent->world) : mat4_identity();
  }
}

/* Moves the companions by the world `offset` from where the move began. */
static void gizmo_companions_move(VkrScene *scene, Vec3 offset) {
  for (uint32_t i = 0; i < state->gizmo_drag.companion_count; ++i) {
    SceneTransform *transform =
        vkr_scene_get_transform(scene, state->gizmo_drag.companions[i]);
    if (!transform) {
      continue;
    }
    const Vec4 local =
        mat4_mul_vec4(state->gizmo_drag.companion_parent_inverse[i],
                      vec3_to_vec4(offset, 0.0f));
    (void)vkr_scene_set_transform(scene, state->gizmo_drag.companions[i],
                                  vec3_add(state->gizmo_drag.companion_start[i],
                                           vec3_new(local.x, local.y, local.z)),
                                  transform->rotation, transform->scale);
  }
}

vkr_internal bool8_t vkr_standard_scene_runtime_begin_gizmo_drag(
    VkrStandardSceneRuntime *application, VkrGizmoHandle handle) {
  if (!application || !state || !state->has_selection) {
    return false_v;
  }

  VkrScene *scene =
      sample_entity_container(application, state->selected_entity, NULL);
  if (!scene) {
    return false_v;
  }

  vkr_scene_update(scene, 0.0);
  SceneTransform *transform =
      vkr_scene_get_transform(scene, state->selected_entity);
  Mat4 parent_inverse;
  VkrQuat parent_rotation;
  if (!vkr_standard_scene_runtime_gizmo_parent_frame(
          scene, transform, &parent_inverse, &parent_rotation))
    return false_v;

  Vec3 pivot_local = vec3_zero();
  bool8_t has_text_pivot = false_v;
  SceneText3D *text = vkr_scene_get_text3d(scene, state->selected_entity);
  if (text) {
    has_text_pivot =
        vkr_standard_scene_runtime_text_pivot_local(text, &pivot_local);
  }

  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!camera) {
    return false_v;
  }

  VkrGizmoMode mode = vkr_gizmo_handle_mode(handle);
  if (mode == VKR_GIZMO_MODE_NONE) {
    return false_v;
  }

  /* Axes follow the gizmo's space; scale axes are always the object's. */
  const int32_t axis_index = vkr_gizmo_handle_axis_index(handle);
  const int32_t plane_index = vkr_gizmo_handle_plane_normal_index(handle);
  bool8_t has_axis = axis_index >= 0;
  Vec3 axis = has_axis ? vkr_gizmo_system_axis(&application->gizmo_system, mode,
                                               (uint32_t)axis_index)
                       : vec3_zero();
  Vec3 plane_normal;

  if (mode == VKR_GIZMO_MODE_ROTATE) {
    if (!has_axis) {
      axis = vec3_normalize(camera->forward);
    }
    plane_normal = axis;
  } else if (plane_index >= 0) {
    plane_normal = vkr_gizmo_system_axis(&application->gizmo_system, mode,
                                         (uint32_t)plane_index);
  } else if (!has_axis) {
    plane_normal = vec3_normalize(camera->forward);
  } else {
    plane_normal =
        vkr_standard_scene_runtime_gizmo_axis_plane_normal(camera, axis);
  }

  Vec3 world_position = mat4_position(transform->world);
  if (has_text_pivot) {
    world_position =
        vkr_standard_scene_runtime_text_pivot_world(transform, pivot_local);
  }
  const Vec3 ray_origin = state->gizmo_drag.pick_ray_origin;
  const Vec3 ray_dir = state->gizmo_drag.pick_ray_direction;

  Vec3 hit = vec3_zero();
  if (!vkr_standard_scene_runtime_ray_plane_intersect(
          ray_origin, ray_dir, world_position, plane_normal, &hit)) {
    return false_v;
  }

  Vec3 offset = vec3_sub(hit, world_position);

  const VkrEntityId owner =
      vkr_scene_physics_owner(scene, state->selected_entity);
  if (owner.u64 && !vkr_scene_physics_is_paused(scene)) {
    return false_v;
  }
  const ScenePhysicsCollider *collider = vkr_entity_get_component(
      scene->world, state->selected_entity, scene->comp_physics_collider);
  state->gizmo_edit_entity =
      collider ? collider->owner : state->selected_entity;
  state->gizmo_collider_id = collider ? collider->authored_id : 0;
  state->gizmo_edit_pending =
      application->editor_viewport.enabled &&
      vkr_scene_edit_read(scene, state->gizmo_edit_entity,
                          &state->gizmo_before);
  if (!state->gizmo_edit_pending ||
      !(state->gizmo_before.fields & VKR_SCENE_EDIT_TRANSFORM)) {
    state->gizmo_edit_pending = false_v;
    return false_v;
  }
  state->gizmo_before.fields =
      collider ? VKR_SCENE_EDIT_PHYSICS : VKR_SCENE_EDIT_TRANSFORM;
  state->gizmo_drag.entity = state->selected_entity;
  state->gizmo_drag.parent_inverse = parent_inverse;
  state->gizmo_drag.parent_rotation = parent_rotation;
  state->gizmo_drag.active = true_v;
  state->gizmo_drag.mode = mode;
  state->gizmo_drag.handle = handle;
  state->gizmo_drag.axis = axis;
  state->gizmo_drag.plane_normal = plane_normal;
  state->gizmo_drag.start_world_position = world_position;
  state->gizmo_drag.start_hit = hit;
  state->gizmo_drag.start_scale = transform->scale;
  state->gizmo_drag.start_rotation = transform->rotation;
  state->gizmo_drag.start_radius = vec3_length(offset);
  state->gizmo_drag.start_distance = vec3_dot(offset, axis);
  state->gizmo_drag.snap_pending = true_v;
  state->gizmo_drag.uses_text_pivot = has_text_pivot;
  state->gizmo_drag.text_pivot_local = pivot_local;
  gizmo_companions_begin(application, scene, mode);
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_update_gizmo_drag(
    VkrStandardSceneRuntime *application,
    const VkrViewportHitInfo *viewport_info) {
  if (!application || !state || !state->gizmo_drag.active || !viewport_info ||
      !viewport_info->has_target_coords) {
    return;
  }

  VkrScene *scene =
      sample_entity_container(application, state->gizmo_drag.entity, NULL);
  if (!scene) {
    vkr_standard_scene_runtime_cancel_gizmo_edit(application);
    return;
  }

  SceneTransform *transform =
      vkr_scene_get_transform(scene, state->gizmo_drag.entity);
  if (!transform) {
    vkr_standard_scene_runtime_cancel_gizmo_edit(application);
    return;
  }

  if (viewport_info->position.x == state->gizmo_drag.pick_position.x &&
      viewport_info->position.y == state->gizmo_drag.pick_position.y) {
    (void)vkr_standard_scene_runtime_restore_gizmo_edit(application);
    return;
  }

  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!camera) {
    vkr_standard_scene_runtime_cancel_gizmo_edit(application);
    return;
  }

  Vec3 ray_origin = vec3_zero();
  Vec3 ray_dir = vec3_zero();
  if (!vkr_standard_scene_runtime_build_view_ray(
          camera, viewport_info->position, &ray_origin, &ray_dir)) {
    return;
  }

  Vec3 hit = vec3_zero();
  if (!vkr_standard_scene_runtime_ray_plane_intersect(
          ray_origin, ray_dir, state->gizmo_drag.start_hit,
          state->gizmo_drag.plane_normal, &hit)) {
    return;
  }

  Vec3 delta = vec3_sub(hit, state->gizmo_drag.start_hit);

  bool8_t updated = false_v;
  Vec3 result_position = transform->position;
  VkrQuat result_rotation = state->gizmo_drag.start_rotation;
  Vec3 result_scale = state->gizmo_drag.start_scale;

  if (state->gizmo_drag.mode == VKR_GIZMO_MODE_TRANSLATE) {
    Vec3 new_pivot;
    /* Free and plane handles move within their drag plane. */
    if (vkr_gizmo_handle_is_free_translate(state->gizmo_drag.handle) ||
        vkr_gizmo_handle_plane_normal_index(state->gizmo_drag.handle) >= 0) {
      new_pivot = vec3_add(state->gizmo_drag.start_world_position, delta);
    } else {
      float32_t dist = vec3_dot(delta, state->gizmo_drag.axis);
      Vec3 axis_delta = vec3_scale(state->gizmo_drag.axis, dist);
      new_pivot = vec3_add(state->gizmo_drag.start_world_position, axis_delta);
    }
    if (state->ui.snap_move && !state->gizmo_drag.uses_text_pivot) {
      /* The client's snap stays on an axis handle's axis and in a plane
         handle's plane; the free handle takes it whole. */
      const Vec3 start = state->gizmo_drag.start_world_position;
      Vec3 offset =
          vec3_sub(state->ui.snap_move(
                       state->ui.state, scene, state->gizmo_drag.entity, start,
                       new_pivot, ray_origin, state->gizmo_drag.snap_pending),
                   start);
      state->gizmo_drag.snap_pending = false_v;
      if (vkr_gizmo_handle_plane_normal_index(state->gizmo_drag.handle) >= 0) {
        const Vec3 normal = state->gizmo_drag.plane_normal;
        offset = vec3_sub(offset, vec3_scale(normal, vec3_dot(offset, normal)));
      } else if (!vkr_gizmo_handle_is_free_translate(
                     state->gizmo_drag.handle)) {
        offset = vec3_scale(state->gizmo_drag.axis,
                            vec3_dot(offset, state->gizmo_drag.axis));
      }
      new_pivot = vec3_add(start, offset);
    }

    gizmo_companions_move(
        scene, vec3_sub(new_pivot, state->gizmo_drag.start_world_position));

    Vec3 local_pos;
    if (state->gizmo_drag.uses_text_pivot) {
      local_pos = vkr_standard_scene_runtime_text_origin_from_pivot(
          scene, transform, new_pivot, state->gizmo_drag.text_pivot_local,
          state->gizmo_drag.start_scale, state->gizmo_drag.start_rotation);
    } else {
      const Vec4 local =
          mat4_mul_vec4(state->gizmo_drag.parent_inverse,
                        vec4_new(new_pivot.x, new_pivot.y, new_pivot.z, 1.0f));
      local_pos = vec3_new(local.x, local.y, local.z);
    }

    result_position = local_pos;
    updated = true_v;
  } else if (state->gizmo_drag.mode == VKR_GIZMO_MODE_SCALE) {
    Vec3 new_scale = state->gizmo_drag.start_scale;
    const Vec3 offset = vec3_sub(hit, state->gizmo_drag.start_world_position);
    const int32_t axis_index =
        vkr_gizmo_handle_axis_index(state->gizmo_drag.handle);
    if (axis_index >= 0) {
      /* One component follows the pointer's distance along its axis. */
      const float32_t start_distance = state->gizmo_drag.start_distance;
      if (fabsf(start_distance) > VKR_FLOAT_EPSILON) {
        const float32_t start = new_scale.elements[axis_index];
        const float32_t factor =
            vec3_dot(offset, state->gizmo_drag.axis) / start_distance;
        float32_t next = start * factor;
        if (fabsf(next) < 0.001f) {
          next = start < 0.0f ? -0.001f : 0.001f;
        }
        new_scale.elements[axis_index] = next;
      }
    } else if (state->gizmo_drag.start_radius > VKR_FLOAT_EPSILON) {
      const float32_t radius = vec3_length(offset);
      const Vec3 start = state->gizmo_drag.start_scale;
      const float32_t smallest =
          Min(fabsf(start.x), Min(fabsf(start.y), fabsf(start.z)));
      const float32_t scale_factor =
          Max(0.001f / smallest, radius / state->gizmo_drag.start_radius);
      new_scale = vec3_scale(start, scale_factor);
    }
    if (state->gizmo_drag.uses_text_pivot) {
      Vec3 local_pos = vkr_standard_scene_runtime_text_origin_from_pivot(
          scene, transform, state->gizmo_drag.start_world_position,
          state->gizmo_drag.text_pivot_local, new_scale,
          state->gizmo_drag.start_rotation);
      result_position = local_pos;
    }

    result_scale = new_scale;
    updated = true_v;
  } else if (state->gizmo_drag.mode == VKR_GIZMO_MODE_ROTATE) {
    Vec3 pivot = state->gizmo_drag.start_world_position;
    Vec3 from = vec3_sub(state->gizmo_drag.start_hit, pivot);
    Vec3 to = vec3_sub(hit, pivot);

    if (vec3_length_squared(from) < VKR_FLOAT_EPSILON ||
        vec3_length_squared(to) < VKR_FLOAT_EPSILON) {
      return;
    }

    Vec3 from_n = vec3_normalize(from);
    Vec3 to_n = vec3_normalize(to);
    float32_t cos_angle = vkr_clamp_f32(vec3_dot(from_n, to_n), -1.0f, 1.0f);
    float32_t angle = vkr_acos_f32(cos_angle);
    Vec3 cross = vec3_cross(from_n, to_n);
    float32_t sign =
        (vec3_dot(state->gizmo_drag.axis, cross) < 0.0f) ? -1.0f : 1.0f;
    angle *= sign;
    if (state->ui.snap_turn) {
      angle = state->ui.snap_turn(state->ui.state, angle);
    }

    /* Preserve editable local TRS. A nonuniform parent can introduce shear
     * under exact world rotation, so only its rotational frame is converted. */
    const Vec3 local_axis = vkr_quat_rotate_vec3(
        vkr_quat_conjugate(state->gizmo_drag.parent_rotation),
        state->gizmo_drag.axis);

    VkrQuat rotation_delta = vkr_quat_from_axis_angle(local_axis, angle);
    VkrQuat new_rotation =
        vkr_quat_mul(rotation_delta, state->gizmo_drag.start_rotation);
    new_rotation = vkr_quat_normalize(new_rotation);

    if (state->gizmo_drag.uses_text_pivot) {
      Vec3 local_pos = vkr_standard_scene_runtime_text_origin_from_pivot(
          scene, transform, state->gizmo_drag.start_world_position,
          state->gizmo_drag.text_pivot_local, state->gizmo_drag.start_scale,
          new_rotation);
      result_position = local_pos;
    }

    result_rotation = new_rotation;
    updated = true_v;
  }

  if (updated) {
    (void)vkr_standard_scene_runtime_apply_gizmo_pose(
        application, result_position, result_rotation, result_scale);
  }
}

vkr_internal bool8_t vkr_standard_scene_runtime_is_mtsdf_atlas(
    const VkrFontSystem *font_system, VkrTextureHandle texture) {
  for (uint64_t i = 0u; i < font_system->fonts.length; ++i) {
    const VkrFont *font = &font_system->fonts.data[i];
    if (font->generation == VKR_INVALID_ID || font->type != VKR_FONT_TYPE_MTSDF)
      continue;
    if (font->atlas.id == texture.id &&
        font->atlas.generation == texture.generation)
      return true_v;
    for (uint64_t page_index = 0u; page_index < font->atlas_pages.length;
         ++page_index) {
      const VkrTextureHandle page = font->atlas_pages.data[page_index];
      if (page.id == texture.id && page.generation == texture.generation)
        return true_v;
    }
  }
  return false_v;
}

vkr_internal void vkr_standard_scene_runtime_apply_filter_mode(
    VkrStandardSceneRuntime *application, uint32_t mode_index) {
  if (!application || !state)
    return;

  uint32_t clamped_index = mode_index % (uint32_t)ArrayCount(FILTER_MODES);
  FilterModeEntry entry = FILTER_MODES[clamped_index];

  bool8_t anisotropy_enable =
      (entry.anisotropy && state->anisotropy_supported) ? true_v : false_v;
  if (entry.anisotropy && !state->anisotropy_supported) {
    log_warn("Anisotropic filtering not supported on this device; disabling "
             "anisotropy for this mode");
  }

  VkrTextureSystem *texture_system = &application->assets.texture_system;
  uint32_t failures = 0;
  for (uint32_t i = 0; i < texture_system->textures.length; ++i) {
    VkrTexture *tex = &texture_system->textures.data[i];
    if (!tex->handle || tex->description.generation == VKR_INVALID_ID ||
        tex->description.id == VKR_INVALID_ID) {
      continue;
    }

    VkrTextureHandle handle = {.id = tex->description.id,
                               .generation = tex->description.generation};
    // MTSDF reconstruction requires linear atlas filtering in every mode.
    if (vkr_standard_scene_runtime_is_mtsdf_atlas(
            &application->assets.font_system, handle))
      continue;
    VkrRendererError err = vkr_texture_system_update_sampler(
        texture_system, handle, entry.min_filter, entry.mag_filter,
        entry.mip_filter, anisotropy_enable, tex->description.u_repeat_mode,
        tex->description.v_repeat_mode, tex->description.w_repeat_mode);
    if (err != VKR_RENDERER_ERROR_NONE) {
      failures++;
    }
  }

  state->filter_mode_index = clamped_index;
  log_info("Texture filtering set to %s%s", entry.label,
           failures ? " (some updates failed)" : "");
  if (state->filter_mode_index == 5) {
    log_info("Anisotropic sampling count: %f", state->max_sampler_anisotropy);
  }
}

/* Returns whether `path` (null-terminated storage) names `next`. */
vkr_internal bool8_t sample_warm_path_is(const char *path, String8 next) {
  const String8 held =
      string8_create_from_cstr((const uint8_t *)path, string_length(path));
  return next.length && string8_equals(&held, &next);
}

/* Holds the loaded mesh assets of the primary scene a switch closes, and with
   them their materials and textures, so opening that scene again skips
   loading them. One closed scene stays warm. The set before it waits as
   the opening set while the next scene is unknown (a project switch unloads
   before its Bakery job and opens afterwards) or is the set's scene, and is
   released otherwise; the opening set lasts until a scene opens. */
vkr_internal void sample_warm_hold_take(VkrStandardSceneRuntime *application,
                                        String8 next_path) {
  VkrMeshManager *manager = &application->assets.mesh_manager;
  uint32_t released = 0u;
  if (!application->active_scene) {
    if (next_path.length &&
        !sample_warm_path_is(state->opening_path, next_path)) {
      released = state->opening_hold.count;
      vkr_mesh_manager_release_hold(manager, &state->opening_hold);
      state->opening_path[0] = '\0';
    }
    if (released) {
      log_info("WARM_ASSETS kept=0 opening=0 released=%u", released);
    }
    return;
  }

  VkrMeshAssetHold hold = {0};
  if (state->scene_path.length < sizeof(state->warm_path) &&
      !vkr_mesh_manager_hold_drawn_assets(manager, &hold)) {
    log_warn("Out of memory keeping the closed scene's assets loaded");
  }

  released = state->opening_hold.count;
  vkr_mesh_manager_release_hold(manager, &state->opening_hold);
  state->opening_path[0] = '\0';
  if (!next_path.length || sample_warm_path_is(state->warm_path, next_path)) {
    state->opening_hold = state->warm_hold;
    MemCopy(state->opening_path, state->warm_path, sizeof(state->warm_path));
  } else {
    released += state->warm_hold.count;
    vkr_mesh_manager_release_hold(manager, &state->warm_hold);
  }
  log_info("WARM_ASSETS kept=%u opening=%u released=%u", hold.count,
           state->opening_hold.count, released);

  state->warm_hold = hold;
  state->warm_path[0] = '\0';
  if (hold.count) {
    MemCopy(state->warm_path, state->scene_path.str, state->scene_path.length);
    state->warm_path[state->scene_path.length] = '\0';
  }
}

/* Lets the closed scenes' assets go once nothing will reopen them. */
vkr_internal void sample_warm_hold_release(VkrStandardSceneRuntime *application,
                                           bool8_t keep_warm) {
  VkrMeshManager *manager = &application->assets.mesh_manager;
  vkr_mesh_manager_release_hold(manager, &state->opening_hold);
  state->opening_path[0] = '\0';
  if (!keep_warm) {
    vkr_mesh_manager_release_hold(manager, &state->warm_hold);
    state->warm_path[0] = '\0';
  }
}

vkr_internal bool8_t vkr_standard_scene_runtime_try_activate_scene_resource(
    VkrStandardSceneRuntime *application) {
  if (!application || !state ||
      state->scene_resource.type != VKR_RESOURCE_TYPE_SCENE) {
    return false_v;
  }

  String8 scene_path = state->scene_path;
  VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
  VkrResourceLoadState load_state =
      vkr_resource_system_get_state(&state->scene_resource, &state_error);

  switch (load_state) {
  case VKR_RESOURCE_LOAD_STATE_READY:
    break;
  case VKR_RESOURCE_LOAD_STATE_FAILED:
    sample_warm_hold_release(application, true_v);
    if (!state->scene_load_terminal_logged) {
      String8 err = vkr_renderer_get_error_string(state_error);
      float64_t elapsed =
          vkr_standard_scene_runtime_consume_scene_load_elapsed_seconds(
              application);
      if (elapsed >= 0.0) {
        log_info("SCENE_LOAD_TIME result=failed seconds=%.3f ms=%.1f", elapsed,
                 elapsed * 1000.0);
      }
      log_error("Scene load failed for '%s': %s", string8_cstr(&scene_path),
                string8_cstr(&err));
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Scene load failed: %.*s", (int)err.length, err.str);
      state->scene_load_terminal_logged = true_v;
    }
    return false_v;
  case VKR_RESOURCE_LOAD_STATE_CANCELED:
    sample_warm_hold_release(application, true_v);
    if (!state->scene_load_terminal_logged) {
      float64_t elapsed =
          vkr_standard_scene_runtime_consume_scene_load_elapsed_seconds(
              application);
      if (elapsed >= 0.0) {
        log_info("SCENE_LOAD_TIME result=canceled seconds=%.3f ms=%.1f",
                 elapsed, elapsed * 1000.0);
      }
      log_warn("Scene load canceled for '%s'", string8_cstr(&scene_path));
      state->scene_load_terminal_logged = true_v;
    }
    return false_v;
  default:
    return false_v;
  }

  if (!state->scene_resource.as.scene) {
    VkrResourceHandleInfo resolved_info = {0};
    if (!vkr_resource_system_try_get_resolved(&state->scene_resource,
                                              &resolved_info) ||
        !resolved_info.as.scene) {
      if (!state->scene_load_terminal_logged) {
        log_error("Scene handle resolve failed for '%s'",
                  string8_cstr(&scene_path));
        state->scene_load_terminal_logged = true_v;
      }
      return false_v;
    }

    state->scene_resource = resolved_info;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(state->scene_resource.as.scene);
  if (!scene) {
    if (!state->scene_load_terminal_logged) {
      log_error("Scene '%s' is marked ready but scene pointer is null",
                string8_cstr(&scene_path));
      state->scene_load_terminal_logged = true_v;
    }
    return false_v;
  }

  state->scene_load_terminal_logged = false_v;
  if (application->active_scene != scene) {
    vkr_standard_scene_runtime_clear_gizmo_selection(application);
    application->active_scene = scene;
    application->scene_generation = application->scene_generation == UINT64_MAX
                                        ? 1u
                                        : application->scene_generation + 1u;
    vkr_scene_edit_reset(&state->edits, &state->edit_allocator,
                         application->scene_generation);
    sample_physics_attach(application, scene, true_v);
    const char *physics_root_error = NULL;
    if (!vkr_scene_physics_set_asset_root(
            scene,
            string8_create((uint8_t *)state->physics_asset_root,
                           strlen(state->physics_asset_root)),
            &physics_root_error)) {
      snprintf(state->edits.status, sizeof(state->edits.status), "%s",
               physics_root_error ? physics_root_error
                                  : "Invalid physics asset root");
    } else if (state->sidecar_path[0]) {
      (void)vkr_scene_edit_load(&state->edits, scene,
                                string8_create((uint8_t *)state->sidecar_path,
                                               strlen(state->sidecar_path)));
    }
    sample_partition_open(scene);
    sample_warm_hold_release(application, true_v);
    /* A fit from the previous scene is framed by a camera and caster set that
       no longer exist, so it is not a previous value of the same quantity. The
       configuration stamps cannot catch this: they are all identical across a
       scene swap. */
    vkr_standard_scene_runtime_invalidate_shadow_fit(application);
    float64_t elapsed =
        vkr_standard_scene_runtime_consume_scene_load_elapsed_seconds(
            application);
    if (elapsed >= 0.0) {
      const VkrMaterialTextureStreamStats texture_stats =
          vkr_material_system_get_texture_stream_stats(
              &application->assets.material_system);
      vkr_renderer_metrics_set_scene_boot_ns(
          &application->renderer_metrics, (uint64_t)(elapsed * 1000000000.0));
      log_info("SCENE_LOAD_TIME result=ready seconds=%.3f ms=%.1f "
               "texture_pending=%u texture_resident=%u",
               elapsed, elapsed * 1000.0, texture_stats.pending_count,
               texture_stats.resident_count);
    }
    log_info("Activated scene '%s' after async load",
             string8_cstr(&scene_path));
    vkr_standard_scene_runtime_log_backend_allocator_stats(
        application, "load-ready",
        state->scene_load_stats_baseline_valid
            ? &state->scene_load_stats_baseline
            : NULL);
    state->scene_load_stats_baseline_valid = false_v;
    // Captured at the same point as the allocator breakdown so the two can be
    // read together: one says how many bytes, the other how many allocations.
    vkr_standard_scene_runtime_log_device_memory_stats(application,
                                                       "load-ready");
  }

  return true_v;
}

/**
 * @brief Initialize scene system and load scene content.
 */
vkr_internal void vkr_standard_scene_runtime_init_scene_system(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  String8 scene_path = state->scene_path;
  if (state->scene_resource.type == VKR_RESOURCE_TYPE_SCENE) {
    if (vkr_standard_scene_runtime_try_activate_scene_resource(application)) {
      return;
    }

    VkrRendererError existing_err = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState existing_state =
        vkr_resource_system_get_state(&state->scene_resource, &existing_err);
    if (existing_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
        existing_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
        existing_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      return;
    }

    if (existing_state == VKR_RESOURCE_LOAD_STATE_FAILED ||
        existing_state == VKR_RESOURCE_LOAD_STATE_CANCELED) {
      vkr_resource_system_unload(&state->scene_resource, scene_path);
      state->scene_resource = (VkrResourceHandleInfo){0};
    }
  }

  if (application->active_scene != NULL) {
    return;
  }

  state->scene_resource = (VkrResourceHandleInfo){0};
  state->scene_load_terminal_logged = false_v;
  state->scene_load_timer_active = false_v;
  state->scene_load_start_time_seconds = 0.0;

  VkrAllocatorScope load_scope =
      vkr_allocator_begin_scope(application->frame_allocator);
  if (!vkr_allocator_scope_is_valid(&load_scope)) {
    log_error("Failed to create scene load scratch scope");
    return;
  }

  /* The scene's texture limit applies before its materials start streaming;
     its sidecar is applied once the scene loads. */
  VkrSceneSettings scene_settings = {0};
  (void)vkr_scene_edit_peek_settings(
      application->frame_allocator,
      string8_create_from_cstr((const uint8_t *)state->sidecar_path,
                               strlen(state->sidecar_path)),
      &scene_settings);
  state->scene_texture_extent = scene_settings.texture_max_extent;
  sample_apply_texture_limit(application);

  VkrRendererError load_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, scene_path,
                                application->frame_allocator,
                                &state->scene_resource, &load_err)) {
    String8 err_str = vkr_renderer_get_error_string(load_err);
    log_error("Failed to load scene '%s': %s", string8_cstr(&scene_path),
              string8_cstr(&err_str));
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return;
  }
  state->scene_load_stats_baseline_valid =
      vkr_standard_scene_runtime_capture_backend_allocator_stats(
          application, &state->scene_load_stats_baseline);
  state->scene_load_timer_active = true_v;
  state->scene_load_start_time_seconds = application->host.clock.elapsed;
  vkr_standard_scene_runtime_log_backend_allocator_stats(application,
                                                         "load-enqueue", NULL);

  vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  // An async load is not ready the instant it is enqueued; activation is
  // retried from vkr_standard_scene_runtime_update_scene. Not an error -- it is
  // the normal path, and logging it as one made every capture look like a
  // failure.
  if (!vkr_standard_scene_runtime_try_activate_scene_resource(application)) {
    log_debug("Scene load enqueued; activation deferred until it completes");
  }
}

vkr_internal void vkr_standard_scene_runtime_unload_scene_system(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  sample_scripts_stop(application);
  String8 scene_path = state->scene_path;
  if (state->scene_resource.type == VKR_RESOURCE_TYPE_SCENE ||
      state->scene_resource.request_id != 0 || state->scene_resource.as.scene) {
    if (vkr_renderer_wait_idle(&application->renderer) !=
        VKR_RENDERER_ERROR_NONE) {
      application->last_renderer_error = VKR_RENDERER_ERROR_DEVICE_ERROR;
      vkr_application_host_close(&application->host);
      return;
    }
    vkr_resource_system_unload(&state->scene_resource, scene_path);
    if (vkr_renderer_wait_idle(&application->renderer) !=
        VKR_RENDERER_ERROR_NONE) {
      application->last_renderer_error = VKR_RENDERER_ERROR_DEVICE_ERROR;
      vkr_application_host_close(&application->host);
    }
  }
  state->scene_resource = (VkrResourceHandleInfo){0};
  state->scene_load_terminal_logged = false_v;
  state->scene_load_stats_baseline_valid = false_v;
  state->scene_load_timer_active = false_v;
  state->scene_load_start_time_seconds = 0.0;
  vkr_scene_edit_reset(&state->edits, &state->edit_allocator, 0);
  state->gizmo_edit_pending = false_v;
  vkr_standard_scene_runtime_clear_gizmo_selection(application);
  application->active_scene = NULL;
  application->scene_output_scale = 1.0f;
  application->editor_viewport.rendered_width = 0u;
  application->editor_viewport.rendered_height = 0u;
  application->editor_viewport.output_width = 0u;
  application->editor_viewport.output_height = 0u;
  application->editor_viewport.scene_error = VKR_RENDERER_ERROR_NONE;
  application->scene_generation = application->scene_generation == UINT64_MAX
                                      ? 1u
                                      : application->scene_generation + 1u;
  vkr_standard_scene_runtime_invalidate_shadow_fit(application);
  vkr_standard_scene_runtime_log_backend_allocator_stats(application, "unload",
                                                         NULL);
  vkr_standard_scene_runtime_log_device_memory_stats(application, "unload");
}

vkr_internal void vkr_standard_scene_runtime_init_memory_text(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  state->memory_update_clock = vkr_clock_create();
  vkr_clock_start(&state->memory_update_clock);
  vkr_standard_scene_runtime_ui_text_set(
      &state->memory_text, string8_lit("Memory metrics: pending"));
}

vkr_internal void vkr_standard_scene_runtime_update_memory_text(
    VkrStandardSceneRuntime *application) {
  if (!application || !state ||
      !vkr_clock_interval_elapsed(&state->memory_update_clock,
                                  VKR_MEMORY_UPDATE_INTERVAL)) {
    return;
  }

  const VkrAllocatorStatistics cpu = vkr_allocator_get_global_statistics();
  uint64_t cpu_other = 0u;
  for (uint32_t tag = 0; tag < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++tag) {
    switch ((VkrAllocatorMemoryTag)tag) {
    case VKR_ALLOCATOR_MEMORY_TAG_RENDERER:
    case VKR_ALLOCATOR_MEMORY_TAG_ARRAY:
    case VKR_ALLOCATOR_MEMORY_TAG_STRING:
    case VKR_ALLOCATOR_MEMORY_TAG_FILE:
    case VKR_ALLOCATOR_MEMORY_TAG_VULKAN:
    case VKR_ALLOCATOR_MEMORY_TAG_TEXTURE:
      break;
    default:
      cpu_other += cpu.tagged_allocs[tag];
      break;
    }
  }

  char formatted[2048] = {0};
  char *write = formatted;
  size_t remaining = sizeof(formatted);
  bool8_t complete =
      vkr_standard_scene_runtime_memory_text_append(
          &write, &remaining, "CPU allocator (tracked live)\n") &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  Total", cpu.total_allocated) &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  Renderer",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_RENDERER]) &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  Array",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]) &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  String",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]) &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  File",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_FILE]) &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  Vulkan state",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_VULKAN]) &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  Texture temp",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_TEXTURE]) &&
      vkr_standard_scene_runtime_memory_text_append_size(
          &write, &remaining, "  Other", cpu_other) &&
      vkr_standard_scene_runtime_memory_text_append(&write, &remaining,
                                                    "\nGPU device memory\n");

  /* Taken when the last frame completed, so the overlay never waits for the
     frame the render thread is rendering. */
  const VkrDeviceMemoryStats gpu = application->device_memory_seen;
  const bool8_t have_gpu = application->device_memory_seen_valid;
  uint64_t resident_bytes = 0u;
  char ram[64] = "RAM (resident): unavailable";
  char vram[64] = "GPU memory (managed): unavailable";
  if (vkr_platform_get_process_resident_memory(&resident_bytes))
    snprintf(ram, sizeof(ram), "RAM (resident): %.1f MiB",
             (float64_t)resident_bytes / MB(1));
  if (have_gpu) {
    /* Metal charges native heaps, external resources and transfer rings to
       one managed budget. Vulkan heap usage is device-wide; use this
       renderer's committed allocations there instead. */
    const bool8_t metal =
        application->renderer.backend_type == VKR_RENDERER_BACKEND_TYPE_METAL;
    const uint64_t gpu_bytes = metal ? gpu.heap_usage_bytes[0] : gpu.live_bytes;
    snprintf(vram, sizeof(vram), "GPU memory (managed): %s%.1f MiB",
             (metal || gpu.live_totals_exact) ? "" : "~",
             (float64_t)gpu_bytes / MB(1));
  }
  snprintf(state->system_text, sizeof(state->system_text), "%s\n%s\n%s",
           state->hardware_text, ram, vram);
  if (complete && have_gpu) {
    uint64_t logical_live = 0u;
    for (uint32_t owner = 0; owner < VKR_GPU_ALLOCATION_OWNER_COUNT; ++owner)
      logical_live += gpu.owners[owner].live_bytes;
    const uint64_t frame_data =
        gpu.owners[VKR_GPU_ALLOCATION_OWNER_INSTANCE].live_bytes +
        gpu.owners[VKR_GPU_ALLOCATION_OWNER_INDIRECT].live_bytes;
    const uint64_t transfer =
        gpu.owners[VKR_GPU_ALLOCATION_OWNER_STAGING].live_bytes +
        gpu.owners[VKR_GPU_ALLOCATION_OWNER_READBACK].live_bytes;
    const uint64_t shader_target =
        gpu.owners[VKR_GPU_ALLOCATION_OWNER_SHADER].live_bytes +
        gpu.owners[VKR_GPU_ALLOCATION_OWNER_SWAPCHAIN].live_bytes;
    complete =
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Committed", gpu.live_bytes) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Committed peak", gpu.peak_bytes) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Logical live", logical_live) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Mesh",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_MESH].live_bytes) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Texture",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Font",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_FONT].live_bytes) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Render graph",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH].live_bytes) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Frame data", frame_data) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Transfer", transfer) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Shader/target", shader_target) &&
        vkr_standard_scene_runtime_memory_text_append_size(
            &write, &remaining, "  Unknown",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_UNKNOWN].live_bytes);
  } else if (complete) {
    complete = vkr_standard_scene_runtime_memory_text_append(
        &write, &remaining, "  Metrics unavailable\n");
  }

  if (!complete)
    return;
  const uint64_t content_length = (uint64_t)(write - formatted);
  arena_clear(state->stats_arena, ARENA_MEMORY_TAG_STRING);
  uint8_t *content = arena_alloc(state->stats_arena, content_length + 1u,
                                 ARENA_MEMORY_TAG_STRING);
  if (!content)
    return;
  MemCopy(content, formatted, content_length + 1u);
  vkr_standard_scene_runtime_ui_text_set(
      &state->memory_text, (String8){.str = content, .length = content_length});
}

/** Logs a paste-ready static camera block for a harness snapshot case. */
vkr_internal void vkr_standard_scene_runtime_log_camera_snapshot(
    VkrStandardSceneRuntime *application) {
  if (!application) {
    log_warn("Cannot capture camera snapshot without an application");
    return;
  }

  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!camera) {
    log_warn("Cannot capture camera snapshot: active camera is unavailable");
    return;
  }
  if (camera->type != VKR_CAMERA_TYPE_PERSPECTIVE) {
    log_warn("Cannot capture harness camera snapshot from a non-perspective "
             "camera");
    return;
  }

  /* Scene entities and the camera share renderer world space. The harness
     applies this pose after loading the same transformed scene, so applying an
     inverse scene transform here would transform the position twice. */
  log_info("CAMERA_SNAPSHOT scene=\"%s\" coordinate_space=world "
           "resolution=[%u, %u]\n"
           "\"camera\": {\n"
           "  \"mode\": \"static\",\n"
           "  \"position\": [%.9g, %.9g, %.9g],\n"
           "  \"yaw\": %.9g,\n"
           "  \"pitch\": %.9g,\n"
           "  \"vertical_fov_degrees\": %.9g,\n"
           "  \"near_plane\": %.9g,\n"
           "  \"far_plane\": %.9g\n"
           "}",
           (const char *)state->scene_path.str, camera->cached_window_width,
           camera->cached_window_height, camera->position.x, camera->position.y,
           camera->position.z, camera->yaw, camera->pitch, camera->zoom,
           camera->near_clip, camera->far_clip);
}

/* Axis views pan in their image plane and zoom by changing the orthographic
 * span. They never pass through the yaw/pitch controller (top/bottom have
 * vertical forward vectors). */
static void sample_orthographic_input(VkrStandardSceneRuntime *application,
                                      float64_t delta) {
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!camera ||
      vkr_standard_scene_runtime_editor_scene_rendering_stopped(application)) {
    return;
  }
  InputState *input = state->input_state;
  bool8_t captured = vkr_window_is_mouse_captured(&application->host.window);
  if (captured && (input_is_button_up(input, BUTTON_RIGHT) ||
                   input_key_just_pressed(input, KEY_ESCAPE))) {
    vkr_window_set_mouse_capture(&application->host.window, false_v);
    state->free_camera_held = false_v;
    captured = false_v;
  }
  int32_t x = 0;
  int32_t y = 0;
  input_get_mouse_position(input, &x, &y);
  const VkrViewportHitInfo hit =
      vkr_standard_scene_runtime_get_viewport_hit_info(application, x, y);
  const bool8_t hovered = hit.has_target_coords &&
                          !application->ui_capture.mouse &&
                          !application->ui_capture.text &&
                          application->ui_system.mouse_input_layer == 0u;
  int32_t wheel = 0;
  input_get_mouse_wheel(input, &wheel);
  if (wheel != 0 && (hovered || captured)) {
    const float32_t old_height = camera->top_clip - camera->bottom_clip;
    const float32_t new_height = vkr_clamp_f32(
        old_height * expf(-(float32_t)wheel * 0.12f), 0.01f, 100000.0f);
    const float32_t ratio = new_height / old_height;
    camera->left_clip *= ratio;
    camera->right_clip *= ratio;
    camera->bottom_clip *= ratio;
    camera->top_clip *= ratio;
    camera->projection_dirty = true_v;
  }
  /* A headless run has no window to capture; synthetic right clicks still
     reach the context click. */
  if (!captured && hovered &&
      vkr_standard_scene_runtime_is_windowed(application) &&
      input_button_just_pressed(input, BUTTON_RIGHT) &&
      input_is_button_down(input, BUTTON_RIGHT)) {
    vkr_window_set_mouse_capture(&application->host.window, true_v);
    state->free_camera_held = true_v;
    state->scene_keyboard_focus = true_v;
    return;
  }
  if (!captured) {
    return;
  }
  application->ui_system.focused_id = VKR_UI_ID_NONE;
  application->ui_system.focused_is_text = false_v;
  application->ui_capture = (VkrUiInputCapture){0};
  int32_t dx = 0;
  int32_t dy = 0;
  input_get_mouse_delta(input, &dx, &dy);
  VkrViewportMapping mapping = {0};
  if (!vkr_standard_scene_runtime_editor_viewport_mapping(
          application, application->ui_system.target_width,
          application->ui_system.target_height, &mapping)) {
    return;
  }
  const float32_t units_per_pixel = (camera->top_clip - camera->bottom_clip) /
                                    Max(1.0f, mapping.image_rect_px.w);
  float32_t right = -(float32_t)dx * units_per_pixel;
  float32_t up = -(float32_t)dy * units_per_pixel;
  const float32_t step = camera->speed * (float32_t)delta;
  right += ((float32_t)input_is_key_down(input, KEY_D) -
            (float32_t)input_is_key_down(input, KEY_A)) *
           step;
  up += ((float32_t)input_is_key_down(input, KEY_W) -
         (float32_t)input_is_key_down(input, KEY_S)) *
        step;
  vkr_camera_translate(camera, vec3_add(vec3_scale(camera->right, right),
                                        vec3_scale(camera->up, up)));
}

/* Runs the global hotkeys and the UI client input hook. Returns false_v when
   the allocator report cannot open its scope; input handling then stops for
   this frame. */
vkr_internal bool8_t vkr_standard_scene_runtime_handle_hotkeys(
    VkrStandardSceneRuntime *application, InputState *input_state) {
  if (!application->ui_capture.keyboard &&
      input_is_key_up(state->input_state, KEY_M) &&
      input_was_key_down(state->input_state, KEY_M)) {
    VkrAllocatorScope stats_scope =
        vkr_allocator_begin_scope(&application->app_allocator);
    if (!vkr_allocator_scope_is_valid(&stats_scope)) {
      log_error("Failed to create allocator stats scope");
      return false_v;
    }
    char *allocator_stats =
        vkr_allocator_print_global_statistics(&application->app_allocator);
    log_debug("Global allocator stats:\n%s", allocator_stats);
    vkr_allocator_end_scope(&stats_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  if (!application->ui_capture.keyboard &&
      input_is_key_up(state->input_state, KEY_L) &&
      input_was_key_down(state->input_state, KEY_L)) {
    vkr_standard_scene_runtime_init_scene_system(application);
  }

  if (!application->ui_capture.keyboard &&
      input_is_key_up(state->input_state, KEY_U) &&
      input_was_key_down(state->input_state, KEY_U)) {
    if (state->edits.revision != state->edits.saved_revision) {
      snprintf(state->edits.status, sizeof(state->edits.status),
               "Save edits before unloading the scene.");
      log_warn("Save editor overrides before unloading the scene");
    } else {
      sample_warm_hold_release(application, false_v);
      vkr_standard_scene_runtime_unload_scene_system(application);
    }
  }

  if (input_is_key_up(input_state, KEY_F4) &&
      input_was_key_down(input_state, KEY_F4)) {
    uint32_t next_mode =
        (state->filter_mode_index + (uint32_t)ArrayCount(FILTER_MODES) - 1) %
        (uint32_t)ArrayCount(FILTER_MODES);
    vkr_standard_scene_runtime_apply_filter_mode(application, next_mode);
  }

  if (input_is_key_up(input_state, KEY_F5) &&
      input_was_key_down(input_state, KEY_F5)) {
    uint32_t next_mode =
        (state->filter_mode_index + 1) % (uint32_t)ArrayCount(FILTER_MODES);
    vkr_standard_scene_runtime_apply_filter_mode(application, next_mode);
  }

  state->ui.handle_input(state->ui.state, input_state);

  if (input_is_key_up(input_state, KEY_F7) &&
      input_was_key_down(input_state, KEY_F7)) {
    application->metrics->config.pass_gpu_timings =
        !application->metrics->config.pass_gpu_timings;
    log_info("RenderGraph GPU timings %s",
             application->metrics->config.pass_gpu_timings ? "enabled"
                                                           : "disabled");
  }

  if (input_is_key_up(input_state, KEY_F8) &&
      input_was_key_down(input_state, KEY_F8)) {
    state->ibl_validation_mode = (state->ibl_validation_mode + 1u) % 4u;
    log_info("IBL validation mode: %s",
             vkr_standard_scene_runtime_ibl_validation_mode_label(
                 state->ibl_validation_mode));
  }

  if (input_is_key_up(input_state, KEY_F9) &&
      input_was_key_down(input_state, KEY_F9)) {
    state->ibl_validation_scalar =
        Max(0.0f, state->ibl_validation_scalar - 0.1f);
    log_info("IBL validation scalar: %.2f", state->ibl_validation_scalar);
  }

  if (input_is_key_up(input_state, KEY_F10) &&
      input_was_key_down(input_state, KEY_F10)) {
    state->ibl_validation_scalar =
        Min(2.0f, state->ibl_validation_scalar + 0.1f);
    log_info("IBL validation scalar: %.2f", state->ibl_validation_scalar);
  }

  /* In the editor G maximizes the Scene instead. */
  if (!application->editor_viewport.enabled &&
      input_is_key_up(input_state, KEY_G) &&
      input_was_key_down(input_state, KEY_G)) {
    vkr_standard_scene_runtime_log_camera_snapshot(application);
  }
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_handle_gameplay_input(
    VkrStandardSceneRuntime *application, InputState *input_state) {
  if (input_key_just_pressed(input_state, KEY_BACKSPACE)) {
    VkrScene *scene = state->scripts.session.active;
    vkr_scene_physics_set_paused(scene, true_v);
    const char *error = NULL;
    if (!vkr_scene_physics_reset(scene, &error)) {
      log_error("Gameplay reset failed: %s", error ? error : "unknown error");
    } else {
      vkr_scene_physics_set_paused(scene, false_v);
      application->editor_viewport.simulation_running = true_v;
    }
  }
  if (input_key_just_pressed(input_state, KEY_TAB) ||
      input_key_just_pressed(input_state, KEY_ESCAPE)) {
    const bool8_t captured =
        vkr_window_is_mouse_captured(&application->host.window);
    vkr_window_set_mouse_capture(&application->host.window, !captured);
  }
  if (vkr_window_is_mouse_captured(&application->host.window)) {
    application->ui_system.focused_id = VKR_UI_ID_NONE;
    application->ui_system.focused_is_text = false_v;
    application->ui_capture = (VkrUiInputCapture){0};
    state->scene_keyboard_focus = true_v;
  }
}

/* Whether the pointer reaches the editor Scene for the right button: no
   window, menu, popup or toolbar layer covers it and no widget or dock
   gesture holds the mouse. Keyboard focus and a hovered Scene label do not
   block it; the right button gives neither a use. */
vkr_internal bool8_t
sample_scene_pointer_free(const VkrStandardSceneRuntime *application) {
  return application->ui_system.mouse_input_layer == 0u &&
         application->ui_capture.active_id == VKR_UI_ID_NONE &&
         !application->editor_viewport.dock_capture.mouse;
}

/* Starts, toggles or releases free-camera mouse capture. Returns true_v when
   this frame started a capture. */
vkr_internal bool8_t vkr_standard_scene_runtime_update_camera_capture(
    VkrStandardSceneRuntime *application, InputState *input_state) {
  bool8_t camera_captured =
      vkr_window_is_mouse_captured(&application->host.window);
  if (state->free_camera_held &&
      (!camera_captured || input_is_button_up(input_state, BUTTON_RIGHT) ||
       vkr_standard_scene_runtime_editor_scene_rendering_stopped(
           application))) {
    if (camera_captured)
      vkr_window_set_mouse_capture(&application->host.window, false_v);
    state->free_camera_held = false_v;
    camera_captured = false_v;
  }
  bool8_t camera_started = false_v;
  const bool8_t camera_tab =
      input_key_just_pressed(input_state, KEY_TAB) &&
      (camera_captured || (application->editor_viewport.enabled
                               ? state->scene_keyboard_focus
                               : !application->ui_capture.keyboard));
  const bool8_t camera_shortcut =
      application->editor_viewport.enabled && !application->ui_capture.text &&
      application->ui_system.keyboard_input_layer == 0u &&
      input_key_just_pressed(input_state, KEY_F3);
  /* Plain F toggles too, under the editor's rule for plain keys: while the
     Scene or no widget holds the keyboard, and always while flying. */
  const uint8_t f_modifiers = input_key_press_modifiers(input_state, KEY_F) &
                              (VKR_INPUT_MOD_SHIFT | VKR_INPUT_MOD_CONTROL |
                               VKR_INPUT_MOD_ALT | VKR_INPUT_MOD_SUPER);
  const bool8_t camera_f =
      application->editor_viewport.enabled &&
      input_key_just_pressed(input_state, KEY_F) && f_modifiers == 0u &&
      (camera_captured ||
       (!application->ui_capture.text &&
        application->ui_system.keyboard_input_layer == 0u &&
        (state->scene_keyboard_focus ||
         application->ui_system.focused_id == VKR_UI_ID_NONE)));
  if ((camera_tab || camera_shortcut || camera_f) &&
      (camera_captured ||
       !vkr_standard_scene_runtime_editor_scene_rendering_stopped(
           application))) {
    /* A toggle while the right button holds the camera keeps it flying
       after the release; the next toggle ends it. */
    if (!state->free_camera_held) {
      vkr_window_set_mouse_capture(&application->host.window, !camera_captured);
      camera_started = !camera_captured;
    }
    state->free_camera_held = false_v;
    state->free_camera_use_gamepad = false_v;
  }

  if (input_is_button_down(input_state, BUTTON_GAMEPAD_A) &&
      input_was_button_up(input_state, BUTTON_GAMEPAD_A)) {
    bool8_t should_capture =
        !vkr_window_is_mouse_captured(&application->host.window);
    vkr_window_set_mouse_capture(&application->host.window, should_capture);
    state->free_camera_held = false_v;
    camera_started = should_capture;
    if (should_capture) {
      state->free_camera_use_gamepad = !state->free_camera_use_gamepad;
    } else {
      state->free_camera_use_gamepad = false_v;
    }
  }

  /* A right press over the Scene flies whatever held the keyboard: it takes
     focus from fields and from the layer of the last clicked window, menu or
     toolbar, as a left click in the Scene does. */
  if (application->editor_viewport.enabled &&
      vkr_standard_scene_runtime_is_windowed(application) &&
      !vkr_window_is_mouse_captured(&application->host.window) &&
      !vkr_standard_scene_runtime_editor_scene_rendering_stopped(application) &&
      sample_scene_pointer_free(application) && !state->gizmo_drag.active &&
      !state->gizmo_drag.pending_pick &&
      !input_is_key_down(input_state, KEY_ESCAPE) &&
      input_button_just_pressed(input_state, BUTTON_RIGHT) &&
      input_is_button_down(input_state, BUTTON_RIGHT)) {
    int32_t press_x = 0, press_y = 0;
    input_get_button_press_position(input_state, BUTTON_RIGHT, &press_x,
                                    &press_y);
    const VkrViewportHitInfo hit =
        vkr_standard_scene_runtime_get_viewport_hit_info(application, press_x,
                                                         press_y);
    if (hit.has_target_coords) {
      vkr_window_set_mouse_capture(&application->host.window, true_v);
      state->free_camera_held = true_v;
      state->free_camera_use_gamepad = false_v;
      state->scene_keyboard_focus = true_v;
      application->ui_system.focused_id = VKR_UI_ID_NONE;
      application->ui_system.focused_is_text = false_v;
      application->ui_system.keyboard_input_layer = 0u;
      application->ui_capture.keyboard = false_v;
      application->ui_capture.text = false_v;
      camera_started = true_v;
    }
  }

  /* Camera capture owns editor input. A previously focused Inspector button or
     a toolbar under the virtual pointer must not block movement. */
  if (application->editor_viewport.enabled &&
      vkr_window_is_mouse_captured(&application->host.window)) {
    application->ui_system.focused_id = VKR_UI_ID_NONE;
    application->ui_system.focused_is_text = false_v;
    application->ui_capture = (VkrUiInputCapture){0};
    state->scene_keyboard_focus = true_v;
  }

  return camera_started;
}

/* Drives the captured free camera from the keyboard, mouse wheel and mouse
   motion, or from the gamepad sticks. */
vkr_internal void vkr_standard_scene_runtime_apply_free_camera_input(
    VkrStandardSceneRuntime *application, InputState *input_state,
    VkrCamera *camera) {
  VkrCameraController *controller = &application->camera_controller;
  controller->camera = camera;

  bool8_t should_rotate = false_v;
  float32_t yaw_input = 0.0f;
  float32_t pitch_input = 0.0f;

  if (!state->free_camera_use_gamepad) {
    if (input_is_key_down(input_state, KEY_W)) {
      vkr_camera_controller_move_forward(controller, 1.0f);
    }
    if (input_is_key_down(input_state, KEY_S)) {
      vkr_camera_controller_move_forward(controller, -1.0f);
    }
    if (input_is_key_down(input_state, KEY_D)) {
      vkr_camera_controller_move_right(controller, 1.0f);
    }
    if (input_is_key_down(input_state, KEY_A)) {
      vkr_camera_controller_move_right(controller, -1.0f);
    }

    /* The wheel steps the flight speed as it does over the speed chip; the
       lens never changes. */
    int32_t wheel_delta = 0;
    input_get_mouse_wheel(input_state, &wheel_delta);
    if (wheel_delta != 0) {
      camera->speed =
          vkr_sample_camera_speed_step(camera->speed, (float32_t)wheel_delta);
      state->view_state.camera_speed = camera->speed;
    }

    int32_t x = 0;
    int32_t y = 0;
    input_get_mouse_position(input_state, &x, &y);

    int32_t last_x = 0;
    int32_t last_y = 0;
    input_get_previous_mouse_position(input_state, &last_x, &last_y);

    if (!((x == last_x && y == last_y) || (x == 0 && y == 0) ||
          (last_x == 0 && last_y == 0))) {
      float32_t x_offset = (float32_t)(x - last_x);
      float32_t y_offset = (float32_t)(y - last_y);

      x_offset =
          vkr_clamp_f32(x_offset, -VKR_MAX_MOUSE_DELTA, VKR_MAX_MOUSE_DELTA);
      y_offset =
          vkr_clamp_f32(y_offset, -VKR_MAX_MOUSE_DELTA, VKR_MAX_MOUSE_DELTA);

      // Positive screen-space X turns the +yaw camera direction to the right.
      // Captured platform input exposes upward motion as positive virtual Y,
      // so both positive deltas map directly to positive camera rotation
      // unless the user inverts vertical look.
      vkr_camera_controller_look(controller, x_offset,
                                 input_state->invert_look_y ? -y_offset
                                                            : y_offset);
    }
  } else {
    float right_x = 0.0f;
    float right_y = 0.0f;
    input_get_right_stick(input_state, &right_x, &right_y);

    float32_t movement_deadzone = VKR_GAMEPAD_MOVEMENT_DEADZONE;
    if (vkr_abs_f32(right_y) > movement_deadzone) {
      vkr_camera_controller_move_forward(controller, -right_y);
    }
    if (vkr_abs_f32(right_x) > movement_deadzone) {
      vkr_camera_controller_move_right(controller, right_x);
    }

    float left_x = 0.0f;
    float left_y = 0.0f;
    input_get_left_stick(input_state, &left_x, &left_y);

    float rotation_deadzone = 0.1f;
    if (vkr_abs_f32(left_x) < rotation_deadzone) {
      left_x = 0.0f;
    }
    if (vkr_abs_f32(left_y) < rotation_deadzone) {
      left_y = 0.0f;
    }

    if (left_x != 0.0f || left_y != 0.0f) {
      float32_t x_offset = left_x * VKR_GAMEPAD_ROTATION_SCALE;
      float32_t y_offset = -left_y * VKR_GAMEPAD_ROTATION_SCALE;
      yaw_input = -x_offset;
      pitch_input = y_offset;
      should_rotate = true_v;
    }
  }

  if (should_rotate) {
    vkr_camera_controller_rotate(controller, yaw_input, pitch_input);
  }
}

/* A right press in the Scene flies the camera; released within a moment
 * without flying, it is a click that opens the menu of the object under the
 * pointer, as in Unreal. */
static void sample_track_context_click(VkrStandardSceneRuntime *application,
                                       InputState *input) {
  if (!application->editor_viewport.enabled) {
    return;
  }
  if (input_button_just_pressed(input, BUTTON_RIGHT) &&
      input_is_button_down(input, BUTTON_RIGHT)) {
    int32_t x = 0;
    int32_t y = 0;
    input_get_button_press_position(input, BUTTON_RIGHT, &x, &y);
    const VkrViewportHitInfo hit =
        vkr_standard_scene_runtime_get_viewport_hit_info(application, x, y);
    state->context_armed =
        hit.has_target_coords && sample_scene_pointer_free(application) &&
        !vkr_standard_scene_runtime_editor_scene_rendering_stopped(application);
    state->context_press_x = x;
    state->context_press_y = y;
    state->context_press_time = vkr_platform_get_absolute_time();
    state->context_motion = 0;
    return;
  }
  if (!state->context_armed) {
    return;
  }
  int32_t dx = 0;
  int32_t dy = 0;
  input_get_mouse_delta(input, &dx, &dy);
  state->context_motion += abs(dx) + abs(dy);
  static const Keys flight[] = {KEY_W, KEY_A, KEY_S, KEY_D, KEY_Q, KEY_E};
  for (uint32_t i = 0; i < ArrayCount(flight); ++i) {
    if (input_is_key_down(input, flight[i])) {
      state->context_armed = false_v;
      return;
    }
  }
  if (input_button_just_released(input, BUTTON_RIGHT)) {
    state->context_armed = false_v;
    const float32_t reach = 6.0f * application->ui_system.content_scale;
    if ((float32_t)state->context_motion <= reach &&
        vkr_platform_get_absolute_time() - state->context_press_time < 0.45) {
      state->context_click_pending = true_v;
      state->context_purpose = VKR_SAMPLE_PICK_MENU;
    }
  }
}

vkr_internal void
vkr_standard_scene_runtime_handle_input(VkrStandardSceneRuntime *application,
                                        float64_t delta_time) {
  if (state == NULL || state->input_state == NULL) {
    log_error("State or input state is NULL");
    return;
  }

  (void)delta_time;

  InputState *input_state = state->input_state;

  if (!vkr_standard_scene_runtime_handle_hotkeys(application, input_state)) {
    return;
  }

  if (!(vkr_script_host_active(&state->scripts) &&
        application->editor_viewport.simulation_running)) {
    sample_track_context_click(application, input_state);
  }

  if (application->editor_viewport.enabled &&
      state->view_state.camera_view != VKR_SAMPLE_CAMERA_PERSPECTIVE) {
    sample_orthographic_input(application, delta_time);
    return;
  }

  if (vkr_script_host_active(&state->scripts) &&
      application->editor_viewport.simulation_running) {
    vkr_standard_scene_runtime_handle_gameplay_input(application, input_state);
    return;
  }

  const bool8_t camera_started =
      vkr_standard_scene_runtime_update_camera_capture(application,
                                                       input_state);

  /* Capture changes the platform cursor coordinates. Consume motion only after
     the next input snapshot establishes a baseline in captured coordinates. */
  if (camera_started)
    return;

  if (!vkr_window_is_mouse_captured(&application->host.window) ||
      application->ui_capture.mouse || application->ui_capture.keyboard ||
      vkr_standard_scene_runtime_editor_scene_rendering_stopped(application)) {
    return;
  }

  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!camera) {
    return;
  }

  vkr_standard_scene_runtime_apply_free_camera_input(application, input_state,
                                                     camera);
}

vkr_internal void
vkr_standard_scene_runtime_update_fps_text(VkrStandardSceneRuntime *application,
                                           float64_t delta_time) {
  if (!application || !state) {
    return;
  }

  state->fps_accumulated_time += delta_time;
  state->fps_frame_count++;
  vkr_standard_scene_runtime_accumulate_frame_time(application);

  if (vkr_clock_interval_elapsed(&state->fps_update_clock,
                                 VKR_FPS_UPDATE_INTERVAL)) {
    // Average the published frame times collected over this interval. Falling
    // back to the loop's own accumulation keeps the HUD alive when metrics are
    // compiled out or no frame published during the window.
    if (state->hud_frame_samples > 0 &&
        state->hud_frametime_sum / (float64_t)state->hud_frame_samples >
            VKR_FPS_DELTA_MIN) {
      state->current_frametime =
          state->hud_frametime_sum / (float64_t)state->hud_frame_samples;
      state->current_fps = 1.0 / state->current_frametime;
    } else if (state->fps_accumulated_time > VKR_FPS_DELTA_MIN &&
               state->fps_frame_count > 0) {
      state->current_fps =
          (float64_t)state->fps_frame_count / state->fps_accumulated_time;
      state->current_frametime =
          state->fps_accumulated_time / (float64_t)state->fps_frame_count;
    }
    state->hud_frametime_sum = 0.0;
    state->hud_frame_samples = 0;

    VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &application->camera_system, application->active_camera);

    VkrAllocator *frame_alloc = application->frame_allocator;

    // Everything below describes one published frame. Seed from the live
    // structs so a metric the collector could not sample leaves the last known
    // value rather than a zero, then let the adapter overwrite what it has.
    VkrRendererFrameMetrics metrics_snapshot = application->frame_metrics;
    VkrVisibilityStats visibility_snapshot = application->visibility_stats;
    VkrRenderGraphResourceStats rg_stats = {0};
    bool8_t have_rg_stats = false_v;
    VkrMetricsSnapshotView snapshot = {0};
    const bool8_t have_snapshot =
        vkr_metrics_snapshot_acquire(application->metrics, &snapshot);
    if (have_snapshot) {
      have_rg_stats = vkr_renderer_metrics_read_frame(
          &application->renderer_metrics, snapshot.frame, &metrics_snapshot,
          &visibility_snapshot, &rg_stats);
    }

    const VkrRendererFrameMetrics *metrics = &metrics_snapshot;
    const VkrWorldBatchMetrics *world = &metrics->world;
    const VkrShadowMetrics *shadow = &metrics->shadow;
    const VkrRendererMetricsPassTable *pass_table =
        vkr_renderer_metrics_get_pass_table(&application->renderer_metrics);
    const VkrRendererMetricsPassSample *rg_pass_timings =
        pass_table ? pass_table->samples : NULL;
    const uint32_t rg_pass_timing_count = pass_table ? pass_table->count : 0;
    const bool8_t have_rg_timings = rg_pass_timing_count > 0;

    String8 fps_text = string8_create_formatted(
        frame_alloc, "FPS: %.1f\nFrametime: %.2f ms", state->current_fps,
        state->current_frametime * 1000.0);
    if (fps_text.length > 0) {
      vkr_standard_scene_runtime_ui_text_set(&state->fps_text, fps_text);

      String8 left_text = string8_create_formatted(
          frame_alloc, "Pos: x %.2f  y %.2f  z %.2f\nYaw %.2f  Pitch %.2f",
          camera->position.x, camera->position.y, camera->position.z,
          camera->yaw, camera->pitch);
      if (vkr_script_host_active(&state->scripts) &&
          state->script_view.hud[0]) {
        left_text =
            string8_create_formatted(frame_alloc, "%s", state->script_view.hud);
      }
      if (left_text.length > 0) {
        vkr_standard_scene_runtime_ui_text_set(&state->left_text, left_text);
      }

      {
        float64_t rg_image_live_mb = 0.0;
        float64_t rg_image_peak_mb = 0.0;
        float64_t rg_buffer_live_mb = 0.0;
        float64_t rg_buffer_peak_mb = 0.0;
        if (have_rg_stats) {
          rg_image_live_mb =
              (float64_t)rg_stats.live_image_bytes / (1024.0 * 1024.0);
          rg_image_peak_mb =
              (float64_t)rg_stats.peak_image_bytes / (1024.0 * 1024.0);
          rg_buffer_live_mb =
              (float64_t)rg_stats.live_buffer_bytes / (1024.0 * 1024.0);
          rg_buffer_peak_mb =
              (float64_t)rg_stats.peak_buffer_bytes / (1024.0 * 1024.0);
        }
        String8 metrics_text = string8_create_formatted(
            frame_alloc,
            "World draws: %u (opaque %u / transparent %u)\n"
            "Batches: %u (opaque %u)  Calls: %u\n"
            "Draws merged: %u  Indirect: %u\n"
            "Batch avg: %.2f  Batch max: %u\n"
            "RG images: %u (peak %u)  RG buffers: %u (peak %u)\n"
            "RG image MB: %.2f (peak %.2f)  RG buffer MB: %.2f (peak %.2f)\n"
            "Shadow C0 indirect d:%u calls:%u overflow:%u\n"
            "Shadow C1 indirect d:%u calls:%u overflow:%u\n"
            "Shadow C2 indirect d:%u calls:%u overflow:%u\n"
            "Shadow C3 indirect d:%u calls:%u overflow:%u",
            world->draws_collected, world->opaque_draws,
            world->transparent_draws, world->batches_created,
            world->opaque_batches, world->draw_calls_issued,
            world->draws_merged, world->indirect_draws_issued,
            world->avg_batch_size, world->max_batch_size,
            have_rg_stats ? rg_stats.live_image_textures : 0u,
            have_rg_stats ? rg_stats.peak_image_textures : 0u,
            have_rg_stats ? rg_stats.live_buffers : 0u,
            have_rg_stats ? rg_stats.peak_buffers : 0u, rg_image_live_mb,
            rg_image_peak_mb, rg_buffer_live_mb, rg_buffer_peak_mb,
            shadow->shadow_indirect_draws_opaque[0],
            shadow->shadow_indirect_calls_opaque[0],
            shadow->shadow_indirect_overflow[0],
            shadow->shadow_indirect_draws_opaque[1],
            shadow->shadow_indirect_calls_opaque[1],
            shadow->shadow_indirect_overflow[1],
            shadow->shadow_indirect_draws_opaque[2],
            shadow->shadow_indirect_calls_opaque[2],
            shadow->shadow_indirect_overflow[2],
            shadow->shadow_indirect_draws_opaque[3],
            shadow->shadow_indirect_calls_opaque[3],
            shadow->shadow_indirect_overflow[3]);
        if (metrics_text.length > 0 &&
            application->metrics->config.pass_gpu_timings && have_rg_timings &&
            rg_pass_timings && rg_pass_timing_count > 0) {
          String8 timing_header = string8_create_formatted(
              frame_alloc, "\nRG pass timings (cpu/gpu):\n");
          if (timing_header.length > 0) {
            metrics_text =
                string8_concat(frame_alloc, &metrics_text, &timing_header);
          }
          for (uint32_t i = 0; i < rg_pass_timing_count; ++i) {
            const VkrRendererMetricsPassSample *timing = &rg_pass_timings[i];
            if (timing->culled || timing->disabled) {
              continue;
            }
            String8 timing_line = {0};
            if (timing->gpu_valid) {
              timing_line = string8_create_formatted(
                  frame_alloc, "RG pass %.*s: cpu %.3f ms  gpu %.3f ms\n",
                  (int)timing->name_length, timing->name, timing->cpu_ms,
                  timing->gpu_ms);
            } else {
              timing_line = string8_create_formatted(
                  frame_alloc, "RG pass %.*s: cpu %.3f ms  gpu n/a\n",
                  (int)timing->name_length, timing->name, timing->cpu_ms);
            }
            if (timing_line.length > 0) {
              metrics_text =
                  string8_concat(frame_alloc, &metrics_text, &timing_line);
            }
          }
        }
        if (metrics_text.length > 0) {
          vkr_standard_scene_runtime_ui_text_set(&state->metrics_text,
                                                 metrics_text);
        }
      }
    }

    state->fps_accumulated_time = 0.0;
    state->fps_frame_count = 0;
    if (have_snapshot) {
      vkr_metrics_snapshot_release(application->metrics, &snapshot);
    }
  }
}

vkr_internal void
vkr_standard_scene_runtime_init_ui_texts(VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  vkr_standard_scene_runtime_ui_text_set(
      &state->fps_text, string8_lit("FPS: 0.0\nFrametime: 0.0"));
  vkr_standard_scene_runtime_ui_text_set(
      &state->left_text,
      string8_lit("Pos: x 0.0  y 0.0  z 0.0\nYaw 0.0  Pitch 0.0"));

  state->fps_update_clock = vkr_clock_create();
  vkr_clock_start(&state->fps_update_clock);
  state->fps_accumulated_time = 0.0;
  state->fps_frame_count = 0;
  state->current_fps = 0.0;
  state->current_frametime = 0.0;

  vkr_standard_scene_runtime_ui_text_set(&state->picked_object_text,
                                         string8_lit("Picked: none"));
  state->last_picked_object_id = 0;
  vkr_standard_scene_runtime_ui_text_set(
      &state->metrics_text, string8_lit("World batches: 0\nShadow: 0"));

  vkr_standard_scene_runtime_init_memory_text(application);
}

/**
 * @brief Initialize world content state.
 */
vkr_internal void vkr_standard_scene_runtime_init_world_content(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  // Initialize the world text update clock (for scene text updates)
  state->world_text_update_clock = vkr_clock_create();
  vkr_clock_start(&state->world_text_update_clock);
}

/* Sends one entity IO input or switches the trace (VkrSampleIoRequest). */
static void sample_io_request(VkrStandardSceneRuntime *application,
                              const VkrSampleIoRequest *request) {
  if (request->set_trace) {
    vkr_script_host_set_io_trace(&state->scripts, request->trace);
  }
  if (!request->send) {
    return;
  }
  VkrSampleIoResult *result = &state->io_result;
  *result = (VkrSampleIoResult){.token = request->token};
  VkrScene *scene = sample_entity_container(application, request->target, NULL);
  VkrIoEndpoint input = {0};
  VkrIoValue value = {0};
  const VkrIoPort *port = NULL;
  if (!state->scripts.started) {
    snprintf(result->message, sizeof(result->message),
             "Inputs reach entities only while the game plays");
  } else if (!scene ||
             !vkr_io_find_input(
                 scene, request->target,
                 string8_create_from_cstr((const uint8_t *)request->input,
                                          strlen(request->input)),
                 &input) ||
             !(port = vkr_io_endpoint_port(input))) {
    snprintf(result->message, sizeof(result->message),
             "The entity has no input '%s'", request->input);
  } else if (request->value[0] &&
             !vkr_io_parse_value(scene, port->kind, request->value, &value)) {
    snprintf(result->message, sizeof(result->message),
             "'%s' is not a value for '%s'", request->value, port->name);
  } else if (!vkr_script_host_io_send(&state->scripts, request->target, input,
                                      request->value[0] ? &value : NULL)) {
    snprintf(result->message, sizeof(result->message), "%s",
             state->scripts.io.faulted ? state->scripts.io.error
                                       : "The input could not be delivered");
  } else {
    result->ok = true_v;
    snprintf(result->message, sizeof(result->message), "Sent %s", port->name);
  }
}

/* Starts the script session once simulation first runs from a reset
 * boundary. A failure keeps the scene paused and reports why. */
static void sample_scripts_start(VkrStandardSceneRuntime *application) {
  state->script_start_attempted = true_v;
  sample_partition_session(application, true_v);
  const char *error = NULL;
  VkrScene *scene = sample_simulated_scene(application, NULL);
  /* A World played alone steps the shared physics itself. */
  if (scene && scene == application->world_scene) {
    vkr_scene_physics_drive(scene);
  }
  /* The played container and the root World each run their instances. */
  const VkrScriptSessionDesc session = {.active = scene,
                                        .world = application->world_scene,
                                        .input = state->input_state,
                                        .assets = &application->assets,
                                        .jobs = &application->job_system,
                                        .sample_content =
                                            state->gameplay_enabled};
  if (!scene || !vkr_script_host_start(&state->scripts, &session, &error)) {
    application->editor_viewport.simulation_running = false_v;
    snprintf(state->scene_status, sizeof(state->scene_status),
             "Scripts failed to start: %s", error ? error : "unknown error");
    log_error("%s", state->scene_status);
    return;
  }
  if (vkr_script_host_active(&state->scripts) &&
      application->editor_viewport.simulation_running) {
    vkr_window_set_mouse_capture(&application->host.window, true_v);
  }
}

/* Publishes one applied load to the UI. The first in a frame replaces the
 * previous frame's results. */
static void sample_script_result(VkrStandardSceneRuntime *application,
                                 const char *name, VkrScriptReload result,
                                 const char *error, bool8_t *first) {
  if (*first) {
    state->script_result_count = 0u;
    *first = false_v;
  }
  if (state->script_result_count < VKR_SAMPLE_SCRIPT_LOAD_MAX) {
    VkrSampleScriptResult *out =
        &state->script_results[state->script_result_count++];
    *out = (VkrSampleScriptResult){.serial = ++state->script_result_serial,
                                   .result = result};
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->message, sizeof(out->message), "%s", error ? error : "");
  }
  if (result != VKR_SCRIPT_RELOAD_FAILED) {
    /* Scenes already loaded learn the component types of modules the load
       added. */
    VkrScene *world = vkr_scene_handle_get_scene(state->world_handle);
    (void)vkr_scene_sync_world_types(application->active_scene);
    (void)vkr_scene_sync_world_types(world);
    for (uint32_t s = 0; s < VKR_SCENE_ADDITIVE_MAX; ++s) {
      (void)vkr_scene_sync_world_types(
          vkr_scene_handle_get_scene(state->additive_handles[s]));
    }
  }
  if (result == VKR_SCRIPT_RELOAD_FAILED) {
    log_error("Script %s was not loaded: %s", name,
              error ? error : "unknown error");
  } else if (result == VKR_SCRIPT_RELOAD_LOADED) {
    log_info("Script library %s loaded", name);
  } else if (result == VKR_SCRIPT_RELOAD_RESTARTED) {
    log_info("Script %s reloaded; its state changed shape, so the "
             "simulation restarted",
             name);
  }
}

static bool8_t sample_script_prepare_job(VkrJobContext *context,
                                         void *payload) {
  (void)context;
  SampleScriptPending *pending = *(SampleScriptPending **)payload;
  vkr_script_prepare_run(&pending->prepared);
  vkr_atomic_bool_store(&pending->done, true_v, VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

/* Starts preparing `load` on a worker, or prepares it here when no job can
 * be queued; the commit happens at a later frame start either way. */
static void sample_script_prepare(VkrStandardSceneRuntime *application,
                                  SampleScriptPending *pending,
                                  const VkrSampleScriptLoad *load) {
  pending->active = true_v;
  pending->queued = false_v;
  vkr_atomic_bool_store(&pending->done, false_v, VKR_MEMORY_ORDER_RELAXED);
  if (!vkr_script_host_prepare(&state->scripts, &pending->prepared, load->name,
                               load->path, load->project, load->in_place)) {
    vkr_atomic_bool_store(&pending->done, true_v, VKR_MEMORY_ORDER_RELAXED);
    return;
  }
  Bitset8 type_mask = bitset8_create();
  bitset8_set(&type_mask, VKR_JOB_TYPE_GENERAL);
  const VkrJobDesc job = {.priority = VKR_JOB_PRIORITY_NORMAL,
                          .type_mask = type_mask,
                          .run = sample_script_prepare_job,
                          .payload = &pending,
                          .payload_size = sizeof(pending)};
  VkrJobHandle handle = {0};
  if (!vkr_job_try_submit(&application->job_system, &job, &handle)) {
    vkr_script_prepare_run(&pending->prepared);
    vkr_atomic_bool_store(&pending->done, true_v, VKR_MEMORY_ORDER_RELAXED);
  }
}

/* Waits for every prepare still running and closes what it opened, before
 * the libraries retire or the host shuts down. */
static void sample_scripts_join(void) {
  for (uint32_t i = 0; i < VKR_SAMPLE_SCRIPT_LOAD_MAX; ++i) {
    SampleScriptPending *pending = &state->script_pending[i];
    if (!pending->active) {
      continue;
    }
    while (!vkr_atomic_bool_load(&pending->done, VKR_MEMORY_ORDER_ACQUIRE)) {
      vkr_platform_sleep(1);
    }
    vkr_script_host_discard(&state->scripts, &pending->prepared);
    pending->active = false_v;
    pending->queued = false_v;
  }
}

/* A library the host has open: its reloads may prepare on a worker. A first
 * load stays here, since the project's documents load after it this frame
 * and need its component types. */
static bool8_t sample_script_library_open(const char *name) {
  for (uint32_t i = 0; i < state->scripts.library_count; ++i) {
    const VkrScriptLibrary *library = &state->scripts.libraries[i];
    if (!strcmp(library->name, name)) {
      return library->handle.handle != NULL;
    }
  }
  return false_v;
}

/* A reload changed a component's fields: every loaded container, its edit
 * journal and the frame's pending edit move to the new layout (ADR-079). */
static void sample_script_migrate(void *context, const VkrTypeDesc *type,
                                  const VkrTypeDesc *previous) {
  VkrStandardSceneRuntime *application = context;
  VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX] = {application->active_scene,
                                                   application->world_scene};
  VkrSceneEditState *journals[2u + VKR_SCENE_ADDITIVE_MAX] = {
      &state->edits, &state->world_edits};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes[2u + i] = vkr_scene_handle_get_scene(state->additive_handles[i]);
    journals[2u + i] = &state->additive_edits[i];
  }
  bool8_t ok = true_v;
  for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
    if (scenes[i] && !vkr_scene_migrate_world_type(scenes[i], type, previous)) {
      ok = false_v;
    }
    vkr_scene_edit_migrate_type(journals[i], NULL, type, previous);
  }
  vkr_scene_edit_migrate_type(NULL, state->script_frame_edit, type, previous);
  if (ok) {
    log_info("Component %s moved its values to its new fields", type->name);
  } else {
    log_error("Component %s could not move every value to its new fields",
              type->name);
  }
}

/* A scene, World or added scene still loading parsed its documents with the
 * registered layouts; prepared reloads wait until it activates. */
static bool8_t sample_scripts_loads_in_flight(void) {
  bool8_t loading =
      state->world_pending ||
      (state->scene_load_timer_active && !state->scene_load_terminal_logged);
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    loading = loading || state->additive_pending[i];
  }
  return loading;
}

/* Library loads between frames, where no module hook runs (ADR-079). A
 * reload during a session keeps its state or restarts it; retiring ends the
 * session and returns the camera. Reloads of an open library prepare on a
 * worker and commit at a later frame start, so its copy and C runtime
 * startup do not stall frames. */
static void sample_script_request(VkrStandardSceneRuntime *application,
                                  const VkrSampleScriptRequest *request,
                                  VkrSceneEditValues *frame_edit) {
  bool8_t first = true_v;
  state->script_frame_edit = frame_edit;
  if (request->retire_libraries) {
    sample_scripts_join();
    sample_scripts_stop(application);
    vkr_script_host_retire_libraries(&state->scripts);
  }

  /* Prepared reloads commit in request order of their slots. */
  const bool8_t loads_in_flight = sample_scripts_loads_in_flight();
  for (uint32_t i = 0; i < VKR_SAMPLE_SCRIPT_LOAD_MAX; ++i) {
    SampleScriptPending *pending = &state->script_pending[i];
    if (!pending->active || loads_in_flight ||
        !vkr_atomic_bool_load(&pending->done, VKR_MEMORY_ORDER_ACQUIRE)) {
      continue;
    }
    const char *error = NULL;
    const VkrScriptReload result =
        vkr_script_host_commit(&state->scripts, &pending->prepared, &error);
    pending->active = false_v;
    sample_script_result(application, pending->prepared.name, result, error,
                         &first);
    if (pending->queued) {
      const VkrSampleScriptLoad next = pending->next;
      sample_script_prepare(application, pending, &next);
    }
  }

  for (uint32_t i = 0;
       i < request->load_count && i < VKR_SAMPLE_SCRIPT_LOAD_MAX; ++i) {
    const VkrSampleScriptLoad *load = &request->loads[i];
    SampleScriptPending *slot = NULL;
    for (uint32_t p = 0; p < VKR_SAMPLE_SCRIPT_LOAD_MAX && !slot; ++p) {
      SampleScriptPending *pending = &state->script_pending[p];
      if (pending->active && !strcmp(pending->prepared.name, load->name)) {
        /* The newest request replaces any queued one. */
        pending->queued = true_v;
        pending->next = *load;
        slot = pending;
      }
    }
    if (slot) {
      continue;
    }
    if (sample_script_library_open(load->name)) {
      for (uint32_t p = 0; p < VKR_SAMPLE_SCRIPT_LOAD_MAX && !slot; ++p) {
        if (!state->script_pending[p].active) {
          slot = &state->script_pending[p];
        }
      }
    }
    if (slot) {
      sample_script_prepare(application, slot, load);
      continue;
    }
    /* A first load applies now: the documents that follow need its types. */
    static VkrScriptPrepared prepared;
    const char *error = NULL;
    VkrScriptReload result = VKR_SCRIPT_RELOAD_FAILED;
    if (vkr_script_host_prepare(&state->scripts, &prepared, load->name,
                                load->path, load->project, load->in_place)) {
      vkr_script_prepare_run(&prepared);
      result = vkr_script_host_commit(&state->scripts, &prepared, &error);
    } else {
      error = prepared.error;
    }
    sample_script_result(application, load->name, result, error, &first);
  }
}

/* Ends the session and returns the camera to the editing pose. */
static void sample_scripts_stop(VkrStandardSceneRuntime *application) {
  vkr_script_host_stop(&state->scripts);
  state->script_start_attempted = false_v;
  state->script_view = (VkrScriptView){0};
  application->editor_viewport.scripts_own_camera = false_v;
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (camera && state->script_camera_active) {
    vkr_camera_set_pose(camera, state->editor_camera_position,
                        state->editor_camera_yaw, state->editor_camera_pitch);
  }
  state->script_camera_active = false_v;
}

/* Advances the simulated container, running the script session on it. */
static void sample_simulate(VkrStandardSceneRuntime *application,
                            VkrScene *scene, VkrSceneHandle handle,
                            float64_t delta_time) {
  if (!state->scripts.started && !state->script_start_attempted &&
      application->editor_viewport.simulation_running) {
    sample_scripts_start(application);
  }
  if (!scene->simulation.faulted) {
    vkr_scene_physics_set_paused(
        scene, !application->editor_viewport.simulation_running);
  }
  if (!vkr_script_host_active(&state->scripts)) {
    vkr_scene_handle_update_and_sync(handle, &application->assets, delta_time);
    return;
  }
  VkrScriptFrame frame = {
      .now = vkr_platform_get_absolute_time(),
      .scene_delta = delta_time,
      /* A headless run has no window to capture and no user over its
         panels; its synthetic input (Cmd ui.key and ui.look) is the only
         input, so gameplay reads it while the Scene shows perspective. */
      .input_focused =
          !state->modal &&
          state->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE &&
          (vkr_standard_scene_runtime_is_windowed(application)
               ? !application->ui_capture.keyboard &&
                     !application->ui_capture.mouse &&
                     vkr_window_is_mouse_captured(&application->host.window)
               : true_v),
      .simulation_running = application->editor_viewport.simulation_running,
      .camera_available =
          state->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE};
  vkr_script_host_frame(&state->scripts, &frame);
  vkr_scene_handle_update(handle, frame.scene_delta);
  if (scene->physics_paused &&
      application->editor_viewport.simulation_running) {
    application->editor_viewport.simulation_running = false_v;
    vkr_window_set_mouse_capture(&application->host.window, false_v);
    const char *reason = vkr_script_host_error(&state->scripts);
    if (!reason) {
      reason = vkr_scene_physics_error(scene);
    }
    snprintf(state->scene_status, sizeof(state->scene_status),
             "Simulation stopped: %s", reason ? reason : "paused");
    log_error("%s", state->scene_status);
  }
  frame.simulation_running = application->editor_viewport.simulation_running;
  vkr_script_host_present(&state->scripts, &frame, &state->script_view);
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (camera && !frame.simulation_running && state->script_camera_active) {
    vkr_camera_set_pose(camera, state->editor_camera_position,
                        state->editor_camera_yaw, state->editor_camera_pitch);
    state->script_camera_active = false_v;
  }
  if (camera && state->script_view.camera_valid) {
    if (!state->script_camera_active) {
      state->editor_camera_position = camera->position;
      state->editor_camera_yaw = camera->yaw;
      state->editor_camera_pitch = camera->pitch;
      state->script_camera_active = true_v;
    }
    vkr_camera_set_pose(camera, state->script_view.camera_position,
                        state->script_view.camera_yaw_degrees,
                        state->script_view.camera_pitch_degrees);
  }
  application->editor_viewport.scripts_own_camera =
      state->script_view.camera_valid;
  vkr_scene_handle_sync(handle, &application->assets);
}

/* A partitioned primary scene's cell documents sit beside its document:
   `<scene>.scene.json` keeps them in `<scene>.scene.cells/`. */
static void sample_partition_open(VkrScene *scene) {
  char root[1024];
  const char *path = state->scene_path_storage;
  const bool8_t absolute = path[0] == '/' || path[0] == '\\' ||
                           (strlen(path) > 1u && path[1] == ':');
  size_t length = strlen(path);
  if (length > 5u && strcmp(path + length - 5u, ".json") == 0) {
    length -= 5u;
  }
  const int written =
      snprintf(root, sizeof(root), "%s%.*s.cells",
               absolute ? "" : vkr_content_root(), (int)length, path);
  vkr_scene_edit_set_cells_root(
      &state->edits,
      length && written > 0 && (size_t)written < sizeof(root)
          ? string8_create_from_cstr((const uint8_t *)root, strlen(root))
          : (String8){0});
  state->partition_revision = UINT64_MAX;
  state->partition_proxy_stamp = 0u;
  if (!vkr_scene_edit_cells_open(&state->edits, scene)) {
    snprintf(state->scene_status, sizeof(state->scene_status), "%s",
             state->edits.status);
  }
}

/* A session's IO router follows the connections and engine components
   cells brought or took away. */
static void sample_partition_routed(bool8_t changed) {
  if (changed && state->scripts.io.published &&
      !vkr_io_router_refresh(&state->scripts.io)) {
    log_warn("World partition: the IO router kept its earlier routes");
  }
}

/* Unloaded cells within the proxy radius draw the proxy the bakery built
   (`vkr_bakery bake proxies`) on a runtime-only entity at the cell's
   corner; a loaded or distant cell drops it. A cell the bakery left
   without one looks again once the proxies directory changes. */
static void sample_partition_proxies(VkrScene *scene,
                                     const SceneWorldPartition *settings) {
  char directory[1100];
  snprintf(directory, sizeof(directory), "%s/proxies", state->edits.cells_root);
  const FilePath proxies = {.path = string8_create_from_cstr(
                                (const uint8_t *)directory, strlen(directory)),
                            .type = FILE_PATH_TYPE_ABSOLUTE};
  FileStats stats = {0};
  const uint64_t stamp = file_stats(&proxies, &stats) == FILE_ERROR_NONE
                             ? stats.last_modified + 1u
                             : 0u;
  const bool8_t rebaked = stamp != state->partition_proxy_stamp;
  state->partition_proxy_stamp = stamp;
  uint32_t count = 0u;
  vkr_scene_partition_cells(scene, &count);
  for (uint32_t i = 0; i < count; ++i) {
    VkrScenePartitionCellRecord *record = vkr_scene_partition_cell(
        scene, vkr_scene_partition_cells(scene, &count)[i].cell, false_v);
    if (rebaked) {
      record->flags &= ~(uint32_t)VKR_SCENE_PARTITION_CELL_NO_PROXY;
    }
    const bool8_t wanted =
        vkr_scene_partition_proxy_wanted(scene, settings, record);
    if (!wanted && record->proxy.u64) {
      vkr_scene_destroy_entity(scene, record->proxy);
      record->proxy = VKR_ENTITY_ID_INVALID;
    }
    if (!wanted || record->proxy.u64) {
      continue;
    }
    char path[1100];
    snprintf(path, sizeof(path), "%s/proxies/%d_%d/proxy.vkb",
             state->edits.cells_root, record->cell.x, record->cell.z);
    FILE *file = file_fopen(path, "rb");
    if (!file) {
      record->flags |= VKR_SCENE_PARTITION_CELL_NO_PROXY;
      continue;
    }
    fclose(file);
    const VkrEntityId proxy = vkr_scene_create_entity(scene, NULL);
    const char *error = NULL;
    const VkrSceneModelDesc desc = {.mesh_path = string8_create_from_cstr(
                                        (const uint8_t *)path, strlen(path))};
    char name[64];
    snprintf(name, sizeof(name), "Proxy %d,%d", record->cell.x, record->cell.z);
    const bool8_t ok =
        proxy.u64 && vkr_scene_set_transient(scene, proxy) &&
        vkr_scene_set_name(
            scene, proxy,
            string8_create_from_cstr((const uint8_t *)name, strlen(name))) &&
        vkr_scene_set_transform(
            scene, proxy,
            vec3_sub(vec3_new((float32_t)record->cell.x * settings->cell_size,
                              0.0f,
                              (float32_t)record->cell.z * settings->cell_size),
                     scene->origin_offset),
            vkr_quat_identity(), vec3_one()) &&
        vkr_scene_request_model(scene, scene->assets, proxy, &desc, &error);
    if (!ok) {
      log_warn("World partition: the proxy of cell %d,%d failed: %s",
               record->cell.x, record->cell.z, error ? error : "no entity");
      if (proxy.u64) {
        vkr_scene_destroy_entity(scene, proxy);
      }
      record->flags |= VKR_SCENE_PARTITION_CELL_NO_PROXY;
      continue;
    }
    record->proxy = proxy;
  }
}

/* Streams the primary scene's cells around its sources: cells that left
   unload when no edit needs them, then the nearest missing ones load within
   the frame budget. */
static void sample_partition_update(VkrStandardSceneRuntime *application) {
  VkrScene *scene = application->active_scene;
  SceneWorldPartition settings;
  if (!scene || !state->scene_resource.as.scene ||
      !vkr_scene_partition_settings(scene, &settings)) {
    return;
  }
  VkrSceneEditState *edits = &state->edits;
  /* Objects moved into a cell bring its document in first. */
  if (state->partition_revision != edits->revision ||
      state->partition_structure != scene->structure_revision) {
    if (!vkr_scene_edit_cells_track(edits, scene)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               edits->status);
    }
    state->partition_revision = edits->revision;
    state->partition_structure = scene->structure_revision;
  }
  VkrScenePartitionPlan plan;
  vkr_scene_partition_plan(scene, &settings, &plan);
  if (!plan.load_count && !plan.unload_count) {
    sample_partition_proxies(scene, &settings);
    state->partition_structure = scene->structure_revision;
    return;
  }
  const float64_t start = vkr_platform_get_absolute_time();
  const float64_t budget = VKR_SAMPLE_PARTITION_BUDGET_MS / 1000.0;
  const uint64_t structure = scene->structure_revision;
  for (uint32_t i = 0; i < plan.unload_count; ++i) {
    if (vkr_scene_edit_cell_unloadable(edits, scene, plan.unload[i])) {
      vkr_scene_edit_cell_unload(edits, scene, plan.unload[i]);
    }
  }
  /* One load always runs, so a slow frame still makes progress. */
  for (uint32_t i = 0; i < plan.load_count; ++i) {
    if (i && vkr_platform_get_absolute_time() - start > budget) {
      break;
    }
    if (!vkr_scene_edit_cell_load(edits, scene, plan.load[i])) {
      /* An unreadable document stays unloaded, and listed, rather than
         retried each frame; the status says which. */
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               edits->status);
    }
  }
  sample_partition_routed(structure != scene->structure_revision);
  sample_partition_proxies(scene, &settings);
  state->partition_structure = scene->structure_revision;
}

/* Cells a designer or agent asked for (partition.load, the World
   Partition window): loading pins them, so they stay for editing whatever
   the camera does; unloading unpins them and unloads those no edit needs.
   The outcome lands in the edit status. */
static void sample_partition_request(VkrScene *scene,
                                     const VkrSceneEditRequest *request) {
  VkrSceneEditState *edits = &state->edits;
  SceneWorldPartition settings;
  if (!vkr_scene_partition_settings(scene, &settings)) {
    snprintf(edits->status, sizeof(edits->status),
             "The scene has no world partition.");
    return;
  }
  const int32_t *range = request->partition_cells;
  const uint64_t structure = scene->structure_revision;
  uint32_t changed = 0u;
  uint32_t kept = 0u;
  if (!request->partition_unload) {
    const int64_t area =
        ((int64_t)range[2] - range[0] + 1) * ((int64_t)range[3] - range[1] + 1);
    if (range[2] < range[0] || range[3] < range[1] ||
        area > VKR_SAMPLE_PARTITION_REQUEST_MAX) {
      snprintf(edits->status, sizeof(edits->status),
               "Load at most %u cells at once.",
               VKR_SAMPLE_PARTITION_REQUEST_MAX);
      return;
    }
    for (int32_t z = range[1]; z <= range[3]; ++z) {
      for (int32_t x = range[0]; x <= range[2]; ++x) {
        const VkrScenePartitionCell cell = {x, z};
        if (vkr_scene_edit_cell_load(edits, scene, cell)) {
          VkrScenePartitionCellRecord *record =
              vkr_scene_partition_cell(scene, cell, false_v);
          record->flags |= VKR_SCENE_PARTITION_CELL_PINNED;
          changed++;
        } else {
          kept++;
        }
      }
    }
    snprintf(edits->status, sizeof(edits->status),
             "Loaded %u cells for editing%s.", changed,
             kept ? "; some documents are unreadable" : "");
  } else {
    uint32_t count = 0u;
    vkr_scene_partition_cells(scene, &count);
    for (uint32_t i = 0; i < count; ++i) {
      VkrScenePartitionCellRecord *record = vkr_scene_partition_cell(
          scene, vkr_scene_partition_cells(scene, &count)[i].cell, false_v);
      const VkrScenePartitionCell cell = record->cell;
      if (!(record->flags & VKR_SCENE_PARTITION_CELL_LOADED) ||
          (!request->partition_all &&
           (cell.x < range[0] || cell.x > range[2] || cell.z < range[1] ||
            cell.z > range[3]))) {
        continue;
      }
      record->flags &= ~(uint32_t)VKR_SCENE_PARTITION_CELL_PINNED;
      if (vkr_scene_edit_cell_unloadable(edits, scene, cell)) {
        vkr_scene_edit_cell_unload(edits, scene, cell);
        changed++;
      } else {
        kept++;
      }
    }
    snprintf(edits->status, sizeof(edits->status),
             "Unloaded %u cells; %u keep unsaved or undoable edits.", changed,
             kept);
  }
  sample_partition_routed(structure != scene->structure_revision);
}

/* Play keeps the cells it starts with while the scene has unsaved edits, so
   Reset finds them as they were; a saved scene streams freely and Reset
   brings its starting cells back from their documents. */
static void sample_partition_session(VkrStandardSceneRuntime *application,
                                     bool8_t begin) {
  VkrScene *scene = application->active_scene;
  SceneWorldPartition settings;
  if (!scene || !vkr_scene_partition_settings(scene, &settings)) {
    return;
  }
  const bool8_t dirty = state->edits.revision != state->edits.saved_revision;
  const uint64_t structure = scene->structure_revision;
  uint32_t count = 0u;
  vkr_scene_partition_cells(scene, &count);
  for (uint32_t i = 0; i < count; ++i) {
    VkrScenePartitionCellRecord *record = vkr_scene_partition_cell(
        scene, vkr_scene_partition_cells(scene, &count)[i].cell, false_v);
    const bool8_t loaded = record->flags & VKR_SCENE_PARTITION_CELL_LOADED;
    if (begin) {
      record->flags &= ~(uint32_t)(VKR_SCENE_PARTITION_CELL_HELD |
                                   VKR_SCENE_PARTITION_CELL_BEFORE_PLAY);
      if (loaded) {
        record->flags |= VKR_SCENE_PARTITION_CELL_BEFORE_PLAY |
                         (dirty ? VKR_SCENE_PARTITION_CELL_HELD : 0u);
      }
      continue;
    }
    const bool8_t before = record->flags & VKR_SCENE_PARTITION_CELL_BEFORE_PLAY;
    record->flags &= ~(uint32_t)(VKR_SCENE_PARTITION_CELL_HELD |
                                 VKR_SCENE_PARTITION_CELL_BEFORE_PLAY);
    if (loaded && !before &&
        vkr_scene_edit_cell_unloadable(&state->edits, scene, record->cell)) {
      vkr_scene_edit_cell_unload(&state->edits, scene, record->cell);
    } else if (!loaded && before) {
      (void)vkr_scene_edit_cell_load(&state->edits, scene, record->cell);
    }
  }
  sample_partition_routed(structure != scene->structure_revision);
}

/* The loaded containers: the open scene, the root World and the added
   scenes. */
static uint32_t sample_containers(VkrStandardSceneRuntime *application,
                                  VkrScene **out) {
  uint32_t count = 0u;
  VkrScene *candidates[2u + VKR_SCENE_ADDITIVE_MAX] = {
      application->active_scene,
      vkr_scene_handle_get_scene(state->world_handle)};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    candidates[2u + i] = vkr_scene_handle_get_scene(state->additive_handles[i]);
  }
  for (uint32_t i = 0; i < ArrayCount(candidates); ++i) {
    bool8_t seen = !candidates[i];
    for (uint32_t j = 0; j < count && !seen; ++j) {
      seen = out[j] == candidates[i];
    }
    if (!seen) {
      out[count++] = candidates[i];
    }
  }
  return count;
}

/* Whether a container's origin is rebased, as during a long Play. */
static bool8_t sample_origin_rebased(VkrStandardSceneRuntime *application) {
  VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX];
  const uint32_t count = sample_containers(application, scenes);
  for (uint32_t i = 0; i < count; ++i) {
    const Vec3 offset = scenes[i]->origin_offset;
    if (offset.x != 0.0f || offset.y != 0.0f || offset.z != 0.0f) {
      return true_v;
    }
  }
  return false_v;
}

/* Moves every container, its physics and the cameras by -`shift`. */
static void sample_rebase_shift(VkrStandardSceneRuntime *application,
                                Vec3 shift) {
  VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX];
  const uint32_t count = sample_containers(application, scenes);
  for (uint32_t i = 0; i < count; ++i) {
    if (!vkr_scene_shift_origin(scenes[i], shift)) {
      log_error("Origin rebase: a container could not move");
    }
  }
  const char *error = NULL;
  if (!vkr_scene_physics_shift(scenes, count, shift, &error)) {
    log_error("Origin rebase: physics did not move: %s",
              error ? error : "unknown error");
  }
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (camera) {
    vkr_camera_set_pose(camera, vec3_sub(camera->position, shift), camera->yaw,
                        camera->pitch);
  }
  state->editor_camera_position =
      vec3_sub(state->editor_camera_position, shift);
}

/* During Play, a camera past the rebase distance moves the world back so
   positions near it stay small (ADR-086). */
static void sample_rebase(VkrStandardSceneRuntime *application) {
  const VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!camera || !application->active_scene ||
      !application->editor_viewport.simulation_running ||
      (fabsf(camera->position.x) <= VKR_SAMPLE_REBASE_DISTANCE &&
       fabsf(camera->position.z) <= VKR_SAMPLE_REBASE_DISTANCE)) {
    return;
  }
  const Vec3 shift =
      vec3_new(roundf(camera->position.x / VKR_SAMPLE_REBASE_STEP) *
                   VKR_SAMPLE_REBASE_STEP,
               0.0f,
               roundf(camera->position.z / VKR_SAMPLE_REBASE_STEP) *
                   VKR_SAMPLE_REBASE_STEP);
  sample_rebase_shift(application, shift);
  const Vec3 offset = application->active_scene->origin_offset;
  log_info("Origin rebased by %.0f, %.0f m; the origin is now at %.0f, %.0f",
           (float64_t)shift.x, (float64_t)shift.z, (float64_t)offset.x,
           (float64_t)offset.z);
}

/* Reset returns every container to its document positions exactly, and
   physics and the cameras with them, before physics resets. */
static void sample_rebase_restore(VkrStandardSceneRuntime *application) {
  VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX];
  const uint32_t count = sample_containers(application, scenes);
  Vec3 offset = vec3_zero();
  for (uint32_t i = 0; i < count; ++i) {
    const Vec3 undone = vkr_scene_restore_origin(scenes[i]);
    if (undone.x != 0.0f || undone.y != 0.0f || undone.z != 0.0f) {
      offset = undone;
    }
  }
  if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) {
    return;
  }
  const char *error = NULL;
  if (!vkr_scene_physics_shift(scenes, count, vec3_scale(offset, -1.0f),
                               &error)) {
    log_error("Origin restore: physics did not move: %s",
              error ? error : "unknown error");
  }
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (camera) {
    vkr_camera_set_pose(camera, vec3_add(camera->position, offset), camera->yaw,
                        camera->pitch);
  }
  state->editor_camera_position =
      vec3_add(state->editor_camera_position, offset);
}

/* Every loaded container streams around the camera that draws the
   viewport, the editor's or a game's. */
static void sample_stream_sources(VkrStandardSceneRuntime *application) {
  const VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  const uint32_t count = camera ? 1u : 0u;
  const Vec3 source = camera ? camera->position : vec3_zero();
  VkrSceneHandle handles[2u + VKR_SCENE_ADDITIVE_MAX] = {
      state->scene_resource.as.scene, state->world_handle};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    handles[2u + i] = state->additive_handles[i];
  }
  for (uint32_t i = 0; i < ArrayCount(handles); ++i) {
    vkr_scene_set_stream_sources(vkr_scene_handle_get_scene(handles[i]),
                                 &source, count);
  }
}

/**
 * @brief Update scene system each frame.
 */
vkr_internal void
vkr_standard_scene_runtime_update_scene(VkrStandardSceneRuntime *application,
                                        float64_t delta_time) {
  if (!application || !state) {
    return;
  }

  (void)vkr_standard_scene_runtime_try_activate_scene_resource(application);
  sample_sync_texture_limit(application);
  sample_rebase(application);
  sample_stream_sources(application);
  sample_partition_update(application);
  VkrSceneHandle simulated_handle = NULL;
  VkrScene *simulated = sample_simulated_scene(application, &simulated_handle);
  if (simulated) {
    sample_simulate(application, simulated, simulated_handle, delta_time);
  }
  sample_world_update(application, simulated_handle != state->world_handle);
  /* Additive scenes render beside the active scene; their simulation and
     physics stay paused (ADR-076). */
  sample_additive_poll(application);
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (state->additive_handles[i]) {
      vkr_scene_handle_update_and_sync(state->additive_handles[i],
                                       &application->assets, 0.0);
    }
  }
  /* During a session the IO router drains sensor events after each tick
     (vkr_io_router.h); otherwise they are discarded here so the queue never
     fills. */
  if (!state->scripts.io.published &&
      !vkr_scene_physics_sensor_events(application->active_scene,
                                       state->physics_sensor_events,
                                       ArrayCount(state->physics_sensor_events),
                                       &state->physics_sensor_event_count)) {
    snprintf(state->edits.status, sizeof(state->edits.status),
             "Physics sensor event drain failed: %s",
             vkr_scene_physics_error(application->active_scene));
  }

  /* The outline follows the selection even where the gizmo cannot edit. */
  application->selection_outline_entity =
      state->has_selection ? state->selected_entity : VKR_ENTITY_ID_INVALID;
  application->selection_outline_scene =
      state->has_selection
          ? sample_entity_container(application, state->selected_entity, NULL)
          : NULL;
  if (application->gizmo_system.initialized) {
    if (!state->has_selection) {
      vkr_gizmo_system_clear_target(&application->gizmo_system);
      return;
    }

    VkrScene *scene =
        sample_entity_container(application, state->selected_entity, NULL);
    SceneTransform *transform =
        scene ? vkr_scene_get_transform(scene, state->selected_entity) : NULL;
    if (!transform) {
      /* Unplaced entities such as fog or the sky stay selected for Details;
         only a destroyed entity loses the selection. */
      const bool8_t alive =
          scene && vkr_scene_entity_alive(scene, state->selected_entity);
      if (!alive) {
        vkr_standard_scene_runtime_clear_gizmo_selection(application);
      } else {
        vkr_gizmo_system_clear_target(&application->gizmo_system);
      }
      return;
    }

    if (vkr_scene_physics_owner(scene, state->selected_entity).u64 &&
        !vkr_scene_physics_is_paused(scene)) {
      vkr_gizmo_system_clear_target(&application->gizmo_system);
      return;
    }
    Mat4 parent_inverse;
    VkrQuat parent_rotation;
    if (!vkr_standard_scene_runtime_gizmo_parent_frame(
            scene, transform, &parent_inverse, &parent_rotation)) {
      vkr_gizmo_system_clear_target(&application->gizmo_system);
      return;
    }
    Vec3 world_position = mat4_position(transform->world);
    SceneText3D *text = vkr_scene_get_text3d(scene, state->selected_entity);
    if (text) {
      Vec3 pivot_local = vec3_zero();
      if (vkr_standard_scene_runtime_text_pivot_local(text, &pivot_local)) {
        world_position =
            vkr_standard_scene_runtime_text_pivot_world(transform, pivot_local);
      }
    }

    /* Local and scale handles follow the object's world rotation. */
    vkr_gizmo_system_set_target(
        &application->gizmo_system, state->selected_entity, world_position,
        vkr_quat_normalize(vkr_quat_mul(parent_rotation, transform->rotation)));
  }
}

vkr_internal void vkr_standard_scene_runtime_finish_gizmo_edit(
    VkrStandardSceneRuntime *application) {
  if (state->gizmo_edit_pending) {
    VkrSceneEditState *edits = NULL;
    VkrScene *scene =
        sample_entity_container(application, state->gizmo_edit_entity, &edits);
    VkrSceneEditValues after;
    if (scene && vkr_scene_edit_read(scene, state->gizmo_edit_entity, &after)) {
      after.fields = state->gizmo_before.fields;
      const bool8_t changed =
          (after.fields & VKR_SCENE_EDIT_PHYSICS)
              ? MemCompare(&state->gizmo_before.physics, &after.physics,
                           sizeof(after.physics)) != 0
              : !vec3_equal(state->gizmo_before.position, after.position,
                            0.0f) ||
                    !vec3_equal(state->gizmo_before.scale, after.scale, 0.0f) ||
                    MemCompare(&state->gizmo_before.rotation, &after.rotation,
                               sizeof(after.rotation)) != 0;
      const uint64_t group = changed && state->gizmo_drag.companion_count
                                 ? vkr_scene_edit_group_begin(edits)
                                 : 0u;
      const bool8_t grouped = group != 0u;
      bool8_t recorded = !changed || vkr_scene_edit_record_external(
                                         edits, scene, state->gizmo_edit_entity,
                                         &state->gizmo_before, &after);
      VkrSampleGizmoEdit report = {.count = 1u,
                                   .fields = after.fields,
                                   .group = group,
                                   .entities = {state->gizmo_edit_entity}};
      for (uint32_t i = 0;
           changed && recorded && i < state->gizmo_drag.companion_count; ++i) {
        /* A companion's values before the move differ only in position. */
        const VkrEntityId companion = state->gizmo_drag.companions[i];
        VkrSceneEditValues moved;
        if (!vkr_scene_edit_read(scene, companion, &moved)) {
          continue;
        }
        moved.fields = VKR_SCENE_EDIT_TRANSFORM;
        VkrSceneEditValues before = moved;
        before.position = state->gizmo_drag.companion_start[i];
        recorded = vkr_scene_edit_record_external(edits, scene, companion,
                                                  &before, &moved);
        report.entities[report.count++] = companion;
      }
      if (grouped) {
        if (recorded) {
          vkr_scene_edit_group_end(edits);
        } else {
          (void)vkr_scene_edit_group_rollback(edits, scene);
        }
      }
      /* A collaborative session shares the drag the journal now holds. */
      if (changed && recorded) {
        state->gizmo_recorded = report;
      }
      if (!recorded &&
          !vkr_standard_scene_runtime_restore_gizmo_edit(application)) {
        state->gizmo_drag.active = false_v;
        vkr_standard_scene_runtime_clear_gizmo_handles(application);
        return;
      }
    }
    state->gizmo_edit_pending = false_v;
  }
  state->gizmo_drag.active = false_v;
  state->gizmo_drag.handle = VKR_GIZMO_HANDLE_NONE;
  vkr_standard_scene_runtime_clear_gizmo_handles(application);
}

vkr_internal void vkr_standard_scene_runtime_capture_gizmo_release(
    const VkrViewportHitInfo *viewport_info) {
  state->gizmo_drag.released = true_v;
  state->gizmo_drag.release_has_target_coords =
      viewport_info->has_target_coords;
  state->gizmo_drag.release_position = viewport_info->position;
}

/* A right click, or a UI pick request, picks like a left click that
   selects, then answers through the frame's context fields. It replaces a
   hover pick in flight. */
static void sample_request_context_pick(VkrStandardSceneRuntime *application,
                                        VkrPickingContext *picking) {
  if (state->gizmo_hover_pending) {
    vkr_picking_cancel(picking);
    state->gizmo_hover_pending = false_v;
  }
  const VkrViewportHitInfo press_info =
      vkr_standard_scene_runtime_get_viewport_hit_info(
          application, state->context_press_x, state->context_press_y);
  state->context_click_pending = false_v;
  if (!vkr_picking_is_pending(picking) &&
      vkr_standard_scene_runtime_request_picking(application, picking,
                                                 &press_info)) {
    state->gizmo_drag.pending_pick = true_v;
    state->gizmo_drag.pending_select = true_v;
    state->gizmo_drag.pick_position = press_info.position;
    state->gizmo_drag.released = false_v;
    state->context_pick = true_v;
    state->context_pick_purpose = state->context_purpose;
  }
}

/* With the Select tool, pressing an object and dragging moves it in the view
   plane, as the gizmo's center handle would; a click only selects, and an
   unmoved drag records nothing. */
static void sample_begin_select_drag(VkrStandardSceneRuntime *application,
                                     const VkrViewportHitInfo *viewport_info) {
  if (application->gizmo_system.tool != VKR_GIZMO_MODE_NONE ||
      !input_is_button_down(state->input_state, BUTTON_LEFT) ||
      !vkr_standard_scene_runtime_begin_gizmo_drag(
          application, VKR_GIZMO_HANDLE_TRANSLATE_FREE)) {
    return;
  }
  application->gizmo_system.mode = VKR_GIZMO_MODE_TRANSLATE;
  vkr_standard_scene_runtime_update_gizmo_drag(application, viewport_info);
}

/* Seconds a grid fit waits for its GPU pick before giving up, so a Scene that
   stops rendering cannot hold the picking context. */
#define SAMPLE_GRID_FIT_TIMEOUT_SECONDS 2.0

/* Lifts the ground grid onto the surface at a window pixel. The nearest
   collision of the scene and its World answers at once; without one, a GPU
   pick reads the pixel's opaque depth (sample_grid_fit_update), so scenes
   without collision such as Bistro fit too. */
static void sample_grid_fit_begin(VkrStandardSceneRuntime *application,
                                  Vec2 position_px) {
  state->grid_status[0] = '\0';
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  const VkrViewportHitInfo info =
      vkr_standard_scene_runtime_get_viewport_hit_info(
          application, (int32_t)position_px.x, (int32_t)position_px.y);
  Vec3 origin = vec3_zero();
  Vec3 direction = vec3_zero();
  if (!camera || !info.has_target_coords ||
      !vkr_standard_scene_runtime_build_view_ray(camera, info.position, &origin,
                                                 &direction)) {
    snprintf(state->grid_status, sizeof(state->grid_status),
             "The Scene view is not available");
    log_info("Grid fit: %s", state->grid_status);
    return;
  }

  VkrScene *scenes[] = {application->active_scene,
                        vkr_scene_handle_get_scene(state->world_handle)};
  bool8_t found = false_v;
  float32_t nearest = 0.0f;
  float32_t height = 0.0f;
  for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
    VkrPhysicsRayHit hit = {0};
    if (!scenes[i] ||
        !vkr_scene_physics_raycast(
            scenes[i], origin, vec3_scale(direction, camera->far_clip), &hit)) {
      continue;
    }
    const Vec3 point =
        vec3_new(hit.position[0], hit.position[1], hit.position[2]);
    const float32_t distance = vec3_length(vec3_sub(point, origin));
    if (!found || distance < nearest) {
      found = true_v;
      nearest = distance;
      height = point.y;
    }
  }
  if (found) {
    state->view_state.grid_height = vkr_clamp_f32(height, -10000.0f, 10000.0f);
    snprintf(state->grid_status, sizeof(state->grid_status),
             "Grid at %.2f m from collision", (double)height);
    log_info("Grid fit: %s", state->grid_status);
    return;
  }

  VkrPickingContext *picking = &application->picking;
  if (!picking->initialized || state->gizmo_drag.pending_pick ||
      state->grid_fit_pending) {
    snprintf(state->grid_status, sizeof(state->grid_status),
             "Picking is busy; try again");
    log_info("Grid fit: %s", state->grid_status);
    return;
  }
  /* A hover pick only refreshes a highlight; the fit takes its place. */
  if (vkr_picking_is_pending(picking)) {
    vkr_picking_cancel(picking);
  }
  state->gizmo_hover_pending = false_v;
  if (picking->width != info.target_width ||
      picking->height != info.target_height) {
    vkr_picking_resize(picking, info.target_width, info.target_height);
  }
  if (info.target_x >= picking->width || info.target_y >= picking->height) {
    snprintf(state->grid_status, sizeof(state->grid_status),
             "The Scene view is not available");
    log_info("Grid fit: %s", state->grid_status);
    return;
  }
  vkr_camera_system_update(camera);
  state->grid_fit_inverse_view_projection =
      mat4_inverse(mat4_mul(vkr_camera_system_get_projection_matrix(camera),
                            vkr_camera_system_get_view_matrix(camera)));
  state->grid_fit_ndc =
      vec2_new(info.position.x * 2.0f - 1.0f, info.position.y * 2.0f - 1.0f);
  state->grid_fit_started = vkr_platform_get_absolute_time();
  vkr_picking_request(picking, info.target_x, info.target_y);
  state->grid_fit_pending = true_v;
  snprintf(state->grid_status, sizeof(state->grid_status),
           "Reading the surface under the view");
  log_info("Grid fit: %s", state->grid_status);
}

/* Consumes the grid fit's GPU pick. Returns true while the fit owns the
   picking context, so selection and hover picks wait for it. */
static bool8_t sample_grid_fit_update(VkrStandardSceneRuntime *application) {
  if (!state->grid_fit_pending) {
    return false_v;
  }
  VkrPickingContext *picking = &application->picking;
  const VkrPickResult result =
      vkr_picking_get_result(&application->renderer, picking);
  if (vkr_picking_is_pending(picking)) {
    if (vkr_platform_get_absolute_time() - state->grid_fit_started <
        SAMPLE_GRID_FIT_TIMEOUT_SECONDS) {
      return true_v;
    }
    vkr_picking_cancel(picking);
    state->grid_fit_pending = false_v;
    snprintf(state->grid_status, sizeof(state->grid_status),
             "The surface read timed out; try again");
    log_info("Grid fit: %s", state->grid_status);
    return true_v;
  }
  state->grid_fit_pending = false_v;
  /* Depth 1 is the far plane: the pixel shows only sky. */
  if (!result.has_depth || !(result.depth >= 0.0f && result.depth < 1.0f)) {
    snprintf(state->grid_status, sizeof(state->grid_status),
             "No surface under the view centre");
    log_info("Grid fit: %s", state->grid_status);
    return true_v;
  }
  const Vec4 world =
      mat4_mul_vec4(state->grid_fit_inverse_view_projection,
                    vec4_new(state->grid_fit_ndc.x, state->grid_fit_ndc.y,
                             result.depth, 1.0f));
  const float32_t height =
      vkr_abs_f32(world.w) > VKR_FLOAT_EPSILON ? world.y / world.w : NAN;
  if (!isfinite(height)) {
    snprintf(state->grid_status, sizeof(state->grid_status),
             "No surface under the view centre");
    log_info("Grid fit: %s", state->grid_status);
    return true_v;
  }
  state->view_state.grid_height = vkr_clamp_f32(height, -10000.0f, 10000.0f);
  snprintf(state->grid_status, sizeof(state->grid_status),
           "Grid at %.2f m from the visible surface", (double)height);
  log_info("Grid fit: %s", state->grid_status);
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_update_picking(
    VkrStandardSceneRuntime *application) {
  if (!application || !state || !state->input_state) {
    return;
  }

  VkrPickingContext *picking = &application->picking;
  if (!picking->initialized) {
    return;
  }
  /* Before the UI-capture cancel below: the fit is asked from a menu. */
  if (sample_grid_fit_update(application)) {
    return;
  }

  if ((state->gizmo_drag.pending_pick || state->gizmo_hover_pending) &&
      (state->pick_scene_generation != application->scene_generation ||
       state->pick_selected_entity.u64 != state->selected_entity.u64))
    vkr_standard_scene_runtime_cancel_gizmo_pick(application);

  if (vkr_standard_scene_runtime_editor_scene_rendering_stopped(application) ||
      input_is_key_down(state->input_state, KEY_ESCAPE) ||
      vkr_window_is_mouse_captured(&application->host.window) ||
      (application->ui_capture.mouse && !state->gizmo_drag.active &&
       !state->gizmo_drag.pending_pick)) {
    if (vkr_picking_is_pending(picking))
      vkr_picking_cancel(picking);
    state->gizmo_hover_pending = false_v;
    state->gizmo_drag.pending_pick = false_v;
    state->gizmo_drag.pending_select = false_v;
    state->context_pick = false_v;
    return;
  }

  bool8_t left_down = input_is_button_down(state->input_state, BUTTON_LEFT);
  bool8_t right_down = input_is_button_down(state->input_state, BUTTON_RIGHT);
  bool8_t middle_down = input_is_button_down(state->input_state, BUTTON_MIDDLE);
  bool8_t left_pressed =
      input_button_just_pressed(state->input_state, BUTTON_LEFT);
  bool8_t click_pressed = left_pressed;
  /* In the editor, Ctrl+click (Cmd on macOS) picks without selecting and
     answers the UI's selection toggle (VKR_SAMPLE_PICK_SELECT_TOGGLE). */
#if defined(PLATFORM_APPLE)
  const bool8_t toggle_modifier =
      input_is_key_down(state->input_state, KEY_LWIN) ||
      input_is_key_down(state->input_state, KEY_RWIN);
#else
  const bool8_t toggle_modifier =
      input_is_key_down(state->input_state, KEY_LCONTROL) ||
      input_is_key_down(state->input_state, KEY_RCONTROL);
#endif
  const bool8_t click_toggle =
      left_pressed && application->editor_viewport.enabled && toggle_modifier;
  bool8_t click_select = left_pressed && !click_toggle;

  int32_t mouse_x = 0;
  int32_t mouse_y = 0;
  int32_t prev_mouse_x = 0;
  int32_t prev_mouse_y = 0;
  input_get_mouse_position(state->input_state, &mouse_x, &mouse_y);
  input_get_previous_mouse_position(state->input_state, &prev_mouse_x,
                                    &prev_mouse_y);
  VkrViewportHitInfo viewport_info =
      vkr_standard_scene_runtime_get_viewport_hit_info(application, mouse_x,
                                                       mouse_y);
  if (state->gizmo_drag.pending_pick && !left_down &&
      !state->gizmo_drag.released)
    vkr_standard_scene_runtime_capture_gizmo_release(&viewport_info);

  if (state->gizmo_drag.active && left_down)
    vkr_standard_scene_runtime_update_gizmo_drag(application, &viewport_info);

  if (click_pressed && state->gizmo_hover_pending) {
    vkr_picking_cancel(picking);
    state->gizmo_hover_pending = false_v;
  }

  if (!state->gizmo_drag.active && !state->gizmo_drag.pending_pick &&
      click_pressed && !vkr_picking_is_pending(picking)) {
    int32_t press_x, press_y;
    input_get_button_press_position(state->input_state, BUTTON_LEFT, &press_x,
                                    &press_y);
    const VkrViewportHitInfo press_info =
        vkr_standard_scene_runtime_get_viewport_hit_info(application, press_x,
                                                         press_y);
    if (vkr_standard_scene_runtime_request_picking(application, picking,
                                                   &press_info)) {
      state->gizmo_drag.pending_pick = true_v;
      state->gizmo_drag.pending_select = click_select;
      state->gizmo_drag.pick_position = press_info.position;
      state->gizmo_drag.released = false_v;
      state->gizmo_drag.release_has_target_coords = false_v;
      if (click_toggle) {
        state->context_pick = true_v;
        state->context_pick_purpose = VKR_SAMPLE_PICK_SELECT_TOGGLE;
      }
      if (!left_down)
        vkr_standard_scene_runtime_capture_gizmo_release(&viewport_info);
    }
  }

  if (state->context_click_pending && !state->gizmo_drag.active &&
      !state->gizmo_drag.pending_pick) {
    sample_request_context_pick(application, picking);
  }

  bool8_t mouse_moved = (mouse_x != prev_mouse_x || mouse_y != prev_mouse_y);
  if (!state->gizmo_drag.active && !state->gizmo_drag.pending_pick &&
      !state->gizmo_hover_pending && !vkr_picking_is_pending(picking) &&
      mouse_moved && !left_down && !right_down && !middle_down &&
      application->gizmo_system.visible) {
    if (vkr_standard_scene_runtime_request_picking(application, picking,
                                                   &viewport_info)) {
      state->gizmo_hover_pending = true_v;
    }
  }

  VkrPickResult result =
      vkr_picking_get_result(&application->renderer, picking);

  if (state->gizmo_drag.pending_pick && !vkr_picking_is_pending(picking)) {
    state->gizmo_drag.pending_pick = false_v;
    const bool8_t context = state->context_pick;
    state->context_pick = false_v;

    VkrAllocator *frame_alloc = application->frame_allocator;
    String8 picked_text = {0};
    VkrEntityId picked_entity = VKR_ENTITY_ID_INVALID;
    bool8_t picked_entity_valid = false_v;
    bool8_t update_selection = state->gizmo_drag.pending_select;

    if (result.hit) {
      VkrPickingDecodedId decoded = vkr_picking_decode_id(result.object_id);
      if (!decoded.valid) {
        picked_text = string8_lit("Picked: unknown");
      } else if (decoded.kind == VKR_PICKING_ID_KIND_SCENE) {
        VkrEntityId entity = VKR_ENTITY_ID_INVALID;
        VkrSceneHandle picked_handle = state->scene_resource.as.scene;
        entity = vkr_scene_handle_entity_from_picking_id(picked_handle,
                                                         result.object_id);
        /* Each additive container, and the World, owns its own picking
           range. */
        for (uint32_t i = 0; !entity.u64 && i < VKR_SCENE_ADDITIVE_MAX; ++i) {
          if (state->additive_handles[i]) {
            picked_handle = state->additive_handles[i];
            entity = vkr_scene_handle_entity_from_picking_id(picked_handle,
                                                             result.object_id);
          }
        }
        if (!entity.u64 && state->world_handle) {
          picked_handle = state->world_handle;
          entity = vkr_scene_handle_entity_from_picking_id(picked_handle,
                                                           result.object_id);
        }

        if (entity.u64 != VKR_ENTITY_ID_INVALID.u64) {
          picked_entity = entity;
          picked_entity_valid = true_v;
          VkrScene *scene = vkr_scene_handle_get_scene(picked_handle);
          String8 name =
              scene ? vkr_scene_get_name(scene, entity) : (String8){0};
          if (name.length > 0) {
            picked_text = string8_create_formatted(frame_alloc, "Picked: %.*s",
                                                   (int)name.length, name.str);
          } else {
            picked_text = string8_create_formatted(
                frame_alloc, "Picked: entity %u", entity.parts.index);
          }
        } else {
          picked_text = string8_create_formatted(
              frame_alloc, "Picked: render id #%u (no entity)", decoded.value);
        }
      } else if (decoded.kind == VKR_PICKING_ID_KIND_UI_TEXT) {
        picked_text = string8_create_formatted(
            frame_alloc, "Picked: UI text #%u", decoded.value);
      } else if (decoded.kind == VKR_PICKING_ID_KIND_WORLD_TEXT) {
        VkrScene *scene =
            vkr_scene_handle_get_scene(state->scene_resource.as.scene);
        VkrEntityId text_entity = VKR_ENTITY_ID_INVALID;
        if (scene && vkr_standard_scene_runtime_world_text_entity_from_id(
                         application, scene, decoded.value, &text_entity)) {
          picked_entity = text_entity;
          picked_entity_valid = true_v;
          String8 name =
              scene ? vkr_scene_get_name(scene, text_entity) : (String8){0};
          if (name.length > 0) {
            picked_text = string8_create_formatted(frame_alloc, "Picked: %.*s",
                                                   (int)name.length, name.str);
          } else {
            picked_text = string8_create_formatted(
                frame_alloc, "Picked: world text #%u", decoded.value);
          }
        } else {
          picked_text = string8_create_formatted(
              frame_alloc, "Picked: world text #%u", decoded.value);
        }
      } else if (decoded.kind == VKR_PICKING_ID_KIND_GIZMO) {
        picked_text = string8_lit("Picked: gizmo");
        update_selection = false_v;

        VkrGizmoHandle handle = vkr_gizmo_decode_picking_id(result.object_id);
        state->gizmo_hot_handle = handle;
        vkr_gizmo_system_set_hot_handle(&application->gizmo_system, handle);
        bool8_t drag_button_down =
            input_is_button_down(state->input_state, BUTTON_LEFT);
        /* A right click on a handle opens the selection's menu. */
        if (context && state->has_selection) {
          picked_entity = state->selected_entity;
          picked_entity_valid = true_v;
        }
        if (!context && (drag_button_down || state->gizmo_drag.released) &&
            handle != VKR_GIZMO_HANDLE_NONE) {
          if (vkr_standard_scene_runtime_begin_gizmo_drag(application,
                                                          handle)) {
            vkr_gizmo_system_set_active_handle(&application->gizmo_system,
                                               handle);
            application->gizmo_system.mode = state->gizmo_drag.mode;
            if (state->gizmo_drag.released) {
              const VkrViewportHitInfo release_info = {
                  .position = state->gizmo_drag.release_position,
                  .has_target_coords =
                      state->gizmo_drag.release_has_target_coords,
              };
              vkr_standard_scene_runtime_update_gizmo_drag(application,
                                                           &release_info);
              vkr_standard_scene_runtime_finish_gizmo_edit(application);
            } else {
              vkr_standard_scene_runtime_update_gizmo_drag(application,
                                                           &viewport_info);
            }
            update_selection = false_v;
          }
        }
      } else {
        picked_text = string8_create_formatted(frame_alloc, "Picked: light #%u",
                                               decoded.value);
      }
    } else {
      picked_text = string8_lit("Picked: none");
    }

    /* A rendered gizmo keeps priority. In collision display mode, an eligible
       CPU ray hit selects its collider child even when the rendered mesh hit
       has no picking ID for the debug wireframe. */
    if (update_selection && state->picked_collider.u64 &&
        vkr_scene_entity_alive(application->active_scene,
                               state->picked_collider)) {
      picked_entity = state->picked_collider;
      picked_entity_valid = true_v;
      picked_text = string8_create_formatted(frame_alloc, "Picked: collider %u",
                                             picked_entity.parts.index);
    }
    if (update_selection) {
      if (picked_entity_valid) {
        state->selected_entity = picked_entity;
        state->has_selection = true_v;
      } else {
        state->selected_entity = VKR_ENTITY_ID_INVALID;
        state->has_selection = false_v;
      }
    }
    if (!context && update_selection && picked_entity_valid) {
      sample_begin_select_drag(application, &viewport_info);
    }
    if (context) {
      state->context_ready = true_v;
      state->context_ready_purpose = state->context_pick_purpose;
      state->context_entity =
          picked_entity_valid ? picked_entity : VKR_ENTITY_ID_INVALID;
      state->context_position_px = (Vec2){(float32_t)state->context_press_x,
                                          (float32_t)state->context_press_y};
    }

    if (picked_text.length > 0 &&
        result.object_id != state->last_picked_object_id) {
      state->last_picked_object_id = result.object_id;
      vkr_standard_scene_runtime_ui_text_set(&state->picked_object_text,
                                             picked_text);
    }

    state->gizmo_drag.pending_select = false_v;
  }

  if (state->gizmo_hover_pending && !vkr_picking_is_pending(picking)) {
    state->gizmo_hover_pending = false_v;

    VkrGizmoHandle hot_handle = VKR_GIZMO_HANDLE_NONE;
    if (result.hit) {
      VkrPickingDecodedId decoded = vkr_picking_decode_id(result.object_id);
      if (decoded.valid && decoded.kind == VKR_PICKING_ID_KIND_GIZMO) {
        hot_handle = vkr_gizmo_decode_picking_id(result.object_id);
      }
    }

    state->gizmo_hot_handle = hot_handle;
    vkr_gizmo_system_set_hot_handle(&application->gizmo_system, hot_handle);
  }
}

/**
 * @brief Update scene text3d entities (e.g., the WorldClock).
 *
 * Uses the new scene-based text3d API instead of layer messages.
 */
vkr_internal void vkr_standard_scene_runtime_update_world_text(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  if (!vkr_clock_interval_elapsed(&state->world_text_update_clock,
                                  VKR_WORLD_TIME_UPDATE_INTERVAL)) {
    return;
  }

  // Get the scene from the loaded resource
  VkrScene *scene = vkr_scene_handle_get_scene(state->scene_resource.as.scene);
  if (!scene) {
    return;
  }

  // Find the WorldClock entity
  VkrEntityId clock_entity =
      vkr_scene_find_entity_by_name(scene, string8_lit("WorldClock"));
  if (clock_entity.u64 == VKR_ENTITY_ID_INVALID.u64) {
    return;
  }

  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&application->app_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    log_error("Failed to create world text allocator scope");
    return;
  }

  VkrTime time = vkr_platform_get_local_time();
  String8 time_text =
      string8_create_formatted(&application->app_allocator, "%02d:%02d:%02d",
                               time.hours, time.minutes, time.seconds);
  if (time_text.length > 0) {
    // Update the scene text3d using the new API
    if (!vkr_scene_update_text3d(scene, clock_entity, time_text)) {
      log_error("Failed to update scene world text");
    }
  }

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
}

vkr_internal void vkr_standard_scene_runtime_poll_upload_wait_stats(
    VkrStandardSceneRuntime *application) {
  if (!application || !state) {
    return;
  }

  bool8_t scene_pending = false_v;
  if (state->scene_resource.request_id != 0) {
    VkrRendererError scene_state_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState scene_state = vkr_resource_system_get_state(
        &state->scene_resource, &scene_state_error);
    scene_pending =
        (scene_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU) ||
        (scene_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES) ||
        (scene_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU);
  }

  VkrMetricsSnapshotView snapshot = {0};
  if (!vkr_metrics_snapshot_acquire(application->metrics, &snapshot)) {
    return;
  }
  if (snapshot.publication_serial ==
      state->upload_wait_last_publication_serial) {
    vkr_metrics_snapshot_release(application->metrics, &snapshot);
    return;
  }
  // Publication serials must advance by exactly one. A gap means a frame's
  // upload waits were never published (both spare snapshot buffers were
  // pinned) and this assertion no longer has complete evidence to stand on.
  const uint64_t previous_serial = state->upload_wait_last_publication_serial;
  const bool8_t serial_gap =
      previous_serial != 0 && snapshot.publication_serial > previous_serial + 1;
  state->upload_wait_last_publication_serial = snapshot.publication_serial;

  VkrRendererUploadWaitStats wait_stats = {0};
  const VkrRendererMetricIds *ids = &application->renderer_metrics.ids;
  // An unavailable slot is missing evidence, never zero waits: reading it as
  // "no stalls" would turn a broken instrument into a passing assertion.
  const bool8_t complete =
      vkr_metrics_frame_read_u64(snapshot.frame, ids->upload_fence_waits,
                                 &wait_stats.fence_wait_count) &&
      vkr_metrics_frame_read_u64(snapshot.frame, ids->upload_queue_idle_waits,
                                 &wait_stats.queue_wait_idle_count) &&
      vkr_metrics_frame_read_u64(snapshot.frame, ids->upload_device_idle_waits,
                                 &wait_stats.device_wait_idle_count);
  vkr_metrics_snapshot_release(application->metrics, &snapshot);

  if (!complete || serial_gap) {
    if (!state->upload_wait_evidence_incomplete) {
      state->upload_wait_evidence_incomplete = true_v;
      log_warn("Upload-wait evidence incomplete (%s); "
               "VKR_ASSERT_NO_UPLOAD_WAITS cannot be relied upon",
               !complete ? "metric unavailable" : "publication gap");
    }
    return;
  }

  /*
   * Track only waits observed while an async scene request is still pending.
   * Startup/bootstrap uploads may legitimately use synchronous helpers and are
   * outside the async-streaming acceptance criteria.
   */
  if (!scene_pending) {
    return;
  }

  const bool8_t has_waits = (wait_stats.fence_wait_count > 0) ||
                            (wait_stats.queue_wait_idle_count > 0) ||
                            (wait_stats.device_wait_idle_count > 0);
  if (!has_waits) {
    return;
  }

  state->upload_wait_fence_total += wait_stats.fence_wait_count;
  state->upload_wait_queue_idle_total += wait_stats.queue_wait_idle_count;
  state->upload_wait_device_idle_total += wait_stats.device_wait_idle_count;

  if (state->assert_no_upload_waits) {
    log_error("Upload-path wait detected during async scene loading "
              "(fence=%llu, queue_idle=%llu, device_idle=%llu)",
              (unsigned long long)wait_stats.fence_wait_count,
              (unsigned long long)wait_stats.queue_wait_idle_count,
              (unsigned long long)wait_stats.device_wait_idle_count);
    state->upload_wait_violation_seen = true_v;
    vkr_standard_scene_runtime_close(application);
  }
}

/* The Show filter and the greybox view reach every loaded container,
   including ones loaded after they changed; unchanged ones cost one compare
   per scene. */
static void sample_show_filter_apply(VkrStandardSceneRuntime *application) {
  const uint32_t hidden = state->view_state.hidden_kinds;
  /* Clip and trigger brushes draw while the editor edits, not during Play
     or in a game (ADR-084). */
  const bool8_t volumes = application->editor_viewport.enabled &&
                          !application->editor_viewport.simulation_running;
  /* The editor's hidden objects likewise apply only while it edits. */
  const uint32_t hidden_count = volumes ? state->hidden.count : 0u;
  VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX] = {
      application->active_scene,
      vkr_scene_handle_get_scene(state->world_handle)};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes[2u + i] = vkr_scene_handle_get_scene(state->additive_handles[i]);
  }
  for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
    vkr_scene_brush_set_greybox_view(scenes[i], state->view_state.greybox_view);
    vkr_scene_set_editor_hidden_kinds(scenes[i], hidden);
    vkr_scene_set_editor_volumes(scenes[i], volumes);
    (void)vkr_scene_set_editor_hidden(scenes[i], state->hidden.entities,
                                      hidden_count, state->hidden.isolate);
  }
}

/* The ground grid follows the view: the XZ plane at the grid height in
   perspective, top and bottom views, the ZY plane from the left and right
   and the XY plane from the front and back, both at the depth the view is
   framed on. In perspective it fades with distance, farther as the camera
   rises. */
static void sample_grid_apply(VkrStandardSceneRuntime *application) {
  const VkrSampleViewState *view = &state->view_state;
  const VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  VkrEditorGridPlane plane = VKR_EDITOR_GRID_PLANE_XZ;
  if (view->camera_view == VKR_SAMPLE_CAMERA_LEFT ||
      view->camera_view == VKR_SAMPLE_CAMERA_RIGHT) {
    plane = VKR_EDITOR_GRID_PLANE_ZY;
  } else if (view->camera_view == VKR_SAMPLE_CAMERA_FRONT ||
             view->camera_view == VKR_SAMPLE_CAMERA_BACK) {
    plane = VKR_EDITOR_GRID_PLANE_XY;
  }
  VkrEditorGridPayload grid = {
      .enabled = view->grid_enabled && camera &&
                 (application->active_scene ||
                  vkr_scene_handle_get_scene(state->world_handle)),
      .through_geometry = view->grid_through_geometry,
      .plane = plane,
      .cell_size = vkr_clamp_f32(view->grid_spacing, 0.001f, 10000.0f),
      .height = view->grid_height,
  };
  if (camera && plane != VKR_EDITOR_GRID_PLANE_XZ) {
    /* An orthographic view is framed on the middle of its depth range. */
    const Vec3 focus =
        vec3_add(camera->position,
                 vec3_scale(camera->forward,
                            0.5f * (camera->near_clip + camera->far_clip)));
    grid.height = plane == VKR_EDITOR_GRID_PLANE_ZY ? focus.x : focus.z;
  }
  if (grid.enabled && view->camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE) {
    const float32_t height = fabsf(camera->position.y - grid.height);
    grid.fade_end = Max(grid.cell_size * 150.0f, height * 60.0f);
    grid.fade_start = grid.fade_end * 0.35f;
  }
  application->editor_viewport.grid = grid;
}

float32_t vkr_sample_camera_speed_fraction(float32_t speed) {
  return vkr_clamp_f32(
      logf(Max(speed, 1e-6f) / VKR_SAMPLE_CAMERA_SPEED_MIN) /
          logf(VKR_SAMPLE_CAMERA_SPEED_MAX / VKR_SAMPLE_CAMERA_SPEED_MIN),
      0.0f, 1.0f);
}

float32_t vkr_sample_camera_speed_value(float32_t fraction) {
  const float32_t speed =
      VKR_SAMPLE_CAMERA_SPEED_MIN *
      powf(VKR_SAMPLE_CAMERA_SPEED_MAX / VKR_SAMPLE_CAMERA_SPEED_MIN,
           vkr_clamp_f32(fraction, 0.0f, 1.0f));
  const float32_t unit = powf(10.0f, floorf(log10f(speed)) - 1.0f);
  return roundf(speed / unit) * unit;
}

float32_t vkr_sample_camera_speed_step(float32_t speed, float32_t wheel_lines) {
  return vkr_sample_camera_speed_value(vkr_sample_camera_speed_fraction(speed) +
                                       wheel_lines / 24.0f);
}

static void sample_view_apply(VkrStandardSceneRuntime *application,
                              const VkrSampleViewRequest *request) {
  if (!request->apply || !application->editor_viewport.enabled) {
    return;
  }
  VkrSampleViewState next = request->value;
  if ((uint32_t)next.camera_view >= VKR_SAMPLE_CAMERA_VIEW_COUNT ||
      (uint32_t)next.render_mode >= VKR_RENDER_MODE_COUNT ||
      !isfinite(next.grid_spacing) || next.grid_spacing <= 0.0f ||
      !isfinite(next.grid_height) || next.gizmo_tool > VKR_GIZMO_MODE_SCALE ||
      next.gizmo_space > VKR_GIZMO_SPACE_LOCAL || next.collision_display > 2u ||
      (next.hidden_kinds & ~VKR_SCENE_SHOW_HIDE_ALL) ||
      !isfinite(next.camera_speed) || next.camera_speed <= 0.0f ||
      !isfinite(next.camera_sensitivity) || next.camera_sensitivity <= 0.0f) {
    return;
  }
  next.grid_spacing = vkr_clamp_f32(next.grid_spacing, 0.001f, 10000.0f);
  next.grid_height = vkr_clamp_f32(next.grid_height, -10000.0f, 10000.0f);
  /* A height set by hand replaces the last fit's outcome in the Grid menu. */
  if (next.grid_height != state->view_state.grid_height) {
    state->grid_status[0] = '\0';
  }
  next.camera_speed = vkr_clamp_f32(next.camera_speed, 0.05f, 10000.0f);
  /* The bound the saved editor preferences accept. */
  next.camera_sensitivity =
      vkr_clamp_f32(next.camera_sensitivity, 0.01f, 100.0f);
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  application->gizmo_system.tool = (VkrGizmoMode)next.gizmo_tool;
  application->gizmo_system.space = (VkrGizmoSpace)next.gizmo_space;
  if (camera) {
    camera->speed = next.camera_speed;
    camera->sensitivity = next.camera_sensitivity;
  }
  if (camera && next.camera_view != state->view_state.camera_view) {
    vkr_standard_scene_runtime_finish_gizmo_edit(application);
    vkr_standard_scene_runtime_cancel_gizmo_pick(application);
    if (next.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE) {
      if (state->perspective_camera_saved) {
        const uint32_t width = camera->cached_window_width;
        const uint32_t height = camera->cached_window_height;
        const float32_t speed = camera->speed;
        const float32_t sensitivity = camera->sensitivity;
        *camera = state->perspective_camera;
        camera->cached_window_width = width;
        camera->cached_window_height = height;
        camera->speed = speed;
        camera->sensitivity = sensitivity;
        camera->projection_dirty = true_v;
        camera->view_dirty = true_v;
      }
    } else {
      Vec3 target;
      float32_t half_height = 25.0f;
      if (camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
        if (state->script_camera_active) {
          vkr_camera_set_pose(camera, state->editor_camera_position,
                              state->editor_camera_yaw,
                              state->editor_camera_pitch);
          state->script_camera_active = false_v;
        }
        state->perspective_camera = *camera;
        state->perspective_camera_saved = true_v;
        const SceneTransform *selected =
            application->active_scene && state->has_selection
                ? vkr_scene_get_transform(application->active_scene,
                                          state->selected_entity)
                : NULL;
        if (selected) {
          target = mat4_position(selected->world);
        } else {
          const Vec3 ahead =
              vec3_add(camera->position, vec3_scale(camera->forward, 25.0f));
          target = ahead;
        }
      } else {
        half_height = 0.5f * (camera->top_clip - camera->bottom_clip);
        target =
            vec3_add(camera->position,
                     vec3_scale(camera->forward,
                                0.5f * (camera->near_clip + camera->far_clip)));
      }
      static const Vec3 forwards[VKR_SAMPLE_CAMERA_VIEW_COUNT] = {
          {0, 0, -1}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0},
          {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
      static const Vec3 ups[VKR_SAMPLE_CAMERA_VIEW_COUNT] = {
          {0, 1, 0}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0},
          {0, 0, 1}, {0, 1, 0},  {0, 1, 0}};
      const Vec3 forward = forwards[next.camera_view];
      const float32_t distance = 0.5f * (camera->near_clip + camera->far_clip);
      (void)vkr_camera_set_basis(
          camera, vec3_sub(target, vec3_scale(forward, distance)), forward,
          ups[next.camera_view]);
      camera->type = VKR_CAMERA_TYPE_ORTHOGRAPHIC;
      const float32_t aspect =
          (float32_t)camera->cached_window_width /
          Max(1.0f, (float32_t)camera->cached_window_height);
      camera->left_clip = -half_height * aspect;
      camera->right_clip = half_height * aspect;
      camera->bottom_clip = -half_height;
      camera->top_clip = half_height;
      camera->projection_dirty = true_v;
      next.grid_enabled = true_v;
    }
    vkr_camera_system_update(camera);
    application->camera_controller.frame_move_forward = 0.0f;
    application->camera_controller.frame_move_right = 0.0f;
    application->camera_controller.frame_move_world_up = 0.0f;
    application->camera_controller.frame_yaw_delta = 0.0f;
    application->camera_controller.frame_pitch_delta = 0.0f;
    application->camera_controller.frame_look_yaw = 0.0f;
    application->camera_controller.frame_look_pitch = 0.0f;
    vkr_window_set_mouse_capture(&application->host.window, false_v);
    state->free_camera_held = false_v;
  }
  state->view_state = next;
  application->globals.render_mode = next.render_mode;
}

/* Requests the UI client fills during one build. The runtime consumes them
   after build returns, so they live for the whole UI update. */
typedef struct VkrSampleUiRequests {
  VkrGraphicsSettingsRequest graphics_request;
  VkrSampleTransportAction transport_action;
  VkrSampleViewRequest view_request;
  bool8_t looks_changed;
  bool8_t custom_materials_reload;
  VkrSampleHideRequest hide_request;
  VkrSamplePhysicsRequest physics_request;
  VkrSampleIoRequest io_request;
  VkrSceneEditRequest scene_edit;
  VkrSampleEditBatchRequest edit_batch;
  VkrSampleCaptureRequest capture_request;
  VkrSampleSceneRequest scene_request;
  VkrSampleWorldRequest world_request;
  VkrSampleScriptRequest script_request;
  VkrSampleEditorStateRequest editor_state_request;
  VkrSamplePickRequest pick_request;
  VkrSampleGridFitRequest grid_fit_request;
  VkrSampleTimeOfDayRequest time_of_day_request;
  VkrSampleCloseResponse close_response;
  bool8_t quit;
  bool8_t scene_shortcuts_blocked;
  bool8_t scene_maximized;
} VkrSampleUiRequests;

/* Describes this frame to the UI client and runs its build. The frame lends
   the client `requests` until build returns. */
vkr_internal VkrUiDockInputCapture vkr_standard_scene_runtime_build_ui_frame(
    VkrStandardSceneRuntime *application, VkrSampleUiRequests *requests) {
  const VkrMaterialTextureStreamStats texture_streams =
      vkr_material_system_get_texture_stream_stats(
          &application->assets.material_system);
  const VkrRendererPipelineStats pipelines =
      vkr_renderer_get_pipeline_stats(&application->renderer);
  VkrSampleUiFrame frame = {
      .ui = &application->ui_system,
      .window = &application->host.window,
      .assets = &application->assets,
      .dock = &application->editor_viewport.dock,
      .input = state->input_state,
      .view_projection =
          mat4_mul(application->globals.projection, application->globals.view),
      .text =
          {
              .camera =
                  vkr_standard_scene_runtime_ui_text_view(&state->left_text),
              .performance =
                  vkr_standard_scene_runtime_ui_text_view(&state->fps_text),
              .metrics =
                  vkr_standard_scene_runtime_ui_text_view(&state->metrics_text),
              .memory =
                  vkr_standard_scene_runtime_ui_text_view(&state->memory_text),
              .system =
                  string8_create_from_cstr((const uint8_t *)state->system_text,
                                           string_length(state->system_text)),
          },
      .simulation_time = application->editor_viewport.simulation_time,
      .simulation_running = application->editor_viewport.simulation_running,
      .scripts_running = vkr_script_host_active(&state->scripts) &&
                         application->editor_viewport.simulation_running,
      .io_running = (vkr_script_host_active(&state->scripts) ||
                     vkr_io_router_active(&state->scripts.io)) &&
                    application->editor_viewport.simulation_running,
      .scene_rendering_stopped =
          application->editor_viewport.scene_rendering_stopped,
      .scene_error = application->editor_viewport.scene_error,
      .texture_pending_count = texture_streams.pending_count,
      .pipeline_pending_count = pipelines.pending_graphs,
      .pipeline_late_draws = pipelines.late_draws,
      .texture_demanded_missing_count = texture_streams.demanded_missing_count,
      .scene_output_scale = application->scene_output_scale,
      .scene_render_width = application->editor_viewport.enabled
                                ? application->editor_viewport.rendered_width
                                : application->target_render_width,
      .scene_render_height = application->editor_viewport.enabled
                                 ? application->editor_viewport.rendered_height
                                 : application->target_render_height,
      .scene_output_width = application->editor_viewport.enabled
                                ? application->editor_viewport.output_width
                                : application->target_window_width,
      .scene_output_height = application->editor_viewport.enabled
                                 ? application->editor_viewport.output_height
                                 : application->target_window_height,
      .graphics = &state->graphics,
      .graphics_request = &requests->graphics_request,
      .transport_action = &requests->transport_action,
      .script_request = &requests->script_request,
      .script_results = state->script_results,
      .script_result_count = state->script_result_count,
      .scripts = &state->scripts,
      .view_state = state->view_state,
      .view_request = &requests->view_request,
      .looks_changed = &requests->looks_changed,
      .custom_materials_reload = &requests->custom_materials_reload,
      .hide_request = &requests->hide_request,
      .physics_request = &requests->physics_request,
      .io_request = &requests->io_request,
      .io_result = &state->io_result,
      .scene_keyboard_focus = &state->scene_keyboard_focus,
      .scene_backdrop_blur = &application->editor_viewport.scene_backdrop_blur,
      .animation_preview = &application->animation_preview,
      .scene_shortcuts_blocked = &requests->scene_shortcuts_blocked,
      .scene = application->active_scene,
      .selected_entity = state->selected_entity,
      .context_requested = state->context_ready,
      .context_entity = state->context_entity,
      .context_position_px = state->context_position_px,
      .context_purpose = state->context_ready_purpose,
      .pick_request = &requests->pick_request,
      .grid_fit_request = &requests->grid_fit_request,
      .time_of_day_request = &requests->time_of_day_request,
      .grid_status = string8_create_from_cstr(
          (const uint8_t *)state->grid_status, strlen(state->grid_status)),
      .scene_generation = application->scene_generation,
      .edits = &state->edits,
      .scene_edit = &requests->scene_edit,
      .edit_batch = &requests->edit_batch,
      .edit_batch_result = &state->edit_batch_result,
      .gizmo_edit = state->gizmo_recorded.count ? &state->gizmo_recorded : NULL,
      .gizmo_edit_pending = state->gizmo_edit_pending,
      .capture_request = &requests->capture_request,
      .capture_ready =
          state->capture_ready_valid ? &state->capture_ready : NULL,
      .scene_request = &requests->scene_request,
      .world = application->world_scene,
      .world_edits = &state->world_edits,
      .world_request = &requests->world_request,
      .world_status = string8_create_from_cstr(
          (const uint8_t *)state->world_status, strlen(state->world_status)),
      .world_loading = state->world_pending,
      .runtime_preferences = sample_preferences_snapshot(application),
      .scene_recall = sample_recall_snapshot(application),
      .editor_state_request = &requests->editor_state_request,
      .close_requested = vkr_window_close_requested(&application->host.window),
      .close_response = &requests->close_response,
      .quit_request = &requests->quit,
      .scene_path = state->scene_path,
      .scene_status = string8_create_from_cstr(
          (const uint8_t *)state->scene_status, strlen(state->scene_status)),
      /* A scene whose Custom material graphs are still creating their
         pipelines draws their fallback, so it still loads (ADR-096). */
      .scene_loading = (state->scene_load_timer_active &&
                        !state->scene_load_terminal_logged) ||
                       pipelines.pending_graphs > 0u,
      .modal = &state->modal,
      .scene_only = application->editor_viewport.scene_only ||
                    application->editor_viewport.scene_maximized,
      .scene_maximized = application->editor_viewport.scene_maximized,
      .scene_maximized_next = &requests->scene_maximized,
      .escape_taken = state->escape_taken,
      .mouse_captured = vkr_window_is_mouse_captured(&application->host.window),
  };
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    frame.additive_loading |= state->additive_pending[i];
    if (state->additive_handles[i]) {
      frame.additive[i] =
          vkr_scene_handle_get_scene(state->additive_handles[i]);
      frame.additive_edits[i] = &state->additive_edits[i];
      frame.additive_names[i] =
          string8_create_from_cstr((const uint8_t *)state->additive_paths[i],
                                   strlen(state->additive_paths[i]));
    }
  }
  if (application->editor_viewport.enabled) {
    frame.mapping_valid = vkr_standard_scene_runtime_editor_viewport_mapping(
        application, application->ui_system.target_width,
        application->ui_system.target_height, &frame.mapping);
  }
  /* The UI sees a context click once. */
  state->context_ready = false_v;
  return state->ui.build(state->ui.state, &frame);
}

/* ---- Root World container (ADR-076) ---- */

/* Joins a newly published scene to the shared physics world before its
   sidecar can create bodies. A failure leaves the scene on a private world. */
static void sample_physics_attach(VkrStandardSceneRuntime *application,
                                  VkrScene *scene, bool8_t driver) {
  if (!state->physics_set) {
    state->physics_set =
        vkr_scene_physics_set_create(&application->assets.allocator);
  }
  const char *error = NULL;
  if (!state->physics_set ||
      !vkr_scene_physics_attach(scene, state->physics_set, driver, &error)) {
    log_warn("Scene keeps a private physics world: %s",
             error ? error : "the physics set could not be created");
  }
}

static bool8_t sample_world_dirty(void) {
  return state->world_handle &&
         state->world_edits.revision != state->world_edits.saved_revision;
}

static String8 sample_world_document(void) {
  return string8_create_from_cstr((const uint8_t *)state->world_path,
                                  strlen(state->world_path));
}

/* Waits for GPU work before release: with no scene loaded the World renders
   alone and owns the published sky products. A pending load is cancelled. */
vkr_internal void sample_world_unload(VkrStandardSceneRuntime *application) {
  if (!state->world_handle && !state->world_pending) {
    return;
  }
  if (state->has_selection &&
      state->selected_entity.parts.world == VKR_SCENE_WORLD_ROOT_ID) {
    vkr_standard_scene_runtime_clear_gizmo_selection(application);
  }
  /* World-attached script instances end before their entities go. */
  if (application->world_scene) {
    vkr_script_host_detach(&state->scripts, application->world_scene);
  }
  if (application->active_scene) {
    vkr_scene_set_world_fallback(application->active_scene, NULL);
  }
  if (vkr_renderer_wait_idle(&application->renderer) !=
      VKR_RENDERER_ERROR_NONE) {
    application->last_renderer_error = VKR_RENDERER_ERROR_DEVICE_ERROR;
  }
  application->world_scene = NULL;
  if (state->world_resource.type == VKR_RESOURCE_TYPE_SCENE) {
    const String8 document = sample_world_document();
    vkr_resource_system_unload(&state->world_resource, document);
    (void)vkr_scene_loader_request_container(&application->assets, document,
                                             0u);
  } else if (state->world_handle) {
    vkr_scene_handle_destroy(state->world_handle, &application->assets);
  }
  state->world_resource = (VkrResourceHandleInfo){0};
  state->world_handle = NULL;
  state->world_pending = false_v;
  vkr_scene_edit_reset(&state->world_edits, &state->edit_allocator, 0u);
}

/* Publishes a loaded World: its journal and sidecar, shared physics and
   collision layers, and its own picking range. */
vkr_internal void sample_world_activate(VkrStandardSceneRuntime *application,
                                        VkrSceneHandle handle) {
  VkrScene *world = vkr_scene_handle_get_scene(handle);
  world->render_id_base = VKR_SCENE_WORLD_RENDER_ID_BASE;
  (void)vkr_scene_resolve_world(world);
  state->world_handle = handle;
  application->world_scene = world;
  sample_physics_attach(application, world, false_v);
  vkr_scene_edit_reset(&state->world_edits, &state->edit_allocator, 1u);
  if (state->world_sidecar[0]) {
    (void)vkr_scene_edit_load(
        &state->world_edits, world,
        string8_create_from_cstr((const uint8_t *)state->world_sidecar,
                                 strlen(state->world_sidecar)));
  }
  /* The World owns collision layers: loaded scenes take its settings. */
  (void)vkr_scene_collision_layers_share(world, NULL);
  /* Opened alone, the World is framed like a blank level: standing above the
     ground grid, looking slightly down toward -Z. */
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (state->world_frame_camera && !application->active_scene && camera) {
    vkr_camera_set_pose(camera, vec3_new(0.0f, 2.5f, 8.0f), -90.0f, -15.0f);
    vkr_camera_system_update(camera);
  }
  state->world_frame_camera = false_v;
  state->world_status[0] = '\0';
}

/* Starts loading the World document; `reload` keeps the camera. A missing
   document starts an empty World at once. */
vkr_internal bool8_t sample_world_load(VkrStandardSceneRuntime *application,
                                       String8 path, String8 sidecar,
                                       bool8_t reload) {
  if (path.length >= sizeof(state->world_path) ||
      sidecar.length >= sizeof(state->world_sidecar)) {
    snprintf(state->world_status, sizeof(state->world_status),
             "World path is too long.");
    return false_v;
  }
  sample_world_unload(application);
  MemZero(state->world_path, sizeof(state->world_path));
  MemZero(state->world_sidecar, sizeof(state->world_sidecar));
  MemCopy(state->world_path, path.str, path.length);
  MemCopy(state->world_sidecar, sidecar.str, sidecar.length);
  state->world_frame_camera = !reload;

  const String8 document = sample_world_document();
  const FilePath file = {.path = document, .type = FILE_PATH_TYPE_ABSOLUTE};
  if (!state->world_path[0] || !file_exists(&file)) {
    VkrSceneError error = VKR_SCENE_ERROR_NONE;
    VkrSceneHandle handle =
        vkr_scene_handle_create(&application->assets.allocator,
                                VKR_SCENE_WORLD_ROOT_ID, 64u, 64u, &error);
    if (!handle) {
      snprintf(state->world_status, sizeof(state->world_status),
               "World could not be created (%d).", (int)error);
      return false_v;
    }
    sample_world_activate(application, handle);
    return true_v;
  }
  if (!vkr_scene_loader_request_container(&application->assets, document,
                                          VKR_SCENE_WORLD_ROOT_ID)) {
    snprintf(state->world_status, sizeof(state->world_status),
             "Too many scene container requests to load the World.");
    return false_v;
  }
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, document,
                                application->frame_allocator,
                                &state->world_resource, &error)) {
    (void)vkr_scene_loader_request_container(&application->assets, document,
                                             0u);
    state->world_resource = (VkrResourceHandleInfo){0};
    const String8 message = vkr_renderer_get_error_string(error);
    snprintf(state->world_status, sizeof(state->world_status),
             "World document could not be loaded: %.*s", (int)message.length,
             message.str);
    return false_v;
  }
  state->world_pending = true_v;
  return true_v;
}

/* Activates a World load that resolved or reports one that failed, then
   keeps the World's transforms and render products current; it has no
   simulation of its own (ADR-076). */
vkr_internal void sample_world_update(VkrStandardSceneRuntime *application,
                                      bool8_t advance) {
  if (state->world_pending) {
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    const VkrResourceLoadState load =
        vkr_resource_system_get_state(&state->world_resource, &error);
    VkrResourceHandleInfo resolved = {0};
    if (load == VKR_RESOURCE_LOAD_STATE_FAILED ||
        load == VKR_RESOURCE_LOAD_STATE_CANCELED) {
      const String8 message = vkr_renderer_get_error_string(error);
      sample_world_unload(application);
      snprintf(state->world_status, sizeof(state->world_status),
               "World document could not be loaded: %.*s", (int)message.length,
               message.str);
    } else if (load == VKR_RESOURCE_LOAD_STATE_READY &&
               vkr_resource_system_try_get_resolved(&state->world_resource,
                                                    &resolved) &&
               resolved.as.scene) {
      state->world_resource = resolved;
      state->world_pending = false_v;
      sample_world_activate(application, resolved.as.scene);
    }
  }
  /* A simulated World advanced already. */
  if (state->world_handle && advance) {
    vkr_scene_handle_update_and_sync(state->world_handle, &application->assets,
                                     0.0);
  }
}

vkr_internal void sample_world_save(void) {
  if (!state->world_handle || !state->world_sidecar[0] ||
      !sample_world_dirty()) {
    return;
  }
  if (!vkr_scene_edit_save(
          &state->world_edits, vkr_scene_handle_get_scene(state->world_handle),
          string8_create_from_cstr((const uint8_t *)state->world_sidecar,
                                   strlen(state->world_sidecar)))) {
    snprintf(state->world_status, sizeof(state->world_status), "%s",
             state->world_edits.status);
  }
}

vkr_internal void sample_world_request(VkrStandardSceneRuntime *application,
                                       const VkrSampleWorldRequest *request) {
  if (request->save) {
    sample_world_save();
  }
  if ((request->load || request->unload) && sample_world_dirty() &&
      !request->discard_edits) {
    snprintf(state->world_status, sizeof(state->world_status),
             "Save or discard World edits first.");
    return;
  }
  /* Closing a project closes every container: added scenes go with the
     World they belong to. A reload of the same World keeps them. */
  if (request->unload || (request->load && !request->reload)) {
    for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
      if (sample_additive_dirty(i) && !request->discard_edits) {
        snprintf(state->world_status, sizeof(state->world_status),
                 "Save or discard added scene edits first.");
        return;
      }
    }
    for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
      sample_additive_remove(application, i);
    }
  }
  if (request->unload) {
    sample_world_unload(application);
  } else if (request->load) {
    (void)sample_world_load(application, request->path, request->sidecar_path,
                            request->reload);
  }
}

/* ---- Time of day (ADR-090) ---- */

/* The rendered scene takes the hour; every loaded container takes a light
   group's intensity, so its lights follow wherever they load. */
vkr_internal void
sample_time_of_day_apply(VkrStandardSceneRuntime *application,
                         const VkrSampleTimeOfDayRequest *request) {
  VkrScene *render = vkr_standard_scene_runtime_render_scene(application);
  if (request->set_hour && render) {
    (void)vkr_scene_set_time_of_day_hour(render, request->hour);
  }
  if (!request->set_group) {
    return;
  }
  VkrScene *scenes[VKR_SCENE_ADDITIVE_MAX + 2u] = {application->active_scene,
                                                   application->world_scene};
  uint32_t count = 2u;
  for (uint32_t i = 0u; i < application->additive_count; ++i) {
    scenes[count++] = application->additive_scenes[i];
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (scenes[i]) {
      (void)vkr_scene_set_light_group_intensity(scenes[i], request->group,
                                                request->intensity);
    }
  }
}

/* ---- Additive scene containers (ADR-076) ---- */

static bool8_t sample_additive_dirty(uint32_t slot) {
  return state->additive_handles[slot] &&
         state->additive_edits[slot].revision !=
             state->additive_edits[slot].saved_revision;
}

/* Rebuilds the runtime's list of rendered additive scenes. */
vkr_internal void
sample_additive_publish(VkrStandardSceneRuntime *application) {
  application->additive_count = 0u;
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (state->additive_handles[i]) {
      application->additive_scenes[application->additive_count++] =
          vkr_scene_handle_get_scene(state->additive_handles[i]);
    }
  }
}

/* Slot of an entity's additive container, or VKR_SCENE_ADDITIVE_MAX. */
static uint32_t sample_additive_slot(VkrEntityId entity) {
  const uint32_t world = entity.parts.world;
  if (!entity.u64 || world == 0u || world > VKR_SCENE_ADDITIVE_MAX ||
      !state->additive_handles[world - 1u]) {
    return VKR_SCENE_ADDITIVE_MAX;
  }
  return world - 1u;
}

static String8 sample_additive_path(uint32_t slot) {
  return string8_create_from_cstr((const uint8_t *)state->additive_paths[slot],
                                  strlen(state->additive_paths[slot]));
}

/* Releases a slot's scene and resource after GPU work that may use its
   instances completes. */
vkr_internal void sample_additive_remove(VkrStandardSceneRuntime *application,
                                         uint32_t slot) {
  if (slot >= VKR_SCENE_ADDITIVE_MAX ||
      (!state->additive_handles[slot] && !state->additive_pending[slot])) {
    return;
  }
  if (state->has_selection && state->selected_entity.parts.world == slot + 1u) {
    vkr_standard_scene_runtime_clear_gizmo_selection(application);
  }
  if (vkr_renderer_wait_idle(&application->renderer) !=
      VKR_RENDERER_ERROR_NONE) {
    application->last_renderer_error = VKR_RENDERER_ERROR_DEVICE_ERROR;
  }
  const String8 path = sample_additive_path(slot);
  vkr_resource_system_unload(&state->additive_resources[slot], path);
  (void)vkr_scene_loader_request_container(&application->assets, path, 0u);
  state->additive_resources[slot] = (VkrResourceHandleInfo){0};
  state->additive_handles[slot] = NULL;
  state->additive_pending[slot] = false_v;
  state->additive_paths[slot][0] = '\0';
  state->additive_sidecars[slot][0] = '\0';
  vkr_scene_edit_reset(&state->additive_edits[slot], &state->edit_allocator,
                       0u);
  sample_additive_publish(application);
}

/* Starts loading a scene beside the active one through the resource system;
   the loader creates it with the slot's world id and picking range. */
vkr_internal bool8_t sample_additive_add(VkrStandardSceneRuntime *application,
                                         String8 path, String8 sidecar) {
  uint32_t slot = VKR_SCENE_ADDITIVE_MAX;
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    const bool8_t used =
        state->additive_handles[i] || state->additive_pending[i];
    if (used && strlen(state->additive_paths[i]) == path.length &&
        MemCompare(state->additive_paths[i], path.str, path.length) == 0) {
      snprintf(state->scene_status, sizeof(state->scene_status),
               "That scene is already added.");
      return false_v;
    }
    if (!used && slot == VKR_SCENE_ADDITIVE_MAX) {
      slot = i;
    }
  }
  if (slot == VKR_SCENE_ADDITIVE_MAX) {
    snprintf(state->scene_status, sizeof(state->scene_status),
             "At most %u scenes can be added.", VKR_SCENE_ADDITIVE_MAX);
    return false_v;
  }
  /* A path is one resource: the active scene cannot also be added. */
  if (!path.length || path.length >= sizeof(state->additive_paths[slot]) ||
      sidecar.length >= sizeof(state->additive_sidecars[slot]) ||
      (state->scene_path.length == path.length &&
       MemCompare(state->scene_path.str, path.str, path.length) == 0)) {
    snprintf(state->scene_status, sizeof(state->scene_status),
             "Invalid additive scene path, or it is the active scene.");
    return false_v;
  }
  MemZero(state->additive_paths[slot], sizeof(state->additive_paths[slot]));
  MemZero(state->additive_sidecars[slot],
          sizeof(state->additive_sidecars[slot]));
  MemCopy(state->additive_paths[slot], path.str, path.length);
  MemCopy(state->additive_sidecars[slot], sidecar.str, sidecar.length);
  const String8 stored = sample_additive_path(slot);
  if (!vkr_scene_loader_request_container(&application->assets, stored,
                                          (uint16_t)(slot + 1u))) {
    state->additive_paths[slot][0] = '\0';
    snprintf(state->scene_status, sizeof(state->scene_status),
             "Too many additive scene requests.");
    return false_v;
  }
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, stored,
                                application->frame_allocator,
                                &state->additive_resources[slot], &error)) {
    (void)vkr_scene_loader_request_container(&application->assets, stored, 0u);
    state->additive_paths[slot][0] = '\0';
    const String8 message = vkr_renderer_get_error_string(error);
    snprintf(state->scene_status, sizeof(state->scene_status),
             "Additive scene load failed: %.*s", (int)message.length,
             message.str);
    return false_v;
  }
  state->additive_pending[slot] = true_v;
  return true_v;
}

/* Activates additive loads that resolved and reports ones that failed. */
vkr_internal void sample_additive_poll(VkrStandardSceneRuntime *application) {
  for (uint32_t slot = 0; slot < VKR_SCENE_ADDITIVE_MAX; ++slot) {
    if (!state->additive_pending[slot]) {
      continue;
    }
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    const VkrResourceLoadState load =
        vkr_resource_system_get_state(&state->additive_resources[slot], &error);
    if (load == VKR_RESOURCE_LOAD_STATE_FAILED ||
        load == VKR_RESOURCE_LOAD_STATE_CANCELED) {
      const String8 message = vkr_renderer_get_error_string(error);
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Additive scene load failed: %.*s", (int)message.length,
               message.str);
      sample_additive_remove(application, slot);
      continue;
    }
    VkrResourceHandleInfo resolved = {0};
    if (load != VKR_RESOURCE_LOAD_STATE_READY ||
        !vkr_resource_system_try_get_resolved(&state->additive_resources[slot],
                                              &resolved) ||
        !resolved.as.scene) {
      continue;
    }
    state->additive_resources[slot] = resolved;
    state->additive_handles[slot] = resolved.as.scene;
    state->additive_pending[slot] = false_v;
    VkrScene *scene = vkr_scene_handle_get_scene(resolved.as.scene);
    vkr_scene_physics_set_paused(scene, true_v);
    sample_physics_attach(application, scene, false_v);
    (void)vkr_scene_resolve_world(scene);
    vkr_scene_edit_reset(&state->additive_edits[slot], &state->edit_allocator,
                         1u);
    if (state->additive_sidecars[slot][0]) {
      (void)vkr_scene_edit_load(
          &state->additive_edits[slot], scene,
          string8_create_from_cstr(
              (const uint8_t *)state->additive_sidecars[slot],
              strlen(state->additive_sidecars[slot])));
    }
    sample_additive_publish(application);
  }
}

vkr_internal void sample_additive_save(void) {
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (sample_additive_dirty(i) && state->additive_sidecars[i][0]) {
      (void)vkr_scene_edit_save(
          &state->additive_edits[i],
          vkr_scene_handle_get_scene(state->additive_handles[i]),
          string8_create_from_cstr((const uint8_t *)state->additive_sidecars[i],
                                   strlen(state->additive_sidecars[i])));
    }
  }
}

/* Switches or unloads the scene the UI selected, unless unsaved edits or an
   invalid path block it. */
vkr_internal void vkr_standard_scene_runtime_apply_scene_request(
    VkrStandardSceneRuntime *application,
    const VkrSampleSceneRequest *scene_request,
    VkrSceneEditRequest *scene_edit) {
  if (scene_request->add) {
    (void)sample_additive_add(application, scene_request->path,
                              scene_request->sidecar_path);
    return;
  }
  if (scene_request->remove) {
    const uint32_t slot = scene_request->container - 1u;
    if (slot >= VKR_SCENE_ADDITIVE_MAX) {
      return;
    }
    if (sample_additive_dirty(slot) && !scene_request->discard_edits) {
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Save or discard that scene's edits before removing it.");
      return;
    }
    sample_additive_remove(application, slot);
    return;
  }
  if (scene_request->select || scene_request->unload) {
    vkr_standard_scene_runtime_finish_gizmo_edit(application);
    if (!scene_request->discard_edits &&
        state->edits.revision != state->edits.saved_revision) {
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Save or discard scene edits before switching.");
    } else if (scene_request->path.length >=
                   sizeof(state->scene_path_storage) ||
               (scene_request->asset_root.length &&
                !scene_request->asset_root.str) ||
               scene_request->asset_root.length >=
                   sizeof(state->physics_asset_root) ||
               scene_request->sidecar_path.length >=
                   sizeof(state->sidecar_path) ||
               (scene_request->select && !scene_request->path.length)) {
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Invalid scene selection or path is too long.");
    } else {
      /* Snapshot borrowed paths before unloading resets scene-owned storage. */
      char next_path[1024] = {0};
      char next_sidecar[1024] = {0};
      char next_asset_root[1024] = {0};
      if (scene_request->asset_root.length) {
        MemCopy(next_asset_root, scene_request->asset_root.str,
                scene_request->asset_root.length);
      } else {
        snprintf(next_asset_root, sizeof(next_asset_root), "%s",
                 vkr_content_root());
      }
      if (scene_request->path.length) {
        MemCopy(next_path, scene_request->path.str, scene_request->path.length);
      }
      if (scene_request->sidecar_path.length) {
        MemCopy(next_sidecar, scene_request->sidecar_path.str,
                scene_request->sidecar_path.length);
      }
      sample_warm_hold_take(application,
                            string8_create_from_cstr((const uint8_t *)next_path,
                                                     string_length(next_path)));
      vkr_standard_scene_runtime_unload_scene_system(application);
      MemCopy(state->scene_path_storage, next_path, sizeof(next_path));
      MemCopy(state->sidecar_path, next_sidecar, sizeof(next_sidecar));
      MemCopy(state->physics_asset_root, next_asset_root,
              sizeof(next_asset_root));
      state->scene_path = string8_create_from_cstr(
          (const uint8_t *)state->scene_path_storage, strlen(next_path));
      state->scene_status[0] = '\0';
      scene_edit->action = VKR_SCENE_EDIT_NONE;
      if (scene_request->select) {
        application->editor_viewport.scene_rendering_stopped = false_v;
        vkr_standard_scene_runtime_init_scene_system(application);
      }
    }
  }
}

/* The loaded container with world id `world` and its journal. */
static bool8_t sample_container_edits(VkrStandardSceneRuntime *application,
                                      uint32_t world, VkrScene **out_scene,
                                      VkrSceneEditState **out_edits) {
  VkrScene *scene = NULL;
  VkrSceneEditState *edits = NULL;
  if (world == VKR_SCENE_WORLD_ROOT_ID) {
    scene = application->world_scene;
    edits = &state->world_edits;
  } else if (world == 0u) {
    scene = application->active_scene;
    edits = &state->edits;
  } else if (world <= VKR_SCENE_ADDITIVE_MAX &&
             state->additive_handles[world - 1u]) {
    scene = vkr_scene_handle_get_scene(state->additive_handles[world - 1u]);
    edits = &state->additive_edits[world - 1u];
  }
  *out_scene = scene;
  *out_edits = edits;
  return scene != NULL;
}

/* Structure requests (ADR-076) go to the container their entity or parent
   lives in; a root creation names its container. Returns false for other
   actions. */
static bool8_t sample_structure_edit(VkrStandardSceneRuntime *application,
                                     const VkrSceneEditRequest *request) {
  const VkrSceneEditAction action = request->action;
  if (action != VKR_SCENE_EDIT_ADD_COMPONENT &&
      action != VKR_SCENE_EDIT_REMOVE_COMPONENT &&
      action != VKR_SCENE_EDIT_REPLACE_COMPONENT &&
      action != VKR_SCENE_EDIT_CREATE && action != VKR_SCENE_EDIT_DELETE &&
      action != VKR_SCENE_EDIT_DUPLICATE && action != VKR_SCENE_EDIT_REPARENT &&
      action != VKR_SCENE_EDIT_APPLY_SCENE_SETTINGS) {
    return false_v;
  }
  const VkrEntityId anchor =
      action == VKR_SCENE_EDIT_CREATE ? request->parent : request->entity;
  const uint32_t world = anchor.u64 ? anchor.parts.world : request->container;
  VkrScene *scene = NULL;
  VkrSceneEditState *edits = NULL;
  if (!sample_container_edits(application, world, &scene, &edits)) {
    return true_v;
  }
  switch (action) {
  case VKR_SCENE_EDIT_ADD_COMPONENT:
    (void)vkr_scene_edit_add_component(edits, scene, request->entity,
                                       request->values.component_type,
                                       request->values.component);
    break;
  case VKR_SCENE_EDIT_REMOVE_COMPONENT:
    (void)vkr_scene_edit_remove_component(edits, scene, request->entity,
                                          request->values.component_type);
    break;
  case VKR_SCENE_EDIT_REPLACE_COMPONENT:
    (void)vkr_scene_edit_replace_component(
        edits, scene, request->entity, request->replaced_type,
        request->values.component_type, request->values.component);
    break;
  case VKR_SCENE_EDIT_CREATE: {
    /* A root object without a placement appears in front of the camera. */
    VkrSceneEditValues values = request->values;
    const VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &application->camera_system, application->active_camera);
    if (!(values.fields & VKR_SCENE_EDIT_TRANSFORM) && !request->parent.u64 &&
        camera) {
      values.fields |= VKR_SCENE_EDIT_TRANSFORM;
      values.position =
          vec3_add(camera->position, vec3_scale(camera->forward, 5.0f));
      values.rotation = vkr_quat_identity();
      values.scale = vec3_one();
    }
    const VkrEntityId entity =
        vkr_scene_edit_create(edits, scene, request->parent, &values);
    if (entity.u64) {
      vkr_standard_scene_runtime_cancel_gizmo_pick(application);
      vkr_standard_scene_runtime_clear_gizmo_handles(application);
      state->gizmo_drag.active = false_v;
      state->selected_entity = entity;
      state->has_selection = true_v;
    }
    break;
  }
  case VKR_SCENE_EDIT_DELETE:
    if (vkr_scene_edit_delete(edits, scene, request->entity) &&
        state->selected_entity.u64 == request->entity.u64) {
      vkr_standard_scene_runtime_clear_gizmo_handles(application);
      state->gizmo_drag.active = false_v;
      state->selected_entity = VKR_ENTITY_ID_INVALID;
      state->has_selection = false_v;
    }
    break;
  case VKR_SCENE_EDIT_DUPLICATE: {
    /* The copy takes the selection, so the next drag moves it. */
    const VkrEntityId copy = vkr_scene_edit_duplicate(
        edits, scene, request->entity, &request->values.ref);
    if (copy.u64) {
      vkr_standard_scene_runtime_cancel_gizmo_pick(application);
      vkr_standard_scene_runtime_clear_gizmo_handles(application);
      state->gizmo_drag.active = false_v;
      state->selected_entity = copy;
      state->has_selection = true_v;
    }
    break;
  }
  case VKR_SCENE_EDIT_REPARENT:
    (void)vkr_scene_edit_reparent(edits, scene, request->entity,
                                  request->parent);
    break;
  case VKR_SCENE_EDIT_APPLY_SCENE_SETTINGS:
    (void)vkr_scene_edit_apply_scene_settings(edits, scene,
                                              &request->scene_settings);
    break;
  default:
    break;
  }
  return true_v;
}

/* Polls the window capture the UI asked for; a finished or failed one is
   lent to the next build, which the release after that build ends. */
static void sample_capture_poll(VkrStandardSceneRuntime *application) {
  if (!state->capture_pending) {
    return;
  }
  VkrCapturePollResult poll = {0};
  const VkrCaptureStatus status = vkr_renderer_capture_poll(
      &application->renderer, state->capture_batch.request_id, &poll);
  /* Not yet submitted, or still rendering. */
  if (status == VKR_CAPTURE_STATUS_PENDING ||
      (status == VKR_CAPTURE_STATUS_NOT_FOUND &&
       application->capture_request == &state->capture_batch)) {
    return;
  }
  state->capture_pending = false_v;
  state->capture_ready =
      (VkrSampleCaptureReady){.token = state->capture_token, .failed = true_v};
  if (status == VKR_CAPTURE_STATUS_READY && poll.item_count >= 1u &&
      poll.items[0].data) {
    state->capture_ready.item = &poll.items[0];
    state->capture_ready.failed = false_v;
  } else if (status == VKR_CAPTURE_STATUS_READY ||
             status == VKR_CAPTURE_STATUS_FAILED) {
    vkr_renderer_capture_release(&application->renderer,
                                 state->capture_batch.request_id);
  }
  state->capture_ready_valid = true_v;
}

/* Ends the capture lent to this build and starts the one it asked for. The
   harness owns the renderer's capture slot when it runs; the UI then gets a
   failure. */
static void sample_capture_after_build(VkrStandardSceneRuntime *application,
                                       const VkrSampleCaptureRequest *request) {
  if (state->capture_ready_valid) {
    if (!state->capture_ready.failed) {
      vkr_renderer_capture_release(&application->renderer,
                                   state->capture_batch.request_id);
    }
    state->capture_ready_valid = false_v;
  }
  if (!request->request || state->capture_pending) {
    return;
  }
  const VkrCaptureChannelId channel = vkr_renderer_capture_channel_from_name(
      request->scene_hdr ? "hdr_post_transmission" : "final_color");
  if (application->capture_request || channel == VKR_CAPTURE_CHANNEL_INVALID) {
    state->capture_ready =
        (VkrSampleCaptureReady){.token = request->token, .failed = true_v};
    state->capture_ready_valid = true_v;
    return;
  }
  state->capture_item = (VkrCaptureItemRequest){.channel = channel};
  /* The high bit keeps editor captures apart from the harness's ids. */
  state->capture_batch = (VkrCaptureBatchRequest){
      .request_id = (UINT64_C(1) << 63) | ++state->capture_serial,
      .items = &state->capture_item,
      .item_count = 1u,
  };
  state->capture_token = request->token;
  state->capture_pending = true_v;
  application->capture_request = &state->capture_batch;
}

/* Applies one batch item to the container's journal. A CREATE reports its
   entity in `out_created`. */
static bool8_t sample_batch_item(VkrStandardSceneRuntime *application,
                                 VkrScene *scene, VkrSceneEditState *edits,
                                 const VkrSceneEditRequest *request,
                                 VkrEntityId *out_created) {
  switch (request->action) {
  case VKR_SCENE_EDIT_APPLY:
    return vkr_scene_edit_apply(edits, scene, request->entity,
                                &request->values);
  case VKR_SCENE_EDIT_ADD_COMPONENT:
    return vkr_scene_edit_add_component(edits, scene, request->entity,
                                        request->values.component_type,
                                        request->values.component);
  case VKR_SCENE_EDIT_REMOVE_COMPONENT:
    return vkr_scene_edit_remove_component(edits, scene, request->entity,
                                           request->values.component_type);
  case VKR_SCENE_EDIT_REPLACE_COMPONENT:
    return vkr_scene_edit_replace_component(
        edits, scene, request->entity, request->replaced_type,
        request->values.component_type, request->values.component);
  case VKR_SCENE_EDIT_CREATE:
    *out_created =
        vkr_scene_edit_create(edits, scene, request->parent, &request->values);
    return out_created->u64 != 0u;
  case VKR_SCENE_EDIT_DELETE:
    if (!vkr_scene_edit_delete(edits, scene, request->entity)) {
      return false_v;
    }
    if (state->selected_entity.u64 == request->entity.u64) {
      vkr_standard_scene_runtime_clear_gizmo_handles(application);
      state->gizmo_drag.active = false_v;
      state->selected_entity = VKR_ENTITY_ID_INVALID;
      state->has_selection = false_v;
    }
    return true_v;
  case VKR_SCENE_EDIT_DUPLICATE:
    *out_created = vkr_scene_edit_duplicate(edits, scene, request->entity,
                                            &request->values.ref);
    return out_created->u64 != 0u;
  case VKR_SCENE_EDIT_REPARENT:
    return vkr_scene_edit_reparent(edits, scene, request->entity,
                                   request->parent);
  case VKR_SCENE_EDIT_TERRAIN:
    return vkr_scene_edit_terrain(edits, scene, request->entity,
                                  &request->terrain, 0u);
  default:
    snprintf(edits->status, sizeof(edits->status),
             "This edit cannot join a batch.");
    return false_v;
  }
}

/* Applies a batch as one journal group, rolling it back on the first failed
   edit, or reverts a closed group (ADR-084,
   phase 0). Edits wait while the simulation runs, as Details edits do. */
static void sample_edit_batch(VkrStandardSceneRuntime *application,
                              const VkrSampleEditBatchRequest *batch) {
  if (!batch->token) {
    return;
  }
  VkrSampleEditBatchResult *result = &state->edit_batch_result;
  if (batch->forced_result) {
    *result = *batch->forced_result;
    result->token = batch->token;
    return;
  }
  MemZero(result, sizeof(*result));
  result->token = batch->token;
  result->failed_index = UINT32_MAX;
  VkrScene *scene = NULL;
  VkrSceneEditState *edits = NULL;
  if (!sample_container_edits(application, batch->container, &scene, &edits)) {
    snprintf(result->message, sizeof(result->message),
             "That scene is not loaded.");
    return;
  }
  if (application->editor_viewport.simulation_running ||
      sample_origin_rebased(application)) {
    snprintf(result->message, sizeof(result->message),
             "Stop the simulation before editing the scene.");
    return;
  }
  if (state->gizmo_drag.active) {
    vkr_standard_scene_runtime_finish_gizmo_edit(application);
  }
  if (batch->revert_group) {
    result->group = batch->revert_group;
    result->ok = vkr_scene_edit_group_revert(edits, scene, batch->revert_group,
                                             &result->conflict);
    if (state->has_selection &&
        !vkr_scene_entity_alive(scene, state->selected_entity) &&
        state->selected_entity.parts.world == batch->container) {
      vkr_standard_scene_runtime_clear_gizmo_handles(application);
      state->selected_entity = VKR_ENTITY_ID_INVALID;
      state->has_selection = false_v;
    }
    snprintf(result->message, sizeof(result->message), "%s", edits->status);
    return;
  }
  if (batch->count > VKR_SAMPLE_EDIT_BATCH_MAX || !batch->items) {
    snprintf(result->message, sizeof(result->message),
             "A batch holds at most %u edits.", VKR_SAMPLE_EDIT_BATCH_MAX);
    return;
  }
  result->group = vkr_scene_edit_group_begin(edits);
  if (!result->group) {
    snprintf(result->message, sizeof(result->message),
             "Another edit group is open.");
    return;
  }
  for (uint32_t i = 0; i < batch->count; ++i) {
    VkrSceneEditRequest request = batch->items[i].request;
    const int32_t entity_ref = batch->items[i].entity_ref;
    const int32_t parent_ref = batch->items[i].parent_ref;
    /* References name earlier creations only. */
    if ((entity_ref >= 0 && (uint32_t)entity_ref >= i) ||
        (parent_ref >= 0 && (uint32_t)parent_ref >= i)) {
      snprintf(edits->status, sizeof(edits->status),
               "Edit %u refers to a later edit.", i);
      result->failed_index = i;
      break;
    }
    if (entity_ref >= 0) {
      request.entity = result->created[entity_ref];
    }
    if (parent_ref >= 0) {
      request.parent = result->created[parent_ref];
    }
    if ((entity_ref >= 0 && !request.entity.u64) ||
        (parent_ref >= 0 && !request.parent.u64)) {
      snprintf(edits->status, sizeof(edits->status),
               "Edit %u refers to an edit that created nothing.", i);
      result->failed_index = i;
      break;
    }
    if (!sample_batch_item(application, scene, edits, &request,
                           &result->created[i])) {
      result->failed_index = i;
      break;
    }
  }
  if (result->failed_index != UINT32_MAX) {
    snprintf(result->message, sizeof(result->message), "%s", edits->status);
    (void)vkr_scene_edit_group_rollback(edits, scene);
    MemZero(result->created, sizeof(result->created));
    return;
  }
  vkr_scene_edit_group_end(edits);
  result->ok = true_v;
  snprintf(result->message, sizeof(result->message), "Applied %u edits.",
           batch->count);
}

/* Moves `camera` so the box fills the view: back along its forward axis in
   perspective, centred with a matching span in an orthographic view. */
static void sample_frame_box(VkrCamera *camera, Vec3 lower, Vec3 upper) {
  Vec3 target = vec3_scale(vec3_add(lower, upper), 0.5f);
  float32_t radius = Max(0.25f, vec3_length(vec3_sub(upper, lower)) * 0.5f);
  float32_t aspect = camera->cached_window_height
                         ? (float32_t)camera->cached_window_width /
                               camera->cached_window_height
                         : 1.0f;
  float32_t half_angle =
      atanf(tanf(camera->zoom * 0.0087266463f) * Min(1.0f, aspect));
  float32_t distance = radius / Max(0.01f, sinf(half_angle)) * 1.1f;
  if (camera->type == VKR_CAMERA_TYPE_ORTHOGRAPHIC) {
    const float32_t half_height = radius * 1.1f / Min(1.0f, aspect);
    camera->left_clip = -half_height * aspect;
    camera->right_clip = half_height * aspect;
    camera->bottom_clip = -half_height;
    camera->top_clip = half_height;
    camera->projection_dirty = true_v;
    distance = 0.5f * (camera->near_clip + camera->far_clip);
  }
  camera->position = vec3_sub(target, vec3_scale(camera->forward, distance));
  camera->view_dirty = true_v;
}

vkr_internal void vkr_standard_scene_runtime_apply_scene_edit(
    VkrStandardSceneRuntime *application,
    const VkrSceneEditRequest *scene_edit) {
  /* A rebased world holds positions the documents do not (ADR-086). */
  if (scene_edit->action != VKR_SCENE_EDIT_NONE &&
      scene_edit->action != VKR_SCENE_EDIT_SELECT &&
      scene_edit->action != VKR_SCENE_EDIT_FRAME &&
      sample_origin_rebased(application)) {
    snprintf(state->edits.status, sizeof(state->edits.status),
             "Reset the simulation to edit or save; the origin is rebased.");
    return;
  }
  if (sample_structure_edit(application, scene_edit)) {
    return;
  }
  /* The World owns the shared collision layers when loaded (ADR-076). */
  if (scene_edit->action == VKR_SCENE_EDIT_APPLY_COLLISION_LAYERS &&
      application->world_scene) {
    (void)vkr_scene_edit_apply_collision_layers(&state->world_edits,
                                                application->world_scene,
                                                scene_edit->collision_layers);
    return;
  }
  /* Root World and additive entities carry their container's world id;
     their edits use that container's journal (ADR-076). */
  VkrScene *world = application->world_scene;
  VkrScene *container = NULL;
  VkrSceneEditState *container_edits = NULL;
  const uint32_t additive = sample_additive_slot(scene_edit->entity);
  if (world && scene_edit->entity.u64 &&
      scene_edit->entity.parts.world == VKR_SCENE_WORLD_ROOT_ID) {
    container = world;
    container_edits = &state->world_edits;
  } else if (additive < VKR_SCENE_ADDITIVE_MAX) {
    container = vkr_scene_handle_get_scene(state->additive_handles[additive]);
    container_edits = &state->additive_edits[additive];
  }
  if (container && (scene_edit->action == VKR_SCENE_EDIT_SELECT ||
                    scene_edit->action == VKR_SCENE_EDIT_APPLY)) {
    if (scene_edit->action == VKR_SCENE_EDIT_SELECT &&
        vkr_scene_entity_alive(container, scene_edit->entity)) {
      vkr_standard_scene_runtime_cancel_gizmo_pick(application);
      vkr_standard_scene_runtime_clear_gizmo_handles(application);
      state->gizmo_drag.active = false_v;
      state->selected_entity = scene_edit->entity;
      state->has_selection = true_v;
    } else if (scene_edit->action == VKR_SCENE_EDIT_APPLY) {
      (void)vkr_scene_edit_apply_gesture(
          container_edits, container, scene_edit->entity, &scene_edit->values,
          scene_edit->gesture);
    }
    return;
  }
  /* Undo and redo follow the most recent edit across every journal. */
  if (scene_edit->action == VKR_SCENE_EDIT_UNDO ||
      scene_edit->action == VKR_SCENE_EDIT_REDO) {
    const bool8_t redo = scene_edit->action == VKR_SCENE_EDIT_REDO;
    VkrSceneEditState *best_edits =
        application->active_scene ? &state->edits : NULL;
    VkrScene *best_scene = application->active_scene;
    uint64_t best =
        best_edits ? vkr_scene_edit_next_sequence(best_edits, redo) : 0u;
    VkrSceneEditState *candidates[VKR_SCENE_ADDITIVE_MAX + 1u];
    VkrScene *candidate_scenes[VKR_SCENE_ADDITIVE_MAX + 1u];
    uint32_t candidate_count = 0u;
    if (world) {
      candidates[candidate_count] = &state->world_edits;
      candidate_scenes[candidate_count++] = world;
    }
    for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
      if (state->additive_handles[i]) {
        candidates[candidate_count] = &state->additive_edits[i];
        candidate_scenes[candidate_count++] =
            vkr_scene_handle_get_scene(state->additive_handles[i]);
      }
    }
    for (uint32_t i = 0; i < candidate_count; ++i) {
      const uint64_t next = vkr_scene_edit_next_sequence(candidates[i], redo);
      if (next && (!best || (redo ? next < best : next > best))) {
        best = next;
        best_edits = candidates[i];
        best_scene = candidate_scenes[i];
      }
    }
    if (best_edits != &state->edits) {
      if (best_edits) {
        (void)vkr_scene_edit_undo(best_edits, best_scene, redo);
      }
      return;
    }
  }
  if (scene_edit->action == VKR_SCENE_EDIT_SAVE) {
    sample_world_save();
    sample_additive_save();
  }
  VkrScene *scene = application->active_scene;
  /* Framing measures the entity in its own container. */
  if (container && scene_edit->action == VKR_SCENE_EDIT_FRAME) {
    scene = container;
  }
  if (scene) {
    switch (scene_edit->action) {
    case VKR_SCENE_EDIT_SELECT:
      if (vkr_scene_entity_alive(scene, scene_edit->entity)) {
        vkr_standard_scene_runtime_cancel_gizmo_pick(application);
        vkr_standard_scene_runtime_clear_gizmo_handles(application);
        state->gizmo_drag.active = false_v;
        state->selected_entity = scene_edit->entity;
        state->has_selection = true_v;
      }
      break;
    case VKR_SCENE_EDIT_APPLY_COLLISION_LAYERS:
      (void)vkr_scene_edit_apply_collision_layers(&state->edits, scene,
                                                  scene_edit->collision_layers);
      break;
    case VKR_SCENE_EDIT_APPLY_PHYSICS_BATCH:
      (void)vkr_scene_edit_apply_physics_batch(&state->edits, scene,
                                               scene_edit->physics_batch,
                                               scene_edit->physics_batch_count);
      break;
    case VKR_SCENE_EDIT_APPLY:
      (void)vkr_scene_edit_apply_gesture(
          &state->edits, scene, scene_edit->entity, &scene_edit->values,
          scene_edit->gesture);
      break;
    case VKR_SCENE_EDIT_TERRAIN:
      if (!application->editor_viewport.simulation_running) {
        (void)vkr_scene_edit_terrain(&state->edits, scene, scene_edit->entity,
                                     &scene_edit->terrain, scene_edit->gesture);
      }
      break;
    case VKR_SCENE_EDIT_PARTITION:
      sample_partition_request(scene, scene_edit);
      break;
    case VKR_SCENE_EDIT_UNDO:
    case VKR_SCENE_EDIT_REDO:
      (void)vkr_scene_edit_undo(&state->edits, scene,
                                scene_edit->action == VKR_SCENE_EDIT_REDO);
      break;
    case VKR_SCENE_EDIT_SAVE:
      if (state->ui.save_scene_edits) {
        (void)state->ui.save_scene_edits(state->ui.state, &state->edits, scene,
                                         state->scene_path);
      } else {
        (void)vkr_scene_edit_save(&state->edits, scene,
                                  string8_create((uint8_t *)state->sidecar_path,
                                                 strlen(state->sidecar_path)));
      }
      break;
    case VKR_SCENE_EDIT_FRAME: {
      const SceneTransform *tr = vkr_entity_get_component(
          scene->world, scene_edit->entity, scene->comp_transform);
      VkrCamera *camera = vkr_camera_registry_get_by_handle(
          &application->camera_system, application->active_camera);
      if (tr && camera) {
        Vec3 lower = mat4_position(tr->world), upper = lower;
        for (uint32_t i = 0; i < scene->topo_count; i++) {
          VkrEntityId candidate = scene->topo_order[i], ancestor = candidate;
          while (ancestor.u64 && ancestor.u64 != scene_edit->entity.u64) {
            const SceneTransform *parent =
                vkr_entity_get_component_unchecked_const(scene->world, ancestor,
                                                         scene->comp_transform);
            ancestor = parent->parent;
          }
          if (!ancestor.u64)
            continue;
          // Topology includes transform-only glTF nodes as well as meshes.
          const SceneMeshRenderer *mesh = vkr_entity_get_component(
              scene->world, candidate, scene->comp_mesh_renderer);
          if (!mesh)
            continue;
          VkrMeshInstance *instance = vkr_mesh_manager_get_instance(
              &application->assets.mesh_manager, mesh->instance);
          if (!instance || !instance->bounds_valid)
            continue;
          Vec3 radius = vec3_new(instance->bounds_world_radius,
                                 instance->bounds_world_radius,
                                 instance->bounds_world_radius);
          Vec3 lo = vec3_sub(instance->bounds_world_center, radius),
               hi = vec3_add(instance->bounds_world_center, radius);
          lower = vec3_new(Min(lower.x, lo.x), Min(lower.y, lo.y),
                           Min(lower.z, lo.z));
          upper = vec3_new(Max(upper.x, hi.x), Max(upper.y, hi.y),
                           Max(upper.z, hi.z));
        }
        sample_frame_box(camera, lower, upper);
      }
      break;
    }
    default:
      break;
    }
  }
}

vkr_internal void vkr_standard_scene_runtime_apply_transport_action(
    VkrStandardSceneRuntime *application,
    VkrSampleTransportAction transport_action) {
  switch (transport_action) {
  case VKR_SAMPLE_TRANSPORT_START_SIMULATION:
    application->editor_viewport.simulation_running = true_v;
    if (vkr_script_host_active(&state->scripts)) {
      vkr_window_set_mouse_capture(&application->host.window, true_v);
    }
    break;
  case VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION:
    application->editor_viewport.simulation_running = false_v;
    if (vkr_script_host_active(&state->scripts)) {
      vkr_window_set_mouse_capture(&application->host.window, false_v);
    }
    break;
  case VKR_SAMPLE_TRANSPORT_STEP_SIMULATION: {
    const char *error = NULL;
    /* Stepping from a reset boundary starts the session, as running does. */
    VkrScene *simulated = sample_simulated_scene(application, NULL);
    if (!application->editor_viewport.simulation_running && simulated &&
        !state->scripts.started && !state->script_start_attempted) {
      sample_scripts_start(application);
    }
    if (!application->editor_viewport.simulation_running && simulated &&
        !vkr_scene_physics_step(simulated, &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Physics step failed.");
    }
    break;
  }
  case VKR_SAMPLE_TRANSPORT_RESET_SIMULATION: {
    const char *error = NULL;
    application->editor_viewport.simulation_running = false_v;
    /* Reset ends the session: spawned objects leave and the next run starts
       a new one from the authored scene. */
    sample_scripts_stop(application);
    vkr_window_set_mouse_capture(&application->host.window, false_v);
    VkrScene *simulated = sample_simulated_scene(application, NULL);
    vkr_scene_physics_set_paused(simulated, true_v);
    sample_rebase_restore(application);
    sample_partition_session(application, false_v);
    if (simulated && !vkr_scene_physics_reset(simulated, &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Physics reset failed.");
    } else {
      application->editor_viewport.simulation_time = 0.0;
    }
    break;
  }
  case VKR_SAMPLE_TRANSPORT_TOGGLE_PHYSICS: {
    const char *error = NULL;
    VkrScene *simulated = sample_simulated_scene(application, NULL);
    if (simulated &&
        !vkr_scene_physics_set_disabled(
            simulated, !vkr_scene_physics_is_disabled(simulated), &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Physics override failed.");
    }
    break;
  }
  case VKR_SAMPLE_TRANSPORT_START_RENDERING:
    application->editor_viewport.scene_error = VKR_RENDERER_ERROR_NONE;
    application->editor_viewport.scene_rendering_stopped = false_v;
    break;
  case VKR_SAMPLE_TRANSPORT_STOP_RENDERING:
    application->editor_viewport.scene_rendering_stopped = true_v;
    if (vkr_window_is_mouse_captured(&application->host.window))
      vkr_window_set_mouse_capture(&application->host.window, false_v);
    state->free_camera_held = false_v;
    state->free_camera_use_gamepad = false_v;
    vkr_picking_cancel(&application->picking);
    state->gizmo_drag.active = false_v;
    state->gizmo_drag.pending_pick = false_v;
    state->gizmo_drag.pending_select = false_v;
    state->gizmo_hover_pending = false_v;
    vkr_standard_scene_runtime_clear_gizmo_handles(application);
    break;
  case VKR_SAMPLE_TRANSPORT_TOGGLE_CAMERA:
    if (!vkr_standard_scene_runtime_editor_scene_rendering_stopped(
            application)) {
      const bool8_t captured =
          vkr_window_is_mouse_captured(&application->host.window);
      vkr_window_set_mouse_capture(&application->host.window, !captured);
      state->free_camera_held = false_v;
      state->scene_keyboard_focus = true_v;
      state->free_camera_use_gamepad = false_v;
    }
    break;
  case VKR_SAMPLE_TRANSPORT_NONE:
    break;
  }
}

vkr_internal void
vkr_standard_scene_runtime_update_ui(VkrStandardSceneRuntime *application,
                                     float64_t delta) {
  if (!application || !state || !state->input_state ||
      !application->ui_system.initialized) {
    return;
  }
  /* Scripted input joins the platform's before anything reads this frame. */
  if (state->ui.feed_input) {
    state->ui.feed_input(state->ui.state, state->input_state);
  }

  vkr_scene_physics_set_paused(
      sample_simulated_scene(application, NULL),
      !application->editor_viewport.simulation_running);

  if (application->editor_viewport.enabled) {
    /* Resolution recovery invalidates GPU picks, not scene-owned edits. */
    if (application->editor_viewport.scene_error != VKR_RENDERER_ERROR_NONE &&
        (state->gizmo_drag.pending_pick || state->gizmo_hover_pending))
      vkr_standard_scene_runtime_cancel_gizmo_pick(application);
    if (state->gizmo_edit_pending &&
        vkr_standard_scene_runtime_editor_scene_rendering_stopped(application))
      vkr_standard_scene_runtime_finish_gizmo_edit(application);
    const bool8_t command =
        input_key_shortcut_modifier(state->input_state, KEY_P);
    const bool8_t escape =
        input_key_just_pressed(state->input_state, KEY_ESCAPE);
    state->escape_taken =
        escape && (vkr_window_is_mouse_captured(&application->host.window) ||
                   state->gizmo_drag.active || state->gizmo_drag.pending_pick ||
                   state->gizmo_edit_pending);
    if (escape ||
        (command && input_key_just_pressed(state->input_state, KEY_P))) {
      vkr_window_set_mouse_capture(&application->host.window, false_v);
      state->free_camera_held = false_v;
      state->free_camera_use_gamepad = false_v;
    }
    if (escape) {
      vkr_standard_scene_runtime_cancel_gizmo_pick(application);
      vkr_standard_scene_runtime_clear_gizmo_handles(application);
    }
    if (escape && state->gizmo_edit_pending)
      vkr_standard_scene_runtime_cancel_gizmo_edit(application);
  }

  /* Complete a released drag before UI commands can change selection or stop
     scene rendering. Picking may be suppressed by that UI interaction. */
  if ((state->gizmo_drag.active || state->gizmo_drag.pending_pick) &&
      !input_is_button_down(state->input_state, BUTTON_LEFT)) {
    int32_t release_x, release_y;
    input_get_mouse_position(state->input_state, &release_x, &release_y);
    const VkrViewportHitInfo release_info =
        vkr_standard_scene_runtime_get_viewport_hit_info(application, release_x,
                                                         release_y);
    if (!state->gizmo_drag.released)
      vkr_standard_scene_runtime_capture_gizmo_release(&release_info);
    if (state->gizmo_drag.active) {
      vkr_standard_scene_runtime_update_gizmo_drag(application, &release_info);
      vkr_standard_scene_runtime_finish_gizmo_edit(application);
    }
  }

  const VkrUiTrack root_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig root = vkr_ui_panel_config_default();
  root.columns = &root_track;
  root.column_count = 1u;
  root.rows = &root_track;
  root.row_count = 1u;
  if (!vkr_ui_begin(&application->ui_system, application->frame_allocator,
                    vkr_standard_scene_runtime_is_windowed(application)
                        ? &application->host.window
                        : NULL,
                    application->target_window_width,
                    application->target_window_height, state->input_state,
                    vkr_window_is_mouse_captured(&application->host.window),
                    delta, &root)) {
    application->ui_capture = (VkrUiInputCapture){0};
    return;
  }

  if (state->graphics_dirty &&
      vkr_platform_get_absolute_time() - state->graphics_changed_at >= .25)
    sample_graphics_save();
  /* No pending notice is the normal case, and the empty-tolerant constructor
     is the one that preserves it. */
  state->graphics.message =
      string8_create_from_cstr((const uint8_t *)state->graphics_message,
                               strlen(state->graphics_message));
  sample_capture_poll(application);
  VkrSampleUiRequests requests = {
      .transport_action = VKR_SAMPLE_TRANSPORT_NONE,
      .close_response = VKR_SAMPLE_CLOSE_NONE,
      .scene_maximized = application->editor_viewport.scene_maximized,
  };
  state->view_state.render_mode = application->globals.render_mode;
  state->view_state.gizmo_tool = (uint32_t)application->gizmo_system.tool;
  /* View-aligned axes are reserved; they show as world. */
  state->view_state.gizmo_space =
      application->gizmo_system.space == VKR_GIZMO_SPACE_LOCAL
          ? VKR_GIZMO_SPACE_LOCAL
          : VKR_GIZMO_SPACE_WORLD;
  {
    const VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &application->camera_system, application->active_camera);
    state->view_state.camera_speed = camera ? camera->speed : 1.0f;
    state->view_state.camera_sensitivity =
        camera ? camera->sensitivity : VKR_DEFAULT_CAMERA_SENSITIVITY;
  }
  state->modal = false_v;
  application->editor_viewport.scene_backdrop_blur = false_v;
  application->animation_preview = (VkrAnimationPreviewRequest){0};
  application->editor_viewport.dock_capture =
      vkr_standard_scene_runtime_build_ui_frame(application, &requests);
  state->gizmo_recorded.count = 0u;
  application->ui_capture = vkr_ui_end(&application->ui_system);
  application->editor_viewport.scene_maximized =
      application->editor_viewport.enabled &&
      !application->editor_viewport.scene_only && requests.scene_maximized;
  if (vkr_application_host_is_windowed(&application->host))
    vkr_window_set_cursor(&application->host.window,
                          application->ui_system.cursor);
  sample_graphics_request(application, &requests.graphics_request);
  if (!state->graphics_present_live || !state->graphics_scale_live)
    sample_graphics_apply_display(application, &state->graphics.settings);
  sample_editor_state_apply(application, &requests.editor_state_request);
  sample_view_apply(application, &requests.view_request);
  if (requests.view_request.frame_box) {
    VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &application->camera_system, application->active_camera);
    if (camera) {
      sample_frame_box(camera, requests.view_request.frame_min,
                       requests.view_request.frame_max);
    }
  }
  if (requests.hide_request.apply) {
    state->hidden = requests.hide_request;
    state->hidden.count =
        Min(state->hidden.count, (uint32_t)VKR_SCENE_EDITOR_HIDDEN_MAX);
  }
  sample_show_filter_apply(application);
  if (requests.looks_changed) {
    VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX] = {
        application->active_scene,
        vkr_scene_handle_get_scene(state->world_handle)};
    for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
      scenes[2u + i] = vkr_scene_handle_get_scene(state->additive_handles[i]);
    }
    for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
      vkr_scene_brush_refresh_looks(scenes[i]);
    }
  }
  if (requests.custom_materials_reload)
    vkr_renderer_reload_custom_materials(&application->renderer);
  sample_grid_apply(application);
  if (requests.grid_fit_request.request) {
    sample_grid_fit_begin(application, requests.grid_fit_request.position_px);
  }
  sample_time_of_day_apply(application, &requests.time_of_day_request);
  /* A UI pick runs as a context click does, at the given pixel. */
  if (requests.pick_request.request) {
    state->context_click_pending = true_v;
    state->context_purpose = requests.pick_request.purpose;
    state->context_press_x = (int32_t)requests.pick_request.position_px.x;
    state->context_press_y = (int32_t)requests.pick_request.position_px.y;
  }
  if (requests.close_response != VKR_SAMPLE_CLOSE_NONE) {
    vkr_window_resolve_close(&application->host.window,
                             requests.close_response ==
                                 VKR_SAMPLE_CLOSE_CONFIRM);
  }
  if (requests.quit)
    vkr_standard_scene_runtime_close(application);
  application->ui_capture.mouse |=
      application->editor_viewport.dock_capture.mouse;
  if (application->editor_viewport.enabled && !application->ui_capture.text &&
      !state->modal && !requests.scene_shortcuts_blocked) {
    if (input_key_shortcut_modifier(state->input_state, KEY_S) &&
        input_key_just_pressed(state->input_state, KEY_S))
      requests.scene_edit.action = VKR_SCENE_EDIT_SAVE;
    if (input_key_shortcut_modifier(state->input_state, KEY_Z) &&
        input_key_just_pressed(state->input_state, KEY_Z))
      requests.scene_edit.action =
          (input_key_press_modifiers(state->input_state, KEY_Z) &
           VKR_INPUT_MOD_SHIFT)
              ? VKR_SCENE_EDIT_REDO
              : VKR_SCENE_EDIT_UNDO;
  }
  if (state->modal) {
    state->scene_keyboard_focus = false_v;
    application->ui_capture.mouse = true_v;
    application->ui_capture.keyboard = true_v;
    vkr_window_set_mouse_capture(&application->host.window, false_v);
  }
  sample_script_request(application, &requests.script_request,
                        &requests.scene_edit.values);
  vkr_standard_scene_runtime_apply_scene_request(
      application, &requests.scene_request, &requests.scene_edit);
  sample_world_request(application, &requests.world_request);
  if (state->gizmo_drag.active &&
      (requests.scene_edit.action != VKR_SCENE_EDIT_NONE ||
       requests.transport_action == VKR_SAMPLE_TRANSPORT_STOP_RENDERING))
    vkr_standard_scene_runtime_finish_gizmo_edit(application);
  if (requests.scene_edit.action == VKR_SCENE_EDIT_LOAD)
    vkr_standard_scene_runtime_init_scene_system(application);
  if (requests.scene_edit.action == VKR_SCENE_EDIT_UNLOAD ||
      requests.scene_edit.action == VKR_SCENE_EDIT_RELOAD) {
    if (state->edits.revision != state->edits.saved_revision) {
      snprintf(state->edits.status, sizeof(state->edits.status),
               "Save edits before reloading or unloading.");
      log_warn("Save editor overrides before reloading or unloading");
    } else {
      if (requests.scene_edit.action == VKR_SCENE_EDIT_RELOAD) {
        sample_warm_hold_take(application, state->scene_path);
      } else {
        sample_warm_hold_release(application, false_v);
      }
      vkr_standard_scene_runtime_unload_scene_system(application);
      if (requests.scene_edit.action == VKR_SCENE_EDIT_RELOAD)
        vkr_standard_scene_runtime_init_scene_system(application);
    }
  }
  vkr_standard_scene_runtime_apply_scene_edit(application,
                                              &requests.scene_edit);
  sample_edit_batch(application, &requests.edit_batch);
  sample_capture_after_build(application, &requests.capture_request);
  vkr_standard_scene_runtime_apply_transport_action(application,
                                                    requests.transport_action);
  vkr_scene_physics_set_paused(
      sample_simulated_scene(application, NULL),
      !application->editor_viewport.simulation_running);
  if (requests.physics_request.set_body_disabled && application->active_scene) {
    const char *error = NULL;
    if (!vkr_scene_physics_set_body_disabled(
            application->active_scene, requests.physics_request.entity,
            requests.physics_request.body_disabled, &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Body session mute rejected.");
    }
  }
  sample_io_request(application, &requests.io_request);
  if (requests.physics_request.apply_impulse && application->active_scene) {
    const char *error = NULL;
    if (!vkr_scene_physics_impulse(application->active_scene,
                                   requests.physics_request.entity,
                                   requests.physics_request.impulse,
                                   requests.physics_request.at_point
                                       ? &requests.physics_request.world_point
                                       : NULL,
                                   &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Impulse rejected.");
    }
  }
}

static void
vkr_standard_scene_runtime_project_ui(VkrStandardSceneRuntime *application,
                                      const VkrViewportMapping *mapping) {
  VkrSampleUiFrame frame = {
      .ui = &application->ui_system,
      .view_state = state->view_state,
      .mapping = *mapping,
      .mapping_valid = true_v,
      .view_projection =
          mat4_mul(application->globals.projection, application->globals.view),
      .scene = application->active_scene,
      .world = application->world_scene,
      .scene_generation = application->scene_generation,
      .scene_rendering_stopped =
          vkr_standard_scene_runtime_editor_scene_rendering_stopped(
              application),
  };
  /* Containers still loaded after this frame's edits; projection must not
     touch one removed since the UI build. */
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (state->additive_handles[i]) {
      frame.additive[i] =
          vkr_scene_handle_get_scene(state->additive_handles[i]);
    }
  }
  state->ui.project_scene(state->ui.state, &frame);
}

vkr_internal void
vkr_sample_runtime_update(void *state_ptr, VkrStandardSceneRuntime *application,
                          float64_t delta) {
  (void)state_ptr;
  vkr_standard_scene_runtime_update_ui(application, delta);
  if (!state->modal) {
    vkr_standard_scene_runtime_handle_input(application, delta);
  }

  /* Q/W/E/R belong to editor transform tools; the View menu selects render
   * modes. F6 cycles shadow diagnostics. */
  if (!application->ui_capture.keyboard &&
      input_is_key_up(state->input_state, KEY_F6) &&
      input_was_key_down(state->input_state, KEY_F6)) {
    application->shadow_debug_mode =
        (application->shadow_debug_mode + 1u) % 14u;
    log_debug("SHADOW DEBUG MODE: %u "
              "(0=off,1=cascades,2=factor,3=depth,4=map0,5=map1,6=map2,7=map3,"
              "8=map4,9=map5,10=map6,11=map7,12=frustum,13=camera)",
              application->shadow_debug_mode);
  }

  vkr_standard_scene_runtime_update_fps_text(application, delta);
  vkr_standard_scene_runtime_update_memory_text(application);
  if (application->editor_viewport.simulation_running)
    vkr_standard_scene_runtime_update_world_text(application);
  vkr_standard_scene_runtime_update_picking(application);
  const float64_t scene_delta =
      application->editor_viewport.simulation_running ? delta : 0.0;
  application->editor_viewport.simulation_time += scene_delta;
  vkr_standard_scene_runtime_update_scene(application, scene_delta);
  if (vkr_scene_physics_body_count(application->active_scene)) {
    application->editor_viewport.simulation_time =
        vkr_scene_physics_time(application->active_scene);
    const char *physics_error =
        vkr_scene_physics_error(application->active_scene);
    if (physics_error && physics_error[0]) {
      application->editor_viewport.simulation_running = false_v;
    }
  }
  vkr_standard_scene_runtime_update_ibl_validation_controls(application);
  vkr_standard_scene_runtime_poll_upload_wait_stats(application);
  vkr_standard_scene_runtime_dump_periodic_metrics(application);

  if (state->auto_close_enabled && !state->auto_close_requested &&
      application->host.clock.elapsed >= state->auto_close_after_seconds) {
    state->auto_close_requested = true_v;
    log_info("Auto-close threshold reached (%.2fs), shutting down app loop",
             state->auto_close_after_seconds);
    vkr_standard_scene_runtime_close(application);
  }
}

/* Allocates and fills the sample State from the parsed options. Returns
   false_v when the State allocation or the UI client fails; a UI failure
   first releases the stats arena it created. */
vkr_internal bool8_t vkr_sample_runtime_initialize_state(
    VkrStandardSceneRuntime *application,
    const VkrSampleRuntimeConfig *runtime_config,
    const VkrSampleRuntimeOptions *options) {
  state = arena_alloc(application->app_arena, sizeof(State),
                      ARENA_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate the sample runtime state");
    return false_v;
  }
  state->graphics = options->graphics;
  state->graphics_started = options->graphics.settings;
  state->graphics_dirty = false_v;
  state->graphics_changed_at = 0.0;
  state->graphics_message[0] = '\0';
  snprintf(state->graphics_path, sizeof(state->graphics_path), "%s",
           options->graphics_path);
  if (!options->graphics_loaded)
    snprintf(state->graphics_message, sizeof(state->graphics_message),
             "Saved settings were invalid. Defaults are in use.");
  sample_graphics_apply_live(application, &state->graphics.settings);
  state->stats_arena = arena_create(KB(1), KB(1));
  /* A headless run has no window to initialize input; synthetic input
     (Cmd ui.*) still dispatches through the host's events. */
  if (!application->host.window.input_state.event_manager) {
    application->host.window.input_state =
        input_init(&application->host.events);
  }
  state->input_state = &application->host.window.input_state;
  /* A headless run initializes its input after the settings applied. */
  state->input_state->invert_look_y = state->graphics.settings.invert_mouse_y;
  state->view_state = (VkrSampleViewState){
      .camera_view = VKR_SAMPLE_CAMERA_PERSPECTIVE,
      .render_mode = application->globals.render_mode,
      .grid_spacing = 1.0f,
      .camera_speed = 1.0f,
      .camera_sensitivity = VKR_DEFAULT_CAMERA_SENSITIVITY,
      .collision_display = 1u,
      /* A blank level shows its ground grid (ADR-076). */
      .grid_enabled = true_v,
  };
  state->app_arena = application->app_arena;
  state->event_arena = application->host.events.arena;
  state->event_manager = &application->host.events;
  state->fps_update_clock = vkr_clock_create();
  state->memory_update_clock = vkr_clock_create();
  state->fps_accumulated_time = 0.0;
  state->fps_frame_count = 0;
  state->current_fps = 0.0;
  state->current_frametime = 0.0;
  state->ui = runtime_config->ui;
  application->project_ui =
      state->ui.project_scene ? vkr_standard_scene_runtime_project_ui : NULL;
  snprintf(state->scene_path_storage, sizeof(state->scene_path_storage), "%s",
           runtime_config->project_managed ? "" : options->scene_path);
  state->scene_path =
      string8_create_from_cstr((const uint8_t *)state->scene_path_storage,
                               strlen(state->scene_path_storage));
  state->sidecar_path[0] = '\0';
  state->scene_status[0] = '\0';
  snprintf(state->physics_asset_root, sizeof(state->physics_asset_root), "%s",
           vkr_content_root());
  state->modal = runtime_config->project_managed;
  if (!runtime_config->project_managed) {
    const char *scene_path = options->scene_path;
    const bool8_t absolute = scene_path[0] == '/' || scene_path[0] == '\\' ||
                             (strlen(scene_path) > 1u && scene_path[1] == ':');
    snprintf(state->sidecar_path, sizeof(state->sidecar_path),
             "%s%s.editor.json", absolute ? "" : vkr_content_root(),
             scene_path);
  }
  /* Address space for the journals that grow (the primary's and the
     World's at their byte budget) plus a loading overlay; pages commit as
     journals grow, and a full pool makes the journal that asks drop its
     oldest steps. */
  if (!vkr_dmemory_create(MB(4), GB(2), &state->edit_memory)) {
    log_error("Failed to reserve the scene edit memory");
    arena_destroy(state->stats_arena);
    state->stats_arena = NULL;
    return false_v;
  }
  state->edit_allocator = (VkrAllocator){.ctx = &state->edit_memory};
  vkr_dmemory_allocator_create(&state->edit_allocator);
  state->edits = (VkrSceneEditState){.allocator = &state->edit_allocator};
  state->world_edits = (VkrSceneEditState){.allocator = &state->edit_allocator};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    state->additive_edits[i] =
        (VkrSceneEditState){.allocator = &state->edit_allocator};
  }
  state->gizmo_edit_pending = false_v;
  application->editor_viewport.simulation_running =
      !runtime_config->presentation.paneled;
  if (!state->ui.initialize(state->ui.state, &application->editor_viewport.dock,
                            &application->ui_system)) {
    vkr_dmemory_allocator_destroy(&state->edit_allocator);
    arena_destroy(state->stats_arena);
    state->stats_arena = NULL;
    return false_v;
  }
  state->gameplay_enabled = options->gameplay_enabled;
  /* Module component types join the scene types before any scene exists. */
  if (!vkr_script_host_init(&state->scripts, &application->app_allocator)) {
    log_error("The script host could not reserve its memory");
  }
  vkr_script_host_set_migrator(&state->scripts, sample_script_migrate,
                               application);
  for (uint32_t i = 0; i < runtime_config->script_module_count; ++i) {
    const char *error = NULL;
    if (!vkr_script_host_add_module(
            &state->scripts, runtime_config->script_modules[i], &error)) {
      log_error("Script module %u was not registered: %s", i, error);
    }
  }
  state->world_text_id = 0;
  state->world_text_update_clock = vkr_clock_create();
  state->free_camera_use_gamepad = false_v;
  state->free_camera_held = false_v;
  state->last_picked_object_id = 0;
  state->selected_entity = VKR_ENTITY_ID_INVALID;
  state->has_selection = false_v;
  state->gizmo_drag.active = false_v;
  state->gizmo_drag.pending_pick = false_v;
  state->gizmo_drag.pending_select = false_v;
  state->gizmo_drag.mode = VKR_GIZMO_MODE_TRANSLATE;
  state->gizmo_drag.handle = VKR_GIZMO_HANDLE_NONE;
  state->gizmo_drag.axis = vec3_zero();
  state->gizmo_drag.plane_normal = vec3_zero();
  state->gizmo_drag.start_world_position = vec3_zero();
  state->gizmo_drag.start_hit = vec3_zero();
  state->gizmo_drag.start_scale = vec3_one();
  state->gizmo_drag.start_rotation = vkr_quat_identity();
  state->gizmo_drag.start_radius = 0.0f;
  state->gizmo_drag.uses_text_pivot = false_v;
  state->gizmo_drag.text_pivot_local = vec3_zero();
  state->gizmo_drag.pick_position = (Vec2){0};
  state->gizmo_hover_pending = false_v;
  state->gizmo_hot_handle = VKR_GIZMO_HANDLE_NONE;
  state->auto_close_enabled = false_v;
  state->auto_close_after_seconds = 0.0;
  state->metrics_dump_enabled = false_v;
  state->metrics_dump_interval_seconds = 0.0;
  state->metrics_dump_index = 0;
  state->auto_close_requested = false_v;
  state->assert_no_upload_waits = false_v;
  state->upload_wait_violation_seen = false_v;
  state->upload_wait_fence_total = 0;
  state->upload_wait_queue_idle_total = 0;
  state->upload_wait_device_idle_total = 0;
  state->upload_wait_last_publication_serial = 0;
  state->upload_wait_evidence_incomplete = false_v;
  state->hud_last_publication_serial = 0;
  state->hud_frametime_sum = 0.0;
  state->hud_frame_samples = 0;
  state->ibl_validation_mode = 0u;
  state->ibl_validation_scalar = 1.0f;
  state->ibl_validation_scene = NULL;
  state->ibl_validation_defaults_captured = false_v;
  state->ibl_validation_base_enabled = false_v;
  state->ibl_validation_base_intensity = 1.0f;
  state->ibl_validation_base_diffuse_intensity = 1.0f;
  state->ibl_validation_base_specular_intensity = 1.0f;
  state->scene_load_timer_active = false_v;
  state->scene_load_start_time_seconds = 0.0;
  return true_v;
}

/* Logs the device description, fills the HUD hardware text and anisotropy
   support from it, and logs the startup filter and IBL controls. The device
   strings live in a scratch released before return; State keeps only the
   copied text and scalars. */
vkr_internal void vkr_sample_runtime_log_device_information(
    VkrStandardSceneRuntime *application) {
  Scratch scratch = scratch_create(application->app_arena);
  VkrDeviceInformation device_information = {0};
  vkr_renderer_get_device_information(&application->renderer,
                                      &device_information, scratch.arena);
  log_info("Device Name: %s", device_information.device_name.str);
  log_info("Device Vendor: %s", device_information.vendor_name.str);
  log_info("Device Driver Version: %s", device_information.driver_version.str);
  log_info("Device Graphics API Version: %s",
           device_information.api_version.str);
  log_info("Device VRAM Size: %.2f GB",
           (float64_t)device_information.vram_size / GB(1));
  log_info("Device VRAM Local Size: %.2f GB",
           (float64_t)device_information.vram_local_size / GB(1));
  log_info("Device VRAM Shared Size: %.2f GB",
           (float64_t)device_information.vram_shared_size / GB(1));
  VkrPlatformSystemInfo system_info = {0};
  (void)vkr_platform_get_system_info(&system_info);
  const String8 gpu_name = device_information.device_name;
  snprintf(state->hardware_text, sizeof(state->hardware_text),
           "CPU: %s\nGPU: %.*s",
           system_info.cpu[0] ? system_info.cpu : "unavailable",
           (int32_t)gpu_name.length, gpu_name.str);
  snprintf(state->system_text, sizeof(state->system_text),
           "%s\nRAM (resident): pending\nGPU memory (managed): pending",
           state->hardware_text);
  state->anisotropy_supported = bitset8_is_set(
      &device_information.sampler_filters, VKR_SAMPLER_FILTER_ANISOTROPIC_BIT);
  state->max_sampler_anisotropy = device_information.max_sampler_anisotropy;
  // Trilinear (index in FILTER_MODES): the sampler textures load with, so the
  // reported, saved and cycled mode matches what scenes sample.
  state->filter_mode_index = 4;

  log_info("Texture filtering controls: F4=prev, F5=next (start: %s)",
           FILTER_MODES[state->filter_mode_index].label);
  log_info("IBL validation controls: F8=mode, F9/F10=intensity "
           "(start: %s x%.2f)",
           vkr_standard_scene_runtime_ibl_validation_mode_label(
               state->ibl_validation_mode),
           state->ibl_validation_scalar);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
}

int vkr_sample_runtime_run(int argc, char **argv,
                           const VkrSampleRuntimeConfig *runtime_config) {
  /* A bundle's content mounts before any path resolves (ADR-077). */
  vkr_content_codec_install();
  if (!vkr_vfs_mount_startup()) {
    fprintf(stderr, "Cannot mount the bundled content\n");
    return 1;
  }
  VkrSampleRuntimeOptions options;
  if (!vkr_sample_runtime_options_parse(argc, argv, runtime_config, &options)) {
    return 2;
  }

  int exit_code = 0;
  const VkrRendererBackendType renderer_backend = options.renderer_backend;
  VkrStandardSceneRuntimeConfig scene_runtime_config =
      vkr_sample_runtime_scene_config(runtime_config, &options);
  VkrStandardSceneRuntime application = {0};
  if (!vkr_standard_scene_runtime_create(&application, &scene_runtime_config)) {
    fprintf(stderr, "VkrStandardSceneRuntime creation failed\n");
    return 1;
  }
  if (vkr_vfs_pack_count()) {
    log_info("Content: %u archives mounted over %s", vkr_vfs_pack_count(),
             vkr_content_root());
  }
  application.host.window.defer_close = runtime_config->project_managed;
  if (runtime_config->presentation.headless) {
    /* The UI lays out on the offscreen target at scale 1; no window events
       resize it. */
    vkr_ui_system_set_offscreen_content_scale(&application.ui_system, 1.0f);
    vkr_ui_system_set_offscreen_size(&application.ui_system, true_v,
                                     scene_runtime_config.width,
                                     scene_runtime_config.height);
    log_info("Headless: rendering offscreen at %ux%u",
             scene_runtime_config.width, scene_runtime_config.height);
  } else {
    if (runtime_config->presentation.window_width_pt &&
        runtime_config->presentation.window_height_pt) {
      (void)vkr_window_resize_centered(
          &application.host.window,
          runtime_config->presentation.window_width_pt,
          runtime_config->presentation.window_height_pt);
    }
    if (runtime_config->presentation.window_mode != VKR_WINDOW_MODE_WINDOWED &&
        !vkr_window_set_mode(&application.host.window,
                             runtime_config->presentation.window_mode)) {
      log_warn("The window could not enter its display mode");
    }
  }
  application.editor_viewport.enabled = runtime_config->presentation.paneled;
  application.editor_viewport.scene_only =
      runtime_config->presentation.scene_only;
  application.editor_viewport.render_scale =
      !runtime_config->presentation.paneled ||
              runtime_config->presentation.scene_only ||
              renderer_backend == VKR_RENDERER_BACKEND_TYPE_METAL
          ? 1.0f
          : runtime_config->presentation.render_scale;
  if (runtime_config->presentation.paneled &&
      runtime_config->presentation.scene_only) {
    log_info("Editor scene-only mode enabled at render scale %.3f",
             application.editor_viewport.render_scale);
  }

  // Baseline before any scene loads: the renderer's own resident allocations.
  vkr_standard_scene_runtime_log_device_memory_stats(&application, "startup");

  if (!vkr_sample_runtime_initialize_state(&application, runtime_config,
                                           &options)) {
    vkr_standard_scene_runtime_shutdown(&application);
    return 6;
  }

  if (options.auto_close_seconds > 0.0) {
    state->auto_close_enabled = true_v;
    state->auto_close_after_seconds = options.auto_close_seconds;
    log_info("Auto-close enabled via VKR_AUTOCLOSE_SECONDS=%.2f",
             options.auto_close_seconds);
  } else if (runtime_config->presentation.headless) {
    /* Nobody can close a headless run, so it always has a deadline. */
    state->auto_close_enabled = true_v;
    state->auto_close_after_seconds = VKR_SAMPLE_HEADLESS_AUTOCLOSE_SECONDS;
    log_info("Headless: auto-close after %.0f s; VKR_AUTOCLOSE_SECONDS "
             "changes it",
             VKR_SAMPLE_HEADLESS_AUTOCLOSE_SECONDS);
  }
  if (options.auto_close_rejected) {
    log_warn("Ignoring invalid VKR_AUTOCLOSE_SECONDS value '%s'",
             options.auto_close_rejected);
  }

  if (options.autoload_scene) {
    log_info("Auto-loading scene '%s'", options.scene_path);
    vkr_standard_scene_runtime_init_scene_system(&application);
  }

  if (options.metrics_interval_seconds > 0.0) {
    state->metrics_dump_enabled = true_v;
    state->metrics_dump_interval_seconds = options.metrics_interval_seconds;
    state->metrics_dump_clock = vkr_clock_create();
    vkr_clock_start(&state->metrics_dump_clock);
    log_info("Periodic metrics dump enabled every %.2fs via "
             "VKR_METRICS_INTERVAL_SECONDS",
             options.metrics_interval_seconds);
  } else if (options.metrics_interval_rejected) {
    log_warn("Ignoring invalid VKR_METRICS_INTERVAL_SECONDS value '%s'",
             options.metrics_interval_rejected);
  }

  // Already applied through config.metrics_config; this only reports it.
  if (application.metrics->config.pass_gpu_timings) {
    log_info("RenderGraph GPU timings enabled via VKR_RG_GPU_TIMING");
  }
  if (options.rg_slow_frame_rejected) {
    log_warn("Ignoring invalid VKR_RG_SLOW_FRAME_MS value '%s'",
             options.rg_slow_frame_rejected);
  } else if (options.rg_slow_frame_ms > 0.0) {
    application.renderer_metrics.slow_frame_gpu_ms = options.rg_slow_frame_ms;
    log_info("Frames with at least %.2f ms of pass GPU time log their slowest "
             "passes (VKR_RG_SLOW_FRAME_MS)%s",
             options.rg_slow_frame_ms,
             application.metrics->config.pass_gpu_timings
                 ? ""
                 : "; it needs VKR_RG_GPU_TIMING=1");
  }
  if (options.metrics_event_subjects) {
    log_info("Metrics event subjects enabled via VKR_METRICS_EVENT_SUBJECTS");
  }

  state->assert_no_upload_waits = options.assert_no_upload_waits;
  if (state->assert_no_upload_waits) {
    log_info("Upload wait assertion enabled via VKR_ASSERT_NO_UPLOAD_WAITS");
  }

  state->scene_memory_verbose = options.scene_memory_verbose;
  if (state->scene_memory_verbose) {
    log_info(
        "Verbose scene memory breakdown enabled via VKR_SCENE_MEM_VERBOSE");
  }

  vkr_sample_runtime_log_device_information(&application);

  vkr_standard_scene_runtime_init_ui_texts(&application);
  vkr_standard_scene_runtime_init_world_content(&application);

  vkr_standard_scene_runtime_set_callbacks(
      &application, &(VkrStandardSceneRuntimeCallbacks){
                        .state = state,
                        .update = vkr_sample_runtime_update,
                    });
  vkr_standard_scene_runtime_run(&application);
  sample_graphics_save();
  vkr_standard_scene_runtime_close(&application);

  if (state->upload_wait_fence_total > 0 ||
      state->upload_wait_queue_idle_total > 0 ||
      state->upload_wait_device_idle_total > 0 ||
      state->assert_no_upload_waits) {
    log_info("UPLOAD_WAIT_SUMMARY fence=%llu queue_idle=%llu device_idle=%llu "
             "violation=%s",
             (unsigned long long)state->upload_wait_fence_total,
             (unsigned long long)state->upload_wait_queue_idle_total,
             (unsigned long long)state->upload_wait_device_idle_total,
             state->upload_wait_violation_seen ? "true" : "false");
    fprintf(stdout,
            "UPLOAD_WAIT_SUMMARY fence=%llu queue_idle=%llu device_idle=%llu "
            "violation=%s\n",
            (unsigned long long)state->upload_wait_fence_total,
            (unsigned long long)state->upload_wait_queue_idle_total,
            (unsigned long long)state->upload_wait_device_idle_total,
            state->upload_wait_violation_seen ? "true" : "false");
    fflush(stdout);
  }

  if (options.metrics_json_path) {
    const char *metrics_json_path = options.metrics_json_path;
    String8 metrics_path = string8_create_from_cstr(
        (const uint8_t *)metrics_json_path, string_length(metrics_json_path));
    if (!vkr_renderer_metrics_write_json(&application.renderer_metrics,
                                         metrics_path)) {
      log_error("Failed to write metrics JSON to '%s'", metrics_json_path);
      exit_code = 5;
    } else {
      fprintf(stdout, "METRICS_JSON status=pass\n");
      fflush(stdout);
    }
  }

  sample_warm_hold_release(&application, false_v);
  vkr_standard_scene_runtime_unload_scene_system(&application);
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    sample_additive_remove(&application, i);
  }
  sample_world_unload(&application);
  /* Scenes that ran module code are gone; close the libraries. */
  sample_scripts_join();
  vkr_script_host_shutdown(&state->scripts);
  vkr_scene_physics_set_destroy(state->physics_set);
  state->physics_set = NULL;
  /* Every journal reset when its container unloaded above. */
  vkr_allocator_release_global_accounting(&state->edit_allocator);
  vkr_dmemory_allocator_destroy(&state->edit_allocator);
  if (application.last_renderer_error == VKR_RENDERER_ERROR_DEVICE_ERROR)
    exit_code = 5;

  arena_destroy(state->stats_arena);
  state->stats_arena = NULL;

  if (!state->ui.shutdown(state->ui.state, &application.editor_viewport.dock,
                          &application.ui_system) &&
      exit_code == 0)
    exit_code = 6;

  vkr_standard_scene_runtime_shutdown(&application);

  return exit_code;
}

static bool8_t
sample_preferences_valid(const VkrSampleRuntimePreferences *value) {
  return value && value->filter_mode < ArrayCount(FILTER_MODES) &&
         value->gizmo_mode <= VKR_GIZMO_MODE_SCALE &&
         value->gizmo_space <= VKR_GIZMO_SPACE_VIEW &&
         isfinite(value->gizmo_size) && value->gizmo_size >= 16 &&
         value->gizmo_size <= 1024 && isfinite(value->camera_speed) &&
         value->camera_speed > 0 && value->camera_speed <= 10000 &&
         isfinite(value->camera_sensitivity) && value->camera_sensitivity > 0 &&
         value->camera_sensitivity <= 100 && isfinite(value->move_multiplier) &&
         value->move_multiplier > 0 && value->move_multiplier <= 10000 &&
         isfinite(value->rotation_multiplier) &&
         value->rotation_multiplier > 0 &&
         value->rotation_multiplier <= 10000 && value->ibl_debug_mode < 4 &&
         isfinite(value->ibl_debug_scalar) && value->ibl_debug_scalar >= 0 &&
         value->ibl_debug_scalar <= 2;
}

static bool8_t sample_recall_valid(const VkrSampleSceneRecall *value) {
  return value && isfinite(value->grid_height) &&
         fabsf(value->grid_height) <= 10000.0f &&
         (!value->camera_valid ||
          (isfinite(value->position.x) && isfinite(value->position.y) &&
           isfinite(value->position.z) && isfinite(value->yaw) &&
           isfinite(value->pitch) && value->pitch >= -89 &&
           value->pitch <= 89 && isfinite(value->field_of_view) &&
           value->field_of_view > 0 && value->field_of_view < 180 &&
           isfinite(value->near_plane) && isfinite(value->far_plane) &&
           value->near_plane > 0 && value->far_plane > value->near_plane));
}

#define SAMPLE_JSON_FLOAT(name, member)                                        \
  (vkr_json_writer_name(writer, string8_lit(name)) &&                          \
   vkr_json_writer_f64(writer, value->member))
#define SAMPLE_JSON_UINT(name, member)                                         \
  (vkr_json_writer_name(writer, string8_lit(name)) &&                          \
   vkr_json_writer_u64(writer, value->member))
#define SAMPLE_JSON_BOOL(name, member)                                         \
  (vkr_json_writer_name(writer, string8_lit(name)) &&                          \
   vkr_json_writer_bool(writer, value->member))

bool8_t vkr_sample_runtime_preferences_write_json(
    const VkrSampleRuntimePreferences *value, VkrJsonWriter *writer) {
  return sample_preferences_valid(value) &&
         vkr_json_writer_begin_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("version")) &&
         vkr_json_writer_u64(writer, 2) &&
         SAMPLE_JSON_UINT("filter_mode", filter_mode) &&
         SAMPLE_JSON_UINT("gizmo_mode", gizmo_mode) &&
         SAMPLE_JSON_UINT("gizmo_space", gizmo_space) &&
         SAMPLE_JSON_FLOAT("gizmo_size", gizmo_size) &&
         SAMPLE_JSON_FLOAT("camera_speed", camera_speed) &&
         SAMPLE_JSON_FLOAT("camera_sensitivity", camera_sensitivity) &&
         SAMPLE_JSON_FLOAT("move_multiplier", move_multiplier) &&
         SAMPLE_JSON_FLOAT("rotation_multiplier", rotation_multiplier) &&
         SAMPLE_JSON_UINT("ibl_debug_mode", ibl_debug_mode) &&
         SAMPLE_JSON_FLOAT("ibl_debug_scalar", ibl_debug_scalar) &&
         SAMPLE_JSON_BOOL("pass_gpu_timings", pass_gpu_timings) &&
         vkr_json_writer_end_object(writer);
}

static bool8_t sample_read_float(String8 json, const char *name,
                                 float32_t *value) {
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  return vkr_json_get_float(&reader, name, value);
}

static bool8_t sample_read_uint(String8 json, const char *name,
                                uint32_t *value) {
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  float64_t number;
  if (!vkr_json_get_double(&reader, name, &number) || !isfinite(number) ||
      number < 0 || number > UINT32_MAX || floor(number) != number) {
    return false_v;
  }
  *value = (uint32_t)number;
  return true_v;
}

static bool8_t sample_read_bool(String8 json, const char *name,
                                bool8_t *value) {
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  return vkr_json_get_bool(&reader, name, value);
}

bool8_t
vkr_sample_runtime_preferences_read_json(String8 json,
                                         VkrSampleRuntimePreferences *value) {
  if (!value) {
    return false_v;
  }
  VkrSampleRuntimePreferences candidate = {0};
  uint32_t version;
  if (!sample_read_uint(json, "version", &version) || version < 1 ||
      version > 2 ||
      !sample_read_uint(json, "filter_mode", &candidate.filter_mode) ||
      !sample_read_uint(json, "gizmo_mode", &candidate.gizmo_mode) ||
      !sample_read_uint(json, "gizmo_space", &candidate.gizmo_space) ||
      !sample_read_float(json, "gizmo_size", &candidate.gizmo_size) ||
      !sample_read_float(json, "camera_speed", &candidate.camera_speed) ||
      !sample_read_float(json, "camera_sensitivity",
                         &candidate.camera_sensitivity) ||
      !sample_read_float(json, "move_multiplier", &candidate.move_multiplier) ||
      !sample_read_float(json, "rotation_multiplier",
                         &candidate.rotation_multiplier) ||
      !sample_read_uint(json, "ibl_debug_mode", &candidate.ibl_debug_mode) ||
      !sample_read_float(json, "ibl_debug_scalar",
                         &candidate.ibl_debug_scalar) ||
      !sample_read_bool(json, "pass_gpu_timings",
                        &candidate.pass_gpu_timings) ||
      !sample_preferences_valid(&candidate)) {
    return false_v;
  }
  /* Version 1 measured the gizmo in window pixels; version 2 in points. */
  if (version == 1) {
    candidate.gizmo_size = VKR_GIZMO_CONFIG_DEFAULT.screen_size;
  }
  *value = candidate;
  return true_v;
}

bool8_t vkr_sample_entity_identity(const VkrScene *scene, VkrEntityId entity,
                                   VkrSampleEntityIdentity *identity) {
  if (!scene || !identity || !vkr_scene_entity_alive(scene, entity)) {
    return false_v;
  }
  const SceneSourceIdentity *source = vkr_entity_get_component(
      scene->world, entity, scene->comp_source_identity);
  if (!source || !source->source_fingerprint) {
    return false_v;
  }
  *identity = (VkrSampleEntityIdentity){
      .scene_entity = source->scene_entity_index,
      .gltf_node = source->gltf_node_index,
      .source_fingerprint = source->source_fingerprint};
  return true_v;
}

VkrEntityId vkr_sample_entity_find(const VkrScene *scene,
                                   const VkrSampleEntityIdentity *identity) {
  if (!scene || !identity || !identity->source_fingerprint) {
    return VKR_ENTITY_ID_INVALID;
  }
  VkrEntityId result = VKR_ENTITY_ID_INVALID;
  for (uint32_t i = 0; i < scene->world->dir.capacity; ++i) {
    if (!scene->world->dir.records[i].chunk) {
      continue;
    }
    VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    VkrSampleEntityIdentity candidate;
    if (vkr_sample_entity_identity(scene, entity, &candidate) &&
        candidate.scene_entity == identity->scene_entity &&
        candidate.gltf_node == identity->gltf_node &&
        candidate.source_fingerprint == identity->source_fingerprint) {
      if (result.u64) {
        return VKR_ENTITY_ID_INVALID;
      }
      result = entity;
    }
  }
  return result;
}

bool8_t
vkr_sample_entity_identity_write_json(const VkrSampleEntityIdentity *value,
                                      VkrJsonWriter *writer) {
  char fingerprint[17];
  snprintf(fingerprint, sizeof(fingerprint), "%016llx",
           (unsigned long long)value->source_fingerprint);
  return vkr_json_writer_begin_object(writer) &&
         SAMPLE_JSON_UINT("scene_entity", scene_entity) &&
         SAMPLE_JSON_UINT("gltf_node", gltf_node) &&
         vkr_json_writer_name(writer, string8_lit("source_fingerprint")) &&
         vkr_json_writer_string(writer,
                                string8_create((uint8_t *)fingerprint, 16)) &&
         vkr_json_writer_end_object(writer);
}

bool8_t
vkr_sample_entity_identity_read_json(String8 json,
                                     VkrSampleEntityIdentity *identity) {
  if (!identity) {
    return false_v;
  }
  VkrSampleEntityIdentity candidate = {0};
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  String8 fingerprint;
  if (!sample_read_uint(json, "scene_entity", &candidate.scene_entity) ||
      !sample_read_uint(json, "gltf_node", &candidate.gltf_node) ||
      !vkr_json_get_string(&reader, "source_fingerprint", &fingerprint) ||
      fingerprint.length != 16) {
    return false_v;
  }
  for (uint32_t i = 0; i < 16; ++i) {
    uint8_t character = fingerprint.str[i];
    uint32_t digit;
    if (character >= '0' && character <= '9') {
      digit = character - '0';
    } else if (character >= 'a' && character <= 'f') {
      digit = character - 'a' + 10;
    } else {
      return false_v;
    }
    candidate.source_fingerprint = (candidate.source_fingerprint << 4) | digit;
  }
  if (!candidate.source_fingerprint) {
    return false_v;
  }
  *identity = candidate;
  return true_v;
}

bool8_t vkr_sample_scene_recall_write_json(const VkrSampleSceneRecall *value,
                                           VkrJsonWriter *writer) {
  if (!sample_recall_valid(value) || !vkr_json_writer_begin_object(writer) ||
      !vkr_json_writer_name(writer, string8_lit("version")) ||
      !vkr_json_writer_u64(writer, 1) ||
      !SAMPLE_JSON_BOOL("camera_valid", camera_valid) ||
      !SAMPLE_JSON_BOOL("selection_valid", selection_valid)) {
    return false_v;
  }
  if (value->camera_valid &&
      !(SAMPLE_JSON_FLOAT("x", position.x) &&
        SAMPLE_JSON_FLOAT("y", position.y) &&
        SAMPLE_JSON_FLOAT("z", position.z) && SAMPLE_JSON_FLOAT("yaw", yaw) &&
        SAMPLE_JSON_FLOAT("pitch", pitch) &&
        SAMPLE_JSON_FLOAT("fov", field_of_view) &&
        SAMPLE_JSON_FLOAT("near", near_plane) &&
        SAMPLE_JSON_FLOAT("far", far_plane))) {
    return false_v;
  }
  if (value->grid_height != 0.0f &&
      !SAMPLE_JSON_FLOAT("grid_height", grid_height)) {
    return false_v;
  }
  if (value->selection_valid &&
      !(vkr_json_writer_name(writer, string8_lit("selection")) &&
        vkr_sample_entity_identity_write_json(&value->selection, writer))) {
    return false_v;
  }
  return vkr_json_writer_end_object(writer);
}

bool8_t vkr_sample_scene_recall_read_json(String8 json,
                                          VkrSampleSceneRecall *value) {
  if (!value) {
    return false_v;
  }
  VkrSampleSceneRecall candidate = {0};
  uint32_t version;
  if (!sample_read_uint(json, "version", &version) || version != 1 ||
      !sample_read_bool(json, "camera_valid", &candidate.camera_valid) ||
      !sample_read_bool(json, "selection_valid", &candidate.selection_valid)) {
    return false_v;
  }
  if (candidate.camera_valid &&
      !(sample_read_float(json, "x", &candidate.position.x) &&
        sample_read_float(json, "y", &candidate.position.y) &&
        sample_read_float(json, "z", &candidate.position.z) &&
        sample_read_float(json, "yaw", &candidate.yaw) &&
        sample_read_float(json, "pitch", &candidate.pitch) &&
        sample_read_float(json, "fov", &candidate.field_of_view) &&
        sample_read_float(json, "near", &candidate.near_plane) &&
        sample_read_float(json, "far", &candidate.far_plane))) {
    return false_v;
  }
  /* Optional: older files have no grid height. */
  if (!sample_read_float(json, "grid_height", &candidate.grid_height)) {
    candidate.grid_height = 0.0f;
  }
  if (candidate.selection_valid) {
    VkrJsonReader reader = vkr_json_reader_from_string(json);
    VkrJsonReader object;
    if (!vkr_json_find_field(&reader, "selection") ||
        !vkr_json_enter_object(&reader, &object) ||
        !vkr_sample_entity_identity_read_json(
            (String8){.str = (uint8_t *)object.data, .length = object.length},
            &candidate.selection)) {
      return false_v;
    }
  }
  if (!sample_recall_valid(&candidate)) {
    return false_v;
  }
  *value = candidate;
  return true_v;
}

#undef SAMPLE_JSON_FLOAT
#undef SAMPLE_JSON_UINT
#undef SAMPLE_JSON_BOOL

static VkrSampleRuntimePreferences
sample_preferences_snapshot(VkrStandardSceneRuntime *application) {
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  return (VkrSampleRuntimePreferences){
      .filter_mode = state->filter_mode_index,
      .gizmo_mode = application->gizmo_system.mode,
      .gizmo_space = application->gizmo_system.space,
      .gizmo_size = application->gizmo_system.config.screen_size >= 16
                        ? application->gizmo_system.config.screen_size
                        : 150,
      .camera_speed = camera ? camera->speed : 1,
      .camera_sensitivity = camera ? camera->sensitivity : 1,
      .move_multiplier = application->camera_controller.move_speed,
      .rotation_multiplier = application->camera_controller.rotation_speed,
      .ibl_debug_mode = state->ibl_validation_mode,
      .ibl_debug_scalar = state->ibl_validation_scalar,
      .pass_gpu_timings = application->metrics->config.pass_gpu_timings};
}

static VkrSampleSceneRecall
sample_recall_snapshot(VkrStandardSceneRuntime *application) {
  VkrSampleSceneRecall result = {0};
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (camera && camera->type == VKR_CAMERA_TYPE_ORTHOGRAPHIC &&
      state->perspective_camera_saved) {
    camera = &state->perspective_camera;
  }
  if (camera && camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
    result.camera_valid = true_v;
    result.position = state->script_camera_active
                          ? state->editor_camera_position
                          : camera->position;
    result.yaw =
        state->script_camera_active ? state->editor_camera_yaw : camera->yaw;
    result.pitch = state->script_camera_active ? state->editor_camera_pitch
                                               : camera->pitch;
    /* Written for older readers, which still apply it. */
    result.field_of_view = VKR_STANDARD_SCENE_CAMERA_FOV_DEGREES;
    result.near_plane = camera->near_clip;
    result.far_plane = camera->far_clip;
  }
  result.grid_height = state->view_state.grid_height;
  result.selection_valid =
      state->has_selection &&
      vkr_sample_entity_identity(application->active_scene,
                                 state->selected_entity, &result.selection);
  return result;
}

static void
sample_editor_state_apply(VkrStandardSceneRuntime *application,
                          const VkrSampleEditorStateRequest *request) {
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (request->apply_preferences &&
      sample_preferences_valid(&request->preferences)) {
    const VkrSampleRuntimePreferences *value = &request->preferences;
    vkr_standard_scene_runtime_apply_filter_mode(application,
                                                 value->filter_mode);
    application->gizmo_system.mode = (VkrGizmoMode)value->gizmo_mode;
    application->gizmo_system.space = (VkrGizmoSpace)value->gizmo_space;
    application->gizmo_system.config.screen_size = value->gizmo_size;
    application->camera_controller.move_speed = value->move_multiplier;
    application->camera_controller.rotation_speed = value->rotation_multiplier;
    application->metrics->config.pass_gpu_timings = value->pass_gpu_timings;
    state->ibl_validation_mode = value->ibl_debug_mode;
    state->ibl_validation_scalar = value->ibl_debug_scalar;
    if (camera) {
      camera->speed = value->camera_speed;
      camera->sensitivity = value->camera_sensitivity;
    }
  }
  if (request->move_camera && camera) {
    vkr_camera_set_pose(camera, request->camera_position, request->camera_yaw,
                        request->camera_pitch);
  }
  if (request->apply_recall && sample_recall_valid(&request->recall)) {
    const VkrSampleSceneRecall *value = &request->recall;
    state->view_state.grid_height = value->grid_height;
    if (camera && value->camera_valid) {
      camera->type = VKR_CAMERA_TYPE_PERSPECTIVE;
      state->view_state.camera_view = VKR_SAMPLE_CAMERA_PERSPECTIVE;
      state->perspective_camera_saved = false_v;
      state->script_camera_active = false_v;
      vkr_camera_set_pose(camera, value->position, value->yaw, value->pitch);
      /* The Scene's lens is fixed: a recalled field of view, such as a
         narrowed one older editors saved, does not apply. */
      (void)vkr_camera_set_perspective_lens(
          camera, VKR_STANDARD_SCENE_CAMERA_FOV_DEGREES, value->near_plane,
          value->far_plane, Max(camera->cached_window_width, 1u),
          Max(camera->cached_window_height, 1u));
      vkr_renderer_invalidate_temporal_history(&application->renderer);
    }
    vkr_standard_scene_runtime_clear_gizmo_selection(application);
    if (value->selection_valid) {
      VkrEntityId entity =
          vkr_sample_entity_find(application->active_scene, &value->selection);
      if (entity.u64) {
        state->selected_entity = entity;
        state->has_selection = true_v;
      }
    }
  }
}

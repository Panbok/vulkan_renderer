#include "vkr_sample_runtime.h"
#include "core/vkr_json.h"
#include "core/vkr_subsystem_plan.h"
#include "gameplay/vkr_gameplay_player.h"
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
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "vkr_renderer.h"
#include <stdio.h>

#define VKR_FPS_UPDATE_INTERVAL 0.25
#define VKR_MEMORY_UPDATE_INTERVAL 1.0
#define VKR_FPS_DELTA_MIN 0.000001
#define VKR_WORLD_TIME_UPDATE_INTERVAL 0.25

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

typedef struct State {
  VkrGameplayPlayer player;
  VkrEntityId player_visual;
  Vec3 editor_camera_position;
  float32_t editor_camera_yaw;
  float32_t editor_camera_pitch;
  bool8_t player_camera_active;
  bool8_t gameplay_enabled;
  bool8_t gameplay_attempted;
  InputState *input_state;
  bool8_t scene_keyboard_focus;
  VkrSampleViewState view_state;
  VkrCamera perspective_camera;
  bool8_t perspective_camera_saved;
  uint32_t collision_display;
  /* Reused application-owned event buffer; draining the editor's demo events
     prevents an otherwise unconsumed sensor queue from exhausting capacity. */
  VkrPhysicsSensorEvent
      physics_sensor_events[VKR_SCENE_PHYSICS_MAX_BODIES *
                            VKR_PHYSICS_SENSOR_EVENTS_PER_BODY];
  uint32_t physics_sensor_event_count;

  Arena *app_arena;
  Arena *event_arena;
  Arena *stats_arena;

  uint32_t filter_mode_index;
  bool8_t anisotropy_supported;
  VkrDeviceInformation device_information;

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
  char graphics_path[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  char graphics_message[160];
  bool8_t graphics_dirty;
  float64_t graphics_changed_at;
  VkrSceneEditState edits;
  String8 scene_path;
  char scene_path_storage[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  char sidecar_path[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
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

static void sample_graphics_apply_live(VkrStandardSceneRuntime *application,
                                       const VkrGraphicsSettings *settings) {
  VkrFrameGlobals *globals = &application->globals;
  globals->exposure_compensation_ev = settings->brightness;
  globals->white_balance_temperature = settings->temperature;
  globals->white_balance_tint = settings->tint;
  globals->color_contrast = settings->contrast;
  globals->color_saturation = settings->saturation;
  globals->image_sharpness = settings->sharpness;
  globals->bloom_enabled = settings->bloom;
  globals->bloom_intensity = settings->bloom_intensity;
  globals->gtao_enabled = settings->ambient_occlusion;
  globals->ssr_enabled = settings->screen_space_reflections;
  globals->ssgi_enabled = settings->screen_space_gi;
  globals->dof_enabled = settings->depth_of_field;
  globals->motion_blur_enabled = settings->motion_blur;
  globals->motion_blur_shutter_angle = settings->motion_blur_amount * 360.0f;
  application->disable_directional_shadows = settings->shadow_quality == 0;
  application->disable_local_shadows =
      settings->shadow_quality == 0 || !settings->local_shadows;
  application->disable_soft_shadows = !settings->soft_shadows;
  application->disable_fog = !settings->fog;
  application->disable_volumetric_fog =
      !settings->fog || !settings->volumetric_fog;
  application->disable_subsurface_scattering = !settings->subsurface_scattering;
  application->ibl_probe_limit = settings->reflection_probes ? UINT32_MAX : 0;
  application->shadow_system.config = settings->shadow_quality == 1
                                          ? VKR_SHADOW_CONFIG_BALANCED
                                          : VKR_SHADOW_CONFIG_HIGH;
  application->renderer.temporal_enabled =
      application->renderer.upscale_mode != VKR_UPSCALE_MODE_SPATIAL ||
      settings->anti_aliasing;
  application->host.config.target_frame_rate = settings->frame_limit;
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
      old.local_shadows != settings.local_shadows ||
      old.ambient_occlusion != settings.ambient_occlusion ||
      old.screen_space_reflections != settings.screen_space_reflections ||
      old.screen_space_gi != settings.screen_space_gi ||
      old.reflection_probes != settings.reflection_probes ||
      old.subsurface_scattering != settings.subsurface_scattering ||
      old.fog != settings.fog || old.volumetric_fog != settings.volumetric_fog;
  state->graphics.settings = settings;
  state->graphics.restart_required = vkr_graphics_settings_restart_required(
      &settings, &state->graphics_started);
  state->graphics_message[0] = '\0';
  state->graphics_dirty = true_v;
  state->graphics_changed_at = vkr_platform_get_absolute_time();
  sample_graphics_apply_live(application, &settings);
  if (lighting_changed) {
    vkr_shadow_system_invalidate_fit_history(&application->shadow_system);
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
    VkrViewportMapping mapping = {0};
    VkrWindowPixelSize window_size =
        vkr_window_get_pixel_size(&application->host.window);
    if (vkr_standard_scene_runtime_editor_viewport_mapping(
            application, window_size.width, window_size.height, &mapping)) {
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

vkr_internal bool8_t vkr_standard_scene_runtime_world_text_entity_from_id(
    VkrScene *scene, uint32_t text_id, VkrEntityId *out_entity) {
  if (!scene || !scene->world || !out_entity) {
    return false_v;
  }

  VkrWorld *world = scene->world;
  if (text_id >= world->dir.capacity) {
    return false_v;
  }

  uint16_t generation = world->dir.generations[text_id];
  if (generation == 0) {
    return false_v;
  }

  VkrEntityId candidate = {
      .parts = {.index = text_id,
                .generation = generation,
                .world = world->world_id},
  };
  if (!vkr_entity_is_alive(world, candidate)) {
    return false_v;
  }

  SceneText3D *text = vkr_scene_get_text3d(scene, candidate);
  if (!text || text->text_index != text_id) {
    return false_v;
  }

  *out_entity = candidate;
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

vkr_internal bool8_t vkr_standard_scene_runtime_restore_gizmo_edit(
    VkrStandardSceneRuntime *application) {
  VkrScene *scene = application->active_scene;
  if (!scene) {
    return false_v;
  }
  if (state->gizmo_before.fields & VKR_SCENE_EDIT_PHYSICS) {
    const char *error = NULL;
    if (!vkr_scene_physics_apply(scene, state->gizmo_edit_entity,
                                 &state->gizmo_before.physics, &error)) {
      snprintf(state->edits.status, sizeof(state->edits.status),
               "Could not restore collider drag: %s",
               error ? error : "allocation failed");
      return false_v;
    }
  } else {
    if (!vkr_scene_set_transform(
            scene, state->gizmo_drag.entity, state->gizmo_before.position,
            state->gizmo_before.rotation, state->gizmo_before.scale)) {
      snprintf(state->edits.status, sizeof(state->edits.status),
               "Could not restore the authored transform.");
      return false_v;
    }
    vkr_standard_scene_runtime_sync_world_text_transform(
        application, scene, state->gizmo_drag.entity);
  }
  return true_v;
}

vkr_internal bool8_t vkr_standard_scene_runtime_apply_gizmo_pose(
    VkrStandardSceneRuntime *application, Vec3 position, VkrQuat rotation,
    Vec3 scale) {
  VkrScene *scene = application->active_scene;
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
        snprintf(state->edits.status, sizeof(state->edits.status), "%s",
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
    snprintf(state->edits.status, sizeof(state->edits.status), "%s",
             error ? error : "Transform edit failed.");
    return false_v;
  }
  vkr_standard_scene_runtime_sync_world_text_transform(
      application, scene, state->gizmo_drag.entity);
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_cancel_gizmo_edit(
    VkrStandardSceneRuntime *application) {
  VkrScene *scene = application->active_scene;
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
  if (state->collision_display && application->active_scene) {
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
      if ((state->collision_display == 2 || owner.u64 == selected_owner.u64) &&
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

vkr_internal bool8_t vkr_standard_scene_runtime_begin_gizmo_drag(
    VkrStandardSceneRuntime *application, VkrGizmoHandle handle) {
  if (!application || !state || !state->has_selection) {
    return false_v;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(state->scene_resource.as.scene);
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

  Vec3 axis = vec3_zero();
  bool8_t has_axis = vkr_gizmo_handle_axis(handle, &axis);
  Vec3 plane_normal;

  if (mode == VKR_GIZMO_MODE_SCALE) {
    axis = vec3_zero();
    has_axis = false_v;
  }

  if (mode == VKR_GIZMO_MODE_ROTATE) {
    if (!has_axis) {
      axis = vec3_normalize(camera->forward);
    }
    plane_normal = axis;
  } else {
    if (!has_axis) {
      plane_normal = vec3_normalize(camera->forward);
    } else {
      plane_normal =
          vkr_standard_scene_runtime_gizmo_axis_plane_normal(camera, axis);
    }
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
  state->gizmo_drag.uses_text_pivot = has_text_pivot;
  state->gizmo_drag.text_pivot_local = pivot_local;
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_update_gizmo_drag(
    VkrStandardSceneRuntime *application,
    const VkrViewportHitInfo *viewport_info) {
  if (!application || !state || !state->gizmo_drag.active || !viewport_info ||
      !viewport_info->has_target_coords) {
    return;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(state->scene_resource.as.scene);
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
    if (vkr_gizmo_handle_is_free_translate(state->gizmo_drag.handle)) {
      new_pivot = vec3_add(state->gizmo_drag.start_world_position, delta);
    } else {
      float32_t dist = vec3_dot(delta, state->gizmo_drag.axis);
      Vec3 axis_delta = vec3_scale(state->gizmo_drag.axis, dist);
      new_pivot = vec3_add(state->gizmo_drag.start_world_position, axis_delta);
    }

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
    if (state->gizmo_drag.start_radius > VKR_FLOAT_EPSILON) {
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
    log_info("Anisotropic sampling count: %f",
             state->device_information.max_sampler_anisotropy);
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
    vkr_scene_edit_reset(&state->edits,
                         &application->ui_system.retained_allocator,
                         application->scene_generation);
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
    /* A fit from the previous scene is framed by a camera and caster set that
       no longer exist, so it is not a previous value of the same quantity. The
       configuration stamps cannot catch this: they are all identical across a
       scene swap. */
    vkr_shadow_system_invalidate_fit_history(&application->shadow_system);
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
      vkr_allocator_begin_scope(&application->frame_allocator);
  if (!vkr_allocator_scope_is_valid(&load_scope)) {
    log_error("Failed to create scene load scratch scope");
    return;
  }

  VkrRendererError load_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, scene_path,
                                &application->frame_allocator,
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

  if (state->player_camera_active) {
    VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &application->camera_system, application->active_camera);
    if (camera) {
      vkr_camera_set_pose(camera, state->editor_camera_position,
                          state->editor_camera_yaw, state->editor_camera_pitch);
    }
    state->player_camera_active = false_v;
  }
  vkr_gameplay_player_shutdown(&state->player);
  state->gameplay_attempted = false_v;
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
  vkr_scene_edit_reset(&state->edits,
                       &application->ui_system.retained_allocator, 0);
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
  vkr_shadow_system_invalidate_fit_history(&application->shadow_system);
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

  VkrDeviceMemoryStats gpu = {0};
  const bool8_t have_gpu =
      vkr_renderer_get_device_memory_stats(&application->renderer, &gpu);
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
  if (!captured && hovered && input_button_just_pressed(input, BUTTON_RIGHT) &&
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

vkr_internal void
vkr_standard_scene_runtime_handle_input(VkrStandardSceneRuntime *application,
                                        float64_t delta_time) {
  if (state == NULL || state->input_state == NULL) {
    log_error("State or input state is NULL");
    return;
  }

  (void)delta_time;

  InputState *input_state = state->input_state;

  if (!application->ui_capture.keyboard &&
      input_is_key_up(state->input_state, KEY_M) &&
      input_was_key_down(state->input_state, KEY_M)) {
    VkrAllocatorScope stats_scope =
        vkr_allocator_begin_scope(&application->app_allocator);
    if (!vkr_allocator_scope_is_valid(&stats_scope)) {
      log_error("Failed to create allocator stats scope");
      return;
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
    } else
      vkr_standard_scene_runtime_unload_scene_system(application);
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

  if (input_is_key_up(input_state, KEY_G) &&
      input_was_key_down(input_state, KEY_G)) {
    vkr_standard_scene_runtime_log_camera_snapshot(application);
  }

  if (application->editor_viewport.enabled &&
      state->view_state.camera_view != VKR_SAMPLE_CAMERA_PERSPECTIVE) {
    sample_orthographic_input(application, delta_time);
    return;
  }

  if (state->gameplay_enabled ||
      (state->player.scene &&
       application->editor_viewport.simulation_running)) {
    if (state->player.scene &&
        input_key_just_pressed(input_state, KEY_BACKSPACE)) {
      VkrScene *scene = state->player.scene;
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
    return;
  }

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
  if ((camera_tab || camera_shortcut) &&
      (camera_captured ||
       !vkr_standard_scene_runtime_editor_scene_rendering_stopped(
           application))) {
    vkr_window_set_mouse_capture(&application->host.window, !camera_captured);
    state->free_camera_held = false_v;
    camera_started = !camera_captured;
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

  if (application->editor_viewport.enabled &&
      !vkr_window_is_mouse_captured(&application->host.window) &&
      !vkr_standard_scene_runtime_editor_scene_rendering_stopped(application) &&
      !application->ui_capture.mouse && !application->ui_capture.text &&
      application->ui_system.mouse_input_layer == 0u &&
      application->ui_system.keyboard_input_layer == 0u &&
      !state->gizmo_drag.active && !state->gizmo_drag.pending_pick &&
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
      application->ui_capture.keyboard = false_v;
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

    int32_t wheel_delta = 0;
    input_get_mouse_wheel(input_state, &wheel_delta);
    if (wheel_delta != 0) {
      float32_t zoom_delta = -(float32_t)wheel_delta * 0.1f;
      vkr_camera_zoom(camera, zoom_delta);
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

      float32_t max_mouse_delta = VKR_MAX_MOUSE_DELTA / camera->sensitivity;
      x_offset = vkr_clamp_f32(x_offset, -max_mouse_delta, max_mouse_delta);
      y_offset = vkr_clamp_f32(y_offset, -max_mouse_delta, max_mouse_delta);

      // Positive screen-space X turns the +yaw camera direction to the right.
      // Captured platform input exposes upward motion as positive virtual Y,
      // so both positive deltas map directly to positive camera rotation.
      yaw_input = x_offset;
      pitch_input = y_offset;
      should_rotate = true_v;
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

    VkrAllocator *frame_alloc = &application->frame_allocator;

    // Everything below describes one published frame. Seed from the live
    // structs so a metric the collector could not sample leaves the last known
    // value rather than a zero, then let the adapter overwrite what it has.
    VkrRendererFrameMetrics metrics_snapshot =
        application->renderer.frame_metrics;
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
      if (state->player.scene) {
        const VkrPlayerState *player = vkr_entity_get_component_if_alive_const(
            state->player.scene->world, state->player.entity,
            state->player.component);
        if (player) {
          left_text = string8_create_formatted(
              frame_alloc,
              "Ammo %u / %u%s  Hits %llu\nWASD move | Mouse fire/look | R "
              "reload\nSpace jump | Ctrl crouch | V camera\nTab mouse | "
              "Backspace reset",
              player->weapon.magazine_rounds, player->reserve_rounds,
              player->weapon.reloading ? "  Reloading" : "",
              (unsigned long long)state->player.hits);
        }
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

/**
 * @brief Update scene system each frame.
 */
/* A small playable training platform in the loaded Bistro world. Its collision
 * belongs to these explicit scene bodies; the decorative city is not implicitly
 * promoted to collision geometry. */
static bool8_t sample_gameplay_start(VkrStandardSceneRuntime *application) {
  VkrScene *scene = application->active_scene;
  state->gameplay_attempted = true_v;
  vkr_scene_physics_set_paused(scene, true_v);
  const char *error = NULL;
  if (!vkr_scene_physics_reset(scene, &error)) {
    log_error("Gameplay reset failed: %s", error ? error : "unknown error");
    return false_v;
  }
  if (scene->player_entity.u64) {
    if (!vkr_gameplay_player_attach(
            &state->player, scene, state->input_state, scene->player_entity,
            application->scene_generation + 1, scene->player_yaw, &error)) {
      log_error("Scene player setup failed: %s",
                error ? error : "unknown error");
      return false_v;
    }
    state->player_visual = scene->player_entity;
    Mat4 initial = vkr_scene_get_transform(scene, scene->player_entity)->world;
    if (scene->player_weapon_entity.u64) {
      VkrAnimationPlayer *animation =
          vkr_scene_animation_get_player(scene, scene->player_entity);
      const VkrAnimationAsset *asset = vkr_animation_player_asset(animation);
      if (!asset || scene->player_weapon_bone >= asset->node_count ||
          !vkr_scene_set_evaluated_transform(scene, scene->player_weapon_entity,
                                             &initial)) {
        vkr_gameplay_player_shutdown(&state->player);
        log_error("Player weapon requires a valid animation bone");
        return false_v;
      }
    }
    if (!vkr_scene_set_evaluated_transform(scene, scene->player_entity,
                                           &initial)) {
      vkr_scene_set_evaluated_transform(scene, scene->player_weapon_entity,
                                        NULL);
      vkr_gameplay_player_shutdown(&state->player);
      return false_v;
    }
    if (application->editor_viewport.simulation_running) {
      vkr_window_set_mouse_capture(&application->host.window, true_v);
      vkr_scene_physics_set_paused(scene, false_v);
    }
    return true_v;
  }
  VkrEntityId created[7] = {0};
  uint32_t count = 0;
  VkrEntityId root = vkr_scene_create_entity(scene, NULL);
  if (!root.u64) {
    return false_v;
  }
  created[count++] = root;
  if (!vkr_scene_set_name(scene, root, string8_lit("GameplayPlayer")) ||
      !vkr_scene_set_transform(scene, root, vec3_new(-28, 20, 3),
                               vkr_quat_identity(), vec3_one())) {
    goto fail;
  }
  for (uint32_t i = 0; i < 6; ++i) {
    VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
    if (!entity.u64) {
      goto fail;
    }
    created[count++] = entity;
    Vec3 position;
    VkrSceneShapeConfig shape = VKR_SCENE_SHAPE_CONFIG_DEFAULT;
    if (i == 0) {
      position = vec3_new(0, .9f, 0);
      shape.dimensions = vec3_new(.6f, 1.8f, .6f);
      shape.color = vec4_new(.15f, .45f, .85f, 1);
    } else if (i == 1) {
      position = vec3_new(-28, 19.5f, 0);
      shape.dimensions = vec3_new(16, 1, 16);
      shape.color = vec4_new(.25f, .3f, .32f, 1);
    } else if (i == 5) {
      position = vec3_new(-24, 20.15f, 0);
      shape.dimensions = vec3_new(2, .3f, 2);
      shape.color = vec4_new(.55f, .6f, .65f, 1);
    } else {
      position = vec3_new(-28 + ((int32_t)i - 3) * 2.0f, 20.6f, -2);
      shape.dimensions = vec3_new(1, 1.2f, 1);
      shape.color = vec4_new(.85f, .3f, .12f, 1);
    }
    if (!vkr_scene_set_transform(scene, entity, position, vkr_quat_identity(),
                                 vec3_one()) ||
        !vkr_scene_set_shape(scene, &application->assets, entity, &shape,
                             NULL)) {
      goto fail;
    }
    if (i == 0) {
      vkr_scene_set_parent(scene, entity, root);
      state->player_visual = entity;
      vkr_scene_set_visibility(scene, entity, false_v, false_v);
    } else {
      VkrScenePhysicsSnapshot body = vkr_scene_physics_default();
      body.motion = i == 1 || i == 5 ? VKR_PHYSICS_STATIC : VKR_PHYSICS_DYNAMIC;
      body.colliders[0].half_extent = vec3_scale(shape.dimensions, .5f);
      if (!vkr_scene_physics_apply(scene, entity, &body, &error)) {
        goto fail;
      }
    }
  }
  if (!vkr_gameplay_player_attach(&state->player, scene, state->input_state,
                                  root, application->scene_generation + 1,
                                  -1.57079632679f, &error)) {
    goto fail;
  }
  vkr_window_set_mouse_capture(&application->host.window, true_v);
  vkr_scene_physics_set_paused(scene, false_v);
  log_info("Gameplay ready: WASD move, mouse look, left click fire, R reload, "
           "Space jump, V camera mode, Backspace reset, Tab/Escape release or "
           "capture mouse. "
           "Collision is limited to the training platform and targets.");
  return true_v;
fail:
  vkr_gameplay_player_shutdown(&state->player);
  while (count) {
    vkr_scene_destroy_entity(scene, created[--count]);
  }
  log_error("Gameplay setup failed: %s",
            error ? error : "scene/resource allocation");
  return false_v;
}

/* Presentation overrides never modify the authored spawn or weapon transform.
 * Both components are acquired during attachment; updates reuse their storage.
 */
static void sample_gameplay_visuals(VkrStandardSceneRuntime *application,
                                    const VkrCameraRigPose *camera) {
  VkrScene *scene = application->active_scene;
  if (!scene->player_entity.u64) {
    return;
  }
  VkrGameplayPlayer *player = &state->player;
  const float32_t alpha =
      scene->physics_paused
          ? 1.0f
          : (float32_t)Min(1.0, scene->simulation.accumulator /
                                    VKR_SCENE_SIMULATION_FIXED_DT);
  const Vec3 foot = vec3_add(
      player->previous_foot,
      vec3_scale(vec3_sub(player->current_foot, player->previous_foot), alpha));
  const VkrQuat rotation = vkr_quat_from_axis_angle(
      vec3_new(0, 1, 0), -player->render_yaw - 1.57079632679f);
  const Mat4 root = mat4_mul(mat4_translate(foot), vkr_quat_to_mat4(rotation));
  vkr_scene_set_evaluated_transform(scene, scene->player_entity, &root);
  if (scene->player_weapon_entity.u64) {
    Mat4 weapon;
    if (camera && application->editor_viewport.simulation_running &&
        player->camera.mode == VKR_CAMERA_RIG_FIRST_PERSON) {
      const Vec3 right = vec3_cross(camera->forward, camera->up);
      const Vec3 position =
          vec3_add(camera->position,
                   vec3_add(vec3_scale(right, .22f),
                            vec3_add(vec3_scale(camera->up, -.25f),
                                     vec3_scale(camera->forward, .35f))));
      weapon = mat4_identity();
      weapon.elements[0] = right.x;
      weapon.elements[1] = right.y;
      weapon.elements[2] = right.z;
      weapon.elements[4] = camera->up.x;
      weapon.elements[5] = camera->up.y;
      weapon.elements[6] = camera->up.z;
      weapon.elements[8] = -camera->forward.x;
      weapon.elements[9] = -camera->forward.y;
      weapon.elements[10] = -camera->forward.z;
      weapon.elements[12] = position.x;
      weapon.elements[13] = position.y;
      weapon.elements[14] = position.z;
      VkrAnimationPlayer *animation =
          vkr_scene_animation_get_player(scene, scene->player_entity);
      const VkrAnimationAsset *asset = vkr_animation_player_asset(animation);
      if (player->weapon_reference_valid && asset &&
          scene->player_weapon_bone < asset->node_count) {
        const Mat4 delta = mat4_mul(player->weapon_reference_inverse,
                                    vkr_animation_player_global_pose(
                                        animation)[scene->player_weapon_bone]);
        weapon = mat4_mul(weapon, delta);
      }
    } else {
      VkrAnimationPlayer *animation =
          vkr_scene_animation_get_player(scene, scene->player_entity);
      const VkrAnimationAsset *asset = vkr_animation_player_asset(animation);
      if (!asset || scene->player_weapon_bone >= asset->node_count) {
        return;
      }
      weapon = mat4_mul(root, vkr_animation_player_global_pose(
                                  animation)[scene->player_weapon_bone]);
    }
    vkr_scene_set_evaluated_transform(scene, scene->player_weapon_entity,
                                      &weapon);
  }
  vkr_scene_update_transforms(scene);
}

vkr_internal void
vkr_standard_scene_runtime_update_scene(VkrStandardSceneRuntime *application,
                                        float64_t delta_time) {
  if (!application || !state) {
    return;
  }

  (void)vkr_standard_scene_runtime_try_activate_scene_resource(application);
  if (!state->scene_resource.as.scene || !application->active_scene) {
    return;
  }

  VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_get_state(&state->scene_resource, &state_error) !=
      VKR_RESOURCE_LOAD_STATE_READY) {
    return;
  }

  if ((state->gameplay_enabled ||
       application->active_scene->player_entity.u64) &&
      !state->gameplay_attempted) {
    sample_gameplay_start(application);
  }
  if (state->player.scene) {
    VkrScene *scene = application->active_scene;
    if (!scene->simulation.faulted) {
      vkr_scene_physics_set_paused(
          scene, !application->editor_viewport.simulation_running);
    }
    const bool8_t focused =
        !state->modal && !application->ui_capture.keyboard &&
        !application->ui_capture.mouse &&
        state->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE &&
        vkr_window_is_mouse_captured(&application->host.window);
    const float64_t gameplay_dt = vkr_gameplay_player_frame(
        &state->player, vkr_platform_get_absolute_time(), focused);
    vkr_scene_handle_update(state->scene_resource.as.scene, gameplay_dt);
    if (scene->physics_paused &&
        application->editor_viewport.simulation_running) {
      application->editor_viewport.simulation_running = false_v;
      vkr_window_set_mouse_capture(&application->host.window, false_v);
      const char *reason = vkr_scene_physics_error(scene);
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Simulation stopped: %s", reason ? reason : "paused");
      log_error("%s", state->scene_status);
    }
    vkr_scene_set_visibility(
        scene, state->player_visual,
        !application->editor_viewport.simulation_running ||
            state->view_state.camera_view != VKR_SAMPLE_CAMERA_PERSPECTIVE ||
            state->player.camera.mode != VKR_CAMERA_RIG_FIRST_PERSON,
        true_v);
    VkrCameraRigPose pose;
    VkrCamera *camera = vkr_camera_registry_get_by_handle(
        &application->camera_system, application->active_camera);
    const bool8_t have_pose = vkr_gameplay_player_camera(&state->player, &pose);
    if (camera && !application->editor_viewport.simulation_running &&
        state->player_camera_active) {
      vkr_camera_set_pose(camera, state->editor_camera_position,
                          state->editor_camera_yaw, state->editor_camera_pitch);
      state->player_camera_active = false_v;
    }
    if (camera && application->editor_viewport.simulation_running &&
        state->view_state.camera_view == VKR_SAMPLE_CAMERA_PERSPECTIVE &&
        have_pose) {
      if (!state->player_camera_active) {
        state->editor_camera_position = camera->position;
        state->editor_camera_yaw = camera->yaw;
        state->editor_camera_pitch = camera->pitch;
        state->player_camera_active = true_v;
      }
      vkr_camera_set_pose(camera, pose.position,
                          state->player.render_yaw * 57.2957795131f,
                          pose.pitch * 57.2957795131f);
    }
    sample_gameplay_visuals(application,
                            have_pose && state->view_state.camera_view ==
                                             VKR_SAMPLE_CAMERA_PERSPECTIVE
                                ? &pose
                                : NULL);
    vkr_scene_handle_sync(state->scene_resource.as.scene, &application->assets);
  } else {
    vkr_scene_handle_update_and_sync(state->scene_resource.as.scene,
                                     &application->assets, delta_time);
  }
  if (!vkr_scene_physics_sensor_events(application->active_scene,
                                       state->physics_sensor_events,
                                       ArrayCount(state->physics_sensor_events),
                                       &state->physics_sensor_event_count)) {
    snprintf(state->edits.status, sizeof(state->edits.status),
             "Physics sensor event drain failed: %s",
             vkr_scene_physics_error(application->active_scene));
  }

  if (application->gizmo_system.initialized) {
    if (!state->has_selection) {
      vkr_gizmo_system_clear_target(&application->gizmo_system);
      return;
    }

    VkrScene *scene =
        vkr_scene_handle_get_scene(state->scene_resource.as.scene);
    SceneTransform *transform =
        scene ? vkr_scene_get_transform(scene, state->selected_entity) : NULL;
    if (!transform) {
      vkr_standard_scene_runtime_clear_gizmo_selection(application);
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

    vkr_gizmo_system_set_target(&application->gizmo_system,
                                state->selected_entity, world_position,
                                vkr_quat_identity());
  }
}

vkr_internal void vkr_standard_scene_runtime_finish_gizmo_edit(
    VkrStandardSceneRuntime *application) {
  if (state->gizmo_edit_pending) {
    VkrScene *scene = application->active_scene;
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
      if (changed &&
          !vkr_scene_edit_record_external(&state->edits, scene,
                                          state->gizmo_edit_entity,
                                          &state->gizmo_before, &after) &&
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

vkr_internal void vkr_standard_scene_runtime_update_picking(
    VkrStandardSceneRuntime *application) {
  if (!application || !state || !state->input_state) {
    return;
  }

  VkrPickingContext *picking = &application->picking;
  if (!picking->initialized) {
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
    return;
  }

  bool8_t left_down = input_is_button_down(state->input_state, BUTTON_LEFT);
  bool8_t right_down = input_is_button_down(state->input_state, BUTTON_RIGHT);
  bool8_t middle_down = input_is_button_down(state->input_state, BUTTON_MIDDLE);
  bool8_t left_pressed =
      input_button_just_pressed(state->input_state, BUTTON_LEFT);
  bool8_t click_pressed = left_pressed;
  bool8_t click_select = left_pressed;

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
      if (!left_down)
        vkr_standard_scene_runtime_capture_gizmo_release(&viewport_info);
    }
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

    VkrAllocator *frame_alloc = &application->frame_allocator;
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
        entity = vkr_scene_handle_entity_from_picking_id(
            state->scene_resource.as.scene, result.object_id);

        if (entity.u64 != VKR_ENTITY_ID_INVALID.u64) {
          picked_entity = entity;
          picked_entity_valid = true_v;
          VkrScene *scene =
              vkr_scene_handle_get_scene(state->scene_resource.as.scene);
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
                         scene, decoded.value, &text_entity)) {
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
        if ((drag_button_down || state->gizmo_drag.released) &&
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

static void sample_view_apply(VkrStandardSceneRuntime *application,
                              const VkrSampleViewRequest *request) {
  if (!request->apply || !application->editor_viewport.enabled) {
    return;
  }
  VkrSampleViewState next = request->value;
  if ((uint32_t)next.camera_view >= VKR_SAMPLE_CAMERA_VIEW_COUNT ||
      (uint32_t)next.render_mode >= VKR_RENDER_MODE_COUNT ||
      !isfinite(next.grid_spacing) || next.grid_spacing <= 0.0f) {
    return;
  }
  next.grid_spacing = vkr_clamp_f32(next.grid_spacing, 0.001f, 10000.0f);
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
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
        if (state->player_camera_active) {
          vkr_camera_set_pose(camera, state->editor_camera_position,
                              state->editor_camera_yaw,
                              state->editor_camera_pitch);
          state->player_camera_active = false_v;
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
          {0, 0, -1}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}};
      static const Vec3 ups[VKR_SAMPLE_CAMERA_VIEW_COUNT] = {
          {0, 1, 0}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}, {0, 0, 1}};
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
    vkr_window_set_mouse_capture(&application->host.window, false_v);
    state->free_camera_held = false_v;
  }
  state->view_state = next;
  application->globals.render_mode = next.render_mode;
}

vkr_internal void
vkr_standard_scene_runtime_update_ui(VkrStandardSceneRuntime *application,
                                     float64_t delta) {
  if (!application || !state || !state->input_state ||
      !application->ui_system.initialized) {
    return;
  }

  vkr_scene_physics_set_paused(
      application->active_scene,
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
  if (!vkr_ui_begin(&application->ui_system, &application->frame_allocator,
                    vkr_standard_scene_runtime_is_windowed(application)
                        ? &application->host.window
                        : NULL,
                    application->renderer.last_window_width,
                    application->renderer.last_window_height,
                    state->input_state,
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
  VkrGraphicsSettingsRequest graphics_request = {0};
  VkrSampleTransportAction transport_action = VKR_SAMPLE_TRANSPORT_NONE;
  VkrSampleViewRequest view_request = {0};
  state->view_state.render_mode = application->globals.render_mode;
  VkrSamplePhysicsRequest physics_request = {0};
  VkrSceneEditRequest scene_edit = {0};
  VkrSampleSceneRequest scene_request = {0};
  VkrSampleEditorStateRequest editor_state_request = {0};
  VkrSampleCloseResponse close_response = VKR_SAMPLE_CLOSE_NONE;
  bool8_t scene_shortcuts_blocked = false_v;
  state->modal = false_v;
  application->editor_viewport.scene_backdrop_blur = false_v;
  application->animation_preview = (VkrAnimationPreviewRequest){0};
  const VkrMaterialTextureStreamStats texture_streams =
      vkr_material_system_get_texture_stream_stats(
          &application->assets.material_system);
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
      .scene_rendering_stopped =
          application->editor_viewport.scene_rendering_stopped,
      .scene_error = application->editor_viewport.scene_error,
      .texture_pending_count = texture_streams.pending_count,
      .texture_demanded_missing_count = texture_streams.demanded_missing_count,
      .scene_output_scale = application->scene_output_scale,
      .scene_render_width = application->editor_viewport.enabled
                                ? application->editor_viewport.rendered_width
                                : application->renderer.render_width,
      .scene_render_height = application->editor_viewport.enabled
                                 ? application->editor_viewport.rendered_height
                                 : application->renderer.render_height,
      .scene_output_width = application->editor_viewport.enabled
                                ? application->editor_viewport.output_width
                                : application->renderer.last_window_width,
      .scene_output_height = application->editor_viewport.enabled
                                 ? application->editor_viewport.output_height
                                 : application->renderer.last_window_height,
      .graphics = &state->graphics,
      .graphics_request = &graphics_request,
      .transport_action = &transport_action,
      .view_state = state->view_state,
      .view_request = &view_request,
      .physics_request = &physics_request,
      .collision_display = state->collision_display,
      .scene_keyboard_focus = &state->scene_keyboard_focus,
      .scene_backdrop_blur = &application->editor_viewport.scene_backdrop_blur,
      .animation_preview = &application->animation_preview,
      .scene_shortcuts_blocked = &scene_shortcuts_blocked,
      .scene = application->active_scene,
      .selected_entity = state->selected_entity,
      .scene_generation = application->scene_generation,
      .edits = &state->edits,
      .scene_edit = &scene_edit,
      .scene_request = &scene_request,
      .runtime_preferences = sample_preferences_snapshot(application),
      .scene_recall = sample_recall_snapshot(application),
      .editor_state_request = &editor_state_request,
      .close_requested = vkr_window_close_requested(&application->host.window),
      .close_response = &close_response,
      .scene_path = state->scene_path,
      .scene_status = string8_create_from_cstr(
          (const uint8_t *)state->scene_status, strlen(state->scene_status)),
      .scene_loading =
          state->scene_load_timer_active && !state->scene_load_terminal_logged,
      .modal = &state->modal,
      .scene_only = application->editor_viewport.scene_only,
      .mouse_captured = vkr_window_is_mouse_captured(&application->host.window),
  };
  if (application->editor_viewport.enabled) {
    frame.mapping_valid = vkr_standard_scene_runtime_editor_viewport_mapping(
        application, application->ui_system.target_width,
        application->ui_system.target_height, &frame.mapping);
  }
  application->editor_viewport.dock_capture =
      state->ui.build(state->ui.state, &frame);
  application->ui_capture = vkr_ui_end(&application->ui_system);
  sample_graphics_request(application, &graphics_request);
  sample_editor_state_apply(application, &editor_state_request);
  sample_view_apply(application, &view_request);
  if (close_response != VKR_SAMPLE_CLOSE_NONE) {
    vkr_window_resolve_close(&application->host.window,
                             close_response == VKR_SAMPLE_CLOSE_CONFIRM);
  }
  application->ui_capture.mouse |=
      application->editor_viewport.dock_capture.mouse;
  if (application->editor_viewport.enabled && !application->ui_capture.text &&
      !state->modal && !scene_shortcuts_blocked) {
    if (input_key_shortcut_modifier(state->input_state, KEY_S) &&
        input_key_just_pressed(state->input_state, KEY_S))
      scene_edit.action = VKR_SCENE_EDIT_SAVE;
    if (input_key_shortcut_modifier(state->input_state, KEY_Z) &&
        input_key_just_pressed(state->input_state, KEY_Z))
      scene_edit.action =
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
  if (scene_request.select || scene_request.unload) {
    vkr_standard_scene_runtime_finish_gizmo_edit(application);
    if (!scene_request.discard_edits &&
        state->edits.revision != state->edits.saved_revision) {
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Save or discard scene edits before switching.");
    } else if (scene_request.path.length >= sizeof(state->scene_path_storage) ||
               (scene_request.asset_root.length &&
                !scene_request.asset_root.str) ||
               scene_request.asset_root.length >=
                   sizeof(state->physics_asset_root) ||
               scene_request.sidecar_path.length >=
                   sizeof(state->sidecar_path) ||
               (scene_request.select && !scene_request.path.length)) {
      snprintf(state->scene_status, sizeof(state->scene_status),
               "Invalid scene selection or path is too long.");
    } else {
      /* Snapshot borrowed paths before unloading resets scene-owned storage. */
      char next_path[1024] = {0};
      char next_sidecar[1024] = {0};
      char next_asset_root[1024] = {0};
      if (scene_request.asset_root.length) {
        MemCopy(next_asset_root, scene_request.asset_root.str,
                scene_request.asset_root.length);
      } else {
        snprintf(next_asset_root, sizeof(next_asset_root), "%s",
                 PROJECT_SOURCE_DIR);
      }
      if (scene_request.path.length) {
        MemCopy(next_path, scene_request.path.str, scene_request.path.length);
      }
      if (scene_request.sidecar_path.length) {
        MemCopy(next_sidecar, scene_request.sidecar_path.str,
                scene_request.sidecar_path.length);
      }
      vkr_standard_scene_runtime_unload_scene_system(application);
      MemCopy(state->scene_path_storage, next_path, sizeof(next_path));
      MemCopy(state->sidecar_path, next_sidecar, sizeof(next_sidecar));
      MemCopy(state->physics_asset_root, next_asset_root,
              sizeof(next_asset_root));
      state->scene_path = string8_create_from_cstr(
          (const uint8_t *)state->scene_path_storage, strlen(next_path));
      state->scene_status[0] = '\0';
      scene_edit.action = VKR_SCENE_EDIT_NONE;
      if (scene_request.select) {
        application->editor_viewport.scene_rendering_stopped = false_v;
        vkr_standard_scene_runtime_init_scene_system(application);
      }
    }
  }
  if (state->gizmo_drag.active &&
      (scene_edit.action != VKR_SCENE_EDIT_NONE ||
       transport_action == VKR_SAMPLE_TRANSPORT_STOP_RENDERING))
    vkr_standard_scene_runtime_finish_gizmo_edit(application);
  if (scene_edit.action == VKR_SCENE_EDIT_LOAD)
    vkr_standard_scene_runtime_init_scene_system(application);
  if (scene_edit.action == VKR_SCENE_EDIT_UNLOAD ||
      scene_edit.action == VKR_SCENE_EDIT_RELOAD) {
    if (state->edits.revision != state->edits.saved_revision) {
      snprintf(state->edits.status, sizeof(state->edits.status),
               "Save edits before reloading or unloading.");
      log_warn("Save editor overrides before reloading or unloading");
    } else {
      vkr_standard_scene_runtime_unload_scene_system(application);
      if (scene_edit.action == VKR_SCENE_EDIT_RELOAD)
        vkr_standard_scene_runtime_init_scene_system(application);
    }
  }
  VkrScene *scene = application->active_scene;
  if (scene) {
    switch (scene_edit.action) {
    case VKR_SCENE_EDIT_SELECT:
      if (vkr_scene_entity_alive(scene, scene_edit.entity)) {
        vkr_standard_scene_runtime_cancel_gizmo_pick(application);
        vkr_standard_scene_runtime_clear_gizmo_handles(application);
        state->gizmo_drag.active = false_v;
        state->selected_entity = scene_edit.entity;
        state->has_selection = true_v;
      }
      break;
    case VKR_SCENE_EDIT_APPLY_COLLISION_LAYERS:
      (void)vkr_scene_edit_apply_collision_layers(&state->edits, scene,
                                                  scene_edit.collision_layers);
      break;
    case VKR_SCENE_EDIT_APPLY_PHYSICS_BATCH:
      (void)vkr_scene_edit_apply_physics_batch(&state->edits, scene,
                                               scene_edit.physics_batch,
                                               scene_edit.physics_batch_count);
      break;
    case VKR_SCENE_EDIT_APPLY:
      (void)vkr_scene_edit_apply(&state->edits, scene, scene_edit.entity,
                                 &scene_edit.values);
      break;
    case VKR_SCENE_EDIT_UNDO:
    case VKR_SCENE_EDIT_REDO:
      (void)vkr_scene_edit_undo(&state->edits, scene,
                                scene_edit.action == VKR_SCENE_EDIT_REDO);
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
          scene->world, scene_edit.entity, scene->comp_transform);
      VkrCamera *camera = vkr_camera_registry_get_by_handle(
          &application->camera_system, application->active_camera);
      if (tr && camera) {
        Vec3 lower = mat4_position(tr->world), upper = lower;
        for (uint32_t i = 0; i < scene->topo_count; i++) {
          VkrEntityId candidate = scene->topo_order[i], ancestor = candidate;
          while (ancestor.u64 && ancestor.u64 != scene_edit.entity.u64) {
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
        Vec3 target = vec3_scale(vec3_add(lower, upper), 0.5f);
        float32_t radius =
            Max(0.25f, vec3_length(vec3_sub(upper, lower)) * 0.5f);
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
        camera->position =
            vec3_sub(target, vec3_scale(camera->forward, distance));
        camera->view_dirty = true_v;
      }
      break;
    }
    default:
      break;
    }
  }
  switch (transport_action) {
  case VKR_SAMPLE_TRANSPORT_START_SIMULATION:
    application->editor_viewport.simulation_running = true_v;
    if (state->player.scene) {
      vkr_window_set_mouse_capture(&application->host.window, true_v);
    }
    break;
  case VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION:
    application->editor_viewport.simulation_running = false_v;
    if (state->player.scene) {
      vkr_window_set_mouse_capture(&application->host.window, false_v);
    }
    break;
  case VKR_SAMPLE_TRANSPORT_STEP_SIMULATION: {
    const char *error = NULL;
    if (!application->editor_viewport.simulation_running &&
        application->active_scene &&
        !vkr_scene_physics_step(application->active_scene, &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Physics step failed.");
    }
    break;
  }
  case VKR_SAMPLE_TRANSPORT_RESET_SIMULATION: {
    const char *error = NULL;
    application->editor_viewport.simulation_running = false_v;
    vkr_scene_physics_set_paused(application->active_scene, true_v);
    if (application->active_scene &&
        !vkr_scene_physics_reset(application->active_scene, &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Physics reset failed.");
    } else {
      application->editor_viewport.simulation_time = 0.0;
    }
    break;
  }
  case VKR_SAMPLE_TRANSPORT_TOGGLE_PHYSICS: {
    const char *error = NULL;
    if (application->active_scene &&
        !vkr_scene_physics_set_disabled(
            application->active_scene,
            !vkr_scene_physics_is_disabled(application->active_scene),
            &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Physics override failed.");
    }
    break;
  }
  case VKR_SAMPLE_TRANSPORT_CYCLE_COLLISION_DISPLAY:
    state->collision_display = (state->collision_display + 1u) % 3u;
    break;
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
  vkr_scene_physics_set_paused(
      application->active_scene,
      !application->editor_viewport.simulation_running);
  if (physics_request.set_body_disabled && application->active_scene) {
    const char *error = NULL;
    if (!vkr_scene_physics_set_body_disabled(
            application->active_scene, physics_request.entity,
            physics_request.body_disabled, &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Body session mute rejected.");
    }
  }
  if (physics_request.apply_impulse && application->active_scene) {
    const char *error = NULL;
    if (!vkr_scene_physics_impulse(
            application->active_scene, physics_request.entity,
            physics_request.impulse,
            physics_request.at_point ? &physics_request.world_point : NULL,
            &error)) {
      snprintf(state->scene_status, sizeof(state->scene_status), "%s",
               error ? error : "Impulse rejected.");
    }
  }
}

static void
vkr_standard_scene_runtime_project_ui(VkrStandardSceneRuntime *application,
                                      const VkrViewportMapping *mapping) {
  const VkrSampleUiFrame frame = {
      .ui = &application->ui_system,
      .view_state = state->view_state,
      .mapping = *mapping,
      .mapping_valid = true_v,
      .view_projection =
          mat4_mul(application->globals.projection, application->globals.view),
      .scene = application->active_scene,
      .scene_generation = application->scene_generation,
      .scene_rendering_stopped =
          vkr_standard_scene_runtime_editor_scene_rendering_stopped(
              application),
  };
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

  if (!application->ui_capture.keyboard &&
      input_is_key_up(state->input_state, KEY_Q) &&
      input_was_key_down(state->input_state, KEY_Q)) {
    application->globals.render_mode =
        (VkrRenderMode)(((uint32_t)application->globals.render_mode + 1) %
                        VKR_RENDER_MODE_COUNT);
    log_debug("RENDER MODE: %d", application->globals.render_mode);
  }

  if (!application->ui_capture.keyboard &&
      input_is_key_up(state->input_state, KEY_E) &&
      input_was_key_down(state->input_state, KEY_E)) {
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

int vkr_sample_runtime_run(int argc, char **argv,
                           const VkrSampleRuntimeConfig *runtime_config) {
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
  application.host.window.defer_close = runtime_config->project_managed;
  if (options.metal_validation_enabled &&
      renderer_backend == VKR_RENDERER_BACKEND_TYPE_METAL) {
    log_info("Metal validation enabled; the Scene uses fixed-scale spatial "
             "reconstruction instead of MetalFX");
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

  state = arena_alloc(application.app_arena, sizeof(State),
                      ARENA_MEMORY_TAG_STRUCT);
  state->graphics = options.graphics;
  state->graphics_started = options.graphics.settings;
  state->graphics_dirty = false_v;
  state->graphics_changed_at = 0.0;
  state->graphics_message[0] = '\0';
  snprintf(state->graphics_path, sizeof(state->graphics_path), "%s",
           options.graphics_path);
  if (!options.graphics_loaded)
    snprintf(state->graphics_message, sizeof(state->graphics_message),
             "Saved settings were invalid. Defaults are in use.");
  sample_graphics_apply_live(&application, &state->graphics.settings);
  state->stats_arena = arena_create(KB(1), KB(1));
  VkrAllocator app_alloc = {.ctx = application.app_arena};
  vkr_allocator_arena(&app_alloc);
  state->input_state = &application.host.window.input_state;
  state->view_state = (VkrSampleViewState){
      .camera_view = VKR_SAMPLE_CAMERA_PERSPECTIVE,
      .render_mode = application.globals.render_mode,
      .grid_spacing = 1.0f,
  };
  state->app_arena = application.app_arena;
  state->event_arena = application.host.events.arena;
  state->event_manager = &application.host.events;
  state->fps_update_clock = vkr_clock_create();
  state->memory_update_clock = vkr_clock_create();
  state->fps_accumulated_time = 0.0;
  state->fps_frame_count = 0;
  state->current_fps = 0.0;
  state->current_frametime = 0.0;
  state->ui = runtime_config->ui;
  application.project_ui =
      state->ui.project_scene ? vkr_standard_scene_runtime_project_ui : NULL;
  snprintf(state->scene_path_storage, sizeof(state->scene_path_storage), "%s",
           runtime_config->project_managed ? "" : options.scene_path);
  state->scene_path =
      string8_create_from_cstr((const uint8_t *)state->scene_path_storage,
                               strlen(state->scene_path_storage));
  state->sidecar_path[0] = '\0';
  state->scene_status[0] = '\0';
  snprintf(state->physics_asset_root, sizeof(state->physics_asset_root), "%s",
           PROJECT_SOURCE_DIR);
  state->modal = runtime_config->project_managed;
  if (!runtime_config->project_managed) {
    const char *scene_path = options.scene_path;
    const bool8_t absolute = scene_path[0] == '/' || scene_path[0] == '\\' ||
                             (strlen(scene_path) > 1u && scene_path[1] == ':');
    snprintf(state->sidecar_path, sizeof(state->sidecar_path),
             "%s%s.editor.json", absolute ? "" : PROJECT_SOURCE_DIR,
             scene_path);
  }
  state->edits = (VkrSceneEditState){
      .allocator = &application.ui_system.retained_allocator};
  state->gizmo_edit_pending = false_v;
  application.editor_viewport.simulation_running =
      !runtime_config->presentation.paneled;
  state->collision_display = 1u;
  if (!state->ui.initialize(state->ui.state, &application.editor_viewport.dock,
                            &application.ui_system)) {
    arena_destroy(state->stats_arena);
    state->stats_arena = NULL;
    vkr_standard_scene_runtime_shutdown(&application);
    return 6;
  }
  state->gameplay_enabled = options.gameplay_enabled;
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

  if (options.auto_close_seconds > 0.0) {
    state->auto_close_enabled = true_v;
    state->auto_close_after_seconds = options.auto_close_seconds;
    log_info("Auto-close enabled via VKR_AUTOCLOSE_SECONDS=%.2f",
             options.auto_close_seconds);
  } else if (options.auto_close_rejected) {
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

  Scratch scratch = scratch_create(application.app_arena);
  vkr_renderer_get_device_information(
      &application.renderer, &state->device_information, scratch.arena);
  log_info("Device Name: %s", state->device_information.device_name.str);
  log_info("Device Vendor: %s", state->device_information.vendor_name.str);
  log_info("Device Driver Version: %s",
           state->device_information.driver_version.str);
  log_info("Device Graphics API Version: %s",
           state->device_information.api_version.str);
  log_info("Device VRAM Size: %.2f GB",
           (float64_t)state->device_information.vram_size / GB(1));
  log_info("Device VRAM Local Size: %.2f GB",
           (float64_t)state->device_information.vram_local_size / GB(1));
  log_info("Device VRAM Shared Size: %.2f GB",
           (float64_t)state->device_information.vram_shared_size / GB(1));
  VkrPlatformSystemInfo system_info = {0};
  (void)vkr_platform_get_system_info(&system_info);
  const String8 gpu_name = state->device_information.device_name;
  snprintf(state->hardware_text, sizeof(state->hardware_text),
           "CPU: %s\nGPU: %.*s",
           system_info.cpu[0] ? system_info.cpu : "unavailable",
           (int32_t)gpu_name.length, gpu_name.str);
  snprintf(state->system_text, sizeof(state->system_text),
           "%s\nRAM (resident): pending\nGPU memory (managed): pending",
           state->hardware_text);
  state->anisotropy_supported =
      bitset8_is_set(&state->device_information.sampler_filters,
                     VKR_SAMPLER_FILTER_ANISOTROPIC_BIT);
  state->filter_mode_index = 3; // Bilinear default (index in FILTER_MODES)

  log_info("Texture filtering controls: F4=prev, F5=next (start: %s)",
           FILTER_MODES[state->filter_mode_index].label);
  log_info("IBL validation controls: F8=mode, F9/F10=intensity "
           "(start: %s x%.2f)",
           vkr_standard_scene_runtime_ibl_validation_mode_label(
               state->ibl_validation_mode),
           state->ibl_validation_scalar);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

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

  vkr_standard_scene_runtime_unload_scene_system(&application);
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
  return value &&
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
         vkr_json_writer_u64(writer, 1) &&
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
  if (!sample_read_uint(json, "version", &version) || version != 1 ||
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
    result.position = state->player_camera_active
                          ? state->editor_camera_position
                          : camera->position;
    result.yaw =
        state->player_camera_active ? state->editor_camera_yaw : camera->yaw;
    result.pitch = state->player_camera_active ? state->editor_camera_pitch
                                               : camera->pitch;
    result.field_of_view = camera->zoom;
    result.near_plane = camera->near_clip;
    result.far_plane = camera->far_clip;
  }
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
  if (request->apply_recall && sample_recall_valid(&request->recall)) {
    const VkrSampleSceneRecall *value = &request->recall;
    if (camera && value->camera_valid) {
      camera->type = VKR_CAMERA_TYPE_PERSPECTIVE;
      state->view_state.camera_view = VKR_SAMPLE_CAMERA_PERSPECTIVE;
      state->perspective_camera_saved = false_v;
      state->player_camera_active = false_v;
      vkr_camera_set_pose(camera, value->position, value->yaw, value->pitch);
      (void)vkr_camera_set_perspective_lens(
          camera, value->field_of_view, value->near_plane, value->far_plane,
          Max(camera->cached_window_width, 1u),
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

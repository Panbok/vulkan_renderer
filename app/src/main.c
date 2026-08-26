#include "application.h"
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
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/vkr_renderer.h"
#include <stdio.h>

#define VKR_FPS_UPDATE_INTERVAL 0.25
#define VKR_MEMORY_UPDATE_INTERVAL 1.0
#define VKR_FPS_DELTA_MIN 0.000001
#define VKR_WORLD_TIME_UPDATE_INTERVAL 0.25
#define VKR_UI_TEXT_PADDING 16.0f
#define SCENE_PATH "assets/scenes/bistro.scene.json"

typedef struct FilterModeEntry {
  VkrFilter min_filter;
  VkrFilter mag_filter;
  VkrMipFilter mip_filter;
  bool8_t anisotropy;
  const char *label;
} FilterModeEntry;

static const FilterModeEntry FILTER_MODES[] = {
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
  Vec3 axis;
  Vec3 plane_normal;
  Vec3 start_world_position;
  Vec3 start_hit;
  Vec3 start_scale;
  VkrQuat start_rotation;
  float32_t start_radius;
  bool8_t uses_text_pivot;
  Vec3 text_pivot_local;
  uint32_t pick_x;
  uint32_t pick_y;
  uint32_t pick_width;
  uint32_t pick_height;
} GizmoDragState;

typedef struct State {
  InputState *input_state;

  Arena *app_arena;
  Arena *event_arena;
  Arena *stats_arena;

  uint32_t filter_mode_index;
  bool8_t anisotropy_supported;
  VkrDeviceInformation device_information;

  EventManager *event_manager; // For dispatching events

  uint32_t fps_text_id;
  uint32_t left_text_id;
  uint32_t memory_text_id;
  uint32_t metrics_text_id;
  VkrClock fps_update_clock;
  VkrClock memory_update_clock;
  float64_t fps_accumulated_time;
  uint32_t fps_frame_count;
  float64_t current_fps;
  float64_t current_frametime;

  uint32_t world_text_id;
  VkrClock world_text_update_clock;

  // Picking demo state
  uint32_t picked_object_text_id;
  uint32_t last_picked_object_id;
  VkrEntityId selected_entity;
  bool8_t has_selection;

  // Gizmo interaction state
  GizmoDragState gizmo_drag;
  bool8_t gizmo_hover_pending;
  VkrGizmoHandle gizmo_hot_handle;

  bool8_t free_camera_use_gamepad;
  bool8_t free_camera_wheel_initialized;
  int8_t free_camera_prev_wheel_delta;

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

vkr_internal bool8_t application_metric_duration_seconds(
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
vkr_internal void application_accumulate_frame_time(Application *application) {
  VkrMetricsSnapshotView snapshot = {0};
  if (!vkr_metrics_snapshot_acquire(application->metrics, &snapshot)) {
    return;
  }
  float64_t frame_seconds = 0.0;
  if (snapshot.publication_serial != state->hud_last_publication_serial &&
      application_metric_duration_seconds(
          snapshot.frame, application->metric_ids.frame_wall, &frame_seconds)) {
    state->hud_last_publication_serial = snapshot.publication_serial;
    state->hud_frametime_sum += frame_seconds;
    state->hud_frame_samples++;
  }
  vkr_metrics_snapshot_release(application->metrics, &snapshot);
}

/**
 * @brief Parses common truthy/falsy environment values.
 *
 * Unknown values keep the provided default to avoid brittle automation.
 */
vkr_internal bool8_t application_env_flag(const char *name,
                                          bool8_t default_value) {
  if (!name || name[0] == '\0') {
    return default_value;
  }

  const char *value = getenv(name);
  if (!value || value[0] == '\0') {
    return default_value;
  }

  switch (value[0]) {
  case '1':
  case 'y':
  case 'Y':
  case 't':
  case 'T':
    return true_v;
  case '0':
  case 'n':
  case 'N':
  case 'f':
  case 'F':
    return false_v;
  default:
    return default_value;
  }
}

vkr_internal const char *application_ibl_validation_mode_label(uint32_t mode) {
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

vkr_internal void
application_update_ibl_validation_controls(Application *application) {
  if (!application || !state) {
    return;
  }

  VkrScene *scene = application->renderer.active_scene;
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

vkr_internal bool8_t application_capture_backend_allocator_stats(
    Application *application, VkrAllocatorStatistics *out_stats) {
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

vkr_internal int64_t application_stat_delta(uint64_t current,
                                            uint64_t baseline) {
  if (current >= baseline) {
    return (int64_t)(current - baseline);
  }
  return -(int64_t)(baseline - current);
}

vkr_internal const char *
application_allocator_tag_name(VkrAllocatorMemoryTag tag) {
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

vkr_internal double application_bytes_to_mb(uint64_t bytes) {
  return (double)bytes / (double)MB(1);
}

vkr_internal double application_delta_bytes_to_mb(int64_t bytes) {
  return (double)bytes / (double)MB(1);
}
vkr_internal const char *
application_gpu_allocation_owner_name(VkrGpuAllocationOwner owner) {
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

vkr_internal bool8_t application_memory_text_append(char **write,
                                                    size_t *remaining,
                                                    const char *text) {
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

vkr_internal bool8_t application_memory_text_append_size(char **write,
                                                         size_t *remaining,
                                                         const char *label,
                                                         uint64_t bytes) {
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

vkr_internal void application_log_backend_allocator_breakdown(
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
        application_allocator_tag_name((VkrAllocatorMemoryTag)tag),
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
vkr_internal void application_log_device_memory_stats(Application *application,
                                                      const char *label) {
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
           application_bytes_to_mb(stats.live_bytes),
           application_bytes_to_mb(stats.peak_bytes),
           stats.live_totals_exact ? "yes" : "no");
  for (uint32_t owner = 0; owner < VKR_GPU_ALLOCATION_OWNER_COUNT; ++owner) {
    const VkrGpuAllocationOwnerTotals *totals = &stats.owners[owner];
    if (!totals->live_allocation_count && !totals->total_allocation_count)
      continue;
    log_info(
        "GPU_MEM   owner=%s allocations=live:%llu peak:%llu total:%llu "
        "bytes=live:%.3fMB peak:%.3fMB total:%.3fMB",
        application_gpu_allocation_owner_name((VkrGpuAllocationOwner)owner),
        (unsigned long long)totals->live_allocation_count,
        (unsigned long long)totals->peak_allocation_count,
        (unsigned long long)totals->total_allocation_count,
        application_bytes_to_mb(totals->live_bytes),
        application_bytes_to_mb(totals->peak_bytes),
        application_bytes_to_mb(totals->total_bytes));
  }

  for (uint32_t i = 0; i < stats.memory_type_count; ++i) {
    if (stats.live_count_by_type[i] == 0) {
      continue;
    }
    log_info("GPU_MEM   type=%u heap=%u flags=0x%x count=%llu bytes=%.3fMB", i,
             stats.heap_index_by_type[i], stats.property_flags_by_type[i],
             (unsigned long long)stats.live_count_by_type[i],
             application_bytes_to_mb(stats.live_bytes_by_type[i]));
  }

  for (uint32_t i = 0; i < stats.heap_count; ++i) {
    if (stats.heap_usage_valid) {
      log_info("GPU_MEM   heap=%u size=%.3fMB usage=%.3fMB budget=%.3fMB", i,
               application_bytes_to_mb(stats.heap_size_bytes[i]),
               application_bytes_to_mb(stats.heap_usage_bytes[i]),
               application_bytes_to_mb(stats.heap_budget_bytes[i]));
    } else {
      log_info("GPU_MEM   heap=%u size=%.3fMB (usage unavailable: no "
               "VK_EXT_memory_budget)",
               i, application_bytes_to_mb(stats.heap_size_bytes[i]));
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
vkr_internal void application_dump_periodic_metrics(Application *application) {
  if (!application || !state || !state->metrics_dump_enabled) {
    return;
  }
  if (!vkr_clock_interval_elapsed(&state->metrics_dump_clock,
                                  state->metrics_dump_interval_seconds)) {
    return;
  }

  char label[64];
  snprintf(label, sizeof(label), "tick%u@%.1fs", state->metrics_dump_index,
           application->clock.elapsed);
  state->metrics_dump_index++;

  application_log_device_memory_stats(application, label);

  VkrAllocatorStatistics stats = {0};
  if (application_capture_backend_allocator_stats(application, &stats)) {
    log_info(
        "CPU_ALLOC label=%s total=%.3fMB renderer=%.3fMB array=%.3fMB "
        "string=%.3fMB file=%.3fMB vulkan_state=%.3fMB texture_temp=%.3fMB",
        label, application_bytes_to_mb(stats.total_allocated),
        application_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_RENDERER]),
        application_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]),
        application_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]),
        application_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_FILE]),
        application_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_VULKAN]),
        application_bytes_to_mb(
            stats.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_TEXTURE]));
  }
}

vkr_internal void application_log_backend_allocator_stats(
    Application *application, const char *label,
    const VkrAllocatorStatistics *baseline_stats) {
  if (!application || !label) {
    return;
  }

  VkrAllocatorStatistics stats = {0};
  if (!application_capture_backend_allocator_stats(application, &stats)) {
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
              label, application_bytes_to_mb(total_bytes),
              application_bytes_to_mb(renderer_bytes),
              application_bytes_to_mb(vulkan_state_bytes),
              application_bytes_to_mb(texture_temp_bytes));
    if (state && state->scene_memory_verbose) {
      application_log_backend_allocator_breakdown(label, &stats);
    }
    return;
  }

  const int64_t delta_total =
      application_stat_delta(total_bytes, baseline_stats->total_allocated);
  const int64_t delta_renderer = application_stat_delta(
      renderer_bytes,
      baseline_stats->tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_RENDERER]);
  const int64_t delta_vulkan_state = application_stat_delta(
      vulkan_state_bytes,
      baseline_stats->tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_VULKAN]);
  const int64_t delta_texture_temp = application_stat_delta(
      texture_temp_bytes,
      baseline_stats->tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_TEXTURE]);

  log_debug("SCENE_CPU_ALLOC label=%s total=%.3fMB renderer=%.3fMB "
            "vulkan_state=%.3fMB texture_temp=%.3fMB delta_total=%+.3fMB "
            "delta_renderer=%+.3fMB delta_vulkan_state=%+.3fMB "
            "delta_texture_temp=%+.3fMB",
            label, application_bytes_to_mb(total_bytes),
            application_bytes_to_mb(renderer_bytes),
            application_bytes_to_mb(vulkan_state_bytes),
            application_bytes_to_mb(texture_temp_bytes),
            application_delta_bytes_to_mb(delta_total),
            application_delta_bytes_to_mb(delta_renderer),
            application_delta_bytes_to_mb(delta_vulkan_state),
            application_delta_bytes_to_mb(delta_texture_temp));
  if (state && state->scene_memory_verbose) {
    application_log_backend_allocator_breakdown(label, &stats);
  }
}

vkr_internal float64_t
application_consume_scene_load_elapsed_seconds(Application *application) {
  if (!application || !state || !state->scene_load_timer_active) {
    return -1.0;
  }

  float64_t elapsed =
      application->clock.elapsed - state->scene_load_start_time_seconds;
  if (elapsed < 0.0) {
    elapsed = 0.0;
  }

  state->scene_load_timer_active = false_v;
  return elapsed;
}

vkr_internal void application_queue_ui_text_update(Application *application,
                                                   uint32_t text_id,
                                                   String8 content) {
  if (!application || text_id == VKR_INVALID_ID) {
    return;
  }

  for (uint32_t i = 0; i < application->ui_text_update_count; ++i) {
    ApplicationTextUpdate *slot = &application->ui_text_updates[i];
    if (slot->text_id == text_id) {
      slot->content = content;
      return;
    }
  }

  if (application->ui_text_update_count >= VKR_MAX_PENDING_TEXT_UPDATES) {
    log_warn("UI text update queue full; dropping text %u", text_id);
    return;
  }

  application->ui_text_updates[application->ui_text_update_count++] =
      (ApplicationTextUpdate){
          .text_id = text_id,
          .content = content,
          .has_transform = false_v,
      };
}

vkr_internal void
application_queue_world_text_update(Application *application, uint32_t text_id,
                                    String8 content,
                                    const VkrTransform *transform) {
  if (!application || text_id == VKR_INVALID_ID) {
    return;
  }

  for (uint32_t i = 0; i < application->world_text_update_count; ++i) {
    ApplicationTextUpdate *slot = &application->world_text_updates[i];
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

  if (application->world_text_update_count >= VKR_MAX_PENDING_TEXT_UPDATES) {
    log_warn("World text update queue full; dropping text %u", text_id);
    return;
  }

  ApplicationTextUpdate update = {
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
  uint32_t target_x;
  uint32_t target_y;
  uint32_t target_width;
  uint32_t target_height;
  bool8_t has_target_coords;
} VkrViewportHitInfo;

/**
 * @brief Compute viewport mapping info for world picking and gizmo rays.
 */
vkr_internal VkrViewportHitInfo application_get_viewport_hit_info(
    Application *application, int32_t mouse_x, int32_t mouse_y) {
  VkrViewportHitInfo info = {0};
  if (!application || !state) {
    return info;
  }

  if (application->editor_viewport.enabled &&
      application->renderer.editor_viewport.initialized) {
    VkrViewportMapping mapping = {0};
    VkrWindowPixelSize window_size =
        vkr_window_get_pixel_size(&application->window);
    if (vkr_editor_viewport_compute_mapping(
            window_size.width, window_size.height,
            application->editor_viewport.fit_mode,
            application->editor_viewport.render_scale, &mapping)) {
      info.target_width = mapping.target_width;
      info.target_height = mapping.target_height;
      if (vkr_viewport_mapping_window_to_target_pixel(
              &mapping, mouse_x, mouse_y, &info.target_x, &info.target_y)) {
        info.has_target_coords = true_v;
      }
    }
  } else {
    VkrWindowPixelSize window_size =
        vkr_window_get_pixel_size(&application->window);
    info.target_width = window_size.width;
    info.target_height = window_size.height;
    if (mouse_x >= 0 && mouse_y >= 0 && (uint32_t)mouse_x < window_size.width &&
        (uint32_t)mouse_y < window_size.height) {
      info.target_x = (uint32_t)mouse_x;
      info.target_y = (uint32_t)mouse_y;
      info.has_target_coords = true_v;
    }
  }

  return info;
}

/**
 * @brief Build a world-space ray from a viewport pixel coordinate.
 */
vkr_internal bool8_t application_build_view_ray(
    VkrCamera *camera, uint32_t viewport_width, uint32_t viewport_height,
    uint32_t target_x, uint32_t target_y, Vec3 *out_origin, Vec3 *out_dir) {
  if (!camera || !out_origin || !out_dir || viewport_width == 0 ||
      viewport_height == 0) {
    return false_v;
  }

  vkr_camera_system_update(camera);

  Mat4 view = vkr_camera_system_get_view_matrix(camera);
  Mat4 projection = vkr_camera_system_get_projection_matrix(camera);
  Mat4 inv_vp = mat4_inverse(mat4_mul(projection, view));

  float32_t ndc_x = 0.0f;
  if (viewport_width > 1u) {
    ndc_x =
        ((float32_t)target_x / (float32_t)(viewport_width - 1u)) * 2.0f - 1.0f;
  }

  float32_t ndc_y = 0.0f;
  if (viewport_height > 1u) {
    ndc_y =
        ((float32_t)target_y / (float32_t)(viewport_height - 1u)) * 2.0f - 1.0f;
  }

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
vkr_internal bool8_t application_ray_plane_intersect(Vec3 ray_origin,
                                                     Vec3 ray_dir,
                                                     Vec3 plane_point,
                                                     Vec3 plane_normal,
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
vkr_internal Vec3 application_gizmo_axis_plane_normal(const VkrCamera *camera,
                                                      Vec3 axis) {
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

vkr_internal void application_clear_gizmo_handles(Application *application) {
  if (!application || !state) {
    return;
  }

  vkr_gizmo_system_set_active_handle(&application->renderer.gizmo_system,
                                     VKR_GIZMO_HANDLE_NONE);
  vkr_gizmo_system_set_hot_handle(&application->renderer.gizmo_system,
                                  VKR_GIZMO_HANDLE_NONE);
  state->gizmo_hot_handle = VKR_GIZMO_HANDLE_NONE;
}

vkr_internal void application_clear_gizmo_selection(Application *application) {
  if (!application || !state) {
    return;
  }

  state->selected_entity = VKR_ENTITY_ID_INVALID;
  state->has_selection = false_v;
  vkr_gizmo_system_clear_target(&application->renderer.gizmo_system);
  state->gizmo_hot_handle = VKR_GIZMO_HANDLE_NONE;
  state->gizmo_drag.uses_text_pivot = false_v;
}

vkr_internal bool8_t application_world_text_entity_from_id(
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
vkr_internal bool8_t application_text_pivot_local(SceneText3D *text,
                                                  Vec3 *out_local) {
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
vkr_internal Vec3 application_text_pivot_world(const SceneTransform *transform,
                                               Vec3 pivot_local) {
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
vkr_internal Vec3 application_text_origin_from_pivot(
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

vkr_internal void
application_sync_world_text_transform(Application *application, VkrScene *scene,
                                      VkrEntityId entity) {
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

  application_queue_world_text_update(application, text->text_index,
                                      (String8){0}, &text_transform);
}

vkr_internal bool8_t application_request_picking(
    Application *application, VkrPickingContext *picking,
    const VkrViewportHitInfo *viewport_info) {
  if (!application || !picking || !viewport_info ||
      !viewport_info->has_target_coords || viewport_info->target_width == 0 ||
      viewport_info->target_height == 0) {
    return false_v;
  }

  if (picking->width != viewport_info->target_width ||
      picking->height != viewport_info->target_height) {
    vkr_picking_resize(&application->renderer, picking,
                       viewport_info->target_width,
                       viewport_info->target_height);
  }

  if (viewport_info->target_x >= picking->width ||
      viewport_info->target_y >= picking->height) {
    return false_v;
  }

  vkr_picking_request(picking, viewport_info->target_x,
                      viewport_info->target_y);
  return true_v;
}

vkr_internal bool8_t application_begin_gizmo_drag(Application *application,
                                                  VkrGizmoHandle handle) {
  if (!application || !state || !state->has_selection) {
    return false_v;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(state->scene_resource.as.scene);
  if (!scene) {
    return false_v;
  }

  SceneTransform *transform =
      vkr_scene_get_transform(scene, state->selected_entity);
  if (!transform) {
    return false_v;
  }

  Vec3 pivot_local = vec3_zero();
  bool8_t has_text_pivot = false_v;
  SceneText3D *text = vkr_scene_get_text3d(scene, state->selected_entity);
  if (text) {
    has_text_pivot = application_text_pivot_local(text, &pivot_local);
  }

  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                        application->renderer.active_camera);
  if (!camera) {
    return false_v;
  }

  VkrGizmoMode mode = vkr_gizmo_handle_mode(handle);
  if (mode == VKR_GIZMO_MODE_NONE) {
    return false_v;
  }

  Vec3 axis = vec3_zero();
  bool8_t has_axis = vkr_gizmo_handle_axis(handle, &axis);
  Vec3 plane_normal = vec3_zero();

  if (mode == VKR_GIZMO_MODE_SCALE) {
    axis = vec3_zero();
    has_axis = false_v;
  }

  if (mode == VKR_GIZMO_MODE_ROTATE) {
    if (!has_axis) {
      axis = vec3_normalize(camera->forward);
      has_axis = true_v;
    }
    plane_normal = axis;
  } else {
    if (!has_axis) {
      plane_normal = vec3_normalize(camera->forward);
    } else {
      plane_normal = application_gizmo_axis_plane_normal(camera, axis);
    }
  }

  Vec3 world_position = mat4_position(transform->world);
  if (has_text_pivot) {
    world_position = application_text_pivot_world(transform, pivot_local);
  }
  Vec3 ray_origin = vec3_zero();
  Vec3 ray_dir = vec3_zero();
  if (!application_build_view_ray(
          camera, state->gizmo_drag.pick_width, state->gizmo_drag.pick_height,
          state->gizmo_drag.pick_x, state->gizmo_drag.pick_y, &ray_origin,
          &ray_dir)) {
    return false_v;
  }

  Vec3 hit = vec3_zero();
  if (!application_ray_plane_intersect(ray_origin, ray_dir, world_position,
                                       plane_normal, &hit)) {
    return false_v;
  }

  Vec3 offset = vec3_sub(hit, world_position);

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

vkr_internal void
application_update_gizmo_drag(Application *application,
                              const VkrViewportHitInfo *viewport_info) {
  if (!application || !state || !state->gizmo_drag.active || !viewport_info ||
      !viewport_info->has_target_coords) {
    return;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(state->scene_resource.as.scene);
  if (!scene) {
    state->gizmo_drag.active = false_v;
    state->gizmo_drag.handle = VKR_GIZMO_HANDLE_NONE;
    return;
  }

  SceneTransform *transform =
      vkr_scene_get_transform(scene, state->selected_entity);
  if (!transform) {
    state->gizmo_drag.active = false_v;
    state->gizmo_drag.handle = VKR_GIZMO_HANDLE_NONE;
    return;
  }

  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                        application->renderer.active_camera);
  if (!camera) {
    state->gizmo_drag.active = false_v;
    state->gizmo_drag.handle = VKR_GIZMO_HANDLE_NONE;
    return;
  }

  Vec3 ray_origin = vec3_zero();
  Vec3 ray_dir = vec3_zero();
  if (!application_build_view_ray(
          camera, viewport_info->target_width, viewport_info->target_height,
          viewport_info->target_x, viewport_info->target_y, &ray_origin,
          &ray_dir)) {
    return;
  }

  Vec3 hit = vec3_zero();
  if (!application_ray_plane_intersect(ray_origin, ray_dir,
                                       state->gizmo_drag.start_hit,
                                       state->gizmo_drag.plane_normal, &hit)) {
    return;
  }

  Vec3 delta = vec3_sub(hit, state->gizmo_drag.start_hit);

  bool8_t updated = false_v;

  if (state->gizmo_drag.mode == VKR_GIZMO_MODE_TRANSLATE) {
    Vec3 new_pivot = state->gizmo_drag.start_world_position;
    if (vkr_gizmo_handle_is_free_translate(state->gizmo_drag.handle)) {
      new_pivot = vec3_add(state->gizmo_drag.start_world_position, delta);
    } else {
      float32_t dist = vec3_dot(delta, state->gizmo_drag.axis);
      Vec3 axis_delta = vec3_scale(state->gizmo_drag.axis, dist);
      new_pivot = vec3_add(state->gizmo_drag.start_world_position, axis_delta);
    }

    Vec3 local_pos = new_pivot;
    if (state->gizmo_drag.uses_text_pivot) {
      local_pos = application_text_origin_from_pivot(
          scene, transform, new_pivot, state->gizmo_drag.text_pivot_local,
          state->gizmo_drag.start_scale, state->gizmo_drag.start_rotation);
    } else if (transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64) {
      SceneTransform *parent_transform =
          vkr_scene_get_transform(scene, transform->parent);
      if (parent_transform) {
        Mat4 parent_inv = mat4_inverse_affine(parent_transform->world);
        Vec4 local = mat4_mul_vec4(
            parent_inv, vec4_new(new_pivot.x, new_pivot.y, new_pivot.z, 1.0f));
        local_pos = vec3_new(local.x, local.y, local.z);
      }
    }

    vkr_scene_set_position(scene, state->selected_entity, local_pos);
    updated = true_v;
  } else if (state->gizmo_drag.mode == VKR_GIZMO_MODE_SCALE) {
    Vec3 new_scale = state->gizmo_drag.start_scale;
    const float32_t min_scale = 0.001f;
    Vec3 offset = vec3_sub(hit, state->gizmo_drag.start_world_position);

    if (state->gizmo_drag.start_radius > VKR_FLOAT_EPSILON) {
      float32_t radius = vec3_length(offset);
      float32_t scale_factor = radius / state->gizmo_drag.start_radius;
      new_scale = vec3_scale(state->gizmo_drag.start_scale, scale_factor);
    }

    new_scale.x = vkr_max_f32(min_scale, new_scale.x);
    new_scale.y = vkr_max_f32(min_scale, new_scale.y);
    new_scale.z = vkr_max_f32(min_scale, new_scale.z);
    if (state->gizmo_drag.uses_text_pivot) {
      Vec3 local_pos = application_text_origin_from_pivot(
          scene, transform, state->gizmo_drag.start_world_position,
          state->gizmo_drag.text_pivot_local, new_scale,
          state->gizmo_drag.start_rotation);
      vkr_scene_set_position(scene, state->selected_entity, local_pos);
    }

    vkr_scene_set_scale(scene, state->selected_entity, new_scale);
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

    Vec3 local_axis = state->gizmo_drag.axis;
    if (transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64) {
      SceneTransform *parent_transform =
          vkr_scene_get_transform(scene, transform->parent);
      if (parent_transform) {
        Mat4 parent_inv = mat4_inverse_affine(parent_transform->world);
        Vec4 axis_local =
            mat4_mul_vec4(parent_inv, vec4_new(local_axis.x, local_axis.y,
                                               local_axis.z, 0.0f));
        local_axis =
            vec3_normalize(vec3_new(axis_local.x, axis_local.y, axis_local.z));
      }
    }

    VkrQuat delta = vkr_quat_from_axis_angle(local_axis, angle);
    VkrQuat new_rotation =
        vkr_quat_mul(delta, state->gizmo_drag.start_rotation);
    new_rotation = vkr_quat_normalize(new_rotation);

    if (state->gizmo_drag.uses_text_pivot) {
      Vec3 local_pos = application_text_origin_from_pivot(
          scene, transform, state->gizmo_drag.start_world_position,
          state->gizmo_drag.text_pivot_local, state->gizmo_drag.start_scale,
          new_rotation);
      vkr_scene_set_position(scene, state->selected_entity, local_pos);
    }

    vkr_scene_set_rotation(scene, state->selected_entity, new_rotation);
    updated = true_v;
  }

  if (updated) {
    application_sync_world_text_transform(application, scene,
                                          state->selected_entity);
  }
}

vkr_internal void application_apply_filter_mode(Application *application,
                                                uint32_t mode_index) {
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

  VkrTextureSystem *texture_system = &application->renderer.texture_system;
  uint32_t failures = 0;
  for (uint32_t i = 0; i < texture_system->textures.length; ++i) {
    VkrTexture *tex = &texture_system->textures.data[i];
    if (!tex->handle || tex->description.generation == VKR_INVALID_ID ||
        tex->description.id == VKR_INVALID_ID) {
      continue;
    }

    VkrTextureHandle handle = {.id = tex->description.id,
                               .generation = tex->description.generation};
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

bool8_t application_on_event(Event *event, UserData user_data) {
  // log_debug("Application on event: %d", event->type);
  return true_v;
}

bool8_t application_on_window_event(Event *event, UserData user_data) {
  // log_debug("Application on window event: %d", event->type);
  return true_v;
}

bool8_t application_on_key_event(Event *event, UserData user_data) {
  return true_v;
}

bool8_t application_on_mouse_event(Event *event, UserData user_data) {
  // log_debug("Application on mouse event: %d", event->type);
  return true_v;
}

vkr_internal bool8_t
application_try_activate_scene_resource(Application *application) {
  if (!application || !state ||
      state->scene_resource.type != VKR_RESOURCE_TYPE_SCENE) {
    return false_v;
  }

  String8 scene_path = string8_lit(SCENE_PATH);
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
          application_consume_scene_load_elapsed_seconds(application);
      if (elapsed >= 0.0) {
        log_info("SCENE_LOAD_TIME result=failed seconds=%.3f ms=%.1f", elapsed,
                 elapsed * 1000.0);
      }
      log_error("Scene load failed for '%s': %s", string8_cstr(&scene_path),
                string8_cstr(&err));
      state->scene_load_terminal_logged = true_v;
    }
    return false_v;
  case VKR_RESOURCE_LOAD_STATE_CANCELED:
    if (!state->scene_load_terminal_logged) {
      float64_t elapsed =
          application_consume_scene_load_elapsed_seconds(application);
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
  if (application->renderer.active_scene != scene) {
    application->renderer.active_scene = scene;
    application->renderer.scene_generation =
        application->renderer.scene_generation == UINT64_MAX
            ? 1u
            : application->renderer.scene_generation + 1u;
    /* A fit from the previous scene is framed by a camera and caster set that
       no longer exist, so it is not a previous value of the same quantity. The
       configuration stamps cannot catch this: they are all identical across a
       scene swap. */
    vkr_shadow_system_invalidate_fit_history(
        &application->renderer.shadow_system);
    float64_t elapsed =
        application_consume_scene_load_elapsed_seconds(application);
    if (elapsed >= 0.0) {
      vkr_renderer_metrics_set_scene_boot_ns(
          &application->renderer_metrics, (uint64_t)(elapsed * 1000000000.0));
      log_info("SCENE_LOAD_TIME result=ready seconds=%.3f ms=%.1f", elapsed,
               elapsed * 1000.0);
    }
    log_info("Activated scene '%s' after async load",
             string8_cstr(&scene_path));
    application_log_backend_allocator_stats(
        application, "load-ready",
        state->scene_load_stats_baseline_valid
            ? &state->scene_load_stats_baseline
            : NULL);
    state->scene_load_stats_baseline_valid = false_v;
    // Captured at the same point as the allocator breakdown so the two can be
    // read together: one says how many bytes, the other how many allocations.
    application_log_device_memory_stats(application, "load-ready");
  }

  return true_v;
}

/**
 * @brief Initialize scene system and load scene content.
 */
vkr_internal void application_init_scene_system(Application *application) {
  if (!application || !state) {
    return;
  }

  String8 scene_path = string8_lit(SCENE_PATH);
  if (state->scene_resource.type == VKR_RESOURCE_TYPE_SCENE) {
    if (application_try_activate_scene_resource(application)) {
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

  if (application->renderer.active_scene != NULL) {
    return;
  }

  state->scene_resource = (VkrResourceHandleInfo){0};
  state->scene_load_terminal_logged = false_v;
  state->scene_load_timer_active = false_v;
  state->scene_load_start_time_seconds = 0.0;

  VkrAllocatorScope load_scope =
      vkr_allocator_begin_scope(&application->renderer.scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&load_scope)) {
    log_error("Failed to create scene load scratch scope");
    return;
  }

  VkrRendererError load_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, scene_path,
                                &application->renderer.scratch_allocator,
                                &state->scene_resource, &load_err)) {
    String8 err_str = vkr_renderer_get_error_string(load_err);
    log_error("Failed to load scene '%s': %s", string8_cstr(&scene_path),
              string8_cstr(&err_str));
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return;
  }
  state->scene_load_stats_baseline_valid =
      application_capture_backend_allocator_stats(
          application, &state->scene_load_stats_baseline);
  state->scene_load_timer_active = true_v;
  state->scene_load_start_time_seconds = application->clock.elapsed;
  application_log_backend_allocator_stats(application, "load-enqueue", NULL);

  vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  // An async load is not ready the instant it is enqueued; activation is
  // retried from application_update_scene. Not an error -- it is the normal
  // path, and logging it as one made every capture look like a failure.
  if (!application_try_activate_scene_resource(application)) {
    log_debug("Scene load enqueued; activation deferred until it completes");
  }
}

vkr_internal void application_unload_scene_system(Application *application) {
  if (!application || !state) {
    return;
  }

  String8 scene_path = string8_lit(SCENE_PATH);
  if (state->scene_resource.type == VKR_RESOURCE_TYPE_SCENE ||
      state->scene_resource.request_id != 0 || state->scene_resource.as.scene) {
    vkr_resource_system_unload(&state->scene_resource, scene_path);
  }
  state->scene_resource = (VkrResourceHandleInfo){0};
  state->scene_load_terminal_logged = false_v;
  state->scene_load_stats_baseline_valid = false_v;
  state->scene_load_timer_active = false_v;
  state->scene_load_start_time_seconds = 0.0;
  application->renderer.active_scene = NULL;
  application->renderer.scene_generation =
      application->renderer.scene_generation == UINT64_MAX
          ? 1u
          : application->renderer.scene_generation + 1u;
  vkr_shadow_system_invalidate_fit_history(
      &application->renderer.shadow_system);
  application_log_backend_allocator_stats(application, "unload", NULL);
  application_log_device_memory_stats(application, "unload");
}

vkr_internal void application_init_memory_text(Application *application) {
  if (!application || !state) {
    return;
  }

  state->memory_text_id = VKR_INVALID_ID;

  state->memory_update_clock = vkr_clock_create();
  vkr_clock_start(&state->memory_update_clock);

  VkrUiTextConfig text_config = VKR_UI_TEXT_CONFIG_DEFAULT;
  text_config.font =
      application->renderer.font_system.default_system_font_handle;
#if defined(_WIN32)
  text_config.font_size = 18.0f;
#else
  VkrFont *font = vkr_font_system_get_default_system_font(
      &application->renderer.font_system);
  if (font) {
    text_config.font_size = (float32_t)font->size * 1.5f;
  }
#endif
  text_config.color = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};

  VkrUiTextCreateData payload = {
      .text_id = state->memory_text_id,
      .content = string8_lit("Memory metrics: pending"),
      .config = &text_config,
      .anchor = VKR_UI_TEXT_ANCHOR_BOTTOM_RIGHT,
      .padding = (Vec2){10.0f, 10.0f},
  };

  uint32_t text_id = VKR_INVALID_ID;
  if (!vkr_renderer_create_ui_text(&application->renderer, &payload,
                                   &text_id) ||
      text_id == VKR_INVALID_ID) {
    log_error("Failed to create memory UI text");
    return;
  }
  state->memory_text_id = text_id;
}

vkr_internal void application_update_memory_text(Application *application) {
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
      application_memory_text_append(&write, &remaining,
                                     "CPU allocator (tracked live)\n") &&
      application_memory_text_append_size(&write, &remaining, "  Total",
                                          cpu.total_allocated) &&
      application_memory_text_append_size(
          &write, &remaining, "  Renderer",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_RENDERER]) &&
      application_memory_text_append_size(
          &write, &remaining, "  Array",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]) &&
      application_memory_text_append_size(
          &write, &remaining, "  String",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]) &&
      application_memory_text_append_size(
          &write, &remaining, "  File",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_FILE]) &&
      application_memory_text_append_size(
          &write, &remaining, "  Vulkan state",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_VULKAN]) &&
      application_memory_text_append_size(
          &write, &remaining, "  Texture temp",
          cpu.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_TEXTURE]) &&
      application_memory_text_append_size(&write, &remaining, "  Other",
                                          cpu_other) &&
      application_memory_text_append(&write, &remaining,
                                     "\nGPU device memory\n");

  VkrDeviceMemoryStats gpu = {0};
  if (complete &&
      vkr_renderer_get_device_memory_stats(&application->renderer, &gpu)) {
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
        application_memory_text_append_size(&write, &remaining, "  Committed",
                                            gpu.live_bytes) &&
        application_memory_text_append_size(
            &write, &remaining, "  Committed peak", gpu.peak_bytes) &&
        application_memory_text_append_size(&write, &remaining,
                                            "  Logical live", logical_live) &&
        application_memory_text_append_size(
            &write, &remaining, "  Mesh",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_MESH].live_bytes) &&
        application_memory_text_append_size(
            &write, &remaining, "  Texture",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes) &&
        application_memory_text_append_size(
            &write, &remaining, "  Font",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_FONT].live_bytes) &&
        application_memory_text_append_size(
            &write, &remaining, "  Render graph",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH].live_bytes) &&
        application_memory_text_append_size(&write, &remaining, "  Frame data",
                                            frame_data) &&
        application_memory_text_append_size(&write, &remaining, "  Transfer",
                                            transfer) &&
        application_memory_text_append_size(&write, &remaining,
                                            "  Shader/target", shader_target) &&
        application_memory_text_append_size(
            &write, &remaining, "  Unknown",
            gpu.owners[VKR_GPU_ALLOCATION_OWNER_UNKNOWN].live_bytes);
  } else if (complete) {
    complete = application_memory_text_append(&write, &remaining,
                                              "  Metrics unavailable\n");
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
  application_queue_ui_text_update(
      application, state->memory_text_id,
      (String8){.str = content, .length = content_length});
}

/** Logs a paste-ready static camera block for a harness snapshot case. */
vkr_internal void application_log_camera_snapshot(Application *application) {
  if (!application) {
    log_warn("Cannot capture camera snapshot without an application");
    return;
  }

  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                        application->renderer.active_camera);
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
           SCENE_PATH, camera->cached_window_width,
           camera->cached_window_height, camera->position.x, camera->position.y,
           camera->position.z, camera->yaw, camera->pitch, camera->zoom,
           camera->near_clip, camera->far_clip);
}

vkr_internal void application_handle_input(Application *application,
                                           float64_t delta_time) {
  if (state == NULL || state->input_state == NULL) {
    log_error("State or input state is NULL");
    return;
  }

  (void)delta_time;

  InputState *input_state = state->input_state;

  if (input_is_key_up(state->input_state, KEY_M) &&
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

  if (input_is_key_up(state->input_state, KEY_L) &&
      input_was_key_down(state->input_state, KEY_L)) {
    application_init_scene_system(application);
  }

  if (input_is_key_up(state->input_state, KEY_U) &&
      input_was_key_down(state->input_state, KEY_U)) {
    application_unload_scene_system(application);
  }

  if (input_is_key_up(input_state, KEY_F4) &&
      input_was_key_down(input_state, KEY_F4)) {
    uint32_t next_mode =
        (state->filter_mode_index + (uint32_t)ArrayCount(FILTER_MODES) - 1) %
        (uint32_t)ArrayCount(FILTER_MODES);
    application_apply_filter_mode(application, next_mode);
  }

  if (input_is_key_up(input_state, KEY_F5) &&
      input_was_key_down(input_state, KEY_F5)) {
    uint32_t next_mode =
        (state->filter_mode_index + 1) % (uint32_t)ArrayCount(FILTER_MODES);
    application_apply_filter_mode(application, next_mode);
  }

  if (input_is_key_up(input_state, KEY_F6) &&
      input_was_key_down(input_state, KEY_F6)) {
    application->editor_viewport.enabled =
        !application->editor_viewport.enabled;
  }

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
             application_ibl_validation_mode_label(state->ibl_validation_mode));
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
    application_log_camera_snapshot(application);
  }

  if (input_is_key_down(input_state, KEY_TAB) &&
      input_was_key_up(input_state, KEY_TAB)) {
    bool8_t should_capture =
        !vkr_window_is_mouse_captured(&application->window);
    vkr_window_set_mouse_capture(&application->window, should_capture);
    if (!should_capture) {
      state->free_camera_wheel_initialized = false_v;
      state->free_camera_use_gamepad = false_v;
    }
  }

  if (input_is_button_down(input_state, BUTTON_GAMEPAD_A) &&
      input_was_button_up(input_state, BUTTON_GAMEPAD_A)) {
    bool8_t should_capture =
        !vkr_window_is_mouse_captured(&application->window);
    vkr_window_set_mouse_capture(&application->window, should_capture);
    if (should_capture) {
      state->free_camera_use_gamepad = !state->free_camera_use_gamepad;
    } else {
      state->free_camera_use_gamepad = false_v;
    }
  }

  if (!vkr_window_is_mouse_captured(&application->window)) {
    return;
  }

  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                        application->renderer.active_camera);
  if (!camera) {
    return;
  }

  VkrCameraController *controller = &application->renderer.camera_controller;
  controller->camera = camera;

  if (!state->free_camera_wheel_initialized) {
    int8_t wheel_delta = 0;
    input_get_mouse_wheel(input_state, &wheel_delta);
    state->free_camera_prev_wheel_delta = wheel_delta;
    state->free_camera_wheel_initialized = true_v;
  }

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

    int8_t wheel_delta = 0;
    input_get_mouse_wheel(input_state, &wheel_delta);
    if (wheel_delta != state->free_camera_prev_wheel_delta) {
      float32_t zoom_delta = -(float32_t)wheel_delta * 0.1f;
      vkr_camera_zoom(camera, zoom_delta);
      state->free_camera_prev_wheel_delta = wheel_delta;
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

vkr_internal void application_update_fps_text(Application *application,
                                              float64_t delta_time) {
  if (!application || !state) {
    return;
  }

  state->fps_accumulated_time += delta_time;
  state->fps_frame_count++;
  application_accumulate_frame_time(application);

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

    VkrCamera *camera =
        vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                          application->renderer.active_camera);

    VkrAllocator *frame_alloc = &application->renderer.scratch_allocator;

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
      application_queue_ui_text_update(application, state->fps_text_id,
                                       fps_text);

      String8 left_text = string8_create_formatted(
          frame_alloc,
          "Camera: {x: %.2f, y: %.2f, z: %.2f}\nCamera rotation: {yaw: %.2f, "
          "pitch: %.2f}\nPress Tab for free mode\nIBL mode: %s (x%.2f)\n"
          "F8 cycle mode, F9/F10 intensity\nG log camera snapshot",
          camera->position.x, camera->position.y, camera->position.z,
          camera->yaw, camera->pitch,
          application_ibl_validation_mode_label(state->ibl_validation_mode),
          state->ibl_validation_scalar);
      if (left_text.length > 0) {
        application_queue_ui_text_update(application, state->left_text_id,
                                         left_text);
      }

      if (state->metrics_text_id != VKR_INVALID_ID) {
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
          application_queue_ui_text_update(application, state->metrics_text_id,
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

vkr_internal void application_init_ui_texts(Application *application) {
  if (!application || !state) {
    return;
  }

  state->fps_text_id = VKR_INVALID_ID;
  state->left_text_id = VKR_INVALID_ID;
  state->metrics_text_id = VKR_INVALID_ID;

  VkrUiTextConfig text_config = VKR_UI_TEXT_CONFIG_DEFAULT;
  text_config.font =
      application->renderer.font_system.default_system_font_handle;
#if defined(_WIN32)
  text_config.font_size = 18.0f;
#else
  VkrFont *font = vkr_font_system_get_default_system_font(
      &application->renderer.font_system);
  if (font) {
    text_config.font_size = (float32_t)font->size * 2.0f;
  }
#endif
  text_config.color = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};

  VkrUiTextCreateData fps_payload = {
      .text_id = VKR_INVALID_ID,
      .content = string8_lit("FPS: 0.0\nFrametime: 0.0"),
      .config = &text_config,
      .anchor = VKR_UI_TEXT_ANCHOR_TOP_RIGHT,
      .padding = vec2_new(VKR_UI_TEXT_PADDING, VKR_UI_TEXT_PADDING),
  };

  uint32_t text_id = VKR_INVALID_ID;
  if (!vkr_renderer_create_ui_text(&application->renderer, &fps_payload,
                                   &text_id) ||
      text_id == VKR_INVALID_ID) {
    log_error("Failed to create FPS UI text");
    return;
  }
  state->fps_text_id = text_id;

  VkrUiTextCreateData left_payload = {
      .text_id = VKR_INVALID_ID,
      .content = string8_lit(
          "Camera: {x: 0.0, y: 0.0, z: 0.0}\nCamera rotation: {yaw: 0.0, "
          "pitch: 0.0, roll: 0.0}\nPress Tab for free mode\nIBL mode: "
          "Scene IBL (x1.00)\nF8 cycle mode, F9/F10 intensity"),
      .config = &text_config,
      .anchor = VKR_UI_TEXT_ANCHOR_TOP_LEFT,
      .padding = vec2_new(VKR_UI_TEXT_PADDING, VKR_UI_TEXT_PADDING),
  };

  text_id = VKR_INVALID_ID;
  if (!vkr_renderer_create_ui_text(&application->renderer, &left_payload,
                                   &text_id) ||
      text_id == VKR_INVALID_ID) {
    log_error("Failed to create left UI text");
    return;
  }
  state->left_text_id = text_id;

  state->fps_update_clock = vkr_clock_create();
  vkr_clock_start(&state->fps_update_clock);
  state->fps_accumulated_time = 0.0;
  state->fps_frame_count = 0;
  state->current_fps = 0.0;
  state->current_frametime = 0.0;

  // Create picked object text (bottom-left corner)
  VkrUiTextCreateData picked_payload = {
      .text_id = VKR_INVALID_ID,
      .content = string8_lit("Picked: none"),
      .config = &text_config,
      .anchor = VKR_UI_TEXT_ANCHOR_BOTTOM_LEFT,
      .padding = vec2_new(VKR_UI_TEXT_PADDING, VKR_UI_TEXT_PADDING),
  };

  text_id = VKR_INVALID_ID;
  if (!vkr_renderer_create_ui_text(&application->renderer, &picked_payload,
                                   &text_id) ||
      text_id == VKR_INVALID_ID) {
    log_error("Failed to create picked object UI text");
    return;
  }
  state->picked_object_text_id = text_id;
  state->last_picked_object_id = 0;

  float32_t metrics_padding_y =
      VKR_UI_TEXT_PADDING + text_config.font_size * 2.5f;
  VkrUiTextCreateData metrics_payload = {
      .text_id = VKR_INVALID_ID,
      .content = string8_lit("World batches: 0\nShadow: 0"),
      .config = &text_config,
      .anchor = VKR_UI_TEXT_ANCHOR_BOTTOM_LEFT,
      .padding = vec2_new(VKR_UI_TEXT_PADDING, metrics_padding_y),
  };

  text_id = VKR_INVALID_ID;
  if (!vkr_renderer_create_ui_text(&application->renderer, &metrics_payload,
                                   &text_id) ||
      text_id == VKR_INVALID_ID) {
    log_error("Failed to create metrics UI text");
    return;
  }
  state->metrics_text_id = text_id;

  application_init_memory_text(application);
}

/**
 * @brief Initialize world content: load fonts needed by scene text3d.
 */
vkr_internal void application_init_world_content(Application *application) {
  if (!application || !state) {
    return;
  }

  // Load the 3D font used by scene text entities
  String8 text_font_name = string8_lit("UbuntuMono-3d");
  String8 text_font_cfg = string8_lit("assets/fonts/UbuntuMono-3d.fontcfg");
  VkrRendererError font_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_font_system_load_from_file(&application->renderer.font_system,
                                      text_font_name, text_font_cfg,
                                      &font_err)) {
    String8 err = vkr_renderer_get_error_string(font_err);
    log_error("Failed to load 3D font: %s", string8_cstr(&err));
  }

  // Initialize the world text update clock (for scene text updates)
  state->world_text_update_clock = vkr_clock_create();
  vkr_clock_start(&state->world_text_update_clock);
}

/**
 * @brief Update scene system each frame.
 */
vkr_internal void application_update_scene(Application *application,
                                           float64_t delta_time) {
  if (!application || !state) {
    return;
  }

  (void)application_try_activate_scene_resource(application);
  if (!state->scene_resource.as.scene || !application->renderer.active_scene) {
    return;
  }

  VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_get_state(&state->scene_resource, &state_error) !=
      VKR_RESOURCE_LOAD_STATE_READY) {
    return;
  }

  vkr_scene_handle_update_and_sync(state->scene_resource.as.scene,
                                   &application->renderer, delta_time);

  if (application->renderer.gizmo_system.initialized) {
    if (!state->has_selection) {
      application_clear_gizmo_selection(application);
      return;
    }

    VkrScene *scene =
        vkr_scene_handle_get_scene(state->scene_resource.as.scene);
    SceneTransform *transform =
        scene ? vkr_scene_get_transform(scene, state->selected_entity) : NULL;
    if (!transform) {
      application_clear_gizmo_selection(application);
      return;
    }

    Vec3 world_position = mat4_position(transform->world);
    SceneText3D *text = vkr_scene_get_text3d(scene, state->selected_entity);
    if (text) {
      Vec3 pivot_local = vec3_zero();
      if (application_text_pivot_local(text, &pivot_local)) {
        world_position = application_text_pivot_world(transform, pivot_local);
      }
    }

    vkr_gizmo_system_set_target(&application->renderer.gizmo_system,
                                state->selected_entity, world_position,
                                vkr_quat_identity());
  }
}

vkr_internal void application_update_picking(Application *application) {
  if (!application || !state || !state->input_state) {
    return;
  }

  VkrPickingContext *picking = &application->renderer.picking;
  if (!picking->initialized) {
    return;
  }

  bool8_t left_down = input_is_button_down(state->input_state, BUTTON_LEFT);
  bool8_t right_down = input_is_button_down(state->input_state, BUTTON_RIGHT);
  bool8_t middle_down = input_is_button_down(state->input_state, BUTTON_MIDDLE);
  bool8_t left_pressed =
      left_down && input_was_button_up(state->input_state, BUTTON_LEFT);
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
      application_get_viewport_hit_info(application, mouse_x, mouse_y);

  if (state->gizmo_drag.active) {
    if (!left_down) {
      state->gizmo_drag.active = false_v;
      state->gizmo_drag.handle = VKR_GIZMO_HANDLE_NONE;
      application_clear_gizmo_handles(application);
    } else {
      application_update_gizmo_drag(application, &viewport_info);
    }
  }

  if (click_pressed && state->gizmo_hover_pending) {
    vkr_picking_cancel(picking);
    state->gizmo_hover_pending = false_v;
  }

  if (!state->gizmo_drag.active && !state->gizmo_drag.pending_pick &&
      click_pressed && !vkr_picking_is_pending(picking)) {
    if (application_request_picking(application, picking, &viewport_info)) {
      state->gizmo_drag.pending_pick = true_v;
      state->gizmo_drag.pending_select = click_select;
      state->gizmo_drag.pick_x = viewport_info.target_x;
      state->gizmo_drag.pick_y = viewport_info.target_y;
      state->gizmo_drag.pick_width = viewport_info.target_width;
      state->gizmo_drag.pick_height = viewport_info.target_height;
    }
  }

  bool8_t mouse_moved = (mouse_x != prev_mouse_x || mouse_y != prev_mouse_y);
  if (!state->gizmo_drag.active && !state->gizmo_drag.pending_pick &&
      !state->gizmo_hover_pending && !vkr_picking_is_pending(picking) &&
      mouse_moved && !left_down && !right_down && !middle_down &&
      application->renderer.gizmo_system.visible) {
    if (application_request_picking(application, picking, &viewport_info)) {
      state->gizmo_hover_pending = true_v;
    }
  }

  VkrPickResult result =
      vkr_picking_get_result(&application->renderer, picking);

  if (state->gizmo_drag.pending_pick && !vkr_picking_is_pending(picking)) {
    state->gizmo_drag.pending_pick = false_v;

    VkrAllocator *frame_alloc = &application->renderer.scratch_allocator;
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
        if (scene && application_world_text_entity_from_id(scene, decoded.value,
                                                           &text_entity)) {
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
        vkr_gizmo_system_set_hot_handle(&application->renderer.gizmo_system,
                                        handle);
        bool8_t drag_button_down =
            input_is_button_down(state->input_state, BUTTON_LEFT);
        if (drag_button_down && handle != VKR_GIZMO_HANDLE_NONE) {
          if (application_begin_gizmo_drag(application, handle)) {
            vkr_gizmo_system_set_active_handle(
                &application->renderer.gizmo_system, handle);
            application->renderer.gizmo_system.mode = state->gizmo_drag.mode;
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
      application_queue_ui_text_update(
          application, state->picked_object_text_id, picked_text);
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
    vkr_gizmo_system_set_hot_handle(&application->renderer.gizmo_system,
                                    hot_handle);
  }
}

/**
 * @brief Update scene text3d entities (e.g., the WorldClock).
 *
 * Uses the new scene-based text3d API instead of layer messages.
 */
vkr_internal void application_update_world_text(Application *application) {
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

vkr_internal void application_poll_upload_wait_stats(Application *application) {
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
    application_close(application);
  }
}

void application_update(Application *application, float64_t delta) {
  application_handle_input(application, delta);

  if (input_is_key_up(state->input_state, KEY_Q) &&
      input_was_key_down(state->input_state, KEY_Q)) {
    application->renderer.globals.render_mode =
        (VkrRenderMode)(((uint32_t)application->renderer.globals.render_mode +
                         1) %
                        VKR_RENDER_MODE_COUNT);
    log_debug("RENDER MODE: %d", application->renderer.globals.render_mode);
  }

  if (input_is_key_up(state->input_state, KEY_E) &&
      input_was_key_down(state->input_state, KEY_E)) {
    application->renderer.shadow_debug_mode =
        (application->renderer.shadow_debug_mode + 1u) % 14u;
    log_debug("SHADOW DEBUG MODE: %u "
              "(0=off,1=cascades,2=factor,3=depth,4=map0,5=map1,6=map2,7=map3,"
              "8=map4,9=map5,10=map6,11=map7,12=frustum,13=camera)",
              application->renderer.shadow_debug_mode);
  }

  application_update_fps_text(application, delta);
  application_update_memory_text(application);
  application_update_world_text(application);
  application_update_picking(application);
  application_update_scene(application, delta);
  application_update_ibl_validation_controls(application);
  application_poll_upload_wait_stats(application);
  application_dump_periodic_metrics(application);

  if (state->auto_close_enabled && !state->auto_close_requested &&
      application->clock.elapsed >= state->auto_close_after_seconds) {
    state->auto_close_requested = true_v;
    log_info("Auto-close threshold reached (%.2fs), shutting down app loop",
             state->auto_close_after_seconds);
    application_close(application);
  }
}

// todo: should look into using DLLs in debug builds for hot reload
// and static for release builds, also should look into how we can
// implement hot reload for the application in debug builds
int main(int argc, char **argv) {
  int exit_code = 0;
  const char *metrics_json_path = NULL;
#if defined(_WIN32)
  VkrRendererBackendType renderer_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
#else
  VkrRendererBackendType renderer_backend = VKR_RENDERER_BACKEND_TYPE_METAL;
#endif
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--metrics-json") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--metrics-json requires an output path\n");
        return 2;
      }
      metrics_json_path = argv[++i];
    } else if (strcmp(argv[i], "--renderer") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--renderer requires 'vulkan' or 'metal'\n");
        return 2;
      }
      const char *renderer_name = argv[++i];
      if (strcmp(renderer_name, "vulkan") == 0) {
        renderer_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
      } else if (strcmp(renderer_name, "metal") == 0) {
        renderer_backend = VKR_RENDERER_BACKEND_TYPE_METAL;
      } else {
        fprintf(stderr, "unknown renderer '%s'\n", renderer_name);
        return 2;
      }
    }
  }
  const bool8_t rg_gpu_timing_enabled =
      application_env_flag("VKR_RG_GPU_TIMING", false_v);
  const bool8_t submission_gpu_timing_enabled =
      application_env_flag("VKR_GPU_SUBMISSION_TIMING", false_v);
  const bool8_t metrics_event_subjects =
      application_env_flag("VKR_METRICS_EVENT_SUBJECTS", false_v);

  ApplicationConfig config = {0};
  config.title = "Hello, World!";
  config.x = 100;
  config.y = 100;
  config.width = 800;
  config.height = 600;
  config.app_arena_size = MB(1);
  config.target_frame_rate = 0;
  config.renderer_backend = renderer_backend;
  config.metrics_config = (VkrMetricsConfig){
      .pass_gpu_timings = rg_gpu_timing_enabled,
      .submission_gpu_timings = submission_gpu_timing_enabled,
      .event_subjects = metrics_event_subjects,
  };
  config.device_requirements = (VkrDeviceRequirements){
      .supported_stages =
          VKR_SHADER_STAGE_VERTEX_BIT | VKR_SHADER_STAGE_FRAGMENT_BIT,
      .supported_queues = VKR_DEVICE_QUEUE_GRAPHICS_BIT |
                          VKR_DEVICE_QUEUE_TRANSFER_BIT |
                          VKR_DEVICE_QUEUE_PRESENT_BIT,
      .allowed_device_types =
          VKR_DEVICE_TYPE_DISCRETE_BIT | VKR_DEVICE_TYPE_INTEGRATED_BIT,
      .supported_sampler_filters = VKR_SAMPLER_FILTER_ANISOTROPIC_BIT,
  };

  Application application = {0};
  if (!application_create(&application, &config)) {
    fprintf(stderr, "Application creation failed\n");
    return 1;
  }

  // Baseline before any scene loads: the renderer's own resident allocations.
  application_log_device_memory_stats(&application, "startup");

  state = arena_alloc(application.app_arena, sizeof(State),
                      ARENA_MEMORY_TAG_STRUCT);
  state->stats_arena = arena_create(KB(1), KB(1));
  VkrAllocator app_alloc = {.ctx = application.app_arena};
  vkr_allocator_arena(&app_alloc);
  state->input_state = &application.window.input_state;
  state->app_arena = application.app_arena;
  state->event_arena = application.event_manager.arena;
  state->event_manager = &application.event_manager;
  state->fps_text_id = VKR_INVALID_ID;
  state->left_text_id = VKR_INVALID_ID;
  state->metrics_text_id = VKR_INVALID_ID;
  state->fps_update_clock = vkr_clock_create();
  state->memory_update_clock = vkr_clock_create();
  state->fps_accumulated_time = 0.0;
  state->fps_frame_count = 0;
  state->current_fps = 0.0;
  state->current_frametime = 0.0;
  state->world_text_id = 0;
  state->world_text_update_clock = vkr_clock_create();
  state->free_camera_use_gamepad = false_v;
  state->free_camera_wheel_initialized = false_v;
  state->free_camera_prev_wheel_delta = 0;
  state->picked_object_text_id = VKR_INVALID_ID;
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
  state->gizmo_drag.pick_x = 0;
  state->gizmo_drag.pick_y = 0;
  state->gizmo_drag.pick_width = 0;
  state->gizmo_drag.pick_height = 0;
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

  const char *auto_close_env = getenv("VKR_AUTOCLOSE_SECONDS");
  if (auto_close_env && auto_close_env[0] != '\0') {
    char *end_ptr = NULL;
    float64_t auto_close_seconds = strtod(auto_close_env, &end_ptr);
    if (end_ptr != auto_close_env && auto_close_seconds > 0.0) {
      state->auto_close_enabled = true_v;
      state->auto_close_after_seconds = auto_close_seconds;
      log_info("Auto-close enabled via VKR_AUTOCLOSE_SECONDS=%.2f",
               auto_close_seconds);
    } else {
      log_warn("Ignoring invalid VKR_AUTOCLOSE_SECONDS value '%s'",
               auto_close_env);
    }
  }

  // Headless metrics capture. Both knobs are opt-in so interactive runs are
  // unchanged; together with VKR_AUTOCLOSE_SECONDS they make a baseline
  // reproducible without a human driving the app.
  if (application_env_flag("VKR_AUTOLOAD_SCENE", false_v)) {
    log_info("Auto-loading scene '%s' via VKR_AUTOLOAD_SCENE", SCENE_PATH);
    application_init_scene_system(&application);
  }

  const char *metrics_interval_env = getenv("VKR_METRICS_INTERVAL_SECONDS");
  if (metrics_interval_env && metrics_interval_env[0] != '\0') {
    char *metrics_end_ptr = NULL;
    float64_t metrics_interval = strtod(metrics_interval_env, &metrics_end_ptr);
    if (metrics_end_ptr != metrics_interval_env && metrics_interval > 0.0) {
      state->metrics_dump_enabled = true_v;
      state->metrics_dump_interval_seconds = metrics_interval;
      state->metrics_dump_clock = vkr_clock_create();
      vkr_clock_start(&state->metrics_dump_clock);
      log_info("Periodic metrics dump enabled every %.2fs via "
               "VKR_METRICS_INTERVAL_SECONDS",
               metrics_interval);
    } else {
      log_warn("Ignoring invalid VKR_METRICS_INTERVAL_SECONDS value '%s'",
               metrics_interval_env);
    }
  }

  // Already applied through config.metrics_config; this only reports it.
  if (application.metrics->config.pass_gpu_timings) {
    log_info("RenderGraph GPU timings enabled via VKR_RG_GPU_TIMING");
  }
  if (metrics_event_subjects) {
    log_info("Metrics event subjects enabled via VKR_METRICS_EVENT_SUBJECTS");
  }

  state->assert_no_upload_waits =
      application_env_flag("VKR_ASSERT_NO_UPLOAD_WAITS", false_v);
  if (state->assert_no_upload_waits) {
    log_info("Upload wait assertion enabled via VKR_ASSERT_NO_UPLOAD_WAITS");
  }

  state->scene_memory_verbose =
      application_env_flag("VKR_SCENE_MEM_VERBOSE", false_v);
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
  state->anisotropy_supported =
      bitset8_is_set(&state->device_information.sampler_filters,
                     VKR_SAMPLER_FILTER_ANISOTROPIC_BIT);
  state->filter_mode_index = 3; // Bilinear default (index in FILTER_MODES)

  log_info("Texture filtering controls: F4=prev, F5=next (start: %s)",
           FILTER_MODES[state->filter_mode_index].label);
  log_info("IBL validation controls: F8=mode, F9/F10=intensity "
           "(start: %s x%.2f)",
           application_ibl_validation_mode_label(state->ibl_validation_mode),
           state->ibl_validation_scalar);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  application_init_ui_texts(&application);
  application_init_world_content(&application);

  application_start(&application);
  application_close(&application);

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

  if (metrics_json_path) {
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

  String8 scene_path = string8_lit(SCENE_PATH);
  if (state->scene_resource.type == VKR_RESOURCE_TYPE_SCENE ||
      state->scene_resource.request_id != 0 || state->scene_resource.as.scene) {
    vkr_resource_system_unload(&state->scene_resource, scene_path);
  }
  state->scene_resource = (VkrResourceHandleInfo){0};

  arena_destroy(state->stats_arena);
  state->stats_arena = NULL;

  application_shutdown(&application);

  return exit_code;
}

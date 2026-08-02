/**
 * @file vkr_harness_child.c
 * @brief One isolated timed repetition: boot the renderer, freeze simulation
 *        until the scene is ready, drive the scripted camera on a fixed delta,
 *        and publish this repetition's raw samples and report.
 *
 * The parent never opens a renderer target; everything below runs only in a
 * `--child-profile` process.
 */
#include "vkr_harness_runtime.h"

#include "application.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_shadow_system.h"

typedef struct VkrHarnessChildContext {
  const VkrHarnessCase *case_manifest;
  const VkrHarnessArenas *arenas;
  VkrResourceHandleInfo scene_resource;
  float64_t load_started;
  /** Set once the scene is resident; simulation time is zero until then. */
  bool8_t phase_started;
  bool8_t failed;
  char failure[128];
  uint64_t last_publication;
  uint32_t completed_frames;
  uint32_t total_frames;
  uint32_t metric_count;
  uint32_t pass_count;
  uint64_t events_dropped;
  uint64_t event_subjects_truncated;
  uint64_t snapshot_publications_dropped;
  VkrHarnessSampleEvent *events;
  uint32_t event_count;
  uint64_t event_storage_dropped;
  VkrHarnessSamplePass *pass_catalog;
  float64_t *pass_cpu_samples;
  float64_t *pass_gpu_samples;
  uint8_t *pass_flags;
  float64_t *samples;
  uint8_t *availability;
  int32_t capture_index;
  const char *run_dir;
  VkrCaptureItemRequest capture_items[VKR_HARNESS_MAX_CAPTURE_CHANNELS];
  VkrCaptureBatchRequest capture_request;
  bool8_t capture_requested;
  bool8_t capture_complete;
  VkrHarnessReport *capture_report;
  VkrHarnessError capture_error;
} VkrHarnessChildContext;

/**
 * The application entry points below are plain callbacks with no user data, so
 * the in-flight repetition is reached through file scope. Exactly one exists
 * per process.
 */
static VkrHarnessChildContext *g_harness_child;

bool8_t application_on_event(Event *event, UserData user_data) {
  (void)event;
  (void)user_data;
  return true_v;
}

bool8_t application_on_window_event(Event *event, UserData user_data) {
  (void)event;
  (void)user_data;
  return true_v;
}

bool8_t application_on_key_event(Event *event, UserData user_data) {
  (void)event;
  (void)user_data;
  return true_v;
}

bool8_t application_on_mouse_event(Event *event, UserData user_data) {
  (void)event;
  (void)user_data;
  return true_v;
}

static const char *vkr_harness_metric_unit_name(VkrMetricUnit unit) {
  static const char *const names[] = {"count", "bytes",   "ns",
                                      "ratio", "percent", "count_per_second"};
  return unit < ArrayCount(names) ? names[unit] : "unknown";
}

static VkrHarnessPresentMode
vkr_harness_present_from_renderer(VkrPresentMode mode) {
  switch (mode) {
  case VKR_PRESENT_MODE_IMMEDIATE:
    return VKR_HARNESS_PRESENT_IMMEDIATE;
  case VKR_PRESENT_MODE_FIFO:
    return VKR_HARNESS_PRESENT_FIFO;
  case VKR_PRESENT_MODE_MAILBOX:
    return VKR_HARNESS_PRESENT_MAILBOX;
  default:
    return VKR_HARNESS_PRESENT_NONE;
  }
}

static VkrPresentMode
vkr_harness_present_to_renderer(VkrHarnessPresentMode mode) {
  return mode == VKR_HARNESS_PRESENT_IMMEDIATE ? VKR_PRESENT_MODE_IMMEDIATE
                                               : VKR_PRESENT_MODE_FIFO;
}

static const char *
vkr_harness_surface_format_name(VkrSurfaceColorFormat format) {
  static const char *const names[] = {"unknown", "bgra8_srgb", "rgba8_srgb",
                                      "bgra8_unorm", "rgba8_unorm"};
  return format < ArrayCount(names) ? names[format] : "unknown";
}

static const char *vkr_harness_color_space_name(VkrSurfaceColorSpace space) {
  return space == VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR ? "srgb_nonlinear"
                                                         : "unknown";
}

static void vkr_harness_child_fail(Application *application,
                                   const char *reason) {
  VkrHarnessChildContext *child = g_harness_child;
  if (!child->failed) {
    child->failed = true_v;
    string_format(child->failure, sizeof(child->failure), "%s", reason);
  }
  application_close(application);
}

/**
 * Copies one newly published metrics frame into this repetition's sample
 * arrays. A repeated publication serial means the renderer produced no frame
 * since the last update, so no sample is recorded and the case-frame index
 * does not advance.
 */
static void vkr_harness_child_sample(Application *application) {
  VkrHarnessChildContext *child = g_harness_child;
  VkrMetricsSnapshotView view = {0};
  if (!vkr_metrics_snapshot_acquire(application->metrics, &view)) {
    return;
  }
  if (view.publication_serial == child->last_publication) {
    vkr_metrics_snapshot_release(application->metrics, &view);
    return;
  }
  child->last_publication = view.publication_serial;
  if (application->last_renderer_error == VKR_RENDERER_ERROR_CAPTURE_BUSY) {
    vkr_metrics_snapshot_release(application->metrics, &view);
    return;
  }
  child->events_dropped = view.frame->events_dropped;
  child->event_subjects_truncated = view.frame->event_subjects_truncated;
  child->snapshot_publications_dropped =
      view.frame->snapshot_publications_dropped;
  if (!child->phase_started || child->completed_frames >= child->total_frames) {
    vkr_metrics_snapshot_release(application->metrics, &view);
    return;
  }
  const uint32_t frame = child->completed_frames++;
  for (uint32_t metric = 0; metric < child->metric_count; ++metric) {
    const VkrMetricSample *sample = &view.frame->samples[metric];
    const uint64_t offset = (uint64_t)frame * child->metric_count + metric;
    child->availability[offset] = (uint8_t)sample->availability;
    if (sample->availability == VKR_METRIC_AVAILABILITY_UNAVAILABLE) {
      child->samples[offset] = 0.0;
    } else if (sample->kind == VKR_METRIC_KIND_DURATION) {
      child->samples[offset] = sample->value.duration.count > 0u
                                   ? (float64_t)sample->value.duration.sum_ns /
                                         (float64_t)sample->value.duration.count
                                   : 0.0;
    } else if (sample->scalar == VKR_METRIC_SCALAR_F64) {
      child->samples[offset] = sample->value.f64;
    } else {
      child->samples[offset] = (float64_t)sample->value.u64;
    }
  }
  /* The pass table is single-buffered and owned by the collecting thread, so
     it only describes this snapshot when both name the same CPU frame. A row
     that fails any check stays flagged invalid rather than contributing a
     zero millisecond sample. */
  const VkrRendererMetricsPassTable *passes =
      vkr_renderer_metrics_get_pass_table(&application->renderer_metrics);
  if (passes && passes->cpu_frame_index == view.frame->cpu_frame_index &&
      passes->count == child->pass_count) {
    for (uint32_t pass = 0; pass < child->pass_count; ++pass) {
      const VkrRendererMetricsPassSample *source = &passes->samples[pass];
      const uint64_t offset = (uint64_t)frame * child->pass_count + pass;
      /* Culled and disabled describe the catalog row only when the source row
         is that row; inherited from a reordered table they would claim this
         pass was legitimately skipped and satisfy the completeness gate on
         another pass's evidence. */
      const bool8_t matched =
          string_equals(source->name, child->pass_catalog[pass].name);
      const bool8_t executed = matched && !source->culled && !source->disabled;
      child->pass_cpu_samples[offset] = source->cpu_ms;
      child->pass_gpu_samples[offset] = source->gpu_ms;
      child->pass_flags[offset] =
          (uint8_t)((executed ? VKR_HARNESS_PASS_FLAG_CPU_VALID : 0u) |
                    (executed && source->gpu_valid
                         ? VKR_HARNESS_PASS_FLAG_GPU_VALID
                         : 0u) |
                    (matched && source->culled ? VKR_HARNESS_PASS_FLAG_CULLED
                                               : 0u) |
                    (matched && source->disabled
                         ? VKR_HARNESS_PASS_FLAG_DISABLED
                         : 0u));
    }
  }
  vkr_metrics_snapshot_release(application->metrics, &view);
}

static void vkr_harness_child_drain_events(Application *application) {
  VkrHarnessChildContext *child = g_harness_child;
  uint32_t catalog_count = 0;
  const VkrMetricCatalogEntry *catalog =
      vkr_metrics_get_catalog(application->metrics, &catalog_count);
  VkrMetricEvent event = {0};
  while (vkr_metrics_event_pop(application->metrics, &event)) {
    if (child->event_count >= VKR_HARNESS_MAX_EVENTS) {
      child->event_storage_dropped++;
      continue;
    }
    VkrHarnessSampleEvent *sample = &child->events[child->event_count++];
    const uint32_t source_index = vkr_metric_id_index(event.source);
    string_format(sample->source, sizeof(sample->source), "%s",
                  catalog && source_index < catalog_count
                      ? catalog[source_index].name
                      : "unknown");
    MemCopy(sample->subject, event.subject, event.subject_length);
    sample->subject[event.subject_length] = '\0';
    sample->start_ns = event.start_ns;
    sample->duration_ns = event.duration_ns;
    sample->bytes = event.bytes;
    sample->thread_id = event.thread_id;
    sample->status = event.status;
    sample->subject_truncated = event.subject_truncated;
  }
}

/**
 * Determinism rule 1: nothing is measured until the requested scene reaches a
 * successful terminal state. Sizing the pass storage here rather than during
 * the measured window keeps execution allocation-free.
 */
static bool8_t vkr_harness_child_activate_scene(Application *application) {
  VkrHarnessChildContext *child = g_harness_child;
  VkrRendererError resource_error = VKR_RENDERER_ERROR_NONE;
  const VkrResourceLoadState load_state =
      vkr_resource_system_get_state(&child->scene_resource, &resource_error);
  if (load_state == VKR_RESOURCE_LOAD_STATE_FAILED ||
      load_state == VKR_RESOURCE_LOAD_STATE_CANCELED) {
    vkr_harness_child_fail(application, "scene.load_failed");
    return false_v;
  }
  if (load_state != VKR_RESOURCE_LOAD_STATE_READY) {
    const float64_t elapsed =
        vkr_platform_get_absolute_time() - child->load_started;
    if (elapsed * 1000.0 > child->case_manifest->asset_ready_timeout_ms) {
      vkr_harness_child_fail(application, "scene.asset_ready_timeout");
    }
    return false_v;
  }
  /* `boot.scene` closes the moment the requested closure reaches READY, which
     is still before the first measured frame. */
  vkr_renderer_metrics_set_scene_boot_ns(
      &application->renderer_metrics,
      vkr_metrics_elapsed_ns(child->load_started));
  if (!child->scene_resource.as.scene) {
    VkrResourceHandleInfo resolved = {0};
    if (!vkr_resource_system_try_get_resolved(&child->scene_resource,
                                              &resolved) ||
        !resolved.as.scene) {
      vkr_harness_child_fail(application, "scene.resolve_failed");
      return false_v;
    }
    child->scene_resource = resolved;
  }
  application->renderer.active_scene =
      vkr_scene_handle_get_scene(child->scene_resource.as.scene);
  if (!application->renderer.active_scene) {
    vkr_harness_child_fail(application, "scene.null");
    return false_v;
  }
  vkr_scene_handle_full_sync(child->scene_resource.as.scene,
                             &application->renderer);
  const VkrRendererMetricsPassTable *passes =
      vkr_renderer_metrics_get_pass_table(&application->renderer_metrics);
  if (!passes || passes->count == 0u || passes->truncated) {
    vkr_harness_child_fail(application, "passes.unavailable");
    return false_v;
  }
  child->pass_count = passes->count;
  const uint64_t pass_value_count =
      (uint64_t)child->total_frames * child->pass_count;
  Arena *arena = child->arenas->persistent;
  child->pass_catalog =
      arena_alloc(arena, child->pass_count * sizeof(*child->pass_catalog),
                  ARENA_MEMORY_TAG_STRUCT);
  child->pass_cpu_samples =
      arena_alloc(arena, pass_value_count * sizeof(*child->pass_cpu_samples),
                  ARENA_MEMORY_TAG_ARRAY);
  child->pass_gpu_samples =
      arena_alloc(arena, pass_value_count * sizeof(*child->pass_gpu_samples),
                  ARENA_MEMORY_TAG_ARRAY);
  child->pass_flags =
      arena_alloc(arena, pass_value_count, ARENA_MEMORY_TAG_ARRAY);
  if (!child->pass_catalog || !child->pass_cpu_samples ||
      !child->pass_gpu_samples || !child->pass_flags) {
    vkr_harness_child_fail(application, "passes.allocation_failed");
    return false_v;
  }
  /* Arenas bump rather than zero: a frame whose pass row is never written must
     still read back as an invalid sample, not as stale bytes. */
  MemZero(child->pass_catalog,
          child->pass_count * sizeof(*child->pass_catalog));
  MemZero(child->pass_cpu_samples,
          pass_value_count * sizeof(*child->pass_cpu_samples));
  MemZero(child->pass_gpu_samples,
          pass_value_count * sizeof(*child->pass_gpu_samples));
  MemZero(child->pass_flags, pass_value_count);
  for (uint32_t pass = 0; pass < child->pass_count; ++pass) {
    string_format(child->pass_catalog[pass].name,
                  sizeof(child->pass_catalog[pass].name), "%s",
                  passes->samples[pass].name);
  }
  child->phase_started = true_v;
  return true_v;
}

void application_update(Application *application, float64_t delta) {
  VkrHarnessChildContext *child = g_harness_child;
  if (!child) {
    application_close(application);
    return;
  }
  vkr_harness_child_drain_events(application);
  vkr_harness_child_sample(application);
  if (child->capture_requested && !child->capture_complete) {
    VkrCapturePollResult poll = {0};
    VkrCaptureStatus status = vkr_renderer_capture_poll(
        &application->renderer, child->capture_request.request_id, &poll);
    if (status == VKR_CAPTURE_STATUS_READY) {
      if (!vkr_harness_capture_publish(
              child->run_dir,
              child->case_manifest->captures[child->capture_index].at_frame,
              &poll, child->arenas, child->capture_report,
              &child->capture_error)) {
        vkr_renderer_capture_release(&application->renderer,
                                     child->capture_request.request_id);
        vkr_harness_child_fail(application, "capture.publish_failed");
        return;
      }
      vkr_renderer_capture_release(&application->renderer,
                                   child->capture_request.request_id);
      child->capture_complete = true_v;
    } else if (status == VKR_CAPTURE_STATUS_FAILED) {
      vkr_renderer_capture_release(&application->renderer,
                                   child->capture_request.request_id);
      vkr_harness_child_fail(application, "capture.failed");
      return;
    } else if (status == VKR_CAPTURE_STATUS_NOT_FOUND &&
               application->capture_request == NULL &&
               application->last_renderer_error != VKR_RENDERER_ERROR_NONE &&
               application->last_renderer_error !=
                   VKR_RENDERER_ERROR_CAPTURE_BUSY) {
      vkr_harness_child_fail(application, "capture.request_rejected");
      return;
    }
  }
  if (child->completed_frames >= child->total_frames &&
      (child->capture_index < 0 || child->capture_complete)) {
    application_close(application);
    return;
  }
  vkr_resource_system_pump(NULL);
  if (!child->phase_started && !vkr_harness_child_activate_scene(application)) {
    return;
  }
  if (child->capture_index >= 0 && !child->capture_requested &&
      child->completed_frames ==
          child->case_manifest->warmup_frames +
              child->case_manifest->captures[child->capture_index].at_frame) {
    application->capture_request = &child->capture_request;
    child->capture_requested = true_v;
  }
  vkr_scene_handle_update_and_sync(child->scene_resource.as.scene,
                                   &application->renderer, delta);
  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                        application->renderer.active_camera);
  /* Determinism rule 2: the pose is a function of the case-frame index and the
     fixed delta, never of wall-clock time or input. */
  VkrHarnessCameraPose pose = {0};
  if (!camera || !vkr_harness_camera_evaluate(
                     &child->case_manifest->camera,
                     (float64_t)child->completed_frames *
                         child->case_manifest->fixed_delta_seconds,
                     &pose)) {
    vkr_harness_child_fail(application, "camera.evaluate_failed");
    return;
  }
  vkr_camera_set_pose(camera, pose.position, pose.yaw_degrees,
                      pose.pitch_degrees);
}

#if VKR_METRICS_ENABLED

static bool8_t vkr_harness_catalog_has(const VkrMetrics *metrics,
                                       const char *name) {
  uint32_t count = 0;
  const VkrMetricCatalogEntry *catalog =
      vkr_metrics_get_catalog(metrics, &count);
  for (uint32_t i = 0; i < count; ++i) {
    if (string_equals(catalog[i].name, name)) {
      return true_v;
    }
  }
  return false_v;
}

/**
 * Determinism rule 4: warmup is a gate, not a settling period. It requires no
 * required pipeline creation during warmup and bounded drift between the two
 * halves of the profile's stability window. The window is never extended,
 * because that would shift every later simulation pose.
 */
static bool8_t vkr_harness_warmup_stable(const VkrHarnessCase *case_manifest,
                                         const VkrHarnessProfile *profile,
                                         uint32_t metric_count,
                                         const VkrHarnessSampleMetric *catalog,
                                         const float64_t *samples,
                                         const uint8_t *availability) {
  int32_t submit_metric = -1;
  int32_t pipeline_metric = -1;
  for (uint32_t i = 0; i < metric_count; ++i) {
    if (string_equals(catalog[i].name, "cpu.render_submit")) {
      submit_metric = (int32_t)i;
    } else if (string_equals(catalog[i].name, "pipeline.created")) {
      pipeline_metric = (int32_t)i;
    }
  }
  const uint32_t window = profile->warmup_stability_window;
  if (submit_metric < 0 || case_manifest->warmup_frames < window ||
      window < 2u) {
    return false_v;
  }
  float64_t first = 0.0;
  float64_t second = 0.0;
  uint32_t first_count = 0;
  uint32_t second_count = 0;
  const uint32_t start = case_manifest->warmup_frames - window;
  for (uint32_t frame = 0; frame < case_manifest->warmup_frames; ++frame) {
    if (pipeline_metric >= 0) {
      const uint64_t pipeline_offset =
          (uint64_t)frame * metric_count + (uint32_t)pipeline_metric;
      if (availability[pipeline_offset] != VKR_METRIC_AVAILABILITY_VALID ||
          samples[pipeline_offset] != 0.0) {
        return false_v;
      }
    }
    if (frame < start) {
      continue;
    }
    const uint64_t offset =
        (uint64_t)frame * metric_count + (uint32_t)submit_metric;
    if (availability[offset] != VKR_METRIC_AVAILABILITY_VALID) {
      return false_v;
    }
    if (frame - start < window / 2u) {
      first += samples[offset];
      first_count++;
    } else {
      second += samples[offset];
      second_count++;
    }
  }
  if (first_count == 0u || second_count == 0u) {
    return false_v;
  }
  first /= first_count;
  second /= second_count;
  const float64_t denominator = first > second ? first : second;
  const float64_t drift =
      denominator > 0.0 ? vkr_abs_f64(first - second) / denominator : 0.0;
  return drift <= profile->warmup_max_drift_ratio;
}

static ApplicationConfig vkr_harness_child_application_config(
    const VkrHarnessCase *case_manifest, const VkrHarnessProfile *profile,
    const VkrSubsystemPlan *subsystem_plan, uint64_t capture_max_batch_bytes) {
  return (ApplicationConfig){
      .title = "VKR Harness",
      .x = 100,
      .y = 100,
      .width = case_manifest->width,
      .height = case_manifest->height,
      /* Determinism rule 5: the frame limiter is off for profiling. */
      .target_frame_rate = 0u,
      .app_arena_size = MB(2),
      .metrics_config = {.pass_gpu_timings = profile->gpu_timing,
                         .event_subjects = profile->event_subjects},
      .fixed_delta_seconds = case_manifest->fixed_delta_seconds,
      .disable_camera_controller = true_v,
      .window_hidden =
          case_manifest->target == VKR_HARNESS_TARGET_WINDOWED_HIDDEN,
      .disable_skybox = !case_manifest->renderer.skybox,
      .requested_present_mode =
          vkr_harness_present_to_renderer(case_manifest->present),
      .capture_enabled = capture_max_batch_bytes > 0u,
      .capture_ring_capacity = 3u,
      .capture_max_batch_bytes = capture_max_batch_bytes,
      .subsystem_plan = *subsystem_plan,
      .device_requirements =
          {.supported_stages =
               VKR_SHADER_STAGE_VERTEX_BIT | VKR_SHADER_STAGE_FRAGMENT_BIT,
           .supported_queues = VKR_DEVICE_QUEUE_GRAPHICS_BIT |
                               VKR_DEVICE_QUEUE_TRANSFER_BIT |
                               VKR_DEVICE_QUEUE_PRESENT_BIT,
           .allowed_device_types =
               VKR_DEVICE_TYPE_DISCRETE_BIT | VKR_DEVICE_TYPE_INTEGRATED_BIT,
           .supported_sampler_filters = VKR_SAMPLER_FILTER_ANISOTROPIC_BIT},
  };
}

/** Applies the case's renderer configuration to an already-created boot. */
static bool8_t
vkr_harness_child_apply_renderer(Application *application,
                                 const VkrHarnessCase *case_manifest) {
  application->editor_viewport.enabled = case_manifest->renderer.editor;
  VkrShadowConfig shadow_config =
      string_equals(case_manifest->renderer.shadow_preset, "balanced")
          ? VKR_SHADOW_CONFIG_BALANCED
          : VKR_SHADOW_CONFIG_DEFAULT;
  shadow_config.cascade_count = case_manifest->renderer.shadow_cascades;
  vkr_shadow_system_shutdown(&application->renderer.shadow_system,
                             &application->renderer);
  if (!vkr_shadow_system_init(&application->renderer.shadow_system,
                              &application->renderer, &shadow_config)) {
    return false_v;
  }
  if (string_equals(case_manifest->renderer.render_mode, "lighting")) {
    application->renderer.globals.render_mode = VKR_RENDER_MODE_LIGHTING;
  } else if (string_equals(case_manifest->renderer.render_mode, "normal")) {
    application->renderer.globals.render_mode = VKR_RENDER_MODE_NORMAL;
  } else if (string_equals(case_manifest->renderer.render_mode, "unlit")) {
    application->renderer.globals.render_mode = VKR_RENDER_MODE_UNLIT;
  }
  /* Determinism rule 3: the harness camera receives an explicit extent and
     lens; it never reads window size or input state. */
  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                        application->renderer.active_camera);
  return vkr_camera_set_perspective_lens(
      camera, case_manifest->camera.vertical_fov_degrees,
      case_manifest->camera.near_plane, case_manifest->camera.far_plane,
      case_manifest->width, case_manifest->height);
}

static void
vkr_harness_child_device_provenance(Application *application,
                                    VkrHarnessProvenance *provenance) {
  Arena *device_arena = arena_create(KB(16), KB(16));
  if (!device_arena) {
    return;
  }
  VkrDeviceInformation device = {0};
  vkr_renderer_get_device_information(&application->renderer, &device,
                                      device_arena);
  char text[128];
  string_format(text, sizeof(text), "%.*s", (int)device.device_name.length,
                device.device_name.str);
  vkr_harness_provenance_set_text(provenance->gpu, sizeof(provenance->gpu),
                                  text);
  string_format(text, sizeof(text), "%.*s", (int)device.driver_version.length,
                device.driver_version.str);
  vkr_harness_provenance_set_text(provenance->driver,
                                  sizeof(provenance->driver), text);
  provenance->gpu_vendor_id = device.vendor_id;
  provenance->gpu_device_id = device.device_id;
  provenance->actual_present =
      vkr_harness_present_from_renderer(device.actual_present_mode);
  provenance->actual_target_image_count = device.actual_target_image_count;
  string_format(provenance->color_format, sizeof(provenance->color_format),
                "%s",
                vkr_harness_surface_format_name(device.actual_color_format));
  string_format(provenance->color_space, sizeof(provenance->color_space), "%s",
                vkr_harness_color_space_name(device.actual_color_space));
  arena_destroy(device_arena);
}

/** Publishes this repetition's report; the samples file is already durable. */
static bool8_t vkr_harness_child_write_report(
    const VkrHarnessArenas *arenas, const char *report_path,
    const VkrHarnessCase *case_manifest, const VkrHarnessProfile *profile,
    const VkrHarnessProvenance *provenance, const VkrHarnessSampleSet *samples,
    const char *samples_path, const VkrHarnessReport *capture_source,
    VkrHarnessError *error) {
  const bool8_t failed =
      (samples->header.flags & VKR_HARNESS_SAMPLE_FLAG_CHILD_FAILED) != 0u;
  VkrHarnessReport report = {
      .tool =
          capture_source ? VKR_HARNESS_TOOL_SNAPSHOT : VKR_HARNESS_TOOL_PROFILE,
      .case_manifest = *case_manifest,
      .profile = *profile,
      .profile_compatible = true_v,
      .provenance = *provenance,
      .subsystem_mask = samples->header.subsystem_mask,
      .requested_repetitions = 1u,
      .completed_repetitions = failed ? 0u : 1u,
      .warmup_stable =
          (samples->header.flags & VKR_HARNESS_SAMPLE_FLAG_WARMUP_STABLE) != 0u,
      .events_dropped = samples->header.events_dropped,
      .event_subjects_truncated = samples->header.event_subjects_truncated,
  };
  const uint32_t capture_capacity =
      capture_source ? capture_source->capture_count : 0u;
  /* One `samples.raw` row on top of whatever the capture phase published. */
  if (!vkr_harness_report_init_storage(
          &report, arenas->persistent, capture_capacity,
          (capture_source ? capture_source->artifact_count : 0u) + 1u)) {
    vkr_harness_error_set(error, "report.storage", "$",
                          "Unable to size the child report tables");
    return false_v;
  }
  if (capture_source) {
    report.capture_count = capture_source->capture_count;
    report.artifact_count = capture_source->artifact_count;
    MemCopy(report.captures, capture_source->captures,
            sizeof(VkrHarnessCaptureResult) * capture_source->capture_count);
    MemCopy(report.artifacts, capture_source->artifacts,
            sizeof(VkrHarnessArtifact) * capture_source->artifact_count);
    vkr_harness_report_add_authority_reason(&report,
                                            "execution.diagnostic_replay");
  }
  string_format(report.run_id, sizeof(report.run_id), "child");
  vkr_harness_report_set_status(&report, failed ? "incomplete" : "pass",
                                failed ? VKR_HARNESS_EXIT_ERROR
                                       : VKR_HARNESS_EXIT_PASS);
  /* A single repetition is a sample, never independent evidence. */
  vkr_harness_report_add_authority_reason(&report,
                                          "execution.child_repetition");
  if (!profile->authoritative) {
    vkr_harness_report_add_authority_reason(&report, "profile.local_only");
  }
  if (report.provenance.dirty) {
    vkr_harness_report_add_authority_reason(&report, "provenance.dirty");
  }

  VkrHarnessFingerprintField environment[VKR_HARNESS_ENVIRONMENT_FIELD_COUNT];
  const uint32_t environment_count = vkr_harness_environment_fields(
      provenance, profile->require_exclusive_gpu_lane, environment);
  VkrHarnessCase effective_case = *case_manifest;
  effective_case.target_image_count = provenance->actual_target_image_count;
  (void)vkr_harness_case_fingerprints(
      report.tool, &effective_case, profile, report.subsystem_mask, environment,
      environment_count, report.environment_fingerprint,
      report.workload_fingerprint, report.policy_fingerprint, error);

  bool8_t ok = vkr_harness_compute_metric_results(
      arenas, case_manifest->warmup_frames, case_manifest->measure_frames,
      samples->header.metric_count, samples->metrics, samples->values,
      samples->availability, &report.metrics, error);
  if (ok) {
    report.metric_count = samples->header.metric_count;
    ok = vkr_harness_compute_pass_results(
        arenas, case_manifest->warmup_frames, case_manifest->measure_frames,
        samples->header.pass_count, samples->passes, samples->pass_cpu_ms,
        samples->pass_gpu_ms, samples->pass_flags, &report.passes, error);
  }
  if (ok) {
    report.pass_count = samples->header.pass_count;
    ok = vkr_harness_report_add_artifact(&report, "samples.raw", "samples.bin",
                                         "application/vnd.vkr.harness-samples",
                                         samples_path);
    if (!ok) {
      vkr_harness_error_set(error, "artifact.samples", "$",
                            "Unable to digest '%s'", samples_path);
    }
  }
  if (ok && samples->header.event_count > 0u) {
    const uint64_t bytes =
        (uint64_t)samples->header.event_count * sizeof(*report.events);
    report.events =
        arena_alloc(arenas->persistent, bytes, ARENA_MEMORY_TAG_STRUCT);
    ok = report.events != NULL;
    if (ok) {
      MemZero(report.events, bytes);
    }
    for (uint32_t i = 0; ok && i < samples->header.event_count; ++i) {
      const VkrHarnessSampleEvent *source = &samples->events[i];
      VkrHarnessEvent *target = &report.events[report.event_count++];
      string_format(target->source, sizeof(target->source), "%s",
                    source->source);
      string_format(target->subject, sizeof(target->subject), "%s",
                    source->subject);
      target->start_ns = source->start_ns;
      target->duration_ns = source->duration_ns;
      target->bytes = source->bytes;
      target->thread_id = source->thread_id;
      target->success = source->status == VKR_METRIC_EVENT_STATUS_SUCCESS;
      target->subject_truncated = source->subject_truncated;
    }
  }
  return ok && vkr_harness_report_write(report_path, &report, error);
}

/**
 * Fills the header describing this repetition's raw evidence. Kept beside the
 * sample arrays it labels so the two cannot drift apart.
 */
static VkrHarnessSampleFileHeader vkr_harness_child_sample_header(
    const VkrHarnessChildContext *child, const VkrHarnessCase *case_manifest,
    const VkrHarnessProvenance *provenance, VkrSubsystemMask subsystem_mask,
    bool8_t warmup_stable) {
  VkrHarnessSampleFileHeader header = {
      .schema_version = VKR_HARNESS_SCHEMA_VERSION,
      .metric_count = child->metric_count,
      .pass_count = child->pass_count,
      .event_count = child->event_count,
      .events_dropped = child->events_dropped + child->event_storage_dropped,
      .event_subjects_truncated = child->event_subjects_truncated,
      .snapshot_publications_dropped = child->snapshot_publications_dropped,
      .subsystem_mask = subsystem_mask,
      .warmup_frames = case_manifest->warmup_frames,
      .measure_frames = case_manifest->measure_frames,
      .actual_present = provenance->actual_present,
      .actual_image_count = provenance->actual_target_image_count,
      .gpu_vendor_id = provenance->gpu_vendor_id,
      .gpu_device_id = provenance->gpu_device_id,
      .flags = (uint32_t)((warmup_stable ? VKR_HARNESS_SAMPLE_FLAG_WARMUP_STABLE
                                         : 0u) |
                          (child->failed ? VKR_HARNESS_SAMPLE_FLAG_CHILD_FAILED
                                         : 0u)),
  };
  MemCopy(header.magic, VKR_HARNESS_SAMPLE_MAGIC, sizeof(header.magic));
  string_format(header.gpu, sizeof(header.gpu), "%s", provenance->gpu);
  string_format(header.driver, sizeof(header.driver), "%s", provenance->driver);
  string_format(header.color_format, sizeof(header.color_format), "%s",
                provenance->color_format);
  string_format(header.color_space, sizeof(header.color_space), "%s",
                provenance->color_space);
  return header;
}

int vkr_harness_child_run(const char *executable, const char *repo_root,
                          const char *case_path, const char *profile_path,
                          const char *run_dir, bool8_t prewarm,
                          int32_t capture_index) {
  VkrHarnessProvenance provenance = {0};
  vkr_harness_timestamp_utc(provenance.started_at);
  VkrHarnessError error = {0};
  VkrHarnessCase case_manifest = {0};
  VkrHarnessProfile profile = {0};
  if (!vkr_harness_case_load(repo_root, case_path, &case_manifest, &error) ||
      !vkr_harness_profile_load(repo_root, profile_path, &profile, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    return VKR_HARNESS_EXIT_INVALID;
  }
  const char *unsupported = vkr_harness_unsupported(&case_manifest, &profile);
  if (unsupported) {
    vkr_harness_stderr("%s\n", unsupported);
    return VKR_HARNESS_EXIT_UNAVAILABLE;
  }
  if (capture_index >= (int32_t)case_manifest.capture_count) {
    vkr_harness_stderr("Capture index is out of range\n");
    return VKR_HARNESS_EXIT_INVALID;
  }
  const char *mismatch =
      vkr_harness_case_profile_mismatch(&case_manifest, &profile);
  if (mismatch) {
    vkr_harness_stderr("%s\n", mismatch);
    return VKR_HARNESS_EXIT_MISSING_BASELINE;
  }
  VkrSubsystemPlan subsystem_plan = {0};
  if (!vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                  &subsystem_plan, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    return VKR_HARNESS_EXIT_INVALID;
  }
  /* A prewarm process exists only to populate the isolated pipeline cache; it
     measures nothing and publishes no artifact. */
  if (prewarm) {
    case_manifest.measure_frames = 1u;
    case_manifest.warmup_frames =
        case_manifest.warmup_frames > 0u ? case_manifest.warmup_frames : 1u;
  }
  vkr_rand_seed(case_manifest.seed);
  vkr_harness_provenance_collect(executable, repo_root, &provenance);

  /* One bump allocation for everything this repetition produces, and one
     scratch arena for the buffers that only span a single computation. */
  VkrHarnessArenas arenas = {.persistent = arena_create(),
                             .transient = arena_create()};
  if (!arenas.persistent || !arenas.transient) {
    vkr_harness_stderr("Unable to create the repetition arenas\n");
    arena_destroy(arenas.persistent);
    arena_destroy(arenas.transient);
    return VKR_HARNESS_EXIT_ERROR;
  }
  int exit_code = VKR_HARNESS_EXIT_ERROR;
  float64_t *samples = NULL;
  uint8_t *availability = NULL;
  VkrHarnessSampleMetric *sample_catalog = NULL;
  VkrHarnessSampleEvent *events = NULL;
  bool8_t application_live = false_v;
  VkrSubsystemMask subsystem_mask = 0u;
  VkrHarnessChildContext child = {0};
  Application application = {0};
  /* Retained by the application for the lifetime of the run, so it must not be
     const-qualified nor go out of scope before shutdown. */
  uint64_t capture_max_batch_bytes = 0u;
  VkrCaptureItemRequest capture_items[VKR_HARNESS_MAX_CAPTURE_CHANNELS] = {0};
  if (capture_index >= 0) {
    const VkrHarnessCapture *capture = &case_manifest.captures[capture_index];
    uint64_t seen = 0u;
    VkrShadowConfig shadow_config =
        string_equals(case_manifest.renderer.shadow_preset, "balanced")
            ? VKR_SHADOW_CONFIG_BALANCED
            : VKR_SHADOW_CONFIG_DEFAULT;
    shadow_config.cascade_count = case_manifest.renderer.shadow_cascades;
    const uint32_t shadow_size =
        vkr_shadow_config_get_max_map_size(&shadow_config);
    for (uint32_t i = 0; i < capture->channel_count; ++i) {
      VkrCaptureChannelId channel =
          vkr_renderer_capture_channel_from_name(capture->channels[i]);
      if (channel == VKR_CAPTURE_CHANNEL_INVALID || channel >= 64u ||
          (seen & (1ull << channel))) {
        vkr_harness_stderr("Unknown or duplicate capture channel: %s\n",
                           capture->channels[i]);
        exit_code = VKR_HARNESS_EXIT_INVALID;
        goto cleanup;
      }
      seen |= 1ull << channel;
      const VkrCaptureChannelDescription *description =
          vkr_renderer_capture_channel_get(channel);
      if (description->required_subsystem != VKR_RENDERER_SUBSYSTEM_COUNT &&
          !vkr_renderer_subsystem_plan_includes(
              &subsystem_plan,
              (VkrRendererSubsystem)description->required_subsystem)) {
        vkr_harness_stderr("Capture channel is unavailable: %s\n",
                           capture->channels[i]);
        exit_code = VKR_HARNESS_EXIT_UNAVAILABLE;
        goto cleanup;
      }
      capture_items[i] =
          (VkrCaptureItemRequest){.channel = channel, .mip = 0, .layer = 0};
      const bool8_t shadow =
          string_n_equals(description->name, "shadow_cascade_", 15u);
      /* Case resolution is authored in logical window units on macOS while
         Vulkan captures drawable pixels. Budget a 2x backing scale before
         backend creation; the ring remains fixed once execution begins. */
      const uint64_t backing_scale =
          case_manifest.target == VKR_HARNESS_TARGET_WINDOWED_HIDDEN ? 2u : 1u;
      const uint64_t width =
          shadow ? shadow_size : (uint64_t)case_manifest.width * backing_scale;
      const uint64_t height =
          shadow ? shadow_size : (uint64_t)case_manifest.height * backing_scale;
      /* Mirrors the frontend's published batch layout so the ring is never
         sized under what a reservation will ask for. */
      capture_max_batch_bytes =
          (capture_max_batch_bytes + VKR_CAPTURE_BUFFER_ALIGNMENT - 1u) &
          ~((uint64_t)VKR_CAPTURE_BUFFER_ALIGNMENT - 1u);
      capture_max_batch_bytes +=
          width * height * VKR_CAPTURE_MAX_BYTES_PER_PIXEL;
    }
  }
  ApplicationConfig config = vkr_harness_child_application_config(
      &case_manifest, &profile, &subsystem_plan, capture_max_batch_bytes);
  if (!application_create(&application, &config)) {
    exit_code = VKR_HARNESS_EXIT_UNAVAILABLE;
    goto cleanup;
  }
  application_live = true_v;
  subsystem_mask = vkr_renderer_get_subsystem_mask(&application.renderer);
  if (subsystem_mask != subsystem_plan.effective_mask) {
    char planned_text[VKR_HARNESS_SUBSYSTEM_MASK_MAX] = {0};
    char actual_text[VKR_HARNESS_SUBSYSTEM_MASK_MAX] = {0};
    vkr_harness_format_subsystem_mask(planned_text,
                                      subsystem_plan.effective_mask);
    vkr_harness_format_subsystem_mask(actual_text, subsystem_mask);
    vkr_harness_stderr("Renderer initialized subsystem mask %s; the workload "
                       "requires %s\n",
                       actual_text, planned_text);
    exit_code = VKR_HARNESS_EXIT_UNAVAILABLE;
    goto cleanup;
  }
  if (!vkr_harness_child_apply_renderer(&application, &case_manifest)) {
    vkr_harness_stderr("Unable to apply the case renderer configuration\n");
    exit_code = VKR_HARNESS_EXIT_INVALID;
    goto cleanup;
  }
  for (uint32_t i = 0; i < profile.required_metric_count; ++i) {
    if (!vkr_harness_catalog_has(application.metrics,
                                 profile.required_metrics[i])) {
      vkr_harness_stderr("Required metric is not registered: %s\n",
                         profile.required_metrics[i]);
      exit_code = VKR_HARNESS_EXIT_INVALID;
      goto cleanup;
    }
  }

  uint32_t metric_count = 0;
  const VkrMetricCatalogEntry *metric_catalog =
      vkr_metrics_get_catalog(application.metrics, &metric_count);
  const uint32_t total_frames =
      case_manifest.warmup_frames + case_manifest.measure_frames;
  const uint64_t value_count = (uint64_t)total_frames * metric_count;
  const uint64_t catalog_bytes =
      (uint64_t)metric_count * sizeof(VkrHarnessSampleMetric);
  const uint64_t event_bytes =
      (uint64_t)VKR_HARNESS_MAX_EVENTS * sizeof(VkrHarnessSampleEvent);
  samples = arena_alloc(arenas.persistent, value_count * sizeof(float64_t),
                        ARENA_MEMORY_TAG_ARRAY);
  availability =
      arena_alloc(arenas.persistent, value_count, ARENA_MEMORY_TAG_ARRAY);
  sample_catalog =
      arena_alloc(arenas.persistent, catalog_bytes, ARENA_MEMORY_TAG_STRUCT);
  events = arena_alloc(arenas.persistent, event_bytes, ARENA_MEMORY_TAG_STRUCT);
  if (!samples || !availability || !sample_catalog || !events) {
    vkr_harness_stderr("Unable to allocate repetition sample storage\n");
    goto cleanup;
  }
  /* Arenas bump rather than zero, and this storage needs zeroing for two
     reasons: a frame the loop never reaches must read back as
     VKR_METRIC_AVAILABILITY_UNAVAILABLE, and every byte here is copied verbatim
     into samples.bin — including struct padding and the tail of each fixed-size
     name — so uninitialized bytes would make the artifact's digest vary between
     otherwise identical repetitions. */
  MemZero(samples, value_count * sizeof(float64_t));
  MemZero(availability, value_count);
  MemZero(sample_catalog, catalog_bytes);
  MemZero(events, event_bytes);
  for (uint32_t i = 0; i < metric_count; ++i) {
    string_format(sample_catalog[i].name, sizeof(sample_catalog[i].name), "%s",
                  metric_catalog[i].name);
    string_format(sample_catalog[i].unit, sizeof(sample_catalog[i].unit), "%s",
                  vkr_harness_metric_unit_name(metric_catalog[i].unit));
  }
  child = (VkrHarnessChildContext){
      .case_manifest = &case_manifest,
      .arenas = &arenas,
      .load_started = vkr_platform_get_absolute_time(),
      .total_frames = total_frames,
      .metric_count = metric_count,
      .events = events,
      .samples = samples,
      .availability = availability,
      .capture_index = capture_index,
      .run_dir = run_dir,
  };
  if (capture_index >= 0) {
    const VkrHarnessCapture *capture = &case_manifest.captures[capture_index];
    child.capture_report = arena_alloc(
        arenas.persistent, sizeof(VkrHarnessReport), ARENA_MEMORY_TAG_STRUCT);
    if (!child.capture_report) {
      vkr_harness_stderr("Unable to allocate capture report storage\n");
      goto cleanup;
    }
    MemZero(child.capture_report, sizeof(VkrHarnessReport));
    if (!vkr_harness_report_init_storage(
            child.capture_report, arenas.persistent, capture->channel_count,
            capture->channel_count * VKR_HARNESS_ARTIFACTS_PER_CAPTURE)) {
      vkr_harness_stderr("Unable to size the capture report tables\n");
      goto cleanup;
    }
    MemCopy(child.capture_items, capture_items,
            sizeof(VkrCaptureItemRequest) * capture->channel_count);
    child.capture_request = (VkrCaptureBatchRequest){
        .request_id = (VkrCaptureRequestId)capture_index + 1u,
        .items = child.capture_items,
        .item_count = capture->channel_count,
    };
    child.capture_report->tool = VKR_HARNESS_TOOL_SNAPSHOT;
  }
  g_harness_child = &child;

  String8 scene = string8_create_from_cstr((const uint8_t *)case_manifest.scene,
                                           string_length(case_manifest.scene));
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, scene,
                                &application.renderer.scratch_allocator,
                                &child.scene_resource, &load_error)) {
    child.failed = true_v;
    string_format(child.failure, sizeof(child.failure), "scene.enqueue_failed");
  } else {
    application_start(&application);
    application_close(&application);
  }
  vkr_harness_child_drain_events(&application);
  if (child.failed) {
    vkr_harness_stderr("Repetition did not complete: %s\n", child.failure);
  }

  vkr_harness_child_device_provenance(&application, &provenance);
  vkr_harness_timestamp_utc(provenance.ended_at);
  const bool8_t warmup_stable =
      !child.failed &&
      vkr_harness_warmup_stable(&case_manifest, &profile, metric_count,
                                sample_catalog, samples, availability);
  if (child.scene_resource.type == VKR_RESOURCE_TYPE_SCENE ||
      child.scene_resource.request_id != 0u || child.scene_resource.as.scene) {
    vkr_resource_system_unload(&child.scene_resource, scene);
  }
  application_shutdown(&application);
  application_live = false_v;
  g_harness_child = NULL;
  if (prewarm) {
    exit_code = child.failed ? VKR_HARNESS_EXIT_ERROR : VKR_HARNESS_EXIT_PASS;
    goto cleanup;
  }

  char samples_path[VKR_HARNESS_PATH_MAX];
  char report_path[VKR_HARNESS_PATH_MAX];
  char capture_summary_path[VKR_HARNESS_PATH_MAX];
  string_format(samples_path, sizeof(samples_path), "%s/samples.bin", run_dir);
  string_format(report_path, sizeof(report_path), "%s/report.json", run_dir);
  string_format(capture_summary_path, sizeof(capture_summary_path),
                "%s/capture-summary.bin", run_dir);
  const VkrHarnessSampleSet sample_set = {
      .header = vkr_harness_child_sample_header(
          &child, &case_manifest, &provenance, subsystem_mask, warmup_stable),
      .metrics = sample_catalog,
      .values = samples,
      .availability = availability,
      .passes = child.pass_catalog,
      .pass_cpu_ms = child.pass_cpu_samples,
      .pass_gpu_ms = child.pass_gpu_samples,
      .pass_flags = child.pass_flags,
      .events = events,
  };
  if (child.capture_report) {
    child.capture_report->provenance = provenance;
  }
  /* The samples file is durable before the report references its digest. */
  if (vkr_harness_samples_write(samples_path, &sample_set.header, &sample_set,
                                arenas.transient, &error) &&
      vkr_harness_child_write_report(
          &arenas, report_path, &case_manifest, &profile, &provenance,
          &sample_set, samples_path, child.capture_report, &error) &&
      (capture_index < 0 || vkr_harness_capture_summary_write(
                                capture_summary_path, child.capture_report,
                                arenas.transient, &error))) {
    exit_code = child.failed ? VKR_HARNESS_EXIT_ERROR : VKR_HARNESS_EXIT_PASS;
  } else {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    exit_code = VKR_HARNESS_EXIT_ERROR;
  }

cleanup:
  if (application_live) {
    application_shutdown(&application);
  }
  g_harness_child = NULL;
  arena_destroy(arenas.transient);
  arena_destroy(arenas.persistent);
  return exit_code;
}

#else /* !VKR_METRICS_ENABLED */

int vkr_harness_child_run(const char *executable, const char *repo_root,
                          const char *case_path, const char *profile_path,
                          const char *run_dir, bool8_t prewarm,
                          int32_t capture_index) {
  (void)executable;
  (void)repo_root;
  (void)case_path;
  (void)profile_path;
  (void)run_dir;
  (void)prewarm;
  (void)capture_index;
  vkr_harness_stderr("The harness requires VKR_METRICS_ENABLED=1\n");
  return VKR_HARNESS_EXIT_UNAVAILABLE;
}

#endif /* VKR_METRICS_ENABLED */

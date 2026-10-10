#include "renderer/systems/vkr_scene_animation.h"

#include "application/vkr_standard_scene_runtime.h"

#include "physics/vkr_physics.h"
#include "platform/vkr_platform.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tiled pipeline's bounded dynamic lights (ADR-087): the point and spot
   lights it draws, nearest the camera first, and how many of them cast
   shadows. */
#define VKR_STANDARD_SCENE_TILED_LIGHT_MAX 16u
#define VKR_STANDARD_SCENE_TILED_SHADOWED_LIGHT_MAX 4u

/* Payload storage for one scene frame. Each draw_frame stage fills its part
   through a pointer; the packet and its payloads borrow these members. */
typedef struct VkrStandardSceneRuntimeDrawContext {
  const VkrFrame *setup;
  VkrAllocator *scratch;
  bool8_t target_changed;
  bool8_t scene_stopped;
  VkrShadowFrameData shadow_frame;
  uint32_t shadow_cascade_count;
  VkrWorldPassPayload world_payload;
  VkrLocalShadowPassPayload local_shadow_payload;
  VkrShadowPassPayload shadow_payload;
  /* Raster depth bias, distinct from receiver bias. Lowered from the shadow
     config so both selected implementations apply the same configured values
     instead of one backend hardcoding defaults and the other applying none.
     Lives here because the payload borrows it until the frame completes. */
  VkrShadowConfigOverride raster_bias_override;
  bool8_t has_shadow;
  VkrPickingPassPayload picking_payload;
  bool8_t has_picking;
  bool8_t editor_enabled;
  bool8_t has_editor;
  uint32_t viewport_width;
  uint32_t viewport_height;
  VkrViewportMapping editor_mapping;
  VkrEditorPassPayload editor_payload;
  VkrUiPassPayload ui_payload;
  const VkrScene *active_scene;
  VkrSkyPassPayload sky_payload;
  bool8_t has_sky;
  VkrTextureHandle frame_ibl_source;
  bool8_t frame_ibl_enabled;
  float32_t frame_ibl_intensity;
  float32_t frame_ibl_diffuse_intensity;
  float32_t frame_ibl_specular_intensity;
  VkrGpuDebugPayload debug_payload;
  const VkrGpuDebugPayload *debug_ptr;
  /* The caller's capture request this frame carries. */
  const VkrCaptureBatchRequest *capture_request;
  VkrCaptureRequestId capture_request_id;
  /* Picking source extent; the request is scaled once the frame is acquired. */
  uint32_t picking_source_width;
  uint32_t picking_source_height;
  VkrFrameIblProbe frame_ibl_probes[VKR_FRAME_IBL_PROBE_MAX];
  /* Source cubemap of each probe; its SH slot resolves after acquisition. */
  VkrTextureHandle frame_ibl_probe_sources[VKR_FRAME_IBL_PROBE_MAX];
  VkrFrameLighting frame_lighting;
  /* The desktop pipeline's baked lamps, candidates for their moving
     casters' shadows; none on the tiled pipeline. */
  const VkrPointLight *baked_lamps;
  uint32_t baked_lamp_count;
  VkrAnimationPreviewInput animation_preview;
  bool8_t has_animation_preview;
} VkrStandardSceneRuntimeDrawContext;

/* Shadow work a frame performs once acquired, from values the frame-loop
   thread captured. Only the thread rendering a frame uses the shadow system,
   because with a render thread the next frame is being built meanwhile. */
typedef struct VkrStandardSceneRuntimeShadowInput {
  VkrShadowConfig config;
  bool8_t invalidate_fit;
  /* False without an active camera or while the Scene is stopped. */
  bool8_t update;
  VkrCamera camera;
  bool8_t light_enabled;
  Vec3 light_direction;
  float32_t sun_angular_diameter_degrees;
  VkrShadowCasterDepthBounds caster_bounds;
  VkrShadowDepthRangeSample depth_range;
  uint64_t frame_index;
  uint64_t scene_generation;
  bool8_t directional_disabled;
  bool8_t local_disabled;
  bool8_t soft_disabled;
  float64_t delta;
  /* Time the frame-loop thread spent measuring caster bounds. */
  uint64_t measure_ns;
} VkrStandardSceneRuntimeShadowInput;

/* Storage for one frame's packet, owned by the runtime. The renderer borrows
   it until the frame completes: before draw_frame returns when rendering
   inline, after the next frame is built with a render thread. */
struct VkrStandardSceneRuntimeFrame {
  VkrStandardSceneRuntime *application;
  VkrFrame setup;
  VkrStandardSceneRuntimeDrawContext draw;
  VkrEditorOverlayDraw overlay_draws[VKR_EDITOR_OVERLAY_DRAW_MAX];
  /* The capture request as submitted; its owner may change it meanwhile. */
  VkrCaptureBatchRequest capture;
  VkrFrameInput packet;
  VkrStandardSceneRuntimeShadowInput shadow;
  /* Shadow fit and resolve time on the thread that rendered the frame. */
  uint64_t shadow_ns;
  /* Frame scratch scope the packet borrows; closed at completion. */
  VkrAllocatorScope scratch_scope;
  /* Built from cached target values; the render thread acquires it. */
  bool8_t decoupled;
};

vkr_internal bool8_t vkr_standard_scene_runtime_register_duration_metric(
    VkrMetrics *metrics, const char *name, VkrMetricDomain domain,
    bool8_t required, VkrMetricId *out_id) {
  const VkrMetricDescription description = {
      .name =
          string8_create_from_cstr((const uint8_t *)name, string_length(name)),
      .domain = domain,
      .kind = VKR_METRIC_KIND_DURATION,
      .unit = VKR_METRIC_UNIT_NANOSECONDS,
      .scalar = VKR_METRIC_SCALAR_U64,
      .writer = VKR_METRIC_WRITER_RENDER_THREAD,
      .required_when_enabled = required,
  };
  return vkr_metrics_register(metrics, &description, out_id);
}

vkr_internal bool8_t vkr_standard_scene_runtime_metrics_initialize(
    VkrStandardSceneRuntime *application) {
  application->metrics_arena = arena_create(MB(2), KB(64));
  if (!application->metrics_arena) {
    return false_v;
  }
  application->metrics = arena_alloc(
      application->metrics_arena, sizeof(VkrMetrics), ARENA_MEMORY_TAG_STRUCT);
  if (!application->metrics) {
    return false_v;
  }
  application->metrics_allocator =
      (VkrAllocator){.ctx = application->metrics_arena};
  if (!vkr_allocator_arena(&application->metrics_allocator)) {
    return false_v;
  }
  vkr_metrics_init(application->metrics);
  application->metrics->config = application->config->metrics_config;
  VkrStandardSceneRuntimeMetricIds *ids = &application->metric_ids;
  // Durations are nanoseconds. No name carries a unit suffix, because the
  // catalog unit is the contract and a name that disagreed with it would be
  // wrong by a factor of a million in every consumer that trusted it.
  if (!vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "frame.wall", VKR_METRIC_DOMAIN_FRAME, true_v,
          &ids->frame_wall) ||
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "cpu.frame_work", VKR_METRIC_DOMAIN_FRAME,
          true_v, &ids->frame_work) ||
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "cpu.update", VKR_METRIC_DOMAIN_FRAME, true_v,
          &ids->update) ||
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "cpu.render_prepare", VKR_METRIC_DOMAIN_FRAME,
          true_v, &ids->render_prepare) ||
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "cpu.render_submit", VKR_METRIC_DOMAIN_FRAME,
          true_v, &ids->render_submit) ||
      // Not required: the frame limiter is off for profiling, so this slot is
      // legitimately unsampled in exactly the runs that matter most. Marking
      // it required would make every authoritative run report incomplete.
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "frame.limiter_sleep", VKR_METRIC_DOMAIN_FRAME,
          false_v, &ids->limiter_sleep) ||
      // Shadow update is absent without an active camera. Keep both optional so
      // non-rendering and partial-boot frames do not make reports incomplete.
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "cpu.shadow_update", VKR_METRIC_DOMAIN_FRAME,
          false_v, &ids->shadow_update) ||
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "cpu.world_payload_build",
          VKR_METRIC_DOMAIN_FRAME, false_v, &ids->world_payload_build) ||
      // Sampled only with a render thread; inline rendering never waits.
      !vkr_standard_scene_runtime_register_duration_metric(
          application->metrics, "cpu.render_wait", VKR_METRIC_DOMAIN_FRAME,
          false_v, &ids->render_wait) ||
      !vkr_metrics_register(application->metrics,
                            &(VkrMetricDescription){
                                .name = string8_lit("frame.decoupled"),
                                .domain = VKR_METRIC_DOMAIN_FRAME,
                                .kind = VKR_METRIC_KIND_COUNTER,
                                .unit = VKR_METRIC_UNIT_COUNT,
                                .scalar = VKR_METRIC_SCALAR_U64,
                                .writer = VKR_METRIC_WRITER_RENDER_THREAD,
                            },
                            &ids->decoupled) ||
      !vkr_renderer_metrics_register(&application->renderer_metrics,
                                     application->metrics)) {
    return false_v;
  }
  return true_v;
}

/* One Jolt job on an engine worker (VkrPhysicsJobs). */
typedef struct StandardScenePhysicsJob {
  void (*run)(void *arg);
  void *arg;
} StandardScenePhysicsJob;

vkr_internal bool8_t standard_scene_runtime_physics_job(VkrJobContext *context,
                                                        void *payload) {
  (void)context;
  const StandardScenePhysicsJob *job = payload;
  job->run(job->arg);
  return true_v;
}

/* Physics steps wait on these jobs, so they go ahead of loading work. */
vkr_internal bool8_t standard_scene_runtime_physics_submit(void *context,
                                                           void (*run)(void *),
                                                           void *arg) {
  Bitset8 type_mask = bitset8_create();
  bitset8_set(&type_mask, VKR_JOB_TYPE_GENERAL);
  const StandardScenePhysicsJob payload = {.run = run, .arg = arg};
  const VkrJobDesc job = {.priority = VKR_JOB_PRIORITY_HIGH,
                          .type_mask = type_mask,
                          .run = standard_scene_runtime_physics_job,
                          .payload = &payload,
                          .payload_size = sizeof(payload)};
  VkrJobHandle handle = {0};
  return vkr_job_try_submit(context, &job, &handle);
}

/**
 * @brief Creates a cube mesh and uploads it to GPU buffers
 * @param application Pointer to the `VkrStandardSceneRuntime` structure.
 * @return `true_v` on success, `false_v` on failure
 */
bool8_t vkr_standard_scene_runtime_create_cube_mesh(
    VkrStandardSceneRuntime *application);

/**
 * @brief Initializes the application and its core subsystems.
 * Sets up memory arenas, logging, event manager, window, and clock.
 * Subscribes default event handlers.
 * Asserts that the provided configuration is valid.
 * @param application Pointer to an `VkrStandardSceneRuntime` structure to be
 * initialized.
 * @param config Pointer to an `VkrStandardSceneRuntimeConfig` structure
 * containing initialization settings.
 * @return `true_v` on successful initialization, `false_v` if any critical
 * initialization step fails (e.g., arena creation).
 */
vkr_internal uint32_t vkr_standard_scene_runtime_picking_pixel(
    uint32_t pixel, uint32_t source_extent, uint32_t target_extent) {
  if (source_extent <= 1u || target_extent <= 1u)
    return 0u;
  return Min((uint32_t)((uint64_t)Min(pixel, source_extent - 1u) *
                        (target_extent - 1u) / (source_extent - 1u)),
             target_extent - 1u);
}

vkr_internal bool8_t vkr_standard_scene_runtime_on_resize(Event *event,
                                                          UserData user_data) {
  VkrStandardSceneRuntime *application = user_data;
  const VkrWindowResizeEventData *resize = event->data;
  if (!resize || resize->width == 0u || resize->height == 0u)
    return true_v;
  vkr_atomic_uint64_store(&application->pending_resize_mailbox,
                          ((uint64_t)resize->width << 32u) | resize->height,
                          VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

vkr_internal bool8_t vkr_standard_scene_runtime_rendering_initialize(
    VkrStandardSceneRuntime *application, const VkrSubsystemPlan *plan,
    const VkrRendererMetricsProducerConfig *metrics_producers) {
  const float64_t start = vkr_platform_get_absolute_time();
  application->subsystem_plan = *plan;
  const uint32_t frame_arena_count =
      vkr_renderer_render_thread_enabled(&application->renderer) ? 2u : 1u;
  for (uint32_t i = 0u; i < frame_arena_count; ++i) {
    application->frame_arenas[i] = arena_create(MB(32), MB(1));
    if (!application->frame_arenas[i]) {
      return false_v;
    }
    application->frame_allocators[i] =
        (VkrAllocator){.ctx = application->frame_arenas[i]};
    if (!vkr_allocator_arena(&application->frame_allocators[i])) {
      return false_v;
    }
  }
  application->frame_allocator = &application->frame_allocators[0];
  for (uint32_t i = 0u; i < ArrayCount(application->frames); ++i) {
    application->frames[i] = vkr_allocator_alloc(
        &application->app_allocator, sizeof(*application->frames[i]),
        VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!application->frames[i]) {
      return false_v;
    }
    MemZero(application->frames[i], sizeof(*application->frames[i]));
    application->frames[i]->application = application;
  }
  application->frame = application->frames[0];
  application->frame_in_flight = NULL;
  application->target_window_width = application->renderer.last_window_width;
  application->target_window_height = application->renderer.last_window_height;
  application->target_generation_seen = application->renderer.target_generation;
  application->target_known = false_v;
  VkrDeviceInformation device = {0};
  vkr_renderer_get_device_information(&application->renderer, &device,
                                      application->frame_arenas[0]);
  if (!vkr_render_assets_initialize(
          &application->assets, &application->renderer.asset_publisher, &device,
          &application->job_system, metrics_producers,
          application->config->bootstrap_font_directory,
          application->config->texture_max_load_dimension))
    return false_v;
  VkrCameraSystemConfig camera_config = {.max_camera_count = 24u};
  if (!vkr_camera_registry_init(&camera_config, &application->camera_system) ||
      !vkr_camera_registry_create_perspective(
          &application->camera_system, string8_lit("camera.default"),
          vkr_standard_scene_runtime_is_windowed(application)
              ? &application->host.window
              : NULL,
          VKR_STANDARD_SCENE_CAMERA_FOV_DEGREES, 0.1f, 500.0f,
          &application->active_camera))
    return false_v;
  vkr_camera_registry_set_active(&application->camera_system,
                                 application->active_camera);
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  uint32_t width = 0u, height = 0u;
  vkr_renderer_present_target_extent(&application->renderer, &width, &height);
  if (!camera || !vkr_camera_set_perspective_lens(
                     camera, VKR_STANDARD_SCENE_CAMERA_FOV_DEGREES, 0.1f,
                     500.0f, width, height))
    return false_v;
  vkr_camera_system_update(camera);
  if (!vkr_lighting_system_init(&application->lighting_system))
    return false_v;
  VkrShadowConfig shadow_config = VKR_SHADOW_CONFIG_DEFAULT;
  if (!vkr_shadow_system_init(&application->shadow_system, &shadow_config))
    return false_v;
  application->shadow_config = application->shadow_system.config;
  if (vkr_subsystem_plan_includes(plan, VKR_RENDERER_SUBSYSTEM_UI) &&
      !vkr_ui_system_init(&application->ui_system,
                          &application->assets.font_system))
    return false_v;
  if (vkr_subsystem_plan_includes(plan, VKR_RENDERER_SUBSYSTEM_SKYBOX) &&
      !vkr_skybox_system_init(&application->skybox_system))
    return false_v;
  VkrGizmoConfig gizmo_config = VKR_GIZMO_CONFIG_DEFAULT;
  if (vkr_subsystem_plan_includes(plan, VKR_RENDERER_SUBSYSTEM_GIZMO) &&
      !vkr_gizmo_system_init(&application->gizmo_system, &application->assets,
                             &gizmo_config))
    return false_v;
  if (vkr_subsystem_plan_includes(plan, VKR_RENDERER_SUBSYSTEM_PICKING) &&
      !vkr_picking_init(&application->picking, width, height))
    return false_v;
  application->scene_generation = 1u;
  application->ibl_probe_limit = UINT32_MAX;
  application->globals = (VkrFrameGlobals){
      .ambient_color = vec4_new(0.1f, 0.1f, 0.1f, 1.0f),
      .exposure_mode = VKR_EXPOSURE_MODE_AUTOMATIC,
      .manual_exposure = VKR_DEFAULT_EXPOSURE,
      .display_transform = VKR_DISPLAY_TRANSFORM_AGX,
      .white_balance_temperature = 0.0f,
      .white_balance_tint = 0.0f,
      .color_contrast = 1.0f,
      .color_saturation = 1.0f,
      .bloom_enabled = !application->renderer.bloom_forced_disabled,
      .bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD,
      .bloom_knee = VKR_BLOOM_DEFAULT_KNEE,
      .bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY,
      .dof_focus_distance = VKR_DOF_DEFAULT_FOCUS_DISTANCE,
      .dof_f_stop = VKR_DOF_DEFAULT_F_STOP,
      .motion_blur_shutter_angle = VKR_MOTION_BLUR_DEFAULT_SHUTTER_ANGLE,
      .ssr_enabled = !application->renderer.ssr_forced_disabled,
      .ssgi_enabled = false_v,
      .gtao_enabled = !application->renderer.gtao_forced_disabled,
      .gtao_radius = VKR_GTAO_DEFAULT_RADIUS,
      .gtao_power = VKR_GTAO_DEFAULT_POWER,
      .image_sharpness = 0.25f,
      .render_mode = VKR_RENDER_MODE_DEFAULT,
      .fog = vkr_fog_settings_defaults(),
  };
#if VKR_METRICS_ENABLED
  application->renderer.boot_metrics.systems_ns = vkr_metrics_elapsed_ns(start);
#else
  (void)start;
#endif
  return true_v;
}

/* Call after joining workers; borrowed asset owners are released before assets,
 * and the native publisher stays alive through all final resource releases. */
vkr_internal void vkr_standard_scene_runtime_rendering_shutdown(
    VkrStandardSceneRuntime *application) {
  vkr_renderer_wait_idle(&application->renderer);
  if (application->assets.resource_system_initialized)
    vkr_resource_system_quiesce();
  if (application->picking.initialized)
    vkr_picking_shutdown(&application->picking);
  if (application->ui_system.initialized)
    vkr_ui_system_shutdown(&application->ui_system);
  if (application->skybox_system.initialized)
    vkr_skybox_system_shutdown(&application->skybox_system);
  if (application->shadow_system.initialized)
    vkr_shadow_system_shutdown(&application->shadow_system);
  if (application->gizmo_system.initialized)
    vkr_gizmo_system_shutdown(&application->gizmo_system, &application->assets);
  vkr_lighting_system_shutdown(&application->lighting_system);
  vkr_camera_registry_shutdown(&application->camera_system);
  vkr_render_assets_shutdown(&application->assets);
  for (uint32_t i = 0u; i < ArrayCount(application->frame_arenas); ++i) {
    if (!application->frame_arenas[i]) {
      continue;
    }
    vkr_allocator_release_global_accounting(&application->frame_allocators[i]);
    arena_destroy(application->frame_arenas[i]);
    application->frame_arenas[i] = NULL;
  }
  application->frame_allocator = NULL;
  application->frames[0] = NULL;
  application->frames[1] = NULL;
  application->frame = NULL;
  application->frame_in_flight = NULL;
}

bool8_t
vkr_standard_scene_runtime_create(VkrStandardSceneRuntime *application,
                                  VkrStandardSceneRuntimeConfig *config) {
  assert(config != NULL && "VkrStandardSceneRuntime config is NULL");
  assert(config->title != NULL && "VkrStandardSceneRuntime title is NULL");
  assert(config->app_arena_size > 0 &&
         "VkrStandardSceneRuntime arena size is 0");
  assert(config->width > 0 && "VkrStandardSceneRuntime width is less than 0");
  assert(config->height > 0 && "VkrStandardSceneRuntime height is less than 0");

  MemZero(application, sizeof(*application));
  bool8_t log_ready = false_v;
  bool8_t host_ready = false_v;
  bool8_t jobs_ready = false_v;
  bool8_t renderer_ready = false_v;

  application->config = config;
  application->scene_output_scale = 1.0f;
  application->editor_viewport = (VkrStandardSceneRuntimeEditorViewport){
      .enabled = false_v,
      .scene_only = false_v,
      .fit_mode = VKR_VIEWPORT_FIT_STRETCH,
      .render_scale = 1.0f,
      .last_target_width = 0,
      .last_target_height = 0,
  };
  vkr_ui_dock_default_editor_layout(&application->editor_viewport.dock);
  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  application->app_arena = arena_create(
      config->app_arena_size, config->app_arena_size, app_arena_flags);
  if (!application->app_arena) {
    fprintf(stderr, "Failed to create app arena\n");
    goto cleanup;
  }

  application->app_allocator = (VkrAllocator){.ctx = application->app_arena};
  if (!vkr_allocator_arena(&application->app_allocator)) {
    fprintf(stderr, "Failed to initialize app allocator\n");
    goto cleanup;
  }

  ArenaFlags log_arena_flags = bitset8_create();
  bitset8_set(&log_arena_flags, ARENA_FLAG_LARGE_PAGES);
  application->log_arena = arena_create(MB(5), MB(5), log_arena_flags);
  if (!application->log_arena) {
    fprintf(stderr, "Failed to create log arena\n");
    goto cleanup;
  }

  if (!log_init(application->log_arena)) {
    fprintf(stderr, "Failed to initialize logging\n");
    goto cleanup;
  }

  log_ready = true_v;
  log_debug("Initialized logging");

  const bool8_t windowed =
      config->present_target.kind != VKR_PRESENT_TARGET_OFFSCREEN;
  const VkrApplicationHostConfig host_config = {
      .title = config->title,
      .x = config->x,
      .y = config->y,
      .width = config->width,
      .height = config->height,
      .target_frame_rate = config->target_frame_rate,
      .windowed = windowed,
      .window_hidden = config->window_hidden,
      .unified_title_bar = config->unified_title_bar,
      .window_high_dpi_disabled = config->window_high_dpi_disabled,
      .fixed_delta_seconds = config->fixed_delta_seconds,
  };
  if (!vkr_application_host_create(&application->host, &host_config,
                                   &application->app_allocator)) {
    log_error("Failed to initialize application host");
    goto cleanup;
  }
  host_ready = true_v;

  if (!vkr_standard_scene_runtime_metrics_initialize(application)) {
    log_error("Failed to initialize application metrics");
    goto cleanup;
  }

  VkrJobSystemConfig job_cfg = vkr_job_system_config_default();
  if (!vkr_job_system_init(&job_cfg, &application->job_system)) {
    log_error("Failed to initialize job system");
    goto cleanup;
  }

  jobs_ready = true_v;
  const VkrPhysicsJobs physics_jobs = {
      .context = &application->job_system,
      .worker_count = application->job_system.worker_count,
      .min_active_bodies = VKR_PHYSICS_PARALLEL_BODIES,
      .submit = standard_scene_runtime_physics_submit};
  vkr_physics_set_jobs(&physics_jobs);
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  const VkrRendererMetricsProducerConfig *metrics_producers =
      vkr_renderer_metrics_get_producers(&application->renderer_metrics);
  VkrRendererBackendConfig backend_cfg = {
      .application_name = "vulkan_renderer",
      .present_target = config->present_target,
      .requested_present_mode = config->requested_present_mode,
      .display_output_mode = config->display_output_mode,
      .render_scale = config->render_scale,
      .upscale_mode = config->upscale_mode,
      .dynamic_resolution = config->dynamic_resolution,
      .capture_enabled = config->capture_enabled,
      .capture_ring_capacity = config->capture_ring_capacity,
      .capture_max_batch_bytes = config->capture_max_batch_bytes,
      .inline_rendering = config->inline_rendering,
  };
  VkrNativeSurface native_surface = {0};
  const VkrNativeSurface *surface = NULL;
  if (windowed) {
    native_surface = vkr_window_get_render_surface(&application->host.window);
    surface = &native_surface;
  }
  if (!vkr_renderer_initialize(&application->renderer, config->renderer_backend,
                               surface,
                               &application->config->device_requirements,
                               &backend_cfg, &renderer_error)) {
    log_error("Failed to create renderer!");
    goto cleanup;
  }
  renderer_ready = true_v;
  const char *local_shadow_feedback = getenv("VKR_LOCAL_SHADOW_FEEDBACK");
  application->disable_local_shadow_feedback =
      local_shadow_feedback && strcmp(local_shadow_feedback, "0") == 0;
  const char *local_shadow_fade = getenv("VKR_LOCAL_SHADOW_FADE_DISTANCE");
  const float32_t fade_distance =
      local_shadow_fade ? strtof(local_shadow_fade, NULL) : 0.0f;
  application->local_shadow_fade_distance_override =
      isfinite(fade_distance) && fade_distance > 0.0f ? fade_distance : 0.0f;
  vkr_atomic_uint64_store(&application->pending_resize_mailbox, 0u,
                          VKR_MEMORY_ORDER_RELAXED);
  if (windowed && !event_manager_subscribe(
                      &application->host.events, EVENT_TYPE_WINDOW_RESIZE,
                      vkr_standard_scene_runtime_on_resize, application))
    goto cleanup;
  if (!vkr_renderer_metrics_register_device_memory(
          &application->renderer_metrics, &application->renderer) ||
      !vkr_metrics_seal(application->metrics)) {
    log_error("Failed to finalize renderer metrics catalog");
    goto cleanup;
  }

  /* The closure is always recomputed from the config's intent, so a caller
     cannot hand-assemble an `effective_mask` that the renderer never agreed
     to, and a zero-initialized config resolves to the full interactive plan. */
  VkrSubsystemPlan subsystem_plan = {0};
  if (!vkr_subsystem_plan_build(config->subsystem_plan.profile,
                                config->subsystem_plan.requested_mask,
                                config->subsystem_plan.excluded_mask,
                                &subsystem_plan, &renderer_error)) {
    log_error("Failed to build the renderer subsystem plan");
    goto cleanup;
  }
  if (!vkr_standard_scene_runtime_rendering_initialize(
          application, &subsystem_plan, metrics_producers)) {
    log_error("Failed to initialize renderer frontend systems");
    goto cleanup;
  }
  if (!vkr_renderer_metrics_prepare_pass_table(
          &application->renderer_metrics, &application->renderer,
          &application->metrics_allocator)) {
    log_error("Failed to prepare renderer metrics pass table");
    goto cleanup;
  }

  VkrCameraHandle active_camera =
      vkr_camera_registry_get_active(&application->camera_system);
  application->active_camera = active_camera;
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  if (!camera) {
    log_error("Failed to retrieve active camera");
    goto cleanup;
  }
  vkr_camera_controller_create(
      &application->camera_controller, camera,
      (float32_t)application->config->target_frame_rate);

  log_info("VkrStandardSceneRuntime initialized");
  return true_v;

cleanup:
  if (jobs_ready) {
    vkr_physics_set_jobs(NULL);
    vkr_job_system_shutdown(&application->job_system);
  }
  if (renderer_ready) {
    vkr_standard_scene_runtime_rendering_shutdown(application);
    vkr_renderer_destroy(&application->renderer);
  }
  if (host_ready)
    vkr_application_host_destroy(&application->host);
  vkr_allocator_release_global_accounting(&application->metrics_allocator);
  arena_destroy(application->metrics_arena);
  vkr_allocator_release_global_accounting(&application->app_allocator);
  if (log_ready)
    log_shutdown();
  arena_destroy(application->log_arena);
  arena_destroy(application->app_arena);
  MemZero(application, sizeof(*application));
  return false_v;
}

vkr_internal bool8_t vkr_standard_scene_runtime_editor_viewport_panel_rect(
    VkrStandardSceneRuntime *application, uint32_t window_width,
    uint32_t window_height, Vec4 *out_panel_rect) {
  if (!application || !out_panel_rect || window_width == 0u ||
      window_height == 0u)
    return false_v;
  VkrUiRect panel = {0.0f, 0.0f, (float32_t)window_width,
                     (float32_t)window_height};
  if (!application->editor_viewport.scene_only &&
      !application->editor_viewport.scene_maximized) {
    float32_t content_scale = 1.0f;
    if (vkr_standard_scene_runtime_is_windowed(application)) {
      const VkrWindowContentScale scale =
          vkr_window_get_content_scale(&application->host.window);
      if (isfinite(scale.value) && scale.value > 0.0f)
        content_scale = scale.value;
    }
    /* Dock geometry follows the UI's interface zoom. */
    if (application->ui_system.user_scale > 0.0f)
      content_scale *= application->ui_system.user_scale;
    VkrUiDockTree *dock = &application->editor_viewport.dock;
    if (!vkr_ui_dock_layout(dock, panel,
                            VKR_UI_DOCK_SPLITTER_PT * content_scale,
                            VKR_UI_DOCK_TAB_BAR_PT * content_scale))
      return false_v;
    // A hidden Scene tab still submits editor UI, never direct presentation.
    if (!vkr_ui_dock_find_panel(dock, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT, NULL,
                                &panel) ||
        !vkr_ui_rect_has_area(panel))
      panel = (VkrUiRect){0.0f, 0.0f, 1.0f, 1.0f};
  }
  *out_panel_rect = (Vec4){panel.x, panel.y, panel.width, panel.height};
  return true_v;
}

bool8_t vkr_standard_scene_runtime_editor_scene_rendering_stopped(
    const VkrStandardSceneRuntime *application) {
  if (!application->editor_viewport.enabled)
    return false_v;
  if (application->editor_viewport.scene_rendering_stopped)
    return true_v;
  if (application->editor_viewport.scene_only ||
      application->editor_viewport.scene_maximized)
    return false_v;
  VkrUiRect content = {0};
  return !vkr_ui_dock_find_panel(&application->editor_viewport.dock,
                                 VKR_UI_DOCK_PANEL_SCENE_VIEWPORT, NULL,
                                 &content) ||
         !vkr_ui_rect_has_area(content);
}

void vkr_standard_scene_runtime_invalidate_shadow_fit(
    VkrStandardSceneRuntime *runtime) {
  runtime->shadow_fit_invalidate_requested = true_v;
}

bool8_t vkr_standard_scene_runtime_editor_viewport_mapping(
    VkrStandardSceneRuntime *application, uint32_t window_width,
    uint32_t window_height, VkrViewportMapping *out_mapping) {
  Vec4 panel = {0};
  if (!out_mapping || !vkr_standard_scene_runtime_editor_viewport_panel_rect(
                          application, window_width, window_height, &panel))
    return false_v;
  /* The render extent is the one the last acquisition chose; reading the
     renderer's would race the render thread's next acquisition. */
  const bool8_t renderer_scaled_scene =
      application->renderer.upscale_mode == VKR_UPSCALE_MODE_FSR31 ||
      application->renderer.scene_output_extent_overridden ||
      application->renderer.render_scale != 1.0f;
  if (renderer_scaled_scene && application->target_render_width > 0u &&
      application->target_render_height > 0u) {
    return vkr_editor_viewport_mapping_from_panel_rect_and_target(
        panel, application->editor_viewport.fit_mode,
        application->target_render_width, application->target_render_height,
        out_mapping);
  }
  return vkr_editor_viewport_mapping_from_panel_rect(
      panel, application->editor_viewport.fit_mode,
      application->editor_viewport.render_scale *
          application->scene_output_scale,
      out_mapping);
}

typedef enum VkrStandardSceneRuntimeSceneOutputMode {
  VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_KEEP = 0,
  VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_RESTORE,
  VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_SET,
} VkrStandardSceneRuntimeSceneOutputMode;

/* The Scene output extent the next frame needs. */
typedef struct VkrStandardSceneRuntimeSceneOutput {
  VkrStandardSceneRuntimeSceneOutputMode mode;
  uint32_t width;
  uint32_t height;
  bool8_t memory_relief;
} VkrStandardSceneRuntimeSceneOutput;

vkr_internal VkrRendererError vkr_standard_scene_runtime_scene_output_request(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeSceneOutput *out_request) {
  *out_request = (VkrStandardSceneRuntimeSceneOutput){0};
  if (application->renderer.backend_type != VKR_RENDERER_BACKEND_TYPE_METAL &&
      application->renderer.upscale_mode != VKR_UPSCALE_MODE_FSR31)
    return VKR_RENDERER_ERROR_NONE;

  if (vkr_standard_scene_runtime_editor_scene_rendering_stopped(application))
    return VKR_RENDERER_ERROR_NONE;
  const bool8_t paneled =
      application->editor_viewport.enabled &&
      vkr_subsystem_plan_includes(&application->subsystem_plan,
                                  VKR_RENDERER_SUBSYSTEM_EDITOR);
  /* Unit-scale editor scenes already use the packet's mapped viewport. */
  if ((paneled &&
       application->renderer.upscale_mode != VKR_UPSCALE_MODE_FSR31 &&
       application->renderer.render_scale == 1.0f &&
       application->scene_output_scale == 1.0f) ||
      (!paneled && application->scene_output_scale == 1.0f)) {
    out_request->mode = VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_RESTORE;
    return VKR_RENDERER_ERROR_NONE;
  }

  /* Temporal scalers recreate completion-protected output on release. Keep the
     previous Scene output while a dock gesture is live, then resize once.
     Pending memory relief must realize smaller targets before texture retries.
   */
  if (paneled && application->renderer.scene_output_extent_overridden &&
      application->scene_memory_relief_generation ==
          application->assets.material_system
              .texture_stream_relief_generation &&
      (application->editor_viewport.dock_capture.resizing_split ||
       application->editor_viewport.dock_capture.dragging_tab))
    return VKR_RENDERER_ERROR_NONE;

  const VkrWindowPixelSize pixels =
      vkr_standard_scene_runtime_is_windowed(application)
          ? vkr_window_get_pixel_size(&application->host.window)
          : (VkrWindowPixelSize){application->target_window_width,
                                 application->target_window_height};
  Vec4 panel = {0, 0, (float32_t)pixels.width, (float32_t)pixels.height};
  if (paneled && !vkr_standard_scene_runtime_editor_viewport_panel_rect(
                     application, pixels.width, pixels.height, &panel))
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  const float32_t scale = application->scene_output_scale;
  *out_request = (VkrStandardSceneRuntimeSceneOutput){
      .mode = VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_SET,
      .width = vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel.z * scale)),
      .height = vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel.w * scale)),
      .memory_relief =
          application->scene_memory_relief_generation !=
          application->assets.material_system.texture_stream_relief_generation,
  };
  return VKR_RENDERER_ERROR_NONE;
}

/* True when applying `request` would change nothing. The override flag and an
   overridden extent change only through calls on this thread, so reading them
   does not race the render thread's acquisition. */
vkr_internal bool8_t vkr_standard_scene_runtime_scene_output_settled(
    const VkrStandardSceneRuntime *application,
    const VkrStandardSceneRuntimeSceneOutput *request) {
  const VkrRenderer *renderer = &application->renderer;
  switch (request->mode) {
  case VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_RESTORE:
    return !renderer->scene_output_extent_overridden;
  case VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_SET:
    return renderer->scene_output_extent_overridden &&
           renderer->scene_output_width == request->width &&
           renderer->scene_output_height == request->height &&
           !request->memory_relief;
  case VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_KEEP:
  default:
    return true_v;
  }
}

vkr_internal VkrRendererError vkr_standard_scene_runtime_configure_scene_output(
    VkrStandardSceneRuntime *application) {
  if (!application) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  VkrStandardSceneRuntimeSceneOutput request = {0};
  const VkrRendererError error =
      vkr_standard_scene_runtime_scene_output_request(application, &request);
  if (error != VKR_RENDERER_ERROR_NONE) {
    return error;
  }
  switch (request.mode) {
  case VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_RESTORE:
    return vkr_renderer_restore_scene_output_extent(&application->renderer);
  case VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_SET:
    return vkr_renderer_set_scene_output_extent(&application->renderer,
                                                request.width, request.height,
                                                request.memory_relief);
  case VKR_STANDARD_SCENE_RUNTIME_SCENE_OUTPUT_KEEP:
  default:
    return VKR_RENDERER_ERROR_NONE;
  }
}

/* A failed packet has already cancelled its speculative uses. Retry once on
 * the next application frame; the 0.25 floor bounds successive OOM retries. */
vkr_internal bool8_t vkr_standard_scene_runtime_reduce_scene_resolution(
    VkrStandardSceneRuntime *application, VkrRendererError error) {
  VkrStandardSceneRuntimeEditorViewport *viewport =
      &application->editor_viewport;
  if (error != VKR_RENDERER_ERROR_OUT_OF_MEMORY ||
      (!viewport->enabled &&
       application->renderer.backend_type != VKR_RENDERER_BACKEND_TYPE_METAL) ||
      vkr_standard_scene_runtime_editor_scene_rendering_stopped(application) ||
      application->scene_output_scale <= 0.25f)
    return false_v;
  application->scene_output_scale =
      Max(0.25f, application->scene_output_scale * 0.75f);
  application->scene_memory_relief_generation++;
  if (viewport->enabled)
    viewport->scene_error = error;
  vkr_picking_cancel(&application->picking);
  if (vkr_standard_scene_runtime_is_windowed(application))
    vkr_window_set_mouse_capture(&application->host.window, false_v);
  vkr_renderer_invalidate_temporal_history(&application->renderer);
  log_warn("Scene memory limit reached; retrying at %.1f%% output resolution",
           (double)(application->scene_output_scale * 100.0f));
  return true_v;
}

/* Releases a frame acquired on this thread before its submission. Shadow
   reuse state is untouched: it resolves only once the frame is submitted. */
vkr_internal void
vkr_standard_scene_runtime_cancel_frame(VkrStandardSceneRuntime *application,
                                        VkrFrame *frame,
                                        VkrRendererError error) {
  const VkrRendererError cancel_error = frame->renderer
                                            ? vkr_renderer_cancel_frame(frame)
                                            : VKR_RENDERER_ERROR_NONE;
  application->last_renderer_error =
      cancel_error != VKR_RENDERER_ERROR_NONE ? cancel_error : error;
  String8 message =
      vkr_renderer_get_error_string(application->last_renderer_error);
  log_error("Failed to prepare scene frame: %s", string8_cstr(&message));
  if (application->last_renderer_error == VKR_RENDERER_ERROR_DEVICE_ERROR)
    vkr_application_host_close(&application->host);
}

/* Acquisition parameters from the frame-loop thread's shadow configuration. */
vkr_internal VkrFrameConfig
vkr_standard_scene_runtime_frame_config(const VkrStandardSceneRuntime *app) {
  const bool8_t shadows = app->shadow_system.initialized;
  const VkrShadowConfig *config = &app->shadow_config;
  return (VkrFrameConfig){
      .shadow_map_size =
          shadows ? vkr_shadow_config_get_max_map_size(config) : 2048u,
      .shadow_cascade_count = shadows ? config->cascade_count : 1u,
      .local_shadow_map_size = shadows ? config->local_shadow_map_size
                                       : VKR_LOCAL_SHADOW_MAP_SIZE_DEFAULT,
      .local_shadow_face_budget =
          shadows ? Min(config->local_shadow_face_budget,
                        VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX)
                  : VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX,
  };
}

/* Reports a failed acquisition. A minimized or resizing window skips frames
   as a matter of course on every tick, so a skipped frame does not log. */
vkr_internal void
vkr_standard_scene_runtime_handle_acquire_error(VkrStandardSceneRuntime *app,
                                                VkrRendererError error) {
  app->last_renderer_error = error;
  if (error == VKR_RENDERER_ERROR_NONE ||
      error == VKR_RENDERER_ERROR_FRAME_SKIPPED) {
    return;
  }
  String8 message = vkr_renderer_get_error_string(error);
  log_error("Failed to prepare renderer frame: %s", string8_cstr(&message));
  const bool8_t reduced =
      vkr_standard_scene_runtime_reduce_scene_resolution(app, error);
  if (!reduced && app->editor_viewport.enabled &&
      !vkr_standard_scene_runtime_editor_scene_rendering_stopped(app) &&
      error != VKR_RENDERER_ERROR_DEVICE_ERROR &&
      error != VKR_RENDERER_ERROR_CAPTURE_BUSY &&
      error != VKR_RENDERER_ERROR_RESOURCE_BUSY) {
    app->editor_viewport.scene_error = error;
    app->editor_viewport.scene_rendering_stopped = true_v;
    vkr_picking_cancel(&app->picking);
    vkr_window_set_mouse_capture(&app->host.window, false_v);
  }
  if (!app->editor_viewport.enabled && !reduced &&
      error == VKR_RENDERER_ERROR_OUT_OF_MEMORY) {
    log_error("Scene memory recovery exhausted; stopping application");
    vkr_application_host_close(&app->host);
  }
  if (error == VKR_RENDERER_ERROR_DEVICE_ERROR) {
    log_fatal("Renderer device is unusable; stopping");
    vkr_application_host_close(&app->host);
  }
}

/* Configures the Scene output and acquires the frame on this thread. Returns
   false_v when nothing was acquired; the failure is reported here. */
vkr_internal bool8_t vkr_standard_scene_runtime_begin_scene_frame(
    VkrStandardSceneRuntime *application, VkrFrame *setup) {
  const VkrFrameConfig frame_config =
      vkr_standard_scene_runtime_frame_config(application);
  VkrRendererError prepare_err = VKR_RENDERER_ERROR_NONE;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.render_prepare) {
    prepare_err =
        vkr_standard_scene_runtime_configure_scene_output(application);
    if (prepare_err == VKR_RENDERER_ERROR_NONE)
      prepare_err = vkr_renderer_begin_frame(&application->renderer,
                                             &frame_config, setup);
  }
  vkr_standard_scene_runtime_handle_acquire_error(application, prepare_err);
  return prepare_err == VKR_RENDERER_ERROR_NONE;
}

/* Finalizes completed asset work and advances texture streaming. Their
   publications are recorded; the thread rendering the next frame runs them
   inside it. Serials and device memory are those last observed, because a
   frame may be rendering. Returns false_v when recording fails. */
vkr_internal bool8_t vkr_standard_scene_runtime_pump_frame_assets(
    VkrStandardSceneRuntime *application, const VkrFrame *setup) {
  const VkrResourceSubmissionState submission = {
      .submit_serial = application->submit_serial_seen,
      .completed_submit_serial = application->completed_submit_serial_seen,
      .frame_active = true_v,
  };
  if (!vkr_render_assets_pump_publications(&application->assets, submission)) {
    return false_v;
  }
  VkrMaterialSystem *materials = &application->assets.material_system;
  if (!materials->texture_stream_budget_user_configured &&
      (materials->texture_stream_active_count ||
       setup->number - 1u >= application->texture_memory_sample_frame + 60u)) {
    if (application->device_memory_seen_valid) {
      vkr_render_assets_refresh_texture_residency_budget(
          &application->assets, &application->device_memory_seen);
      application->texture_memory_sample_frame = setup->number - 1u;
    }
  }
  vkr_material_system_pump_texture_streams(materials, 32u);
  return true_v;
}

/* Builds the world payload unless the Scene is stopped. Returns the build
   error that cancels the frame; texture demand and visibility stats are
   refreshed only when there is none. */
vkr_internal VkrRendererError vkr_standard_scene_runtime_build_world_payload(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  VkrVisibilityStats visibility_stats = {0};
  VkrRendererError world_error = VKR_RENDERER_ERROR_NONE;
  /* The renderer's publication state joins the payload once the frame is
     acquired (vkr_standard_scene_runtime_prepare_acquired). */
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.world_payload_build) {
    if (!draw->scene_stopped)
      world_error = vkr_scene_build_world_draws(
          &application->assets.mesh_manager,
          &application->assets.material_system, false_v, 1u,
          application->globals.view, application->globals.projection,
          application->renderer.graphics_pipeline ==
              VKR_GRAPHICS_PIPELINE_TILED,
          draw->scratch, &draw->world_payload, &visibility_stats);
  }
  if (world_error != VKR_RENDERER_ERROR_NONE) {
    return world_error;
  }
  draw->world_payload.caster_publication_generation =
      application->assets.caster_publication_generation;
  /* Both pipelines draw decals (ADR-092); their textures join this frame's
     residency demand. */
  if (!draw->scene_stopped) {
    VkrSceneDecalFrame *decals = &application->decal_frame;
    vkr_scene_decal_frame_begin(decals);
    vkr_scene_decal_frame_append(
        decals, vkr_standard_scene_runtime_render_scene(application),
        application->globals.view_position);
    for (uint32_t i = 0; i < application->additive_count; ++i) {
      vkr_scene_decal_frame_append(decals, application->additive_scenes[i],
                                   application->globals.view_position);
    }
    vkr_scene_decal_frame_finish(decals, &application->assets.material_system);
    if (decals->count > 0u) {
      draw->world_payload.decals = decals->decals;
      draw->world_payload.decal_count = decals->count;
      draw->world_payload.decal_grid = &decals->grid;
    }
  }
  vkr_material_system_refresh_texture_stream_demand(
      &application->assets.material_system);
  application->visibility_stats = visibility_stats;
  return VKR_RENDERER_ERROR_NONE;
}

/* Resolves directional cascades and local shadow views, then lowers the
   cascades and the shadow config into the shadow pass payload. */
vkr_internal void vkr_standard_scene_runtime_prepare_shadow_payloads(
    VkrStandardSceneRuntime *application, VkrStandardSceneRuntimeFrame *frame) {
  VkrStandardSceneRuntimeDrawContext *draw = &frame->draw;
  const VkrStandardSceneRuntimeShadowInput *input = &frame->shadow;
  const VkrFrame *setup = draw->setup;
  if (application->shadow_system.initialized) {
    /* Moments derive from a cascade's depth, and a newly realized moments
       array holds none, so retained cascades redraw once when filtering
       changes which cascades keep moments. */
    const VkrShadowConfig *previous = &application->shadow_system.config;
    const bool8_t moments_changed =
        previous->far_cascade_evsm != input->config.far_cascade_evsm ||
        (input->config.far_cascade_evsm &&
         previous->cascade_count != input->config.cascade_count);
    application->shadow_system.config = input->config;
    if (input->invalidate_fit || moments_changed) {
      vkr_shadow_system_invalidate_fit_history(&application->shadow_system);
    }
  }
  if (input->update) {
    const float64_t update_start = vkr_platform_get_absolute_time();
    vkr_shadow_system_set_depth_range_sample(
        &application->shadow_system,
        input->depth_range.submit_value > 0u ? &input->depth_range : NULL,
        input->frame_index, input->scene_generation);
    vkr_shadow_system_update(&application->shadow_system, &input->camera,
                             input->light_enabled, input->light_direction,
                             &input->caster_bounds);
    frame->shadow_ns =
        (uint64_t)((vkr_platform_get_absolute_time() - update_start) *
                   1000000000.0);
  }
  if (!input->directional_disabled &&
      draw->world_payload.gpu_shadow_candidate_count > 0u &&
      application->shadow_system.initialized) {
    vkr_shadow_system_resolve_frame(
        &application->shadow_system, setup->image_index, setup->retained_shadow,
        &draw->world_payload,
        vkr_renderer_get_shadow_depth_format(&application->renderer),
        &draw->shadow_frame);
    draw->shadow_cascade_count =
        draw->shadow_frame.enabled ? draw->shadow_frame.cascade_count : 0u;
  } else {
    vkr_shadow_system_discard_frame(&application->shadow_system);
  }

  if (!draw->scene_stopped && !input->local_disabled &&
      draw->world_payload.gpu_shadow_candidate_count > 0u &&
      application->shadow_system.initialized) {
    const VkrLocalShadowCamera camera = {
        .view = frame->packet.globals.view,
        .projection = frame->packet.globals.projection,
        .position = frame->packet.globals.view_position,
        .delta_seconds = (float32_t)input->delta,
        .frame_index = setup->number,
    };
    application->shadow_system.light_contribution_ranking_disabled =
        application->disable_local_shadow_feedback;
    if (application->local_shadow_fade_distance_override > 0.0f)
      application->shadow_system.config.local_shadow_fade_distance =
          application->local_shadow_fade_distance_override;
    vkr_shadow_system_set_light_contribution_sample(
        &application->shadow_system,
        &application->renderer.timing_result.local_light_contribution);
    vkr_shadow_system_resolve_local_shadows(
        &application->shadow_system, setup->retained_local_shadow,
        &draw->world_payload, draw->frame_lighting.point_lights,
        draw->frame_lighting.point_light_count, draw->baked_lamps,
        draw->baked_lamp_count, &camera, &draw->local_shadow_payload);
  }

  const VkrShadowFrameData *shadow_frame = &draw->shadow_frame;
  const uint32_t shadow_cascade_count = draw->shadow_cascade_count;
  VkrShadowPassPayload *shadow_payload = &draw->shadow_payload;
  if (shadow_cascade_count > 0) {
    const VkrShadowConfig *shadow_config = &application->shadow_system.config;
    const float32_t inverse_map_size =
        1.0f / (float32_t)shadow_config->shadow_map_size;
    const float32_t sun_tan_half_angle = tanf(
        (input->soft_disabled ? 0.0f : input->sun_angular_diameter_degrees) *
        0.008726646259971648f);
    shadow_payload->cascade_count = shadow_cascade_count;
    shadow_payload->sdsm_enabled = shadow_config->sdsm_enabled;
    shadow_payload->evsm_enabled =
        shadow_config->far_cascade_evsm &&
        shadow_cascade_count > VKR_SHADOW_EVSM_FIRST_CASCADE;
    shadow_payload->cascade_render_mask = shadow_frame->cascade_render_mask;
    for (uint32_t i = 0; i < shadow_cascade_count; ++i) {
      shadow_payload->cascades[i] = (VkrShadowCascadePacketData){
          .light_view_projection = shadow_frame->view_projection[i],
          .split_near_far_texel_depth =
              {shadow_frame->split_near[i], shadow_frame->split_far[i],
               shadow_frame->world_units_per_texel[i],
               shadow_frame->light_space_depth_span[i]},
          .origin_inv_size_sun = {shadow_frame->light_space_origin[i].x,
                                  shadow_frame->light_space_origin[i].y,
                                  inverse_map_size, sun_tan_half_angle},
      };
    }
    /* The fade ends at the *resolved* last split, not at
       `max_shadow_distance`. The split loop clamps the far split to the
       camera's far plane as well, so a camera closer than the configured
       distance would otherwise leave the band unfinished and restore the hard
       terminating edge the fade exists to remove. */
    const float32_t last_split =
        shadow_frame->split_far[shadow_cascade_count - 1u];
    const float32_t fade_start = vkr_max_f32(
        last_split - shadow_config->shadow_distance_fade_range, 0.0f);
    shadow_payload->receiver = (VkrShadowReceiverPacketData){
        .receiver_bias_texels = shadow_config->receiver_bias_texels,
        .slope_bias_texels = shadow_config->receiver_slope_bias_texels,
        .normal_offset_texels = shadow_config->normal_offset_texels,
        .pcf_radius_texels = shadow_config->pcf_radius_texels,
        .pcf_sample_count = shadow_config->pcf_sample_count,
        .pcf_uniform_early_out = shadow_config->pcf_uniform_early_out,
        .cascade_blend_fraction = shadow_config->cascade_blend_fraction,
        .fade_start = fade_start,
        .fade_end = vkr_max_f32(last_split, fade_start),
    };
    draw->raster_bias_override = (VkrShadowConfigOverride){
        .depth_bias_constant = shadow_config->depth_bias_constant_factor,
        .depth_bias_slope = shadow_config->depth_bias_slope_factor,
        .depth_bias_clamp = shadow_config->depth_bias_clamp,
    };
    shadow_payload->config_override = &draw->raster_bias_override;
    draw->has_shadow = draw->world_payload.gpu_shadow_candidate_count > 0u;
  }
}

/* The capture request this frame carries: the caller's, unless the frame still
   rendering already carries it. */
vkr_internal const VkrCaptureBatchRequest *
vkr_standard_scene_runtime_frame_capture(
    const VkrStandardSceneRuntime *application) {
  const VkrCaptureBatchRequest *request = application->capture_request;
  const VkrStandardSceneRuntimeFrame *in_flight = application->frame_in_flight;
  if (request && in_flight && in_flight->draw.capture_request == request &&
      in_flight->draw.capture_request_id == request->request_id) {
    return NULL;
  }
  return request;
}

vkr_internal void vkr_standard_scene_runtime_prepare_picking_payload(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  const VkrCaptureBatchRequest *capture =
      vkr_standard_scene_runtime_frame_capture(application);
  draw->has_picking =
      !draw->scene_stopped &&
      application->picking.state == VKR_PICKING_STATE_RENDER_PENDING;
  /* An identifier capture has no producer unless the picking pass runs this
     frame, so the request itself schedules it. The catalog names the dependency
     as a subsystem; matching on the channel name instead would silently stop
     working the moment a channel is renamed. */
  if (!draw->scene_stopped && !draw->has_picking && capture) {
    for (uint32_t i = 0; i < capture->item_count; ++i) {
      const VkrCaptureChannelDescription *channel =
          vkr_renderer_capture_channel_get(capture->items[i].channel);
      if (channel && channel->required_feature == VKR_CAPTURE_FEATURE_PICKING) {
        draw->has_picking = true_v;
        break;
      }
    }
  }
  if (draw->has_picking) {
    draw->picking_payload.pending = true_v;
    draw->picking_payload.x = application->picking.requested_x;
    draw->picking_payload.y = application->picking.requested_y;
    draw->picking_payload.request_id =
        application->picking.state == VKR_PICKING_STATE_RENDER_PENDING
            ? application->picking.request_id
            : 0u;
  }
}

/* Maps the editor Scene panel into the editor payload and keeps camera
   extents in step with the presented target. */
vkr_internal void vkr_standard_scene_runtime_prepare_editor_viewport(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  const VkrFrame *setup = draw->setup;
  draw->editor_enabled =
      application->editor_viewport.enabled &&
      vkr_subsystem_plan_includes(&application->subsystem_plan,
                                  VKR_RENDERER_SUBSYSTEM_EDITOR);
  if (draw->editor_enabled) {
    if (vkr_standard_scene_runtime_editor_viewport_mapping(
            application, setup->window_width, setup->window_height,
            &draw->editor_mapping) &&
        vkr_editor_viewport_build_payload(&draw->editor_mapping,
                                          &draw->editor_payload)) {
      draw->editor_payload.scene_rendering_stopped = draw->scene_stopped;
      draw->editor_payload.scene_backdrop_blur =
          application->editor_viewport.scene_backdrop_blur;
      draw->editor_payload.grid = application->editor_viewport.grid;
      draw->viewport_width = draw->editor_mapping.target_width;
      draw->viewport_height = draw->editor_mapping.target_height;
      draw->has_editor = true_v;
    } else {
      draw->editor_enabled = false_v;
    }
  }

  if (draw->editor_enabled) {
    if (!draw->scene_stopped &&
        (draw->viewport_width !=
             application->editor_viewport.last_target_width ||
         draw->viewport_height !=
             application->editor_viewport.last_target_height)) {
      vkr_camera_registry_resize_all(&application->camera_system,
                                     draw->viewport_width,
                                     draw->viewport_height);
      application->editor_viewport.last_target_width = draw->viewport_width;
      application->editor_viewport.last_target_height = draw->viewport_height;
    }
  } else if (draw->target_changed ||
             application->editor_viewport.last_target_width != 0 ||
             application->editor_viewport.last_target_height != 0) {
    vkr_camera_registry_resize_all(&application->camera_system,
                                   setup->window_width, setup->window_height);
    application->editor_viewport.last_target_width = 0;
    application->editor_viewport.last_target_height = 0;
  }
}

/* Records the extent the requested picking pixel refers to. */
vkr_internal void vkr_standard_scene_runtime_prepare_picking_source(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  const VkrFrame *setup = draw->setup;
  draw->picking_source_width =
      draw->editor_enabled ? application->picking.width : setup->window_width;
  draw->picking_source_height =
      draw->editor_enabled ? application->picking.height : setup->window_height;
}

/* Maps the requested picking pixel into the acquired frame's scaled Scene
   render extent. Runs on the thread rendering the frame, where the scale the
   acquisition chose is current. */
vkr_internal void vkr_standard_scene_runtime_scale_picking_payload(
    const VkrRenderer *renderer, VkrStandardSceneRuntimeDrawContext *draw) {
  const VkrFrame *setup = draw->setup;
  if (draw->has_picking && (renderer->scene_output_extent_overridden ||
                            renderer->render_scale != 1.0f)) {
    draw->picking_payload.x = vkr_standard_scene_runtime_picking_pixel(
        draw->picking_payload.x, draw->picking_source_width,
        setup->render_width);
    draw->picking_payload.y = vkr_standard_scene_runtime_picking_pixel(
        draw->picking_payload.y, draw->picking_source_height,
        setup->render_height);
  }
}

/* The published cloud layer drifts with its wind. Offsets wrap at the weather
   period, where the cloud noise tiles without a seam. */
vkr_internal void vkr_standard_scene_runtime_advance_cloud_wind(
    VkrStandardSceneRuntime *application, float64_t delta) {
  const VkrScene *scene = vkr_standard_scene_runtime_render_scene(application);
  if (!scene || !scene->atmosphere.active_revision ||
      !scene->atmosphere.active_clouds.enabled || !isfinite(delta) ||
      delta <= 0.0)
    return;
  const float64_t period = (float64_t)VKR_CLOUD_WIND_PERIOD_M;
  const float64_t wind[2] = {scene->atmosphere.active_clouds.wind_mps.x,
                             scene->atmosphere.active_clouds.wind_mps.y};
  for (uint32_t i = 0; i < 2u; ++i) {
    const float64_t offset =
        fmod(application->cloud_wind_offset_m[i] + wind[i] * delta, period);
    application->cloud_wind_offset_m[i] =
        offset < 0.0 ? offset + period : offset;
  }
}

/* The wind offset plus the scene's origin rebase (ADR-086), so clouds stay
   where the document puts them when the world moves under the camera,
   wrapped into one period. Narrowing can round an offset just below the
   period up to it. */
vkr_internal float32_t
vkr_standard_scene_runtime_cloud_offset(float64_t offset, float32_t origin) {
  const float64_t period = (float64_t)VKR_CLOUD_WIND_PERIOD_M;
  float64_t wrapped = fmod(offset + (float64_t)origin, period);
  wrapped = wrapped < 0.0 ? wrapped + period : wrapped;
  const float32_t narrowed = (float32_t)wrapped;
  return narrowed < VKR_CLOUD_WIND_PERIOD_M ? narrowed : 0.0f;
}

/* Selects the visible sky and the frame IBL from the scene's ready global
   environment. A disabled sky light keeps an atmosphere sky visible and
   removes only its image-based lighting. */
vkr_internal void vkr_standard_scene_runtime_prepare_environment(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  const VkrScene *active_scene = draw->active_scene;
  VkrAtmosphereSettings no_atmosphere = vkr_atmosphere_settings_defaults();
  no_atmosphere.enabled = false_v;
  draw->sky_payload = (VkrSkyPassPayload){
      .atmosphere = no_atmosphere,
      .transmittance = VKR_TEXTURE_HANDLE_INVALID,
      .multiple_scattering = VKR_TEXTURE_HANDLE_INVALID,
      .clouds = vkr_cloud_settings_defaults(),
  };
  draw->has_sky = false_v;
  /* A prefilter is sampled once its publication and bake are confirmed. */
  const bool8_t environment_ready =
      active_scene &&
      active_scene->environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_READY &&
      vkr_texture_system_publication_confirmed(
          &application->assets.texture_system,
          active_scene->environment.prefilter_cubemap);
  const VkrSceneEnvironment *environment =
      environment_ready ? &active_scene->environment : NULL;
  const bool8_t sky_light = environment && environment->enabled;

  /* The published generation's medium travels with the lookup textures it
     baked, so a pending candidate never mixes with the visible sky; the sun
     is the scene's current one. */
  if (application->skybox_system.initialized && environment) {
    if (environment->source_kind == VKR_SCENE_ENV_SOURCE_ATMOSPHERE &&
        active_scene->world_state.atmosphere.enabled &&
        active_scene->atmosphere.active_revision &&
        active_scene->atmosphere.active_settings.enabled) {
      draw->sky_payload.atmosphere =
          vkr_scene_atmosphere_frame_settings(active_scene);
      draw->sky_payload.clouds = active_scene->atmosphere.active_clouds;
      draw->sky_payload.cloud_wind_offset_m =
          vec2_new(vkr_standard_scene_runtime_cloud_offset(
                       application->cloud_wind_offset_m[0],
                       active_scene->origin_offset.x),
                   vkr_standard_scene_runtime_cloud_offset(
                       application->cloud_wind_offset_m[1],
                       active_scene->origin_offset.z));
      draw->sky_payload.transmittance = environment->atmosphere_transmittance;
      draw->sky_payload.multiple_scattering =
          environment->atmosphere_multiple_scattering;
      draw->has_sky = true_v;
    } else if (environment->source_kind == VKR_SCENE_ENV_SOURCE_CONSTANT) {
      draw->sky_payload.constant_radiance = environment->constant_radiance;
      draw->has_sky = true_v;
    }
  }

  draw->frame_ibl_source =
      sky_light ? environment->source_cubemap : VKR_TEXTURE_HANDLE_INVALID;
  draw->frame_ibl_enabled = sky_light;
  draw->frame_ibl_intensity = sky_light ? environment->intensity : 1.0f;
  draw->frame_ibl_diffuse_intensity =
      sky_light ? environment->diffuse_intensity : 1.0f;
  draw->frame_ibl_specular_intensity =
      sky_light ? environment->specular_intensity : 1.0f;
}

/* Applies queued world text edits and prepares visible text draws. Returns
   the error that cancels the frame, or VKR_RENDERER_ERROR_NONE. */
vkr_internal VkrRendererError vkr_standard_scene_runtime_prepare_world_text(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  VkrWorldResources *world_resources = &application->assets.world_resources;
  if (application->world_text_update_count >
      VKR_STANDARD_SCENE_RUNTIME_MAX_PENDING_TEXT_UPDATES) {
    return VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
  }
  for (uint32_t i = 0u; i < application->world_text_update_count; ++i) {
    const VkrStandardSceneRuntimeTextUpdate *pending =
        &application->world_text_updates[i];
    if (!world_resources->initialized ||
        !vkr_world_resources_text_update(world_resources, pending->text_id,
                                         pending->content) ||
        (pending->has_transform &&
         !vkr_world_resources_text_set_transform(
             world_resources, pending->text_id, &pending->transform))) {
      return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
    }
  }
  application->world_text_update_count = 0u;
  if (!draw->scene_stopped && world_resources->initialized) {
    VkrPreparedTextDraw *text_draws = NULL;
    uint32_t text_draw_count = 0u;
    if (!vkr_world_resources_prepare_text_draws(
            world_resources, draw->scratch, &text_draws, &text_draw_count)) {
      return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
    }
    draw->world_payload.text_draws = text_draws;
    draw->world_payload.text_draw_count = text_draw_count;
  }
  return VKR_RENDERER_ERROR_NONE;
}

/* Projects UI anchors for the editor viewport and prepares the UI draw
   list. Returns false_v when the draw list cannot be prepared. */
vkr_internal bool8_t vkr_standard_scene_runtime_prepare_ui_payload(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  const VkrFrame *setup = draw->setup;
  VkrUiSystem *ui = &application->ui_system;
  if (application->project_ui && draw->has_editor)
    application->project_ui(application, &draw->editor_mapping);
  /* An unauthored UI frame contributes an empty stream. */
  if (ui->initialized && ui->frame_index > 0u &&
      !vkr_ui_system_prepare_draw_list(ui, draw->scratch, setup->window_width,
                                       setup->window_height,
                                       &draw->ui_payload.draw_list)) {
    return false_v;
  }
  return true_v;
}

/* Selection outline: every submesh of the selected entity and its
 * descendants, visited depth first through the hierarchy index. Storage is
 * frame scratch; a selection past VKR_EDITOR_SELECTION_DRAW_MAX outlines its
 * first draws only. Skinned meshes outline their bind pose. */
vkr_internal void vkr_standard_scene_runtime_prepare_selection_outline(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  /* The selection may live in the World or an added scene. */
  const VkrScene *scene = application->selection_outline_scene
                              ? application->selection_outline_scene
                              : draw->active_scene;
  const VkrEntityId selected = application->selection_outline_entity;
  if (!scene || !selected.u64 || !vkr_scene_entity_alive(scene, selected))
    return;
  VkrEditorOverlayDraw *draws = vkr_allocator_alloc(
      draw->scratch, sizeof(*draws) * VKR_EDITOR_SELECTION_DRAW_MAX,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrEntityId *stack = vkr_allocator_alloc(
      draw->scratch, sizeof(*stack) * VKR_EDITOR_SELECTION_DRAW_MAX,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!draws || !stack)
    return;
  VkrMeshManager *meshes = &application->assets.mesh_manager;
  uint32_t depth = 0u;
  uint32_t count = 0u;
  stack[depth++] = selected;
  while (depth && count < VKR_EDITOR_SELECTION_DRAW_MAX) {
    const VkrEntityId entity = stack[--depth];
    uint32_t child_count = 0u;
    const VkrEntityId *children =
        vkr_scene_get_children(scene, entity, &child_count);
    for (uint32_t i = 0u;
         i < child_count && depth < VKR_EDITOR_SELECTION_DRAW_MAX; ++i)
      stack[depth++] = children[i];
    /* Shapes draw from a generated mesh slot. */
    const SceneShape *shape =
        vkr_entity_get_component(scene->world, entity, scene->comp_shape);
    const VkrMesh *slot = shape && shape->mesh_index != VKR_INVALID_ID
                              ? vkr_mesh_manager_get(meshes, shape->mesh_index)
                              : NULL;
    if (slot && slot->visible &&
        slot->loading_state == VKR_MESH_LOADING_STATE_LOADED) {
      for (uint64_t s = 0u;
           s < slot->submeshes.length && count < VKR_EDITOR_SELECTION_DRAW_MAX;
           ++s) {
        draws[count++] = (VkrEditorOverlayDraw){
            .geometry = slot->submeshes.data[s].geometry,
            .submesh_index = slot->submeshes.data[s].geometry_submesh_index,
            .model = slot->model,
        };
      }
    }
    const SceneMeshRenderer *renderer = vkr_entity_get_component(
        scene->world, entity, scene->comp_mesh_renderer);
    VkrMeshInstance *instance =
        renderer ? vkr_mesh_manager_get_instance(meshes, renderer->instance)
                 : NULL;
    if (!instance || !instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    const VkrMeshAsset *asset =
        vkr_mesh_manager_get_live_asset(meshes, instance->asset);
    for (uint64_t s = 0u; asset && s < asset->submeshes.length &&
                          count < VKR_EDITOR_SELECTION_DRAW_MAX;
         ++s) {
      const VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
      draws[count++] = (VkrEditorOverlayDraw){
          .geometry = submesh->geometry,
          .submesh_index = submesh->geometry_submesh_index,
          .model = instance->model,
      };
    }
  }
  if (!count)
    return;
  const float32_t scale = application->ui_system.content_scale;
  draw->editor_payload.selection_draws = draws;
  draw->editor_payload.selection_draw_count = count;
  /* Orange reads against both the blue accent and most scene colors. */
  draw->editor_payload.selection_color = (Vec4){1.0f, 0.26f, 0.01f, 1.0f};
  draw->editor_payload.selection_width_px = (uint32_t)vkr_clamp_f32(
      roundf(2.0f * (scale > 0.0f ? scale : 1.0f)), 1.0f, 8.0f);
}

vkr_internal void vkr_standard_scene_runtime_prepare_debug_payload(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  // The metrics config is the single authority for whether timestamps are
  // recorded. A second copy could drift and make a report claim timestamps
  // were on for a run that never took one, which would silently mislabel the
  // run's comparison configuration.
  const bool8_t pass_gpu_timing = application->metrics->config.pass_gpu_timings;
  const bool8_t submission_gpu_timing =
      application->metrics->config.submission_gpu_timings;
  const bool8_t gpu_timing = pass_gpu_timing || submission_gpu_timing;
  const VkrCaptureBatchRequest *capture =
      vkr_standard_scene_runtime_frame_capture(application);
  draw->debug_payload = (VkrGpuDebugPayload){
      .enable_timing = gpu_timing,
      .capture_pass_timestamps = pass_gpu_timing,
      .capture_submission_timing = submission_gpu_timing,
      .transmission_depth_diagnostic_enabled =
          !draw->scene_stopped &&
          application->transmission_depth_diagnostic_enabled,
      .shadow_debug_mode =
          draw->scene_stopped ? 0u : application->shadow_debug_mode,
      .capture = capture,
  };
  draw->capture_request = capture;
  draw->capture_request_id = capture ? capture->request_id : 0u;
  draw->debug_ptr = (gpu_timing || capture ||
                     application->transmission_depth_diagnostic_enabled ||
                     application->shadow_debug_mode != 0u)
                        ? &draw->debug_payload
                        : NULL;
}

/* Packs ready reflection probes and lowers the lighting system into the
   frame lighting. */
vkr_internal void vkr_standard_scene_runtime_prepare_frame_lighting(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw) {
  const VkrScene *active_scene = draw->active_scene;
  VkrFrameIblProbe *frame_ibl_probes = draw->frame_ibl_probes;
  uint32_t frame_ibl_probe_count = 0;
  /* Cold ADR-038 control: the fixture asserts the packed count, so an
     unavailable probe texture cannot silently reduce the measured work. */
  const uint32_t frame_ibl_probe_cap =
      Min(application->ibl_probe_limit, VKR_FRAME_IBL_PROBE_MAX);
  if (active_scene) {
    for (uint32_t i = 0; i < active_scene->reflection_probe_count &&
                         frame_ibl_probe_count < frame_ibl_probe_cap;
         ++i) {
      const VkrSceneReflectionProbe *probe =
          &active_scene->reflection_probes[i];
      if (!probe->enabled ||
          probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY ||
          !vkr_texture_system_publication_confirmed(
              &application->assets.texture_system, probe->prefilter_cubemap)) {
        continue;
      }
      /* The SH slot is the renderer's; it resolves once the frame is
         acquired. */
      draw->frame_ibl_probe_sources[frame_ibl_probe_count] =
          probe->source_cubemap;
      frame_ibl_probes[frame_ibl_probe_count++] = (VkrFrameIblProbe){
          .prefilter = probe->prefilter_cubemap,
          /* Probes are placed in document space (ADR-086). */
          .center = vec3_sub(probe->center, active_scene->origin_offset),
          .extents = probe->extents,
          .blend_distance = probe->blend_distance,
          .weight = 1.0f,
          .intensity = probe->intensity,
          .diffuse_intensity = probe->diffuse_intensity,
          .specular_intensity = probe->specular_intensity,
          .box_projection_enabled = true_v,
      };
    }
  }
  draw->frame_ibl_enabled =
      draw->frame_ibl_enabled || frame_ibl_probe_count > 0u;
  draw->frame_lighting = (VkrFrameLighting){
      .directional_enabled = application->lighting_system.directional.enabled,
      .directional_direction =
          application->lighting_system.directional.direction,
      .directional_color = application->lighting_system.directional.color,
      .directional_intensity =
          application->lighting_system.directional.intensity,
      .ibl_enabled = draw->frame_ibl_enabled,
      .ibl_source = draw->frame_ibl_source,
      .ibl_intensity = draw->frame_ibl_intensity,
      .ibl_diffuse_intensity = draw->frame_ibl_diffuse_intensity,
      .ibl_specular_intensity = draw->frame_ibl_specular_intensity,
      .rectangle_lights = application->lighting_system.rectangle_lights,
      .rectangle_light_count =
          application->lighting_system.rectangle_light_count,
      .point_lights = application->lighting_system.point_lights,
      .point_light_count = application->lighting_system.point_light_count,
      .point_light_grid = &application->lighting_system.point_light_grid,
      .ibl_probes = frame_ibl_probes,
      .ibl_probe_count = frame_ibl_probe_count,
      .subsurface = active_scene &&
                            active_scene->world_state.subsurface.enabled &&
                            !application->disable_subsurface_scattering &&
                            application->globals.projection.m33 == 0.0f
                        ? active_scene->subsurface
                        : (VkrSubsurfaceBinding){0},
  };
  vkr_scene_lightmap_binding(active_scene, &draw->frame_lighting.lightmap);
  const bool8_t desktop =
      application->renderer.graphics_pipeline != VKR_GRAPHICS_PIPELINE_TILED;
  /* Both pipeline classes shade moving casters' shadows of the baked lamps
     (ADR-104, ADR-108). */
  (void)desktop;
  draw->baked_lamps = application->lighting_system.baked_lamps;
  draw->baked_lamp_count = application->lighting_system.baked_lamp_count;
  if (active_scene && active_scene->world_state.diffuse_volume.enabled) {
    vkr_scene_diffuse_volume_binding(active_scene,
                                     &application->assets.texture_system,
                                     &draw->frame_lighting.diffuse_volume);
    /* The baked volume is placed in document space (ADR-086). */
    draw->frame_lighting.diffuse_volume.origin =
        vec3_sub(draw->frame_lighting.diffuse_volume.origin,
                 active_scene->origin_offset);
  }
}

/* Assembles the frame input. Its payload pointers borrow `draw`, which the
   caller keeps alive until submit returns. */
vkr_internal VkrFrameInput vkr_standard_scene_runtime_build_frame_input(
    VkrStandardSceneRuntime *application,
    VkrStandardSceneRuntimeDrawContext *draw, float64_t delta) {
  const VkrFrame *setup = draw->setup;
  const VkrScene *active_scene = draw->active_scene;
  /* A resolved post_process component overrides the runtime's base
     post-processing; without one the base (defaults or a harness case)
     applies (ADR-076). */
  VkrFrameGlobals base = application->globals;
  if (active_scene && active_scene->world_state.has_post_process) {
    const ScenePostProcess *post = &active_scene->world_state.post_process;
    base.exposure_mode = (VkrExposureMode)post->exposure_mode;
    base.manual_exposure = post->manual_exposure;
    base.exposure_compensation_ev = post->exposure_compensation_ev;
    base.white_balance_temperature = post->white_balance_temperature;
    base.white_balance_tint = post->white_balance_tint;
    base.color_contrast = post->contrast;
    base.color_saturation = post->saturation;
    base.image_sharpness = post->sharpness;
    base.bloom_threshold = post->bloom_threshold;
    base.bloom_knee = post->bloom_knee;
    base.bloom_intensity = post->bloom_intensity;
    base.dof_focus_distance = post->dof_focus_distance;
    base.dof_f_stop = post->dof_f_stop;
    base.motion_blur_shutter_angle = post->motion_blur_shutter_angle;
    base.gtao_radius = post->gtao_radius;
    base.gtao_power = post->gtao_power;
  }
  /* Look volumes at the camera override part of that look, the height fog
     and the sky light (ADR-097). */
  const VkrExposureMeteringConfig metering =
      vkr_exposure_metering_config_default();
  VkrLook look = {
      .exposure_compensation_ev = base.exposure_compensation_ev,
      .metering_min_ev = metering.min_ev,
      .metering_max_ev = metering.max_ev,
      .white_balance_temperature = base.white_balance_temperature,
      .white_balance_tint = base.white_balance_tint,
      .contrast = base.color_contrast,
      .saturation = base.color_saturation,
      .bloom_intensity = base.bloom_intensity,
      .fog_color = active_scene ? active_scene->world_state.fog.color
                                : vkr_fog_settings_defaults().color,
      .fog_density = active_scene ? active_scene->world_state.fog.density
                                  : vkr_fog_settings_defaults().density,
      .sky_light_intensity = draw->frame_lighting.ibl_intensity,
  };
  VkrLookVolume *volumes = application->look_volumes;
  const uint32_t volume_count = vkr_scene_look_volumes(
      active_scene, volumes, ArrayCount(application->look_volumes));
  if (volume_count > 0u) {
    vkr_look_blend(&look, volumes, volume_count, base.view_position);
    base.exposure_compensation_ev = look.exposure_compensation_ev;
    base.white_balance_temperature = look.white_balance_temperature;
    base.white_balance_tint = look.white_balance_tint;
    base.color_contrast = look.contrast;
    base.color_saturation = look.saturation;
    base.bloom_intensity = look.bloom_intensity;
    draw->frame_lighting.ibl_intensity = look.sky_light_intensity;
  }
  const VkrFrameGlobals *globals = &base;
  VkrFrameInput packet = {
      .animation_preview =
          draw->has_animation_preview ? &draw->animation_preview : NULL,
      .version = VKR_FRAME_INPUT_VERSION,
      .frame =
          {
              .frame_index = (uint32_t)setup->number,
              .delta_time = delta,
              .window_width = setup->window_width,
              .window_height = setup->window_height,
              .viewport_width = draw->viewport_width,
              .viewport_height = draw->viewport_height,
              .editor_enabled = draw->editor_enabled,
              .scene_generation = application->scene_generation,
          },
      .globals =
          {
              .view = globals->view,
              .projection = globals->projection,
              .view_position = globals->view_position,
              .ambient_color = globals->ambient_color,
              .exposure_mode = (uint32_t)globals->exposure_mode,
              .manual_exposure = globals->manual_exposure,
              .exposure_compensation_ev = globals->exposure_compensation_ev,
              .exposure_min_ev = look.metering ? look.metering_min_ev : 0.0f,
              .exposure_max_ev = look.metering ? look.metering_max_ev : 0.0f,
              .display_transform = (uint32_t)globals->display_transform,
              .white_balance_temperature = globals->white_balance_temperature,
              .white_balance_tint = globals->white_balance_tint,
              .color_contrast = globals->color_contrast,
              .color_saturation = globals->color_saturation,
              .bloom_enabled = globals->bloom_enabled,
              .bloom_threshold = globals->bloom_threshold,
              .bloom_knee = globals->bloom_knee,
              .bloom_intensity = globals->bloom_intensity,
              .dof_enabled =
                  globals->dof_enabled && globals->projection.m33 == 0.0f,
              .dof_focus_distance = globals->dof_focus_distance,
              .dof_f_stop = globals->dof_f_stop,
              .motion_blur_enabled = globals->motion_blur_enabled &&
                                     globals->projection.m33 == 0.0f,
              .motion_blur_shutter_angle = globals->motion_blur_shutter_angle,
              .ssr_enabled = globals->ssr_enabled,
              .ssgi_enabled = globals->ssgi_enabled,
              .gtao_enabled = globals->gtao_enabled,
              .gtao_radius = globals->gtao_radius,
              .gtao_power = globals->gtao_power,
              .image_sharpness = globals->image_sharpness,
              .render_mode = (uint32_t)globals->render_mode,
              .fog = active_scene ? active_scene->world_state.fog
                                  : vkr_fog_settings_defaults(),
              .froxel_fog = active_scene ? active_scene->world_state.froxel_fog
                                         : vkr_froxel_fog_settings_defaults(),
          },
      .lighting = draw->scene_stopped ? NULL : &draw->frame_lighting,
      .world = draw->scene_stopped ? NULL : &draw->world_payload,
      .shadow = draw->has_shadow ? &draw->shadow_payload : NULL,
      .local_shadow = draw->local_shadow_payload.view_count
                          ? &draw->local_shadow_payload
                          : NULL,
      .sky = !draw->scene_stopped && !application->config->disable_skybox &&
                     draw->has_sky
                 ? &draw->sky_payload
                 : NULL,
      .ui = &draw->ui_payload,
      .editor = draw->has_editor ? &draw->editor_payload : NULL,
      .picking = draw->has_picking ? &draw->picking_payload : NULL,
      .debug = draw->debug_ptr,
  };

  /* Density boxes are placed in document space (ADR-086). */
  for (uint32_t i = 0; active_scene && i < packet.globals.froxel_fog.box_count;
       ++i) {
    VkrFroxelDensityBox *box = &packet.globals.froxel_fog.boxes[i];
    box->minimum = vec3_sub(box->minimum, active_scene->origin_offset);
    box->maximum = vec3_sub(box->maximum, active_scene->origin_offset);
  }
  packet.globals.fog.color = look.fog_color;
  packet.globals.fog.density = look.fog_density;
  if (application->disable_fog)
    packet.globals.fog.enabled = false_v;
  if (application->disable_volumetric_fog ||
      application->globals.projection.m33 != 0.0f)
    packet.globals.froxel_fog.enabled = false_v;
  return packet;
}

/* Applies the submit result to retained shadow and texture relief, editor
   output, picking and capture state, and the last renderer error. Also fills
   the frame telemetry the renderer does not produce. */
vkr_internal void vkr_standard_scene_runtime_finish_submit(
    VkrStandardSceneRuntime *application,
    const VkrStandardSceneRuntimeDrawContext *draw, VkrRendererError submit_err,
    VkrRendererFrameMetrics *metrics) {
  const VkrShadowFrameData *shadow_frame = &draw->shadow_frame;
  if (submit_err == VKR_RENDERER_ERROR_NONE) {
    if (!draw->scene_stopped) {
      if (application->scene_memory_relief_generation >
          application->assets.material_system
              .texture_stream_relief_generation) {
        VkrDeviceMemoryStats relieved_memory = {0};
        if (vkr_renderer_get_device_memory_stats(&application->renderer,
                                                 &relieved_memory)) {
          vkr_render_assets_refresh_texture_residency_budget(
              &application->assets, &relieved_memory);
          application->texture_memory_sample_frame = draw->setup->number - 1u;
        }
      }
      vkr_material_system_commit_scene_memory_relief(
          &application->assets.material_system,
          application->scene_memory_relief_generation);
    }
    if (draw->has_editor && !draw->scene_stopped) {
      application->editor_viewport.scene_error = VKR_RENDERER_ERROR_NONE;
      application->editor_viewport.rendered_width = draw->viewport_width;
      application->editor_viewport.rendered_height = draw->viewport_height;
      const bool8_t scaled_scene =
          application->renderer.scene_output_extent_overridden ||
          application->renderer.render_scale != 1.0f ||
          application->renderer.upscale_mode == VKR_UPSCALE_MODE_FSR31;
      application->editor_viewport.output_width =
          scaled_scene ? application->renderer.scene_output_width
                       : draw->viewport_width;
      application->editor_viewport.output_height =
          scaled_scene ? application->renderer.scene_output_height
                       : draw->viewport_height;
    }
  }
  for (uint32_t cascade = 0u; cascade < shadow_frame->cascade_count;
       ++cascade) {
    metrics->shadow.rendered[cascade] = shadow_frame->rendered[cascade];
    metrics->shadow.reused[cascade] = shadow_frame->reused[cascade];
    metrics->shadow.correctness_forced[cascade] =
        shadow_frame->correctness_forced[cascade];
    metrics->shadow.proactive_refreshed[cascade] =
        shadow_frame->proactive_refreshed[cascade];
    metrics->shadow.dynamic_candidates_tested[cascade] =
        shadow_frame->dynamic_candidates_tested[cascade];
    metrics->shadow.dynamic_forced[cascade] =
        shadow_frame->dynamic_forced[cascade];
  }
  metrics->shadow.sdsm_status = (uint32_t)shadow_frame->sdsm_status;
  metrics->shadow.sdsm_source_lag = shadow_frame->sdsm_source_lag;
  metrics->shadow.sdsm_occupied_count = shadow_frame->sdsm_occupied_count;
  metrics->shadow.sdsm_linear_near = shadow_frame->sdsm_linear_near;
  metrics->shadow.sdsm_linear_far = shadow_frame->sdsm_linear_far;
  application->last_renderer_error = submit_err;
  vkr_mesh_manager_get_metrics(&application->assets.mesh_manager,
                               &metrics->world.mesh_assets);
  /* With a render thread the next frame's update ran before this frame
     completed; a request made there is not this frame's to settle. */
  if (submit_err == VKR_RENDERER_ERROR_NONE && draw->has_picking &&
      application->picking.state == VKR_PICKING_STATE_RENDER_PENDING &&
      application->picking.request_id == draw->picking_payload.request_id) {
    application->picking.state = VKR_PICKING_STATE_READBACK_PENDING;
  }
  if (submit_err != VKR_RENDERER_ERROR_CAPTURE_BUSY &&
      application->capture_request == draw->capture_request &&
      (!draw->capture_request ||
       draw->capture_request->request_id == draw->capture_request_id)) {
    application->capture_request = NULL;
  }
}

/* Reports a failed submit or unrelieved texture pressure, then retries at a
   lower Scene resolution, stops the editor Scene, or closes the host. */
vkr_internal void vkr_standard_scene_runtime_handle_scene_error(
    VkrStandardSceneRuntime *application,
    const VkrStandardSceneRuntimeDrawContext *draw, VkrRendererError submit_err,
    const VkrValidationError *validation) {
  VkrRendererError scene_error = submit_err;
  const VkrMaterialTextureStreamStats texture_streams =
      vkr_material_system_get_texture_stream_stats(
          &application->assets.material_system);
  /* Finish outstanding publication/retry work before judging remaining
     texture pressure. Native frame OOM still requests immediate relief. */
  if (scene_error == VKR_RENDERER_ERROR_NONE && !draw->scene_stopped &&
      application->assets.material_system
          .texture_stream_memory_recovery_enabled &&
      texture_streams.in_flight_count == 0u &&
      (application->assets.material_system.texture_stream_memory_wait_count ||
       texture_streams.demanded_evicted_count))
    scene_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  if (scene_error != VKR_RENDERER_ERROR_NONE) {
    if (submit_err == VKR_RENDERER_ERROR_NONE) {
      log_warn("Scene textures require memory relief");
    } else if (validation->field_path && validation->message) {
      log_error("Packet validation failed: %s (%s)", validation->field_path,
                validation->message);
    } else {
      String8 err = vkr_renderer_get_error_string(submit_err);
      log_error("Packet submit failed: %s", string8_cstr(&err));
    }
    const bool8_t reduced = !draw->scene_stopped &&
                            vkr_standard_scene_runtime_reduce_scene_resolution(
                                application, scene_error);
    if (draw->has_editor && !draw->scene_stopped && !reduced &&
        submit_err != VKR_RENDERER_ERROR_DEVICE_ERROR &&
        submit_err != VKR_RENDERER_ERROR_CAPTURE_BUSY &&
        submit_err != VKR_RENDERER_ERROR_RESOURCE_BUSY &&
        submit_err != VKR_RENDERER_ERROR_FRAME_SKIPPED) {
      /* A failed Scene must not starve presentation of the editor controls.
         Resume is an explicit retry after unloading or changing the scene. */
      application->editor_viewport.scene_error = scene_error;
      application->editor_viewport.scene_rendering_stopped = true_v;
      vkr_picking_cancel(&application->picking);
      vkr_window_set_mouse_capture(&application->host.window, false_v);
      log_warn("Scene rendering stopped after a frame failure; editor controls "
               "remain available. Unload the scene or retry rendering.");
    }
    if (!draw->has_editor && !reduced &&
        scene_error == VKR_RENDERER_ERROR_OUT_OF_MEMORY) {
      log_error("Scene memory recovery exhausted; stopping application");
      vkr_application_host_close(&application->host);
    }
    if (submit_err == VKR_RENDERER_ERROR_DEVICE_ERROR) {
      log_fatal("Renderer device is unusable; stopping");
      vkr_application_host_close(&application->host);
    }
  }
}

/* Copies `bytes` of `source` into frame scratch; an empty copy still yields
   storage. Returns NULL when scratch is exhausted. */
vkr_internal void *
vkr_standard_scene_runtime_scratch_copy(VkrAllocator *scratch,
                                        const void *source, uint64_t bytes) {
  void *copy = vkr_allocator_alloc(scratch, bytes ? bytes : 1u,
                                   VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (copy && bytes) {
    MemCopy(copy, source, bytes);
  }
  return copy;
}

/* With a render thread the renderer reads the packet while the next frame is
   built. Copies what that work may change or free: animation palettes and
   bind poses, world text geometry, the decals, the lights, the UI draw list
   and the capture request. Returns false_v when scratch is exhausted. */
vkr_internal bool8_t
vkr_standard_scene_runtime_own_frame_data(VkrStandardSceneRuntimeFrame *frame) {
  VkrStandardSceneRuntimeDrawContext *draw = &frame->draw;
  VkrAllocator *scratch = draw->scratch;
  VkrWorldPassPayload *world = &draw->world_payload;

  if (world->skinning_count) {
    VkrSkinningInput *skinning = vkr_standard_scene_runtime_scratch_copy(
        scratch, world->skinning,
        (uint64_t)world->skinning_count * sizeof(*skinning));
    if (!skinning) {
      return false_v;
    }
    for (uint32_t i = 0u; i < world->skinning_count; ++i) {
      VkrSkinningInput *input = &skinning[i];
      input->vertices = vkr_standard_scene_runtime_scratch_copy(
          scratch, input->vertices,
          (uint64_t)input->vertex_count * sizeof(*input->vertices));
      input->influences = vkr_standard_scene_runtime_scratch_copy(
          scratch, input->influences,
          (uint64_t)input->vertex_count * sizeof(*input->influences));
      input->palette = vkr_standard_scene_runtime_scratch_copy(
          scratch, input->palette,
          (uint64_t)input->joint_count * sizeof(*input->palette));
      if (!input->vertices || !input->influences || !input->palette) {
        return false_v;
      }
    }
    world->skinning = skinning;
  }

  if (world->text_draw_count) {
    VkrPreparedTextDraw *text_draws = vkr_standard_scene_runtime_scratch_copy(
        scratch, world->text_draws,
        (uint64_t)world->text_draw_count * sizeof(*text_draws));
    if (!text_draws) {
      return false_v;
    }
    for (uint32_t i = 0u; i < world->text_draw_count; ++i) {
      VkrPreparedTextDraw *text = &text_draws[i];
      text->vertices = vkr_standard_scene_runtime_scratch_copy(
          scratch, text->vertices,
          (uint64_t)text->vertex_count * sizeof(*text->vertices));
      text->indices = vkr_standard_scene_runtime_scratch_copy(
          scratch, text->indices,
          (uint64_t)text->index_count * sizeof(*text->indices));
      if (!text->vertices || !text->indices) {
        return false_v;
      }
    }
    world->text_draws = text_draws;
  }

  if (world->decal_count) {
    world->decals = vkr_standard_scene_runtime_scratch_copy(
        scratch, world->decals,
        (uint64_t)world->decal_count * sizeof(*world->decals));
    world->decal_grid = vkr_standard_scene_runtime_scratch_copy(
        scratch, world->decal_grid, sizeof(*world->decal_grid));
    if (!world->decals || !world->decal_grid) {
      return false_v;
    }
  }

  if (draw->baked_lamp_count) {
    draw->baked_lamps = vkr_standard_scene_runtime_scratch_copy(
        scratch, draw->baked_lamps,
        (uint64_t)draw->baked_lamp_count * sizeof(*draw->baked_lamps));
    if (!draw->baked_lamps) {
      return false_v;
    }
  }

  VkrFrameLighting *lighting = &draw->frame_lighting;
  if (lighting->point_light_count) {
    lighting->point_lights = vkr_standard_scene_runtime_scratch_copy(
        scratch, lighting->point_lights,
        (uint64_t)lighting->point_light_count *
            sizeof(*lighting->point_lights));
    if (!lighting->point_lights) {
      return false_v;
    }
  }
  if (lighting->point_light_grid) {
    lighting->point_light_grid = vkr_standard_scene_runtime_scratch_copy(
        scratch, lighting->point_light_grid,
        sizeof(*lighting->point_light_grid));
    if (!lighting->point_light_grid) {
      return false_v;
    }
  }
  if (lighting->rectangle_light_count) {
    lighting->rectangle_lights = vkr_standard_scene_runtime_scratch_copy(
        scratch, lighting->rectangle_lights,
        (uint64_t)lighting->rectangle_light_count *
            sizeof(*lighting->rectangle_lights));
    if (!lighting->rectangle_lights) {
      return false_v;
    }
  }

  /* The UI system rewrites its cached draw list when it builds the next. */
  VkrPreparedUiDrawList *ui = &draw->ui_payload.draw_list;
  if (ui->vertex_count) {
    ui->vertices = vkr_standard_scene_runtime_scratch_copy(
        scratch, ui->vertices,
        (uint64_t)ui->vertex_count * sizeof(*ui->vertices));
    ui->indices = vkr_standard_scene_runtime_scratch_copy(
        scratch, ui->indices, (uint64_t)ui->index_count * sizeof(*ui->indices));
    ui->batches = vkr_standard_scene_runtime_scratch_copy(
        scratch, ui->batches, (uint64_t)ui->batch_count * sizeof(*ui->batches));
    if (!ui->vertices || !ui->indices || !ui->batches) {
      return false_v;
    }
  }

  const VkrCaptureBatchRequest *capture = draw->debug_payload.capture;
  if (capture) {
    frame->capture = *capture;
    frame->capture.items = vkr_standard_scene_runtime_scratch_copy(
        scratch, capture->items,
        (uint64_t)capture->item_count * sizeof(*capture->items));
    if (!frame->capture.items) {
      return false_v;
    }
    draw->debug_payload.capture = &frame->capture;
  }
  return true_v;
}

/* Completes the parts of a frame that need its acquisition: publication
   state, probe SH slots, shadows and the picking pixel. Runs on the thread
   rendering the frame, the only one that uses the shadow system while it
   renders. */
vkr_internal bool8_t vkr_standard_scene_runtime_prepare_acquired(
    void *state, const VkrFrame *acquired, VkrFrameInput *packet) {
  VkrStandardSceneRuntimeFrame *frame = state;
  VkrStandardSceneRuntime *application = frame->application;
  VkrStandardSceneRuntimeDrawContext *draw = &frame->draw;
  if (frame->decoupled &&
      acquired->target_generation != frame->setup.target_generation) {
    frame->shadow.invalidate_fit = true_v;
  }
  frame->setup = *acquired;
  frame->setup.renderer = NULL;
  draw->setup = &frame->setup;
  packet->frame.frame_index = (uint32_t)acquired->number;

  /* This thread owns the renderer now, and the frame's publications have run:
     ask the backend directly. */
  const VkrAssetPublisher *native = &application->renderer.native_publisher;
  draw->world_payload.publication_pending =
      draw->world_payload.publication_pending || !native->publications_idle ||
      !native->publications_idle(native->state);
  draw->world_payload.publication_generation =
      native->publication_generation
          ? native->publication_generation(native->state)
          : 1u;
  for (uint32_t i = 0u; i < draw->frame_lighting.ibl_probe_count; ++i) {
    draw->frame_ibl_probes[i].sh_slot =
        native->ibl_sh_slot
            ? native->ibl_sh_slot(native->state,
                                  draw->frame_ibl_probe_sources[i])
            : VKR_SH_SLOT_BLACK;
  }

  vkr_standard_scene_runtime_prepare_shadow_payloads(application, frame);
  packet->shadow = draw->has_shadow ? &draw->shadow_payload : NULL;
  packet->local_shadow = draw->local_shadow_payload.view_count
                             ? &draw->local_shadow_payload
                             : NULL;
  vkr_standard_scene_runtime_scale_picking_payload(&application->renderer,
                                                   draw);
  return true_v;
}

/* Keeps a submitted frame's shadow reuse state, or drops it for a frame that
   did not submit. */
vkr_internal void vkr_standard_scene_runtime_finish_acquired(
    void *state, const VkrFrame *acquired, VkrRendererError error) {
  (void)acquired;
  VkrStandardSceneRuntimeFrame *frame = state;
  VkrStandardSceneRuntime *application = frame->application;
  if (error == VKR_RENDERER_ERROR_NONE) {
    vkr_shadow_system_commit_frame(
        &application->shadow_system,
        vkr_renderer_get_submit_serial(&application->renderer));
  } else {
    vkr_shadow_system_discard_frame(&application->shadow_system);
  }
}

/* Takes the submission serials and device memory the asset pump reads. Call
   only when no frame is rendering. */
vkr_internal void vkr_standard_scene_runtime_sync_submission_state(
    VkrStandardSceneRuntime *application) {
  VkrRenderer *renderer = &application->renderer;
  application->submit_serial_seen = vkr_renderer_get_submit_serial(renderer);
  application->completed_submit_serial_seen =
      vkr_renderer_get_completed_submit_serial(renderer);
  application->device_memory_seen_valid = vkr_renderer_get_device_memory_stats(
      renderer, &application->device_memory_seen);
}

/* Takes the renderer values the frame-loop thread reads while a frame may be
   rendering. Call only when no frame is rendering. */
vkr_internal void vkr_standard_scene_runtime_sync_renderer_state(
    VkrStandardSceneRuntime *application) {
  const VkrRenderer *renderer = &application->renderer;
  application->target_window_width = renderer->last_window_width;
  application->target_window_height = renderer->last_window_height;
  application->target_render_width = renderer->render_width;
  application->target_render_height = renderer->render_height;
  application->target_generation_seen = renderer->target_generation;
  application->frame_number_seen = renderer->frame_number;
  application->display_exposure = renderer->display_exposure;
  application->shadow_depth_range = renderer->timing_result.shadow_depth_range;
  vkr_standard_scene_runtime_sync_submission_state(application);
}

/* Applies a completed frame's result: retained texture relief, editor output,
   picking and capture state, renderer metrics and Scene error recovery.
   Then releases the scratch the packet borrowed. */
vkr_internal void
vkr_standard_scene_runtime_finish_frame(VkrStandardSceneRuntime *application,
                                        VkrStandardSceneRuntimeFrame *frame,
                                        VkrRendererFrameResult *result) {
  const VkrStandardSceneRuntimeDrawContext *draw = &frame->draw;
  vkr_standard_scene_runtime_sync_renderer_state(application);

  /* A decoupled frame the render thread could not acquire, or acquired for
     another target extent, never ran: report it like an acquisition here and
     acquire on this thread next. */
  if (frame->decoupled && (!result->acquired ||
                           result->error == VKR_RENDERER_ERROR_FRAME_SKIPPED)) {
    application->target_known = false_v;
    vkr_standard_scene_runtime_handle_acquire_error(application, result->error);
  } else {
    VkrRendererFrameMetrics *metrics = &result->metrics;
    application->frame_metrics = *metrics;
    if (frame->setup.target_generation != application->last_target_generation &&
        result->acquired) {
      application->last_target_generation = frame->setup.target_generation;
      if (application->ui_system.initialized) {
        vkr_ui_system_resize(&application->ui_system, frame->setup.window_width,
                             frame->setup.window_height);
      }
    }
    vkr_standard_scene_runtime_finish_submit(application, draw, result->error,
                                             metrics);
#if VKR_METRICS_ENABLED
    /* Inline rendering timed the submit call itself; a render thread reports
       the time it spent on the frame, and on acquiring a decoupled one. */
    if (vkr_renderer_render_thread_enabled(&application->renderer)) {
      vkr_metrics_duration_add_ns(application->metrics,
                                  application->metric_ids.render_submit,
                                  result->render_ns);
      if (frame->decoupled) {
        vkr_metrics_duration_add_ns(application->metrics,
                                    application->metric_ids.render_prepare,
                                    result->acquire_ns);
      }
    }
    if (frame->shadow.update) {
      vkr_metrics_duration_add_ns(application->metrics,
                                  application->metric_ids.shadow_update,
                                  frame->shadow.measure_ns + frame->shadow_ns);
    }
    vkr_metrics_counter_add(application->metrics,
                            application->metric_ids.decoupled,
                            frame->decoupled ? 1u : 0u);
    VkrRendererMetricsCollectContext metrics_context = {
        .application = vkr_application_metrics_snapshot(
            &application->assets, &application->ui_system,
            &application->lighting_system, &application->shadow_system),
        .renderer = &application->renderer,
        .frame_metrics = metrics,
        .visibility = &application->visibility_stats,
        .job_system = &application->job_system,
        .cpu_frame_index = frame->packet.frame.frame_index,
        .submit_serial = vkr_renderer_get_submit_serial(&application->renderer),
    };
    vkr_renderer_metrics_collect(&application->renderer_metrics,
                                 &metrics_context);
#endif
    vkr_standard_scene_runtime_handle_scene_error(
        application, draw, result->error, &result->validation);
  }
  application->completed_frame_submit_serial =
      vkr_renderer_get_submit_serial(&application->renderer);
  if (vkr_allocator_scope_is_valid(&frame->scratch_scope)) {
    vkr_allocator_end_scope(&frame->scratch_scope,
                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  frame->scratch_scope = (VkrAllocatorScope){0};
}

/* Collects the submitted frame, waiting for the render thread when it is
   still rendering. Does nothing when no frame awaits completion. */
vkr_internal void vkr_standard_scene_runtime_complete_frame(
    VkrStandardSceneRuntime *application) {
  VkrStandardSceneRuntimeFrame *frame = application->frame_in_flight;
  if (!frame) {
    return;
  }
  application->frame_in_flight = NULL;
  VkrRendererFrameResult result = {0};
  const bool8_t render_thread =
      vkr_renderer_render_thread_enabled(&application->renderer);
  if (render_thread) {
    VKR_METRICS_SCOPE_NS(application->metrics,
                         application->metric_ids.render_wait) {
      vkr_renderer_complete_frame(&application->renderer, &result);
    }
  } else {
    vkr_renderer_complete_frame(&application->renderer, &result);
  }
  vkr_standard_scene_runtime_finish_frame(application, frame, &result);
  /* Inline frames close their metrics frame at the end of the loop
     iteration. A render thread finishes a frame during a later iteration,
     whose remaining work belongs to the next frame. */
  if (render_thread) {
    vkr_metrics_end_frame(application->metrics);
    vkr_metrics_begin_frame(application->metrics,
                            application->renderer.frame_number + 1u,
                            application->completed_frame_submit_serial);
  }
}

/* True when the frame may be built without acquiring it here: the render
   thread renders it while the next frame is built. Frames that resize or
   change the Scene output acquire on this thread, because their build needs
   the new extent. */
vkr_internal bool8_t
vkr_standard_scene_runtime_can_decouple(VkrStandardSceneRuntime *application) {
  if (!vkr_renderer_render_thread_enabled(&application->renderer) ||
      !application->target_known) {
    return false_v;
  }
  VkrStandardSceneRuntimeSceneOutput scene_output = {0};
  return vkr_standard_scene_runtime_scene_output_request(
             application, &scene_output) == VKR_RENDERER_ERROR_NONE &&
         vkr_standard_scene_runtime_scene_output_settled(application,
                                                         &scene_output);
}

void vkr_standard_scene_runtime_draw_frame(VkrStandardSceneRuntime *application,
                                           float64_t delta) {
  assert(application != NULL && "VkrStandardSceneRuntime is NULL");
  assert(bitset8_is_set(&application->host.flags,
                        VKR_APPLICATION_HOST_FLAG_RUNNING) &&
         "VkrStandardSceneRuntime is not running");

  const bool8_t render_thread =
      vkr_renderer_render_thread_enabled(&application->renderer);
  if (!application->frame_in_flight) {
    vkr_standard_scene_runtime_sync_renderer_state(application);
  }
  const uint64_t resize = vkr_atomic_uint64_exchange(
      &application->pending_resize_mailbox, 0u, VKR_MEMORY_ORDER_ACQ_REL);
  if (resize) {
    vkr_renderer_resize(&application->renderer, (uint32_t)(resize >> 32u),
                        (uint32_t)resize);
    application->target_known = false_v;
  }
  application->assets.material_system.texture_stream_memory_recovery_enabled =
      application->editor_viewport.enabled ||
      application->renderer.backend_type == VKR_RENDERER_BACKEND_TYPE_METAL;

  VkrStandardSceneRuntimeFrame *frame = application->frame;
  frame->decoupled = vkr_standard_scene_runtime_can_decouple(application);
  frame->shadow_ns = 0u;
  VkrFrame *setup = &frame->setup;
  *setup = (VkrFrame){0};
  bool8_t target_changed = false_v;
  if (!frame->decoupled) {
    vkr_standard_scene_runtime_complete_frame(application);
    if (!vkr_standard_scene_runtime_begin_scene_frame(application, setup)) {
      return;
    }
    target_changed =
        application->last_target_generation != setup->target_generation;
    if (target_changed) {
      application->last_target_generation = setup->target_generation;
      if (application->ui_system.initialized) {
        vkr_ui_system_resize(&application->ui_system, setup->window_width,
                             setup->window_height);
      }
    }
    application->target_known = true_v;
    application->target_window_width = setup->window_width;
    application->target_window_height = setup->window_height;
    application->target_render_width = setup->render_width;
    application->target_render_height = setup->render_height;
    vkr_standard_scene_runtime_sync_submission_state(application);
  } else {
    /* The render thread acquires this frame; build it for the target the
       last frame used. */
    setup->number = application->frame_number_seen + 1u;
    setup->target_generation = application->last_target_generation;
    setup->window_width = application->target_window_width;
    setup->window_height = application->target_window_height;
    setup->render_width = application->target_render_width;
    setup->render_height = application->target_render_height;
  }
  if (!vkr_standard_scene_runtime_pump_frame_assets(application, setup)) {
    vkr_standard_scene_runtime_cancel_frame(
        application, setup, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    return;
  }
  frame->shadow.config = application->shadow_config;
  /* The tiled pipeline filters every cascade with PCF and renders no
     moments, so a far cascade must not wait for moments to be reused
     (ADR-087). */
  if (application->renderer.graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED) {
    frame->shadow.config.far_cascade_evsm = false_v;
    /* Its dynamic-light limit bounds the shadowed lights, so each takes the
       full filter (ADR-087). */
    frame->shadow.config.local_shadow_full_filter_all = true_v;
  }
  frame->shadow.invalidate_fit =
      application->shadow_fit_invalidate_requested || target_changed;
  application->shadow_fit_invalidate_requested = false_v;

  VkrStandardSceneRuntimeDrawContext *draw = &frame->draw;
  *draw = (VkrStandardSceneRuntimeDrawContext){
      .setup = setup,
      .scratch = application->frame_allocator,
      .target_changed = target_changed,
      .scene_stopped =
          vkr_standard_scene_runtime_editor_scene_rendering_stopped(
              application) ||
          application->editor_viewport.scene_backdrop_blur,
  };
  const VkrRendererError world_error =
      vkr_standard_scene_runtime_build_world_payload(application, draw);
  if (world_error != VKR_RENDERER_ERROR_NONE) {
    vkr_standard_scene_runtime_cancel_frame(application, setup, world_error);
    return;
  }

  vkr_standard_scene_runtime_prepare_picking_payload(application, draw);
  vkr_standard_scene_runtime_prepare_editor_viewport(application, draw);
  vkr_standard_scene_runtime_prepare_picking_source(application, draw);
  draw->active_scene = vkr_standard_scene_runtime_render_scene(application);
  vkr_standard_scene_runtime_prepare_environment(application, draw);

  const VkrRendererError text_error =
      vkr_standard_scene_runtime_prepare_world_text(application, draw);
  if (text_error != VKR_RENDERER_ERROR_NONE) {
    vkr_standard_scene_runtime_cancel_frame(application, setup, text_error);
    return;
  }
  if (!vkr_standard_scene_runtime_prepare_ui_payload(application, draw)) {
    vkr_standard_scene_runtime_cancel_frame(
        application, setup, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    return;
  }

  vkr_standard_scene_runtime_prepare_debug_payload(application, draw);
  vkr_standard_scene_runtime_prepare_frame_lighting(application, draw);

  if (draw->has_editor && !draw->scene_stopped) {
    application->gizmo_system.pixel_scale =
        application->ui_system.content_scale;
    if (vkr_gizmo_system_published(&application->gizmo_system,
                                   &application->assets)) {
      draw->editor_payload.overlay_draw_count = vkr_gizmo_system_build_draws(
          &application->gizmo_system, application->globals.view,
          application->globals.projection, &draw->editor_mapping,
          frame->overlay_draws);
    }
    draw->editor_payload.overlay_draws = frame->overlay_draws;
    vkr_standard_scene_runtime_prepare_selection_outline(application, draw);
  }

  if (application->animation_preview.player && draw->active_scene &&
      application->animation_preview.scene_generation ==
          application->scene_generation) {
    const VkrAnimationPreviewRequest *request = &application->animation_preview;
    draw->has_animation_preview = vkr_scene_animation_build_preview(
        draw->active_scene, request->wrapper, request->player, request->yaw,
        request->pitch, request->distance, draw->scratch, &draw->world_payload,
        &draw->animation_preview);
  }

  if (render_thread && !vkr_standard_scene_runtime_own_frame_data(frame)) {
    vkr_standard_scene_runtime_cancel_frame(
        application, setup, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    return;
  }
  frame->packet =
      vkr_standard_scene_runtime_build_frame_input(application, draw, delta);

  const VkrFrameHooks hooks = {
      .state = frame,
      .prepare = vkr_standard_scene_runtime_prepare_acquired,
      .finish = vkr_standard_scene_runtime_finish_acquired,
  };
  VkrValidationError validation = {0};
  VkrRendererError submit_error = VKR_RENDERER_ERROR_NONE;
  if (frame->decoupled) {
    /* The previous frame finishes only now, after this one was built beside
       it. */
    vkr_standard_scene_runtime_complete_frame(application);
    const VkrFrameConfig config =
        vkr_standard_scene_runtime_frame_config(application);
    submit_error = vkr_renderer_submit_unacquired_frame(
        &application->renderer, &config, &frame->packet, &hooks);
  } else if (render_thread) {
    submit_error =
        vkr_renderer_submit_frame(setup, &frame->packet, &hooks, &validation);
  } else {
    VKR_METRICS_SCOPE_NS(application->metrics,
                         application->metric_ids.render_submit) {
      submit_error =
          vkr_renderer_submit_frame(setup, &frame->packet, &hooks, &validation);
    }
  }
  if (submit_error != VKR_RENDERER_ERROR_NONE) {
    VkrRendererFrameResult result = {
        .error = submit_error,
        .validation = validation,
    };
    vkr_standard_scene_runtime_finish_frame(application, frame, &result);
    return;
  }
  application->frame_in_flight = frame;
  application->frame = frame == application->frames[0] ? application->frames[1]
                                                       : application->frames[0];
  application->frames_submitted++;
  application->frames_decoupled += frame->decoupled ? 1u : 0u;
  if (!render_thread) {
    vkr_standard_scene_runtime_complete_frame(application);
  }
}

/* The tiled pipeline lights a scene's static lights through its lightmap set
   (ADR-087); a scene without a loaded set draws them with its dynamic
   lights, so its lamps show before a bake. */
/* Static lamps leave the runtime light lists once the scene's lightmaps hold
   every lamp group in planes this pipeline samples: all of a set's layers
   on the tiled pipeline, its lamp groups' desktop planes on the desktop
   one (ADR-104). */
vkr_internal bool8_t
vkr_standard_scene_runtime_static_lights_baked(const VkrScene *scene) {
  return scene->lightmaps && scene->lightmaps->texture.id != 0u &&
         scene->lightmaps->lamps_baked;
}

vkr_internal bool8_t vkr_standard_scene_runtime_host_frame(
    void *state, const VkrApplicationHostFrame *host_frame) {
  VkrStandardSceneRuntime *application = state;
  const float64_t delta = host_frame->delta_seconds;
  const float64_t current_absolute_time = host_frame->absolute_time_seconds;

  /* A metrics frame describes one rendered frame. With a render thread it
     stays open until that frame completes, then the next one opens there. */
  if (!application->metrics->frame_active) {
    vkr_metrics_begin_frame(
        application->metrics, application->renderer.frame_number + 1u,
        vkr_renderer_get_submit_serial(&application->renderer));
  }

  VkrAllocatorScope frame_scope = {0};
  VkrAllocator *frame_alloc = application->frame_allocator;
  if (vkr_allocator_supports_scopes(frame_alloc))
    frame_scope = vkr_allocator_begin_scope(frame_alloc);
  application->world_text_update_count = 0;

  VKR_METRICS_SCOPE_NS(application->metrics, application->metric_ids.update) {
    if (application->callbacks.update)
      application->callbacks.update(application->callbacks.state, application,
                                    delta);
  }

  if (!bitset8_is_set(&application->host.flags,
                      VKR_APPLICATION_HOST_FLAG_RUNNING) ||
      !bitset8_is_set(&application->host.flags,
                      VKR_APPLICATION_HOST_FLAG_INITIALIZED)) {
    if (vkr_allocator_scope_is_valid(&frame_scope))
      vkr_allocator_end_scope(&frame_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  VkrCameraSystem *camera_system = &application->camera_system;
  VkrCameraHandle active_camera = vkr_camera_registry_get_active(camera_system);
  application->active_camera = active_camera;
  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(camera_system, active_camera);

  if (camera) {
    application->camera_controller.camera = camera;
  } else {
    log_warn("Active camera handle invalid; skipping controller update");
  }

  if (camera &&
      !vkr_standard_scene_runtime_editor_scene_rendering_stopped(application) &&
      !application->config->disable_camera_controller &&
      !application->editor_viewport.scripts_own_camera) {
    vkr_camera_controller_update(&application->camera_controller, delta,
                                 application->ui_capture.mouse ||
                                     application->ui_capture.keyboard);
  }

  vkr_camera_registry_update_all(camera_system);

  /* The active scene resolves its world with the root World as fallback;
     with no scene loaded the World renders alone and owns the sky products
     (ADR-076). */
  VkrScene *render_scene = vkr_standard_scene_runtime_render_scene(application);
  if (application->active_scene) {
    vkr_scene_set_world_fallback(application->active_scene,
                                 application->world_scene);
  }
  if (render_scene) {
    (void)vkr_scene_resolve_world(render_scene);
    vkr_scene_sync_sun(render_scene, delta);
    /* The volume's sun keys and lamp groups follow the turned sun and the
       light group factors (ADR-090). */
    vkr_scene_update_diffuse_volume(render_scene, delta);
    (void)vkr_scene_bind_lightmaps(render_scene);
    (void)vkr_world_resources_prepare_scene_atmosphere(
        &application->assets, &application->assets.world_resources,
        render_scene);
    vkr_world_resources_poll_scene_atmosphere(&application->assets,
                                              render_scene);
    vkr_standard_scene_runtime_advance_cloud_wind(application, delta);
    const bool8_t tiled =
        application->renderer.graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED;
    application->lighting_system.static_lights_baked =
        vkr_standard_scene_runtime_static_lights_baked(render_scene);
    vkr_lighting_system_sync_from_scene(&application->lighting_system,
                                        render_scene);
    /* Additive scenes' light groups follow the rendered scene's sun and
       time of day (ADR-090). */
    for (uint32_t i = 0; i < application->additive_count; ++i) {
      vkr_scene_update_light_groups(
          application->additive_scenes[i], render_scene->clock.night,
          render_scene->world_state.time_of_day.night_groups);
      application->lighting_system.static_lights_baked =
          vkr_standard_scene_runtime_static_lights_baked(
              application->additive_scenes[i]);
      vkr_lighting_system_append_scene(&application->lighting_system,
                                       application->additive_scenes[i]);
    }
    if (render_scene->atmosphere.active_revision &&
        render_scene->atmosphere.active_settings.enabled) {
      const VkrSceneKeyLight key =
          vkr_scene_atmosphere_frame_key_light(render_scene);
      vkr_lighting_system_apply_atmosphere_light(
          &application->lighting_system, key.toward, key.irradiance,
          key.angular_diameter_degrees, application->display_exposure);
    }
    /* It draws a bounded set of the dynamic lights nearest the camera, and
       shadows fewer still. */
    if (tiled && camera)
      vkr_lighting_system_limit_point_lights(
          &application->lighting_system, camera->position, (float32_t)delta,
          VKR_STANDARD_SCENE_TILED_LIGHT_MAX,
          VKR_STANDARD_SCENE_TILED_SHADOWED_LIGHT_MAX);
  }

  /* The frame fits and resolves shadows once acquired, on the thread that
     renders it; capture what it needs from this frame's state now. */
  VkrStandardSceneRuntimeShadowInput *shadow = &application->frame->shadow;
  *shadow = (VkrStandardSceneRuntimeShadowInput){
      .light_enabled = application->lighting_system.directional.enabled &&
                       !application->disable_directional_shadows,
      .light_direction = application->lighting_system.directional.direction,
      .sun_angular_diameter_degrees =
          application->lighting_system.directional.sun_angular_diameter_degrees,
      .depth_range = application->shadow_depth_range,
      .frame_index = application->frame_number_seen,
      .scene_generation = application->scene_generation,
      .directional_disabled = application->disable_directional_shadows,
      .local_disabled = application->disable_local_shadows,
      .soft_disabled = application->disable_soft_shadows,
      .delta = delta,
  };
  if (camera &&
      !vkr_standard_scene_runtime_editor_scene_rendering_stopped(application)) {
    const float64_t measure_start = vkr_platform_get_absolute_time();
    vkr_scene_measure_caster_bounds(&application->assets.mesh_manager,
                                    &shadow->caster_bounds);
    shadow->camera = *camera;
    shadow->update = true_v;
    shadow->measure_ns =
        (uint64_t)((vkr_platform_get_absolute_time() - measure_start) *
                   1000000000.0);
  }

  if (camera) {
    // update_all() refreshed these cached matrices above.
    application->globals.view = camera->view;
    application->globals.projection = camera->projection;
    application->globals.view_position = camera->position;
  } else {
    application->globals.view = mat4_identity();
    application->globals.projection = mat4_identity();
  }

  uint32_t mesh_capacity =
      vkr_mesh_manager_capacity(&application->assets.mesh_manager);
  for (uint32_t mesh_index = 0; mesh_index < mesh_capacity; ++mesh_index) {
    VkrMesh *mesh =
        vkr_mesh_manager_get(&application->assets.mesh_manager, mesh_index);
    if (!mesh) {
      continue;
    }

    // Scene-driven meshes update their model via the scene bridge; avoid
    // overwriting those transforms with the mesh-local transform.
    if (mesh->render_id != 0) {
      continue;
    }

    vkr_mesh_manager_update_model(&application->assets.mesh_manager,
                                  mesh_index);
  }

  if (!bitset8_is_set(&application->host.flags,
                      VKR_APPLICATION_HOST_FLAG_RUNNING) ||
      !bitset8_is_set(&application->host.flags,
                      VKR_APPLICATION_HOST_FLAG_INITIALIZED)) {
    if (vkr_allocator_scope_is_valid(&frame_scope)) {
      vkr_allocator_end_scope(&frame_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    // Publish before leaving: this is the shutdown path an auto-closing run
    // takes, and dropping it would make the final snapshot one frame stale.
    vkr_metrics_end_frame(application->metrics);
    return false_v;
  }

  vkr_standard_scene_runtime_draw_frame(application, delta);
  /* A frame still rendering keeps its scratch until it completes; the next
     frame takes the other arena. An older frame still in flight already
     holds its own scope. */
  VkrStandardSceneRuntimeFrame *in_flight = application->frame_in_flight;
  if (in_flight && !vkr_allocator_scope_is_valid(&in_flight->scratch_scope)) {
    in_flight->scratch_scope = frame_scope;
    frame_scope = (VkrAllocatorScope){0};
    application->frame_allocator =
        application->frame_allocator == &application->frame_allocators[0]
            ? &application->frame_allocators[1]
            : &application->frame_allocators[0];
  }

  VKR_METRICS_ADD_ELAPSED_NS(application->metrics,
                             application->metric_ids.frame_work,
                             current_absolute_time);

  if (vkr_allocator_scope_is_valid(&frame_scope))
    vkr_allocator_end_scope(&frame_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return true_v;
}

vkr_internal void vkr_standard_scene_runtime_host_sleep(void *state,
                                                        uint64_t milliseconds) {
  VkrStandardSceneRuntime *application = state;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.limiter_sleep) {
    vkr_platform_sleep(milliseconds);
  }
}

vkr_internal void vkr_standard_scene_runtime_host_frame_complete(
    void *state, const VkrApplicationHostFrame *host_frame) {
  VkrStandardSceneRuntime *application = state;
  VKR_METRICS_ADD_ELAPSED_NS(application->metrics,
                             application->metric_ids.frame_wall,
                             host_frame->absolute_time_seconds);
  /* A frame still rendering closes its metrics frame when it completes. */
  if (!application->frame_in_flight) {
    vkr_metrics_end_frame(application->metrics);
  }
}

vkr_internal bool8_t vkr_standard_scene_runtime_host_event(void *state,
                                                           Event *event) {
  VkrStandardSceneRuntime *application = state;
  if (!application->callbacks.event)
    return true_v;
  return application->callbacks.event(application->callbacks.state, event);
}

void vkr_standard_scene_runtime_set_callbacks(
    VkrStandardSceneRuntime *application,
    const VkrStandardSceneRuntimeCallbacks *callbacks) {
  assert(application && callbacks);
  application->callbacks = *callbacks;
}

void vkr_standard_scene_runtime_run(VkrStandardSceneRuntime *application) {
  assert(application);
  const VkrApplicationHostCallbacks callbacks = {
      .state = application,
      .frame = vkr_standard_scene_runtime_host_frame,
      .sleep = vkr_standard_scene_runtime_host_sleep,
      .frame_complete = vkr_standard_scene_runtime_host_frame_complete,
      .event = vkr_standard_scene_runtime_host_event,
  };
  vkr_application_host_set_callbacks(&application->host, &callbacks);
  vkr_application_host_run(&application->host);
}

/**
 * @brief Stops or suspends the application's main loop.
 * Sets the `VKR_APPLICATION_HOST_FLAG_SUSPENDED` flag, causing the main loop in
 * `vkr_standard_scene_runtime_start` to pause processing application updates.
 * Dispatches an `EVENT_TYPE_APPLICATION_STOP` event.
 * Asserts that the application is currently running.
 * @param application Pointer to the `VkrStandardSceneRuntime` structure.
 */
void vkr_standard_scene_runtime_stop(VkrStandardSceneRuntime *application) {
  vkr_application_host_stop(&application->host);
}

/**
 * @brief Resumes a previously stopped or suspended application.
 * Clears the `VKR_APPLICATION_HOST_FLAG_SUSPENDED` flag, allowing the main loop
 * in `vkr_standard_scene_runtime_start` to continue processing updates.
 * Dispatches an `EVENT_TYPE_APPLICATION_RESUME` event.
 * Asserts that the application is currently suspended.
 * @param application Pointer to the `VkrStandardSceneRuntime` structure.
 */
void vkr_standard_scene_runtime_resume(VkrStandardSceneRuntime *application) {
  vkr_application_host_resume(&application->host);
}

/**
 * @brief Signals the application's main loop to terminate.
 * Clears the `VKR_APPLICATION_HOST_FLAG_RUNNING` flag, which will cause the
 * `while` condition in `vkr_standard_scene_runtime_start` to become false,
 * leading to loop exit. This call is idempotent to support shutdown paths that
 * may request close from both update-time logic and post-loop teardown.
 * @param application Pointer to the `VkrStandardSceneRuntime` structure.
 */
void vkr_standard_scene_runtime_close(VkrStandardSceneRuntime *application) {
  vkr_application_host_close(&application->host);
}

/**
 * @brief Shuts down the application and releases all associated resources.
 * This function should be called after the main loop has terminated (e.g.,
 * after `vkr_standard_scene_runtime_start` returns). It dispatches an
 * `EVENT_TYPE_APPLICATION_SHUTDOWN` event, then destroys the window, event
 * manager, and all application-specific memory arenas. Asserts that the
 * application is not still marked as running.
 * @param application Pointer to the `VkrStandardSceneRuntime` structure to be
 * shut down.
 */
void vkr_standard_scene_runtime_shutdown(VkrStandardSceneRuntime *application) {
  assert_log(application != NULL, "VkrStandardSceneRuntime is NULL");
  assert_log(!bitset8_is_set(&application->host.flags,
                             VKR_APPLICATION_HOST_FLAG_RUNNING),
             "VkrStandardSceneRuntime is still running");

  log_info("VkrStandardSceneRuntime shutting down...");
  if (vkr_renderer_render_thread_enabled(&application->renderer)) {
    log_info("Render thread acquired %llu of %llu submitted frames",
             (unsigned long long)application->frames_decoupled,
             (unsigned long long)application->frames_submitted);
  }
  /* A frame submitted just before the loop stopped has no frame left to apply
     its result to. */
  if (application->frame_in_flight) {
    vkr_renderer_complete_frame(&application->renderer, NULL);
    application->frame_in_flight = NULL;
  }
  vkr_application_host_shutdown(&application->host);

  if (vkr_renderer_wait_idle(&application->renderer) !=
      VKR_RENDERER_ERROR_NONE) {
    log_warn("Failed to wait for renderer to be idle");
  }

  /*
   * Resource async workers call loader prepare/finalize callbacks that use
   * renderer-owned async allocators (texture/material/mesh/scene). Join worker
   * threads before renderer teardown so those allocators remain valid for the
   * entire worker lifetime. Physics lets go of the workers first.
   */
  vkr_physics_set_jobs(NULL);
  vkr_job_system_shutdown(&application->job_system);

  vkr_standard_scene_runtime_rendering_shutdown(application);
  vkr_renderer_destroy(&application->renderer);
  vkr_application_host_destroy(&application->host);

  vkr_allocator_release_global_accounting(&application->metrics_allocator);
  arena_destroy(application->metrics_arena);
  application->metrics_arena = NULL;
  application->metrics = NULL;

  vkr_allocator_release_global_accounting(&application->app_allocator);
  log_shutdown();
  arena_destroy(application->log_arena);
  arena_destroy(application->app_arena);
}

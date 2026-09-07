
#include "application/vkr_standard_scene_runtime.h"

#include "platform/vkr_platform.h"

#include <assert.h>
#include <stdio.h>

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
      !vkr_renderer_metrics_register(&application->renderer_metrics,
                                     application->metrics)) {
    return false_v;
  }
  return true_v;
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
  application->frame_arena = arena_create(MB(32), MB(1));
  if (!application->frame_arena)
    return false_v;
  application->frame_allocator =
      (VkrAllocator){.ctx = application->frame_arena};
  if (!vkr_allocator_arena(&application->frame_allocator))
    return false_v;
  VkrDeviceInformation device = {0};
  vkr_renderer_get_device_information(&application->renderer, &device,
                                      application->frame_arena);
  if (!vkr_render_assets_initialize(
          &application->assets, &application->renderer.asset_publisher, &device,
          &application->job_system, metrics_producers))
    return false_v;
  VkrCameraSystemConfig camera_config = {.max_camera_count = 24u};
  if (!vkr_camera_registry_init(&camera_config, &application->camera_system) ||
      !vkr_camera_registry_create_perspective(
          &application->camera_system, string8_lit("camera.default"),
          vkr_standard_scene_runtime_is_windowed(application)
              ? &application->host.window
              : NULL,
          70.0f, 0.1f, 500.0f, &application->active_camera))
    return false_v;
  vkr_camera_registry_set_active(&application->camera_system,
                                 application->active_camera);
  VkrCamera *camera = vkr_camera_registry_get_by_handle(
      &application->camera_system, application->active_camera);
  uint32_t width = 0u, height = 0u;
  vkr_renderer_present_target_extent(&application->renderer, &width, &height);
  if (!camera || !vkr_camera_set_perspective_lens(camera, 70.0f, 0.1f, 500.0f,
                                                  width, height))
    return false_v;
  vkr_camera_system_update(camera);
  if (!vkr_lighting_system_init(&application->lighting_system))
    return false_v;
  VkrShadowConfig shadow_config = VKR_SHADOW_CONFIG_DEFAULT;
  if (!vkr_shadow_system_init(&application->shadow_system, &shadow_config))
    return false_v;
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
      .bloom_enabled = !application->renderer.bloom_forced_disabled,
      .bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD,
      .bloom_knee = VKR_BLOOM_DEFAULT_KNEE,
      .bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY,
      .gtao_enabled = !application->renderer.gtao_forced_disabled,
      .gtao_radius = VKR_GTAO_DEFAULT_RADIUS,
      .gtao_power = VKR_GTAO_DEFAULT_POWER,
      .image_sharpness = 0.25f,
      .render_mode = VKR_RENDER_MODE_DEFAULT,
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
  vkr_allocator_release_global_accounting(&application->frame_allocator);
  arena_destroy(application->frame_arena);
  application->frame_arena = NULL;
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
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  const VkrRendererMetricsProducerConfig *metrics_producers =
      vkr_renderer_metrics_get_producers(&application->renderer_metrics);
  VkrRendererBackendConfig backend_cfg = {
      .application_name = "vulkan_renderer",
      .present_target = config->present_target,
      .requested_present_mode = config->requested_present_mode,
      .render_scale = config->render_scale,
      .upscale_mode = config->upscale_mode,
      .dynamic_resolution = config->dynamic_resolution,
      .capture_enabled = config->capture_enabled,
      .capture_ring_capacity = config->capture_ring_capacity,
      .capture_max_batch_bytes = config->capture_max_batch_bytes,
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
  if (jobs_ready)
    vkr_job_system_shutdown(&application->job_system);
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
  if (!application->editor_viewport.scene_only) {
    float32_t content_scale = 1.0f;
    if (vkr_standard_scene_runtime_is_windowed(application)) {
      const VkrWindowContentScale scale =
          vkr_window_get_content_scale(&application->host.window);
      if (isfinite(scale.value) && scale.value > 0.0f)
        content_scale = scale.value;
    }
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
  if (application->editor_viewport.scene_only)
    return false_v;
  VkrUiRect content = {0};
  return !vkr_ui_dock_find_panel(&application->editor_viewport.dock,
                                 VKR_UI_DOCK_PANEL_SCENE_VIEWPORT, NULL,
                                 &content) ||
         !vkr_ui_rect_has_area(content);
}

bool8_t vkr_standard_scene_runtime_editor_viewport_mapping(
    VkrStandardSceneRuntime *application, uint32_t window_width,
    uint32_t window_height, VkrViewportMapping *out_mapping) {
  Vec4 panel = {0};
  if (!out_mapping || !vkr_standard_scene_runtime_editor_viewport_panel_rect(
                          application, window_width, window_height, &panel))
    return false_v;
  const bool8_t renderer_scaled_scene =
      application->renderer.scene_output_extent_overridden ||
      application->renderer.render_scale != 1.0f ||
      application->renderer.upscale_mode == VKR_UPSCALE_MODE_METALFX_TEMPORAL ||
      application->renderer.upscale_mode == VKR_UPSCALE_MODE_FSR31;
  if (renderer_scaled_scene && application->renderer.render_width > 0u &&
      application->renderer.render_height > 0u) {
    return vkr_editor_viewport_mapping_from_panel_rect_and_target(
        panel, application->editor_viewport.fit_mode,
        application->renderer.render_width, application->renderer.render_height,
        out_mapping);
  }
  return vkr_editor_viewport_mapping_from_panel_rect(
      panel, application->editor_viewport.fit_mode,
      application->editor_viewport.render_scale *
          application->scene_output_scale,
      out_mapping);
}

vkr_internal VkrRendererError vkr_standard_scene_runtime_configure_scene_output(
    VkrStandardSceneRuntime *application) {
  if (!application)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
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
  if (paneled && application->renderer.render_scale == 1.0f &&
      application->renderer.upscale_mode != VKR_UPSCALE_MODE_METALFX_TEMPORAL &&
      application->renderer.upscale_mode != VKR_UPSCALE_MODE_FSR31 &&
      application->scene_output_scale == 1.0f)
    return vkr_renderer_restore_scene_output_extent(&application->renderer);
  if (!paneled && application->scene_output_scale == 1.0f)
    return vkr_renderer_restore_scene_output_extent(&application->renderer);

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
          : (VkrWindowPixelSize){application->renderer.last_window_width,
                                 application->renderer.last_window_height};
  Vec4 panel = {0, 0, (float32_t)pixels.width, (float32_t)pixels.height};
  if (paneled && !vkr_standard_scene_runtime_editor_viewport_panel_rect(
                     application, pixels.width, pixels.height, &panel))
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  const float32_t scale = application->scene_output_scale;
  const uint32_t width =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel.z * scale));
  const uint32_t height =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel.w * scale));
  return vkr_renderer_set_scene_output_extent(
      &application->renderer, width, height,
      application->scene_memory_relief_generation !=
          application->assets.material_system.texture_stream_relief_generation);
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

/* Release the acquired target and discard unsubmitted shadow reuse state. */
vkr_internal void
vkr_standard_scene_runtime_cancel_frame(VkrStandardSceneRuntime *application,
                                        VkrFrame *frame,
                                        VkrRendererError error) {
  const VkrRendererError cancel_error = vkr_renderer_cancel_frame(frame);
  vkr_shadow_system_discard_frame(&application->shadow_system);
  application->last_renderer_error =
      cancel_error != VKR_RENDERER_ERROR_NONE ? cancel_error : error;
  String8 message =
      vkr_renderer_get_error_string(application->last_renderer_error);
  log_error("Failed to prepare scene frame: %s", string8_cstr(&message));
  if (application->last_renderer_error == VKR_RENDERER_ERROR_DEVICE_ERROR)
    vkr_application_host_close(&application->host);
}

void vkr_standard_scene_runtime_draw_frame(VkrStandardSceneRuntime *application,
                                           float64_t delta) {
  assert(application != NULL && "VkrStandardSceneRuntime is NULL");
  assert(bitset8_is_set(&application->host.flags,
                        VKR_APPLICATION_HOST_FLAG_RUNNING) &&
         "VkrStandardSceneRuntime is not running");

  const uint64_t resize = vkr_atomic_uint64_exchange(
      &application->pending_resize_mailbox, 0u, VKR_MEMORY_ORDER_ACQ_REL);
  if (resize)
    vkr_renderer_resize(&application->renderer, (uint32_t)(resize >> 32u),
                        (uint32_t)resize);
  VkrFrame setup = {0};
  const VkrFrameConfig frame_config = {
      .shadow_map_size = application->shadow_system.initialized
                             ? vkr_shadow_config_get_max_map_size(
                                   &application->shadow_system.config)
                             : 2048u,
      .shadow_cascade_count =
          application->shadow_system.initialized
              ? application->shadow_system.config.cascade_count
              : 1u,
  };
  application->assets.material_system.texture_stream_memory_recovery_enabled =
      application->editor_viewport.enabled ||
      application->renderer.backend_type == VKR_RENDERER_BACKEND_TYPE_METAL;
  VkrRendererError prepare_err = VKR_RENDERER_ERROR_NONE;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.render_prepare) {
    prepare_err =
        vkr_standard_scene_runtime_configure_scene_output(application);
    if (prepare_err == VKR_RENDERER_ERROR_NONE)
      prepare_err = vkr_renderer_begin_frame(&application->renderer,
                                             &frame_config, &setup);
  }
  application->last_renderer_error = prepare_err;
  if (prepare_err != VKR_RENDERER_ERROR_NONE) {
    // A minimized or resizing window skips frames as a matter of course; this
    // path runs every tick while minimized, so it must not log.
    if (prepare_err != VKR_RENDERER_ERROR_FRAME_SKIPPED) {
      String8 err = vkr_renderer_get_error_string(prepare_err);
      log_error("Failed to prepare renderer frame: %s", string8_cstr(&err));
      const bool8_t reduced =
          vkr_standard_scene_runtime_reduce_scene_resolution(application,
                                                             prepare_err);
      if (!reduced && application->editor_viewport.enabled &&
          !vkr_standard_scene_runtime_editor_scene_rendering_stopped(
              application) &&
          prepare_err != VKR_RENDERER_ERROR_DEVICE_ERROR &&
          prepare_err != VKR_RENDERER_ERROR_CAPTURE_BUSY &&
          prepare_err != VKR_RENDERER_ERROR_RESOURCE_BUSY) {
        application->editor_viewport.scene_error = prepare_err;
        application->editor_viewport.scene_rendering_stopped = true_v;
        vkr_picking_cancel(&application->picking);
        vkr_window_set_mouse_capture(&application->host.window, false_v);
      }
      if (!application->editor_viewport.enabled && !reduced &&
          prepare_err == VKR_RENDERER_ERROR_OUT_OF_MEMORY) {
        log_error("Scene memory recovery exhausted; stopping application");
        vkr_application_host_close(&application->host);
      }
      if (prepare_err == VKR_RENDERER_ERROR_DEVICE_ERROR) {
        log_fatal("Renderer device is unusable; stopping");
        vkr_application_host_close(&application->host);
      }
    }
    return;
  }

  const bool8_t target_changed =
      application->last_target_generation != setup.target_generation;
  if (target_changed) {
    application->last_target_generation = setup.target_generation;
    if (application->ui_system.initialized)
      vkr_ui_system_resize(&application->ui_system, setup.window_width,
                           setup.window_height);
    vkr_shadow_system_invalidate_fit_history(&application->shadow_system);
  }
  const VkrResourceSubmissionState submission = {
      .submit_serial = vkr_renderer_get_submit_serial(&application->renderer),
      .completed_submit_serial =
          vkr_renderer_get_completed_submit_serial(&application->renderer),
      .frame_active = true_v,
  };
  if (!vkr_render_assets_pump_publications(&application->assets, submission)) {
    vkr_standard_scene_runtime_cancel_frame(
        application, &setup, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    return;
  }
  VkrMaterialSystem *materials = &application->assets.material_system;
  if (!materials->texture_stream_budget_user_configured &&
      (materials->texture_stream_active_count ||
       setup.number - 1u >= application->texture_memory_sample_frame + 60u)) {
    VkrDeviceMemoryStats device_memory = {0};
    if (vkr_renderer_get_device_memory_stats(&application->renderer,
                                             &device_memory)) {
      vkr_render_assets_refresh_texture_residency_budget(&application->assets,
                                                         &device_memory);
      application->texture_memory_sample_frame = setup.number - 1u;
    }
  }
  vkr_material_system_pump_texture_streams(materials, 32u);
  VkrAllocator *scratch = &application->frame_allocator;

  const bool8_t scene_stopped =
      vkr_standard_scene_runtime_editor_scene_rendering_stopped(application);
  VkrShadowFrameData shadow_frame = {0};
  uint32_t shadow_cascade_count = 0;

  VkrWorldPassPayload world_payload = {0};
  VkrVisibilityStats visibility_stats = {0};
  const VkrAssetPublisher *publisher = &application->renderer.asset_publisher;
  VkrRendererError world_error = VKR_RENDERER_ERROR_NONE;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.world_payload_build) {
    if (!scene_stopped)
      world_error = vkr_scene_build_world_draws(
          &application->assets.mesh_manager,
          &application->assets.material_system,
          !publisher->publications_idle ||
              !publisher->publications_idle(publisher->state),
          publisher->publication_generation
              ? publisher->publication_generation(publisher->state)
              : 1u,
          application->globals.view, application->globals.projection, scratch,
          &world_payload, &visibility_stats);
  }
  if (world_error != VKR_RENDERER_ERROR_NONE) {
    vkr_standard_scene_runtime_cancel_frame(application, &setup, world_error);
    return;
  }
  vkr_material_system_refresh_texture_stream_demand(
      &application->assets.material_system);
  application->visibility_stats = visibility_stats;

  if (world_payload.gpu_shadow_candidate_count > 0u &&
      application->shadow_system.initialized) {
    vkr_shadow_system_resolve_frame(
        &application->shadow_system, setup.image_index, setup.retained_shadow,
        &world_payload,
        vkr_renderer_get_shadow_depth_format(&application->renderer),
        &shadow_frame);
    shadow_cascade_count =
        shadow_frame.enabled ? shadow_frame.cascade_count : 0u;
  } else {
    vkr_shadow_system_discard_frame(&application->shadow_system);
  }

  VkrLocalShadowPassPayload local_shadow_payload = {0};
  if (!scene_stopped && world_payload.gpu_shadow_candidate_count > 0u &&
      application->shadow_system.initialized) {
    vkr_local_shadow_prepare(
        application->lighting_system.point_lights,
        application->lighting_system.point_light_count,
        application->shadow_system.config.local_shadow_face_budget,
        application->shadow_system.config.local_shadow_map_size,
        &local_shadow_payload);
  }
  VkrShadowPassPayload shadow_payload = {0};
  /* Raster depth bias, distinct from receiver bias. Lowered from the shadow
     config so both selected implementations apply the same configured values
     instead of one backend hardcoding defaults and the other applying none.
     Lives in the frame scope because the payload borrows it until submit
     returns. */
  VkrShadowConfigOverride raster_bias_override = {0};
  bool8_t has_shadow = false_v;
  if (shadow_cascade_count > 0) {
    const VkrShadowConfig *shadow_config = &application->shadow_system.config;
    const float32_t inverse_map_size =
        1.0f / (float32_t)shadow_config->shadow_map_size;
    shadow_payload.cascade_count = shadow_cascade_count;
    shadow_payload.sdsm_enabled = shadow_config->sdsm_enabled;
    shadow_payload.cascade_render_mask = shadow_frame.cascade_render_mask;
    for (uint32_t i = 0; i < shadow_cascade_count; ++i) {
      shadow_payload.cascades[i] = (VkrShadowCascadePacketData){
          .light_view_projection = shadow_frame.view_projection[i],
          .split_near_far_texel_depth =
              {shadow_frame.split_near[i], shadow_frame.split_far[i],
               shadow_frame.world_units_per_texel[i],
               shadow_frame.light_space_depth_span[i]},
          .origin_inv_size_pad = {shadow_frame.light_space_origin[i].x,
                                  shadow_frame.light_space_origin[i].y,
                                  inverse_map_size, 0.0f},
      };
    }
    /* The fade ends at the *resolved* last split, not at
       `max_shadow_distance`. The split loop clamps the far split to the
       camera's far plane as well, so a camera closer than the configured
       distance would otherwise leave the band unfinished and restore the hard
       terminating edge the fade exists to remove. */
    const float32_t last_split =
        shadow_frame.split_far[shadow_cascade_count - 1u];
    const float32_t fade_start = vkr_max_f32(
        last_split - shadow_config->shadow_distance_fade_range, 0.0f);
    shadow_payload.receiver = (VkrShadowReceiverPacketData){
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
    raster_bias_override = (VkrShadowConfigOverride){
        .depth_bias_constant = shadow_config->depth_bias_constant_factor,
        .depth_bias_slope = shadow_config->depth_bias_slope_factor,
        .depth_bias_clamp = shadow_config->depth_bias_clamp,
    };
    shadow_payload.config_override = &raster_bias_override;
    has_shadow = world_payload.gpu_shadow_candidate_count > 0u;
  }

  VkrPickingPassPayload picking_payload = {0};
  bool8_t has_picking = !scene_stopped && application->picking.state ==
                                              VKR_PICKING_STATE_RENDER_PENDING;
  /* An identifier capture has no producer unless the picking pass runs this
     frame, so the request itself schedules it. The catalog names the dependency
     as a subsystem; matching on the channel name instead would silently stop
     working the moment a channel is renamed. */
  if (!scene_stopped && !has_picking && application->capture_request) {
    for (uint32_t i = 0; i < application->capture_request->item_count; ++i) {
      const VkrCaptureChannelDescription *channel =
          vkr_renderer_capture_channel_get(
              application->capture_request->items[i].channel);
      if (channel && channel->required_feature == VKR_CAPTURE_FEATURE_PICKING) {
        has_picking = true_v;
        break;
      }
    }
  }
  if (has_picking) {
    picking_payload.pending = true_v;
    picking_payload.x = application->picking.requested_x;
    picking_payload.y = application->picking.requested_y;
    picking_payload.request_id =
        application->picking.state == VKR_PICKING_STATE_RENDER_PENDING
            ? application->picking.request_id
            : 0u;
  }

  bool8_t editor_enabled =
      application->editor_viewport.enabled &&
      vkr_subsystem_plan_includes(&application->subsystem_plan,
                                  VKR_RENDERER_SUBSYSTEM_EDITOR);
  bool8_t has_editor = false_v;
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;
  VkrViewportMapping editor_mapping = {0};
  VkrEditorPassPayload editor_payload = {0};

  if (editor_enabled) {
    if (vkr_standard_scene_runtime_editor_viewport_mapping(
            application, setup.window_width, setup.window_height,
            &editor_mapping) &&
        vkr_editor_viewport_build_payload(&editor_mapping, &editor_payload)) {
      editor_payload.scene_rendering_stopped = scene_stopped;
      viewport_width = editor_mapping.target_width;
      viewport_height = editor_mapping.target_height;
      has_editor = true_v;
    } else {
      editor_enabled = false_v;
    }
  }

  if (editor_enabled) {
    if (!scene_stopped &&
        (viewport_width != application->editor_viewport.last_target_width ||
         viewport_height != application->editor_viewport.last_target_height)) {
      vkr_camera_registry_resize_all(&application->camera_system,
                                     viewport_width, viewport_height);
      application->editor_viewport.last_target_width = viewport_width;
      application->editor_viewport.last_target_height = viewport_height;
    }
  } else if (target_changed ||
             application->editor_viewport.last_target_width != 0 ||
             application->editor_viewport.last_target_height != 0) {
    vkr_camera_registry_resize_all(&application->camera_system,
                                   setup.window_width, setup.window_height);
    application->editor_viewport.last_target_width = 0;
    application->editor_viewport.last_target_height = 0;
  }

  if (has_picking && (application->renderer.scene_output_extent_overridden ||
                      application->renderer.render_scale != 1.0f ||
                      application->renderer.upscale_mode ==
                          VKR_UPSCALE_MODE_METALFX_TEMPORAL)) {
    const uint32_t source_width =
        editor_enabled ? application->picking.width : setup.window_width;
    const uint32_t source_height =
        editor_enabled ? application->picking.height : setup.window_height;
    picking_payload.x = vkr_standard_scene_runtime_picking_pixel(
        picking_payload.x, source_width, setup.render_width);
    picking_payload.y = vkr_standard_scene_runtime_picking_pixel(
        picking_payload.y, source_height, setup.render_height);
  }
  VkrUiPassPayload ui_payload = {0};
  const VkrScene *active_scene = application->active_scene;
  VkrSkyboxPassPayload skybox_payload = {
      .cubemap = VKR_TEXTURE_HANDLE_INVALID,
      .material = VKR_MATERIAL_HANDLE_INVALID,
  };
  const bool8_t scene_environment_ready =
      active_scene && active_scene->environment.enabled &&
      active_scene->environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_READY;
  const VkrSceneEnvironment *scene_environment =
      scene_environment_ready ? &active_scene->environment : NULL;
  VkrTextureHandle frame_ibl_source = VKR_TEXTURE_HANDLE_INVALID;
  if (scene_environment) {
    frame_ibl_source = scene_environment->source_cubemap;
  } else if (application->assets.world_resources.ibl_default_ready) {
    frame_ibl_source =
        application->assets.world_resources.ibl_fallback_source_cubemap;
  }
  skybox_payload.cubemap = application->skybox_system.initialized
                               ? frame_ibl_source
                               : VKR_TEXTURE_HANDLE_INVALID;
  bool8_t frame_ibl_enabled = frame_ibl_source.id != 0;
  float32_t frame_ibl_intensity = 1.0f;
  float32_t frame_ibl_diffuse_intensity = 1.0f;
  float32_t frame_ibl_specular_intensity = 1.0f;
  if (scene_environment) {
    frame_ibl_enabled = true_v;
    frame_ibl_intensity = scene_environment->intensity;
    frame_ibl_diffuse_intensity = scene_environment->diffuse_intensity;
    frame_ibl_specular_intensity = scene_environment->specular_intensity;
  }

  VkrWorldResources *world_resources = &application->assets.world_resources;
  if (application->world_text_update_count >
      VKR_STANDARD_SCENE_RUNTIME_MAX_PENDING_TEXT_UPDATES) {
    vkr_standard_scene_runtime_cancel_frame(
        application, &setup, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT);
    return;
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
      vkr_standard_scene_runtime_cancel_frame(
          application, &setup, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
      return;
    }
  }
  application->world_text_update_count = 0u;
  if (!scene_stopped && world_resources->initialized) {
    VkrPreparedTextDraw *text_draws = NULL;
    uint32_t text_draw_count = 0u;
    if (!vkr_world_resources_prepare_text_draws(
            world_resources, scratch, &text_draws, &text_draw_count)) {
      vkr_standard_scene_runtime_cancel_frame(
          application, &setup, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
      return;
    }
    world_payload.text_draws = text_draws;
    world_payload.text_draw_count = text_draw_count;
  }
  VkrUiSystem *ui = &application->ui_system;
  if (application->project_ui && has_editor)
    application->project_ui(application, &editor_mapping);
  /* An unauthored UI frame contributes an empty stream. */
  if (ui->initialized && ui->frame_index > 0u &&
      !vkr_ui_system_prepare_draw_list(ui, scratch, setup.window_width,
                                       setup.window_height,
                                       &ui_payload.draw_list)) {
    vkr_standard_scene_runtime_cancel_frame(
        application, &setup, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    return;
  }

  // The metrics config is the single authority for whether timestamps are
  // recorded. A second copy could drift and make a report claim timestamps
  // were on for a run that never took one, which would silently mislabel the
  // run's comparison configuration.
  const bool8_t pass_gpu_timing = application->metrics->config.pass_gpu_timings;
  const bool8_t submission_gpu_timing =
      application->metrics->config.submission_gpu_timings;
  const bool8_t gpu_timing = pass_gpu_timing || submission_gpu_timing;
  VkrGpuDebugPayload debug_payload = {
      .enable_timing = gpu_timing,
      .capture_pass_timestamps = pass_gpu_timing,
      .capture_submission_timing = submission_gpu_timing,
      .transmission_depth_diagnostic_enabled =
          !scene_stopped && application->transmission_depth_diagnostic_enabled,
      .shadow_debug_mode = scene_stopped ? 0u : application->shadow_debug_mode,
      .capture = application->capture_request,
  };
  const VkrGpuDebugPayload *debug_ptr =
      (gpu_timing || application->capture_request ||
       application->transmission_depth_diagnostic_enabled ||
       application->shadow_debug_mode != 0u)
          ? &debug_payload
          : NULL;
  VkrFrameIblProbe frame_ibl_probes[VKR_FRAME_IBL_PROBE_MAX] = {0};
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
          probe->prefilter_cubemap.id == 0 ||
          probe->prefilter_cubemap.generation == VKR_INVALID_ID) {
        continue;
      }
      frame_ibl_probes[frame_ibl_probe_count++] = (VkrFrameIblProbe){
          .sh_slot = vkr_render_assets_ibl_sh_slot(&application->assets,
                                                   probe->source_cubemap),
          .prefilter = probe->prefilter_cubemap,
          .center = probe->center,
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
  const VkrFrameLighting frame_lighting = {
      .directional_enabled = application->lighting_system.directional.enabled,
      .directional_direction =
          application->lighting_system.directional.direction,
      .directional_color = application->lighting_system.directional.color,
      .directional_intensity =
          application->lighting_system.directional.intensity,
      .ibl_enabled = frame_ibl_enabled,
      .ibl_source = frame_ibl_source,
      .ibl_intensity = frame_ibl_intensity,
      .ibl_diffuse_intensity = frame_ibl_diffuse_intensity,
      .ibl_specular_intensity = frame_ibl_specular_intensity,
      .point_lights = application->lighting_system.point_lights,
      .point_light_count = application->lighting_system.point_light_count,
      .point_light_grid = &application->lighting_system.point_light_grid,
      .ibl_probes = frame_ibl_probes,
      .ibl_probe_count = frame_ibl_probe_count,
  };

  VkrEditorOverlayDraw overlay_draws[VKR_EDITOR_OVERLAY_DRAW_MAX];
  if (has_editor && !scene_stopped) {
    editor_payload.overlay_draw_count = vkr_gizmo_system_build_draws(
        &application->gizmo_system, application->globals.view,
        application->globals.projection, &editor_mapping, overlay_draws);
    editor_payload.overlay_draws = overlay_draws;
  }

  VkrFrameInput packet = {
      .version = VKR_FRAME_INPUT_VERSION,
      .frame =
          {
              .frame_index = (uint32_t)application->renderer.frame_number,
              .delta_time = delta,
              .window_width = setup.window_width,
              .window_height = setup.window_height,
              .viewport_width = viewport_width,
              .viewport_height = viewport_height,
              .editor_enabled = editor_enabled,
              .scene_generation = application->scene_generation,
          },
      .globals =
          {
              .view = application->globals.view,
              .projection = application->globals.projection,
              .view_position = application->globals.view_position,
              .ambient_color = application->globals.ambient_color,
              .exposure_mode = (uint32_t)application->globals.exposure_mode,
              .manual_exposure = application->globals.manual_exposure,
              .exposure_compensation_ev =
                  application->globals.exposure_compensation_ev,
              .bloom_enabled = application->globals.bloom_enabled,
              .bloom_threshold = application->globals.bloom_threshold,
              .bloom_knee = application->globals.bloom_knee,
              .bloom_intensity = application->globals.bloom_intensity,
              .gtao_enabled = application->globals.gtao_enabled,
              .gtao_radius = application->globals.gtao_radius,
              .gtao_power = application->globals.gtao_power,
              .image_sharpness = application->globals.image_sharpness,
              .render_mode = (uint32_t)application->globals.render_mode,
          },
      .lighting = scene_stopped ? NULL : &frame_lighting,
      .world = scene_stopped ? NULL : &world_payload,
      .shadow = has_shadow ? &shadow_payload : NULL,
      .local_shadow =
          local_shadow_payload.view_count ? &local_shadow_payload : NULL,
      .skybox = !scene_stopped && !application->config->disable_skybox &&
                        skybox_payload.cubemap.id != 0 &&
                        skybox_payload.cubemap.generation != VKR_INVALID_ID
                    ? &skybox_payload
                    : NULL,
      .ui = &ui_payload,
      .editor = has_editor ? &editor_payload : NULL,
      .picking = has_picking ? &picking_payload : NULL,
      .debug = debug_ptr,
  };

  VkrRendererFrameMetrics metrics = {0};
  VkrValidationError validation = {0};
  VkrRendererError submit_err = VKR_RENDERER_ERROR_NONE;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.render_submit) {
    submit_err =
        vkr_renderer_render_frame(&setup, &packet, &metrics, &validation);
  }
  if (submit_err == VKR_RENDERER_ERROR_NONE) {
    if (!scene_stopped) {
      if (application->scene_memory_relief_generation >
          application->assets.material_system
              .texture_stream_relief_generation) {
        VkrDeviceMemoryStats relieved_memory = {0};
        if (vkr_renderer_get_device_memory_stats(&application->renderer,
                                                 &relieved_memory)) {
          vkr_render_assets_refresh_texture_residency_budget(
              &application->assets, &relieved_memory);
          application->texture_memory_sample_frame = setup.number - 1u;
        }
      }
      vkr_material_system_commit_scene_memory_relief(
          &application->assets.material_system,
          application->scene_memory_relief_generation);
    }
    if (has_editor && !scene_stopped) {
      application->editor_viewport.scene_error = VKR_RENDERER_ERROR_NONE;
      application->editor_viewport.rendered_width = viewport_width;
      application->editor_viewport.rendered_height = viewport_height;
      const bool8_t scaled_scene =
          application->renderer.scene_output_extent_overridden ||
          application->renderer.render_scale != 1.0f ||
          application->renderer.upscale_mode ==
              VKR_UPSCALE_MODE_METALFX_TEMPORAL ||
          application->renderer.upscale_mode == VKR_UPSCALE_MODE_FSR31;
      application->editor_viewport.output_width =
          scaled_scene ? application->renderer.scene_output_width
                       : viewport_width;
      application->editor_viewport.output_height =
          scaled_scene ? application->renderer.scene_output_height
                       : viewport_height;
    }
    vkr_shadow_system_commit_frame(
        &application->shadow_system,
        vkr_renderer_get_submit_serial(&application->renderer));
  } else {
    vkr_shadow_system_discard_frame(&application->shadow_system);
  }
  for (uint32_t cascade = 0u; cascade < shadow_frame.cascade_count; ++cascade) {
    metrics.shadow.rendered[cascade] = shadow_frame.rendered[cascade];
    metrics.shadow.reused[cascade] = shadow_frame.reused[cascade];
    metrics.shadow.correctness_forced[cascade] =
        shadow_frame.correctness_forced[cascade];
    metrics.shadow.proactive_refreshed[cascade] =
        shadow_frame.proactive_refreshed[cascade];
    metrics.shadow.dynamic_candidates_tested[cascade] =
        shadow_frame.dynamic_candidates_tested[cascade];
    metrics.shadow.dynamic_forced[cascade] =
        shadow_frame.dynamic_forced[cascade];
  }
  metrics.shadow.sdsm_status = (uint32_t)shadow_frame.sdsm_status;
  metrics.shadow.sdsm_source_lag = shadow_frame.sdsm_source_lag;
  metrics.shadow.sdsm_occupied_count = shadow_frame.sdsm_occupied_count;
  metrics.shadow.sdsm_linear_near = shadow_frame.sdsm_linear_near;
  metrics.shadow.sdsm_linear_far = shadow_frame.sdsm_linear_far;
  application->last_renderer_error = submit_err;
  vkr_mesh_manager_get_metrics(&application->assets.mesh_manager,
                               &metrics.world.mesh_assets);
  if (submit_err == VKR_RENDERER_ERROR_NONE && has_picking &&
      application->picking.state == VKR_PICKING_STATE_RENDER_PENDING)
    application->picking.state = VKR_PICKING_STATE_READBACK_PENDING;
  if (submit_err != VKR_RENDERER_ERROR_CAPTURE_BUSY) {
    application->capture_request = NULL;
  }
#if VKR_METRICS_ENABLED
  VkrRendererMetricsCollectContext metrics_context = {
      .application = vkr_application_metrics_snapshot(
          &application->assets, &application->ui_system,
          &application->lighting_system),
      .renderer = &application->renderer,
      .frame_metrics = &metrics,
      .visibility = &application->visibility_stats,
      .job_system = &application->job_system,
      .cpu_frame_index = packet.frame.frame_index,
      .submit_serial = vkr_renderer_get_submit_serial(&application->renderer),
  };
  vkr_renderer_metrics_collect(&application->renderer_metrics,
                               &metrics_context);
#endif
  VkrRendererError scene_error = submit_err;
  const VkrMaterialTextureStreamStats texture_streams =
      vkr_material_system_get_texture_stream_stats(
          &application->assets.material_system);
  /* Finish outstanding publication/retry work before judging remaining
     texture pressure. Native frame OOM still requests immediate relief. */
  if (scene_error == VKR_RENDERER_ERROR_NONE && !scene_stopped &&
      application->assets.material_system
          .texture_stream_memory_recovery_enabled &&
      texture_streams.in_flight_count == 0u &&
      (application->assets.material_system.texture_stream_memory_wait_count ||
       texture_streams.demanded_evicted_count))
    scene_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  if (scene_error != VKR_RENDERER_ERROR_NONE) {
    if (submit_err == VKR_RENDERER_ERROR_NONE) {
      log_warn("Scene textures require memory relief");
    } else if (validation.field_path && validation.message) {
      log_error("Packet validation failed: %s (%s)", validation.field_path,
                validation.message);
    } else {
      String8 err = vkr_renderer_get_error_string(submit_err);
      log_error("Packet submit failed: %s", string8_cstr(&err));
    }
    const bool8_t reduced =
        !scene_stopped && vkr_standard_scene_runtime_reduce_scene_resolution(
                              application, scene_error);
    if (has_editor && !scene_stopped && !reduced &&
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
    if (!has_editor && !reduced &&
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

vkr_internal bool8_t vkr_standard_scene_runtime_host_frame(
    void *state, const VkrApplicationHostFrame *host_frame) {
  VkrStandardSceneRuntime *application = state;
  const float64_t delta = host_frame->delta_seconds;
  const float64_t current_absolute_time = host_frame->absolute_time_seconds;

  vkr_metrics_begin_frame(
      application->metrics, application->renderer.frame_number + 1u,
      vkr_renderer_get_submit_serial(&application->renderer));

  VkrAllocatorScope frame_scope = {0};
  VkrAllocator *frame_alloc = &application->frame_allocator;
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
      !application->config->disable_camera_controller) {
    vkr_camera_controller_update(&application->camera_controller, delta,
                                 application->ui_capture.mouse ||
                                     application->ui_capture.keyboard);
  }

  vkr_camera_registry_update_all(camera_system);

  if (application->active_scene) {
    vkr_lighting_system_sync_from_scene(&application->lighting_system,
                                        application->active_scene);
  }

  if (camera &&
      !vkr_standard_scene_runtime_editor_scene_rendering_stopped(application)) {
    VKR_METRICS_SCOPE_NS(application->metrics,
                         application->metric_ids.shadow_update) {
      VkrShadowCasterDepthBounds caster_bounds = {0};
      vkr_scene_measure_caster_bounds(&application->assets.mesh_manager,
                                      &caster_bounds);
      const VkrShadowDepthRangeSample *sdsm_sample =
          application->renderer.timing_result.shadow_depth_range.submit_value >
                  0u
              ? &application->renderer.timing_result.shadow_depth_range
              : NULL;
      vkr_shadow_system_set_depth_range_sample(
          &application->shadow_system, sdsm_sample,
          application->renderer.frame_number, application->scene_generation);
      vkr_shadow_system_update(
          &application->shadow_system, camera,
          application->lighting_system.directional.enabled,
          application->lighting_system.directional.direction, &caster_bounds);
    }
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
  vkr_metrics_end_frame(application->metrics);
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
  vkr_application_host_shutdown(&application->host);

  if (vkr_renderer_wait_idle(&application->renderer) !=
      VKR_RENDERER_ERROR_NONE) {
    log_warn("Failed to wait for renderer to be idle");
  }

  /*
   * Resource async workers call loader prepare/finalize callbacks that use
   * renderer-owned async allocators (texture/material/mesh/scene). Join worker
   * threads before renderer teardown so those allocators remain valid for the
   * entire worker lifetime.
   */
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

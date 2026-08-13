// clang-format off
/**
 * @file application.h
 * @brief Defines the core application structure, lifecycle management, and event handling.
 *
 * This file provides the main `Application` structure that encapsulates all
 * core components of a typical application, such as windowing, event management,
 * and the main game/application loop. It handles initialization, the primary
 * update loop (including frame timing and limiting), and shutdown procedures.
 *
 * Key Components:
 * - **ApplicationConfig:** A structure to specify initial settings for the
 *   application, such as window title, dimensions, and target frame rate.
 * - **Application:** The central structure holding all application-specific data,
 *   including memory arenas, the event manager, window, clock for timing, and
 *   state flags.
 * - **Lifecycle Functions:**
 *   - `application_create()`: Initializes the application and its subsystems.
 *   - `application_start()`: Begins the main application loop.
 *   - `application_stop()`: Suspends the application loop.
 *   - `application_resume()`: Resumes a suspended application loop.
 *   - `application_close()`: Signals the application loop to terminate.
 *   - `application_shutdown()`: Cleans up and releases all application resources.
 * - **Event Handling:** Provides callback functions (`application_on_*_event`)
 *   that are registered with the `EventManager` to respond to various system
 *   and input events.
 * - **Main Loop:** `application_start()` contains the core loop that updates
 *   the clock, processes window events, calls the user-defined
 *   `application_update()` function, manages input state, and implements frame
 *   rate limiting.
 *
 * Usage Pattern:
 * 1. Populate an `ApplicationConfig` structure with desired settings.
 * 2. Call `application_create()` with a pointer to an `Application` struct and
 *    the configuration. Check the return value for success.
 * 3. Implement the `application_update()` function (defined by the user, typically
 *    in `app/src/main.c` or similar) to contain game logic.
 * 4. Implement `application_on_*_event` callback functions as needed to handle
 *    specific events.
 * 5. Call `application_start()` to run the main loop.
 * 6. Upon loop termination (e.g., window close), `application_start()` will exit.
 * 7. Call `application_close()` if a programmatic stop is needed before the natural end of the loop.
 * 8. Call `application_shutdown()` to free all resources before program exit.
 */
// clang-format on
#pragma once

#include "containers/bitset.h"
#include "core/event.h"
#include "core/logger.h"
#include "core/vkr_clock.h"
#include "core/vkr_gamepad.h"
#include "core/vkr_job_system.h"
#include "core/vkr_metrics.h"
#include "core/vkr_threads.h"
#include "core/vkr_window.h"
#include "defines.h"
#include "math/vec.h"
#include "math/vkr_frustum.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vkr_renderer_metrics.h"
#include "renderer/vkr_visibility.h"

/**
 * @brief Editor viewport state owned by the application.
 */
typedef struct ApplicationEditorViewport {
  bool8_t enabled;
  VkrViewportFitMode fit_mode;
  float32_t render_scale;
  uint32_t last_target_width;
  uint32_t last_target_height;
} ApplicationEditorViewport;

/**
 * @brief Flags representing the current state of the application.
 * These flags are used to manage the application's lifecycle and behavior.
 */
typedef enum ApplicationFlag {
  APPLICATION_FLAG_NONE = 0, /**< No specific flags set. */
  APPLICATION_FLAG_INITIALIZED =
      1 << 0, /**< Application has been successfully initialized. */
  APPLICATION_FLAG_RUNNING = 1 << 1, /**< Application is currently running its
                                        main loop. */
  APPLICATION_FLAG_SUSPENDED =
      1 << 2, /**< Application loop is currently suspended. */
} ApplicationFlag;

#define VKR_MAX_PENDING_TEXT_UPDATES 32

typedef struct ApplicationTextUpdate {
  uint32_t text_id;
  String8 content;
  bool8_t has_transform;
  VkrTransform transform;
} ApplicationTextUpdate;

/**
 * @brief Configuration settings for creating an application instance.
 * This structure is passed to `application_create()` to specify initial
 * properties of the application, such as window characteristics and resource
 * sizes.
 */
typedef struct ApplicationConfig {
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
  bool8_t capture_enabled;
  uint32_t capture_ring_capacity;
  uint64_t capture_max_batch_bytes;
  /** Boot intent only: `profile`, `requested_mask`, and `excluded_mask` are
      read and the closure is recomputed. Zero-initialized means full boot. */
  VkrSubsystemPlan subsystem_plan;
} ApplicationConfig;

typedef struct ApplicationMetricIds {
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
} ApplicationMetricIds;

/**
 * @brief Main structure representing the application.
 * Encapsulates all core components, state, and resources needed for the
 * application to run.
 */
/**
 * @brief Draw lists for the shadow pass, built from light visibility.
 *
 * Kept separate from the world payload because camera-culled objects can still
 * cast visible shadows: reusing a camera-culled list for CSM drops them. Both
 * lists index the same instance array, which holds every object visible to
 * either the camera or the light.
 */
typedef struct VkrShadowDrawLists {
  VkrDrawItem *opaque_draws;
  uint32_t opaque_draw_count;
  VkrDrawItem *alpha_draws;
  uint32_t alpha_draw_count;
  /**
   * Shadow keeps its own instance array: its visible set differs from the
   * camera's, so one array cannot hold contiguous merged runs for both.
   */
  VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrShadowDrawLists;

typedef struct Application {
  Arena *app_arena; /**< Main memory arena for general application use (e.g.,
                       game entities, state). */
  Arena *log_arena; /**< Memory arena dedicated to the logging system. */
  VkrAllocator app_allocator; /**< Allocator backed by `app_arena` for thread
                                 primitives and other systems. */
  Arena *metrics_arena;
  VkrAllocator metrics_allocator;
  VkrMetrics *metrics;
  ApplicationMetricIds metric_ids;
  VkrRendererMetrics renderer_metrics;
  EventManager event_manager; /**< Manages event dispatch and subscriptions. */
  VkrWindow window;           /**< Represents the application window. */
  ApplicationConfig *config;  /**< Pointer to the configuration used to create
                                 this application instance. */
  RendererFrontend renderer;  /**< Renderer frontend state (public). */

  VkrClock
      clock; /**< Clock used for timing frames and calculating delta time. */
  float64_t last_frame_time; /**< Timestamp of the previous frame, used for
                                delta time calculation. */
  Bitset8 app_flags;         /**< Bitset holding `ApplicationFlag`s to track the
                                current state. */
  VkrMutex app_mutex;        /**< Mutex for application state. */

  VkrGamepad gamepad; /**< The gamepad system for the application. */

  /** Last frame's frustum-culling counters, produced by payload construction.
   */
  VkrVisibilityStats visibility_stats;

  VkrJobSystem job_system; /**< Engine-wide job system. */

  ApplicationTextUpdate ui_text_updates[VKR_MAX_PENDING_TEXT_UPDATES];
  uint32_t ui_text_update_count;
  ApplicationTextUpdate world_text_updates[VKR_MAX_PENDING_TEXT_UPDATES];
  uint32_t world_text_update_count;

  ApplicationEditorViewport editor_viewport;
  const VkrCaptureBatchRequest *capture_request;
  VkrRendererError last_renderer_error;
  /* Per-pass GPU timing is owned by `metrics->config.pass_gpu_timings`. */
} Application;

/**
 * @brief True when the application owns a window.
 *
 * An offscreen application creates no window, so it has no surface, input
 * state, or gamepads to poll, update, or destroy.
 */
vkr_internal INLINE bool8_t
application_is_windowed(const Application *application) {
  return application->config->present_target.kind !=
         VKR_PRESENT_TARGET_OFFSCREEN;
}

/**
 * @brief Default event handler for general application events.
 * Registered with the `EventManager` for events like `APPLICATION_INIT`,
 * `APPLICATION_SHUTDOWN`, etc.
 * @param event Pointer to the `Event` being processed.
 * @return `true_v` if the event was handled, `false_v` otherwise (though
 * typically always `true_v`).
 */
bool8_t application_on_event(Event *event, UserData user_data);

/**
 * @brief Default event handler for window-specific events.
 * Registered with the `EventManager` for events like `WINDOW_CLOSE`,
 * `WINDOW_RESIZE`.
 * @param event Pointer to the `Event` being processed.
 * @return `true_v` if the event was handled, `false_v` otherwise (though
 * typically always `true_v`).
 */
bool8_t application_on_window_event(Event *event, UserData user_data);

/**
 * @brief Default event handler for key input events.
 * Registered with the `EventManager` for events like `KEY_PRESS`,
 * `KEY_RELEASE`.
 * @param event Pointer to the `Event` being processed.
 * @return `true_v` if the event was handled, `false_v` otherwise (though
 * typically always `true_v`).
 */
bool8_t application_on_key_event(Event *event, UserData user_data);

/**
 * @brief Default event handler for mouse input events.
 * Registered with the `EventManager` for events like `MOUSE_MOVE`,
 * `BUTTON_PRESS`.
 * @param event Pointer to the `Event` being processed.
 * @return `true_v` if the event was handled, `false_v` otherwise (though
 * typically always `true_v`).
 */
bool8_t application_on_mouse_event(Event *event, UserData user_data);

/**
 * @brief User-defined application update function.
 * This function is called once per frame from within the main application
 * loop
 * (`application_start`). It is intended to house the primary game logic,
 * rendering calls, and other per-frame updates.
 * @param application Pointer to the main `Application` structure.
 * @param delta The time elapsed since the last frame, in seconds.
 */
void application_update(Application *application, float64_t delta);

vkr_internal bool8_t application_register_duration_metric(
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

vkr_internal bool8_t application_metrics_initialize(Application *application) {
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
  ApplicationMetricIds *ids = &application->metric_ids;
  // Durations are nanoseconds. No name carries a unit suffix, because the
  // catalog unit is the contract and a name that disagreed with it would be
  // wrong by a factor of a million in every consumer that trusted it.
  if (!application_register_duration_metric(application->metrics, "frame.wall",
                                            VKR_METRIC_DOMAIN_FRAME, true_v,
                                            &ids->frame_wall) ||
      !application_register_duration_metric(
          application->metrics, "cpu.frame_work", VKR_METRIC_DOMAIN_FRAME,
          true_v, &ids->frame_work) ||
      !application_register_duration_metric(application->metrics, "cpu.update",
                                            VKR_METRIC_DOMAIN_FRAME, true_v,
                                            &ids->update) ||
      !application_register_duration_metric(
          application->metrics, "cpu.render_prepare", VKR_METRIC_DOMAIN_FRAME,
          true_v, &ids->render_prepare) ||
      !application_register_duration_metric(
          application->metrics, "cpu.render_submit", VKR_METRIC_DOMAIN_FRAME,
          true_v, &ids->render_submit) ||
      // Not required: the frame limiter is off for profiling, so this slot is
      // legitimately unsampled in exactly the runs that matter most. Marking
      // it required would make every authoritative run report incomplete.
      !application_register_duration_metric(
          application->metrics, "frame.limiter_sleep", VKR_METRIC_DOMAIN_FRAME,
          false_v, &ids->limiter_sleep) ||
      !vkr_renderer_metrics_register(&application->renderer_metrics,
                                     application->metrics)) {
    return false_v;
  }
  return true_v;
}
/**
 * @brief Creates a cube mesh and uploads it to GPU buffers
 * @param application Pointer to the `Application` structure.
 * @return `true_v` on success, `false_v` on failure
 */
bool8_t application_create_cube_mesh(Application *application);

/**
 * @brief Initializes the application and its core subsystems.
 * Sets up memory arenas, logging, event manager, window, and clock.
 * Subscribes default event handlers.
 * Asserts that the provided configuration is valid.
 * @param application Pointer to an `Application` structure to be initialized.
 * @param config Pointer to an `ApplicationConfig` structure containing
 * initialization settings.
 * @return `true_v` on successful initialization, `false_v` if any critical
 * initialization step fails (e.g., arena creation).
 */
bool8_t application_create(Application *application,
                           ApplicationConfig *config) {
  assert(config != NULL && "Application config is NULL");
  assert(config->title != NULL && "Application title is NULL");
  assert(config->app_arena_size > 0 && "Application arena size is 0");
  assert(config->width > 0 && "Application width is less than 0");
  assert(config->height > 0 && "Application height is less than 0");

  if (!vkr_platform_init()) {
    log_fatal("Failed to initialize platform!");
    return false_v;
  }

  application->config = config;
  application->editor_viewport = (ApplicationEditorViewport){
      .enabled = false_v,
      .fit_mode = VKR_VIEWPORT_FIT_STRETCH,
      .render_scale = 1.0f,
      .last_target_width = 0,
      .last_target_height = 0,
  };
  application->app_flags = bitset8_create();

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  application->app_arena = arena_create(
      config->app_arena_size, config->app_arena_size, app_arena_flags);
  if (!application->app_arena) {
    log_fatal("Failed to create app_arena!");
    return false_v;
  }

  application->app_allocator = (VkrAllocator){.ctx = application->app_arena};
  if (!vkr_allocator_arena(&application->app_allocator)) {
    log_fatal("Failed to initialize app allocator!");
    return false_v;
  }

  ArenaFlags log_arena_flags = bitset8_create();
  bitset8_set(&log_arena_flags, ARENA_FLAG_LARGE_PAGES);
  application->log_arena = arena_create(MB(5), MB(5), log_arena_flags);
  if (!application->log_arena) {
    log_fatal("Failed to create log_arena!");
    return false_v;
  }

  log_init(application->log_arena);

  log_debug("Initialized logging");

  if (!application_metrics_initialize(application)) {
    log_fatal("Failed to initialize application metrics");
    return false_v;
  }

  event_manager_create(&application->event_manager);
  const bool8_t windowed =
      config->present_target.kind != VKR_PRESENT_TARGET_OFFSCREEN;
  if (windowed) {
    application->window.hidden = config->window_hidden;
    if (!vkr_window_create(&application->window, &application->event_manager,
                           config->title, config->x, config->y, config->width,
                           config->height)) {
      log_fatal("Failed to create application window");
      return false_v;
    }
  }
  application->clock = vkr_clock_create();
  if (!vkr_mutex_create(&application->app_allocator, &application->app_mutex)) {
    log_fatal("Failed to create application mutex!");
    return false_v;
  }

  VkrJobSystemConfig job_cfg = vkr_job_system_config_default();
  if (!vkr_job_system_init(&job_cfg, &application->job_system)) {
    log_fatal("Failed to initialize job system");
    return false_v;
  }

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  const VkrRendererMetricsProducerConfig *metrics_producers =
      vkr_renderer_metrics_get_producers(&application->renderer_metrics);
  VkrRendererBackendConfig backend_cfg = {
      .application_name = "vulkan_renderer",
      .present_target = config->present_target,
      .requested_present_mode = config->requested_present_mode,
      .capture_enabled = config->capture_enabled,
      .capture_ring_capacity = config->capture_ring_capacity,
      .capture_max_batch_bytes = config->capture_max_batch_bytes,
  };
  if (!vkr_renderer_initialize(
          &application->renderer, config->renderer_backend,
          windowed ? &application->window : NULL, &application->event_manager,
          &application->config->device_requirements, &backend_cfg,
          application->config->target_frame_rate, &renderer_error)) {
    log_fatal("Failed to create renderer!");
    return false_v;
  }
  if (!vkr_renderer_metrics_register_device_memory(
          &application->renderer_metrics, &application->renderer) ||
      !vkr_metrics_seal(application->metrics)) {
    log_fatal("Failed to finalize renderer metrics catalog");
    return false_v;
  }

  if (windowed) {
    vkr_gamepad_init(&application->gamepad, &application->window.input_state);
  }

  /* The closure is always recomputed from the config's intent, so a caller
     cannot hand-assemble an `effective_mask` that the renderer never agreed
     to, and a zero-initialized config resolves to the full interactive plan. */
  VkrSubsystemPlan subsystem_plan = {0};
  if (!vkr_renderer_subsystem_plan_build(config->subsystem_plan.profile,
                                         config->subsystem_plan.requested_mask,
                                         config->subsystem_plan.excluded_mask,
                                         &subsystem_plan, &renderer_error)) {
    log_fatal("Failed to build the renderer subsystem plan");
    return false_v;
  }
  if (!vkr_renderer_systems_initialize(&application->renderer,
                                       &application->job_system,
                                       metrics_producers, &subsystem_plan)) {
    log_fatal("Failed to initialize renderer frontend systems");
    return false_v;
  }
  if (!vkr_renderer_metrics_prepare_pass_table(
          &application->renderer_metrics, &application->renderer,
          &application->metrics_allocator)) {
    log_fatal("Failed to prepare renderer metrics pass table");
    return false_v;
  }

  VkrCameraHandle active_camera =
      vkr_camera_registry_get_active(&application->renderer.camera_system);
  application->renderer.active_camera = active_camera;
  VkrCamera *camera =
      vkr_camera_registry_get_by_handle(&application->renderer.camera_system,
                                        application->renderer.active_camera);
  if (!camera) {
    log_fatal("Failed to retrieve active camera");
    return false_v;
  }
  vkr_camera_controller_create(
      &application->renderer.camera_controller, camera,
      (float32_t)application->config->target_frame_rate);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_WINDOW_CLOSE,
                          application_on_window_event, NULL);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_WINDOW_INIT,
                          application_on_window_event, NULL);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_KEY_PRESS,
                          application_on_key_event, NULL);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_KEY_RELEASE,
                          application_on_key_event, NULL);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_MOUSE_MOVE,
                          application_on_mouse_event, NULL);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_MOUSE_WHEEL,
                          application_on_mouse_event, NULL);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_BUTTON_PRESS,
                          application_on_mouse_event, NULL);

  event_manager_subscribe(&application->event_manager,
                          EVENT_TYPE_BUTTON_RELEASE, application_on_mouse_event,
                          NULL);

  event_manager_subscribe(&application->event_manager,
                          EVENT_TYPE_APPLICATION_INIT, application_on_event,
                          NULL);

  event_manager_subscribe(&application->event_manager,
                          EVENT_TYPE_APPLICATION_SHUTDOWN, application_on_event,
                          NULL);

  event_manager_subscribe(&application->event_manager,
                          EVENT_TYPE_APPLICATION_RESUME, application_on_event,
                          NULL);

  bitset8_set(&application->app_flags, APPLICATION_FLAG_INITIALIZED);

  event_manager_dispatch(&application->event_manager,
                         (Event){.type = EVENT_TYPE_APPLICATION_INIT});

  log_info("Application initialized");
  return true_v;
}

vkr_internal VkrMaterial *application_get_material(RendererFrontend *rf,
                                                   VkrMaterialHandle handle) {
  if (!rf) {
    return NULL;
  }

  VkrMaterial *material =
      vkr_material_system_get_by_handle(&rf->material_system, handle);
  if (!material && rf->material_system.default_material.id != 0) {
    material = vkr_material_system_get_by_handle(
        &rf->material_system, rf->material_system.default_material);
  }

  return material;
}

vkr_internal VkrDrawAlphaRouting application_material_alpha_routing(
    RendererFrontend *rf, VkrMaterial *material) {
  if (!rf || !material) {
    return (VkrDrawAlphaRouting){0};
  }
  return vkr_draw_alpha_routing(
      vkr_material_system_material_alpha_mode(&rf->material_system, material));
}

vkr_internal bool8_t application_material_is_transmissive(
    RendererFrontend *rf, VkrMaterial *material) {
  return rf && material ? vkr_material_system_material_is_transmissive(
                              &rf->material_system, material)
                        : false_v;
}

typedef enum VkrWorldGpuCandidateStream {
  VKR_WORLD_GPU_CANDIDATE_STREAM_NONE = 0,
  VKR_WORLD_GPU_CANDIDATE_STREAM_OPAQUE,
  VKR_WORLD_GPU_CANDIDATE_STREAM_TRANSMISSION,
} VkrWorldGpuCandidateStream;

vkr_internal VkrWorldGpuCandidateStream application_world_gpu_candidate_stream(
    VkrDrawAlphaRouting alpha, bool8_t transmissive) {
  if (transmissive)
    return VKR_WORLD_GPU_CANDIDATE_STREAM_TRANSMISSION;
  return alpha.world_transparent ? VKR_WORLD_GPU_CANDIDATE_STREAM_NONE
                                 : VKR_WORLD_GPU_CANDIDATE_STREAM_OPAQUE;
}

/**
 * Local reflection-probe descriptors are selected from world position and
 * applied once per draw. Until probe selection moves into per-instance data,
 * instances at different positions cannot legally share one world draw.
 */
vkr_internal bool8_t
application_world_may_have_position_dependent_ibl(const RendererFrontend *rf) {
  if (!rf || !rf->world_resources.initialized || !rf->active_scene) {
    return false_v;
  }

  const VkrScene *scene = rf->active_scene;
  for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
    const VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
    // PENDING matters too: the IBL-bake pass precedes the world pass and can
    // make the probe READY after this packet has already been built.
    if (probe->enabled &&
        (probe->bake_state == VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING ||
         probe->bake_state == VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY)) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal float32_t application_transparent_depth(Mat4 view, Mat4 model,
                                                     Vec3 local_center) {
  Vec3 world_center = mat4_mul_vec3(model, local_center);
  Vec4 view_pos = mat4_mul_vec4(
      view, vec4_new(world_center.x, world_center.y, world_center.z, 1.0f));
  float32_t depth = -view_pos.z;
  return depth > 0.0f ? depth : 0.0f;
}

vkr_internal uint64_t application_pack_transparent_sort_key(
    float32_t distance, uint32_t tie_breaker) {
  uint32_t distance_bits = 0;
  MemCopy(&distance_bits, &distance, sizeof(distance_bits));
  return ((uint64_t)distance_bits << 32) | (uint64_t)tie_breaker;
}

/**
 * @brief Builds the world and shadow draw lists for one frame.
 *
 * Both lists are produced in one traversal so their visibility decisions are
 * taken from the same classification. The camera list and the shadow list are
 * independent: an object behind the camera can still cast a shadow into view,
 * so the shadow list is built from the light's volume rather than reusing the
 * camera-culled list.
 *
 * Visibility is decided once per object in the count pass and cached in
 * `visibility`, because the count and populate passes must agree exactly --
 * re-testing risks a divergence that would desynchronize the arrays.
 *
 * @param shadow_frustums Cascade volumes to test casters against. Shadow
 *        visibility is their union; NULL/zero disables shadow-side culling.
 * @param out_shadow Optional shadow draw lists; may be NULL when shadows are
 *        disabled for this frame.
 */
vkr_internal bool8_t application_build_world_payload(
    Application *application, VkrAllocator *scratch,
    const VkrFrustum *shadow_frustums, uint32_t shadow_frustum_count,
    VkrWorldPassPayload *out_payload, VkrShadowDrawLists *out_shadow,
    VkrVisibilityStats *out_stats) {
  if (!application || !scratch || !out_payload) {
    return false_v;
  }

  RendererFrontend *rf = &application->renderer;
  Mat4 view = rf->globals.view;
  VkrFrustum camera_frustum =
      vkr_frustum_from_view_projection(view, rf->globals.projection);

  VkrVisibilityStats stats = {0};
  const bool8_t position_dependent_ibl =
      application_world_may_have_position_dependent_ibl(rf);
  if (out_shadow) {
    *out_shadow = (VkrShadowDrawLists){0};
  }

  const uint32_t mesh_count = vkr_mesh_manager_count(&rf->mesh_manager);
  const uint32_t live_instance_count =
      vkr_mesh_manager_instance_count(&rf->mesh_manager);

  // Visibility is decided per submesh, not per object. A scene like Sponza is a
  // handful of instances whose bounds enclose the camera, so object-granularity
  // culling rejects nothing; the submesh AABBs are what actually localize
  // geometry.
  uint32_t submesh_slot_capacity = 0;
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager,
                                                            i, &mesh_slot);
    if (!mesh || !mesh->visible ||
        mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }
    submesh_slot_capacity += vkr_mesh_manager_submesh_count(mesh);
  }
  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_live_index(
        &rf->mesh_manager, i, &instance_slot);
    if (!instance || !instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_asset(&rf->mesh_manager, instance->asset);
    if (asset) {
      submesh_slot_capacity += (uint32_t)asset->submeshes.length;
    }
  }

  uint8_t *visibility = NULL;
  if (submesh_slot_capacity > 0) {
    visibility = vkr_allocator_alloc(scratch, submesh_slot_capacity,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!visibility) {
      *out_payload = (VkrWorldPassPayload){0};
      return false_v;
    }
    MemZero(visibility, submesh_slot_capacity);
  }
  // Ordinal walked identically by the count and populate passes, so a cached
  // decision always lands on the submesh it was taken for.
  uint32_t vis_slot = 0;

  uint32_t camera_opaque_count = 0;
  uint32_t camera_transmission_count = 0;
  uint32_t camera_transparent_count = 0;
  uint32_t shadow_opaque_count = 0;
  uint32_t shadow_alpha_count = 0;
  uint32_t instance_slot_count = 0;
  uint32_t gpu_candidate_count = 0;
  uint32_t transmission_gpu_candidate_count = 0;

  // ---- Count pass: classify each object once, then size the lists. ----
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager,
                                                            i, &mesh_slot);
    if (!mesh || !mesh->visible ||
        mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }

    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, mesh_slot, s);
      if (!submesh) {
        continue;
      }

      Vec3 sphere_center = {0};
      float32_t sphere_radius = 0.0f;
      vkr_visibility_submesh_sphere(mesh->model, submesh->center,
                                    submesh->min_extents, submesh->max_extents,
                                    &sphere_center, &sphere_radius);
      uint8_t flags = vkr_visibility_classify(
          &camera_frustum, shadow_frustums, shadow_frustum_count,
          mesh->bounds_valid, sphere_center, sphere_radius, &stats);
      if (!out_shadow) {
        flags &= (uint8_t)~VKR_VISIBLE_SHADOW;
      }
      visibility[vis_slot++] = flags;
      VkrMaterial *material = application_get_material(rf, submesh->material);
      const VkrDrawAlphaRouting alpha =
          application_material_alpha_routing(rf, material);
      const bool8_t transmissive =
          application_material_is_transmissive(rf, material);
      const VkrWorldGpuCandidateStream gpu_stream =
          application_world_gpu_candidate_stream(alpha, transmissive);
      if (gpu_stream == VKR_WORLD_GPU_CANDIDATE_STREAM_TRANSMISSION) {
        transmission_gpu_candidate_count++;
      } else if (gpu_stream == VKR_WORLD_GPU_CANDIDATE_STREAM_OPAQUE) {
        gpu_candidate_count++;
      }
      if (flags == 0) {
        continue;
      }
      if (flags & VKR_VISIBLE_CAMERA) {
        if (transmissive) {
          camera_transmission_count++;
        } else if (alpha.world_transparent) {
          camera_transparent_count++;
        } else {
          camera_opaque_count++;
        }
      }
      if (flags & VKR_VISIBLE_SHADOW) {
        if (alpha.shadow_alpha_tested) {
          shadow_alpha_count++;
        } else {
          shadow_opaque_count++;
        }
      }
      instance_slot_count++;
    }
  }

  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_live_index(
        &rf->mesh_manager, i, &instance_slot);
    if (!instance || !instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }

    VkrMeshAsset *asset =
        vkr_mesh_manager_get_asset(&rf->mesh_manager, instance->asset);
    if (!asset) {
      continue;
    }

    uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, s);
      if (!submesh) {
        continue;
      }

      Vec3 sphere_center = {0};
      float32_t sphere_radius = 0.0f;
      vkr_visibility_submesh_sphere(instance->model, submesh->center,
                                    submesh->min_extents, submesh->max_extents,
                                    &sphere_center, &sphere_radius);
      uint8_t flags = vkr_visibility_classify(
          &camera_frustum, shadow_frustums, shadow_frustum_count,
          instance->bounds_valid, sphere_center, sphere_radius, &stats);
      if (!out_shadow) {
        flags &= (uint8_t)~VKR_VISIBLE_SHADOW;
      }
      visibility[vis_slot++] = flags;
      VkrMaterial *material = application_get_material(rf, submesh->material);
      const VkrDrawAlphaRouting alpha =
          application_material_alpha_routing(rf, material);
      const bool8_t transmissive =
          application_material_is_transmissive(rf, material);
      const VkrWorldGpuCandidateStream gpu_stream =
          application_world_gpu_candidate_stream(alpha, transmissive);
      if (gpu_stream == VKR_WORLD_GPU_CANDIDATE_STREAM_TRANSMISSION) {
        transmission_gpu_candidate_count++;
      } else if (gpu_stream == VKR_WORLD_GPU_CANDIDATE_STREAM_OPAQUE) {
        gpu_candidate_count++;
      }
      if (flags == 0) {
        continue;
      }
      if (flags & VKR_VISIBLE_CAMERA) {
        if (transmissive) {
          camera_transmission_count++;
        } else if (alpha.world_transparent) {
          camera_transparent_count++;
        } else {
          camera_opaque_count++;
        }
      }
      if (flags & VKR_VISIBLE_SHADOW) {
        if (alpha.shadow_alpha_tested) {
          shadow_alpha_count++;
        } else {
          shadow_opaque_count++;
        }
      }
      instance_slot_count++;
    }
  }

  if (out_stats) {
    *out_stats = stats;
  }

  if (instance_slot_count == 0 && gpu_candidate_count == 0 &&
      transmission_gpu_candidate_count == 0) {
    *out_payload = (VkrWorldPassPayload){0};
    return true_v;
  }

  // Candidates are collected first so emission order can be chosen after the
  // fact: merging needs equal keys adjacent AND their instance records
  // contiguous, which is only decidable once every candidate is known.
  VkrDrawCandidate *camera_opaque_cands = NULL;
  VkrDrawCandidate *camera_transmission_cands = NULL;
  VkrDrawCandidate *camera_transparent_cands = NULL;
  VkrDrawCandidate *shadow_opaque_cands = NULL;
  VkrDrawCandidate *shadow_alpha_cands = NULL;
  VkrWorldDrawCandidate *gpu_candidates = NULL;
  VkrWorldDrawCandidate *transmission_gpu_candidates = NULL;
  if (gpu_candidate_count > 0) {
    gpu_candidates = vkr_allocator_alloc(
        scratch, sizeof(VkrWorldDrawCandidate) * (uint64_t)gpu_candidate_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (transmission_gpu_candidate_count > 0) {
    transmission_gpu_candidates =
        vkr_allocator_alloc(scratch,
                            sizeof(VkrWorldDrawCandidate) *
                                (uint64_t)transmission_gpu_candidate_count,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (camera_opaque_count > 0) {
    camera_opaque_cands = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawCandidate) * (uint64_t)camera_opaque_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (camera_transparent_count > 0) {
    camera_transparent_cands = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawCandidate) * (uint64_t)camera_transparent_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (camera_transmission_count > 0) {
    camera_transmission_cands = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawCandidate) * (uint64_t)camera_transmission_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (out_shadow && shadow_opaque_count > 0) {
    shadow_opaque_cands = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawCandidate) * (uint64_t)shadow_opaque_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (out_shadow && shadow_alpha_count > 0) {
    shadow_alpha_cands = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawCandidate) * (uint64_t)shadow_alpha_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  VkrDrawItem *opaque_draws = NULL;
  if (camera_opaque_count > 0) {
    opaque_draws = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawItem) * (uint64_t)camera_opaque_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  VkrDrawItem *transparent_draws = NULL;
  if (camera_transparent_count > 0) {
    transparent_draws = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawItem) * (uint64_t)camera_transparent_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  VkrDrawItem *transmission_draws = NULL;
  if (camera_transmission_count > 0) {
    transmission_draws = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawItem) * (uint64_t)camera_transmission_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  VkrDrawItem *shadow_opaque_draws = NULL;
  if (out_shadow && shadow_opaque_count > 0) {
    shadow_opaque_draws = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawItem) * (uint64_t)shadow_opaque_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  VkrDrawItem *shadow_alpha_draws = NULL;
  if (out_shadow && shadow_alpha_count > 0) {
    shadow_alpha_draws = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawItem) * (uint64_t)shadow_alpha_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  // World and shadow keep separate instance arrays: their visible sets differ,
  // so one array cannot hold contiguous merged runs for both.
  const uint32_t world_instance_count = camera_opaque_count +
                                        camera_transmission_count +
                                        camera_transparent_count;
  const uint32_t shadow_instance_count =
      out_shadow ? shadow_opaque_count + shadow_alpha_count : 0u;
  VkrInstanceDataGPU *instances = NULL;
  if (world_instance_count > 0) {
    instances = vkr_allocator_alloc(
        scratch, sizeof(VkrInstanceDataGPU) * (uint64_t)world_instance_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  VkrInstanceDataGPU *shadow_instances = NULL;
  if (shadow_instance_count > 0) {
    shadow_instances = vkr_allocator_alloc(
        scratch, sizeof(VkrInstanceDataGPU) * (uint64_t)shadow_instance_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if ((camera_opaque_count > 0 && (!opaque_draws || !camera_opaque_cands)) ||
      (camera_transmission_count > 0 &&
       (!transmission_draws || !camera_transmission_cands)) ||
      (camera_transparent_count > 0 &&
       (!transparent_draws || !camera_transparent_cands)) ||
      (out_shadow && shadow_opaque_count > 0 &&
       (!shadow_opaque_draws || !shadow_opaque_cands)) ||
      (out_shadow && shadow_alpha_count > 0 &&
       (!shadow_alpha_draws || !shadow_alpha_cands)) ||
      (gpu_candidate_count > 0 && !gpu_candidates) ||
      (transmission_gpu_candidate_count > 0 && !transmission_gpu_candidates) ||
      (world_instance_count > 0 && !instances) ||
      (shadow_instance_count > 0 && !shadow_instances)) {
    *out_payload = (VkrWorldPassPayload){0};
    if (out_shadow) {
      *out_shadow = (VkrShadowDrawLists){0};
    }
    return false_v;
  }

  uint32_t opaque_index = 0;
  uint32_t transmission_index = 0;
  uint32_t transparent_index = 0;
  uint32_t shadow_opaque_index = 0;
  uint32_t shadow_alpha_index = 0;
  uint32_t gpu_candidate_index = 0;
  uint32_t transmission_gpu_candidate_index = 0;
  vis_slot = 0;

  // ---- Populate pass: reuse the cached visibility, never re-test. ----
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager,
                                                            i, &mesh_slot);
    if (!mesh || !mesh->visible ||
        mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }
    uint32_t object_id =
        mesh->render_id
            ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE, mesh->render_id)
            : 0;

    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, mesh_slot, s);
      if (!submesh) {
        continue;
      }
      const uint8_t flags = visibility[vis_slot++];
      VkrMaterial *material = application_get_material(rf, submesh->material);
      const VkrMaterialHandle draw_material =
          material ? (VkrMaterialHandle){.id = material->id,
                                         .generation = material->generation}
                   : submesh->material;
      const VkrDrawAlphaRouting alpha =
          application_material_alpha_routing(rf, material);
      const bool8_t transmissive =
          application_material_is_transmissive(rf, material);
      const VkrWorldGpuCandidateStream gpu_stream =
          application_world_gpu_candidate_stream(alpha, transmissive);
      if (gpu_stream != VKR_WORLD_GPU_CANDIDATE_STREAM_NONE) {
        const Vec3 half_extents = vec3_scale(
            vec3_sub(submesh->max_extents, submesh->min_extents), 0.5f);
        VkrWorldDrawCandidate gpu_candidate = (VkrWorldDrawCandidate){
            .mesh = {.id = mesh_slot + 1u, .generation = 0},
            .geometry = submesh->geometry,
            .submesh_index = s,
            .material = draw_material,
            .instance = {.model = mesh->model, .object_id = object_id},
            .local_bounding_sphere = {submesh->center.x, submesh->center.y,
                                      submesh->center.z,
                                      vec3_length(half_extents)},
            .state_bucket = vkr_world_draw_state_bucket(
                alpha.shadow_alpha_tested ? VKR_MATERIAL_ALPHA_CUTOUT
                                          : VKR_MATERIAL_ALPHA_OPAQUE,
                material ? material->double_sided : false_v),
            .flags =
                mesh->bounds_valid ? VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID : 0u,
        };
        if (gpu_stream == VKR_WORLD_GPU_CANDIDATE_STREAM_TRANSMISSION) {
          transmission_gpu_candidates[transmission_gpu_candidate_index++] =
              gpu_candidate;
        } else {
          gpu_candidates[gpu_candidate_index++] = gpu_candidate;
        }
      }
      if (flags == 0) {
        continue;
      }
      const float32_t mesh_distance =
          (transmissive || alpha.world_transparent)
              ? application_transparent_depth(view, mesh->model,
                                              submesh->center)
              : 0.0f;
      const uint64_t sort_key =
          (transmissive || alpha.world_transparent)
              ? application_pack_transparent_sort_key(mesh_distance, vis_slot)
              : 0u;
      const VkrDrawCandidate candidate = {
          .key =
              {
                  .geometry = ((uint64_t)submesh->geometry.id << 32) |
                              (uint64_t)submesh->geometry.generation,
                  .material = ((uint64_t)draw_material.id << 32) |
                              (uint64_t)draw_material.generation,
                  .binding_context =
                      position_dependent_ibl ? (uint64_t)vis_slot : 0u,
                  .first_index = submesh->first_index,
                  .index_count = submesh->index_count,
                  .vertex_offset = submesh->vertex_offset,
                  .domain = (uint32_t)submesh->pipeline_domain,
              },
          .model = mesh->model,
          .mesh = {.id = mesh_slot + 1u, .generation = 0},
          .geometry = submesh->geometry,
          .submesh_index = s,
          .object_id = object_id,
          .sort_key = sort_key,
      };

      if (flags & VKR_VISIBLE_CAMERA) {
        if (transmissive) {
          camera_transmission_cands[transmission_index++] = candidate;
        } else if (alpha.world_transparent) {
          camera_transparent_cands[transparent_index++] = candidate;
        } else {
          camera_opaque_cands[opaque_index++] = candidate;
        }
      }
      if ((flags & VKR_VISIBLE_SHADOW) && out_shadow) {
        VkrDrawCandidate shadow_candidate = candidate;
        shadow_candidate.key.binding_context = 0u;
        if (alpha.shadow_alpha_tested) {
          shadow_alpha_cands[shadow_alpha_index++] = shadow_candidate;
        } else {
          shadow_opaque_cands[shadow_opaque_index++] = shadow_candidate;
        }
      }
    }
  }

  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_live_index(
        &rf->mesh_manager, i, &instance_slot);
    if (!instance || !instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }

    VkrMeshAsset *asset =
        vkr_mesh_manager_get_asset(&rf->mesh_manager, instance->asset);
    if (!asset) {
      continue;
    }
    uint32_t object_id = 0;
    if (instance->render_id != 0) {
      object_id =
          vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE, instance->render_id);
    }

    uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, s);
      if (!submesh) {
        continue;
      }
      const uint8_t flags = visibility[vis_slot++];
      VkrMaterial *material = application_get_material(rf, submesh->material);
      const VkrMaterialHandle draw_material =
          material ? (VkrMaterialHandle){.id = material->id,
                                         .generation = material->generation}
                   : submesh->material;
      const VkrDrawAlphaRouting alpha =
          application_material_alpha_routing(rf, material);
      const bool8_t transmissive =
          application_material_is_transmissive(rf, material);
      const VkrWorldGpuCandidateStream gpu_stream =
          application_world_gpu_candidate_stream(alpha, transmissive);
      if (gpu_stream != VKR_WORLD_GPU_CANDIDATE_STREAM_NONE) {
        const Vec3 half_extents = vec3_scale(
            vec3_sub(submesh->max_extents, submesh->min_extents), 0.5f);
        VkrWorldDrawCandidate gpu_candidate = (VkrWorldDrawCandidate){
            .mesh = {.id = instance_slot + 1u,
                     .generation = instance->generation},
            .geometry = submesh->geometry,
            .submesh_index = s,
            .material = draw_material,
            .instance = {.model = instance->model, .object_id = object_id},
            .local_bounding_sphere = {submesh->center.x, submesh->center.y,
                                      submesh->center.z,
                                      vec3_length(half_extents)},
            .state_bucket = vkr_world_draw_state_bucket(
                alpha.shadow_alpha_tested ? VKR_MATERIAL_ALPHA_CUTOUT
                                          : VKR_MATERIAL_ALPHA_OPAQUE,
                material ? material->double_sided : false_v),
            .flags = instance->bounds_valid
                         ? VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID
                         : 0u,
        };
        if (gpu_stream == VKR_WORLD_GPU_CANDIDATE_STREAM_TRANSMISSION) {
          transmission_gpu_candidates[transmission_gpu_candidate_index++] =
              gpu_candidate;
        } else {
          gpu_candidates[gpu_candidate_index++] = gpu_candidate;
        }
      }
      if (flags == 0) {
        continue;
      }
      const float32_t instance_distance =
          (transmissive || alpha.world_transparent)
              ? application_transparent_depth(view, instance->model,
                                              submesh->center)
              : 0.0f;
      const uint64_t sort_key = (transmissive || alpha.world_transparent)
                                    ? application_pack_transparent_sort_key(
                                          instance_distance, vis_slot)
                                    : 0u;
      const VkrDrawCandidate candidate = {
          .key =
              {
                  .geometry = ((uint64_t)submesh->geometry.id << 32) |
                              (uint64_t)submesh->geometry.generation,
                  .material = ((uint64_t)draw_material.id << 32) |
                              (uint64_t)draw_material.generation,
                  .binding_context =
                      position_dependent_ibl ? (uint64_t)vis_slot : 0u,
                  .first_index = submesh->first_index,
                  .index_count = submesh->index_count,
                  .vertex_offset = submesh->vertex_offset,
                  .domain = (uint32_t)submesh->pipeline_domain,
              },
          .model = instance->model,
          .mesh = {.id = instance_slot + 1u,
                   .generation = instance->generation},
          .geometry = submesh->geometry,
          .submesh_index = s,
          .object_id = object_id,
          .sort_key = sort_key,
      };

      if (flags & VKR_VISIBLE_CAMERA) {
        if (transmissive) {
          camera_transmission_cands[transmission_index++] = candidate;
        } else if (alpha.world_transparent) {
          camera_transparent_cands[transparent_index++] = candidate;
        } else {
          camera_opaque_cands[opaque_index++] = candidate;
        }
      }
      if ((flags & VKR_VISIBLE_SHADOW) && out_shadow) {
        VkrDrawCandidate shadow_candidate = candidate;
        shadow_candidate.key.binding_context = 0u;
        if (alpha.shadow_alpha_tested) {
          shadow_alpha_cands[shadow_alpha_index++] = shadow_candidate;
        } else {
          shadow_opaque_cands[shadow_opaque_index++] = shadow_candidate;
        }
      }
    }
  }

  if (gpu_candidate_index != gpu_candidate_count ||
      transmission_gpu_candidate_index != transmission_gpu_candidate_count) {
    *out_payload = (VkrWorldPassPayload){0};
    if (out_shadow)
      *out_shadow = (VkrShadowDrawLists){0};
    return false_v;
  }

  // ---- Emission: opaque merges; transparent and alpha do not. ----
  // Transparent draws carry a back-to-front order and alpha-tested shadow draws
  // rebind per-draw material state, so collapsing either would change what is
  // drawn, not merely how many calls it takes.
  uint32_t merged_opaque_draws = 0;
  const uint32_t opaque_instances_written = vkr_draw_merge_candidates(
      camera_opaque_cands, camera_opaque_count, 0u, opaque_draws,
      &merged_opaque_draws, instances, &stats);

  if (camera_transmission_count > 1) {
    qsort(camera_transmission_cands, camera_transmission_count,
          sizeof(VkrDrawCandidate), vkr_draw_candidate_depth_compare);
  }
  vkr_draw_emit_unmerged(camera_transmission_cands, camera_transmission_count,
                         opaque_instances_written, transmission_draws,
                         instances);

  if (camera_transparent_count > 1) {
    qsort(camera_transparent_cands, camera_transparent_count,
          sizeof(VkrDrawCandidate), vkr_draw_candidate_depth_compare);
  }
  vkr_draw_emit_unmerged(camera_transparent_cands, camera_transparent_count,
                         opaque_instances_written + camera_transmission_count,
                         transparent_draws, instances);

  uint32_t merged_shadow_draws = 0;
  if (out_shadow) {
    const uint32_t shadow_written = vkr_draw_merge_candidates(
        shadow_opaque_cands, shadow_opaque_count, 0u, shadow_opaque_draws,
        &merged_shadow_draws, shadow_instances, NULL);
    vkr_draw_emit_unmerged(shadow_alpha_cands, shadow_alpha_count,
                           shadow_written, shadow_alpha_draws,
                           shadow_instances);
  }

  stats.opaque_draws_before_merge = camera_opaque_count;
  stats.opaque_draws_emitted = merged_opaque_draws;
  if (out_stats) {
    *out_stats = stats;
  }

  *out_payload = (VkrWorldPassPayload){
      .gpu_candidates = gpu_candidates,
      .gpu_candidate_count = gpu_candidate_count,
      .transmission_gpu_candidates = transmission_gpu_candidates,
      .transmission_gpu_candidate_count = transmission_gpu_candidate_count,
      .opaque_draws = opaque_draws,
      .opaque_draw_count = merged_opaque_draws,
      .transmission_draws = transmission_draws,
      .transmission_draw_count = camera_transmission_count,
      .transparent_draws = transparent_draws,
      .transparent_draw_count = camera_transparent_count,
      .instances = instances,
      .instance_count = world_instance_count,
  };
  if (out_shadow) {
    *out_shadow = (VkrShadowDrawLists){
        .opaque_draws = shadow_opaque_draws,
        .opaque_draw_count = merged_shadow_draws,
        .alpha_draws = shadow_alpha_draws,
        .alpha_draw_count = shadow_alpha_count,
        .instances = shadow_instances,
        .instance_count = shadow_instance_count,
    };
  }
  return true_v;
}
/**
 * @brief Draws a frame using the renderer.
 * This function is called once per frame from within the main application
 * loop
 * (`application_start`). It handles:
 * - Calling the user-defined `application_update()` function.
 * - Updating the input system state.
 * - Implementing frame rate limiting to match `target_frame_rate`.
 * - Calling the user-defined `application_draw_frame()` function.
 * Asserts that the application has been initialized and is running.
 * @param application Pointer to the initialized `Application` structure.
 * @param delta The time elapsed since the last frame, in seconds.
 */
void application_draw_frame(Application *application, float64_t delta) {
  assert(application != NULL && "Application is NULL");
  assert(bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) &&
         "Application is not running");

  VkrFrameSetup setup = {0};
  VkrRendererError prepare_err = VKR_RENDERER_ERROR_NONE;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.render_prepare) {
    prepare_err = vkr_renderer_prepare_frame(&application->renderer, &setup);
  }
  if (prepare_err != VKR_RENDERER_ERROR_NONE) {
    // A minimized or resizing window skips frames as a matter of course; this
    // path runs every tick while minimized, so it must not log.
    if (prepare_err != VKR_RENDERER_ERROR_FRAME_SKIPPED) {
      String8 err = vkr_renderer_get_error_string(prepare_err);
      log_error("Failed to prepare renderer frame: %s", string8_cstr(&err));
      if (prepare_err == VKR_RENDERER_ERROR_DEVICE_ERROR) {
        log_fatal("Renderer device is unusable; stopping");
        bitset8_clear(&application->app_flags, APPLICATION_FLAG_RUNNING);
      }
    }
    return;
  }

  VkrAllocator *scratch = &application->renderer.scratch_allocator;

  // Shadow frame data is fetched before the payload because the caster list is
  // built from the light's volume, not from the camera-culled list.
  VkrShadowFrameData shadow_frame = {0};
  uint32_t shadow_cascade_count = 0;
  const bool8_t shadows_active =
      application->renderer.shadow_system.initialized &&
      application->renderer.lighting_system.directional.enabled;
  if (shadows_active) {
    vkr_shadow_system_get_frame_data(&application->renderer.shadow_system,
                                     setup.image_index, &shadow_frame);
    shadow_cascade_count = shadow_frame.cascade_count;
    if (shadow_cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX) {
      log_error("Shadow frame returned %u cascades; clamping to %u",
                shadow_cascade_count, VKR_SHADOW_CASCADE_COUNT_MAX);
      shadow_cascade_count = VKR_SHADOW_CASCADE_COUNT_MAX;
    }
  }

  // Cascade projections have different centers and are not generally nested.
  // A caster survives if it intersects any cascade; testing only the last one
  // can drop near-cascade shadows near the edge of its shifted far volume.
  VkrFrustum shadow_frustums[VKR_SHADOW_CASCADE_COUNT_MAX] = {0};
  for (uint32_t i = 0; i < shadow_cascade_count; ++i) {
    shadow_frustums[i] =
        vkr_frustum_from_matrix(shadow_frame.view_projection[i]);
  }

  VkrWorldPassPayload world_payload = {0};
  VkrShadowDrawLists shadow_lists = {0};
  VkrVisibilityStats visibility_stats = {0};
  bool8_t has_world = application_build_world_payload(
      application, scratch, shadow_cascade_count > 0 ? shadow_frustums : NULL,
      shadow_cascade_count, &world_payload,
      shadow_cascade_count > 0 ? &shadow_lists : NULL, &visibility_stats);
  application->visibility_stats = visibility_stats;

  VkrShadowPassPayload shadow_payload = {0};
  bool8_t has_shadow = false_v;
  if (has_world && shadow_cascade_count > 0) {
    shadow_payload.cascade_count = shadow_cascade_count;
    for (uint32_t i = 0; i < shadow_cascade_count; ++i) {
      shadow_payload.light_view_proj[i] = shadow_frame.view_projection[i];
      shadow_payload.split_depths[i] = shadow_frame.split_far[i];
    }
    shadow_payload.opaque_draws = shadow_lists.opaque_draws;
    shadow_payload.opaque_draw_count = shadow_lists.opaque_draw_count;
    shadow_payload.alpha_draws = shadow_lists.alpha_draws;
    shadow_payload.alpha_draw_count = shadow_lists.alpha_draw_count;
    shadow_payload.instances = shadow_lists.instances;
    shadow_payload.instance_count = shadow_lists.instance_count;
    shadow_payload.config_override = NULL;
    has_shadow = shadow_payload.opaque_draw_count > 0 ||
                 shadow_payload.alpha_draw_count > 0;
  }

  VkrPickingPassPayload picking_payload = {0};
  bool8_t has_picking =
      application->renderer.picking.state == VKR_PICKING_STATE_RENDER_PENDING;
  /* An identifier capture has no producer unless the picking pass runs this
     frame, so the request itself schedules it. The catalog names the dependency
     as a subsystem; matching on the channel name instead would silently stop
     working the moment a channel is renamed. */
  if (!has_picking && application->capture_request) {
    for (uint32_t i = 0; i < application->capture_request->item_count; ++i) {
      const VkrCaptureChannelDescription *channel =
          vkr_renderer_capture_channel_get(
              application->capture_request->items[i].channel);
      if (channel &&
          channel->required_subsystem == VKR_RENDERER_SUBSYSTEM_PICKING) {
        has_picking = true_v;
        break;
      }
    }
  }
  if (has_picking) {
    picking_payload.pending = true_v;
    picking_payload.x = application->renderer.picking.requested_x;
    picking_payload.y = application->renderer.picking.requested_y;
    if (has_world) {
      const uint32_t picking_draw_count =
          world_payload.opaque_draw_count +
          world_payload.transmission_draw_count +
          world_payload.transparent_draw_count;
      VkrDrawItem *picking_draws =
          picking_draw_count > 0
              ? vkr_allocator_alloc(scratch,
                                    (uint64_t)picking_draw_count *
                                        sizeof(*picking_draws),
                                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
              : NULL;
      if (picking_draw_count == 0 || picking_draws) {
        uint32_t offset = 0;
#define VKR_COPY_PICKING_DRAWS(FIELD)                                          \
  do {                                                                         \
    if (world_payload.FIELD##_draw_count > 0) {                                \
      MemCopy(picking_draws + offset, world_payload.FIELD##_draws,             \
              (uint64_t)world_payload.FIELD##_draw_count *                     \
                  sizeof(*picking_draws));                                     \
      offset += world_payload.FIELD##_draw_count;                              \
    }                                                                          \
  } while (0)
        VKR_COPY_PICKING_DRAWS(opaque);
        VKR_COPY_PICKING_DRAWS(transmission);
        VKR_COPY_PICKING_DRAWS(transparent);
#undef VKR_COPY_PICKING_DRAWS
        picking_payload.draws = picking_draws;
        picking_payload.draw_count = picking_draw_count;
        picking_payload.instances = world_payload.instances;
        picking_payload.instance_count = world_payload.instance_count;
      } else {
        log_error("Unable to allocate the frame picking draw list");
        has_picking = false_v;
      }
    }
  }

  bool8_t editor_enabled = application->editor_viewport.enabled &&
                           application->renderer.editor_viewport.initialized;
  bool8_t has_editor = false_v;
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;
  VkrViewportMapping editor_mapping = {0};
  VkrDrawItem editor_draws[1] = {0};
  VkrInstanceDataGPU editor_instances[1] = {0};
  VkrEditorPassPayload editor_payload = {0};

  if (editor_enabled) {
    if (vkr_editor_viewport_compute_mapping(
            setup.window_width, setup.window_height,
            application->editor_viewport.fit_mode,
            application->editor_viewport.render_scale, &editor_mapping) &&
        vkr_editor_viewport_build_payload(
            &application->renderer.editor_viewport, &editor_mapping,
            editor_draws, editor_instances, &editor_payload)) {
      viewport_width = editor_mapping.target_width;
      viewport_height = editor_mapping.target_height;
      has_editor = true_v;
    } else {
      editor_enabled = false_v;
    }
  }

  if (editor_enabled) {
    if (viewport_width != application->editor_viewport.last_target_width ||
        viewport_height != application->editor_viewport.last_target_height) {
      vkr_camera_registry_resize_all(&application->renderer.camera_system,
                                     viewport_width, viewport_height);
      application->editor_viewport.last_target_width = viewport_width;
      application->editor_viewport.last_target_height = viewport_height;
    }
  } else if (application->editor_viewport.last_target_width != 0 ||
             application->editor_viewport.last_target_height != 0) {
    vkr_camera_registry_resize_all(&application->renderer.camera_system,
                                   setup.window_width, setup.window_height);
    application->editor_viewport.last_target_width = 0;
    application->editor_viewport.last_target_height = 0;
  }

  VkrUiPassPayload ui_payload = {0};
  const VkrScene *active_scene = application->renderer.active_scene;
  VkrSkyboxPassPayload skybox_payload = {
      .cubemap = application->renderer.skybox_system.initialized
                     ? application->renderer.skybox_system.cube_map_texture
                     : VKR_TEXTURE_HANDLE_INVALID,
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
  } else if (application->renderer.world_resources.ibl_default_ready) {
    frame_ibl_source =
        application->renderer.world_resources.ibl_fallback_source_cubemap;
  } else {
    frame_ibl_source = skybox_payload.cubemap;
  }
  bool8_t frame_ibl_enabled = application->renderer.material_system.ibl_enabled;
  float32_t frame_ibl_intensity =
      application->renderer.material_system.ibl_intensity;
  float32_t frame_ibl_diffuse_intensity =
      application->renderer.material_system.ibl_diffuse_intensity;
  float32_t frame_ibl_specular_intensity =
      application->renderer.material_system.ibl_specular_intensity;
  if (scene_environment) {
    frame_ibl_enabled = true_v;
    frame_ibl_intensity = scene_environment->intensity;
    frame_ibl_diffuse_intensity = scene_environment->diffuse_intensity;
    frame_ibl_specular_intensity = scene_environment->specular_intensity;
  } else if (!frame_ibl_enabled && frame_ibl_source.id != 0) {
    frame_ibl_enabled = true_v;
  }

  VkrTextUpdate world_text_updates[VKR_MAX_PENDING_TEXT_UPDATES];
  VkrTextUpdate ui_text_updates[VKR_MAX_PENDING_TEXT_UPDATES];
  VkrTextUpdatesPayload text_updates_payload = {0};
  bool8_t has_text_updates = false_v;

  if (application->world_text_update_count > 0) {
    uint32_t count = application->world_text_update_count;
    if (count > VKR_MAX_PENDING_TEXT_UPDATES) {
      count = VKR_MAX_PENDING_TEXT_UPDATES;
    }
    for (uint32_t i = 0; i < count; ++i) {
      ApplicationTextUpdate *pending = &application->world_text_updates[i];
      world_text_updates[i] = (VkrTextUpdate){
          .text_id = pending->text_id,
          .content = pending->content,
          .transform = pending->has_transform ? &pending->transform : NULL,
      };
    }
    text_updates_payload.world_text_updates = world_text_updates;
    text_updates_payload.world_text_update_count = count;
    has_text_updates = true_v;
  }

  if (application->ui_text_update_count > 0) {
    uint32_t count = application->ui_text_update_count;
    if (count > VKR_MAX_PENDING_TEXT_UPDATES) {
      count = VKR_MAX_PENDING_TEXT_UPDATES;
    }
    for (uint32_t i = 0; i < count; ++i) {
      ApplicationTextUpdate *pending = &application->ui_text_updates[i];
      ui_text_updates[i] = (VkrTextUpdate){
          .text_id = pending->text_id,
          .content = pending->content,
          .transform = NULL,
      };
    }
    text_updates_payload.ui_text_updates = ui_text_updates;
    text_updates_payload.ui_text_update_count = count;
    has_text_updates = true_v;
  }

  // The metrics config is the single authority for whether timestamps are
  // recorded. A second copy could drift and make a report claim timestamps
  // were on for a run that never took one, which would silently mislabel the
  // run's comparison configuration.
  const bool8_t gpu_timing = application->metrics->config.pass_gpu_timings;
  VkrGpuDebugPayload debug_payload = {
      .enable_timing = gpu_timing,
      .capture_pass_timestamps = gpu_timing,
      .capture = application->capture_request,
  };
  const VkrGpuDebugPayload *debug_ptr =
      (gpu_timing || application->capture_request) ? &debug_payload : NULL;
  VkrFrameIblProbe frame_ibl_probes[VKR_FRAME_IBL_PROBE_MAX] = {0};
  uint32_t frame_ibl_probe_count = 0;
  if (active_scene) {
    for (uint32_t i = 0; i < active_scene->reflection_probe_count &&
                         frame_ibl_probe_count < VKR_FRAME_IBL_PROBE_MAX;
         ++i) {
      const VkrSceneReflectionProbe *probe =
          &active_scene->reflection_probes[i];
      if (!probe->enabled ||
          probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY ||
          probe->irradiance_cubemap.id == 0 ||
          probe->irradiance_cubemap.generation == VKR_INVALID_ID ||
          probe->prefilter_cubemap.id == 0 ||
          probe->prefilter_cubemap.generation == VKR_INVALID_ID) {
        continue;
      }
      frame_ibl_probes[frame_ibl_probe_count++] = (VkrFrameIblProbe){
          .irradiance = probe->irradiance_cubemap,
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
      .directional_enabled =
          application->renderer.lighting_system.directional.enabled,
      .directional_direction =
          application->renderer.lighting_system.directional.direction,
      .directional_color =
          application->renderer.lighting_system.directional.color,
      .directional_intensity =
          application->renderer.lighting_system.directional.intensity,
      .ibl_enabled = frame_ibl_enabled,
      .ibl_source = frame_ibl_source,
      .ibl_intensity = frame_ibl_intensity,
      .ibl_diffuse_intensity = frame_ibl_diffuse_intensity,
      .ibl_specular_intensity = frame_ibl_specular_intensity,
      .point_lights = application->renderer.lighting_system.point_lights,
      .point_light_count =
          application->renderer.lighting_system.point_light_count,
      .point_light_grid =
          &application->renderer.lighting_system.point_light_grid,
      .ibl_probes = frame_ibl_probes,
      .ibl_probe_count = frame_ibl_probe_count,
  };

  VkrRenderPacket packet = {
      .packet_version = VKR_RENDER_PACKET_VERSION,
      .frame =
          {
              .frame_index = (uint32_t)application->renderer.frame_number,
              .delta_time = delta,
              .window_width = setup.window_width,
              .window_height = setup.window_height,
              .viewport_width = viewport_width,
              .viewport_height = viewport_height,
              .editor_enabled = editor_enabled,
          },
      .globals =
          {
              .view = application->renderer.globals.view,
              .projection = application->renderer.globals.projection,
              .view_position = application->renderer.globals.view_position,
              .ambient_color = application->renderer.globals.ambient_color,
              .exposure = application->renderer.globals.exposure,
              .render_mode =
                  (uint32_t)application->renderer.globals.render_mode,
          },
      .lighting = &frame_lighting,
      .world = has_world ? &world_payload : NULL,
      .shadow = has_shadow ? &shadow_payload : NULL,
      .skybox = !application->config->disable_skybox &&
                        skybox_payload.cubemap.id != 0 &&
                        skybox_payload.cubemap.generation != VKR_INVALID_ID
                    ? &skybox_payload
                    : NULL,
      .ui = &ui_payload,
      .editor = has_editor ? &editor_payload : NULL,
      .picking = has_picking ? &picking_payload : NULL,
      .text_updates = has_text_updates ? &text_updates_payload : NULL,
      .debug = debug_ptr,
  };

  VkrRendererFrameMetrics metrics = {0};
  VkrValidationError validation = {0};
  VkrRendererError submit_err = VKR_RENDERER_ERROR_NONE;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.render_submit) {
    submit_err = vkr_renderer_submit_packet(&application->renderer, &packet,
                                            &metrics, &validation);
  }
  application->last_renderer_error = submit_err;
  if (submit_err != VKR_RENDERER_ERROR_CAPTURE_BUSY) {
    application->capture_request = NULL;
  }
#if VKR_METRICS_ENABLED
  VkrRendererMetricsCollectContext metrics_context = {
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
  if (submit_err != VKR_RENDERER_ERROR_NONE) {
    if (validation.field_path && validation.message) {
      log_error("Packet validation failed: %s (%s)", validation.field_path,
                validation.message);
    } else {
      String8 err = vkr_renderer_get_error_string(submit_err);
      log_error("Packet submit failed: %s", string8_cstr(&err));
    }
    if (submit_err == VKR_RENDERER_ERROR_DEVICE_ERROR) {
      log_fatal("Renderer device is unusable; stopping");
      bitset8_clear(&application->app_flags, APPLICATION_FLAG_RUNNING);
    }
  }
}

/**
 * @brief Starts the main application loop.
 * This function contains the core loop that drives the application. It
 * handles:
 * - Updating the application clock and calculating delta time.
 * - Processing window events (input, close requests, etc.).
 * - Calling the user-defined `application_update()` function.
 * - Updating the input system state.
 * - Implementing frame rate limiting to match `target_frame_rate`.
 * The loop continues until the application is no longer running (e.g.,
 * `application_close()` is called or the window is closed).
 * Asserts that the application has been initialized.
 * @param application Pointer to the initialized `Application` structure.
 */
void application_start(Application *application) {
  assert(application != NULL && "Application is NULL");
  assert(
      bitset8_is_set(&application->app_flags, APPLICATION_FLAG_INITIALIZED) &&
      "Application is not initialized");

  bitset8_set(&application->app_flags, APPLICATION_FLAG_RUNNING);

  log_info("Application is running...");

  vkr_clock_start(&application->clock);
  vkr_clock_update(&application->clock);
  application->last_frame_time = application->clock.elapsed;

  float64_t target_frame_seconds = 0.0;
  if (application->config->target_frame_rate > 0) {
    target_frame_seconds =
        1.0 / (float64_t)application->config->target_frame_rate;
  }

  bool8_t running = true_v;
  while (
      running &&
      bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) &&
      bitset8_is_set(&application->app_flags, APPLICATION_FLAG_INITIALIZED)) {

    vkr_clock_update(&application->clock);

    float64_t current_absolute_time = vkr_platform_get_absolute_time();
    float64_t current_total_time = application->clock.elapsed;

    float64_t delta = application->config->fixed_delta_seconds > 0.0
                          ? application->config->fixed_delta_seconds
                          : current_total_time - application->last_frame_time;

    if (delta > 0.1f) {
      delta = 0.1f;
    }

    if (delta <= 0.0) {
      delta = target_frame_seconds > 0.0 ? target_frame_seconds : (1.0 / 60.0);
    }

    // Without a window there is no close request or input device to poll; an
    // offscreen run ends through its own exit condition.
    if (application_is_windowed(application)) {
      running = vkr_window_update(&application->window);
      vkr_gamepad_poll_all(&application->gamepad);
    }

    if (!running ||
        bitset8_is_set(&application->app_flags, APPLICATION_FLAG_SUSPENDED)) {
      application->last_frame_time = current_total_time;
      if (!running) {
        break;
      }
      continue;
    }

    vkr_metrics_begin_frame(
        application->metrics, application->renderer.frame_number + 1u,
        vkr_renderer_get_submit_serial(&application->renderer));

    VkrAllocatorScope frame_scope = {0};
    VkrAllocator *frame_alloc = &application->renderer.scratch_allocator;
    if (vkr_allocator_supports_scopes(frame_alloc)) {
      frame_scope = vkr_allocator_begin_scope(frame_alloc);
    }
    application->ui_text_update_count = 0;
    application->world_text_update_count = 0;

    VKR_METRICS_SCOPE_NS(application->metrics, application->metric_ids.update) {
      application_update(application, delta);
    }

    // `application_update()` may request shutdown (for example via auto-close).
    // Stop this frame immediately to avoid recording/render calls after
    // APPLICATION_FLAG_RUNNING has been cleared.
    if (!bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) ||
        !bitset8_is_set(&application->app_flags,
                        APPLICATION_FLAG_INITIALIZED)) {
      if (vkr_allocator_scope_is_valid(&frame_scope)) {
        vkr_allocator_end_scope(&frame_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
      break;
    }

    VkrCameraSystem *camera_system = &application->renderer.camera_system;
    VkrCameraHandle active_camera =
        vkr_camera_registry_get_active(camera_system);
    application->renderer.active_camera = active_camera;
    VkrCamera *camera =
        vkr_camera_registry_get_by_handle(camera_system, active_camera);

    if (camera) {
      application->renderer.camera_controller.camera = camera;
    } else {
      log_warn("Active camera handle invalid; skipping controller update");
    }

    if (camera && !application->config->disable_camera_controller) {
      vkr_camera_controller_update(&application->renderer.camera_controller,
                                   delta);
    }

    vkr_camera_registry_update_all(camera_system);

    if (application->renderer.active_scene) {
      vkr_lighting_system_sync_from_scene(
          &application->renderer.lighting_system,
          application->renderer.active_scene);
    }

    if (camera) {
      vkr_shadow_system_update(
          &application->renderer.shadow_system, camera,
          application->renderer.lighting_system.directional.enabled,
          application->renderer.lighting_system.directional.direction);
    }

    // Update world view/projection from camera each frame to reflect movement
    application->renderer.globals.view =
        vkr_camera_registry_get_view(camera_system, active_camera);
    application->renderer.globals.projection =
        vkr_camera_registry_get_projection(camera_system, active_camera);
    if (camera) {
      application->renderer.globals.view_position = camera->position;
    }

    uint32_t mesh_capacity =
        vkr_mesh_manager_capacity(&application->renderer.mesh_manager);
    for (uint32_t mesh_index = 0; mesh_index < mesh_capacity; ++mesh_index) {
      VkrMesh *mesh =
          vkr_mesh_manager_get(&application->renderer.mesh_manager, mesh_index);
      if (!mesh) {
        continue;
      }

      // Scene-driven meshes update their model via the scene bridge; avoid
      // overwriting those transforms with the mesh-local transform.
      if (mesh->render_id != 0) {
        continue;
      }

      vkr_mesh_manager_update_model(&application->renderer.mesh_manager,
                                    mesh_index);
    }

    if (!bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) ||
        !bitset8_is_set(&application->app_flags,
                        APPLICATION_FLAG_INITIALIZED)) {
      if (vkr_allocator_scope_is_valid(&frame_scope)) {
        vkr_allocator_end_scope(&frame_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
      // Publish before leaving: this is the shutdown path an auto-closing run
      // takes, and dropping it would make the final snapshot one frame stale.
      vkr_metrics_end_frame(application->metrics);
      break;
    }

    application_draw_frame(application, delta);

    VKR_METRICS_ADD_ELAPSED_NS(application->metrics,
                               application->metric_ids.frame_work,
                               current_absolute_time);

    if (vkr_allocator_scope_is_valid(&frame_scope)) {
      vkr_allocator_end_scope(&frame_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }

    if (application->config->target_frame_rate > 0) {
      // Frame limiting / yielding CPU
      float64_t frame_end_time = vkr_platform_get_absolute_time();
      float64_t frame_elapsed_work_time =
          frame_end_time - current_absolute_time;

      float64_t remaining_seconds =
          target_frame_seconds - frame_elapsed_work_time;

      if (remaining_seconds > 0.0) {
        uint64_t remaining_ms = (uint64_t)(remaining_seconds * 1000.0);

        if (remaining_ms > 0) {
          VKR_METRICS_SCOPE_NS(application->metrics,
                               application->metric_ids.limiter_sleep) {
            vkr_platform_sleep(remaining_ms);
          }
        }
      }
    }

    VKR_METRICS_ADD_ELAPSED_NS(application->metrics,
                               application->metric_ids.frame_wall,
                               current_absolute_time);
    vkr_metrics_end_frame(application->metrics);

    application->last_frame_time = current_total_time;

    if (application_is_windowed(application)) {
      input_update(&application->window.input_state);
    }
  }
}

/**
 * @brief Stops or suspends the application's main loop.
 * Sets the `APPLICATION_FLAG_SUSPENDED` flag, causing the main loop in
 * `application_start` to pause processing application updates.
 * Dispatches an `EVENT_TYPE_APPLICATION_STOP` event.
 * Asserts that the application is currently running.
 * @param application Pointer to the `Application` structure.
 */
void application_stop(Application *application) {
  assert(application != NULL && "Application is NULL");
  assert(bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) &&
         "Application is not running");

  event_manager_dispatch(&application->event_manager,
                         (Event){.type = EVENT_TYPE_APPLICATION_STOP});

  bitset8_set(&application->app_flags, APPLICATION_FLAG_SUSPENDED);
}

/**
 * @brief Resumes a previously stopped or suspended application.
 * Clears the `APPLICATION_FLAG_SUSPENDED` flag, allowing the main loop in
 * `application_start` to continue processing updates.
 * Dispatches an `EVENT_TYPE_APPLICATION_RESUME` event.
 * Asserts that the application is currently suspended.
 * @param application Pointer to the `Application` structure.
 */
void application_resume(Application *application) {
  assert(application != NULL && "Application is NULL");
  assert(bitset8_is_set(&application->app_flags, APPLICATION_FLAG_SUSPENDED) &&
         "Application is not suspended");

  event_manager_dispatch(&application->event_manager,
                         (Event){.type = EVENT_TYPE_APPLICATION_RESUME});

  bitset8_clear(&application->app_flags, APPLICATION_FLAG_SUSPENDED);
}

/**
 * @brief Signals the application's main loop to terminate.
 * Clears the `APPLICATION_FLAG_RUNNING` flag, which will cause the `while`
 * condition in `application_start` to become false, leading to loop exit.
 * This call is idempotent to support shutdown paths that may request close
 * from both update-time logic and post-loop teardown.
 * @param application Pointer to the `Application` structure.
 */
void application_close(Application *application) {
  assert(application != NULL && "Application is NULL");

  if (!bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING)) {
    return;
  }

  bitset8_clear(&application->app_flags, APPLICATION_FLAG_RUNNING);
}

/**
 * @brief Shuts down the application and releases all associated resources.
 * This function should be called after the main loop has terminated (e.g.,
 * after `application_start` returns). It dispatches an
 * `EVENT_TYPE_APPLICATION_SHUTDOWN` event, then destroys the window, event
 * manager, and all application-specific memory arenas. Asserts that the
 * application is not still marked as running.
 * @param application Pointer to the `Application` structure to be shut down.
 */
void application_shutdown(Application *application) {
  assert_log(application != NULL, "Application is NULL");
  assert_log(!bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING),
             "Application is still running");

  log_info("Application shutting down...");

  event_manager_dispatch(&application->event_manager,
                         (Event){.type = EVENT_TYPE_APPLICATION_SHUTDOWN});

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

  vkr_renderer_destroy(&application->renderer);
  if (application_is_windowed(application)) {
    vkr_window_destroy(&application->window);
  }
  event_manager_destroy(&application->event_manager);
  vkr_mutex_destroy(&application->app_allocator, &application->app_mutex);
  if (application_is_windowed(application)) {
    vkr_gamepad_shutdown(&application->gamepad);
  }

  arena_destroy(application->metrics_arena);
  application->metrics_arena = NULL;
  application->metrics = NULL;

  vkr_platform_shutdown();

  arena_destroy(application->log_arena);
  arena_destroy(application->app_arena);
}

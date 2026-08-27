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
  // Nested inside update and frame_work respectively. Both are frontend work
  // that runs before any backend sees the packet, so they belong here rather
  // than in the renderer catalog.
  VkrMetricId shadow_update;
  VkrMetricId world_payload_build;
} ApplicationMetricIds;

/**
 * @brief Main structure representing the application.
 * Encapsulates all core components, state, and resources needed for the
 * application to run.
 */
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
  /* GPU timing policy is owned by `metrics->config`. */
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
      // Shadow update is absent without an active camera. Keep both optional so
      // non-rendering and partial-boot frames do not make reports incomplete.
      !application_register_duration_metric(
          application->metrics, "cpu.shadow_update", VKR_METRIC_DOMAIN_FRAME,
          false_v, &ids->shadow_update) ||
      !application_register_duration_metric(
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
    fprintf(stderr, "Failed to initialize platform\n");
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
  return vkr_material_system_get_live(&rf->material_system, handle);
}

vkr_internal VkrDrawAlphaRouting application_material_alpha_routing(
    RendererFrontend *rf, VkrMaterial *material) {
  return vkr_draw_alpha_routing(
      vkr_material_system_material_alpha_mode(&rf->material_system, material));
}

vkr_internal bool8_t application_material_is_transmissive(
    RendererFrontend *rf, VkrMaterial *material) {
  return vkr_material_system_material_is_transmissive(&rf->material_system,
                                                      material);
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

typedef struct ApplicationWorldSource {
  VkrMeshHandle mesh;
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  Mat4 model;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  VkrDrawAlphaRouting alpha;
  uint32_t submesh_index;
  uint32_t object_id;
  uint32_t temporal_index;
  uint32_t temporal_generation;
  bool8_t bounds_valid;
  bool8_t transmissive;
  bool8_t double_sided;
  VkrShadowCasterMobility shadow_mobility;
} ApplicationWorldSource;

typedef struct ApplicationWorldEmitContext {
  Mat4 view;
  const uint8_t *transparent_visible;
  VkrWorldDrawCandidate *gpu_candidates;
  VkrWorldDrawCandidate *transmission_gpu_candidates;
  VkrTransparentDrawCandidate *transparent_candidates;
  uint32_t source_index;
  uint32_t gpu_index;
  uint32_t transmission_index;
  uint32_t transparent_index;
  /**
   * Emission runs twice so the candidate stream comes out partitioned with
   * static casters first, which is what cascade reuse tests against. Only
   * sources whose mobility matches `gpu_mobility` are written to
   * `gpu_candidates`; `source_index` still advances for every source in both
   * passes, so the `transparent_visible` mapping stays keyed to encounter
   * order rather than to emission order.
   *
   * The transmission and transparent streams are emitted in the first pass
   * only, keeping their existing encounter order untouched.
   */
  VkrShadowCasterMobility gpu_mobility;
  bool8_t emit_side_streams;
} ApplicationWorldEmitContext;

vkr_internal inline void
application_emit_world_source(ApplicationWorldEmitContext *context,
                              const ApplicationWorldSource *source) {
  const Vec3 half_extents =
      vec3_scale(vec3_sub(source->max_extents, source->min_extents), 0.5f);
  const VkrWorldDrawCandidate candidate = {
      .mesh = source->mesh,
      .geometry = source->geometry,
      .submesh_index = source->submesh_index,
      .material = source->material,
      .instance =
          {
              .model = source->model,
              .object_id = source->object_id,
              .temporal_index = source->temporal_index,
              .temporal_generation = source->temporal_generation,
          },
      .local_bounding_sphere = {source->center.x, source->center.y,
                                source->center.z, vec3_length(half_extents)},
      .state_bucket = vkr_world_draw_state_bucket(
          source->alpha.shadow_alpha_tested ? VKR_MATERIAL_ALPHA_CUTOUT
                                            : VKR_MATERIAL_ALPHA_OPAQUE,
          source->double_sided),
      .flags =
          (source->bounds_valid ? VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID : 0u) |
          (!source->transmissive && !source->alpha.world_transparent
               ? VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE
               : 0u) |
          VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER,
  };
  if (source->shadow_mobility == context->gpu_mobility)
    context->gpu_candidates[context->gpu_index++] = candidate;
  if (context->emit_side_streams && source->transmissive)
    context->transmission_gpu_candidates[context->transmission_index++] =
        candidate;
  if (context->emit_side_streams && !source->transmissive &&
      source->alpha.world_transparent &&
      context->transparent_visible[context->source_index]) {
    const float32_t depth = application_transparent_depth(
        context->view, source->model, source->center);
    context->transparent_candidates[context->transparent_index++] =
        (VkrTransparentDrawCandidate){
            .model = source->model,
            .mesh = source->mesh,
            .geometry = source->geometry,
            .material = source->material,
            .submesh_index = source->submesh_index,
            .object_id = source->object_id,
            .sort_key = application_pack_transparent_sort_key(
                depth, context->source_index + 1u),
        };
  }
  context->source_index++;
}

/**
 * Grows a world-space AABB by one caster's bounding sphere.
 *
 * A sphere rather than the mesh's own AABB because that is what the renderer
 * already maintains; it over-covers, which is the safe direction for a volume
 * that must contain every caster.
 */
vkr_internal inline void
application_accumulate_caster_bounds(Vec3 *min, Vec3 *max, bool8_t *valid,
                                     Vec3 center, float32_t radius) {
  min->x = vkr_min_f32(min->x, center.x - radius);
  min->y = vkr_min_f32(min->y, center.y - radius);
  min->z = vkr_min_f32(min->z, center.z - radius);
  max->x = vkr_max_f32(max->x, center.x + radius);
  max->y = vkr_max_f32(max->y, center.y + radius);
  max->z = vkr_max_f32(max->z, center.z + radius);
  *valid = true_v;
}

/**
 * @brief Measures the world-space AABB of every visible, loaded caster.
 *
 * Runs its own traversal rather than reusing the payload build's, because
 * cascade fitting happens during update and the payload is built later in the
 * frame. A one-frame-stale interval would be the cheaper option and the wrong
 * one: the Z fit clips casters, so lagging it by a frame can drop the shadow of
 * something that just moved. The traversal is a bounds min/max per instance
 * with no allocation, against a CPU that measurement showed is ~95% idle.
 */
vkr_internal void
application_measure_caster_bounds(Application *application,
                                  VkrShadowCasterDepthBounds *out_bounds) {
  RendererFrontend *rf = &application->renderer;
  Vec3 min = {VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX};
  Vec3 max = {-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX};
  bool8_t valid = false_v;

  const uint32_t mesh_count = vkr_mesh_manager_count(&rf->mesh_manager);
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager,
                                                            i, &mesh_slot);
    if (!mesh || !mesh->visible || !mesh->bounds_valid ||
        mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    application_accumulate_caster_bounds(&min, &max, &valid,
                                         mesh->bounds_world_center,
                                         mesh->bounds_world_radius);
  }

  const uint32_t instance_count =
      vkr_mesh_manager_instance_count(&rf->mesh_manager);
  for (uint32_t i = 0; i < instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_live_index(
        &rf->mesh_manager, i, &instance_slot);
    if (!instance || !instance->visible || !instance->bounds_valid ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    application_accumulate_caster_bounds(&min, &max, &valid,
                                         instance->bounds_world_center,
                                         instance->bounds_world_radius);
  }

  *out_bounds = (VkrShadowCasterDepthBounds){
      .min = min,
      .max = max,
      .valid = valid,
  };
}

/**
 * @brief Builds the sole GPU-driven world source and retained blend list.
 *
 * Opaque, cutout, transmission, and shadow visibility remain unculled packet
 * candidates; the selected backend owns their multi-view classification.
 * Ordinary alpha blend is the only camera-culled and depth-sorted CPU list.
 */
vkr_internal bool8_t application_build_world_payload(
    Application *application, VkrAllocator *scratch,
    VkrWorldPassPayload *out_payload, VkrVisibilityStats *out_stats) {
  RendererFrontend *rf = &application->renderer;
  vkr_material_system_begin_texture_residency_frame(&rf->material_system);
  const Mat4 view = rf->globals.view;
  const VkrFrustum camera_frustum =
      vkr_frustum_from_view_projection(view, rf->globals.projection);
  const uint32_t mesh_count = vkr_mesh_manager_count(&rf->mesh_manager);
  const uint32_t live_instance_count =
      vkr_mesh_manager_instance_count(&rf->mesh_manager);
  const uint32_t temporal_instance_offset =
      vkr_mesh_manager_capacity(&rf->mesh_manager);
  const uint64_t temporal_slot_capacity =
      (uint64_t)temporal_instance_offset +
      vkr_mesh_manager_instance_capacity(&rf->mesh_manager);
  if (temporal_slot_capacity > VKR_TEMPORAL_TRANSFORM_CAPACITY) {
    *out_payload = (VkrWorldPassPayload){0};
    return false_v;
  }
  VkrVisibilityStats stats = {0};
  uint64_t candidate_count_64 = 0u;
  /* True when a visible caster exists that has not finished loading. Its
     geometry is absent from the candidate stream, so a cascade cannot be
     reused: the missing caster may belong inside the volume. */
  bool8_t publication_pending =
      !rf->asset_publisher.publications_idle ||
      !rf->asset_publisher.publications_idle(rf->asset_publisher.state);
  const uint64_t publication_generation =
      rf->asset_publisher.publication_generation
          ? rf->asset_publisher.publication_generation(
                rf->asset_publisher.state)
          : 1u;

  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager,
                                                            i, &mesh_slot);
    if (mesh->visible && mesh->loading_state == VKR_MESH_LOADING_STATE_LOADED) {
      candidate_count_64 += vkr_mesh_manager_submesh_count(mesh);
    } else if (mesh->visible) {
      publication_pending = true_v;
    }
  }
  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_live_index(
        &rf->mesh_manager, i, &instance_slot);
    if (!instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      publication_pending = publication_pending || instance->visible;
      continue;
    }
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_live_asset(&rf->mesh_manager, instance->asset);
    candidate_count_64 += asset->submeshes.length;
  }

  if (candidate_count_64 > VKR_GPU_DRAW_CANDIDATE_CAPACITY) {
    *out_payload = (VkrWorldPassPayload){
        .gpu_candidate_count = VKR_GPU_DRAW_CANDIDATE_CAPACITY + 1u,
    };
    stats.objects_tested = VKR_GPU_DRAW_CANDIDATE_CAPACITY + 1u;
    if (out_stats)
      *out_stats = stats;
    return true_v;
  }

  const uint32_t gpu_candidate_count = (uint32_t)candidate_count_64;
  uint8_t *transparent_visible = NULL;
  if (gpu_candidate_count > 0u) {
    transparent_visible = vkr_allocator_alloc(scratch, gpu_candidate_count,
                                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!transparent_visible) {
      *out_payload = (VkrWorldPassPayload){0};
      return false_v;
    }
    MemZero(transparent_visible, gpu_candidate_count);
  }

  uint32_t gpu_camera_opaque_candidate_count = 0u;
  uint32_t transmission_gpu_candidate_count = 0u;
  uint32_t transparent_draw_count = 0u;
  uint32_t source_index = 0u;

  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager,
                                                            i, &mesh_slot);
    if (!mesh->visible || mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    const uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(&rf->mesh_manager, mesh_slot, s);
      VkrMaterial *material = application_get_material(rf, submesh->material);
      const VkrDrawAlphaRouting alpha =
          application_material_alpha_routing(rf, material);
      const bool8_t transmissive =
          application_material_is_transmissive(rf, material);
      stats.objects_tested++;
      stats.objects_without_bounds += mesh->bounds_valid ? 0u : 1u;
      gpu_camera_opaque_candidate_count +=
          !transmissive && !alpha.world_transparent ? 1u : 0u;
      transmission_gpu_candidate_count += transmissive ? 1u : 0u;
      if (!transmissive && alpha.world_transparent) {
        bool8_t visible = true_v;
        if (mesh->bounds_valid) {
          Vec3 center = {0};
          float32_t radius = 0.0f;
          vkr_visibility_submesh_sphere(mesh->model, submesh->center,
                                        submesh->min_extents,
                                        submesh->max_extents, &center, &radius);
          visible = vkr_frustum_test_sphere(&camera_frustum, center, radius);
        }
        transparent_visible[source_index] = visible;
        transparent_draw_count += visible ? 1u : 0u;
        stats.objects_culled_camera += visible ? 0u : 1u;
      }
      source_index++;
    }
  }

  for (uint32_t i = 0; i < live_instance_count; ++i) {
    uint32_t instance_slot = 0;
    VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_live_index(
        &rf->mesh_manager, i, &instance_slot);
    if (!instance->visible ||
        instance->loading_state != VKR_MESH_LOADING_STATE_LOADED)
      continue;
    VkrMeshAsset *asset =
        vkr_mesh_manager_get_live_asset(&rf->mesh_manager, instance->asset);
    const uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
      VkrMaterial *material = application_get_material(rf, submesh->material);
      const VkrDrawAlphaRouting alpha =
          application_material_alpha_routing(rf, material);
      const bool8_t transmissive =
          application_material_is_transmissive(rf, material);
      stats.objects_tested++;
      stats.objects_without_bounds += instance->bounds_valid ? 0u : 1u;
      gpu_camera_opaque_candidate_count +=
          !transmissive && !alpha.world_transparent ? 1u : 0u;
      transmission_gpu_candidate_count += transmissive ? 1u : 0u;
      if (!transmissive && alpha.world_transparent) {
        bool8_t visible = true_v;
        if (instance->bounds_valid) {
          Vec3 center = {0};
          float32_t radius = 0.0f;
          vkr_visibility_submesh_sphere(instance->model, submesh->center,
                                        submesh->min_extents,
                                        submesh->max_extents, &center, &radius);
          visible = vkr_frustum_test_sphere(&camera_frustum, center, radius);
        }
        transparent_visible[source_index] = visible;
        transparent_draw_count += visible ? 1u : 0u;
        stats.objects_culled_camera += visible ? 0u : 1u;
      }
      source_index++;
    }
  }

  VkrWorldDrawCandidate *gpu_candidates = NULL;
  VkrWorldDrawCandidate *transmission_gpu_candidates = NULL;
  VkrTransparentDrawCandidate *transparent_candidates = NULL;
  VkrDrawItem *transparent_draws = NULL;
  VkrInstanceDataGPU *transparent_instances = NULL;
  if (gpu_candidate_count > 0u)
    gpu_candidates = vkr_allocator_alloc(
        scratch, sizeof(*gpu_candidates) * (uint64_t)gpu_candidate_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (transmission_gpu_candidate_count > 0u)
    transmission_gpu_candidates =
        vkr_allocator_alloc(scratch,
                            sizeof(*transmission_gpu_candidates) *
                                (uint64_t)transmission_gpu_candidate_count,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (transparent_draw_count > 0u) {
    transparent_candidates = vkr_allocator_alloc(
        scratch,
        sizeof(*transparent_candidates) * (uint64_t)transparent_draw_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    transparent_draws = vkr_allocator_alloc(
        scratch, sizeof(*transparent_draws) * (uint64_t)transparent_draw_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    transparent_instances = vkr_allocator_alloc(
        scratch,
        sizeof(*transparent_instances) * (uint64_t)transparent_draw_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if ((gpu_candidate_count > 0u && !gpu_candidates) ||
      (transmission_gpu_candidate_count > 0u && !transmission_gpu_candidates) ||
      (transparent_draw_count > 0u &&
       (!transparent_candidates || !transparent_draws ||
        !transparent_instances))) {
    *out_payload = (VkrWorldPassPayload){0};
    return false_v;
  }

  ApplicationWorldEmitContext emit = {
      .view = view,
      .transparent_visible = transparent_visible,
      .gpu_candidates = gpu_candidates,
      .transmission_gpu_candidates = transmission_gpu_candidates,
      .transparent_candidates = transparent_candidates,
  };

  /* Two emission passes so the candidate stream is partitioned with static
     casters first, which is the layout cascade reuse compares against. Pass 0
     also emits the transmission and transparent streams, so those keep their
     original encounter order. `source_index` restarts each pass because it
     indexes `transparent_visible`, which is keyed to encounter order. */
  static const VkrShadowCasterMobility emit_order[] = {
      VKR_SHADOW_CASTER_MOBILITY_STATIC,
      VKR_SHADOW_CASTER_MOBILITY_DYNAMIC,
  };
  uint32_t static_candidate_count = 0u;
  for (uint32_t pass = 0; pass < ArrayCount(emit_order); ++pass) {
    emit.gpu_mobility = emit_order[pass];
    emit.emit_side_streams = pass == 0u ? true_v : false_v;
    emit.source_index = 0u;

    for (uint32_t i = 0; i < mesh_count; ++i) {
      uint32_t mesh_slot = 0;
      VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(&rf->mesh_manager,
                                                              i, &mesh_slot);
      if (!mesh->visible ||
          mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED)
        continue;
      const uint32_t object_id =
          mesh->render_id ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE,
                                                  mesh->render_id)
                          : 0u;
      const uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
      for (uint32_t s = 0; s < submesh_count; ++s) {
        VkrSubMesh *submesh =
            vkr_mesh_manager_get_submesh(&rf->mesh_manager, mesh_slot, s);
        VkrMaterial *material = application_get_material(rf, submesh->material);
        const VkrMaterialHandle draw_material =
            material ? (VkrMaterialHandle){.id = material->id,
                                           .generation = material->generation}
                     : submesh->material;
        if (pass == 0u) {
          vkr_material_system_touch_texture_residency(&rf->material_system,
                                                      draw_material);
        }
        const VkrDrawAlphaRouting alpha =
            application_material_alpha_routing(rf, material);
        const bool8_t transmissive =
            application_material_is_transmissive(rf, material);
        const ApplicationWorldSource source = {
            .mesh = {.id = mesh_slot + 1u, .generation = 0u},
            .geometry = submesh->geometry,
            .material = draw_material,
            .model = mesh->model,
            .center = submesh->center,
            .min_extents = submesh->min_extents,
            .max_extents = submesh->max_extents,
            .alpha = alpha,
            .submesh_index = s,
            .object_id = object_id,
            .temporal_index = mesh_slot,
            .temporal_generation = mesh->temporal_generation,
            .bounds_valid = mesh->bounds_valid,
            .transmissive = transmissive,
            .double_sided = material ? material->double_sided : false_v,
            .shadow_mobility = mesh->shadow_mobility,
        };
        application_emit_world_source(&emit, &source);
      }
    }

    for (uint32_t i = 0; i < live_instance_count; ++i) {
      uint32_t instance_slot = 0;
      VkrMeshInstance *instance = vkr_mesh_manager_get_instance_by_live_index(
          &rf->mesh_manager, i, &instance_slot);
      if (!instance->visible ||
          instance->loading_state != VKR_MESH_LOADING_STATE_LOADED)
        continue;
      VkrMeshAsset *asset =
          vkr_mesh_manager_get_live_asset(&rf->mesh_manager, instance->asset);
      const uint32_t object_id =
          instance->render_id ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE,
                                                      instance->render_id)
                              : 0u;
      const uint32_t submesh_count = (uint32_t)asset->submeshes.length;
      for (uint32_t s = 0; s < submesh_count; ++s) {
        VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
        VkrMaterial *material = application_get_material(rf, submesh->material);
        const VkrMaterialHandle draw_material =
            material ? (VkrMaterialHandle){.id = material->id,
                                           .generation = material->generation}
                     : submesh->material;
        if (pass == 0u) {
          vkr_material_system_touch_texture_residency(&rf->material_system,
                                                      draw_material);
        }
        const VkrDrawAlphaRouting alpha =
            application_material_alpha_routing(rf, material);
        const bool8_t transmissive =
            application_material_is_transmissive(rf, material);
        const ApplicationWorldSource source = {
            .mesh = {.id = instance_slot + 1u,
                     .generation = instance->generation},
            .geometry = submesh->geometry,
            .material = draw_material,
            .model = instance->model,
            .center = submesh->center,
            .min_extents = submesh->min_extents,
            .max_extents = submesh->max_extents,
            .alpha = alpha,
            .submesh_index = s,
            .object_id = object_id,
            .temporal_index = temporal_instance_offset + instance_slot,
            .temporal_generation = instance->generation,
            .bounds_valid = instance->bounds_valid,
            .transmissive = transmissive,
            .double_sided = material ? material->double_sided : false_v,
            .shadow_mobility = instance->shadow_mobility,
        };
        application_emit_world_source(&emit, &source);
      }
    }
    if (pass == 0u)
      static_candidate_count = emit.gpu_index;
  }

  if (transparent_draw_count > 1u)
    qsort(transparent_candidates, transparent_draw_count,
          sizeof(*transparent_candidates), vkr_transparent_draw_depth_compare);
  vkr_transparent_draw_emit(transparent_candidates, transparent_draw_count,
                            transparent_draws, transparent_instances);

  *out_payload = (VkrWorldPassPayload){
      .gpu_candidates = gpu_candidates,
      .gpu_candidate_count = gpu_candidate_count,
      .gpu_camera_opaque_candidate_count = gpu_camera_opaque_candidate_count,
      .gpu_shadow_candidate_count = gpu_candidate_count,
      .static_candidate_count = static_candidate_count,
      .static_generation = rf->mesh_manager.generations.static_content,
      .dynamic_generation = rf->mesh_manager.generations.dynamic_content,
      .publication_generation = publication_generation,
      .caster_bounds_generation = rf->mesh_manager.generations.caster_bounds,
      .publication_pending = publication_pending,
      .transmission_gpu_candidates = transmission_gpu_candidates,
      .transmission_gpu_candidate_count = transmission_gpu_candidate_count,
      .transparent_draws = transparent_draws,
      .transparent_draw_count = transparent_draw_count,
      .instances = transparent_instances,
      .instance_count = transparent_draw_count,
  };
  if (out_stats)
    *out_stats = stats;
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
  application->last_renderer_error = prepare_err;
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

  VkrShadowFrameData shadow_frame = {0};
  uint32_t shadow_cascade_count = 0;

  VkrWorldPassPayload world_payload = {0};
  VkrVisibilityStats visibility_stats = {0};
  bool8_t has_world = false_v;
  VKR_METRICS_SCOPE_NS(application->metrics,
                       application->metric_ids.world_payload_build) {
    has_world = application_build_world_payload(
        application, scratch, &world_payload, &visibility_stats);
  }
  application->visibility_stats = visibility_stats;

  if (has_world && world_payload.gpu_shadow_candidate_count > 0u &&
      application->renderer.shadow_system.initialized) {
    vkr_shadow_system_resolve_frame(
        &application->renderer.shadow_system, setup.image_index,
        setup.retained_shadow, &world_payload,
        vkr_renderer_get_shadow_depth_format(&application->renderer),
        &shadow_frame);
    shadow_cascade_count =
        shadow_frame.enabled ? shadow_frame.cascade_count : 0u;
  } else {
    vkr_shadow_system_discard_frame(&application->renderer.shadow_system);
  }

  VkrShadowPassPayload shadow_payload = {0};
  /* Raster depth bias, distinct from receiver bias. Lowered from the shadow
     config so both selected implementations apply the same configured values
     instead of one backend hardcoding defaults and the other applying none.
     Lives in the frame scope because the payload borrows it until submit
     returns. */
  VkrShadowConfigOverride raster_bias_override = {0};
  bool8_t has_shadow = false_v;
  if (has_world && shadow_cascade_count > 0) {
    const VkrShadowConfig *shadow_config =
        &application->renderer.shadow_system.config;
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
  } else if (application->renderer.world_resources.ibl_default_ready) {
    frame_ibl_source =
        application->renderer.world_resources.ibl_fallback_source_cubemap;
  }
  skybox_payload.cubemap = application->renderer.skybox_system.initialized
                               ? frame_ibl_source
                               : VKR_TEXTURE_HANDLE_INVALID;
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
  const bool8_t pass_gpu_timing = application->metrics->config.pass_gpu_timings;
  const bool8_t submission_gpu_timing =
      application->metrics->config.submission_gpu_timings;
  const bool8_t gpu_timing = pass_gpu_timing || submission_gpu_timing;
  VkrGpuDebugPayload debug_payload = {
      .enable_timing = gpu_timing,
      .capture_pass_timestamps = pass_gpu_timing,
      .capture_submission_timing = submission_gpu_timing,
      .shadow_debug_mode = application->renderer.shadow_debug_mode,
      .capture = application->capture_request,
  };
  const VkrGpuDebugPayload *debug_ptr =
      (gpu_timing || application->capture_request ||
       application->renderer.shadow_debug_mode != 0u)
          ? &debug_payload
          : NULL;
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
              .scene_generation = application->renderer.scene_generation,
          },
      .globals =
          {
              .view = application->renderer.globals.view,
              .projection = application->renderer.globals.projection,
              .view_position = application->renderer.globals.view_position,
              .ambient_color = application->renderer.globals.ambient_color,
              .exposure_mode =
                  (uint32_t)application->renderer.globals.exposure_mode,
              .manual_exposure = application->renderer.globals.manual_exposure,
              .exposure_compensation_ev =
                  application->renderer.globals.exposure_compensation_ev,
              .bloom_enabled = application->renderer.globals.bloom_enabled,
              .bloom_threshold = application->renderer.globals.bloom_threshold,
              .bloom_knee = application->renderer.globals.bloom_knee,
              .bloom_intensity = application->renderer.globals.bloom_intensity,
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
  if (submit_err == VKR_RENDERER_ERROR_NONE) {
    vkr_shadow_system_commit_frame(
        &application->renderer.shadow_system,
        vkr_renderer_get_submit_serial(&application->renderer));
  } else {
    vkr_shadow_system_discard_frame(&application->renderer.shadow_system);
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
      VKR_METRICS_SCOPE_NS(application->metrics,
                           application->metric_ids.shadow_update) {
        VkrShadowCasterDepthBounds caster_bounds = {0};
        application_measure_caster_bounds(application, &caster_bounds);
        const VkrShadowDepthRangeSample *sdsm_sample =
            application->renderer.timing_result.shadow_depth_range
                        .submit_value > 0u
                ? &application->renderer.timing_result.shadow_depth_range
                : NULL;
        vkr_shadow_system_set_depth_range_sample(
            &application->renderer.shadow_system, sdsm_sample,
            application->renderer.frame_number,
            application->renderer.scene_generation);
        vkr_shadow_system_update(
            &application->renderer.shadow_system, camera,
            application->renderer.lighting_system.directional.enabled,
            application->renderer.lighting_system.directional.direction,
            &caster_bounds);
      }
    }

    if (camera) {
      // update_all() refreshed these cached matrices above.
      application->renderer.globals.view = camera->view;
      application->renderer.globals.projection = camera->projection;
      application->renderer.globals.view_position = camera->position;
    } else {
      application->renderer.globals.view = mat4_identity();
      application->renderer.globals.projection = mat4_identity();
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

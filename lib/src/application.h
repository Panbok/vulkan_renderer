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
#include "core/vkr_threads.h"
#include "core/vkr_window.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/vkr_renderer.h"

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
} ApplicationConfig;

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

  VkrJobSystem job_system; /**< Engine-wide job system. */

} Application;

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
  assert(config->target_frame_rate > 0 && "Application target frame rate is 0");

  if (!vkr_platform_init()) {
    log_fatal("Failed to initialize platform!");
    return false_v;
  }

  application->config = config;
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

  event_manager_create(&application->event_manager);
  vkr_window_create(&application->window, &application->event_manager,
                    config->title, config->x, config->y, config->width,
                    config->height);
  application->clock = vkr_clock_create();
  if (!vkr_mutex_create(&application->app_allocator, &application->app_mutex)) {
    log_fatal("Failed to create application mutex!");
    return false_v;
  }

  // Initialize engine-wide job system
  VkrJobSystemConfig job_cfg = vkr_job_system_config_default();
  if (!vkr_job_system_init(&job_cfg, &application->job_system)) {
    log_fatal("Failed to initialize job system");
    return false_v;
  }

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_renderer_initialize(
          &application->renderer, VKR_RENDERER_BACKEND_TYPE_VULKAN,
          &application->window, &application->event_manager,
          &application->config->device_requirements, NULL, &renderer_error)) {
    log_fatal("Failed to create renderer!");
    return false_v;
  }

  vkr_gamepad_init(&application->gamepad, &application->window.input_state);

  if (!vkr_renderer_systems_initialize(&application->renderer)) {
    log_fatal("Failed to initialize renderer frontend systems");
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

  if (vkr_renderer_begin_frame(&application->renderer, delta) !=
      VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to begin renderer frame");
    return;
  }

  vkr_renderer_draw_frame(&application->renderer);

  if (vkr_renderer_end_frame(&application->renderer, delta) !=
      VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to end renderer frame");
    return;
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

  const float64_t target_frame_seconds =
      1.0 / application->config->target_frame_rate;

  bool8_t running = true_v;
  while (
      running &&
      bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) &&
      bitset8_is_set(&application->app_flags, APPLICATION_FLAG_INITIALIZED)) {

    vkr_clock_update(&application->clock);
    float64_t current_frame_time = application->clock.elapsed;
    float64_t delta = current_frame_time - application->last_frame_time;

    // If delta is zero or negative (e.g., first frame or timer issue), use a
    // default fixed delta.
    if (delta <= 0.0) {
      delta = target_frame_seconds;
    }

    running = vkr_window_update(&application->window);
    vkr_gamepad_poll_all(&application->gamepad);

    if (!running ||
        bitset8_is_set(&application->app_flags, APPLICATION_FLAG_SUSPENDED)) {
      application->last_frame_time =
          current_frame_time; // Update time before potentially skipping frame
                              // logic
      if (!running) {
        break;
      }
      continue;
    }

    float64_t frame_processing_start_time = vkr_platform_get_absolute_time();

    application_update(application, delta);

    VkrCameraSystem *camera_system = &application->renderer.camera_system;
    VkrCameraHandle active_camera =
        vkr_camera_registry_get_active(camera_system);
    application->renderer.active_camera = active_camera;
    VkrCamera *camera =
        vkr_camera_registry_get_by_handle(camera_system, active_camera);

    if (camera) {
      application->renderer.camera_controller.camera = camera;
      vkr_camera_controller_update(&application->renderer.camera_controller,
                                   delta);
    } else {
      log_warn("Active camera handle invalid; skipping controller update");
    }

    vkr_camera_registry_update_all(camera_system);

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
      vkr_mesh_manager_update_model(&application->renderer.mesh_manager,
                                    mesh_index);
    }

    application_draw_frame(application, delta);

    // Frame limiting / yielding CPU
    float64_t frame_processing_end_time = vkr_platform_get_absolute_time();
    float64_t frame_elapsed_processing_time =
        frame_processing_end_time - frame_processing_start_time;
    float64_t remaining_seconds_in_frame =
        target_frame_seconds - frame_elapsed_processing_time;

    if (remaining_seconds_in_frame > 0.0) {
      uint64_t remaining_ms = (uint64_t)(remaining_seconds_in_frame * 1000.0);
      if (remaining_ms > 0) {
        // Consider a flag to enable/disable this sleep for debugging or
        // specific needs
        vkr_platform_sleep(remaining_ms);
      }
    }

    input_update(&application->window.input_state);

    application->last_frame_time = current_frame_time;
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
 * Asserts that the application is currently running.
 * @param application Pointer to the `Application` structure.
 */
void application_close(Application *application) {
  assert(application != NULL && "Application is NULL");
  assert(bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) &&
         "Application is not running");

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

  vkr_renderer_destroy(&application->renderer);
  vkr_window_destroy(&application->window);
  event_manager_destroy(&application->event_manager);
  vkr_mutex_destroy(&application->app_allocator, &application->app_mutex);
  vkr_gamepad_shutdown(&application->gamepad);

  vkr_job_system_shutdown(&application->job_system);

  vkr_platform_shutdown();

  arena_destroy(application->log_arena);
  arena_destroy(application->app_arena);
}

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

#include "./defines.h"
#include "containers/bitset.h"
#include "core/clock.h"
#include "core/event.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "math/mat.h"
#include "math/math.h"
#include "math/vec.h"
#include "memory/arena.h"
#include "platform/vkr_threads.h"
#include "platform/window.h"
#include "renderer/renderer.h"
#include "renderer/resources/resources.h"

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
  DeviceRequirements device_requirements; /**< The device requirements for the
                                             application. */
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
  Arena *renderer_arena;      /**< Memory arena dedicated to the renderer. */
  EventManager event_manager; /**< Manages event dispatch and subscriptions. */
  Window window;              /**< Represents the application window. */
  ApplicationConfig *config;  /**< Pointer to the configuration used to create
                                 this application instance. */
  RendererFrontendHandle renderer; /**< Renderer instance. */

  Clock clock; /**< Clock used for timing frames and calculating delta time. */
  float64_t last_frame_time; /**< Timestamp of the previous frame, used for
                                delta time calculation. */
  Bitset8 app_flags;         /**< Bitset holding `ApplicationFlag`s to track the
                                current state. */
  VkrMutex app_mutex;        /**< Mutex for application state. */

  Camera camera;

  GraphicsPipelineDescription pipeline;
  PipelineHandle pipeline_handle;

  Array_Mesh meshes;
  VertexArray cube_vertex_array;

  // Window size tracking for resize detection
  uint32_t
      last_window_width; /**< Last known window width for resize detection */
  uint32_t last_window_height; /**< Last known window height for resize
                                  detection */
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
 * @brief Initializes the camera for the application
 * @param application Pointer to the `Application` structure.
 * @return The initialized camera
 */
Camera application_init_camera(Application *application);

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
 * @brief Handles window resize events.
 * This function is called when the window is resized. It updates the window
 * dimensions and the renderer.
 * @param application Pointer to the `Application` structure.
 */
static bool8_t application_handle_window_resize(Event *event,
                                                UserData user_data) {
  assert(event != NULL && "Event is NULL");
  assert(event->type == EVENT_TYPE_WINDOW_RESIZE &&
         "Event is not a window resize event");

  Application *application = (Application *)user_data;
  assert(application != NULL && "Application is NULL");

  vkr_mutex_lock(application->app_mutex);

  WindowResizeEventData *resize_event_data =
      (WindowResizeEventData *)event->data;

  if (resize_event_data->width != application->last_window_width ||
      resize_event_data->height != application->last_window_height) {

    if (resize_event_data->width == 0 || resize_event_data->height == 0) {
      log_debug("Skipping resize with zero dimensions: %dx%d",
                resize_event_data->width, resize_event_data->height);
      application->last_window_width = resize_event_data->width;
      application->last_window_height = resize_event_data->height;
      vkr_mutex_unlock(application->app_mutex);
      return true_v;
    }

    log_info("Processing window resize to %dx%d", resize_event_data->width,
             resize_event_data->height);

    application->pipeline.shader_object_description.global_uniform_object
        .projection = camera_get_projection_matrix(&application->camera);

    renderer_resize(application->renderer, resize_event_data->width,
                    resize_event_data->height);

    application->window.width = resize_event_data->width;
    application->window.height = resize_event_data->height;

    application->last_window_width = resize_event_data->width;
    application->last_window_height = resize_event_data->height;
  }

  vkr_mutex_unlock(application->app_mutex);

  return true_v;
}

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

  if (!platform_init()) {
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

  ArenaFlags log_arena_flags = bitset8_create();
  bitset8_set(&log_arena_flags, ARENA_FLAG_LARGE_PAGES);
  application->log_arena = arena_create(MB(5), MB(5), log_arena_flags);
  if (!application->log_arena) {
    log_fatal("Failed to create log_arena!");
    if (application->app_arena)
      arena_destroy(application->app_arena);
    return false_v;
  }

  log_init(application->log_arena);

  log_debug("Initialized logging");

  event_manager_create(&application->event_manager);
  window_create(&application->window, &application->event_manager,
                config->title, config->x, config->y, config->width,
                config->height);
  application->clock = clock_create();
  if (!vkr_mutex_create(application->app_arena, &application->app_mutex)) {
    log_fatal("Failed to create application mutex!");
    arena_destroy(application->log_arena);
    arena_destroy(application->app_arena);
    return false_v;
  }

  application->renderer_arena = arena_create(MB(3));
  if (!application->renderer_arena) {
    log_fatal("Failed to create renderer_arena!");
    if (application->app_arena)
      arena_destroy(application->app_arena);
    return false_v;
  }

  RendererError renderer_error = RENDERER_ERROR_NONE;
  application->renderer = renderer_create(
      application->renderer_arena, RENDERER_BACKEND_TYPE_VULKAN,
      &application->window, &application->config->device_requirements,
      &renderer_error);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to create renderer!");
    if (application->app_arena)
      arena_destroy(application->app_arena);
    if (application->log_arena)
      arena_destroy(application->log_arena);
    if (application->renderer_arena)
      arena_destroy(application->renderer_arena);
    return false_v;
  }

  Mat4 model = mat4_identity();
  application->camera = application_init_camera(application);

  ShaderObjectDescription shader_object_description = {
      .file_format = SHADER_FILE_FORMAT_SPIR_V,
      .file_type = SHADER_FILE_TYPE_SINGLE,
      .modules =
          {
              [SHADER_STAGE_VERTEX] =
                  {
                      .stages =
                          shader_stage_flags_from_bits(SHADER_STAGE_VERTEX_BIT),
                      .path = string8_lit("assets/cube.spv"),
                      .entry_point = string8_lit("vertexMain"),
                  },
              [SHADER_STAGE_FRAGMENT] =
                  {
                      .stages = shader_stage_flags_from_bits(
                          SHADER_STAGE_FRAGMENT_BIT),
                      .path = string8_lit("assets/cube.spv"),
                      .entry_point = string8_lit("fragmentMain"),
                  },
          },
      .global_uniform_object =
          (GlobalUniformObject){
              .view = camera_get_view_matrix(&application->camera),
              .projection = camera_get_projection_matrix(&application->camera),
          },
      .shader_state_object =
          (ShaderStateObject){
              .model = model,
          },
  };

  // Generate cube mesh (1x1x1 cube)
  Mesh cube_mesh =
      mesh_generate_cube(application->renderer_arena, 2.0f, 2.0f, 2.0f);

  VertexArrayFromMeshOptions opts =
      VERTEX_ARRAY_FROM_MESH_OPTIONS_INTERLEAVED_POSITION_COLOR();
  application->cube_vertex_array = vertex_array_from_mesh(
      application->renderer, application->renderer_arena, &cube_mesh, opts,
      string8_lit("Cube Vertex Array"), &renderer_error);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to create cube vertex array: %s",
              renderer_get_error_string(renderer_error));
    return false_v;
  }

  log_debug("Cube mesh uploaded to GPU successfully");

  log_debug("Attribute count: %d",
            application->cube_vertex_array.attribute_count);
  log_debug("Binding count: %d", application->cube_vertex_array.binding_count);
  log_debug("Attributes: %p", application->cube_vertex_array.attributes);
  log_debug("Bindings: %p", application->cube_vertex_array.bindings);

  application->pipeline = (GraphicsPipelineDescription){
      .shader_object_description = shader_object_description,
      .attribute_count = application->cube_vertex_array.attribute_count,
      .attributes = application->cube_vertex_array.attributes,
      .binding_count = application->cube_vertex_array.binding_count,
      .bindings = application->cube_vertex_array.bindings,
      .topology = application->cube_vertex_array.topology,
  };

  application->pipeline_handle = renderer_create_graphics_pipeline(
      application->renderer, &application->pipeline, &renderer_error);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to create pipeline: %s",
              renderer_get_error_string(renderer_error));
    return false_v;
  }

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_WINDOW_CLOSE,
                          application_on_window_event, NULL);

  event_manager_subscribe(&application->event_manager, EVENT_TYPE_WINDOW_RESIZE,
                          application_handle_window_resize, application);

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

  // Initialize window size tracking
  WindowPixelSize initial_size = window_get_pixel_size(&application->window);
  application->last_window_width = initial_size.width;
  application->last_window_height = initial_size.height;

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

  RendererError renderer_error =
      renderer_begin_frame(application->renderer, delta);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to begin frame: %s",
              renderer_get_error_string(renderer_error));
    return;
  }

  application->pipeline.shader_object_description.global_uniform_object.view =
      camera_get_view_matrix(&application->camera);

  renderer_update_pipeline_state(
      application->renderer, application->pipeline_handle,
      &application->pipeline.shader_object_description.global_uniform_object,
      &application->pipeline.shader_object_description.shader_state_object);

  vertex_array_render(application->renderer, &application->cube_vertex_array,
                      1);

  renderer_error = renderer_end_frame(application->renderer, delta);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to end frame: %s",
              renderer_get_error_string(renderer_error));
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

  clock_start(&application->clock);
  clock_update(&application->clock);
  application->last_frame_time = application->clock.elapsed;

  const float64_t target_frame_seconds =
      1.0 / application->config->target_frame_rate;

  bool8_t running = true_v;
  while (
      running &&
      bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) &&
      bitset8_is_set(&application->app_flags, APPLICATION_FLAG_INITIALIZED)) {

    clock_update(&application->clock);
    float64_t current_frame_time = application->clock.elapsed;
    float64_t delta = current_frame_time - application->last_frame_time;

    // If delta is zero or negative (e.g., first frame or timer issue), use a
    // default fixed delta.
    if (delta <= 0.0) {
      delta = target_frame_seconds;
    }

    running = window_update(&application->window);

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

    float64_t frame_processing_start_time = platform_get_absolute_time();

    application_update(application, delta);

    camera_update(&application->camera, delta);

    application_draw_frame(application, delta);

    // Frame limiting / yielding CPU
    float64_t frame_processing_end_time = platform_get_absolute_time();
    float64_t frame_elapsed_processing_time =
        frame_processing_end_time - frame_processing_start_time;
    float64_t remaining_seconds_in_frame =
        target_frame_seconds - frame_elapsed_processing_time;

    if (remaining_seconds_in_frame > 0.0) {
      uint64_t remaining_ms = (uint64_t)(remaining_seconds_in_frame * 1000.0);
      if (remaining_ms > 0) {
        // Consider a flag to enable/disable this sleep for debugging or
        // specific needs
        platform_sleep(remaining_ms);
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
  assert(application != NULL && "Application is NULL");
  assert(!bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) &&
         "Application is still running");

  log_info("Application shutting down...");

  event_manager_dispatch(&application->event_manager,
                         (Event){.type = EVENT_TYPE_APPLICATION_SHUTDOWN});

  if (renderer_wait_idle(application->renderer) != RENDERER_ERROR_NONE) {
    log_warn("Failed to wait for renderer to be idle");
  }

  vertex_array_destroy(application->renderer, &application->cube_vertex_array);
  renderer_destroy_pipeline(application->renderer,
                            application->pipeline_handle);
  renderer_destroy(application->renderer);
  window_destroy(&application->window);
  event_manager_destroy(&application->event_manager);
  vkr_mutex_destroy(application->app_arena, &application->app_mutex);

  platform_shutdown();

  arena_destroy(application->renderer_arena);
  arena_destroy(application->log_arena);
  arena_destroy(application->app_arena);
}

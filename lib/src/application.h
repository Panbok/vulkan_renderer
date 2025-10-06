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
#include "core/vkr_threads.h"
#include "core/vkr_window.h"
#include "defines.h"
#include "math/mat.h"
#include "memory/arena.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"
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
  Arena *renderer_arena;      /**< Memory arena dedicated to the renderer. */
  EventManager event_manager; /**< Manages event dispatch and subscriptions. */
  VkrWindow window;           /**< Represents the application window. */
  ApplicationConfig *config;  /**< Pointer to the configuration used to create
                                 this application instance. */
  VkrRendererFrontendHandle renderer; /**< Renderer instance. */

  VkrPipelineRegistry pipeline_registry;
  VkrPipelineHandle world_pipeline;
  VkrPipelineHandle ui_pipeline;

  VkrClock
      clock; /**< Clock used for timing frames and calculating delta time. */
  float64_t last_frame_time; /**< Timestamp of the previous frame, used for
                                delta time calculation. */
  Bitset8 app_flags;         /**< Bitset holding `ApplicationFlag`s to track the
                                current state. */
  VkrMutex app_mutex;        /**< Mutex for application state. */

  VkrCamera camera; /**< The camera for the application. */

  // Per-frame and per-draw state
  VkrGlobalUniformObject global_uniform;
  VkrShaderStateObject draw_state;

  VkrGamepad gamepad; /**< The gamepad system for the application. */

  VkrGeometrySystem geometry_system;
  VkrGeometryHandle cube_geometry;
  VkrGeometryHandle ui_geometry;

  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;

  Mat4 world_model;
  Mat4 ui_model;

  VkrMaterialHandle world_material;
  VkrMaterialHandle ui_material;

  // Scene renderables
  Array_VkrRenderable renderables;
  uint32_t renderable_count;

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
VkrCamera application_init_camera(Application *application);

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

  VkrWindowResizeEventData *resize_event_data =
      (VkrWindowResizeEventData *)event->data;

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

    application->global_uniform.projection =
        vkr_camera_system_get_projection_matrix(&application->camera);

    vkr_renderer_resize(application->renderer, resize_event_data->width,
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
  vkr_window_create(&application->window, &application->event_manager,
                    config->title, config->x, config->y, config->width,
                    config->height);
  application->clock = vkr_clock_create();
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

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  application->renderer = vkr_renderer_create(
      application->renderer_arena, VKR_RENDERER_BACKEND_TYPE_VULKAN,
      &application->window, &application->config->device_requirements,
      &renderer_error);
  if (renderer_error != VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to create renderer!");
    if (application->app_arena)
      arena_destroy(application->app_arena);
    if (application->log_arena)
      arena_destroy(application->log_arena);
    if (application->renderer_arena)
      arena_destroy(application->renderer_arena);
    return false_v;
  }

  vkr_gamepad_init(&application->gamepad, &application->window.input_state);
  application->camera = application_init_camera(application);

  if (!vkr_pipeline_registry_init(&application->pipeline_registry,
                                  application->renderer, NULL)) {
    log_fatal("Failed to initialize pipeline registry");
    return false_v;
  }

  // TODO: Move all resources and renderer related stuff to front-end
  application->world_model = mat4_identity();
  application->ui_model = mat4_identity();

  // Resource system init (central registry and delegate to systems)
  if (!vkr_resource_system_init(application->app_arena,
                                application->renderer)) {
    log_fatal("Failed to initialize resource system");
    return false_v;
  }

  VkrGeometrySystemConfig geo_cfg = {
      .default_max_geometries = 1024,
      .default_max_vertices = 200000,
      .default_max_indices = 300000,
      .primary_layout = GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD,
      .default_vertex_stride_bytes =
          sizeof(VkrInterleavedVertex_PositionTexcoord),
  };
  if (!vkr_geometry_system_init(&application->geometry_system,
                                application->renderer, &geo_cfg,
                                &renderer_error)) {
    log_fatal("Failed to initialize geometry system: %s",
              vkr_renderer_get_error_string(renderer_error));
    return false_v;
  }

  VkrTextureSystemConfig tex_cfg = {.max_texture_count = 1024};
  if (!vkr_texture_system_init(application->renderer, &tex_cfg,
                               &application->texture_system)) {
    log_fatal("Failed to initialize texture system");
    return false_v;
  }

  VkrMaterialSystemConfig mat_cfg = {.max_material_count = 1024};
  if (!vkr_material_system_init(&application->material_system,
                                application->app_arena,
                                &application->texture_system, &mat_cfg)) {
    log_fatal("Failed to initialize material system");
    return false_v;
  }

  vkr_resource_system_register_loader((void *)&application->texture_system,
                                      vkr_texture_loader_create());
  vkr_resource_system_register_loader((void *)&application->material_system,
                                      vkr_material_loader_create());

  // TODO: Move all resources and renderer related stuff to front-end

  VkrResourceHandleInfo default_material_info = {0};
  VkrRendererError material_load_error = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                               string8_lit("assets/default.world.mt"),
                               application->app_arena, &default_material_info,
                               &material_load_error)) {
    log_info(
        "Successfully loaded default material from assets/default.world.mt");
    application->world_material = default_material_info.as.material;
  } else {
    String8 error_string = vkr_renderer_get_error_string(material_load_error);
    log_warn(
        "Failed to load default material from assets/default.world.mt; using "
        "built-in default: %s",
        string8_cstr(&error_string));
  }

  VkrResourceHandleInfo default_ui_material_info = {0};
  if (vkr_resource_system_load(
          VKR_RESOURCE_TYPE_MATERIAL, string8_lit("assets/default.ui.mt"),
          application->app_arena, &default_ui_material_info,
          &material_load_error)) {
    log_info(
        "Successfully loaded default UI material from assets/default.ui.mt");
    application->ui_material = default_ui_material_info.as.material;
  } else {
    String8 error_string = vkr_renderer_get_error_string(material_load_error);
    log_warn(
        "Failed to load default UI material from assets/default.ui.mt; using "
        "built-in default: %s",
        string8_cstr(&error_string));
  }

  application->global_uniform = (VkrGlobalUniformObject){
      .view = vkr_camera_system_get_view_matrix(&application->camera),
      .projection =
          vkr_camera_system_get_projection_matrix(&application->camera),
  };
  application->draw_state = (VkrShaderStateObject){
      .model = application->world_model, .local_state = {0}};

  application->cube_geometry = vkr_geometry_system_create_default_cube(
      &application->geometry_system, 2.0f, 2.0f, 2.0f, &renderer_error);
  if (renderer_error != VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to create default geometry: %s",
              vkr_renderer_get_error_string(renderer_error));
    return false_v;
  }

  application->ui_geometry = vkr_geometry_system_create_default_plane(
      &application->geometry_system, 2.0f, 2.0f, &renderer_error);
  if (renderer_error != VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to create default UI geometry: %s",
              vkr_renderer_get_error_string(renderer_error));
    return false_v;
  }

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_material_layout(
          &application->pipeline_registry, VKR_PIPELINE_DOMAIN_WORLD,
          GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD,
          string8_lit("assets/deafult.world.spv"), string8_lit("world"),
          &application->world_pipeline, &pipeline_error)) {
    log_fatal("Failed to create world pipeline: %s",
              vkr_renderer_get_error_string(pipeline_error));
    return false_v;
  }

  if (!vkr_pipeline_registry_create_from_material_layout(
          &application->pipeline_registry, VKR_PIPELINE_DOMAIN_UI,
          GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD,
          string8_lit("assets/deafult.ui.spv"), string8_lit("ui"),
          &application->ui_pipeline, &pipeline_error)) {
    log_fatal("Failed to create UI pipeline: %s",
              vkr_renderer_get_error_string(pipeline_error));
    return false_v;
  }

  application->renderables =
      array_create_VkrRenderable(application->app_arena, 1024);
  VkrRenderable r0 = {
      .geometry = application->cube_geometry,
      .material = application->world_material,
      .pipeline = application->world_pipeline,
      .model = application->world_model,
      .local_state = {0},
  };
  VkrRenderable r1 = {
      .geometry = application->ui_geometry,
      .material = application->ui_material,
      .pipeline = application->ui_pipeline,
      .model = mat4_mul(mat4_translate(vec3_new(200.0f, 200.0f, 0.0f)),
                        mat4_scale(vec3_new(200.0f, 200.0f, 1.0f))),
      .local_state = {0},
  };
  array_set_VkrRenderable(&application->renderables, 0, r0);
  array_set_VkrRenderable(&application->renderables, 1, r1);
  application->renderable_count = 2;

  for (uint32_t renderable_idx = 0;
       renderable_idx < application->renderable_count; ++renderable_idx) {
    VkrRenderable *renderable =
        array_get_VkrRenderable(&application->renderables, renderable_idx);
    VkrRendererError ls_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_acquire_local_state(
            &application->pipeline_registry, renderable->pipeline,
            &renderable->local_state, &ls_err)) {
      log_fatal("Failed to acquire local renderer state for renderable %u",
                renderable_idx);
      return false_v;
    }
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
  VkrWindowPixelSize initial_size =
      vkr_window_get_pixel_size(&application->window);
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

  VkrRendererError renderer_error =
      vkr_renderer_begin_frame(application->renderer, delta);
  if (renderer_error != VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to begin frame: %s",
              vkr_renderer_get_error_string(renderer_error));
    return;
  }

  application->global_uniform.view =
      vkr_camera_system_get_view_matrix(&application->camera);
  application->global_uniform.projection =
      vkr_camera_system_get_projection_matrix(&application->camera);

  VkrRendererError reg_err = VKR_RENDERER_ERROR_NONE;
  vkr_pipeline_registry_bind_pipeline(&application->pipeline_registry,
                                      application->world_pipeline, &reg_err);
  vkr_pipeline_registry_update_global_state(
      &application->pipeline_registry, &application->global_uniform, &reg_err);

  for (uint32_t i = 0; i < application->renderable_count; i++) {
    VkrRenderable *renderable =
        array_get_VkrRenderable(&application->renderables, i);
    // Resolve material via handle
    const VkrMaterial *material = vkr_material_system_get_by_handle(
        &application->material_system, renderable->material);

    application->draw_state.model = renderable->model;
    application->draw_state.local_state = renderable->local_state;

    VkrRendererMaterialState mat_state = {0};
    if (material) {
      mat_state.uniforms.diffuse_color = material->phong.diffuse_color;
      VkrTexture *t = vkr_texture_system_get_by_handle(
          &application->texture_system,
          material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle);
      if (t) {
        mat_state.texture0 = t->handle;
        mat_state.texture0_enabled = true_v;
      }
    }

    // Ensure correct pipeline and global state before local update
    VkrPipelineHandle current_pipeline =
        vkr_pipeline_registry_get_current_pipeline(
            &application->pipeline_registry);
    // Ensure correct pipeline is bound
    if (current_pipeline.id != renderable->pipeline.id ||
        current_pipeline.generation != renderable->pipeline.generation) {
      vkr_pipeline_registry_bind_pipeline(&application->pipeline_registry,
                                          renderable->pipeline, &reg_err);
    }

    // Upload correct globals per-domain (avoid relying on cached flag)
    if (renderable->pipeline.id == application->ui_pipeline.id &&
        renderable->pipeline.generation ==
            application->ui_pipeline.generation) {
      VkrWindowPixelSize sz = vkr_window_get_pixel_size(&application->window);
      application->global_uniform.view = mat4_identity();
      application->global_uniform.projection = mat4_ortho(
          0.0f, (float32_t)sz.width, (float32_t)sz.height, 0.0f, -1.0f, 1.0f);
    } else {
      application->global_uniform.view =
          vkr_camera_system_get_view_matrix(&application->camera);
      application->global_uniform.projection =
          vkr_camera_system_get_projection_matrix(&application->camera);
    }
    // Force upload
    vkr_pipeline_registry_update_global_state(&application->pipeline_registry,
                                              &application->global_uniform,
                                              &reg_err);

    vkr_pipeline_registry_update_local_state(
        &application->pipeline_registry, renderable->pipeline,
        &application->draw_state, &mat_state, &reg_err);

    vkr_geometry_system_render(application->renderer,
                               &application->geometry_system,
                               renderable->geometry, 1);
  }

  renderer_error = vkr_renderer_end_frame(application->renderer, delta);
  if (renderer_error != VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to end frame: %s",
              vkr_renderer_get_error_string(renderer_error));
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

    vkr_camera_system_update(&application->camera, delta);

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

  if (vkr_renderer_wait_idle(application->renderer) !=
      VKR_RENDERER_ERROR_NONE) {
    log_warn("Failed to wait for renderer to be idle");
  }

  // Resource system does not own GPU resources; shutdown registry first
  // Release per-renderable local renderer state before destroying pipeline
  for (uint32_t i = 0; i < application->renderable_count; ++i) {
    VkrRenderable *r = array_get_VkrRenderable(&application->renderables, i);
    vkr_pipeline_registry_release_local_state(&application->pipeline_registry,
                                              r->pipeline, r->local_state,
                                              &(VkrRendererError){0});
  }
  vkr_pipeline_registry_destroy_pipeline(&application->pipeline_registry,
                                         application->world_pipeline);
  vkr_pipeline_registry_destroy_pipeline(&application->pipeline_registry,
                                         application->ui_pipeline);
  vkr_pipeline_registry_shutdown(&application->pipeline_registry);
  vkr_texture_system_shutdown(application->renderer,
                              &application->texture_system);
  vkr_material_system_shutdown(&application->material_system);
  vkr_geometry_system_shutdown(&application->geometry_system);
  vkr_renderer_destroy(application->renderer);
  vkr_window_destroy(&application->window);
  event_manager_destroy(&application->event_manager);
  vkr_mutex_destroy(application->app_arena, &application->app_mutex);
  vkr_gamepad_shutdown(&application->gamepad);

  vkr_platform_shutdown();

  arena_destroy(application->renderer_arena);
  arena_destroy(application->log_arena);
  arena_destroy(application->app_arena);
}

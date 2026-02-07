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
#include "math/vec.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_editor_viewport.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_camera.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vkr_render_packet.h"

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

  ApplicationTextUpdate ui_text_updates[VKR_MAX_PENDING_TEXT_UPDATES];
  uint32_t ui_text_update_count;
  ApplicationTextUpdate world_text_updates[VKR_MAX_PENDING_TEXT_UPDATES];
  uint32_t world_text_update_count;

  ApplicationEditorViewport editor_viewport;
  bool8_t rg_gpu_timing_enabled; /**< Enables per-pass GPU timing in RG. */
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

  event_manager_create(&application->event_manager);
  vkr_window_create(&application->window, &application->event_manager,
                    config->title, config->x, config->y, config->width,
                    config->height);
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
  if (!vkr_renderer_initialize(
          &application->renderer, VKR_RENDERER_BACKEND_TYPE_VULKAN,
          &application->window, &application->event_manager,
          &application->config->device_requirements, NULL,
          application->config->target_frame_rate, &renderer_error)) {
    log_fatal("Failed to create renderer!");
    return false_v;
  }

  vkr_gamepad_init(&application->gamepad, &application->window.input_state);

  if (!vkr_renderer_systems_initialize(&application->renderer,
                                       &application->job_system)) {
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

vkr_internal VkrMaterial *
application_get_material(RendererFrontend *rf, VkrMaterialHandle handle) {
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

vkr_internal bool8_t application_material_is_cutout(RendererFrontend *rf,
                                                    VkrMaterial *material) {
  if (!rf || !material) {
    return false_v;
  }
  return vkr_material_system_material_has_transparency(&rf->material_system,
                                                       material);
}

vkr_internal float32_t application_transparent_depth(Mat4 view, Mat4 model,
                                                     Vec3 local_center) {
  Vec3 world_center = mat4_mul_vec3(model, local_center);
  Vec4 view_pos =
      mat4_mul_vec4(view, vec4_new(world_center.x, world_center.y,
                                   world_center.z, 1.0f));
  float32_t depth = -view_pos.z;
  return depth > 0.0f ? depth : 0.0f;
}

vkr_internal uint64_t application_pack_transparent_sort_key(
    float32_t distance, uint32_t tie_breaker) {
  uint32_t distance_bits = 0;
  MemCopy(&distance_bits, &distance, sizeof(distance_bits));
  return ((uint64_t)distance_bits << 32) | (uint64_t)tie_breaker;
}

vkr_internal int application_transparent_draw_compare(const void *lhs,
                                                      const void *rhs) {
  const VkrDrawItem *a = (const VkrDrawItem *)lhs;
  const VkrDrawItem *b = (const VkrDrawItem *)rhs;
  if (a->sort_key > b->sort_key) {
    return -1;
  }
  if (a->sort_key < b->sort_key) {
    return 1;
  }
  if (a->first_instance < b->first_instance) {
    return -1;
  }
  if (a->first_instance > b->first_instance) {
    return 1;
  }
  return 0;
}

vkr_internal bool8_t application_build_world_payload(
    Application *application, VkrAllocator *scratch,
    VkrWorldPassPayload *out_payload) {
  if (!application || !scratch || !out_payload) {
    return false_v;
  }

  RendererFrontend *rf = &application->renderer;
  Mat4 view = rf->globals.view;
  uint32_t opaque_count = 0;
  uint32_t transparent_count = 0;

  uint32_t mesh_count = vkr_mesh_manager_count(&rf->mesh_manager);
  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(
        &rf->mesh_manager, i, &mesh_slot);
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

      VkrMaterial *material = application_get_material(rf, submesh->material);
      if (application_material_is_cutout(rf, material)) {
        transparent_count++;
      } else {
        opaque_count++;
      }
    }
  }

  uint32_t live_instance_count =
      vkr_mesh_manager_instance_count(&rf->mesh_manager);
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

      VkrMaterial *material = application_get_material(rf, submesh->material);
      if (application_material_is_cutout(rf, material)) {
        transparent_count++;
      } else {
        opaque_count++;
      }
    }
  }

  uint32_t total_draws = opaque_count + transparent_count;
  if (total_draws == 0) {
    *out_payload = (VkrWorldPassPayload){0};
    return true_v;
  }

  VkrDrawItem *opaque_draws = NULL;
  if (opaque_count > 0) {
    opaque_draws = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawItem) * (uint64_t)opaque_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  VkrDrawItem *transparent_draws = NULL;
  if (transparent_count > 0) {
    transparent_draws = vkr_allocator_alloc(
        scratch, sizeof(VkrDrawItem) * (uint64_t)transparent_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  VkrInstanceDataGPU *instances = vkr_allocator_alloc(
      scratch, sizeof(VkrInstanceDataGPU) * (uint64_t)total_draws,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if ((opaque_count > 0 && !opaque_draws) ||
      (transparent_count > 0 && !transparent_draws) || !instances) {
    *out_payload = (VkrWorldPassPayload){0};
    return false_v;
  }

  uint32_t opaque_index = 0;
  uint32_t transparent_index = 0;
  uint32_t instance_index = 0;

  for (uint32_t i = 0; i < mesh_count; ++i) {
    uint32_t mesh_slot = 0;
    VkrMesh *mesh = vkr_mesh_manager_get_mesh_by_live_index(
        &rf->mesh_manager, i, &mesh_slot);
    if (!mesh || !mesh->visible ||
        mesh->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      continue;
    }

    VkrMeshHandle mesh_handle = {.id = mesh_slot + 1u, .generation = 0};

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

      VkrMaterial *material = application_get_material(rf, submesh->material);
      bool8_t cutout = application_material_is_cutout(rf, material);
      float32_t mesh_distance = cutout ? application_transparent_depth(
                                             view, mesh->model, submesh->center)
                                       : 0.0f;
      uint64_t sort_key = cutout ? application_pack_transparent_sort_key(
                                       mesh_distance, instance_index)
                                 : 0u;

      VkrDrawItem *draw = cutout ? &transparent_draws[transparent_index++]
                                 : &opaque_draws[opaque_index++];
      *draw = (VkrDrawItem){
          .mesh = mesh_handle,
          .submesh_index = s,
          .material = VKR_MATERIAL_HANDLE_INVALID,
          .instance_count = 1,
          .first_instance = instance_index,
          .sort_key = sort_key,
          .pipeline_override = VKR_PIPELINE_HANDLE_INVALID,
      };

      instances[instance_index] = (VkrInstanceDataGPU){
          .model = mesh->model,
          .object_id = object_id,
          .material_index = 0,
          .flags = 0,
          ._padding = 0,
      };
      instance_index++;
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

    VkrMeshInstanceHandle handle = {
        .id = instance_slot + 1u,
        .generation = instance->generation,
    };

    uint32_t object_id = 0;
    if (instance->render_id != 0) {
      object_id = vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE,
                                        instance->render_id);
    }

    uint32_t submesh_count = (uint32_t)asset->submeshes.length;
    for (uint32_t s = 0; s < submesh_count; ++s) {
      VkrMeshAssetSubmesh *submesh =
          array_get_VkrMeshAssetSubmesh(&asset->submeshes, s);
      if (!submesh) {
        continue;
      }

      VkrMaterial *material = application_get_material(rf, submesh->material);
      bool8_t cutout = application_material_is_cutout(rf, material);
      float32_t instance_distance =
          cutout ? application_transparent_depth(view, instance->model,
                                                 submesh->center)
                 : 0.0f;
      uint64_t sort_key = cutout ? application_pack_transparent_sort_key(
                                       instance_distance, instance_index)
                                 : 0u;

      VkrDrawItem *draw = cutout ? &transparent_draws[transparent_index++]
                                 : &opaque_draws[opaque_index++];
      *draw = (VkrDrawItem){
          .mesh = handle,
          .submesh_index = s,
          .material = VKR_MATERIAL_HANDLE_INVALID,
          .instance_count = 1,
          .first_instance = instance_index,
          .sort_key = sort_key,
          .pipeline_override = VKR_PIPELINE_HANDLE_INVALID,
      };

      instances[instance_index] = (VkrInstanceDataGPU){
          .model = instance->model,
          .object_id = object_id,
          .material_index = 0,
          .flags = 0,
          ._padding = 0,
      };
      instance_index++;
    }
  }

  if (transparent_count > 1) {
    qsort(transparent_draws, transparent_count, sizeof(VkrDrawItem),
          application_transparent_draw_compare);
  }

  *out_payload = (VkrWorldPassPayload){
      .opaque_draws = opaque_draws,
      .opaque_draw_count = opaque_count,
      .transparent_draws = transparent_draws,
      .transparent_draw_count = transparent_count,
      .instances = instances,
      .instance_count = total_draws,
  };
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
  if (vkr_renderer_prepare_frame(&application->renderer, &setup) !=
      VKR_RENDERER_ERROR_NONE) {
    log_fatal("Failed to prepare renderer frame");
    return;
  }

  VkrAllocator *scratch = &application->renderer.scratch_allocator;

  VkrWorldPassPayload world_payload = {0};
  bool8_t has_world =
      application_build_world_payload(application, scratch, &world_payload);

  VkrShadowPassPayload shadow_payload = {0};
  bool8_t has_shadow = false_v;
  if (has_world && application->renderer.shadow_system.initialized &&
      application->renderer.lighting_system.directional.enabled) {
    VkrShadowFrameData shadow_frame = {0};
    vkr_shadow_system_get_frame_data(&application->renderer.shadow_system,
                                     setup.image_index, &shadow_frame);
    uint32_t cascade_count = shadow_frame.cascade_count;
    if (cascade_count > 0) {
      shadow_payload.cascade_count = cascade_count;
      for (uint32_t i = 0; i < cascade_count; ++i) {
        shadow_payload.light_view_proj[i] = shadow_frame.view_projection[i];
        shadow_payload.split_depths[i] = shadow_frame.split_far[i];
      }
      shadow_payload.opaque_draws = world_payload.opaque_draws;
      shadow_payload.opaque_draw_count = world_payload.opaque_draw_count;
      shadow_payload.alpha_draws = world_payload.transparent_draws;
      shadow_payload.alpha_draw_count = world_payload.transparent_draw_count;
      shadow_payload.instances = world_payload.instances;
      shadow_payload.instance_count = world_payload.instance_count;
      shadow_payload.config_override = NULL;
      has_shadow = true_v;
    }
  }

  VkrPickingPassPayload picking_payload = {0};
  bool8_t has_picking =
      application->renderer.picking.state == VKR_PICKING_STATE_RENDER_PENDING;
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
        vkr_editor_viewport_build_payload(&application->renderer.editor_viewport,
                                          &editor_mapping, editor_draws,
                                          editor_instances, &editor_payload)) {
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
  VkrSkyboxPassPayload skybox_payload = {
      .cubemap = VKR_TEXTURE_HANDLE_INVALID,
      .material = VKR_MATERIAL_HANDLE_INVALID,
  };

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
      ApplicationTextUpdate *pending =
          &application->world_text_updates[i];
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

  VkrGpuDebugPayload debug_payload = {
      .enable_timing = application->rg_gpu_timing_enabled,
      .capture_pass_timestamps = application->rg_gpu_timing_enabled,
  };
  const VkrGpuDebugPayload *debug_ptr =
      application->rg_gpu_timing_enabled ? &debug_payload : NULL;

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
              .render_mode =
                  (uint32_t)application->renderer.globals.render_mode,
          },
      .world = has_world ? &world_payload : NULL,
      .shadow = has_shadow ? &shadow_payload : NULL,
      .skybox = &skybox_payload,
      .ui = &ui_payload,
      .editor = has_editor ? &editor_payload : NULL,
      .picking = has_picking ? &picking_payload : NULL,
      .text_updates = has_text_updates ? &text_updates_payload : NULL,
      .debug = debug_ptr,
  };

  VkrRendererFrameMetrics metrics = {0};
  VkrValidationError validation = {0};
  VkrRendererError submit_err = vkr_renderer_submit_packet(
      &application->renderer, &packet, &metrics, &validation);
  if (submit_err != VKR_RENDERER_ERROR_NONE) {
    if (validation.field_path && validation.message) {
      log_error("Packet validation failed: %s (%s)", validation.field_path,
                validation.message);
    } else {
      String8 err = vkr_renderer_get_error_string(submit_err);
      log_error("Packet submit failed: %s", string8_cstr(&err));
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

    float64_t delta = current_total_time - application->last_frame_time;

    if (delta > 0.1f) {
      delta = 0.1f;
    }

    if (delta <= 0.0) {
      delta = target_frame_seconds > 0.0 ? target_frame_seconds : (1.0 / 60.0);
    }

    running = vkr_window_update(&application->window);
    vkr_gamepad_poll_all(&application->gamepad);

    if (!running ||
        bitset8_is_set(&application->app_flags, APPLICATION_FLAG_SUSPENDED)) {
      application->last_frame_time = current_total_time;
      if (!running) {
        break;
      }
      continue;
    }

    VkrAllocatorScope frame_scope = {0};
    VkrAllocator *frame_alloc = &application->renderer.scratch_allocator;
    if (vkr_allocator_supports_scopes(frame_alloc)) {
      frame_scope = vkr_allocator_begin_scope(frame_alloc);
    }
    application->ui_text_update_count = 0;
    application->world_text_update_count = 0;

    application_update(application, delta);

    // `application_update()` may request shutdown (for example via auto-close).
    // Stop this frame immediately to avoid recording/render calls after
    // APPLICATION_FLAG_RUNNING has been cleared.
    if (!bitset8_is_set(&application->app_flags, APPLICATION_FLAG_RUNNING) ||
        !bitset8_is_set(&application->app_flags, APPLICATION_FLAG_INITIALIZED)) {
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

    if (camera) {
      vkr_camera_controller_update(&application->renderer.camera_controller,
                                   delta);
    }

    vkr_camera_registry_update_all(camera_system);

    if (application->renderer.active_scene) {
      vkr_lighting_system_sync_from_scene(&application->renderer.lighting_system,
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
        !bitset8_is_set(&application->app_flags, APPLICATION_FLAG_INITIALIZED)) {
      if (vkr_allocator_scope_is_valid(&frame_scope)) {
        vkr_allocator_end_scope(&frame_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
      break;
    }

    application_draw_frame(application, delta);

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
          vkr_platform_sleep(remaining_ms);
        }
      }
    }

    application->last_frame_time = current_total_time;

    input_update(&application->window.input_state);
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

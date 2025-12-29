#include "application.h"
#include "containers/array.h"
#include "core/event.h"
#include "core/input.h"
#include "core/logger.h"
#include "defines.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_quat.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/vkr_renderer.h"

#define PLAYER_SPEED 50.0
#define ENTITY_COUNT 1

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

typedef struct State {
  Array_uint16_t entities;

  Array_float64_t player_position_x;
  Array_float64_t player_position_y;

  InputState *input_state;
  bool8_t use_gamepad;
  int8_t previous_wheel_delta;

  Arena *app_arena;
  Arena *event_arena;
  Arena *stats_arena;

  uint32_t filter_mode_index;
  bool8_t anisotropy_supported;
  VkrDeviceInformation device_information;

  EventManager *event_manager; // For dispatching events
} State;

vkr_global State *state = NULL;

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

static void application_handle_input(Application *application,
                                     float64_t delta_time) {
  if (state == NULL || state->input_state == NULL) {
    log_error("State or input state is NULL");
    return;
  }

  InputState *input_state = state->input_state;
  VkrCameraController *controller = &application->renderer.camera_controller;
  VkrCamera *camera = controller->camera;
  if (camera == NULL) {
    log_error("Camera controller has no camera bound");
    return;
  }

  if (input_is_key_down(input_state, KEY_TAB) &&
      input_was_key_up(input_state, KEY_TAB)) {
    bool8_t should_capture =
        !vkr_window_is_mouse_captured(&application->window);
    vkr_window_set_mouse_capture(&application->window, should_capture);
  }

  if (input_is_button_down(input_state, BUTTON_GAMEPAD_A) &&
      input_was_button_up(input_state, BUTTON_GAMEPAD_A)) {
    bool8_t should_capture =
        !vkr_window_is_mouse_captured(&application->window);
    vkr_window_set_mouse_capture(&application->window, should_capture);
    state->use_gamepad = !state->use_gamepad;
  }

  if (!vkr_window_is_mouse_captured(&application->window)) {
    vkr_camera_controller_update(controller, delta_time);
    return;
  }

  bool8_t should_rotate = false_v;
  float32_t yaw_input = 0.0f;
  float32_t pitch_input = 0.0f;

  if (!state->use_gamepad) {
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
    if (wheel_delta != state->previous_wheel_delta) {
      float32_t zoom_delta = -(float32_t)wheel_delta * 0.1f;
      vkr_camera_zoom(camera, zoom_delta);
      state->previous_wheel_delta = wheel_delta;
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
      float32_t y_offset = (float32_t)(last_y - y);

      float32_t max_mouse_delta = VKR_MAX_MOUSE_DELTA / camera->sensitivity;
      x_offset = vkr_clamp_f32(x_offset, -max_mouse_delta, max_mouse_delta);
      y_offset = vkr_clamp_f32(y_offset, -max_mouse_delta, max_mouse_delta);

      yaw_input = -x_offset;
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

  // vkr_camera_controller_update(controller, delta_time);
}

void application_update(Application *application, float64_t delta) {
  // double fps_value = 0.0;
  // if (delta > 0.000001) {
  //   fps_value = 1.0 / delta;
  // } else {
  //   fps_value = 1.0 / application->config->target_frame_rate;
  // }
  // vkr_local_persist int log_fps_counter = 0;
  // VkrCameraSystem *camera_system = &application->renderer.camera_system;
  // VkrCameraHandle active_camera =
  // vkr_camera_registry_get_active(camera_system);
  // application->renderer.active_camera = active_camera;
  // VkrCamera *camera =
  //     vkr_camera_registry_get_by_handle(camera_system, active_camera);
  // application->renderer.camera_controller.camera = camera;

  // if (++log_fps_counter % 60 == 0) { // Log every 60 mouse moves to avoid
  // spam
  //   log_debug("CALCULATED FPS VALUE: %f, DELTA WAS: %f", fps_value, delta);
  //   if (camera) {
  //     log_debug("CAMERA POSITION: %f, %f, %f", camera->position.x,
  //               camera->position.y, camera->position.z);
  //     log_debug("CAMERA FORWARD: %f, %f, %f", camera->forward.x,
  //               camera->forward.y, camera->forward.z);
  //   } else {
  //     log_debug("CAMERA INVALID");
  //   }
  // }

  // VkrQuat q =
  //     vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 0.5f * delta);
  // VkrMesh *falcon =
  //     vkr_mesh_manager_get(&application->renderer.mesh_manager, 0);
  // if (falcon) {
  //   vkr_transform_rotate(&falcon->transform, q);
  // }
  // uint32_t mesh_capacity =
  //     vkr_mesh_manager_capacity(&application->renderer.mesh_manager);
  // uint32_t rotated = 0;
  // for (uint32_t mesh_index = 0; mesh_index < mesh_capacity; ++mesh_index) {
  //   VkrMesh *m =
  //       vkr_mesh_manager_get(&application->renderer.mesh_manager,
  //       mesh_index);
  //   if (!m)
  //     continue;
  //   if (rotated >= 3)
  //     break;
  //   vkr_transform_rotate(&m->transform, q);
  //   rotated++;
  // }

  application_handle_input(application, delta);

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

  // if (input_is_key_up(state->input_state, KEY_NUMPAD1) &&
  //     input_was_key_down(state->input_state, KEY_NUMPAD1)) {
  //   application->renderer.globals.ambient_color =
  //       vec4_add(application->renderer.globals.ambient_color,
  //                vec4_new(0.01f, 0.01f, 0.01f, 0.0f));
  //   log_debug("AMBIENT COLOR: %f, %f, %f, %f",
  //             application->renderer.globals.ambient_color.x,
  //             application->renderer.globals.ambient_color.y,
  //             application->renderer.globals.ambient_color.z,
  //             application->renderer.globals.ambient_color.w);
  // }

  // if (input_is_key_up(state->input_state, KEY_NUMPAD2) &&
  //     input_was_key_down(state->input_state, KEY_NUMPAD2)) {
  //   application->renderer.globals.ambient_color =
  //       vec4_sub(application->renderer.globals.ambient_color,
  //                vec4_new(0.01f, 0.01f, 0.01f, 0.0f));
  //   log_debug("AMBIENT COLOR: %f, %f, %f, %f",
  //             application->renderer.globals.ambient_color.x,
  //             application->renderer.globals.ambient_color.y,
  //             application->renderer.globals.ambient_color.z,
  //             application->renderer.globals.ambient_color.w);
  // }

  if (input_is_key_up(state->input_state, KEY_Q) &&
      input_was_key_down(state->input_state, KEY_Q)) {
    application->renderer.globals.render_mode =
        (VkrRenderMode)(((uint32_t)application->renderer.globals.render_mode +
                         1) %
                        VKR_RENDER_MODE_COUNT);
    log_debug("RENDER MODE: %d", application->renderer.globals.render_mode);
  }
}

// todo: should look into using DLLs in debug builds for hot reload
// and static for release builds, also should look into how we can
// implement hot reload for the application in debug builds
int main(int argc, char **argv) {
  ApplicationConfig config = {0};
  config.title = "Hello, World!";
  config.x = 100;
  config.y = 100;
  config.width = 800;
  config.height = 600;
  config.app_arena_size = MB(1);
  config.target_frame_rate = 0;
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
    log_fatal("Application creation failed!");
    return 1;
  }

  state = arena_alloc(application.app_arena, sizeof(State),
                      ARENA_MEMORY_TAG_STRUCT);
  state->stats_arena = arena_create(KB(1), KB(1));
  VkrAllocator app_alloc = {.ctx = application.app_arena};
  vkr_allocator_arena(&app_alloc);
  state->entities = array_create_uint16_t(&app_alloc, ENTITY_COUNT);
  state->player_position_x = array_create_float64_t(&app_alloc, ENTITY_COUNT);
  state->player_position_y = array_create_float64_t(&app_alloc, ENTITY_COUNT);
  state->input_state = &application.window.input_state;
  state->use_gamepad = false_v;
  int8_t initial_wheel_delta = 0;
  input_get_mouse_wheel(state->input_state, &initial_wheel_delta);
  state->previous_wheel_delta = initial_wheel_delta;
  state->app_arena = application.app_arena;
  state->event_arena = application.event_manager.arena;
  state->event_manager = &application.event_manager;

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
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  application_start(&application);
  application_close(&application);

  array_destroy_uint16_t(&state->entities);
  array_destroy_float64_t(&state->player_position_x);
  array_destroy_float64_t(&state->player_position_y);

  application_shutdown(&application);

  return 0;
}

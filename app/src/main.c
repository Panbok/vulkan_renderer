#include "application.h"
#include "containers/array.h"
#include "core/event.h"
#include "core/input.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/arena.h"
#include "renderer/renderer.h"

#define PLAYER_SPEED 50.0
#define ENTITY_COUNT 1

typedef struct State {
  Array_uint16_t entities;

  Array_float64_t player_position_x;
  Array_float64_t player_position_y;

  InputState *input_state;

  Arena *app_arena;
  Arena *event_arena;
  Arena *stats_arena;

  // todo: test materials
  VkrResourceHandleInfo materials[3];
  uint8_t material_index;
} State;

vkr_global State *state = NULL;

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

VkrCamera application_init_camera(Application *application) {
  VkrCamera camera;
  vkr_camera_system_perspective_create(
      &camera, &application->window.input_state, &application->window,
      application->config->target_frame_rate, 60.0f, 0.1f, 100.0f);
  return camera;
}

void application_update(Application *application, float64_t delta) {
  double fps_value = 0.0;
  if (delta > 0.000001) {
    fps_value = 1.0 / delta;
  } else {
    fps_value = 1.0 / application->config->target_frame_rate;
  }
  vkr_local_persist int log_fps_counter = 0;
  if (++log_fps_counter % 60 == 0) { // Log every 60 mouse moves to avoid spam
    log_debug("CALCULATED FPS VALUE: %f, DELTA WAS: %f", fps_value, delta);
  }

  // Add rotation to test descriptors
  vkr_local_persist float64_t rotation_angle = 0.0;
  rotation_angle += delta * 90.0; // Rotate 90 degrees per second
  if (rotation_angle > 360.0) {
    rotation_angle -= 360.0;
  }

  // Quat q = quat_from_euler(to_radians(rotation_angle),
  //                          to_radians(rotation_angle), 0);
  // application->pipeline.shader_object_description.shader_state_object.model =
  //     quat_to_mat4(q);

  if (input_is_key_up(state->input_state, KEY_T) &&
      input_was_key_down(state->input_state, KEY_T)) {
    log_debug("Material index: %d", state->material_index);
    // Update material color factor and texture (simple showcase)
    VkrPhongProperties phong_props = {
        .diffuse_color = vec4_new(1, 1, 1, 1),
        .specular_color = vec4_new(1, 1, 1, 1),
        .shininess = 32.0f,
        .emission_color = vec3_new(0, 0, 0),
    };
    application->current_material =
        state->materials[state->material_index].as.material;
    state->material_index++;
    if (state->material_index >= 3) {
      state->material_index = 0;
    }
  }

  if (state == NULL || state->input_state == NULL) {
    log_error("State or input state is NULL");
    return;
  }

  if (input_is_key_up(state->input_state, KEY_M) &&
      input_was_key_down(state->input_state, KEY_M)) {
    Scratch scratch_stats = scratch_create(state->stats_arena);
    char *arena_stats =
        arena_format_statistics(state->app_arena, scratch_stats.arena);
    char *event_stats =
        arena_format_statistics(state->event_arena, scratch_stats.arena);
    log_debug("Application arena stats:\n%s", arena_stats);
    log_debug("Event arena stats:\n%s", event_stats);
    scratch_destroy(scratch_stats, ARENA_MEMORY_TAG_STRING);
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
  config.target_frame_rate = 60;
  config.device_requirements = (DeviceRequirements){
      .supported_stages = SHADER_STAGE_VERTEX_BIT | SHADER_STAGE_FRAGMENT_BIT,
      .supported_queues = DEVICE_QUEUE_GRAPHICS_BIT |
                          DEVICE_QUEUE_TRANSFER_BIT | DEVICE_QUEUE_PRESENT_BIT,
      .allowed_device_types =
          DEVICE_TYPE_DISCRETE_BIT | DEVICE_TYPE_INTEGRATED_BIT,
      .supported_sampler_filters = SAMPLER_FILTER_ANISOTROPIC_BIT,
  };

  Application application = {0};
  if (!application_create(&application, &config)) {
    log_fatal("Application creation failed!");
    return 1;
  }

  state = arena_alloc(application.app_arena, sizeof(State),
                      ARENA_MEMORY_TAG_STRUCT);
  state->stats_arena = arena_create(KB(1), KB(1));
  state->entities = array_create_uint16_t(application.app_arena, ENTITY_COUNT);
  state->player_position_x =
      array_create_float64_t(application.app_arena, ENTITY_COUNT);
  state->player_position_y =
      array_create_float64_t(application.app_arena, ENTITY_COUNT);
  state->input_state = &application.window.input_state;
  state->app_arena = application.app_arena;
  state->event_arena = application.event_manager.arena;

  state->material_index = 0;

  VkrResourceHandleInfo material_info = {0};
  RendererError material_load_error = RENDERER_ERROR_NONE;
  if (vkr_resource_system_load(
          VKR_RESOURCE_TYPE_MATERIAL, string8_lit("assets/default1.mt"),
          application.app_arena, &material_info, &material_load_error)) {
    log_info("Successfully loaded default material from assets/default1.mt");
    state->materials[0] = material_info;
  } else {
    log_warn("Failed to load default material from assets/default1.mt; using "
             "built-in default: %s",
             renderer_get_error_string(material_load_error).str);
  }

  if (vkr_resource_system_load(
          VKR_RESOURCE_TYPE_MATERIAL, string8_lit("assets/default2.mt"),
          application.app_arena, &material_info, &material_load_error)) {
    log_info("Successfully loaded default material from assets/default2.mt");
    state->materials[1] = material_info;
  } else {
    log_warn("Failed to load default material from assets/default2.mt; using "
             "built-in default: %s",
             renderer_get_error_string(material_load_error).str);
  }

  if (vkr_resource_system_load(
          VKR_RESOURCE_TYPE_MATERIAL, string8_lit("assets/default3.mt"),
          application.app_arena, &material_info, &material_load_error)) {
    log_info("Successfully loaded default material from assets/default3.mt");
    state->materials[2] = material_info;
  } else {
    log_warn("Failed to load default material from assets/default3.mt; using "
             "built-in default: %s",
             renderer_get_error_string(material_load_error).str);
  }

  Scratch scratch = scratch_create(application.app_arena);
  DeviceInformation device_information;
  renderer_get_device_information(application.renderer, &device_information,
                                  scratch.arena);
  log_info("Device Name: %s", device_information.device_name.str);
  log_info("Device Vendor: %s", device_information.vendor_name.str);
  log_info("Device Driver Version: %s", device_information.driver_version.str);
  log_info("Device Graphics API Version: %s",
           device_information.api_version.str);
  log_info("Device VRAM Size: %.2f GB",
           (float64_t)device_information.vram_size / GB(1));
  log_info("Device VRAM Local Size: %.2f GB",
           (float64_t)device_information.vram_local_size / GB(1));
  log_info("Device VRAM Shared Size: %.2f GB",
           (float64_t)device_information.vram_shared_size / GB(1));
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  application_start(&application);
  application_close(&application);

  array_destroy_uint16_t(&state->entities);
  array_destroy_float64_t(&state->player_position_x);
  array_destroy_float64_t(&state->player_position_y);

  application_shutdown(&application);

  return 0;
}
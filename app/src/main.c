#include "application.h"
#include "containers/array.h"
#include "core/event.h"
#include "core/input.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
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
} State;

State *state = NULL;

bool8_t application_on_event(Event *event) {
  // log_debug("Application on event: %d", event->type);
  return true_v;
}

bool8_t application_on_window_event(Event *event) {
  // log_debug("Application on window event: %d", event->type);
  return true_v;
}

bool8_t application_on_key_event(Event *event) { return true_v; }

bool8_t application_on_mouse_event(Event *event) {
  // log_debug("Application on mouse event: %d", event->type);
  return true_v;
}

void application_update(Application *application, float64_t delta) {
  double fps_value = 0.0;
  if (delta > 0.000001) {
    fps_value = 1.0 / delta;
  } else {
    fps_value = 1.0 / application->config->target_frame_rate;
  }
  // log_debug("CALCULATED FPS VALUE: %f, DELTA WAS: %f", fps_value, delta);

  if (state == NULL || state->input_state == NULL) {
    log_error("State or input state is NULL");
    return;
  }

  InputState *input_state = state->input_state;
  for (uint16_t i = 0; i < state->entities.length; i++) {
    float64_t *player_position_x =
        array_get_float64_t(&state->player_position_x, i);
    float64_t *player_position_y =
        array_get_float64_t(&state->player_position_y, i);

    if (input_is_key_down(input_state, KEY_W)) {
      *player_position_x += PLAYER_SPEED * delta;
    }
    if (input_is_key_down(input_state, KEY_S)) {
      *player_position_x -= PLAYER_SPEED * delta;
    }
    if (input_is_key_down(input_state, KEY_A)) {
      *player_position_y += PLAYER_SPEED * delta;
    }
    if (input_is_key_down(input_state, KEY_D)) {
      *player_position_y -= PLAYER_SPEED * delta;
    }

    // log_debug("Entity ID: %u, Player Position: (x - %f, y - %f)",
    //           *array_get_uint16_t(&state->entities, i), *player_position_x,
    //           *player_position_y);
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

  Application application;
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
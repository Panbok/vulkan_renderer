#include "application.h"
#include "arena.h"
#include "array.h"
#include "event.h"
#include "input.h"
#include "logger.h"

#define PLAYER_SPEED 50.0
#define ENTITY_COUNT 1

typedef struct State {
  Array_uint16_t entities;

  Array_float64_t player_position_x;
  Array_float64_t player_position_y;

  InputState *input_state;
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

bool8_t application_on_key_event(Event *event) {
  // KeyEventData *key_event_data = (KeyEventData *)event->data;
  return true_v;
}

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
  log_debug("CALCULATED FPS VALUE: %f, DELTA WAS: %f", fps_value, delta);

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

    log_debug("Entity ID: %u, Player Position: (x - %f, y - %f)",
              *array_get_uint16_t(&state->entities, i), *player_position_x,
              *player_position_y);
  }
}

int main(int argc, char **argv) {
  ApplicationConfig config = {0};
  config.title = "Hello, World!";
  config.x = 100;
  config.y = 100;
  config.width = 800;
  config.height = 600;
  config.app_arena_size = MB(1);
  config.target_frame_rate = 60;

  Application application;
  if (!application_create(&application, &config)) {
    log_fatal("Application creation failed!");
    return 1;
  }

  state = arena_alloc(application.app_arena, sizeof(State));
  state->entities = array_create_uint16_t(application.app_arena, ENTITY_COUNT);
  state->player_position_x =
      array_create_float64_t(application.app_arena, ENTITY_COUNT);
  state->player_position_y =
      array_create_float64_t(application.app_arena, ENTITY_COUNT);
  state->input_state = &application.window.input_state;

  application_start(&application);
  application_close(&application);

  array_destroy_uint16_t(&state->entities);
  array_destroy_float64_t(&state->player_position_x);
  array_destroy_float64_t(&state->player_position_y);

  application_shutdown(&application);
  return 0;
}
#include "input.h"

InputState input_init(EventManager *event_manager) {
  assert_log(event_manager != NULL, "Event manager is NULL");

  InputState input_state = {
      .event_manager = event_manager,
      .previous_keys = {0},
      .current_keys = {0},
      .previous_buttons = {0},
      .current_buttons = {0},
      .previous_axes = {0},
      .current_axes = {0},
      .is_initialized = true,
  };

  Event event = {
      .type = EVENT_TYPE_INPUT_SYSTEM_INIT,
      .data = NULL,
      .data_size = 0,
  };

  if (!event_manager_dispatch(event_manager, event)) {
    log_warn("Failed to enqueue INPUT_SYSTEM_INIT event");
  }

  log_debug("Input system initialized");
  return input_state;
}

void input_shutdown(InputState *input_state) {
  assert_log(input_state != NULL, "Input state is NULL");

  Event event = {
      .type = EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN,
      .data = NULL,
      .data_size = 0,
  };
  input_state->is_initialized = false;
  event_manager_dispatch(input_state->event_manager, event);
  log_debug("Input system shutdown");
}

void input_update(InputState *input_state) {
  assert_log(input_state != NULL, "Input state is NULL");

  MemCopy(&input_state->previous_keys, &input_state->current_keys,
          sizeof(KeysState));
  MemCopy(&input_state->previous_buttons, &input_state->current_buttons,
          sizeof(ButtonsState));
  MemCopy(&input_state->previous_axes, &input_state->current_axes,
          sizeof(GamepadAxes));
}

bool8_t input_is_key_down(InputState *input_state, Keys key) {
  return input_state->current_keys.keys[key];
}

bool8_t input_is_key_up(InputState *input_state, Keys key) {
  return !input_state->current_keys.keys[key];
}

bool8_t input_was_key_down(InputState *input_state, Keys key) {
  return input_state->previous_keys.keys[key];
}

bool8_t input_was_key_up(InputState *input_state, Keys key) {
  return !input_state->previous_keys.keys[key];
}

bool8_t input_is_button_down(InputState *input_state, Buttons button) {
  return input_state->current_buttons.buttons[button];
}

bool8_t input_is_button_up(InputState *input_state, Buttons button) {
  return !input_state->current_buttons.buttons[button];
}

bool8_t input_was_button_down(InputState *input_state, Buttons button) {
  return input_state->previous_buttons.buttons[button];
}

bool8_t input_was_button_up(InputState *input_state, Buttons button) {
  return !input_state->previous_buttons.buttons[button];
}

void input_process_key(InputState *input_state, Keys key, bool8_t pressed) {
  if (input_state->current_keys.keys[key] != pressed) {
    input_state->current_keys.keys[key] = pressed;

    KeyEventData key_event_data = {
        .key = key,
        .pressed = pressed,
    };

    Event event = {
        .type = pressed ? EVENT_TYPE_KEY_PRESS : EVENT_TYPE_KEY_RELEASE,
        .data = (void *)&key_event_data,
        .data_size = sizeof(KeyEventData),
    };
    event_manager_dispatch(input_state->event_manager, event);
  }
}

void input_process_button(InputState *input_state, Buttons button,
                          bool8_t pressed) {
  if (input_state->current_buttons.buttons[button] != pressed) {
    input_state->current_buttons.buttons[button] = pressed;

    ButtonEventData button_event_data = {
        .button = button,
        .pressed = pressed,
    };

    Event event = {
        .type = pressed ? EVENT_TYPE_BUTTON_PRESS : EVENT_TYPE_BUTTON_RELEASE,
        .data = (void *)&button_event_data,
        .data_size = sizeof(ButtonEventData),
    };
    event_manager_dispatch(input_state->event_manager, event);
  }
}

void input_process_mouse_move(InputState *input_state, int32_t x, int32_t y) {
  if (input_state->current_buttons.x != x ||
      input_state->current_buttons.y != y) {
    input_state->current_buttons.x = x;
    input_state->current_buttons.y = y;

    MouseMoveEventData mouse_move_event_data = {
        .x = x,
        .y = y,
    };

    Event event = {
        .type = EVENT_TYPE_MOUSE_MOVE,
        .data = (void *)&mouse_move_event_data,
        .data_size = sizeof(MouseMoveEventData),
    };
    event_manager_dispatch(input_state->event_manager, event);
  }
}

void input_process_mouse_wheel(InputState *input_state, int8_t delta) {
  if (input_state->current_buttons.wheel != delta) {
    input_state->current_buttons.wheel = delta;

    MouseWheelEventData mouse_wheel_event_data = {
        .delta = delta,
    };

    Event event = {
        .type = EVENT_TYPE_MOUSE_WHEEL,
        .data = (void *)&mouse_wheel_event_data,
        .data_size = sizeof(MouseWheelEventData),
    };
    event_manager_dispatch(input_state->event_manager, event);
  }
}

void input_get_mouse_position(InputState *input_state, int32_t *x, int32_t *y) {
  *x = input_state->current_buttons.x;
  *y = input_state->current_buttons.y;
}

void input_get_previous_mouse_position(InputState *input_state, int32_t *x,
                                       int32_t *y) {
  *x = input_state->previous_buttons.x;
  *y = input_state->previous_buttons.y;
}

void input_get_mouse_wheel(InputState *input_state, int8_t *delta) {
  *delta = input_state->current_buttons.wheel;
}

void input_process_thumbsticks(InputState *input_state, float left_x,
                               float left_y, float right_x, float right_y) {
  assert_log(input_state != NULL, "Input state is NULL");
  input_state->current_axes.left_x = left_x;
  input_state->current_axes.left_y = left_y;
  input_state->current_axes.right_x = right_x;
  input_state->current_axes.right_y = right_y;
}

void input_get_left_stick(InputState *input_state, float *x, float *y) {
  assert_log(input_state != NULL && x && y, "Invalid args");
  *x = input_state->current_axes.left_x;
  *y = input_state->current_axes.left_y;
}

void input_get_previous_left_stick(InputState *input_state, float *x,
                                   float *y) {
  assert_log(input_state != NULL && x && y, "Invalid args");
  *x = input_state->previous_axes.left_x;
  *y = input_state->previous_axes.left_y;
}

void input_get_right_stick(InputState *input_state, float *x, float *y) {
  assert_log(input_state != NULL && x && y, "Invalid args");
  *x = input_state->current_axes.right_x;
  *y = input_state->current_axes.right_y;
}

void input_get_previous_right_stick(InputState *input_state, float *x,
                                    float *y) {
  assert_log(input_state != NULL && x && y, "Invalid args");
  *x = input_state->previous_axes.right_x;
  *y = input_state->previous_axes.right_y;
}

#include "input.h"

typedef struct KeysState {
  bool8_t keys[KEYS_MAX_KEYS];
} KeysState;

typedef struct ButtonsState {
  bool8_t buttons[BUTTON_MAX_BUTTONS];
  int32_t x;
  int32_t y;
  int8_t wheel;
} ButtonsState;

typedef struct InputState {
  EventManager *event_manager;
  KeysState previous_keys;
  KeysState current_keys;
  ButtonsState previous_buttons;
  ButtonsState current_buttons;
} InputState;

static bool8_t input_initialized = false;
static InputState input_state = {
    .event_manager = NULL,
    .previous_keys = {0},
    .current_keys = {0},
    .previous_buttons = {0},
    .current_buttons = {0},
};

void input_init(EventManager *event_manager) {
  assert_log(event_manager != NULL, "Event manager is NULL");

  if (input_initialized) {
    log_warn("Input system already initialized");
    return;
  }

  input_state.event_manager = event_manager;

  Event event = {
      .type = EVENT_TYPE_INPUT_SYSTEM_INIT,
      .data = NULL,
      .data_size = 0,
  };

  input_initialized = true;
  event_manager_dispatch(event_manager, event);
  log_info("Input system initialized");
}

void input_shutdown() {
  assert_log(input_state.event_manager != NULL, "Event manager is NULL");

  if (!input_initialized) {
    log_warn("Input system not initialized");
    return;
  }

  Event event = {
      .type = EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN,
      .data = NULL,
      .data_size = 0,
  };
  event_manager_dispatch(input_state.event_manager, event);
  input_initialized = false;
  log_info("Input system shutdown");
}

void input_update(float64_t delta_time) {
  if (!input_initialized) {
    log_warn("Input system not initialized");
    return;
  }

  MemCopy(&input_state.previous_keys, &input_state.current_keys,
          sizeof(KeysState));
  MemCopy(&input_state.previous_buttons, &input_state.current_buttons,
          sizeof(ButtonsState));
}

bool8_t input_is_key_down(Keys key) {
  return input_state.current_keys.keys[key];
}

bool8_t input_is_key_up(Keys key) {
  return !input_state.current_keys.keys[key];
}

bool8_t input_was_key_down(Keys key) {
  return input_state.previous_keys.keys[key];
}

bool8_t input_was_key_up(Keys key) {
  return !input_state.previous_keys.keys[key];
}

bool8_t input_is_button_down(Buttons button) {
  return input_state.current_buttons.buttons[button];
}

bool8_t input_is_button_up(Buttons button) {
  return !input_state.current_buttons.buttons[button];
}

bool8_t input_was_button_down(Buttons button) {
  return input_state.previous_buttons.buttons[button];
}

bool8_t input_was_button_up(Buttons button) {
  return !input_state.previous_buttons.buttons[button];
}

void input_process_key(Keys key, bool8_t pressed) {
  if (input_state.current_keys.keys[key] != pressed) {
    input_state.current_keys.keys[key] = pressed;

    KeyEventData key_event_data = {
        .key = key,
        .pressed = pressed,
    };

    Event event = {
        .type = pressed ? EVENT_TYPE_KEY_PRESS : EVENT_TYPE_KEY_RELEASE,
        .data = (void *)&key_event_data,
        .data_size = sizeof(KeyEventData),
    };
    event_manager_dispatch(input_state.event_manager, event);
  }
}

void input_process_button(Buttons button, bool8_t pressed) {
  if (input_state.current_buttons.buttons[button] != pressed) {
    input_state.current_buttons.buttons[button] = pressed;

    ButtonEventData button_event_data = {
        .button = button,
        .pressed = pressed,
    };

    Event event = {
        .type = pressed ? EVENT_TYPE_BUTTON_PRESS : EVENT_TYPE_BUTTON_RELEASE,
        .data = (void *)&button_event_data,
        .data_size = sizeof(ButtonEventData),
    };
    event_manager_dispatch(input_state.event_manager, event);
  }
}

void input_process_mouse_move(int16_t x, int16_t y) {
  if (input_state.current_buttons.x != x ||
      input_state.current_buttons.y != y) {
    input_state.current_buttons.x = x;
    input_state.current_buttons.y = y;

    MouseMoveEventData mouse_move_event_data = {
        .x = x,
        .y = y,
    };

    Event event = {
        .type = EVENT_TYPE_MOUSE_MOVE,
        .data = (void *)&mouse_move_event_data,
        .data_size = sizeof(MouseMoveEventData),
    };
    event_manager_dispatch(input_state.event_manager, event);
  }
}

void input_process_mouse_wheel(int8_t delta) {
  if (input_state.current_buttons.wheel != delta) {
    input_state.current_buttons.wheel = delta;

    MouseWheelEventData mouse_wheel_event_data = {
        .delta = delta,
    };

    Event event = {
        .type = EVENT_TYPE_MOUSE_WHEEL,
        .data = (void *)&mouse_wheel_event_data,
        .data_size = sizeof(MouseWheelEventData),
    };
    event_manager_dispatch(input_state.event_manager, event);
  }
}

void input_get_mouse_position(int32_t *x, int32_t *y) {
  *x = input_state.current_buttons.x;
  *y = input_state.current_buttons.y;
}

void input_get_previous_mouse_position(int32_t *x, int32_t *y) {
  *x = input_state.previous_buttons.x;
  *y = input_state.previous_buttons.y;
}

void input_get_mouse_wheel(int8_t *delta) {
  *delta = input_state.current_buttons.wheel;
}

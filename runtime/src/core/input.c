#include "input.h"

InputState input_init(EventManager *event_manager) {
  assert_log(event_manager != NULL, "Event manager is NULL");

  InputState input_state = {
      .event_manager = event_manager,
      .previous_keys = {0},
      .current_keys = {0},
      .previous_buttons = {0},
      .current_buttons = {0},
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
  MemZero(&input_state->pressed_keys, sizeof(input_state->pressed_keys));
  MemZero(&input_state->released_keys, sizeof(input_state->released_keys));
  MemZero(input_state->pressed_buttons, sizeof(input_state->pressed_buttons));
  MemZero(input_state->released_buttons, sizeof(input_state->released_buttons));
  MemZero(input_state->key_press_modifiers,
          sizeof(input_state->key_press_modifiers));
  input_state->character_count = 0u;
  input_state->dropped_character_count = 0u;
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

bool8_t input_key_just_pressed(const InputState *input_state, Keys key) {
  return input_state->pressed_keys.keys[key] ||
         (input_state->current_keys.keys[key] &&
          !input_state->previous_keys.keys[key]);
}

bool8_t input_key_just_released(const InputState *input_state, Keys key) {
  return input_state->released_keys.keys[key] ||
         (!input_state->current_keys.keys[key] &&
          input_state->previous_keys.keys[key]);
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

bool8_t input_button_just_pressed(const InputState *input_state,
                                  Buttons button) {
  return input_state->pressed_buttons[button] ||
         (input_state->current_buttons.buttons[button] &&
          !input_state->previous_buttons.buttons[button]);
}

bool8_t input_button_just_released(const InputState *input_state,
                                   Buttons button) {
  return input_state->released_buttons[button] ||
         (!input_state->current_buttons.buttons[button] &&
          input_state->previous_buttons.buttons[button]);
}

static uint8_t input_held_modifiers(const InputState *input) {
  const bool8_t *keys = input->current_keys.keys;
  return ((keys[KEY_SHIFT] || keys[KEY_LSHIFT] || keys[KEY_RSHIFT])
              ? VKR_INPUT_MOD_SHIFT
              : 0) |
         ((keys[KEY_CONTROL] || keys[KEY_LCONTROL] || keys[KEY_RCONTROL])
              ? VKR_INPUT_MOD_CONTROL
              : 0) |
         ((keys[KEY_LMENU] || keys[KEY_RMENU]) ? VKR_INPUT_MOD_ALT : 0) |
         ((keys[KEY_LWIN] || keys[KEY_RWIN]) ? VKR_INPUT_MOD_SUPER : 0);
}

uint8_t input_key_press_modifiers(const InputState *input_state, Keys key) {
  return input_state->pressed_keys.keys[key]
             ? input_state->key_press_modifiers[key]
             : input_held_modifiers(input_state);
}

bool8_t input_key_shortcut_modifier(const InputState *input_state, Keys key) {
  const uint8_t modifiers = input_key_press_modifiers(input_state, key);
#if defined(PLATFORM_APPLE)
  return (modifiers & VKR_INPUT_MOD_SUPER) != 0;
#else
  return (modifiers & (VKR_INPUT_MOD_CONTROL | VKR_INPUT_MOD_ALT)) ==
         VKR_INPUT_MOD_CONTROL;
#endif
}

void input_process_key(InputState *input_state, Keys key, bool8_t pressed) {
  if (input_state->current_keys.keys[key] != pressed) {
    input_state->current_keys.keys[key] = pressed;
    if (pressed) {
      if (!input_state->pressed_keys.keys[key])
        input_state->key_press_modifiers[key] =
            input_held_modifiers(input_state);
      input_state->pressed_keys.keys[key] = true_v;
    } else
      input_state->released_keys.keys[key] = true_v;

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

bool8_t input_process_char(InputState *input_state, uint32_t codepoint) {
  if (!input_state || codepoint > 0x10ffffu ||
      (codepoint >= 0xd800u && codepoint <= 0xdfffu))
    return false_v;
  if (input_state->character_count == VKR_INPUT_CHARACTER_CAPACITY) {
    input_state->dropped_character_count++;
    return false_v;
  }
  input_state->characters[input_state->character_count++] = codepoint;
  return true_v;
}

const uint32_t *input_get_characters(const InputState *input_state,
                                     uint32_t *out_count) {
  if (out_count)
    *out_count = input_state ? input_state->character_count : 0u;
  return input_state ? input_state->characters : NULL;
}

uint32_t input_get_dropped_character_count(const InputState *input_state) {
  return input_state ? input_state->dropped_character_count : 0u;
}

void input_process_button(InputState *input_state, Buttons button,
                          bool8_t pressed) {
  if (input_state->current_buttons.buttons[button] != pressed) {
    input_state->current_buttons.buttons[button] = pressed;
    if (pressed) {
      if (!input_state->pressed_buttons[button]) {
        input_state->button_press_x[button] = input_state->current_buttons.x;
        input_state->button_press_y[button] = input_state->current_buttons.y;
      }
      input_state->pressed_buttons[button] = true_v;
    } else
      input_state->released_buttons[button] = true_v;

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

void input_get_button_press_position(const InputState *input_state,
                                     Buttons button, int32_t *x, int32_t *y) {
  const bool8_t pressed = input_state->pressed_buttons[button];
  *x = pressed ? input_state->button_press_x[button]
               : input_state->current_buttons.x;
  *y = pressed ? input_state->button_press_y[button]
               : input_state->current_buttons.y;
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

void input_get_mouse_delta(const InputState *input_state, int32_t *dx,
                           int32_t *dy) {
  int32_t current_x = 0;
  int32_t current_y = 0;
  int32_t previous_x = 0;
  int32_t previous_y = 0;
  input_get_mouse_position((InputState *)input_state, &current_x, &current_y);
  input_get_previous_mouse_position((InputState *)input_state, &previous_x,
                                    &previous_y);
  *dx = current_x - previous_x;
  *dy = current_y - previous_y;
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

void input_get_right_stick(InputState *input_state, float *x, float *y) {
  assert_log(input_state != NULL && x && y, "Invalid args");
  *x = input_state->current_axes.right_x;
  *y = input_state->current_axes.right_y;
}

#include "core/vkr_gamepad.h"

/* Platform-independent gamepad bookkeeping. Native discovery, connection and
 * polling live in platform/vkr_gamepad_<platform>. */

static void vkr_gamepad_release_all(InputState *input_state) {
  assert_log(input_state, "Input state is NULL");

  input_process_button(input_state, BUTTON_GAMEPAD_A, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_B, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_X, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_Y, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_LEFT_SHOULDER, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_RIGHT_SHOULDER, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_LEFT_TRIGGER, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_RIGHT_TRIGGER, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_BACK, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_START, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_UP, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_DOWN, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_LEFT, false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_RIGHT, false_v);

  input_process_thumbsticks(input_state, 0.0f, 0.0f, 0.0f, 0.0f);
}

bool8_t vkr_gamepad_poll_all(VkrGamepad *gamepad) {
  assert_log(gamepad, "Gamepad is NULL");
  assert_log(gamepad->input_state, "Input state is NULL");

  for (int32_t i = 0; i < VKR_GAMEPAD_MAX_CONTROLLERS; i++) {
    (void)vkr_gamepad_poll(gamepad, i);
  }

  return true_v;
}

bool8_t vkr_gamepad_disconnect(VkrGamepad *system, int32_t controller_id) {
  assert_log(system, "Gamepad system is NULL");
  assert_log(controller_id >= 0 && controller_id < VKR_GAMEPAD_MAX_CONTROLLERS,
             "Controller id is out of bounds");

  system->gamepads[controller_id].is_connected = false_v;
  vkr_gamepad_release_all(system->input_state);
  log_debug("Gamepad %d disconnected", controller_id);
  return true_v;
}

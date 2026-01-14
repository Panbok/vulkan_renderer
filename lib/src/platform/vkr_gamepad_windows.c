#include "core/vkr_gamepad.h"

// todo: we should re-work our windows impl from XInput to GameInput API,
// the current impl doesn't support DualSense, DualShock, etc. only Xbox
// controllers are supported, if we would want to support other controllers
// either we would need to add DirectInput to our existing XInput impl or
// re-work the impl to use GameInput API
// NOTE: GameInput API is C++ only, so we can't use it in our C code, for
// native DualSense, DualShock, etc. controllers we would need to use
// DirectInput

#if defined(PLATFORM_WINDOWS)

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
}

bool8_t vkr_gamepad_init(VkrGamepad *gamepad, InputState *input_state) {
  assert_log(gamepad, "Gamepad system is NULL");
  assert_log(input_state, "Input state is NULL");

  for (uint8_t i = 0; i < VKR_GAMEPAD_MAX_CONTROLLERS; i++) {
    gamepad->gamepads[i].id = i;
    gamepad->gamepads[i].is_connected = false_v;
    gamepad->gamepads[i].type = VKR_GAMEPAD_TYPE_GENERIC;
  }

  log_debug("Initializing gamepad system");

  gamepad->input_state = input_state;
  return true_v;
}

bool8_t vkr_gamepad_connect(VkrGamepad *system, int32_t controller_id) {
  assert_log(system, "Gamepad system is NULL");
  assert_log(controller_id >= 0 && controller_id < VKR_GAMEPAD_MAX_CONTROLLERS,
             "Controller id is out of bounds");
  assert_log(system->input_state, "Input state is NULL");

  XINPUT_STATE state;
  DWORD result = XInputGetState((DWORD)controller_id, &state);
  if (result == ERROR_SUCCESS &&
      !system->gamepads[controller_id].is_connected) {
    system->gamepads[controller_id].is_connected = true_v;
    system->gamepads[controller_id].type = VKR_GAMEPAD_TYPE_XBOX;
    log_debug("Gamepad %d connected", controller_id);
    return true_v;
  } else if (result != ERROR_SUCCESS &&
             system->gamepads[controller_id].is_connected) {
    vkr_gamepad_disconnect(system, controller_id);
    return false_v;
  } else if (result == ERROR_SUCCESS &&
             system->gamepads[controller_id].is_connected) {
    return true_v;
  }

  return false_v;
}

bool8_t vkr_gamepad_poll_all(VkrGamepad *gamepad) {
  assert_log(gamepad, "Gamepad is NULL");
  assert_log(gamepad->input_state, "Input state is NULL");

  for (int32_t i = 0; i < VKR_GAMEPAD_MAX_CONTROLLERS; i++) {
    VkrGamepadState *gamepad_state = &gamepad->gamepads[i];
    if (!vkr_gamepad_poll(gamepad, i)) {
      continue;
    }
  }

  return true_v;
}

bool8_t vkr_gamepad_poll(VkrGamepad *system, int32_t controller_id) {
  assert_log(system, "Gamepad system is NULL");
  assert_log(controller_id >= 0 && controller_id < VKR_GAMEPAD_MAX_CONTROLLERS,
             "Controller id is out of bounds");
  assert_log(system->input_state, "Input state is NULL");

  if (!vkr_gamepad_connect(system, controller_id)) {
    return false_v;
  }

  // NOTE: Double polling is not ideal, probably better to set it as a state on
  // a gamepad when we connect just once and simply pull the data from there
  XINPUT_STATE xi_state;
  DWORD result = XInputGetState((DWORD)controller_id, &xi_state);
  if (result != ERROR_SUCCESS) {
    log_warn("Failed to poll gamepad %d", controller_id);
    return false_v;
  }

  InputState *input_state = system->input_state;
  const XINPUT_GAMEPAD *g = &xi_state.Gamepad;

  // Buttons
  input_process_button(input_state, BUTTON_GAMEPAD_A,
                       (g->wButtons & XINPUT_GAMEPAD_A) ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_B,
                       (g->wButtons & XINPUT_GAMEPAD_B) ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_X,
                       (g->wButtons & XINPUT_GAMEPAD_X) ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_Y,
                       (g->wButtons & XINPUT_GAMEPAD_Y) ? true_v : false_v);

  input_process_button(input_state, BUTTON_GAMEPAD_LEFT_SHOULDER,
                       (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? true_v
                                                                    : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_RIGHT_SHOULDER,
                       (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? true_v
                                                                     : false_v);

  input_process_button(input_state, BUTTON_GAMEPAD_BACK,
                       (g->wButtons & XINPUT_GAMEPAD_BACK) ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_START,
                       (g->wButtons & XINPUT_GAMEPAD_START) ? true_v : false_v);

  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_UP,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) ? true_v
                                                              : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_DOWN,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) ? true_v
                                                                : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_LEFT,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) ? true_v
                                                                : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_RIGHT,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) ? true_v
                                                                 : false_v);

  // Triggers (treat as buttons when exceeding threshold)
  const uint8_t lt_thresh = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
  const uint8_t rt_thresh = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
  input_process_button(input_state, BUTTON_GAMEPAD_LEFT_TRIGGER,
                       (g->bLeftTrigger > lt_thresh) ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_RIGHT_TRIGGER,
                       (g->bRightTrigger > rt_thresh) ? true_v : false_v);

  // Thumbsticks: normalize to [-1, 1] with deadzone
  const int16_t lx = g->sThumbLX;
  const int16_t ly = g->sThumbLY;
  const int16_t rx = g->sThumbRX;
  const int16_t ry = g->sThumbRY;

  const int16_t deadzone_l = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
  const int16_t deadzone_r = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;

  float32_t n_lx = 0.0f, n_ly = 0.0f, n_rx = 0.0f, n_ry = 0.0f;

  // Left stick magnitude
  int32_t mag_l = (int32_t)vkr_sqrt_f64((float64_t)lx * (float64_t)lx +
                                        (float64_t)ly * (float64_t)ly);
  if (mag_l > deadzone_l) {
    float32_t nx = (float32_t)lx / VKR_GAMEPAD_THUMB_MAX;
    float32_t ny = (float32_t)ly / VKR_GAMEPAD_THUMB_MAX;
    float32_t scale = (float32_t)(mag_l - deadzone_l) /
                      (float32_t)(VKR_GAMEPAD_THUMB_MAX - deadzone_l);
    if (scale > 1.0f)
      scale = 1.0f;
    n_lx = nx * scale;
    n_ly = ny * scale;
  }

  // Right stick magnitude
  int32_t mag_r = (int32_t)vkr_sqrt_f64((float64_t)rx * (float64_t)rx +
                                        (float64_t)ry * (float64_t)ry);
  if (mag_r > deadzone_r) {
    float32_t nx = (float32_t)rx / VKR_GAMEPAD_THUMB_MAX;
    float32_t ny = (float32_t)ry / VKR_GAMEPAD_THUMB_MAX;
    float32_t scale = (float32_t)(mag_r - deadzone_r) /
                      (float32_t)(VKR_GAMEPAD_THUMB_MAX - deadzone_r);
    if (scale > 1.0f)
      scale = 1.0f;
    n_rx = nx * scale;
    n_ry = ny * scale;
  }

  input_process_thumbsticks(input_state, n_lx, n_ly, n_rx, n_ry);

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

bool8_t vkr_gamepad_shutdown(VkrGamepad *gamepad) {
  assert_log(gamepad, "Gamepad is NULL");

  for (int32_t i = 0; i < VKR_GAMEPAD_MAX_CONTROLLERS; i++) {
    if (!vkr_gamepad_disconnect(gamepad, i)) {
      log_warn("Failed to disconnect gamepad %d", i);
      return false_v;
    }
  }

  log_debug("Gamepad system shutdown");

  return true_v;
}

#endif

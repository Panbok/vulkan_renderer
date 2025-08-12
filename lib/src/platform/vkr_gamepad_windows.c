#include "platform/vkr_gamepad.h"

// todo: we should re-work our windows impl from XInput to GameInput API,
// the current impl doesn't support DualSense, DualShock, etc. only Xbox
// controllers are supported, if we would want to support other controllers
// either we would need to add DirectInput to our existing XInput impl or
// re-work the impl to use GameInput API

#if defined(PLATFORM_WINDOWS)

static void vkr_gamepad_release_all(VkrGamepad *gamepad,
                                    InputState *input_state) {
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

bool8_t vkr_gamepad_init(VkrGamepad *gamepad, int32_t id,
                         InputState *input_state) {
  if (!gamepad)
    return false_v;
  gamepad->id = id;
  gamepad->is_connected = false_v;
  gamepad->type = VKR_GAMEPAD_TYPE_GENERIC;
  gamepad->input_state = input_state;
  return true_v;
}

bool8_t vkr_gamepad_connect(VkrGamepad *gamepad) {
  if (!gamepad)
    return false_v;
  XINPUT_STATE state;
  DWORD result = XInputGetState((DWORD)gamepad->id, &state);
  if (result == ERROR_SUCCESS) {
    gamepad->is_connected = true_v;
    // Heuristic: XInput usually exposes Xbox layout. Keep GENERIC otherwise.
    gamepad->type = VKR_GAMEPAD_TYPE_XBOX;
    return true_v;
  }
  gamepad->is_connected = false_v;
  return false_v;
}

bool8_t vkr_gamepad_poll(VkrGamepad *gamepad) {
  if (!gamepad || !gamepad->input_state)
    return false_v;

  XINPUT_STATE xi_state;
  DWORD result = XInputGetState((DWORD)gamepad->id, &xi_state);
  if (result != ERROR_SUCCESS) {
    if (gamepad->is_connected) {
      // Mark as disconnected and release any held buttons
      gamepad->is_connected = false_v;
      vkr_gamepad_release_all(gamepad, gamepad->input_state);
    }
    return false_v;
  }

  if (!gamepad->is_connected) {
    gamepad->is_connected = true_v;
  }

  const XINPUT_GAMEPAD *g = &xi_state.Gamepad;

  // Buttons
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_A,
                       (g->wButtons & XINPUT_GAMEPAD_A) ? true_v : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_B,
                       (g->wButtons & XINPUT_GAMEPAD_B) ? true_v : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_X,
                       (g->wButtons & XINPUT_GAMEPAD_X) ? true_v : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_Y,
                       (g->wButtons & XINPUT_GAMEPAD_Y) ? true_v : false_v);

  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_LEFT_SHOULDER,
                       (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? true_v
                                                                    : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_RIGHT_SHOULDER,
                       (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? true_v
                                                                     : false_v);

  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_BACK,
                       (g->wButtons & XINPUT_GAMEPAD_BACK) ? true_v : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_START,
                       (g->wButtons & XINPUT_GAMEPAD_START) ? true_v : false_v);

  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_DPAD_UP,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) ? true_v
                                                              : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_DPAD_DOWN,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) ? true_v
                                                                : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_DPAD_LEFT,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) ? true_v
                                                                : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_DPAD_RIGHT,
                       (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) ? true_v
                                                                 : false_v);

  // Triggers (treat as buttons when exceeding threshold)
  const uint8_t lt_thresh = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
  const uint8_t rt_thresh = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_LEFT_TRIGGER,
                       (g->bLeftTrigger > lt_thresh) ? true_v : false_v);
  input_process_button(gamepad->input_state, BUTTON_GAMEPAD_RIGHT_TRIGGER,
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
  // todo: we need srqt_f64 op
  int32_t mag_l = (int32_t)sqrt((float64_t)lx * (float64_t)lx +
                                (float64_t)ly * (float64_t)ly);
  if (mag_l > deadzone_l) {
    float32_t nx = (float32_t)lx / 32767.0f;
    float32_t ny = (float32_t)ly / 32767.0f;
    float32_t scale =
        (float32_t)(mag_l - deadzone_l) / (float32_t)(32767 - deadzone_l);
    if (scale > 1.0f)
      scale = 1.0f;
    n_lx = nx * scale;
    n_ly = ny * scale;
  }

  // Right stick magnitude
  int32_t mag_r = (int32_t)sqrt((float64_t)rx * (float64_t)rx +
                                (float64_t)ry * (float64_t)ry);
  if (mag_r > deadzone_r) {
    float32_t nx = (float32_t)rx / 32767.0f;
    float32_t ny = (float32_t)ry / 32767.0f;
    float32_t scale =
        (float32_t)(mag_r - deadzone_r) / (float32_t)(32767 - deadzone_r);
    if (scale > 1.0f)
      scale = 1.0f;
    n_rx = nx * scale;
    n_ry = ny * scale;
  }

  input_process_thumbsticks(gamepad->input_state, n_lx, n_ly, n_rx, n_ry);
  return true_v;
}

bool8_t vkr_gamepad_disconnect(VkrGamepad *gamepad) {
  if (!gamepad)
    return false_v;
  gamepad->is_connected = false_v;
  return true_v;
}

bool8_t vkr_gamepad_shutdown(VkrGamepad *gamepad) {
  return vkr_gamepad_disconnect(gamepad);
}

#endif

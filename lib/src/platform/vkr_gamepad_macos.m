#include "core/vkr_gamepad.h"
#include "math/math.h"

#if defined(PLATFORM_APPLE)
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

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

static GamepadType vkr_detect_type_from_controller(GCController *controller) {
  assert_log(controller, "Controller is NULL");

  NSString *vendor = controller.vendorName ?: @"";
  NSString *lower = [vendor lowercaseString];
  if ([lower containsString:@"xbox"] || [lower containsString:@"microsoft"]) {
    return VKR_GAMEPAD_TYPE_XBOX;
  }

  if ([lower containsString:@"playstation"] || [lower containsString:@"dual"] ||
      [lower containsString:@"sony"]) {
    return VKR_GAMEPAD_TYPE_PLAYSTATION;
  }

  if ([lower containsString:@"nintendo"] || [lower containsString:@"switch"] ||
      [lower containsString:@"joy-con"]) {
    return VKR_GAMEPAD_TYPE_NINTENDO;
  }

  return VKR_GAMEPAD_TYPE_GENERIC;
}

static GCController *vkr_get_controller_for_index(int32_t controller_id) {
  NSArray<GCController *> *controllers = [GCController controllers];
  if (controller_id < 0 || controller_id >= (int32_t)controllers.count) {
    return nil;
  }

  return controllers[controller_id];
}

bool8_t vkr_gamepad_init(VkrGamepad *gamepad, InputState *input_state) {
  assert_log(gamepad, "Gamepad system is NULL");
  assert_log(input_state, "Input state is NULL");

  for (uint8_t i = 0; i < VKR_GAMEPAD_MAX_CONTROLLERS; i++) {
    gamepad->gamepads[i].id = i;
    gamepad->gamepads[i].is_connected = false_v;
    gamepad->gamepads[i].type = VKR_GAMEPAD_TYPE_GENERIC;
  }

  gamepad->input_state = input_state;

  log_debug("Initializing gamepad system (macOS GameController)");

  // Ensure controller discovery begins
  [GCController startWirelessControllerDiscoveryWithCompletionHandler:^{
  }];

  return true_v;
}

bool8_t vkr_gamepad_connect(VkrGamepad *system, int32_t controller_id) {
  assert_log(system, "Gamepad system is NULL");
  assert_log(controller_id >= 0 && controller_id < VKR_GAMEPAD_MAX_CONTROLLERS,
             "Controller id is out of bounds");

  GCController *controller = vkr_get_controller_for_index(controller_id);
  if (controller) {
    if (!system->gamepads[controller_id].is_connected) {
      system->gamepads[controller_id].is_connected = true_v;
      system->gamepads[controller_id].type =
          vkr_detect_type_from_controller(controller);
      log_debug("Gamepad %d connected (%s)", controller_id,
                controller.vendorName ? controller.vendorName.UTF8String
                                      : "Unknown");
    }
    return true_v;
  }

  if (system->gamepads[controller_id].is_connected) {
    // Controller disappeared
    (void)vkr_gamepad_disconnect(system, controller_id);
  }
  return false_v;
}

bool8_t vkr_gamepad_poll_all(VkrGamepad *gamepad) {
  assert_log(gamepad, "Gamepad is NULL");
  assert_log(gamepad->input_state, "Input state is NULL");

  for (int32_t i = 0; i < VKR_GAMEPAD_MAX_CONTROLLERS; i++) {
    (void)vkr_gamepad_poll(gamepad, i);
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

  GCController *controller = vkr_get_controller_for_index(controller_id);
  if (!controller) {
    return false_v;
  }

  InputState *input_state = system->input_state;

  GCExtendedGamepad *pad = controller.extendedGamepad;
  if (!pad) {
    // Fallback: try micro gamepad with a minimal mapping
    GCMicroGamepad *micro = controller.microGamepad;
    if (!micro) {
      return false_v;
    }

    // Map micro gamepad: A -> A, X -> B; dpad available; no shoulders/triggers
    input_process_button(input_state, BUTTON_GAMEPAD_A,
                         micro.buttonA.isPressed ? true_v : false_v);
    input_process_button(input_state, BUTTON_GAMEPAD_B,
                         micro.buttonX.isPressed ? true_v : false_v);

    GCControllerDirectionPad *d = micro.dpad;
    input_process_button(input_state, BUTTON_GAMEPAD_DPAD_UP,
                         d.up.isPressed ? true_v : false_v);
    input_process_button(input_state, BUTTON_GAMEPAD_DPAD_DOWN,
                         d.down.isPressed ? true_v : false_v);
    input_process_button(input_state, BUTTON_GAMEPAD_DPAD_LEFT,
                         d.left.isPressed ? true_v : false_v);
    input_process_button(input_state, BUTTON_GAMEPAD_DPAD_RIGHT,
                         d.right.isPressed ? true_v : false_v);

    float32_t lx = d.xAxis.value; // -1..1
    float32_t ly = d.yAxis.value; // -1..1
    float32_t rx = 0.0f;
    float32_t ry = 0.0f;

    // Simple deadzone
    const float32_t dz = 0.12f;
    if (abs_f32(lx) < dz)
      lx = 0.0f;
    if (abs_f32(ly) < dz)
      ly = 0.0f;

    input_process_thumbsticks(input_state, lx, ly, rx, ry);
    return true_v;
  }

  // Buttons A/B/X/Y
  input_process_button(input_state, BUTTON_GAMEPAD_A,
                       pad.buttonA.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_B,
                       pad.buttonB.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_X,
                       pad.buttonX.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_Y,
                       pad.buttonY.isPressed ? true_v : false_v);

  // Shoulders
  input_process_button(input_state, BUTTON_GAMEPAD_LEFT_SHOULDER,
                       pad.leftShoulder.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_RIGHT_SHOULDER,
                       pad.rightShoulder.isPressed ? true_v : false_v);

  // Triggers (analog -> button via threshold or isPressed)
  input_process_button(input_state, BUTTON_GAMEPAD_LEFT_TRIGGER,
                       pad.leftTrigger.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_RIGHT_TRIGGER,
                       pad.rightTrigger.isPressed ? true_v : false_v);

  // Start/Back (menu/options) if available
  // Start/Menu mapping omitted to maintain compatibility across SDK versions
  input_process_button(input_state, BUTTON_GAMEPAD_START, false_v);

  // Back/Options mapping omitted for broad SDK compatibility; treat as not
  // pressed
  input_process_button(input_state, BUTTON_GAMEPAD_BACK, false_v);

  // D-Pad
  GCControllerDirectionPad *d = pad.dpad;
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_UP,
                       d.up.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_DOWN,
                       d.down.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_LEFT,
                       d.left.isPressed ? true_v : false_v);
  input_process_button(input_state, BUTTON_GAMEPAD_DPAD_RIGHT,
                       d.right.isPressed ? true_v : false_v);

  // Sticks
  float32_t lx = pad.leftThumbstick.xAxis.value;
  float32_t ly = pad.leftThumbstick.yAxis.value;
  float32_t rx = pad.rightThumbstick.xAxis.value;
  float32_t ry = pad.rightThumbstick.yAxis.value;

  // Simple deadzone filtering
  const float32_t dz = 0.12f;
  if (abs_f32(lx) < dz)
    lx = 0.0f;
  if (abs_f32(ly) < dz)
    ly = 0.0f;
  if (abs_f32(rx) < dz)
    rx = 0.0f;
  if (abs_f32(ry) < dz)
    ry = 0.0f;

  input_process_thumbsticks(input_state, lx, ly, rx, ry);

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
    if (gamepad->gamepads[i].is_connected) {
      (void)vkr_gamepad_disconnect(gamepad, i);
    }
  }

  // Stop wireless discovery if still running
  [GCController stopWirelessControllerDiscovery];

  log_debug("Gamepad system shutdown");

  return true_v;
}

#endif // PLATFORM_APPLE

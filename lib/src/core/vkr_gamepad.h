// clang-format off
/**
 * @file gamepad.h
 * @brief Platform-layer gamepad interface and minimal state. The platform
 * implementation (e.g., Windows/macOS) is responsible for polling native
 * controller APIs and translating them into input events via `InputState`.
 *
 * Scope: keep this simple for now — create, connect, poll buttons/triggers.
 */
// clang-format on

// todo: we should probably introduce a new struct that will hold
// all created gamepads and their state, a pointer to platform-specific
// gamepad systems, a define for how many gamepads we want to have connected
// at once, gamepad create and destroy functions and change init and shutdown
// to be a full gamepad system shutdown

#pragma once

#include "core/input.h"
#include "defines.h"
#include "math/math.h"

#define VKR_GAMEPAD_THUMB_MAX 32767.0f
#define VKR_GAMEPAD_MAX_CONTROLLERS 4

typedef enum VkrGamepadType {
  VKR_GAMEPAD_TYPE_XBOX,
  VKR_GAMEPAD_TYPE_PLAYSTATION,
  VKR_GAMEPAD_TYPE_NINTENDO,
  VKR_GAMEPAD_TYPE_GENERIC,
} GamepadType;

typedef struct VkrGamepadState {
  int32_t id;           // Controller slot/index (0-3 for XInput)
  bool8_t is_connected; // Cached connection status
  GamepadType type;     // Best-effort type (may be generic under some APIs)
} VkrGamepadState;

typedef struct VkrGamepad {
  VkrGamepadState
      gamepads[VKR_GAMEPAD_MAX_CONTROLLERS]; // State of all connected
                                             // controllers
  InputState *input_state; // Input state to dispatch events to
} VkrGamepad;

/**
 * @brief Initialize gamepad system.
 * @param gamepad The gamepad system to initialize.
 * @param input_state The input state to dispatch events to.
 * @return true if the gamepad system was initialized successfully, false
 * otherwise.
 */
bool8_t vkr_gamepad_init(VkrGamepad *gamepad, InputState *input_state);

/**
 * @brief Connect a gamepad.
 * @param system The gamepad system to connect.
 * @param controller_id The controller id to connect.
 * @return true if the gamepad was connected successfully, false otherwise.
 */
bool8_t vkr_gamepad_connect(VkrGamepad *system, int32_t controller_id);

/**
 * @brief Disconnect a gamepad.
 * @param system The gamepad system to disconnect.
 * @param controller_id The controller id to disconnect.
 * @return true if the gamepad was disconnected successfully, false otherwise.
 */
bool8_t vkr_gamepad_disconnect(VkrGamepad *system, int32_t controller_id);

/**
 * @brief Shutdown gamepad system.
 * @param gamepad The gamepad system to shutdown.
 * @return true if the gamepad system was shutdown successfully, false
 * otherwise.
 */
bool8_t vkr_gamepad_shutdown(VkrGamepad *gamepad);

/**
 * @brief Polls all gamepads.
 * @param gamepad The gamepad to poll.
 * @return true if the gamepads were polled successfully, false otherwise.
 */
bool8_t vkr_gamepad_poll_all(VkrGamepad *gamepad);

/**
 * @brief Polls a specific gamepad.
 * @param system The gamepad system to poll.
 * @param controller_id The controller id to poll.
 * @return true if the gamepad was polled successfully, false otherwise.
 */
bool8_t vkr_gamepad_poll(VkrGamepad *system, int32_t controller_id);

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

#include "core/input.h" // For InputState and Buttons
#include "defines.h"

typedef enum VkrGamepadType {
  GAMEPAD_TYPE_XBOX,
  GAMEPAD_TYPE_PLAYSTATION,
  GAMEPAD_TYPE_NINTENDO,
  GAMEPAD_TYPE_GENERIC,
} GamepadType;

typedef struct VkrGamepad {
  int32_t id;              // Controller slot/index (0-3 for XInput)
  bool8_t is_connected;    // Cached connection status
  GamepadType type;        // Best-effort type (may be generic under some APIs)
  InputState *input_state; // Input state to dispatch events to
} VkrGamepad;

/**
 * @brief Initialize a gamepad.
 * @param gamepad The gamepad to initialize.
 * @param id The ID of the gamepad.
 * @param input_state The input state to dispatch events to.
 * @return true if the gamepad was initialized successfully, false otherwise.
 */
bool8_t vkr_gamepad_init(VkrGamepad *gamepad, int32_t id,
                         InputState *input_state);

/**
 * @brief Connect a gamepad.
 * @param gamepad The gamepad to connect.
 * @return true if the gamepad was connected successfully, false otherwise.
 */
bool8_t vkr_gamepad_connect(VkrGamepad *gamepad);

/**
 * @brief Disconnect a gamepad.
 * @param gamepad The gamepad to disconnect.
 * @return true if the gamepad was disconnected successfully, false otherwise.
 */
bool8_t vkr_gamepad_disconnect(VkrGamepad *gamepad);

/**
 * @brief Shutdown a gamepad.
 * @param gamepad The gamepad to shutdown.
 * @return true if the gamepad was shutdown successfully, false otherwise.
 */
bool8_t vkr_gamepad_shutdown(VkrGamepad *gamepad);

/**
 * @brief Poll a gamepad.
 * @param gamepad The gamepad to poll.
 * @return true if the gamepad was polled successfully, false otherwise.
 */
bool8_t vkr_gamepad_poll(VkrGamepad *gamepad);

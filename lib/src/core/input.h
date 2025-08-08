// clang-format off
/**
 * @file input.h
 * @brief Defines the interface for the input management system.
 *
 * This system is responsible for tracking and processing user input from various
 * devices, primarily the keyboard and mouse. It maintains the current and
 * previous state of inputs for a given input context (e.g., a window),
 * allowing for checks like "key just pressed" or "button was released".
 * It integrates with an EventManager to dispatch events when input states change.
 *
 * Key Features:
 * - **Explicit State Management:** Input state is managed via an `InputState`
 *   structure, allowing for multiple independent input contexts (e.g., one per window).
 * - **State Tracking:** Keeps track of the current and previous states for keys
 *   and mouse buttons within a given `InputState`.
 * - **Mouse Position & Wheel:** Tracks current and previous mouse cursor
 *   positions and mouse wheel delta within a given `InputState`.
 * - **Event Dispatch:** Dispatches events (e.g., key press/release, mouse
 *   button press/release, mouse move, mouse wheel) via a provided `EventManager`
 *   when input states change.
 * - **Platform Agnostic Core:** The core logic is platform-agnostic; platform-
 *   specific code is expected to call the `input_process_*` functions with the
 *   appropriate `InputState` to feed raw input data into this system.
 *
 * Architecture:
 * 1. **InputState:** An `InputState` structure holds the current and previous
 *    states for keyboard keys, mouse buttons, mouse position, and mouse wheel,
 *    as well as a pointer to an `EventManager`. Instances of this struct are
 *    managed by the application (e.g., one per window).
 * 2. **Initialization & Shutdown:**
 *    - `input_init()`: Called to initialize an `InputState` structure. It takes
 *      an `EventManager*` and returns the initialized `InputState`. It also
 *      dispatches an `EVENT_TYPE_INPUT_SYSTEM_INIT` event.
 *    - `input_shutdown()`: Called with a pointer to an `InputState` to perform
 *      cleanup and dispatch an `EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN` event.
 * 3. **State Update:**
 *    - `input_update()`: Called once per frame (typically) for each active
 *      `InputState`. It copies the current input states to the previous input states
 *      within that `InputState`. This is crucial for detecting state transitions.
 * 4. **Input Processing:**
 *    - `input_process_key()`, `input_process_button()`, `input_process_mouse_move()`,
 *      `input_process_mouse_wheel()`: These functions are called by the platform
 *      layer (or application logic) with a pointer to the relevant `InputState`
 *      when a raw input event occurs. They update the current state within that
 *      `InputState` and dispatch corresponding events if a change occurred.
 * 5. **State Querying:**
 *    - `input_is_key_down()`, `input_is_key_up()`, `input_was_key_down()`, `input_was_key_up()`.
 *    - `input_is_button_down()`, `input_is_button_up()`, `input_was_button_down()`, `input_was_button_up()`.
 *    - `input_get_mouse_position()`, `input_get_previous_mouse_position()`.
 *    - `input_get_mouse_wheel()`.
 *    These functions take a pointer to an `InputState` and allow other systems
 *    to query its current and previous input states.
 *
 * Usage Pattern (Example: Per-Window Input):
 * 1. Application creates an `EventManager` instance.
 * 2. When a window is created, it initializes its own `InputState`:
 *    `InputState window_input_state = input_init(&my_app_event_manager);`
 * 3. In the main game loop, for each active/focused window, call `input_update()`:
 *    `input_update(&window_input_state, delta_time);`
 * 4. The platform layer (e.g., window event handling for the focused window)
 *    calls the appropriate `input_process_*` functions with `&window_input_state`
 *    when it receives raw input from the OS.
 * 5. Game logic can then use `input_is_*` or `input_get_*` functions with
 *    `&window_input_state` to check input states for that specific window,
 *    or subscribe to input events via the `EventManager`.
 * 6. When a window is destroyed, call `input_shutdown(&window_input_state)`.
 */
// clang-format on
#pragma once

#include "core/event.h"
#include "defines.h"
#include "platform/platform.h"

/**
 * @brief Defines mouse button identifiers.
 */
typedef enum Buttons {
  BUTTON_LEFT,   /**< Left mouse button. */
  BUTTON_RIGHT,  /**< Right mouse button. */
  BUTTON_MIDDLE, /**< Middle mouse button. */
  // BUTTON_X1,       // Example: Extra mouse button 1 (if supported/needed)
  // BUTTON_X2,       // Example: Extra mouse button 2 (if supported/needed)
  BUTTON_MAX_BUTTONS /**< Maximum number of mouse buttons supported. */
} Buttons;

/**
 * @brief Helper macro to define key codes in the Keys enum.
 * @param name The symbolic name of the key (e.g., A, ENTER).
 * @param code The platform-independent key code value.
 */
#define DEFINE_KEY(name, code) KEY_##name = code

/**
 * @brief Defines keyboard key identifiers. Values are typically platform-
 * independent virtual key codes.
 */
typedef enum Keys {
  DEFINE_KEY(BACKSPACE, 0x08), /**< Backspace key. */
  DEFINE_KEY(ENTER, 0x0D),     /**< Enter key. */
  DEFINE_KEY(TAB, 0x09),       /**< Tab key. */
  DEFINE_KEY(SHIFT, 0x10),     /**< Shift key (either left or right). */
  DEFINE_KEY(CONTROL, 0x11),   /**< Control key (either left or right). */

  DEFINE_KEY(PAUSE, 0x13),   /**< Pause key. */
  DEFINE_KEY(CAPITAL, 0x14), /**< Caps Lock key. */

  DEFINE_KEY(ESCAPE, 0x1B), /**< Escape key. */

  // Keys 0x1C - 0x1F are related to IME (Input Method Editor)
  DEFINE_KEY(CONVERT, 0x1C),    /**< IME Convert key. */
  DEFINE_KEY(NONCONVERT, 0x1D), /**< IME Non-convert key. */
  DEFINE_KEY(ACCEPT, 0x1E),     /**< IME Accept key. */
  DEFINE_KEY(MODECHANGE, 0x1F), /**< IME Mode change request. */

  DEFINE_KEY(SPACE, 0x20),    /**< Spacebar. */
  DEFINE_KEY(PRIOR, 0x21),    /**< Page Up key. */
  DEFINE_KEY(NEXT, 0x22),     /**< Page Down key. */
  DEFINE_KEY(END, 0x23),      /**< End key. */
  DEFINE_KEY(HOME, 0x24),     /**< Home key. */
  DEFINE_KEY(LEFT, 0x25),     /**< Left Arrow key. */
  DEFINE_KEY(UP, 0x26),       /**< Up Arrow key. */
  DEFINE_KEY(RIGHT, 0x27),    /**< Right Arrow key. */
  DEFINE_KEY(DOWN, 0x28),     /**< Down Arrow key. */
  DEFINE_KEY(SELECT, 0x29),   /**< Select key. */
  DEFINE_KEY(PRINT, 0x2A),    /**< Print key. */
  #if defined(PLATFORM_APPLE)
  DEFINE_KEY(EXECUTE, 0x2B),  /**< Execute key. */
  #endif
  DEFINE_KEY(SNAPSHOT, 0x2C), /**< Print Screen key. */
  DEFINE_KEY(INSERT, 0x2D),   /**< Insert key. */
  DEFINE_KEY(DELETE, 0x2E),   /**< Delete key. */
  DEFINE_KEY(HELP, 0x2F),     /**< Help key. */

  // ASCII 0-9 match their character codes for convenience if platform supports
  // it but these are for the top-row numbers, not numpad. DEFINE_KEY(0, 0x30),
  // Number keys (top row, not numpad)
  DEFINE_KEY(0, 0x30), /**< 0 key. */
  DEFINE_KEY(1, 0x31), /**< 1 key. */
  DEFINE_KEY(2, 0x32), /**< 2 key. */
  DEFINE_KEY(3, 0x33), /**< 3 key. */
  DEFINE_KEY(4, 0x34), /**< 4 key. */
  DEFINE_KEY(5, 0x35), /**< 5 key. */
  DEFINE_KEY(6, 0x36), /**< 6 key. */
  DEFINE_KEY(7, 0x37), /**< 7 key. */
  DEFINE_KEY(8, 0x38), /**< 8 key. */
  DEFINE_KEY(9, 0x39), /**< 9 key. */

  // ASCII A-Z match their character codes
  DEFINE_KEY(A, 0x41), /**< A key. */
  DEFINE_KEY(B, 0x42), /**< B key. */
  DEFINE_KEY(C, 0x43), /**< C key. */
  DEFINE_KEY(D, 0x44), /**< D key. */
  DEFINE_KEY(E, 0x45), /**< E key. */
  DEFINE_KEY(F, 0x46), /**< F key. */
  DEFINE_KEY(G, 0x47), /**< G key. */
  DEFINE_KEY(H, 0x48), /**< H key. */
  DEFINE_KEY(I, 0x49), /**< I key. */
  DEFINE_KEY(J, 0x4A), /**< J key. */
  DEFINE_KEY(K, 0x4B), /**< K key. */
  DEFINE_KEY(L, 0x4C), /**< L key. */
  DEFINE_KEY(M, 0x4D), /**< M key. */
  DEFINE_KEY(N, 0x4E), /**< N key. */
  DEFINE_KEY(O, 0x4F), /**< O key. */
  DEFINE_KEY(P, 0x50), /**< P key. */
  DEFINE_KEY(Q, 0x51), /**< Q key. */
  DEFINE_KEY(R, 0x52), /**< R key. */
  DEFINE_KEY(S, 0x53), /**< S key. */
  DEFINE_KEY(T, 0x54), /**< T key. */
  DEFINE_KEY(U, 0x55), /**< U key. */
  DEFINE_KEY(V, 0x56), /**< V key. */
  DEFINE_KEY(W, 0x57), /**< W key. */
  DEFINE_KEY(X, 0x58), /**< X key. */
  DEFINE_KEY(Y, 0x59), /**< Y key. */
  DEFINE_KEY(Z, 0x5A), /**< Z key. */

  DEFINE_KEY(LWIN, 0x5B), /**< Left Windows key (Microsoft Natural Keyboard). */
  DEFINE_KEY(RWIN,
             0x5C), /**< Right Windows key (Microsoft Natural Keyboard). */
  DEFINE_KEY(APPS, 0x5D), /**< Applications key (Microsoft Natural Keyboard). */

  DEFINE_KEY(SLEEP, 0x5F), /**< Computer Sleep key. */

  // Numeric keypad keys
  DEFINE_KEY(NUMPAD0, 0x60),   /**< Numeric keypad 0 key. */
  DEFINE_KEY(NUMPAD1, 0x61),   /**< Numeric keypad 1 key. */
  DEFINE_KEY(NUMPAD2, 0x62),   /**< Numeric keypad 2 key. */
  DEFINE_KEY(NUMPAD3, 0x63),   /**< Numeric keypad 3 key. */
  DEFINE_KEY(NUMPAD4, 0x64),   /**< Numeric keypad 4 key. */
  DEFINE_KEY(NUMPAD5, 0x65),   /**< Numeric keypad 5 key. */
  DEFINE_KEY(NUMPAD6, 0x66),   /**< Numeric keypad 6 key. */
  DEFINE_KEY(NUMPAD7, 0x67),   /**< Numeric keypad 7 key. */
  DEFINE_KEY(NUMPAD8, 0x68),   /**< Numeric keypad 8 key. */
  DEFINE_KEY(NUMPAD9, 0x69),   /**< Numeric keypad 9 key. */
  DEFINE_KEY(MULTIPLY, 0x6A),  /**< Multiply key (* on numpad). */
  DEFINE_KEY(ADD, 0x6B),       /**< Add key (+ on numpad). */
  DEFINE_KEY(SEPARATOR, 0x6C), /**< Separator key (often locale-specific, e.g.,
                                  Numpad Decimal or Enter). */
  DEFINE_KEY(SUBTRACT, 0x6D),  /**< Subtract key (- on numpad). */
  DEFINE_KEY(DECIMAL, 0x6E),   /**< Decimal key (. on numpad). */
  DEFINE_KEY(DIVIDE, 0x6F),    /**< Divide key (/ on numpad). */

  // Function keys
  DEFINE_KEY(F1, 0x70),  /**< F1 key. */
  DEFINE_KEY(F2, 0x71),  /**< F2 key. */
  DEFINE_KEY(F3, 0x72),  /**< F3 key. */
  DEFINE_KEY(F4, 0x73),  /**< F4 key. */
  DEFINE_KEY(F5, 0x74),  /**< F5 key. */
  DEFINE_KEY(F6, 0x75),  /**< F6 key. */
  DEFINE_KEY(F7, 0x76),  /**< F7 key. */
  DEFINE_KEY(F8, 0x77),  /**< F8 key. */
  DEFINE_KEY(F9, 0x78),  /**< F9 key. */
  DEFINE_KEY(F10, 0x79), /**< F10 key. */
  DEFINE_KEY(F11, 0x7A), /**< F11 key. */
  DEFINE_KEY(F12, 0x7B), /**< F12 key. */
  DEFINE_KEY(F13, 0x7C), /**< F13 key. */
  DEFINE_KEY(F14, 0x7D), /**< F14 key. */
  DEFINE_KEY(F15, 0x7E), /**< F15 key. */
  DEFINE_KEY(F16, 0x7F), /**< F16 key. */
  DEFINE_KEY(F17, 0x80), /**< F17 key. */
  DEFINE_KEY(F18, 0x81), /**< F18 key. */
  DEFINE_KEY(F19, 0x82), /**< F19 key. */
  DEFINE_KEY(F20, 0x83), /**< F20 key. */
  DEFINE_KEY(F21, 0x84), /**< F21 key. */
  DEFINE_KEY(F22, 0x85), /**< F22 key. */
  DEFINE_KEY(F23, 0x86), /**< F23 key. */
  DEFINE_KEY(F24, 0x87), /**< F24 key. */

  DEFINE_KEY(NUMLOCK, 0x90), /**< Num Lock key. */
  DEFINE_KEY(SCROLL, 0x91),  /**< Scroll Lock key. */

  DEFINE_KEY(NUMPAD_EQUAL, 0x92), /**< Numpad '=' key. */

  // Modifier keys (specific locations)
  DEFINE_KEY(LSHIFT, 0xA0),   /**< Left Shift key. */
  DEFINE_KEY(RSHIFT, 0xA1),   /**< Right Shift key. */
  DEFINE_KEY(LCONTROL, 0xA2), /**< Left Control key. */
  DEFINE_KEY(RCONTROL, 0xA3), /**< Right Control key. */
  DEFINE_KEY(LMENU, 0xA4),    /**< Left Alt (Menu) key. */
  DEFINE_KEY(RMENU, 0xA5),    /**< Right Alt (Menu) key. */

  // Punctuation keys (US standard layout examples)
  DEFINE_KEY(SEMICOLON,
             0xBA),         /**< Semicolon key (OEM_1 typically ';:' for US). */
  DEFINE_KEY(PLUS, 0xBB),   /**< Plus key ('=', '+ ' for US). */
  DEFINE_KEY(COMMA, 0xBC),  /**< Comma key (',' for US). */
  DEFINE_KEY(MINUS, 0xBD),  /**< Minus key ('-', '_' for US). */
  DEFINE_KEY(PERIOD, 0xBE), /**< Period key ('.' for US). */
  DEFINE_KEY(SLASH, 0xBF),  /**< Slash key (OEM_2 typically '/?' for US). */
  DEFINE_KEY(GRAVE,
             0xC0), /**< Grave accent key (OEM_3 typically '`~' for US). */

  KEY_MAX_KEYS /**< Maximum number of keys supported. Sentinel value. */
} Keys;

/**
 * @brief Data associated with a key press or release event.
 * Dispatched as part of an `Event` when `input_process_key` detects a change.
 */
typedef struct KeyEventData {
  Keys key;        /**< The key that was pressed or released. */
  bool8_t pressed; /**< `true` if the key was pressed, `false` if released. */
} KeyEventData;

/**
 * @brief Data associated with a mouse button press or release event.
 * Dispatched as part of an `Event` when `input_process_button` detects a
 * change.
 */
typedef struct ButtonEventData {
  Buttons button; /**< The mouse button that was pressed or released. */
  bool8_t
      pressed; /**< `true` if the button was pressed, `false` if released. */
} ButtonEventData;

/**
 * @brief Data associated with a mouse movement event.
 * Dispatched as part of an `Event` when `input_process_mouse_move` detects a
 * change.
 */
typedef struct MouseMoveEventData {
  int32_t x; /**< The new X-coordinate of the mouse cursor. */
  int32_t y; /**< The new Y-coordinate of the mouse cursor. */
} MouseMoveEventData;

/**
 * @brief Data associated with a mouse wheel scroll event.
 * Dispatched as part of an `Event` when `input_process_mouse_wheel` detects a
 * change.
 */
typedef struct MouseWheelEventData {
  int8_t delta; /**< The amount the mouse wheel was scrolled. Positive for
                   scroll up/forward, negative for scroll down/backward. */
} MouseWheelEventData;

typedef struct KeysState {
  bool8_t keys[KEY_MAX_KEYS];
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
  bool32_t is_initialized;
} InputState;

/**
 * @brief Initializes the input system.
 * Stores the provided event manager for dispatching input events and dispatches
 * an `EVENT_TYPE_INPUT_SYSTEM_INIT` event.
 * @param event_manager Pointer to an initialized `EventManager` instance.
 * @return An `InputState` structure initialized with the event manager and
 * default input states.
 */
InputState input_init(EventManager *event_manager);

/**
 * @brief Shuts down the input system.
 * Dispatches an `EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN` event.
 * @param input_state Pointer to the `InputState` to shut down.
 */
void input_shutdown(InputState *input_state);

/**
 * @brief Updates the input system's state.
 * This function should be called once per frame, typically before any game
 * logic that depends on input. It copies the current input states (keys,
 * buttons, mouse position) to the previous state buffers, allowing for
 * detection of just-pressed/just-released states.
 * @param input_state Pointer to the `InputState` to update.
 */
void input_update(InputState *input_state);

/**
 * @brief Checks if a specific keyboard key is currently held down.
 * @param input_state Pointer to the `InputState` to query.
 * @param key The `Keys` identifier of the key to check.
 * @return `true` if the key is currently down, `false` otherwise.
 */
bool8_t input_is_key_down(InputState *input_state, Keys key);

/**
 * @brief Checks if a specific keyboard key is currently up (not pressed).
 * @param input_state Pointer to the `InputState` to query.
 * @param key The `Keys` identifier of the key to check.
 * @return `true` if the key is currently up, `false` otherwise.
 */
bool8_t input_is_key_up(InputState *input_state, Keys key);

/**
 * @brief Checks if a specific keyboard key was held down in the previous frame.
 * Requires `input_update()` to have been called to update previous states.
 * @param input_state Pointer to the `InputState` to query.
 * @param key The `Keys` identifier of the key to check.
 * @return `true` if the key was down in the previous frame, `false` otherwise.
 */
bool8_t input_was_key_down(InputState *input_state, Keys key);

/**
 * @brief Checks if a specific keyboard key was up (not pressed) in the previous
 * frame. Requires `input_update()` to have been called to update previous
 * states.
 * @param input_state Pointer to the `InputState` to query.
 * @param key The `Keys` identifier of the key to check.
 * @return `true` if the key was up in the previous frame, `false` otherwise.
 */
bool8_t input_was_key_up(InputState *input_state, Keys key);

/**
 * @brief Processes a keyboard key event.
 * Called by the platform layer when a key is pressed or released.
 * Updates the internal current key state and dispatches a
 * `EVENT_TYPE_KEY_PRESS` or `EVENT_TYPE_KEY_RELEASE` event via the
 * `EventManager` if the state changed.
 * @param input_state Pointer to the `InputState` to modify.
 * @param key The `Keys` identifier of the key involved in the event.
 * @param pressed `true` if the key was pressed, `false` if it was released.
 */
void input_process_key(InputState *input_state, Keys key, bool8_t pressed);

/**
 * @brief Checks if a specific mouse button is currently held down.
 * @param input_state Pointer to the `InputState` to query.
 * @param button The `Buttons` identifier of the mouse button to check.
 * @return `true` if the button is currently down, `false` otherwise.
 */
bool8_t input_is_button_down(InputState *input_state, Buttons button);

/**
 * @brief Checks if a specific mouse button is currently up (not pressed).
 * @param input_state Pointer to the `InputState` to query.
 * @param button The `Buttons` identifier of the mouse button to check.
 * @return `true` if the button is currently up, `false` otherwise.
 */
bool8_t input_is_button_up(InputState *input_state, Buttons button);

/**
 * @brief Checks if a specific mouse button was held down in the previous frame.
 * Requires `input_update()` to have been called to update previous states.
 * @param input_state Pointer to the `InputState` to query.
 * @param button The `Buttons` identifier of the mouse button to check.
 * @return `true` if the button was down in the previous frame, `false`
 * otherwise.
 */
bool8_t input_was_button_down(InputState *input_state, Buttons button);

/**
 * @brief Checks if a specific mouse button was up (not pressed) in the previous
 * frame. Requires `input_update()` to have been called to update previous
 * states.
 * @param input_state Pointer to the `InputState` to query.
 * @param button The `Buttons` identifier of the mouse button to check.
 * @return `true` if the button was up in the previous frame, `false` otherwise.
 */
bool8_t input_was_button_up(InputState *input_state, Buttons button);

/**
 * @brief Retrieves the current mouse cursor position.
 * @param input_state Pointer to the `InputState` to query.
 * @param[out] x Pointer to store the current X-coordinate of the mouse.
 * @param[out] y Pointer to store the current Y-coordinate of the mouse.
 */
void input_get_mouse_position(InputState *input_state, int32_t *x, int32_t *y);

/**
 * @brief Retrieves the mouse cursor position from the previous frame.
 * Requires `input_update()` to have been called to update previous states.
 * @param input_state Pointer to the `InputState` to query.
 * @param[out] x Pointer to store the previous X-coordinate of the mouse.
 * @param[out] y Pointer to store the previous Y-coordinate of the mouse.
 */
void input_get_previous_mouse_position(InputState *input_state, int32_t *x,
                                       int32_t *y);

/**
 * @brief Retrieves the last processed mouse wheel delta.
 * Note: The concept of a "previous" mouse wheel delta is not explicitly stored
 * across frames in the same way as position or button states. This function
 * returns the most recent delta processed by `input_process_mouse_wheel`.
 * @param input_state Pointer to the `InputState` to query.
 * @param[out] delta Pointer to store the last mouse wheel delta. Positive for
 * up/forward, negative for down/backward.
 */
void input_get_mouse_wheel(InputState *input_state, int8_t *delta);

/**
 * @brief Processes a mouse button event.
 * Called by the platform layer when a mouse button is pressed or released.
 * Updates the internal current button state and dispatches a
 * `EVENT_TYPE_BUTTON_PRESS` or `EVENT_TYPE_BUTTON_RELEASE` event via the
 * `EventManager` if the state changed.
 * @param input_state Pointer to the `InputState` to modify.
 * @param button The `Buttons` identifier of the mouse button involved.
 * @param pressed `true` if the button was pressed, `false` if it was released.
 */
void input_process_button(InputState *input_state, Buttons button,
                          bool8_t pressed);

/**
 * @brief Processes a mouse movement event.
 * Called by the platform layer when the mouse cursor moves.
 * Updates the internal current mouse position and dispatches an
 * `EVENT_TYPE_MOUSE_MOVE` event via the `EventManager` if the position changed.
 * @param input_state Pointer to the `InputState` to modify.
 * @param x The new X-coordinate of the mouse cursor.
 * @param y The new Y-coordinate of the mouse cursor.
 */
void input_process_mouse_move(InputState *input_state, int32_t x, int32_t y);

/**
 * @brief Processes a mouse wheel scroll event.
 * Called by the platform layer when the mouse wheel is scrolled.
 * Updates the internal mouse wheel state and dispatches an
 * `EVENT_TYPE_MOUSE_WHEEL` event via the `EventManager` if the delta is
 * non-zero or changed from the previous non-zero delta.
 * @param input_state Pointer to the `InputState` to modify.
 * @param delta The amount the wheel was scrolled. Positive for up/forward,
 * negative for down/backward.
 */
void input_process_mouse_wheel(InputState *input_state, int8_t delta);

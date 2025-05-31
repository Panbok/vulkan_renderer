// clang-format off
/**
 * @file window.h
 * @brief Defines the interface for the platform-agnostic windowing system.
 *
 * This system provides a way to create, manage, and interact with a native
 * window on the target platform. It integrates with the EventManager and InputState
 * systems to provide a cohesive way of handling window events and user input.
 *
 * Key Features:
 * - **Platform Abstraction:** Provides a common C API (this file) for windowing
 *   operations, with platform-specific implementations (e.g., window_macos.m).
 * - **Event Integration:** Uses an EventManager to dispatch window-related events
 *   like resize or close requests.
 * - **Input Management:** Each window owns and manages its own InputState, which
 *   is populated by the platform-specific implementation based on native input events.
 * - **Lifecycle Management:** Supports window creation, destruction, and a per-frame
 *   update mechanism for processing window messages.
 *
 * Architecture:
 * 1. **Window Struct:** The primary `Window` struct holds common window properties
 *    (dimensions, title), references to the event and input systems, and an
 *    opaque `platform_state` pointer. This `platform_state` points to a
 *    platform-specific structure containing native window handles and other
 *    implementation details.
 * 2. **window_create():** Initializes a `Window` struct and its associated
 *    platform-specific resources. It sets up the native window, prepares it for
 *    rendering, and initializes the window's `InputState`.
 * 3. **window_destroy():** Cleans up all resources associated with a `Window`,
 *    including the native window and the platform-specific state.
 * 4. **window_update():** Called once per frame. The platform-specific
 *    implementation processes pending native window events (like messages from the OS),
 *    updates input state, and dispatches engine events. It returns a boolean
 *    indicating if the window (and typically the application) should continue running.
 */
// clang-format on
#pragma once

#include "event.h"
#include "input.h"
#include "pch.h"
#include "platform.h"

/**
 * @brief Represents a platform window and its associated state.
 * The `platform_state` member is an opaque pointer to internal,
 * platform-specific data (e.g., native window handles, delegates on macOS). The
 * window owns its `InputState`.
 */
typedef struct Window {
  void *platform_state;        /**< Opaque pointer to platform-specific window
                                  state. Managed internally. */
  EventManager *event_manager; /**< Pointer to the global EventManager. Not
                                  owned by the window. */
  InputState input_state; /**< Input state specific to this window. Owned and
                             managed by the window. */
  char *title; /**< The window title. Lifetime of the string must be managed by
                  the caller or be static/long-lived. */
  int32_t x;   /**< Initial x-coordinate of the window's top-left corner. */
  int32_t y;   /**< Initial y-coordinate of the window's top-left corner. */
  uint32_t width;  /**< Initial width of the window's client area. */
  uint32_t height; /**< Initial height of the window's client area. */
} Window;

typedef struct WindowPixelSize {
  uint32_t width;
  uint32_t height;
} WindowPixelSize;

/**
 * @brief Data for window resize event (EVENT_TYPE_WINDOW_RESIZE).
 * Dispatched by the platform layer when the window's client area (or
 * framebuffer) size changes.
 */
typedef struct WindowResizeEventData {
  uint32_t width; /**< The new width of the window's client/framebuffer area. */
  uint32_t
      height; /**< The new height of the window's client/framebuffer area. */
} WindowResizeEventData;

/**
 * @brief Creates and initializes a new platform window.
 * This function sets up the native window, its associated platform-specific
 * state, and initializes the `window->input_state` using the provided
 * `event_manager`. The caller is responsible for ensuring the `title` string
 * remains valid for the lifetime of the window, or until the platform-specific
 * implementation copies it.
 *
 * @param window Pointer to a `Window` struct to be initialized. Must not be
 * NULL. Its `platform_state` should be NULL or the struct zero-initialized.
 * @param event_manager Pointer to an initialized `EventManager` instance for
 * event dispatch. Must not be NULL.
 * @param title The desired title for the window. Must not be NULL.
 * @param x The initial x-coordinate for the window.
 * @param y The initial y-coordinate for the window.
 * @param width The initial width of the window's client area.
 * @param height The initial height of the window's client area.
 * @return `true_v` if the window was created successfully, `false_v` otherwise.
 *         On failure, `window->platform_state` will be NULL and an error
 * logged.
 */
bool8_t window_create(Window *window, EventManager *event_manager,
                      const char *title, int32_t x, int32_t y, uint32_t width,
                      uint32_t height);

/**
 * @brief Destroys a window and releases all associated platform resources.
 * This includes closing the native window, cleaning up platform-specific state,
 * and shutting down the window's input state.
 *
 * @param window Pointer to the `Window` struct to be destroyed. If `window` or
 *               `window->platform_state` is NULL, the function does nothing or
 * asserts.
 */
void window_destroy(Window *window);

/**
 * @brief Processes pending window events and updates the window state.
 * This function should be called once per frame (e.g., in the main application
 * loop). The platform-specific implementation will typically process OS
 * messages, handle input events (populating `window->input_state`), and
 * dispatch engine events (like resize or close requests) via the
 * `EventManager`.
 *
 * @param window Pointer to the `Window` to be updated. Must not be NULL and
 * must have been successfully created.
 * @return `true_v` if the window is still active and the application should
 * continue running. `false_v` if a close request has been processed (e.g., user
 * clicked the close button) and the application should terminate.
 */
bool8_t window_update(Window *window);

WindowPixelSize window_get_pixel_size(Window *window);

#if defined(PLATFORM_APPLE)
/**
 * @brief Gets the Metal layer from the window for Vulkan surface creation.
 * This function provides access to the CAMetalLayer needed for creating
 * a Vulkan surface on macOS.
 *
 * @param window Pointer to the `Window` to get the Metal layer from. Must not
 * be NULL.
 * @return Pointer to the CAMetalLayer, or NULL if not available.
 */
void *window_get_metal_layer(Window *window);
#endif

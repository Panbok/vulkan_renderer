#include "window.h"

#if defined(PLATFORM_WINDOWS)

typedef struct PlatformState {
  HINSTANCE instance;
  HWND window;
  bool8_t quit_flagged;
  EventManager *event_manager;
  InputState *input_state;

  // Mouse capture state
  bool8_t cursor_hidden;
  bool8_t mouse_captured;
  float64_t restore_cursor_x;
  float64_t restore_cursor_y;
  float64_t cursor_warp_delta_x;
  float64_t cursor_warp_delta_y;

  // Track last physical cursor position for re-centering
  int32_t last_cursor_pos_x;
  int32_t last_cursor_pos_y;

  // Track mouse movement for delta calculation
  bool8_t first_mouse_move;
  int32_t mouse_last_x;
  int32_t mouse_last_y;

  // Window state
  uint32_t window_width;
  uint32_t window_height;
} PlatformState;

// Forward declarations
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam);
static Keys translate_keycode(uint32_t vk_keycode);
static void hide_cursor(PlatformState *state);
static void show_cursor(PlatformState *state);
static void update_cursor_image(PlatformState *state);
static void center_cursor_in_window(PlatformState *state);
static bool8_t cursor_in_content_area(PlatformState *state);

bool8_t window_create(Window *window, EventManager *event_manager,
                      const char *title, int32_t x, int32_t y, uint32_t width,
                      uint32_t height) {
  assert_log(event_manager != NULL, "Event manager not initialized");
  assert_log(title != NULL, "Title not initialized");
  assert_log(x >= 0, "X position not initialized");
  assert_log(y >= 0, "Y position not initialized");
  assert_log(width > 0, "Width not initialized");
  assert_log(height > 0, "Height not initialized");

  window->title = (char *)title;
  window->x = x;
  window->y = y;
  window->width = width;
  window->height = height;
  window->event_manager = event_manager;
  window->input_state = input_init(event_manager);

  PlatformState *state = (PlatformState *)malloc(sizeof(PlatformState));
  if (!state) {
    log_error("Failed to allocate PlatformState");
    window->platform_state = NULL;
    return false_v;
  }

  // Initialize state
  state->instance = GetModuleHandle(NULL);
  state->window = NULL;
  state->quit_flagged = false_v;
  state->event_manager = event_manager;
  state->input_state = &window->input_state;
  state->window_width = width;
  state->window_height = height;

  // Initialize mouse capture state
  state->cursor_hidden = false_v;
  state->mouse_captured = false_v;
  state->restore_cursor_x = 0.0;
  state->restore_cursor_y = 0.0;
  state->cursor_warp_delta_x = 0.0;
  state->cursor_warp_delta_y = 0.0;
  state->last_cursor_pos_x = 0;
  state->last_cursor_pos_y = 0;
  state->first_mouse_move = true_v;
  state->mouse_last_x = 0;
  state->mouse_last_y = 0;

  window->platform_state = state;

  // Register window class
  const char *class_name = "VulkanRendererWindowClass";
  WNDCLASSEX wc = {0};
  MemZero(&wc, sizeof(WNDCLASSEX));
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = state->instance;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = class_name;
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  if (!RegisterClassEx(&wc)) {
    log_error("Failed to register window class");
    MessageBoxA(NULL, "Failed to register window class", "Error",
                MB_ICONEXCLAMATION | MB_OK);
    free(state);
    window->platform_state = NULL;
    return false_v;
  }

  // Calculate window size including decorations
  RECT window_rect = {0, 0, (LONG)width, (LONG)height};
  DWORD window_style = WS_OVERLAPPEDWINDOW;
  DWORD window_ex_style = WS_EX_APPWINDOW;

  if (!AdjustWindowRectEx(&window_rect, window_style, FALSE, window_ex_style)) {
    log_error("Failed to adjust window rect");
    MessageBoxA(NULL, "Failed to adjust window rect", "Error",
                MB_ICONEXCLAMATION | MB_OK);
    free(state);
    window->platform_state = NULL;
    return false_v;
  }

  int32_t window_width = window_rect.right - window_rect.left;
  int32_t window_height = window_rect.bottom - window_rect.top;

  // Create window
  state->window = CreateWindowExA(
      window_ex_style, class_name, title, window_style, x, y, window_width,
      window_height, NULL, NULL, state->instance, state);

  if (!state->window) {
    log_error("Failed to create window");
    MessageBoxA(NULL, "Failed to create window", "Error",
                MB_ICONEXCLAMATION | MB_OK);
    free(state);
    window->platform_state = NULL;
    return false_v;
  }

  // Show and update window
  ShowWindow(state->window, SW_SHOW);
  UpdateWindow(state->window);

  // Dispatch window init event
  event_manager_dispatch(event_manager,
                         (Event){.type = EVENT_TYPE_WINDOW_INIT});

  log_info("Window created successfully");
  return true_v;
}

void window_destroy(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  if (state->window) {
    DestroyWindow(state->window);
    state->window = NULL;
  }

  input_shutdown(state->input_state);
  free(state);
  window->platform_state = NULL;
}

bool8_t window_update(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  if (state->quit_flagged) {
    return false_v;
  }

  MSG msg;
  while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  // Re-center cursor if in capture mode and it has moved since the last call
  // This prevents the cursor from hitting window boundaries and stopping
  // movement
  if (state->mouse_captured) {
    RECT client_rect;
    GetClientRect(state->window, &client_rect);
    int32_t center_x = (client_rect.right - client_rect.left) / 2;
    int32_t center_y = (client_rect.bottom - client_rect.top) / 2;

    // Only re-center if cursor has moved away from center to avoid breaking
    // mouse events
    if (state->last_cursor_pos_x != center_x ||
        state->last_cursor_pos_y != center_y) {
      // Convert client coordinates to screen coordinates and re-center
      POINT center_pos = {center_x, center_y};
      ClientToScreen(state->window, &center_pos);
      SetCursorPos(center_pos.x, center_pos.y);

      // Update warp deltas to account for the cursor repositioning
      state->cursor_warp_delta_x +=
          (float64_t)(center_x - state->last_cursor_pos_x);
      state->cursor_warp_delta_y +=
          (float64_t)(center_y - state->last_cursor_pos_y);

      // Update tracking to prevent immediate re-centering
      state->last_cursor_pos_x = center_x;
      state->last_cursor_pos_y = center_y;
      state->mouse_last_x = center_x;
      state->mouse_last_y = center_y;
    }
  }

  return !state->quit_flagged;
}

WindowPixelSize window_get_pixel_size(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  RECT client_rect;
  GetClientRect(state->window, &client_rect);

  return (WindowPixelSize){
      .width = (uint32_t)(client_rect.right - client_rect.left),
      .height = (uint32_t)(client_rect.bottom - client_rect.top),
  };
}

void *window_get_win32_handle(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;
  return state->window;
}

void *window_get_win32_instance(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;
  return state->instance;
}

void window_set_mouse_capture(Window *window, bool8_t capture) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  if (capture) {
    state->mouse_captured = true_v;

    // Get current cursor position
    POINT cursor_pos;
    GetCursorPos(&cursor_pos);
    ScreenToClient(state->window, &cursor_pos);

    // Store restore coordinates in client coordinate system
    state->restore_cursor_x = (float64_t)cursor_pos.x;
    state->restore_cursor_y = (float64_t)cursor_pos.y;

    // Initialize virtual cursor position to match current physical position
    input_process_mouse_move(state->input_state, cursor_pos.x, cursor_pos.y);

    log_debug(
        "Initialized virtual cursor to: (%d, %d) from physical: (%.1f, %.1f)",
        cursor_pos.x, cursor_pos.y, state->restore_cursor_x,
        state->restore_cursor_y);

    // Capture mouse but don't clip cursor - we'll re-center it instead
    SetCapture(state->window);

    // Center cursor in window initially
    RECT client_rect;
    GetClientRect(state->window, &client_rect);
    int32_t center_x = (client_rect.right - client_rect.left) / 2;
    int32_t center_y = (client_rect.bottom - client_rect.top) / 2;

    POINT center_pos = {center_x, center_y};
    ClientToScreen(state->window, &center_pos);
    SetCursorPos(center_pos.x, center_pos.y);

    // Set initial tracking position
    state->last_cursor_pos_x = center_x;
    state->last_cursor_pos_y = center_y;

    // Reset mouse movement tracking for new capture session
    state->first_mouse_move = true_v;
    state->cursor_warp_delta_x = 0.0;
    state->cursor_warp_delta_y = 0.0;

    update_cursor_image(state);
  } else {
    state->mouse_captured = false_v;

    // Release capture
    ReleaseCapture();

    // Restore cursor position
    POINT restore_pos = {(LONG)state->restore_cursor_x,
                         (LONG)state->restore_cursor_y};
    ClientToScreen(state->window, &restore_pos);
    SetCursorPos(restore_pos.x, restore_pos.y);

    log_debug("Restored cursor to client coords: (%.1f, %.1f)",
              state->restore_cursor_x, state->restore_cursor_y);

    update_cursor_image(state);
  }
}

bool8_t window_is_mouse_captured(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;
  return state->mouse_captured;
}

void window_set_mouse_position(Window *window, int32_t x, int32_t y) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  // Get current cursor position for delta calculation
  POINT current_pos;
  GetCursorPos(&current_pos);
  ScreenToClient(state->window, &current_pos);

  // Calculate warp deltas to smooth out movement
  state->cursor_warp_delta_x += (float64_t)(x - current_pos.x);
  state->cursor_warp_delta_y += (float64_t)(y - current_pos.y);

  // Convert client coordinates to screen coordinates and set position
  POINT new_pos = {x, y};
  ClientToScreen(state->window, &new_pos);
  SetCursorPos(new_pos.x, new_pos.y);

  update_cursor_image(state);
}

// Window procedure
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {

  if (msg == WM_NCCREATE) {
    CREATESTRUCT *cs = (CREATESTRUCT *)lparam;
    PlatformState *p = (PlatformState *)cs->lpCreateParams;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)p);
    return TRUE;
  }

  PlatformState *state = (PlatformState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

  if (!state) {
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }

  switch (msg) {
  case WM_CLOSE: {
    state->quit_flagged = true_v;
    Event event = {.type = EVENT_TYPE_WINDOW_CLOSE};
    event_manager_dispatch(state->event_manager, event);
    return FALSE;
  }

  case WM_SIZE: {
    uint32_t new_width = LOWORD(lparam);
    uint32_t new_height = HIWORD(lparam);

    state->window_width = new_width;
    state->window_height = new_height;

    WindowResizeEventData resize_data = {.width = new_width,
                                         .height = new_height};
    Event event = {.type = EVENT_TYPE_WINDOW_RESIZE,
                   .data = &resize_data,
                   .data_size = sizeof(WindowResizeEventData)};
    event_manager_dispatch(state->event_manager, event);

    // Re-center cursor if in capture mode after window resize
    if (state->mouse_captured) {
      center_cursor_in_window(state);
    }
    return FALSE;
  }

  case WM_KEYDOWN:
  case WM_SYSKEYDOWN: {
    Keys key = translate_keycode((uint32_t)wparam);
    if (key != KEY_MAX_KEYS) {
      input_process_key(state->input_state, key, true_v);
    }
    return FALSE;
  }

  case WM_KEYUP:
  case WM_SYSKEYUP: {
    Keys key = translate_keycode((uint32_t)wparam);
    if (key != KEY_MAX_KEYS) {
      input_process_key(state->input_state, key, false_v);
    }
    return FALSE;
  }

  case WM_LBUTTONDOWN: {
    input_process_button(state->input_state, BUTTON_LEFT, true_v);
    return FALSE;
  }

  case WM_LBUTTONUP: {
    input_process_button(state->input_state, BUTTON_LEFT, false_v);
    return FALSE;
  }

  case WM_RBUTTONDOWN: {
    input_process_button(state->input_state, BUTTON_RIGHT, true_v);
    return FALSE;
  }

  case WM_RBUTTONUP: {
    input_process_button(state->input_state, BUTTON_RIGHT, false_v);
    return FALSE;
  }

  case WM_MBUTTONDOWN: {
    input_process_button(state->input_state, BUTTON_MIDDLE, true_v);
    return FALSE;
  }

  case WM_MBUTTONUP: {
    input_process_button(state->input_state, BUTTON_MIDDLE, false_v);
    return FALSE;
  }

  case WM_MOUSEMOVE: {
    int32_t x = GET_X_LPARAM(lparam);
    int32_t y = GET_Y_LPARAM(lparam);

    if (state->mouse_captured) {
      // In capture mode, use delta movement for virtual cursor
      if (state->first_mouse_move) {
        state->mouse_last_x = x;
        state->mouse_last_y = y;
        state->first_mouse_move = false_v;
      }

      float64_t dx =
          (float64_t)(x - state->mouse_last_x) - state->cursor_warp_delta_x;
      float64_t dy =
          (float64_t)(y - state->mouse_last_y) - state->cursor_warp_delta_y;

      // Get current virtual cursor position from input state
      int32_t current_x, current_y;
      input_get_mouse_position(state->input_state, &current_x, &current_y);

      // Update virtual position with delta
      int32_t new_x = current_x + (int32_t)dx;
      int32_t new_y = current_y - (int32_t)dy; // Invert Y axis

      input_process_mouse_move(state->input_state, new_x, new_y);

      state->mouse_last_x = x;
      state->mouse_last_y = y;

      // Track physical cursor position for re-centering logic
      state->last_cursor_pos_x = x;
      state->last_cursor_pos_y = y;

#ifndef NDEBUG
      static int log_counter = 0;
      if (++log_counter % 60 == 0) { // Log every 60 mouse moves to avoid spam
        log_debug("Virtual cursor: (%d, %d)", new_x, new_y);
      }
#endif
    } else {
      // Normal mode, use absolute position
      input_process_mouse_move(state->input_state, x, y);

#ifndef NDEBUG
      static int normal_mode_log_counter = 0;
      if (++normal_mode_log_counter % 60 ==
          0) { // Log every 60 mouse moves to avoid spam
        log_debug("Normal mode cursor: (%d, %d)", x, y);
      }
#endif
    }

    // Reset warp deltas
    state->cursor_warp_delta_x = 0.0;
    state->cursor_warp_delta_y = 0.0;
    return FALSE;
  }

  case WM_MOUSEWHEEL: {
    int16_t delta = GET_WHEEL_DELTA_WPARAM(wparam);
    int8_t wheel_delta = (int8_t)(delta / WHEEL_DELTA);
    input_process_mouse_wheel(state->input_state, wheel_delta);
    return FALSE;
  }

  case WM_ACTIVATE: {
    if (LOWORD(wparam) != WA_INACTIVE) {
      // Window gained focus
      if (state->mouse_captured) {
        center_cursor_in_window(state);
      }
    } else {
      // Window lost focus
      if (state->mouse_captured) {
        show_cursor(state);
      }
    }
    update_cursor_image(state);
    return FALSE;
  }

  default:
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }
}

// Helper functions
static void hide_cursor(PlatformState *state) {
  // if (!state->cursor_hidden) {
  //   ShowCursor(FALSE);
  //   state->cursor_hidden = true_v;
  // }
  while (ShowCursor(FALSE) >= 0)
    ;
  state->cursor_hidden = true_v;
}

static void show_cursor(PlatformState *state) {
  // if (state->cursor_hidden) {
  //   ShowCursor(TRUE);
  //   state->cursor_hidden = false_v;
  // }
  while (ShowCursor(TRUE) < 0)
    ;
  state->cursor_hidden = false_v;
}

static void update_cursor_image(PlatformState *state) {
  if (state->mouse_captured) {
    hide_cursor(state);
  } else {
    show_cursor(state);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
  }
}

static void center_cursor_in_window(PlatformState *state) {
  RECT client_rect;
  GetClientRect(state->window, &client_rect);

  POINT center = {(client_rect.right - client_rect.left) / 2,
                  (client_rect.bottom - client_rect.top) / 2};

  // Store client coordinates for tracking
  int32_t client_center_x = center.x;
  int32_t client_center_y = center.y;

  ClientToScreen(state->window, &center);
  SetCursorPos(center.x, center.y);

  // Update tracking position (use client coordinates)
  state->last_cursor_pos_x = client_center_x;
  state->last_cursor_pos_y = client_center_y;
}

static bool8_t cursor_in_content_area(PlatformState *state) {
  POINT cursor_pos;
  GetCursorPos(&cursor_pos);
  ScreenToClient(state->window, &cursor_pos);

  RECT client_rect;
  GetClientRect(state->window, &client_rect);

  return (cursor_pos.x >= client_rect.left &&
          cursor_pos.x < client_rect.right && cursor_pos.y >= client_rect.top &&
          cursor_pos.y < client_rect.bottom);
}

static Keys translate_keycode(uint32_t vk_keycode) {
  switch (vk_keycode) {
  case VK_NUMPAD0:
    return KEY_NUMPAD0;
  case VK_NUMPAD1:
    return KEY_NUMPAD1;
  case VK_NUMPAD2:
    return KEY_NUMPAD2;
  case VK_NUMPAD3:
    return KEY_NUMPAD3;
  case VK_NUMPAD4:
    return KEY_NUMPAD4;
  case VK_NUMPAD5:
    return KEY_NUMPAD5;
  case VK_NUMPAD6:
    return KEY_NUMPAD6;
  case VK_NUMPAD7:
    return KEY_NUMPAD7;
  case VK_NUMPAD8:
    return KEY_NUMPAD8;
  case VK_NUMPAD9:
    return KEY_NUMPAD9;

  case 'A':
    return KEY_A;
  case 'B':
    return KEY_B;
  case 'C':
    return KEY_C;
  case 'D':
    return KEY_D;
  case 'E':
    return KEY_E;
  case 'F':
    return KEY_F;
  case 'G':
    return KEY_G;
  case 'H':
    return KEY_H;
  case 'I':
    return KEY_I;
  case 'J':
    return KEY_J;
  case 'K':
    return KEY_K;
  case 'L':
    return KEY_L;
  case 'M':
    return KEY_M;
  case 'N':
    return KEY_N;
  case 'O':
    return KEY_O;
  case 'P':
    return KEY_P;
  case 'Q':
    return KEY_Q;
  case 'R':
    return KEY_R;
  case 'S':
    return KEY_S;
  case 'T':
    return KEY_T;
  case 'U':
    return KEY_U;
  case 'V':
    return KEY_V;
  case 'W':
    return KEY_W;
  case 'X':
    return KEY_X;
  case 'Y':
    return KEY_Y;
  case 'Z':
    return KEY_Z;

  case '0':
    return KEY_NUMPAD0;
  case '1':
    return KEY_NUMPAD1;
  case '2':
    return KEY_NUMPAD2;
  case '3':
    return KEY_NUMPAD3;
  case '4':
    return KEY_NUMPAD4;
  case '5':
    return KEY_NUMPAD5;
  case '6':
    return KEY_NUMPAD6;
  case '7':
    return KEY_NUMPAD7;
  case '8':
    return KEY_NUMPAD8;
  case '9':
    return KEY_NUMPAD9;

  case VK_OEM_COMMA:
    return KEY_COMMA;
  case VK_OEM_MINUS:
    return KEY_MINUS;
  case VK_OEM_PERIOD:
    return KEY_PERIOD;
  case VK_OEM_1:
    return KEY_SEMICOLON;
  case VK_OEM_2:
    return KEY_SLASH;
  case VK_OEM_3:
    return KEY_GRAVE;

  case VK_BACK:
    return KEY_BACKSPACE;
  case VK_CAPITAL:
    return KEY_CAPITAL;
  case VK_DELETE:
    return KEY_DELETE;
  case VK_DOWN:
    return KEY_DOWN;
  case VK_END:
    return KEY_END;
  case VK_RETURN:
    return KEY_ENTER;
  case VK_ESCAPE:
    return KEY_ESCAPE;
  case VK_F1:
    return KEY_F1;
  case VK_F2:
    return KEY_F2;
  case VK_F3:
    return KEY_F3;
  case VK_F4:
    return KEY_F4;
  case VK_F5:
    return KEY_F5;
  case VK_F6:
    return KEY_F6;
  case VK_F7:
    return KEY_F7;
  case VK_F8:
    return KEY_F8;
  case VK_F9:
    return KEY_F9;
  case VK_F10:
    return KEY_F10;
  case VK_F11:
    return KEY_F11;
  case VK_F12:
    return KEY_F12;
  case VK_F13:
    return KEY_F13;
  case VK_F14:
    return KEY_F14;
  case VK_F15:
    return KEY_F15;
  case VK_F16:
    return KEY_F16;
  case VK_F17:
    return KEY_F17;
  case VK_F18:
    return KEY_F18;
  case VK_F19:
    return KEY_F19;
  case VK_F20:
    return KEY_F20;
  case VK_HOME:
    return KEY_HOME;
  case VK_INSERT:
    return KEY_INSERT;
  case VK_LEFT:
    return KEY_LEFT;
  case VK_LMENU:
    return KEY_LMENU;
  case VK_LCONTROL:
    return KEY_LCONTROL;
  case VK_LSHIFT:
    return KEY_LSHIFT;
  case VK_LWIN:
    return KEY_LWIN;
  case VK_NUMLOCK:
    return KEY_NUMLOCK;
  case VK_PRINT:
    return KEY_PRINT;
  case VK_RIGHT:
    return KEY_RIGHT;
  case VK_RMENU:
    return KEY_RMENU;
  case VK_RCONTROL:
    return KEY_RCONTROL;
  case VK_RSHIFT:
    return KEY_RSHIFT;
  case VK_RWIN:
    return KEY_RWIN;
  case VK_SPACE:
    return KEY_SPACE;
  case VK_TAB:
    return KEY_TAB;
  case VK_UP:
    return KEY_UP;

  case VK_ADD:
    return KEY_ADD;
  case VK_DECIMAL:
    return KEY_DECIMAL;
  case VK_DIVIDE:
    return KEY_DIVIDE;
  case VK_MULTIPLY:
    return KEY_MULTIPLY;
  case VK_SUBTRACT:
    return KEY_SUBTRACT;

  default:
    return KEY_MAX_KEYS;
  }
}

#endif
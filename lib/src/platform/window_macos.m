#include "window.h"

#if defined(PLATFORM_APPLE)
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

@class ApplicationDelegate;
@class WindowDelegate;
@class ContentView;

typedef struct PlatformState {
  ApplicationDelegate *app_delegate;
  WindowDelegate *wnd_delegate;
  NSWindow *window;
  ContentView *view;
  CAMetalLayer *layer;
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
} PlatformState;

// Key translation
static Keys translate_keycode(uint32_t ns_keycode);

// Helper functions for cursor management
static void hide_cursor(PlatformState *state);
static void show_cursor(PlatformState *state);
static void update_cursor_image(PlatformState *state);
static void center_cursor_in_window(PlatformState *state);
static bool8_t cursor_in_content_area(PlatformState *state);

@interface WindowDelegate : NSObject <NSWindowDelegate> {
  PlatformState *state;
}

- (instancetype)initWithState:(PlatformState *)init_state;

@end // WindowDelegate

@implementation WindowDelegate

- (instancetype)initWithState:(PlatformState *)init_state {
  self = [super init];

  if (self != nil) {
    state = init_state;
    state->quit_flagged = false_v;
  }

  return self;
}

- (BOOL)windowShouldClose:(id)sender {
  state->quit_flagged = true_v;

  Event event = {.type = EVENT_TYPE_WINDOW_CLOSE};
  event_manager_dispatch(state->event_manager, event);

  return YES;
}

- (void)windowDidResize:(NSNotification *)notification {
  const NSRect contentRect = [state->view frame];
  const NSRect framebufferRect = [state->view convertRectToBacking:contentRect];
  WindowResizeEventData resize_data = {
      .width = (uint32_t)framebufferRect.size.width,
      .height = (uint32_t)framebufferRect.size.height};
  Event event = {.type = EVENT_TYPE_WINDOW_RESIZE,
                 .data = &resize_data,
                 .data_size = sizeof(WindowResizeEventData)};
  event_manager_dispatch(state->event_manager, event);

  // Re-center cursor if in capture mode after window resize
  if (state->mouse_captured) {
    center_cursor_in_window(state);
  }
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
  WindowResizeEventData resize_data = {.width = 0, .height = 0};
  Event event = {.type = EVENT_TYPE_WINDOW_RESIZE,
                 .data = &resize_data,
                 .data_size = sizeof(WindowResizeEventData)};
  event_manager_dispatch(state->event_manager, event);

  // [state->window miniaturize:nil]; // Redundant, system already miniaturized
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
  const NSRect contentRect = [state->view frame];
  const NSRect framebufferRect = [state->view convertRectToBacking:contentRect];
  WindowResizeEventData resize_data = {
      .width = (uint32_t)framebufferRect.size.width,
      .height = (uint32_t)framebufferRect.size.height};
  Event event = {.type = EVENT_TYPE_WINDOW_RESIZE,
                 .data = &resize_data,
                 .data_size = sizeof(WindowResizeEventData)};
  event_manager_dispatch(state->event_manager, event);

  // [state->window deminiaturize:nil]; // Redundant, system already
  // deminiaturized
}

// This method is called when the window is about to be closed by the system
// (e.g., user clicked the close button and windowShouldClose: returned YES, or
// [window close] was called).
- (void)windowWillClose:(NSNotification *)notification {
  if (state && state->window == [notification object]) {
    [[notification object] setDelegate:nil];

    state->view = nil;
    state->layer = nil;
    state->window = nil;
  }
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
  if (state->mouse_captured) {
    center_cursor_in_window(state);
  }
  update_cursor_image(state);
}

- (void)windowDidResignKey:(NSNotification *)notification {
  // When window loses focus, show cursor if it was hidden due to capture
  if (state->mouse_captured) {
    show_cursor(state);
  }
}

@end // WindowDelegate

@interface ContentView : NSView <NSTextInputClient> {
  NSWindow *window;
  NSTrackingArea *trackingArea;
  NSMutableAttributedString *markedText;
  PlatformState *platform_state;
}

- (instancetype)initWithWindow:(NSWindow *)initWindow
                         state:(PlatformState *)initState;

@end // ContentView

@implementation ContentView

- (instancetype)initWithWindow:(NSWindow *)initWindow
                         state:(PlatformState *)initState {
  self = [super init];
  if (self != nil) {
    window = initWindow;
    platform_state = initState;
    trackingArea = nil;
    markedText = [[NSMutableAttributedString alloc] init];

    [self updateTrackingAreas];
  }

  return self;
}

- (BOOL)canBecomeKeyView {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)wantsUpdateLayer {
  return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
  return YES;
}

- (void)mouseDown:(NSEvent *)event {
  input_process_button(platform_state->input_state, BUTTON_LEFT, true_v);
}

- (void)mouseDragged:(NSEvent *)event {
  // Equivalent to moving the mouse for now
  [self mouseMoved:event];
}

- (void)mouseUp:(NSEvent *)event {
  input_process_button(platform_state->input_state, BUTTON_LEFT, false_v);
}

- (void)mouseMoved:(NSEvent *)event {
  if (platform_state->mouse_captured) {
    // In capture mode, use delta movement
    const float64_t dx = [event deltaX] - platform_state->cursor_warp_delta_x;
    const float64_t dy = [event deltaY] - platform_state->cursor_warp_delta_y;

    // Get current virtual cursor position from input state
    int32_t current_x, current_y;
    input_get_mouse_position(platform_state->input_state, &current_x,
                             &current_y);

    // Update virtual position with delta
    int32_t new_x = current_x + (int32_t)dx;
    int32_t new_y = current_y + (int32_t)dy;

    input_process_mouse_move(platform_state->input_state, new_x, new_y);

    // NOTE: Do NOT update restore coordinates during capture mode.
    // The restore position should remain fixed to where the mouse was
    // when capture was first enabled, not follow virtual cursor movement.
#ifndef NDEBUG
    static int log_counter = 0;
    if (++log_counter % 60 == 0) { // Log every 60 mouse moves to avoid spam
      log_debug("Virtual cursor: (%d, %d)", new_x, new_y);
    }
#endif
  } else {
    // Normal mode, use absolute position
    const NSPoint pos = [event locationInWindow];

    // Need to invert Y on macOS, since origin is bottom-left.
    // Also need to scale the mouse position by the device pixel ratio so screen
    // lookups are correct.
    NSSize window_size = platform_state->layer.drawableSize;
    int32_t x = pos.x * platform_state->layer.contentsScale;
    int32_t y =
        window_size.height - (pos.y * platform_state->layer.contentsScale);
#ifndef NDEBUG
    static int normal_mode_log_counter = 0;
    if (++normal_mode_log_counter % 60 ==
        0) { // Log every 60 mouse moves to avoid spam
      log_debug("Normal mode cursor: (%d, %d) -> Window coords: (%.1f, %.1f)",
                x, y, pos.x, pos.y);
    }
#endif

    input_process_mouse_move(platform_state->input_state, x, y);
  }

  // Reset warp deltas
  platform_state->cursor_warp_delta_x = 0;
  platform_state->cursor_warp_delta_y = 0;
}

- (void)rightMouseDown:(NSEvent *)event {
  input_process_button(platform_state->input_state, BUTTON_RIGHT, true_v);
}

- (void)rightMouseDragged:(NSEvent *)event {
  // Equivalent to moving the mouse for now
  [self mouseMoved:event];
}

- (void)rightMouseUp:(NSEvent *)event {
  input_process_button(platform_state->input_state, BUTTON_RIGHT, false_v);
}

- (void)otherMouseDown:(NSEvent *)event {
  // Interpreted as middle click
  input_process_button(platform_state->input_state, BUTTON_MIDDLE, true_v);
}

- (void)otherMouseDragged:(NSEvent *)event {
  // Equivalent to moving the mouse for now
  [self mouseMoved:event];
}

- (void)otherMouseUp:(NSEvent *)event {
  // Interpreted as middle click
  input_process_button(platform_state->input_state, BUTTON_MIDDLE, false_v);
}

- (void)keyDown:(NSEvent *)event {
  Keys key = translate_keycode((uint32_t)[event keyCode]);

  input_process_key(platform_state->input_state, key, true_v);

  [self interpretKeyEvents:@[ event ]];
}

- (void)keyUp:(NSEvent *)event {
  Keys key = translate_keycode((uint32_t)[event keyCode]);

  input_process_key(platform_state->input_state, key, false_v);
}

- (void)scrollWheel:(NSEvent *)event {
  input_process_mouse_wheel(platform_state->input_state,
                            (int8_t)[event scrollingDeltaY]);
}

- (void)mouseEntered:(NSEvent *)event {
  if (platform_state->mouse_captured) {
    hide_cursor(platform_state);
  }
}

- (void)mouseExited:(NSEvent *)event {
  if (platform_state->mouse_captured) {
    show_cursor(platform_state);
  }
}

- (void)cursorUpdate:(NSEvent *)event {
  update_cursor_image(platform_state);
}

- (void)updateTrackingAreas {
  if (trackingArea != nil) {
    [self removeTrackingArea:trackingArea];
    [trackingArea release];
  }

  const NSTrackingAreaOptions options =
      NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
      NSTrackingEnabledDuringMouseDrag | NSTrackingCursorUpdate |
      NSTrackingInVisibleRect | NSTrackingAssumeInside;

  trackingArea = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                              options:options
                                                owner:self
                                             userInfo:nil];

  [self addTrackingArea:trackingArea];
  [super updateTrackingAreas];
}

- (void)dealloc {
  if (trackingArea) {
    [trackingArea release];
  }
  if (markedText) {
    [markedText release];
  }
  [super dealloc];
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange {
}

- (void)unmarkText {
}

// Defines a constant for empty ranges in NSTextInputClient
static const NSRange kEmptyRange = {NSNotFound, 0};

- (NSRange)selectedRange {
  return kEmptyRange;
}

- (NSRange)markedRange {
  return kEmptyRange;
}

- (BOOL)hasMarkedText {
  return false_v;
}

- (nullable NSAttributedString *)
    attributedSubstringForProposedRange:(NSRange)range
                            actualRange:(nullable NSRangePointer)actualRange {
  return nil;
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
  return [NSArray array];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range
                         actualRange:(nullable NSRangePointer)actualRange {
  return NSMakeRect(0, 0, 0, 0);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  return 0;
}

@end // ContentView

@interface ApplicationDelegate : NSObject <NSApplicationDelegate> {
}

@end // ApplicationDelegate

@implementation ApplicationDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
  // Posting an empty event at start
  @autoreleasepool {

    NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];

  } // autoreleasepool

  [NSApp stop:nil];
}

@end // ApplicationDelegate

/********************************************************************************
 *********************** External Window Functions *****************************
 ********************************************************************************
 */

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

  state->app_delegate = nil;
  state->wnd_delegate = nil;
  state->window = nil;
  state->view = nil;
  state->layer = nil;
  state->quit_flagged = false_v;
  state->event_manager = event_manager;
  state->input_state = &window->input_state;

  // Initialize mouse capture state
  state->cursor_hidden = false_v;
  state->mouse_captured = false_v;
  state->restore_cursor_x = 0.0;
  state->restore_cursor_y = 0.0;
  state->cursor_warp_delta_x = 0.0;
  state->cursor_warp_delta_y = 0.0;

  window->platform_state = state;

  @autoreleasepool {

    [NSApplication sharedApplication];

    // App delegate creation
    state->app_delegate = [[ApplicationDelegate alloc] init];
    if (!state->app_delegate) {
      log_error("Failed to create application delegate");
      free(state);
      return false_v;
    }
    [NSApp setDelegate:state->app_delegate];

    // Window delegate creation
    state->wnd_delegate = [[WindowDelegate alloc] initWithState:state];
    if (!state->wnd_delegate) {
      log_error("Failed to create window delegate");
      free(state);
      return false_v;
    }

    // Window creation
    state->window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(x, y, width, height)
                                    styleMask:NSWindowStyleMaskMiniaturizable |
                                              NSWindowStyleMaskTitled |
                                              NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    if (!state->window) {
      log_error("Failed to create window");
      free(state);
      return false_v;
    }

    // Layer creation
    state->layer = [CAMetalLayer layer];
    if (!state->layer) {
      log_error("Failed to create layer for view");
      free(state);
      return false_v;
    }

    // View creation
    state->view = [[ContentView alloc] initWithWindow:state->window
                                                state:state];
    [state->view setLayer:state->layer];
    [state->view setWantsLayer:YES];

    // Setting window properties
    [state->window setLevel:NSNormalWindowLevel];
    [state->window setContentView:state->view];
    [state->window makeFirstResponder:state->view];
    [state->window setTitle:@(title)];
    [state->window setDelegate:state->wnd_delegate];
    [state->window setAcceptsMouseMovedEvents:YES];
    [state->window setRestorable:NO];

    if (![[NSRunningApplication currentApplication] isFinishedLaunching])
      [NSApp run];

    // Making the app a proper UI app since we're unbundled
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Putting window in front on launch
    [NSApp activateIgnoringOtherApps:YES];
    [state->window makeKeyAndOrderFront:nil];

    event_manager_dispatch(event_manager,
                           (Event){.type = EVENT_TYPE_WINDOW_INIT});

    return true_v;

  } // autoreleasepool
}

void window_destroy(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  // Simply cold-cast to the known type.
  PlatformState *state = (PlatformState *)window->platform_state;

  @autoreleasepool {

    if (state->app_delegate) {
      [NSApp setDelegate:nil];
      [state->app_delegate release];
      state->app_delegate = nil;
    }

    if (state->wnd_delegate) {
      [state->window setDelegate:nil];
      [state->wnd_delegate release];
      state->wnd_delegate = nil;
    }

    if (state->layer) {
      [state->layer release];
      state->layer = nil;
    }

    if (state->view) {
      [state->view release];
      state->view = nil;
    }

    if (state->window) {
      [state->window close];
      state->window = nil;
    }
  }

  input_shutdown(state->input_state);
  free(state);
}

bool8_t window_update(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  // If a quit has been flagged (e.g., by windowShouldClose or other means),
  // stop processing new system events here. The main loop will then call
  // window_destroy.
  if (state->quit_flagged) {
    return !state->quit_flagged;
  }

  @autoreleasepool {

    NSEvent *event;

    for (;;) {
      event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                 untilDate:[NSDate distantPast]
                                    inMode:NSDefaultRunLoopMode
                                   dequeue:YES];

      if (!event)
        break;

      [NSApp sendEvent:event];
    }

  } // autoreleasepool

  return !state->quit_flagged;
}

WindowPixelSize window_get_pixel_size(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  const NSRect contentRect = [state->view frame];
  const NSRect framebufferRect = [state->view convertRectToBacking:contentRect];

  return (WindowPixelSize){
      .width = (uint32_t)framebufferRect.size.width,
      .height = (uint32_t)framebufferRect.size.height,
  };
}

void *window_get_metal_layer(Window *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;
  return state->layer;
}

void window_set_mouse_capture(Window *window, bool8_t capture) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  @autoreleasepool {
    if (capture) {
      state->mouse_captured = true_v;

      const NSPoint pos = [state->window mouseLocationOutsideOfEventStream];
      const NSRect contentRect = [state->view frame];

      // Store restore coordinates in window coordinate system (bottom-left
      // origin)
      state->restore_cursor_x = pos.x;
      state->restore_cursor_y = pos.y;

      // Initialize virtual cursor position to match current physical position
      // This provides continuity when entering capture mode
      NSSize window_size = state->layer.drawableSize;
      int32_t virtual_x = (int32_t)(pos.x * state->layer.contentsScale);
      int32_t virtual_y =
          (int32_t)(window_size.height - (pos.y * state->layer.contentsScale));
      input_process_mouse_move(state->input_state, virtual_x, virtual_y);

      log_debug(
          "Initialized virtual cursor to: (%d, %d) from physical: (%.1f, %.1f)",
          virtual_x, virtual_y, pos.x, pos.y);

      CGAssociateMouseAndMouseCursorPosition(false);

      update_cursor_image(state);
    } else {
      state->mouse_captured = false_v;

      CGAssociateMouseAndMouseCursorPosition(true);

      // Restore cursor position directly without recursion
      const NSRect contentRect = [state->view frame];

      // Clamp restore position to window bounds
      // Note: restore coordinates are stored in bottom-left origin system
      float64_t clamped_x = max_f64(
          0.0, min_f64(state->restore_cursor_x, contentRect.size.width));
      float64_t clamped_y = max_f64(
          0.0, min_f64(state->restore_cursor_y, contentRect.size.height));

      log_debug("Restoring cursor to window coords: (%.1f, %.1f) in window "
                "size: (%.1f, %.1f)",
                clamped_x, clamped_y, contentRect.size.width,
                contentRect.size.height);

      // Create local rect - coordinates are already in bottom-left origin
      const NSRect localRect = NSMakeRect(clamped_x, clamped_y, 0, 0);
      const NSRect globalRect = [state->window convertRectToScreen:localRect];
      const NSPoint globalPoint = globalRect.origin;

      const CGFloat screenHeight =
          CGDisplayBounds(CGMainDisplayID()).size.height;
      CGWarpMouseCursorPosition(
          CGPointMake(globalPoint.x, screenHeight - globalPoint.y - 1));

      update_cursor_image(state);
    }
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

  @autoreleasepool {
    update_cursor_image(state);

    const NSRect contentRect = [state->view frame];

    const NSPoint currentPos =
        [state->window mouseLocationOutsideOfEventStream];

    // Calculate warp deltas to smooth out movement
    state->cursor_warp_delta_x += x - currentPos.x;
    state->cursor_warp_delta_y += y - (contentRect.size.height - currentPos.y);

    // Convert window coordinates to screen coordinates
    const NSRect localRect =
        NSMakeRect(x, contentRect.size.height - y - 1, 0, 0);
    const NSRect globalRect = [state->window convertRectToScreen:localRect];
    const NSPoint globalPoint = globalRect.origin;

    // Transform Y coordinate for screen space (macOS screen origin is top -
    // left for CGWarp)
    const CGFloat screenHeight = CGDisplayBounds(CGMainDisplayID()).size.height;
    CGWarpMouseCursorPosition(
        CGPointMake(globalPoint.x, screenHeight - globalPoint.y - 1));

    // Re-associate mouse and cursor position to prevent freezing
    // This is a workaround for macOS behavior after warping
    if (!state->mouse_captured) {
      CGAssociateMouseAndMouseCursorPosition(true);
    }
  }
}

/********************************************************************************
 ***************************** Helper Functions ********************************
 ********************************************************************************
 */

bool8_t cursor_in_content_area(PlatformState *state) {
  const NSPoint pos = [state->window mouseLocationOutsideOfEventStream];
  return [state->view mouse:pos inRect:[state->view frame]];
}

void hide_cursor(PlatformState *state) {
  if (!state->cursor_hidden) {
    [NSCursor hide];
    state->cursor_hidden = true_v;
  }
}

void show_cursor(PlatformState *state) {
  if (state->cursor_hidden) {
    [NSCursor unhide];
    state->cursor_hidden = false_v;
  }
}

void update_cursor_image(PlatformState *state) {
  if (state->mouse_captured) {
    hide_cursor(state);
  } else {
    show_cursor(state);
    [[NSCursor arrowCursor] set];
  }
}

void center_cursor_in_window(PlatformState *state) {
  const NSRect contentRect = [state->view frame];
  const NSRect globalRect = [state->window
      convertRectToScreen:NSMakeRect(contentRect.size.width / 2.0,
                                     contentRect.size.height / 2.0, 0, 0)];
  const NSPoint globalPoint = globalRect.origin;

  const CGFloat screenHeight = CGDisplayBounds(CGMainDisplayID()).size.height;
  CGWarpMouseCursorPosition(
      CGPointMake(globalPoint.x, screenHeight - globalPoint.y - 1));
}

Keys translate_keycode(uint32_t ns_keycode) {
  switch (ns_keycode) {
  case 0x1D:
    return KEY_NUMPAD0;
  case 0x12:
    return KEY_NUMPAD1;
  case 0x13:
    return KEY_NUMPAD2;
  case 0x14:
    return KEY_NUMPAD3;
  case 0x15:
    return KEY_NUMPAD4;
  case 0x17:
    return KEY_NUMPAD5;
  case 0x16:
    return KEY_NUMPAD6;
  case 0x1A:
    return KEY_NUMPAD7;
  case 0x1C:
    return KEY_NUMPAD8;
  case 0x19:
    return KEY_NUMPAD9;

  case 0x00:
    return KEY_A;
  case 0x0B:
    return KEY_B;
  case 0x08:
    return KEY_C;
  case 0x02:
    return KEY_D;
  case 0x0E:
    return KEY_E;
  case 0x03:
    return KEY_F;
  case 0x05:
    return KEY_G;
  case 0x04:
    return KEY_H;
  case 0x22:
    return KEY_I;
  case 0x26:
    return KEY_J;
  case 0x28:
    return KEY_K;
  case 0x25:
    return KEY_L;
  case 0x2E:
    return KEY_M;
  case 0x2D:
    return KEY_N;
  case 0x1F:
    return KEY_O;
  case 0x23:
    return KEY_P;
  case 0x0C:
    return KEY_Q;
  case 0x0F:
    return KEY_R;
  case 0x01:
    return KEY_S;
  case 0x11:
    return KEY_T;
  case 0x20:
    return KEY_U;
  case 0x09:
    return KEY_V;
  case 0x0D:
    return KEY_W;
  case 0x07:
    return KEY_X;
  case 0x10:
    return KEY_Y;
  case 0x06:
    return KEY_Z;

  case 0x27:
    return KEY_MAX_KEYS; // Apostrophe
  case 0x2A:
    return KEY_MAX_KEYS; // Backslash
  case 0x2B:
    return KEY_COMMA;
  case 0x18:
    return KEY_MAX_KEYS; // Equal
  case 0x32:
    return KEY_GRAVE;
  case 0x21:
    return KEY_MAX_KEYS; // Left bracket
  case 0x1B:
    return KEY_MINUS;
  case 0x2F:
    return KEY_PERIOD;
  case 0x1E:
    return KEY_MAX_KEYS; // Right bracket
  case 0x29:
    return KEY_SEMICOLON;
  case 0x2C:
    return KEY_SLASH;
  case 0x0A:
    return KEY_MAX_KEYS;

  case 0x33:
    return KEY_BACKSPACE;
  case 0x39:
    return KEY_CAPITAL;
  case 0x75:
    return KEY_DELETE;
  case 0x7D:
    return KEY_DOWN;
  case 0x77:
    return KEY_END;
  case 0x24:
    return KEY_ENTER;
  case 0x35:
    return KEY_ESCAPE;
  case 0x7A:
    return KEY_F1;
  case 0x78:
    return KEY_F2;
  case 0x63:
    return KEY_F3;
  case 0x76:
    return KEY_F4;
  case 0x60:
    return KEY_F5;
  case 0x61:
    return KEY_F6;
  case 0x62:
    return KEY_F7;
  case 0x64:
    return KEY_F8;
  case 0x65:
    return KEY_F9;
  case 0x6D:
    return KEY_F10;
  case 0x67:
    return KEY_F11;
  case 0x6F:
    return KEY_F12;
  case 0x69:
    return KEY_PRINT;
  case 0x6B:
    return KEY_F14;
  case 0x71:
    return KEY_F15;
  case 0x6A:
    return KEY_F16;
  case 0x40:
    return KEY_F17;
  case 0x4F:
    return KEY_F18;
  case 0x50:
    return KEY_F19;
  case 0x5A:
    return KEY_F20;
  case 0x73:
    return KEY_HOME;
  case 0x72:
    return KEY_INSERT;
  case 0x7B:
    return KEY_LEFT;
  case 0x3A:
    return KEY_LMENU;
  case 0x3B:
    return KEY_LCONTROL;
  case 0x38:
    return KEY_LSHIFT;
  case 0x37:
    return KEY_LWIN;
  case 0x6E:
    return KEY_MAX_KEYS; // Menu
  case 0x47:
    return KEY_NUMLOCK;
  case 0x79:
    return KEY_MAX_KEYS; // Page down
  case 0x74:
    return KEY_MAX_KEYS; // Page up
  case 0x7C:
    return KEY_RIGHT;
  case 0x3D:
    return KEY_RMENU;
  case 0x3E:
    return KEY_RCONTROL;
  case 0x3C:
    return KEY_RSHIFT;
  case 0x36:
    return KEY_RWIN;
  case 0x31:
    return KEY_SPACE;
  case 0x30:
    return KEY_TAB;
  case 0x7E:
    return KEY_UP;

  case 0x52:
    return KEY_NUMPAD0;
  case 0x53:
    return KEY_NUMPAD1;
  case 0x54:
    return KEY_NUMPAD2;
  case 0x55:
    return KEY_NUMPAD3;
  case 0x56:
    return KEY_NUMPAD4;
  case 0x57:
    return KEY_NUMPAD5;
  case 0x58:
    return KEY_NUMPAD6;
  case 0x59:
    return KEY_NUMPAD7;
  case 0x5B:
    return KEY_NUMPAD8;
  case 0x5C:
    return KEY_NUMPAD9;
  case 0x45:
    return KEY_ADD;
  case 0x41:
    return KEY_DECIMAL;
  case 0x4B:
    return KEY_DIVIDE;
  case 0x4C:
    return KEY_ENTER;
  case 0x51:
    return KEY_NUMPAD_EQUAL;
  case 0x43:
    return KEY_MULTIPLY;
  case 0x4E:
    return KEY_SUBTRACT;

  default:
    return KEY_MAX_KEYS;
  }
}
#endif

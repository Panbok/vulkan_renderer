#include "core/vkr_window.h"
#include "platform/vkr_window_internal.h"

#if defined(PLATFORM_APPLE)
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#import <QuartzCore/QuartzCore.h>

#include <math.h>

bool8_t vkr_platform_clipboard_read_text(uint8_t *buffer, uint32_t capacity,
                                         uint32_t *out_length) {
  if (!buffer || capacity == 0u || !out_length)
    return false_v;
  buffer[0] = 0u;
  *out_length = 0u;
  @autoreleasepool {
    NSString *text =
        [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!text)
      return false_v;
    NSUInteger used = 0u;
    [text getBytes:buffer
             maxLength:capacity - 1u
            usedLength:&used
              encoding:NSUTF8StringEncoding
               options:0
                 range:NSMakeRange(0, [text length])
        remainingRange:NULL];
    buffer[used] = 0u;
    *out_length = (uint32_t)used;
    return true_v;
  }
}

bool8_t vkr_platform_clipboard_write_text(const uint8_t *text,
                                          uint32_t length) {
  if (!text && length != 0u)
    return false_v;
  @autoreleasepool {
    // initWithBytes: requires a pointer even for an empty string.
    const void *bytes = text ? (const void *)text : (const void *)"";
    NSString *string = [[NSString alloc] initWithBytes:bytes
                                                length:length
                                              encoding:NSUTF8StringEncoding];
    if (!string)
      return false_v;
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    const bool8_t written = [pasteboard setString:string
                                          forType:NSPasteboardTypeString];
    [string release];
    return written;
  }
}

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
  bool8_t close_requested;
  EventManager *event_manager;
  InputState *input_state;
  VkrWindow *owner;
  /** Atomic `{availability|revision, IEEE-754 current-headroom bits}` cache.
   * The window delegate and main-thread pump publish it; frame preparation only
   * copies the settled snapshot. */
  VkrAtomicUint64 display_output_state;

  // Pointer shape requested by the UI.
  VkrWindowCursor cursor;
  // Unified title bar: backing-pixel drag rectangle published by the UI.
  int32_t drag_x;
  int32_t drag_y;
  int32_t drag_width;
  int32_t drag_height;
  bool8_t drag_allowed;

  // Mouse capture state
  bool8_t cursor_hidden;
  bool8_t mouse_captured;
  float64_t restore_cursor_x;
  float64_t restore_cursor_y;
  float64_t cursor_warp_delta_x;
  float64_t cursor_warp_delta_y;
  /* Fraction of a wheel line carried between precise scroll events. */
  float64_t scroll_remainder_lines;
} PlatformState;

static uint32_t vkr_window_display_output_float_bits(float32_t value) {
  uint32_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  return bits;
}

static float32_t vkr_window_display_output_bits_float(uint32_t bits) {
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static uint64_t vkr_window_display_output_pack(uint32_t revision,
                                               bool8_t available,
                                               float32_t headroom) {
  const uint32_t state = (revision & 0x7fffffffu) |
                         (available ? 0x80000000u : 0u);
  return ((uint64_t)state << 32u) |
         (uint64_t)vkr_window_display_output_float_bits(headroom);
}

static void vkr_window_publish_display_output(PlatformState *state,
                                              float32_t current_headroom,
                                              bool8_t available) {
  if (!isfinite(current_headroom) || current_headroom < 1.0f)
    current_headroom = 1.0f;
  const uint32_t bits =
      vkr_window_display_output_float_bits(current_headroom);
  uint64_t expected = vkr_atomic_uint64_load(&state->display_output_state,
                                             VKR_MEMORY_ORDER_ACQUIRE);
  for (;;) {
    const uint32_t current_state = (uint32_t)(expected >> 32u);
    const bool8_t current_available = (current_state & 0x80000000u) != 0u;
    if ((uint32_t)expected == bits && current_available == available)
      return;
    uint32_t revision = (current_state & 0x7fffffffu) + 1u;
    if (revision == 0u || revision > 0x7fffffffu)
      revision = 1u;
    const uint64_t desired =
        vkr_window_display_output_pack(revision, available, current_headroom);
    if (vkr_atomic_uint64_compare_exchange(
            &state->display_output_state, &expected, desired,
            VKR_MEMORY_ORDER_ACQ_REL, VKR_MEMORY_ORDER_ACQUIRE))
      return;
  }
}

static void vkr_window_refresh_display_output(PlatformState *state) {
  if (!state->window) {
    vkr_window_publish_display_output(state, 1.0f, false_v);
    return;
  }
  NSScreen *screen = state->window.screen;
  if (!screen)
    screen = [NSScreen mainScreen];
  const float32_t potential_headroom =
      screen ? (float32_t)screen.maximumPotentialExtendedDynamicRangeColorComponentValue
             : 1.0f;
  const float32_t current_headroom =
      screen ? (float32_t)screen.maximumExtendedDynamicRangeColorComponentValue
             : 1.0f;
  vkr_window_publish_display_output(
      state, current_headroom,
      isfinite(potential_headroom) && potential_headroom > 1.0f);
}

static void vkr_window_dispatch_resize(PlatformState *state, uint32_t width,
                                       uint32_t height) {
  VkrWindowResizeEventData resize_data = {.width = width, .height = height};
  Event event = {.type = EVENT_TYPE_WINDOW_RESIZE,
                 .data = &resize_data,
                 .data_size = sizeof(VkrWindowResizeEventData)};
  event_manager_dispatch(state->event_manager, event);
}

// Key translation
static Keys translate_keycode(uint32_t ns_keycode);

static bool8_t canvas_command_key(Keys key) {
  switch (key) {
  case KEY_A:
  case KEY_C:
  case KEY_X:
  case KEY_V:
  case KEY_P:
  case KEY_S:
  case KEY_Z:
    return true_v;
  default:
    return false_v;
  }
}

static void sync_modifier_keys(InputState *input, NSEventModifierFlags flags,
                               Keys changed_key) {
  static const struct {
    NSEventModifierFlags aggregate_mask;
    NSUInteger left_mask, right_mask;
    Keys left_key, right_key, aggregate_key;
  } modifiers[] = {
      {NSEventModifierFlagShift, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK,
       KEY_LSHIFT, KEY_RSHIFT, KEY_SHIFT},
      {NSEventModifierFlagControl, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK,
       KEY_LCONTROL, KEY_RCONTROL, KEY_CONTROL},
      {NSEventModifierFlagOption, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK,
       KEY_LMENU, KEY_RMENU, KEY_MAX_KEYS},
      {NSEventModifierFlagCommand, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK,
       KEY_LWIN, KEY_RWIN, KEY_MAX_KEYS},
  };
  for (uint32_t i = 0; i < ArrayCount(modifiers); ++i) {
    const bool8_t held = (flags & modifiers[i].aggregate_mask) != 0;
    bool8_t left = held && (flags & modifiers[i].left_mask) != 0;
    bool8_t right = held && (flags & modifiers[i].right_mask) != 0;
    if (held && !left && !right) {
      // Synthetic/accessibility events may supply only aggregate flags. Keep
      // known sides, use flagsChanged's keycode for transitions, then fall
      // back to left when the event does not identify a physical side.
      left = input_is_key_down(input, modifiers[i].left_key);
      right = input_is_key_down(input, modifiers[i].right_key);
      if (changed_key == modifiers[i].left_key) {
        left = !left;
        if (!left)
          right = true_v;
      } else if (changed_key == modifiers[i].right_key) {
        right = !right;
        if (!right)
          left = true_v;
      } else if (!left && !right)
        left = true_v;
    }
    input_process_key(input, modifiers[i].left_key, left);
    input_process_key(input, modifiers[i].right_key, right);
    if (modifiers[i].aggregate_key != KEY_MAX_KEYS)
      input_process_key(input, modifiers[i].aggregate_key, held);
  }
}

// Helper functions for cursor management
static void hide_cursor(PlatformState *state);
static void show_cursor(PlatformState *state);
static void update_cursor_image(PlatformState *state);
static void center_cursor_in_window(PlatformState *state);

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
  if (state->owner->defer_close) {
    state->close_requested = true_v;
    return NO;
  }
  state->quit_flagged = true_v;

  Event event = {.type = EVENT_TYPE_WINDOW_CLOSE};
  event_manager_dispatch(state->event_manager, event);

  return YES;
}

- (void)windowDidResize:(NSNotification *)notification {
  const CGFloat backingScale = [state->window backingScaleFactor];
  vkr_window_content_scale_publish(state->owner, (float32_t)backingScale);
  const NSRect contentRect = [state->view frame];
  const NSRect framebufferRect = [state->view convertRectToBacking:contentRect];

  // Update Metal layer drawable size for Retina displays
  [state->layer setDrawableSize:framebufferRect.size];

  vkr_window_dispatch_resize(state, (uint32_t)framebufferRect.size.width,
                             (uint32_t)framebufferRect.size.height);

  // Re-center cursor if in capture mode after window resize
  if (state->mouse_captured) {
    center_cursor_in_window(state);
  }
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
  (void)notification;
  vkr_window_refresh_display_output(state);
  const CGFloat backingScale = [state->window backingScaleFactor];
  vkr_window_content_scale_publish(state->owner, (float32_t)backingScale);
  [state->layer setContentsScale:backingScale];

  const NSRect contentRect = [state->view frame];
  const NSRect framebufferRect = [state->view convertRectToBacking:contentRect];
  [state->layer setDrawableSize:framebufferRect.size];

  vkr_window_dispatch_resize(state, (uint32_t)framebufferRect.size.width,
                             (uint32_t)framebufferRect.size.height);
}

- (void)windowDidChangeScreen:(NSNotification *)notification {
  (void)notification;
  vkr_window_refresh_display_output(state);
}

- (void)displayParametersChanged:(NSNotification *)notification {
  (void)notification;
  vkr_window_refresh_display_output(state);
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
  vkr_window_dispatch_resize(state, 0u, 0u);

  // [state->window miniaturize:nil]; // Redundant, system already miniaturized
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
  vkr_window_refresh_display_output(state);
  const CGFloat backingScale = [state->window backingScaleFactor];
  vkr_window_content_scale_publish(state->owner, (float32_t)backingScale);
  const NSRect contentRect = [state->view frame];
  const NSRect framebufferRect = [state->view convertRectToBacking:contentRect];

  [state->layer setDrawableSize:framebufferRect.size];

  vkr_window_dispatch_resize(state, (uint32_t)framebufferRect.size.width,
                             (uint32_t)framebufferRect.size.height);

  // [state->window deminiaturize:nil]; // Redundant, system already
  // deminiaturized
}

// This method is called when the window is about to be closed by the system
// (e.g., user clicked the close button and windowShouldClose: returned YES, or
// [window close] was called).
- (void)windowWillClose:(NSNotification *)notification {
  if (state && state->window == [notification object]) {
    [[notification object] setDelegate:nil];

    // The view and layer keep their owned references until window_destroy.
    state->window = nil;
    vkr_window_publish_display_output(state, 1.0f, false_v);
  }
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
  if (state->mouse_captured) {
    center_cursor_in_window(state);
  }
  update_cursor_image(state);
}

- (void)windowDidResignKey:(NSNotification *)notification {
  sync_modifier_keys(state->input_state, 0, KEY_MAX_KEYS);
  /* A release outside this window may never arrive. End editor RMB holds while
     leaving the latched camera-capture policy to the runtime. */
  input_process_button(state->input_state, BUTTON_RIGHT, false_v);
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

- (void)syncMouseButtonEvent:(NSEvent *)event {
  sync_modifier_keys(platform_state->input_state, [event modifierFlags],
                     KEY_MAX_KEYS);
  // Down/up events carry their own location; a preceding mouseMoved is not
  // guaranteed. Captured input keeps its existing virtual cursor position.
  if (!platform_state->mouse_captured)
    [self mouseMoved:event];
}

/* Content fills the window; only the UI's published title-bar area moves it,
 * so controls under the transparent title bar still receive clicks. */
- (BOOL)mouseDownCanMoveWindow {
  return NO;
}

- (void)mouseDown:(NSEvent *)event {
  [self syncMouseButtonEvent:event];
  if (platform_state->owner && platform_state->owner->unified_title_bar &&
      !platform_state->mouse_captured && platform_state->drag_allowed) {
    int32_t x = 0, y = 0;
    input_get_mouse_position(platform_state->input_state, &x, &y);
    if (x >= platform_state->drag_x && y >= platform_state->drag_y &&
        x < platform_state->drag_x + platform_state->drag_width &&
        y < platform_state->drag_y + platform_state->drag_height) {
      if ([event clickCount] == 2)
        [window performZoom:nil];
      else
        [window performWindowDragWithEvent:event];
      return;
    }
  }
  input_process_button(platform_state->input_state, BUTTON_LEFT, true_v);
}

- (void)mouseDragged:(NSEvent *)event {
  // Equivalent to moving the mouse for now
  [self mouseMoved:event];
}

- (void)mouseUp:(NSEvent *)event {
  [self syncMouseButtonEvent:event];
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
    // In captured mode, we want to invert the Y delta so that moving mouse up
    // results in positive Y movement (camera goes up)
    int32_t new_x = current_x + (int32_t)dx;
    int32_t new_y = current_y - (int32_t)dy; // Invert Y delta

    input_process_mouse_move(platform_state->input_state, new_x, new_y);
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

    input_process_mouse_move(platform_state->input_state, x, y);
  }

  // Reset warp deltas
  platform_state->cursor_warp_delta_x = 0;
  platform_state->cursor_warp_delta_y = 0;
}

- (void)rightMouseDown:(NSEvent *)event {
  [self syncMouseButtonEvent:event];
  input_process_button(platform_state->input_state, BUTTON_RIGHT, true_v);
}

- (void)rightMouseDragged:(NSEvent *)event {
  // Equivalent to moving the mouse for now
  [self mouseMoved:event];
}

- (void)rightMouseUp:(NSEvent *)event {
  [self syncMouseButtonEvent:event];
  input_process_button(platform_state->input_state, BUTTON_RIGHT, false_v);
}

- (void)otherMouseDown:(NSEvent *)event {
  [self syncMouseButtonEvent:event];
  // Interpreted as middle click
  input_process_button(platform_state->input_state, BUTTON_MIDDLE, true_v);
}

- (void)otherMouseDragged:(NSEvent *)event {
  // Equivalent to moving the mouse for now
  [self mouseMoved:event];
}

- (void)otherMouseUp:(NSEvent *)event {
  [self syncMouseButtonEvent:event];
  // Interpreted as middle click
  input_process_button(platform_state->input_state, BUTTON_MIDDLE, false_v);
}

- (BOOL)performKeyEquivalent:(NSEvent *)event {
  if ([super performKeyEquivalent:event])
    return YES;
  if ([event type] != NSEventTypeKeyDown || ![window isKeyWindow] ||
      [window firstResponder] != self)
    return NO;
  const NSEventModifierFlags modifiers = [event modifierFlags];
  if (!(modifiers & NSEventModifierFlagCommand) ||
      (modifiers & (NSEventModifierFlagOption | NSEventModifierFlagControl)) ||
      !canvas_command_key(translate_keycode((uint32_t)[event keyCode])))
    return NO;
  // Native menu bindings keep priority. The renderer's canvas handles only
  // its supported equivalents, through the same keyDown input bridge.
  if ([[NSApp mainMenu] performKeyEquivalent:event])
    return YES;
  [self keyDown:event];
  return YES;
}

- (void)keyDown:(NSEvent *)event {
  // Update modifiers before the normal key so InputState snapshots the chord
  // at press time, even when flagsChanged was not delivered to this view.
  sync_modifier_keys(platform_state->input_state, [event modifierFlags],
                     KEY_MAX_KEYS);
  Keys key = translate_keycode((uint32_t)[event keyCode]);
  if (key != KEY_MAX_KEYS)
    input_process_key(platform_state->input_state, key, true_v);

  // Canvas shortcuts are not text input or Cocoa edit-selector actions.
  if (!([event modifierFlags] & NSEventModifierFlagCommand) ||
      !canvas_command_key(key))
    [self interpretKeyEvents:@[ event ]];
}

- (void)keyUp:(NSEvent *)event {
  sync_modifier_keys(platform_state->input_state, [event modifierFlags],
                     KEY_MAX_KEYS);
  Keys key = translate_keycode((uint32_t)[event keyCode]);
  if (key != KEY_MAX_KEYS)
    input_process_key(platform_state->input_state, key, false_v);
}

- (void)flagsChanged:(NSEvent *)event {
  sync_modifier_keys(platform_state->input_state, [event modifierFlags],
                     translate_keycode((uint32_t)[event keyCode]));
}

/* Wheel input is in lines, and the UI scrolls VKR_WINDOW_SCROLL_LINE_POINTS
 * per line. Trackpads and Magic Mouse report precise deltas in points, so
 * they accumulate into whole lines and content follows the finger instead of
 * moving a line per point. A notched wheel already reports lines. */
#define VKR_WINDOW_SCROLL_LINE_POINTS 32.0

- (void)scrollWheel:(NSEvent *)event {
  float64_t lines = [event scrollingDeltaY];
  if ([event hasPreciseScrollingDeltas]) {
    platform_state->scroll_remainder_lines +=
        lines / VKR_WINDOW_SCROLL_LINE_POINTS;
    lines = trunc(platform_state->scroll_remainder_lines);
    platform_state->scroll_remainder_lines -= lines;
  }
  if (lines != 0.0)
    input_process_mouse_wheel(platform_state->input_state,
                              (int8_t)Clamp(lines, -127.0, 127.0));
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
  (void)replacementRange;
  NSString *committed = [string isKindOfClass:[NSAttributedString class]]
                            ? [(NSAttributedString *)string string]
                            : (NSString *)string;
  const NSUInteger length = [committed length];
  for (NSUInteger i = 0u; i < length; ++i) {
    const unichar first = [committed characterAtIndex:i];
    if (first >= 0xd800u && first <= 0xdbffu && i + 1u < length) {
      const unichar second = [committed characterAtIndex:i + 1u];
      if (second >= 0xdc00u && second <= 0xdfffu) {
        const uint32_t codepoint = 0x10000u +
                                   (((uint32_t)first - 0xd800u) << 10u) +
                                   ((uint32_t)second - 0xdc00u);
        (void)input_process_char(platform_state->input_state, codepoint);
        ++i;
      }
      continue;
    }
    if (first < 0xdc00u || first > 0xdfffu)
      (void)input_process_char(platform_state->input_state, (uint32_t)first);
  }
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
@public
  PlatformState *state;
}

@end // ApplicationDelegate

@implementation ApplicationDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
  if (state && state->owner->defer_close) {
    state->close_requested = true_v;
    return NSTerminateCancel;
  }
  return NSTerminateNow;
}

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

bool8_t vkr_window_create(VkrWindow *window, EventManager *event_manager,
                          const char *title, int32_t x, int32_t y,
                          uint32_t width, uint32_t height) {
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
  vkr_window_content_scale_init(window);

  PlatformState *state = (PlatformState *)malloc(sizeof(PlatformState));
  if (!state) {
    log_error("Failed to allocate PlatformState");
    window->platform_state = NULL;
    return false_v;
  }
  MemZero(state, sizeof(*state));

  state->app_delegate = nil;
  state->wnd_delegate = nil;
  state->window = nil;
  state->view = nil;
  state->layer = nil;
  state->quit_flagged = false_v;
  state->event_manager = event_manager;
  state->input_state = &window->input_state;
  state->owner = window;
  vkr_atomic_uint64_store(&state->display_output_state,
                          vkr_window_display_output_pack(1u, false_v, 1.0f),
                          VKR_MEMORY_ORDER_RELEASE);

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
      vkr_window_destroy(window);
      return false_v;
    }
    state->app_delegate->state = state;
    [NSApp setDelegate:state->app_delegate];

    // Window delegate creation
    state->wnd_delegate = [[WindowDelegate alloc] initWithState:state];
    if (!state->wnd_delegate) {
      log_error("Failed to create window delegate");
      vkr_window_destroy(window);
      return false_v;
    }

    // Window creation
    NSWindowStyleMask style_mask =
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskTitled |
        NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;
    if (window->unified_title_bar)
      style_mask |= NSWindowStyleMaskFullSizeContentView;
    state->window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(x, y, width, height)
                                    styleMask:style_mask
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    if (!state->window) {
      log_error("Failed to create window");
      vkr_window_destroy(window);
      return false_v;
    }

    // Layer creation
    state->layer = [[CAMetalLayer layer] retain];
    if (!state->layer) {
      log_error("Failed to create layer for view");
      vkr_window_destroy(window);
      return false_v;
    }

    // Configure for Retina displays - use full physical pixel resolution
    CGFloat backingScale = [state->window backingScaleFactor];
    vkr_window_content_scale_publish(window, (float32_t)backingScale);
    [state->layer setContentsScale:backingScale];

    // View creation
    state->view = [[ContentView alloc] initWithWindow:state->window
                                                state:state];
    if (!state->view) {
      log_error("Failed to create content view");
      vkr_window_destroy(window);
      return false_v;
    }
    [state->view setLayer:state->layer];
    [state->view setWantsLayer:YES];

    // Setting window properties
    if (window->unified_title_bar) {
      /* A compact unified toolbar centers the traffic lights in the taller
       * application top bar drawn beneath the transparent title bar. */
      [state->window setTitlebarAppearsTransparent:YES];
      [state->window setTitleVisibility:NSWindowTitleHidden];
      NSToolbar *toolbar =
          [[NSToolbar alloc] initWithIdentifier:@"vkr.unified.titlebar"];
      [state->window setToolbar:toolbar];
      [toolbar release];
      [state->window setToolbarStyle:NSWindowToolbarStyleUnifiedCompact];
    }
    [state->window setLevel:NSNormalWindowLevel];
    [state->window setContentView:state->view];
    [state->window makeFirstResponder:state->view];
    [state->window setTitle:@(title)];
    [state->window setDelegate:state->wnd_delegate];
    [[NSNotificationCenter defaultCenter]
        addObserver:state->wnd_delegate
           selector:@selector(displayParametersChanged:)
               name:NSApplicationDidChangeScreenParametersNotification
             object:nil];
    vkr_window_refresh_display_output(state);
    [state->window setAcceptsMouseMovedEvents:YES];
    [state->window setRestorable:NO];

    // Set initial drawable size for Retina displays
    const NSRect contentRect =
        [state->window contentRectForFrameRect:[state->window frame]];
    const NSRect framebufferRect =
        [state->view convertRectToBacking:contentRect];
    [state->layer setDrawableSize:framebufferRect.size];

    if (![[NSRunningApplication currentApplication] isFinishedLaunching])
      [NSApp run];

    // Making the app a proper UI app since we're unbundled
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Putting window in front on launch
    if (!window->hidden) {
      [NSApp activateIgnoringOtherApps:YES];
      [state->window makeKeyAndOrderFront:nil];
      if (getenv("VKR_WINDOW_DIAGNOSTICS")) {
        // Requested explicitly, so it bypasses the Release-compiled-out logger.
        NSRect frame = [state->window frame];
        fprintf(stderr, "VKR window pid=%d visible=%d key=%d miniaturized=%d policy=%ld frame=%.0f,%.0f %.0fx%.0f screen=%p\n",
                getpid(), [state->window isVisible], [state->window isKeyWindow],
                [state->window isMiniaturized], (long)[NSApp activationPolicy],
                frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
                (void *)[state->window screen]);
      }
    }

    event_manager_dispatch(event_manager,
                           (Event){.type = EVENT_TYPE_WINDOW_INIT});

    return true_v;

  } // autoreleasepool
}

void vkr_window_destroy(VkrWindow *window) {
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
      [[NSNotificationCenter defaultCenter] removeObserver:state->wnd_delegate];
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
  window->platform_state = NULL;
}

bool8_t vkr_window_close_requested(const VkrWindow *window) {
  const PlatformState *state = window ? window->platform_state : NULL;
  return state && state->close_requested;
}

void vkr_window_resolve_close(VkrWindow *window, bool8_t confirm) {
  PlatformState *state = window ? window->platform_state : NULL;
  if (!state || !state->close_requested) {
    return;
  }
  state->close_requested = false_v;
  if (confirm && !state->quit_flagged) {
    state->quit_flagged = true_v;
    Event event = {.type = EVENT_TYPE_WINDOW_CLOSE};
    event_manager_dispatch(state->event_manager, event);
  }
}

bool8_t vkr_window_update(VkrWindow *window) {
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
    vkr_window_refresh_display_output(state);

    NSEvent *event;

    for (;;) {
      event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                 untilDate:[NSDate distantPast]
                                    inMode:NSDefaultRunLoopMode
                                   dequeue:YES];

      if (!event)
        break;

      [NSApp sendEvent:event];
      // AppKit can consume Command key-up instead of forwarding it to the
      // canvas. Forward only a still-held key belonging to this key window;
      // a normally delivered keyUp has already cleared it, avoiding duplicates.
      if ([event type] == NSEventTypeKeyUp && state->window &&
          [state->window firstResponder] == state->view &&
          ([event window] == state->window ||
           (![event window] && [state->window isKeyWindow]))) {
        const Keys key = translate_keycode((uint32_t)[event keyCode]);
        if (key != KEY_MAX_KEYS && input_is_key_down(state->input_state, key))
          [state->view keyUp:event];
      }
    }

  } // autoreleasepool

  return !state->quit_flagged;
}

VkrWindowPixelSize vkr_window_get_pixel_size(VkrWindow *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  const NSRect contentRect = [state->view frame];
  const NSRect framebufferRect = [state->view convertRectToBacking:contentRect];

  return (VkrWindowPixelSize){
      .width = (uint32_t)framebufferRect.size.width,
      .height = (uint32_t)framebufferRect.size.height,
  };
}

VkrDisplayOutputSnapshot vkr_window_get_display_output(VkrWindow *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");
  PlatformState *state = (PlatformState *)window->platform_state;
  const uint64_t packed = vkr_atomic_uint64_load(&state->display_output_state,
                                                 VKR_MEMORY_ORDER_ACQUIRE);
  const uint32_t state_bits = (uint32_t)(packed >> 32u);
  const float32_t headroom =
      vkr_window_display_output_bits_float((uint32_t)packed);
  return (VkrDisplayOutputSnapshot){
      .headroom = isfinite(headroom) && headroom >= 1.0f ? headroom : 1.0f,
      .output_scale = 1.0f,
      .revision = state_bits & 0x7fffffffu,
      .available = (state_bits & 0x80000000u) != 0u,
  };
}

bool8_t vkr_window_resize(VkrWindow *window, uint32_t width, uint32_t height) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");
  if (width == 0 || height == 0) {
    return false_v;
  }

  PlatformState *state = (PlatformState *)window->platform_state;
  @autoreleasepool {
    if (!state->window || !state->view || !state->layer) {
      return false_v;
    }
    [state->window setContentSize:NSMakeSize(width, height)];
    const NSRect framebuffer =
        [state->view convertRectToBacking:[state->view frame]];
    [state->layer setDrawableSize:framebuffer.size];
  }
  window->width = width;
  window->height = height;
  return true_v;
}

bool8_t vkr_window_resize_centered(VkrWindow *window, uint32_t width,
                                   uint32_t height) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");
  if (width == 0 || height == 0) {
    return false_v;
  }

  PlatformState *state = (PlatformState *)window->platform_state;
  @autoreleasepool {
    if (!state->window || !state->view || !state->layer) {
      return false_v;
    }

    NSScreen *screen = [state->window screen];
    if (!screen) {
      screen = [NSScreen mainScreen];
    }
    if (!screen) {
      return false_v;
    }

    const NSRect area = [screen visibleFrame];
    NSRect frame = [state->window
        frameRectForContentRect:NSMakeRect(0.0, 0.0, width, height)];
    frame.size.width = Min(frame.size.width, area.size.width);
    frame.size.height = Min(frame.size.height, area.size.height);
    frame.origin.x = area.origin.x + (area.size.width - frame.size.width) * 0.5;
    frame.origin.y =
        area.origin.y + (area.size.height - frame.size.height) * 0.5;
    [state->window setFrame:frame display:YES];

    const NSRect content = [state->window contentRectForFrameRect:frame];
    const NSRect framebuffer =
        [state->view convertRectToBacking:[state->view frame]];
    [state->layer setDrawableSize:framebuffer.size];
    window->width = (uint32_t)content.size.width;
    window->height = (uint32_t)content.size.height;
  }
  return true_v;
}

/* macOS keeps the native traffic lights inside the unified title bar. */
bool8_t vkr_window_draws_caption_buttons(const VkrWindow *window) {
  (void)window;
  return false_v;
}

void vkr_window_minimize(VkrWindow *window) {
  PlatformState *state =
      window ? (PlatformState *)window->platform_state : NULL;
  if (state && state->window)
    [state->window miniaturize:nil];
}

void vkr_window_toggle_maximize(VkrWindow *window) {
  PlatformState *state =
      window ? (PlatformState *)window->platform_state : NULL;
  if (state && state->window)
    [state->window zoom:nil];
}

bool8_t vkr_window_is_maximized(const VkrWindow *window) {
  const PlatformState *state =
      window ? (const PlatformState *)window->platform_state : NULL;
  return state && state->window && [state->window isZoomed];
}

void vkr_window_request_close(VkrWindow *window) {
  PlatformState *state =
      window ? (PlatformState *)window->platform_state : NULL;
  if (state && state->window)
    [state->window performClose:nil];
}

void *vkr_window_get_cocoa_handle(VkrWindow *window) {
  if (!window || !window->platform_state) {
    return NULL;
  }
  PlatformState *state = (PlatformState *)window->platform_state;
  return state->window;
}

void *vkr_window_get_metal_layer(VkrWindow *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;
  return state->layer;
}

void vkr_window_set_mouse_capture(VkrWindow *window, bool8_t capture) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;

  // Capture is a state transition. Modal UI may request release every frame;
  // only an actual transition restores the cursor saved on capture entry.
  capture = capture ? true_v : false_v;
  if (state->mouse_captured == capture) {
    return;
  }

  @autoreleasepool {
    if (capture) {
      state->mouse_captured = true_v;

      const NSPoint pos = [state->window mouseLocationOutsideOfEventStream];

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

      CGAssociateMouseAndMouseCursorPosition(false);

      update_cursor_image(state);
    } else {
      state->mouse_captured = false_v;

      CGAssociateMouseAndMouseCursorPosition(true);

      // Restore cursor position directly without recursion
      const NSRect contentRect = [state->view frame];

      // Clamp restore position to window bounds
      // Note: restore coordinates are stored in bottom-left origin system
      float64_t clamped_x = vkr_max_f64(
          0.0, vkr_min_f64(state->restore_cursor_x, contentRect.size.width));
      float64_t clamped_y = vkr_max_f64(
          0.0, vkr_min_f64(state->restore_cursor_y, contentRect.size.height));

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

bool8_t vkr_window_is_mouse_captured(VkrWindow *window) {
  assert_log(window != NULL, "Window not initialized");
  assert_log(window->platform_state != NULL, "Platform state not initialized");

  PlatformState *state = (PlatformState *)window->platform_state;
  return state->mouse_captured;
}

void vkr_window_set_mouse_position(VkrWindow *window, int32_t x, int32_t y) {
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

static NSCursor *vkr_window_native_cursor(VkrWindowCursor cursor) {
  switch (cursor) {
  case VKR_WINDOW_CURSOR_IBEAM:
    return [NSCursor IBeamCursor];
  case VKR_WINDOW_CURSOR_RESIZE_EW:
    return [NSCursor resizeLeftRightCursor];
  case VKR_WINDOW_CURSOR_RESIZE_NS:
    return [NSCursor resizeUpDownCursor];
  case VKR_WINDOW_CURSOR_HAND:
    return [NSCursor pointingHandCursor];
  case VKR_WINDOW_CURSOR_GRAB:
    return [NSCursor openHandCursor];
  case VKR_WINDOW_CURSOR_GRABBING:
    return [NSCursor closedHandCursor];
  case VKR_WINDOW_CURSOR_CROSSHAIR:
    return [NSCursor crosshairCursor];
  case VKR_WINDOW_CURSOR_NOT_ALLOWED:
    return [NSCursor operationNotAllowedCursor];
  default:
    return [NSCursor arrowCursor];
  }
}

void update_cursor_image(PlatformState *state) {
  if (state->mouse_captured) {
    hide_cursor(state);
  } else {
    show_cursor(state);
    [vkr_window_native_cursor(state->cursor) set];
  }
}

void vkr_window_set_cursor(VkrWindow *window, VkrWindowCursor cursor) {
  if (!window || !window->platform_state || cursor >= VKR_WINDOW_CURSOR_COUNT)
    return;
  PlatformState *state = (PlatformState *)window->platform_state;
  if (state->cursor == cursor)
    return;
  state->cursor = cursor;
  if (!state->mouse_captured)
    [vkr_window_native_cursor(cursor) set];
}

void vkr_window_set_title_drag_region(VkrWindow *window, int32_t x, int32_t y,
                                      int32_t width, int32_t height,
                                      bool8_t allowed) {
  if (!window || !window->platform_state || !window->unified_title_bar)
    return;
  PlatformState *state = (PlatformState *)window->platform_state;
  state->drag_x = x;
  state->drag_y = y;
  state->drag_width = Max(0, width);
  state->drag_height = Max(0, height);
  state->drag_allowed = allowed;
}

float32_t vkr_window_title_bar_inset(const VkrWindow *window) {
  if (!window || !window->platform_state || !window->unified_title_bar)
    return 0.0f;
  const PlatformState *state = (const PlatformState *)window->platform_state;
  NSButton *zoom = [state->window standardWindowButton:NSWindowZoomButton];
  if (!zoom)
    return 0.0f;
  const NSRect frame = [zoom convertRect:[zoom bounds] toView:nil];
  return (float32_t)(frame.origin.x + frame.size.width) + 14.0f;
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

/*
 * macOS virtual keycodes (kVK_*) to VKR keys. Top-row digits share the numpad
 * keys, as on Windows. Unlisted codes, including apostrophe, backslash, equal,
 * the brackets, section, menu and the page keys, map to KEY_MAX_KEYS; `Keys`
 * has no zero value, so zero marks an unlisted code.
 */
static const Keys s_macos_keys[128] = {
    [0x1D] = KEY_NUMPAD0,
    [0x12] = KEY_NUMPAD1,
    [0x13] = KEY_NUMPAD2,
    [0x14] = KEY_NUMPAD3,
    [0x15] = KEY_NUMPAD4,
    [0x17] = KEY_NUMPAD5,
    [0x16] = KEY_NUMPAD6,
    [0x1A] = KEY_NUMPAD7,
    [0x1C] = KEY_NUMPAD8,
    [0x19] = KEY_NUMPAD9,

    [0x00] = KEY_A,
    [0x0B] = KEY_B,
    [0x08] = KEY_C,
    [0x02] = KEY_D,
    [0x0E] = KEY_E,
    [0x03] = KEY_F,
    [0x05] = KEY_G,
    [0x04] = KEY_H,
    [0x22] = KEY_I,
    [0x26] = KEY_J,
    [0x28] = KEY_K,
    [0x25] = KEY_L,
    [0x2E] = KEY_M,
    [0x2D] = KEY_N,
    [0x1F] = KEY_O,
    [0x23] = KEY_P,
    [0x0C] = KEY_Q,
    [0x0F] = KEY_R,
    [0x01] = KEY_S,
    [0x11] = KEY_T,
    [0x20] = KEY_U,
    [0x09] = KEY_V,
    [0x0D] = KEY_W,
    [0x07] = KEY_X,
    [0x10] = KEY_Y,
    [0x06] = KEY_Z,

    [0x2B] = KEY_COMMA,
    [0x32] = KEY_GRAVE,
    [0x1B] = KEY_MINUS,
    [0x2F] = KEY_PERIOD,
    [0x29] = KEY_SEMICOLON,
    [0x2C] = KEY_SLASH,

    [0x33] = KEY_BACKSPACE,
    [0x39] = KEY_CAPITAL,
    [0x75] = KEY_DELETE,
    [0x7D] = KEY_DOWN,
    [0x77] = KEY_END,
    [0x24] = KEY_ENTER,
    [0x35] = KEY_ESCAPE,
    [0x7A] = KEY_F1,
    [0x78] = KEY_F2,
    [0x63] = KEY_F3,
    [0x76] = KEY_F4,
    [0x60] = KEY_F5,
    [0x61] = KEY_F6,
    [0x62] = KEY_F7,
    [0x64] = KEY_F8,
    [0x65] = KEY_F9,
    [0x6D] = KEY_F10,
    [0x67] = KEY_F11,
    [0x6F] = KEY_F12,
    [0x69] = KEY_PRINT,
    [0x6B] = KEY_F14,
    [0x71] = KEY_F15,
    [0x6A] = KEY_F16,
    [0x40] = KEY_F17,
    [0x4F] = KEY_F18,
    [0x50] = KEY_F19,
    [0x5A] = KEY_F20,
    [0x73] = KEY_HOME,
    [0x72] = KEY_INSERT,
    [0x7B] = KEY_LEFT,
    [0x3A] = KEY_LMENU,
    [0x3B] = KEY_LCONTROL,
    [0x38] = KEY_LSHIFT,
    [0x37] = KEY_LWIN,
    [0x47] = KEY_NUMLOCK,
    [0x7C] = KEY_RIGHT,
    [0x3D] = KEY_RMENU,
    [0x3E] = KEY_RCONTROL,
    [0x3C] = KEY_RSHIFT,
    [0x36] = KEY_RWIN,
    [0x31] = KEY_SPACE,
    [0x30] = KEY_TAB,
    [0x7E] = KEY_UP,

    [0x52] = KEY_NUMPAD0,
    [0x53] = KEY_NUMPAD1,
    [0x54] = KEY_NUMPAD2,
    [0x55] = KEY_NUMPAD3,
    [0x56] = KEY_NUMPAD4,
    [0x57] = KEY_NUMPAD5,
    [0x58] = KEY_NUMPAD6,
    [0x59] = KEY_NUMPAD7,
    [0x5B] = KEY_NUMPAD8,
    [0x5C] = KEY_NUMPAD9,
    [0x45] = KEY_ADD,
    [0x41] = KEY_DECIMAL,
    [0x4B] = KEY_DIVIDE,
    [0x4C] = KEY_ENTER,
    [0x51] = KEY_NUMPAD_EQUAL,
    [0x43] = KEY_MULTIPLY,
    [0x4E] = KEY_SUBTRACT,
};

static Keys translate_keycode(uint32_t ns_keycode) {
  if (ns_keycode >= ArrayCount(s_macos_keys) || s_macos_keys[ns_keycode] == 0) {
    return KEY_MAX_KEYS;
  }
  return s_macos_keys[ns_keycode];
}
#endif

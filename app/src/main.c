#include "event.h"
#include "input.h"
#include "logger.h"
#include "window.h"

static bool8_t event_callback_window_resize(Event *event) {
  WindowResizeEventData *data = (WindowResizeEventData *)event->data;
  log_info("Window resized to %d x %d", data->width, data->height);
  return true_v;
}

static bool8_t event_callback_key_press(Event *event) {
  KeyEventData *data = (KeyEventData *)event->data;
  log_info("Key pressed: %d", data->key);
  return true_v;
}

int main(int argc, char **argv) {
  Arena *log_arena = arena_create(MB(1), MB(1));
  log_init(log_arena);

  Arena *arena = arena_create(MB(1), MB(1));
  EventManager event_manager = {0};
  event_manager_create(arena, &event_manager);

  Window window = {0};
  bool8_t result = window_create(&window, &event_manager, "Hello, World!", 100,
                                 100, 800, 600);
  assert_log(result, "Failed to create window");

  event_manager_subscribe(&event_manager, EVENT_TYPE_WINDOW_RESIZE,
                          event_callback_window_resize);
  event_manager_subscribe(&event_manager, EVENT_TYPE_KEY_PRESS,
                          event_callback_key_press);
  bool8_t running = true_v;
  while (running) {
    running = window_update(&window);
    input_update(&window.input_state, 0);
  }

  window_destroy(&window);
  event_manager_destroy(&event_manager);
  arena_destroy(arena);
  arena_destroy(log_arena);
  return 0;
}
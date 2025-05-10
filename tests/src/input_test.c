#include "input_test.h"

static Arena *arena = NULL;
static const uint64_t ARENA_SIZE = MB(1);

// Setup function called before each test function in this suite
static void setup_suite(void) { arena = arena_create(ARENA_SIZE, ARENA_SIZE); }

// Teardown function called after each test function in this suite
static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
  }
}

static bool8_t input_initialized = false;
static bool8_t on_input_system_init(Event *event) {
  input_initialized = true;
  return true;
}

static bool8_t on_input_system_shutdown(Event *event) {
  input_initialized = false;
  return true;
}

// --- Structs and variables for detailed event testing ---
static bool8_t key_event_received = false;
static KeyEventData last_key_event_data;

static bool8_t button_event_received = false;
static ButtonEventData last_button_event_data;

static bool8_t mouse_move_event_received = false;
static MouseMoveEventData last_mouse_move_event_data;

static bool8_t mouse_wheel_event_received = false;
static MouseWheelEventData last_mouse_wheel_event_data;

// --- Helper event handlers for detailed event testing ---
static bool8_t on_key_event(Event *event) {
  key_event_received = true;
  last_key_event_data = *(KeyEventData *)event->data;
  return true;
}

static bool8_t on_button_event(Event *event) {
  button_event_received = true;
  last_button_event_data = *(ButtonEventData *)event->data;
  return true;
}

static bool8_t on_mouse_move_event(Event *event) {
  mouse_move_event_received = true;
  last_mouse_move_event_data = *(MouseMoveEventData *)event->data;
  return true;
}

static bool8_t on_mouse_wheel_event(Event *event) {
  mouse_wheel_event_received = true;
  last_mouse_wheel_event_data = *(MouseWheelEventData *)event->data;
  return true;
}

// Dummy handler for EVENT_TYPE_INPUT_SYSTEM_INIT to diagnose potential issue
static bool8_t dummy_input_init_handler(Event *event) {
  (void)event; // Mark as unused
  return true;
}

// --- Helper function to reset event tracking state ---
static void reset_event_trackers() {
  key_event_received = false;
  MemZero(&last_key_event_data, sizeof(KeyEventData));
  button_event_received = false;
  MemZero(&last_button_event_data, sizeof(ButtonEventData));
  mouse_move_event_received = false;
  MemZero(&last_mouse_move_event_data, sizeof(MouseMoveEventData));
  mouse_wheel_event_received = false;
  MemZero(&last_mouse_wheel_event_data, sizeof(MouseWheelEventData));
}

static void test_input_init() {
  printf("  Running test_input_init...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);
  event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_INIT,
                          on_input_system_init);
  event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN,
                          on_input_system_shutdown);
  InputState input_state = input_init(&manager);

  platform_sleep(100);

  assert(input_initialized == true && "Input system not initialized");

  input_shutdown(&input_state);
  event_manager_destroy(&manager);

  teardown_suite();
  printf("  test_input_init PASSED\n");
}

static void test_input_shutdown() {
  printf("  Running test_input_shutdown...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);
  event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_INIT,
                          on_input_system_init);
  event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN,
                          on_input_system_shutdown);
  InputState input_state = input_init(&manager);
  input_shutdown(&input_state);

  platform_sleep(100);

  assert(input_initialized == false && "Input system was not shutdown");

  event_manager_destroy(&manager);

  teardown_suite();
  printf("  test_input_shutdown PASSED\n");
}

static void test_input_key_press_release() {
  printf("  Running test_input_key_press_release...\n");
  setup_suite();
  reset_event_trackers();

  EventManager manager;
  event_manager_create(arena, &manager);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, on_key_event);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_RELEASE, on_key_event);
  // Subscribe dummy handler for INPUT_SYSTEM_INIT for this test context
  event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_INIT,
                          dummy_input_init_handler);

  InputState input_state = input_init(&manager);

  // Test KEY_A press
  input_process_key(&input_state, KEY_A, true);
  platform_sleep(100);

  assert(input_is_key_down(&input_state, KEY_A) && "KEY_A should be down");
  assert(input_is_key_up(&input_state, KEY_A) == false &&
         "KEY_A should not be up");
  assert(key_event_received && "Key press event not received");
  assert(last_key_event_data.key == KEY_A && "Incorrect key in press event");
  assert(last_key_event_data.pressed == true &&
         "Incorrect state in press event");
  reset_event_trackers();

  input_update(&input_state, 0.0); // Simulate a frame update
  assert(input_was_key_down(&input_state, KEY_A) &&
         "KEY_A should have been down previously");
  assert(input_was_key_up(&input_state, KEY_A) == false &&
         "KEY_A should not have been up previously");

  // Test KEY_A release
  input_process_key(&input_state, KEY_A, false);
  platform_sleep(100);

  assert(input_is_key_down(&input_state, KEY_A) == false &&
         "KEY_A should not be down");
  assert(input_is_key_up(&input_state, KEY_A) && "KEY_A should be up");
  assert(key_event_received && "Key release event not received");
  assert(last_key_event_data.key == KEY_A && "Incorrect key in release event");
  assert(last_key_event_data.pressed == false &&
         "Incorrect state in release event");
  reset_event_trackers();

  input_update(&input_state, 0.0);
  assert(input_was_key_down(&input_state, KEY_A) == false &&
         "KEY_A should not have been down previously after release");
  assert(input_was_key_up(&input_state, KEY_A) &&
         "KEY_A should have been up previously after release");

  // Test no event if state doesn't change
  input_process_key(&input_state, KEY_A, false); // Already released
  platform_sleep(100);
  assert(key_event_received == false &&
         "Event received when state did not change");

  input_shutdown(&input_state);
  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_input_key_press_release PASSED\n");
}

static void test_input_button_press_release() {
  printf("  Running test_input_button_press_release...\n");
  setup_suite();
  reset_event_trackers();

  EventManager manager;
  event_manager_create(arena, &manager);
  event_manager_subscribe(&manager, EVENT_TYPE_BUTTON_PRESS, on_button_event);
  event_manager_subscribe(&manager, EVENT_TYPE_BUTTON_RELEASE, on_button_event);
  InputState input_state = input_init(&manager);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000}; // 100ms

  // Test BUTTON_LEFT press
  input_process_button(&input_state, BUTTON_LEFT, true);
  platform_sleep(100); // Allow time for event processing

  assert(input_is_button_down(&input_state, BUTTON_LEFT) &&
         "BUTTON_LEFT should be down");
  assert(input_is_button_up(&input_state, BUTTON_LEFT) == false &&
         "BUTTON_LEFT should not be up");
  assert(button_event_received && "Button press event not received");
  assert(last_button_event_data.button == BUTTON_LEFT &&
         "Incorrect button in press event");
  assert(last_button_event_data.pressed == true &&
         "Incorrect state in press event");
  reset_event_trackers();

  input_update(&input_state, 0.0); // Simulate a frame update
  assert(input_was_button_down(&input_state, BUTTON_LEFT) &&
         "BUTTON_LEFT should have been down previously");
  assert(input_was_button_up(&input_state, BUTTON_LEFT) == false &&
         "BUTTON_LEFT should not have been up previously");

  // Test BUTTON_LEFT release
  input_process_button(&input_state, BUTTON_LEFT, false);
  platform_sleep(100); // Allow time for event processing

  assert(input_is_button_down(&input_state, BUTTON_LEFT) == false &&
         "BUTTON_LEFT should not be down");
  assert(input_is_button_up(&input_state, BUTTON_LEFT) &&
         "BUTTON_LEFT should be up");
  assert(button_event_received && "Button release event not received");
  assert(last_button_event_data.button == BUTTON_LEFT &&
         "Incorrect button in release event");
  assert(last_button_event_data.pressed == false &&
         "Incorrect state in release event");
  reset_event_trackers();

  input_update(&input_state, 0.0);
  assert(input_was_button_down(&input_state, BUTTON_LEFT) == false &&
         "BUTTON_LEFT should not have been down previously after release");
  assert(input_was_button_up(&input_state, BUTTON_LEFT) &&
         "BUTTON_LEFT should have been up previously after release");

  // Test no event if state doesn't change
  input_process_button(&input_state, BUTTON_LEFT, false); // Already released
  platform_sleep(
      100); // Allow time for any potential (erroneous) event processing
  assert(button_event_received == false &&
         "Event received when button state did not change");

  input_shutdown(&input_state);
  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_input_button_press_release PASSED\n");
}

static void test_input_mouse_move() {
  printf("  Running test_input_mouse_move...\n");
  setup_suite();
  reset_event_trackers();

  EventManager manager;
  event_manager_create(arena, &manager);
  event_manager_subscribe(&manager, EVENT_TYPE_MOUSE_MOVE, on_mouse_move_event);
  InputState input_state = input_init(&manager);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000}; // 100ms

  int32_t current_x, current_y;
  int32_t prev_x, prev_y;

  // Initial move
  input_process_mouse_move(&input_state, 100, 200);
  platform_sleep(100); // Allow time for event processing

  input_get_mouse_position(&input_state, &current_x, &current_y);
  assert(current_x == 100 && current_y == 200 &&
         "Mouse position not updated correctly");
  assert(mouse_move_event_received && "Mouse move event not received");
  assert(last_mouse_move_event_data.x == 100 &&
         last_mouse_move_event_data.y == 200 &&
         "Incorrect data in mouse move event");
  reset_event_trackers();

  input_update(&input_state, 0.0);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(prev_x == 100 && prev_y == 200 &&
         "Previous mouse position not updated correctly after update");

  // Second move
  input_process_mouse_move(&input_state, -50, 75);
  platform_sleep(100); // Allow time for event processing

  input_get_mouse_position(&input_state, &current_x, &current_y);
  assert(current_x == -50 && current_y == 75 &&
         "Mouse position not updated correctly on second move");
  assert(mouse_move_event_received &&
         "Mouse move event not received on second move");
  assert(last_mouse_move_event_data.x == -50 &&
         last_mouse_move_event_data.y == 75 &&
         "Incorrect data in second mouse move event");
  reset_event_trackers();

  input_update(&input_state, 0.0);
  input_get_mouse_position(&input_state, &current_x, &current_y);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(current_x == -50 && current_y == 75 &&
         "Current mouse position incorrect after second update");
  assert(prev_x == -50 && prev_y == 75 &&
         "Previous mouse position not updated correctly after second move and "
         "update");

  // No event if position doesn't change
  input_process_mouse_move(&input_state, -50, 75); // Same position
  platform_sleep(
      100); // Allow time for any potential (erroneous) event processing
  assert(mouse_move_event_received == false &&
         "Mouse move event received when position did not change");

  input_shutdown(&input_state);
  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_input_mouse_move PASSED\n");
}

static void test_input_mouse_wheel() {
  printf("  Running test_input_mouse_wheel...\n");
  setup_suite();
  reset_event_trackers();

  EventManager manager;
  event_manager_create(arena, &manager);
  event_manager_subscribe(&manager, EVENT_TYPE_MOUSE_WHEEL,
                          on_mouse_wheel_event);
  InputState input_state = input_init(&manager);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000}; // 100ms

  int8_t current_delta;

  // Initial wheel movement (scroll up)
  input_process_mouse_wheel(&input_state, 1);
  platform_sleep(100); // Allow time for event processing

  input_get_mouse_wheel(
      &input_state,
      &current_delta); // Although this gets current, the event is key
  assert(current_delta == 1 && "Mouse wheel delta not updated correctly");
  assert(mouse_wheel_event_received && "Mouse wheel event not received");
  assert(last_mouse_wheel_event_data.delta == 1 &&
         "Incorrect data in mouse wheel event");
  reset_event_trackers();

  // Subsequent wheel movement (scroll down)
  input_process_mouse_wheel(&input_state, -1);
  platform_sleep(100); // Allow time for event processing

  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == -1 &&
         "Mouse wheel delta not updated correctly on second scroll");
  assert(mouse_wheel_event_received &&
         "Mouse wheel event not received on second scroll");
  assert(last_mouse_wheel_event_data.delta == -1 &&
         "Incorrect data in second mouse wheel event");
  reset_event_trackers();

  // No event if wheel delta is the same (though input_process_mouse_wheel will
  // always fire if delta != current_buttons.wheel) So, let's ensure that if we
  // process the *same* delta again, it still fires, as current_buttons.wheel is
  // updated. This is different from keys/buttons/mouse_move where state is
  // compared before firing. However, if we call it with 0 after it was
  // non-zero, that should be a change.

  input_process_mouse_wheel(&input_state, 0); // Reset wheel to 0
  platform_sleep(100);                        // Allow time for event processing

  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == 0 && "Mouse wheel delta not reset to 0");
  assert(mouse_wheel_event_received &&
         "Mouse wheel event for 0 delta not received");
  assert(last_mouse_wheel_event_data.delta == 0 &&
         "Incorrect data for 0 delta event");
  reset_event_trackers();

  // Test no event if delta is already 0 and we process 0 again
  input_process_mouse_wheel(&input_state, 0);
  platform_sleep(
      100); // Allow time for any potential (erroneous) event processing
  assert(mouse_wheel_event_received == false &&
         "Mouse wheel event received when delta did not change from 0");

  input_shutdown(&input_state);
  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_input_mouse_wheel PASSED\n");
}

static void test_input_update_state_copy() {
  printf("  Running test_input_update_state_copy...\n");
  setup_suite();

  // Correctly initialize with an EventManager
  EventManager manager;
  event_manager_create(arena, &manager);
  InputState input_state = input_init(&manager);

  // 1. Test Key State Copy
  // Set initial current key state
  input_process_key(&input_state, KEY_W, true);
  input_process_key(&input_state, KEY_S,
                    false); // Assuming S was false initially

  // Current state: W=down, S=up. Previous state: (initially all up/false)
  assert(input_is_key_down(&input_state, KEY_W) &&
         "Initial: KEY_W should be down");
  assert(input_is_key_up(&input_state, KEY_S) && "Initial: KEY_S should be up");
  assert(input_was_key_up(&input_state, KEY_W) &&
         "Initial: KEY_W should have been up previously");
  assert(input_was_key_up(&input_state, KEY_S) &&
         "Initial: KEY_S should have been up previously");

  input_update(&input_state, 0.0);

  // After update: Previous state should now match the last current state
  // W was down, S was up.
  assert(input_was_key_down(&input_state, KEY_W) &&
         "After Update: KEY_W should have been down");
  assert(input_was_key_up(&input_state, KEY_S) &&
         "After Update: KEY_S should have been up");

  // Change current state again
  input_process_key(&input_state, KEY_W, false);
  input_process_key(&input_state, KEY_S, true);

  // Current state: W=up, S=down. Previous state: W=down, S=up (from last
  // update)
  assert(input_is_key_up(&input_state, KEY_W) &&
         "New Current: KEY_W should be up");
  assert(input_is_key_down(&input_state, KEY_S) &&
         "New Current: KEY_S should be down");
  assert(input_was_key_down(&input_state, KEY_W) &&
         "New Current: KEY_W should still show previous as down");
  assert(input_was_key_up(&input_state, KEY_S) &&
         "New Current: KEY_S should still show previous as up");

  input_update(&input_state, 0.0);

  // After second update: Previous state should match the new current state
  // W was up, S was down.
  assert(input_was_key_up(&input_state, KEY_W) &&
         "After 2nd Update: KEY_W should have been up");
  assert(input_was_key_down(&input_state, KEY_S) &&
         "After 2nd Update: KEY_S should have been down");

  // 2. Test Button State Copy (similar logic)
  // Set initial current button state
  input_process_button(&input_state, BUTTON_LEFT, true);
  input_process_button(&input_state, BUTTON_RIGHT, false);

  assert(input_is_button_down(&input_state, BUTTON_LEFT) &&
         "Initial: BUTTON_LEFT should be down");
  assert(input_is_button_up(&input_state, BUTTON_RIGHT) &&
         "Initial: BUTTON_RIGHT should be up");
  assert(input_was_button_up(&input_state, BUTTON_LEFT) &&
         "Initial: BUTTON_LEFT should have been up previously");
  assert(input_was_button_up(&input_state, BUTTON_RIGHT) &&
         "Initial: BUTTON_RIGHT should have been up previously");

  input_update(&input_state, 0.0);

  assert(input_was_button_down(&input_state, BUTTON_LEFT) &&
         "After Update: BUTTON_LEFT should have been down");
  assert(input_was_button_up(&input_state, BUTTON_RIGHT) &&
         "After Update: BUTTON_RIGHT should have been up");

  // 3. Test Mouse Position Copy
  int32_t prev_x, prev_y;
  input_process_mouse_move(&input_state, 10, 20);
  // Previous position is 0,0 initially or from last frame if it was running.
  // For this test, it's okay that it might be 0,0 or what it was from key
  // tests. The critical part is that after update, prev == current of that
  // frame.

  input_update(&input_state, 0.0);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(prev_x == 10 && prev_y == 20 &&
         "Mouse position not copied to previous correctly");

  input_process_mouse_move(&input_state, 30, 40);
  input_update(&input_state, 0.0);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(prev_x == 30 && prev_y == 40 &&
         "Mouse position not copied to previous correctly on second update");

  input_shutdown(&input_state);
  event_manager_destroy(&manager); // Clean up the manager

  teardown_suite();
  printf("  test_input_update_state_copy PASSED\n");
}

bool32_t run_input_tests() {
  printf("--- Running Input System tests... ---\n");
  test_input_init();
  test_input_shutdown();
  test_input_key_press_release();
  test_input_button_press_release();
  test_input_mouse_move();
  test_input_mouse_wheel();
  test_input_update_state_copy();
  printf("--- Input System tests completed. ---\n");
  return true;
}

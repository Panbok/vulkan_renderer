#include "input_test.h"
#include "event_test_wait.h"

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
static bool8_t on_input_system_init(Event *event, UserData user_data) {
  input_initialized = true;
  return true;
}

static bool8_t on_input_system_shutdown(Event *event, UserData user_data) {
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
static uint32_t mouse_wheel_event_count;
static MouseWheelEventData last_mouse_wheel_event_data;

// --- Helper event handlers for detailed event testing ---
static bool8_t on_key_event(Event *event, UserData user_data) {
  key_event_received = true;
  last_key_event_data = *(KeyEventData *)event->data;
  return true;
}

static bool8_t on_button_event(Event *event, UserData user_data) {
  button_event_received = true;
  last_button_event_data = *(ButtonEventData *)event->data;
  return true;
}

static bool8_t on_mouse_move_event(Event *event, UserData user_data) {
  mouse_move_event_received = true;
  last_mouse_move_event_data = *(MouseMoveEventData *)event->data;
  return true;
}

static bool8_t on_mouse_wheel_event(Event *event, UserData user_data) {
  mouse_wheel_event_received = true;
  mouse_wheel_event_count++;
  last_mouse_wheel_event_data = *(MouseWheelEventData *)event->data;
  return true;
}

// Dummy handler for EVENT_TYPE_INPUT_SYSTEM_INIT to diagnose potential issue
static bool8_t dummy_input_init_handler(Event *event, UserData user_data) {
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
  mouse_wheel_event_count = 0;
  MemZero(&last_mouse_wheel_event_data, sizeof(MouseWheelEventData));
}

static void test_input_init() {
  printf("  Running test_input_init...\n");
  setup_suite();

  EventManager manager;
  assert(event_manager_create(&manager));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_INIT,
                                 on_input_system_init, NULL));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN,
                                 on_input_system_shutdown, NULL));
  InputState input_state = input_init(&manager);

  event_test_wait_idle(&manager);

  assert(input_initialized == true && "Input system not initialized");
  assert(input_state.is_initialized == true && "Input state not initialized");

  input_shutdown(&input_state);
  event_manager_destroy(&manager);

  teardown_suite();
  printf("  test_input_init PASSED\n");
}

static void test_input_shutdown() {
  printf("  Running test_input_shutdown...\n");
  setup_suite();

  EventManager manager;
  assert(event_manager_create(&manager));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_INIT,
                                 on_input_system_init, NULL));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN,
                                 on_input_system_shutdown, NULL));
  InputState input_state = input_init(&manager);
  input_shutdown(&input_state);

  event_test_wait_idle(&manager);

  assert(input_initialized == false && "Input system was not shutdown");
  assert(input_state.is_initialized == false && "Input state not shutdown");

  event_manager_destroy(&manager);

  teardown_suite();
  printf("  test_input_shutdown PASSED\n");
}

static void test_input_key_press_release() {
  printf("  Running test_input_key_press_release...\n");
  setup_suite();
  reset_event_trackers();

  EventManager manager;
  assert(event_manager_create(&manager));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, on_key_event,
                                 NULL));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_KEY_RELEASE, on_key_event,
                                 NULL));
  // Subscribe dummy handler for INPUT_SYSTEM_INIT for this test context
  assert(event_manager_subscribe(&manager, EVENT_TYPE_INPUT_SYSTEM_INIT,
                                 dummy_input_init_handler, NULL));

  InputState input_state = input_init(&manager);

  // Test KEY_A press
  input_process_key(&input_state, KEY_A, true);
  event_test_wait_idle(&manager);

  assert(input_is_key_down(&input_state, KEY_A) && "KEY_A should be down");
  assert(input_is_key_up(&input_state, KEY_A) == false &&
         "KEY_A should not be up");
  assert(key_event_received && "Key press event not received");
  assert(last_key_event_data.key == KEY_A && "Incorrect key in press event");
  assert(last_key_event_data.pressed == true &&
         "Incorrect state in press event");
  reset_event_trackers();

  input_update(&input_state); // Simulate a frame update
  assert(input_was_key_down(&input_state, KEY_A) &&
         "KEY_A should have been down previously");
  assert(input_was_key_up(&input_state, KEY_A) == false &&
         "KEY_A should not have been up previously");

  // Test KEY_A release
  input_process_key(&input_state, KEY_A, false);
  event_test_wait_idle(&manager);

  assert(input_is_key_down(&input_state, KEY_A) == false &&
         "KEY_A should not be down");
  assert(input_is_key_up(&input_state, KEY_A) && "KEY_A should be up");
  assert(key_event_received && "Key release event not received");
  assert(last_key_event_data.key == KEY_A && "Incorrect key in release event");
  assert(last_key_event_data.pressed == false &&
         "Incorrect state in release event");
  reset_event_trackers();

  input_update(&input_state);
  assert(input_was_key_down(&input_state, KEY_A) == false &&
         "KEY_A should not have been down previously after release");
  assert(input_was_key_up(&input_state, KEY_A) &&
         "KEY_A should have been up previously after release");

  // Test no event if state doesn't change
  input_process_key(&input_state, KEY_A, false); // Already released
  event_test_wait_idle(&manager);
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
  assert(event_manager_create(&manager));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_BUTTON_PRESS,
                                 on_button_event, NULL));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_BUTTON_RELEASE,
                                 on_button_event, NULL));
  InputState input_state = input_init(&manager);

  // Test BUTTON_LEFT press
  input_process_button(&input_state, BUTTON_LEFT, true);
  event_test_wait_idle(&manager);

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

  input_update(&input_state);
  assert(input_was_button_down(&input_state, BUTTON_LEFT) &&
         "BUTTON_LEFT should have been down previously");
  assert(input_was_button_up(&input_state, BUTTON_LEFT) == false &&
         "BUTTON_LEFT should not have been up previously");

  // Test BUTTON_LEFT release
  input_process_button(&input_state, BUTTON_LEFT, false);
  event_test_wait_idle(&manager);

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

  input_update(&input_state);
  assert(input_was_button_down(&input_state, BUTTON_LEFT) == false &&
         "BUTTON_LEFT should not have been down previously after release");
  assert(input_was_button_up(&input_state, BUTTON_LEFT) &&
         "BUTTON_LEFT should have been up previously after release");

  // Test no event if state doesn't change
  input_process_button(&input_state, BUTTON_LEFT, false); // Already released
  event_test_wait_idle(&manager);
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
  assert(event_manager_create(&manager));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_MOUSE_MOVE,
                                 on_mouse_move_event, NULL));
  InputState input_state = input_init(&manager);

  int32_t current_x, current_y;
  int32_t prev_x, prev_y;

  // Initial move
  input_process_mouse_move(&input_state, 100, 200);
  event_test_wait_idle(&manager);

  input_get_mouse_position(&input_state, &current_x, &current_y);
  assert(current_x == 100 && current_y == 200 &&
         "Mouse position not updated correctly");
  assert(mouse_move_event_received && "Mouse move event not received");
  assert(last_mouse_move_event_data.x == 100 &&
         last_mouse_move_event_data.y == 200 &&
         "Incorrect data in mouse move event");
  reset_event_trackers();

  input_update(&input_state);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(prev_x == 100 && prev_y == 200 &&
         "Previous mouse position not updated correctly after update");

  // Second move
  input_process_mouse_move(&input_state, -50, 75);
  event_test_wait_idle(&manager);

  input_get_mouse_position(&input_state, &current_x, &current_y);
  assert(current_x == -50 && current_y == 75 &&
         "Mouse position not updated correctly on second move");
  assert(mouse_move_event_received &&
         "Mouse move event not received on second move");
  assert(last_mouse_move_event_data.x == -50 &&
         last_mouse_move_event_data.y == 75 &&
         "Incorrect data in second mouse move event");
  reset_event_trackers();

  input_update(&input_state);
  input_get_mouse_position(&input_state, &current_x, &current_y);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(current_x == -50 && current_y == 75 &&
         "Current mouse position incorrect after second update");
  assert(prev_x == -50 && prev_y == 75 &&
         "Previous mouse position not updated correctly after second move and "
         "update");

  // No event if position doesn't change
  input_process_mouse_move(&input_state, -50, 75); // Same position
  event_test_wait_idle(&manager);
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
  assert(event_manager_create(&manager));
  assert(event_manager_subscribe(&manager, EVENT_TYPE_MOUSE_WHEEL,
                                 on_mouse_wheel_event, NULL));
  InputState input_state = input_init(&manager);
  int32_t current_delta;

  // Initial wheel movement (scroll up)
  input_process_mouse_wheel(&input_state, 1);
  event_test_wait_idle(&manager);

  input_get_mouse_wheel(
      &input_state,
      &current_delta); // Although this gets current, the event is key
  assert(current_delta == 1 && "Mouse wheel delta not updated correctly");
  assert(mouse_wheel_event_received && "Mouse wheel event not received");
  assert(last_mouse_wheel_event_data.delta == 1 &&
         "Incorrect data in mouse wheel event");
  reset_event_trackers();

  // Identical notches are separate movement and separate event notifications.
  input_process_mouse_wheel(&input_state, 1);
  input_process_mouse_wheel(&input_state, 1);
  event_test_wait_idle(&manager);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == 3 && mouse_wheel_event_count == 2);
  reset_event_trackers();

  // Opposite movement cancels only its own contribution within the frame.
  input_process_mouse_wheel(&input_state, -1);
  event_test_wait_idle(&manager);

  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == 2 &&
         "Mouse wheel delta not updated correctly on second scroll");
  assert(mouse_wheel_event_received &&
         "Mouse wheel event not received on second scroll");
  assert(last_mouse_wheel_event_data.delta == -1 &&
         "Incorrect data in second mouse wheel event");
  reset_event_trackers();

  // Zero is no movement, not a request to erase this frame's movement.
  input_process_mouse_wheel(&input_state, 0);
  event_test_wait_idle(&manager);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == 2 && !mouse_wheel_event_received);

  // End-of-frame retirement prevents idle frames from repeating a scroll.
  input_update(&input_state);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == 0 && input_state.previous_buttons.wheel == 2);
  input_process_mouse_wheel(&input_state, -1);
  input_process_mouse_wheel(&input_state, -1);
  input_process_mouse_wheel(&input_state, 1);
  event_test_wait_idle(&manager);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == -1 && mouse_wheel_event_count == 3);
  input_update(&input_state);
  input_update(&input_state);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == 0);

  // Per-event int8 deltas must not narrow the accumulated frame total.
  reset_event_trackers();
  for (uint32_t i = 0; i < 256; ++i) {
    input_process_mouse_wheel(&input_state, 1);
  }
  event_test_wait_idle(&manager);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == 256 && mouse_wheel_event_count == 256);

  input_state.current_buttons.wheel = INT32_MAX - 1;
  input_process_mouse_wheel(&input_state, 127);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == INT32_MAX);
  input_state.current_buttons.wheel = INT32_MIN + 1;
  input_process_mouse_wheel(&input_state, -128);
  input_get_mouse_wheel(&input_state, &current_delta);
  assert(current_delta == INT32_MIN);

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
  assert(event_manager_create(&manager));
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

  input_update(&input_state);

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

  input_update(&input_state);

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

  input_update(&input_state);

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

  input_update(&input_state);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(prev_x == 10 && prev_y == 20 &&
         "Mouse position not copied to previous correctly");

  input_process_mouse_move(&input_state, 30, 40);
  input_update(&input_state);
  input_get_previous_mouse_position(&input_state, &prev_x, &prev_y);
  assert(prev_x == 30 && prev_y == 40 &&
         "Mouse position not copied to previous correctly on second update");

  input_shutdown(&input_state);
  event_manager_destroy(&manager); // Clean up the manager

  teardown_suite();
  printf("  test_input_update_state_copy PASSED\n");
}

static void test_input_character_queue() {
  printf("  Running test_input_character_queue...\n");
  InputState input_state = {0};
  assert(input_process_char(&input_state, 'A'));
  assert(input_process_char(&input_state, 0x00e9u));
  assert(input_process_char(&input_state, 0x1f642u));
  assert(!input_process_char(&input_state, 0xd800u));
  assert(!input_process_char(&input_state, 0x110000u));

  uint32_t count = 0u;
  const uint32_t *characters = input_get_characters(&input_state, &count);
  assert(count == 3u);
  assert(characters[0] == 'A');
  assert(characters[1] == 0x00e9u);
  assert(characters[2] == 0x1f642u);

  for (uint32_t i = count; i < VKR_INPUT_CHARACTER_CAPACITY; ++i)
    assert(input_process_char(&input_state, (uint32_t)('a' + i % 26u)));
  assert(!input_process_char(&input_state, 'Z'));
  assert(input_get_dropped_character_count(&input_state) == 1u);

  input_update(&input_state);
  (void)input_get_characters(&input_state, &count);
  assert(count == 0u);
  assert(input_get_dropped_character_count(&input_state) == 0u);
  printf("  test_input_character_queue PASSED\n");
}

static void test_input_quick_edges_and_modifiers(void) {
  printf("  Running test_input_quick_edges_and_modifiers...\n");
  setup_suite();
  EventManager manager;
  assert(event_manager_create(&manager));
  InputState input = input_init(&manager);
  input_process_key(&input, KEY_LCONTROL, true_v);
  input_process_key(&input, KEY_LSHIFT, true_v);
  input_process_key(&input, KEY_Z, true_v);
  input_process_key(&input, KEY_Z, false_v);
  input_process_key(&input, KEY_LSHIFT, false_v);
  input_process_key(&input, KEY_LCONTROL, false_v);
  // A complete drag in one event pump retains its start independently of the
  // final cursor; a second press cannot overwrite that first edge's position.
  input_process_mouse_move(&input, 31, 47);
  input_process_button(&input, BUTTON_LEFT, true_v);
  input_process_mouse_move(&input, 281, 163);
  input_process_button(&input, BUTTON_LEFT, false_v);
  input_process_button(&input, BUTTON_RIGHT, true_v);
  input_process_button(&input, BUTTON_RIGHT, false_v);
  input_process_button(&input, BUTTON_LEFT, true_v);
  input_process_button(&input, BUTTON_LEFT, false_v);
  int32_t press_x, press_y;
  input_get_button_press_position(&input, BUTTON_LEFT, &press_x, &press_y);
  assert(press_x == 31 && press_y == 47);
  input_get_button_press_position(&input, BUTTON_RIGHT, &press_x, &press_y);
  assert(press_x == 281 && press_y == 163);
  input_get_mouse_position(&input, &press_x, &press_y);
  assert(press_x == 281 && press_y == 163);
  assert(!input_is_key_down(&input, KEY_Z));
  assert(input_key_just_pressed(&input, KEY_Z) &&
         input_key_just_released(&input, KEY_Z));
  assert(input_key_press_modifiers(&input, KEY_Z) ==
         (VKR_INPUT_MOD_CONTROL | VKR_INPUT_MOD_SHIFT));
  assert(!input_is_button_down(&input, BUTTON_LEFT));
  assert(input_button_just_pressed(&input, BUTTON_LEFT) &&
         input_button_just_released(&input, BUTTON_LEFT));
  input_update(&input);
  input_process_mouse_move(&input, 9, 17);
  input_get_button_press_position(&input, BUTTON_LEFT, &press_x, &press_y);
  assert(press_x == 9 && press_y == 17);
  assert(!input_key_just_pressed(&input, KEY_Z) &&
         !input_key_just_released(&input, KEY_Z));
  assert(input_key_press_modifiers(&input, KEY_Z) == 0);
  assert(!input_button_just_pressed(&input, BUTTON_LEFT) &&
         !input_button_just_released(&input, BUTTON_LEFT));
  // The modifier snapshot belongs to the key press, not a later modifier.
  input_process_key(&input, KEY_P, true_v);
  input_process_key(&input, KEY_P, false_v);
  input_process_key(&input, KEY_LCONTROL, true_v);
  assert(input_key_press_modifiers(&input, KEY_P) == 0);
  input_update(&input);
  assert(input_is_key_down(&input, KEY_LCONTROL));
  assert(!input_key_just_pressed(&input, KEY_LCONTROL));
  printf("  InputState size: %zu bytes\n", sizeof(InputState));
  input_shutdown(&input);
  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_input_quick_edges_and_modifiers PASSED\n");
}

typedef struct InputObservationTest {
  VkrInputTransition transitions[16];
  uint32_t count;
} InputObservationTest;

static void observe_input_transition(const VkrInputTransition *transition,
                                     void *context) {
  InputObservationTest *observations = context;
  assert(observations->count < ArrayCount(observations->transitions));
  observations->transitions[observations->count++] = *transition;
}

static void test_input_synchronous_observer(void) {
  printf("  Running test_input_synchronous_observer...\n");
  setup_suite();
  EventManager manager;
  assert(event_manager_create(&manager));
  InputState input = input_init(&manager);
  InputObservationTest observations = {0};
  InputObservationTest other = {0};
  assert(!input_observe(&input, NULL, &observations));
  assert(input_observe(&input, observe_input_transition, &observations));
  assert(!input_observe(&input, observe_input_transition, &other));

  input_process_key(&input, KEY_W, true_v);
  assert(observations.count == 1); // Delivery precedes return, without a frame.
  input_process_key(&input, KEY_W, true_v); // Repeat has no transition.
  assert(observations.count == 1);
  input_process_button(&input, BUTTON_LEFT, true_v);
  input_process_button(&input, BUTTON_LEFT, true_v);
  input_process_mouse_move(&input, 10, -5);
  input_process_mouse_move(&input, 10, -5);
  input_process_key(&input, KEY_W, false_v);
  input_process_key(&input, KEY_W, true_v);
  input_process_mouse_move(&input, 7, 4);
  input_process_button(&input, BUTTON_LEFT, false_v);
  assert(observations.count == 7);
  const VkrInputTransition *events = observations.transitions;
  assert(events[0].kind == VKR_INPUT_TRANSITION_KEY &&
         events[0].code == KEY_W && events[0].pressed);
  assert(events[1].kind == VKR_INPUT_TRANSITION_BUTTON &&
         events[1].code == BUTTON_LEFT && events[1].pressed);
  assert(events[2].kind == VKR_INPUT_TRANSITION_LOOK &&
         events[2].delta_x == 10 && events[2].delta_y == -5);
  assert(events[3].kind == VKR_INPUT_TRANSITION_KEY &&
         events[3].code == KEY_W && !events[3].pressed);
  assert(events[4].kind == VKR_INPUT_TRANSITION_KEY &&
         events[4].code == KEY_W && events[4].pressed);
  assert(events[5].kind == VKR_INPUT_TRANSITION_LOOK &&
         events[5].delta_x == -3 && events[5].delta_y == 9);
  assert(events[6].kind == VKR_INPUT_TRANSITION_BUTTON &&
         events[6].code == BUTTON_LEFT && !events[6].pressed);
  assert(input_is_key_down(&input, KEY_W));
  assert(input_key_just_pressed(&input, KEY_W));
  assert(input_key_just_released(&input, KEY_W));
  assert(!input_is_button_down(&input, BUTTON_LEFT));
  assert(input_button_just_pressed(&input, BUTTON_LEFT));
  assert(input_button_just_released(&input, BUTTON_LEFT));
  input_update(&input);
  assert(observations.count == 7);
  assert(input_is_key_down(&input, KEY_W));
  assert(!input_key_just_pressed(&input, KEY_W));
  assert(!input_key_just_released(&input, KEY_W));
  assert(!input_button_just_pressed(&input, BUTTON_LEFT));
  assert(!input_button_just_released(&input, BUTTON_LEFT));

  assert(!input_unobserve(&input, &other));
  input_process_key(&input, KEY_W, false_v);
  assert(observations.count == 8 && !events[7].pressed);
  assert(input_unobserve(&input, &observations));
  input_process_key(&input, KEY_W, true_v);
  assert(observations.count == 8);
  assert(input_observe(&input, observe_input_transition, &other));
  input_process_key(&input, KEY_W, false_v);
  assert(other.count == 1 && observations.count == 8);
  input_shutdown(&input);
  assert(input.observer == NULL && input.observer_context == NULL);
  assert(other.count == 1);
  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_input_synchronous_observer PASSED\n");
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
  test_input_character_queue();
  test_input_quick_edges_and_modifiers();
  test_input_synchronous_observer();
  printf("--- Input System tests completed. ---\n");
  return true;
}

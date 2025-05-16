#include "event_test.h"

static Arena *arena = NULL;
static const uint64_t ARENA_SIZE = 1024 * 1024; // 1MB

typedef struct TestEventData {
  uint32_t value;
  bool32_t processed;
} TestEventData;

typedef struct ThreadData {
  EventManager *manager;
  Arena *arena;
  uint32_t thread_id;
} ThreadData;

// Static variables for test state
static uint32_t *test_process_order = NULL;
static uint32_t test_next_index = 0;
static uint32_t callback1_count = 0;
static uint32_t callback2_count = 0;
static EventManager *self_unsubscribe_manager = NULL;
static uint32_t processed_count = 0;
static pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool32_t slow_callback_executed = false;
static bool32_t fast_callback_executed = false;

// Global state for test_event_dispatch_processing
static bool32_t g_dp_key_press_processed_flag = false;
static uint32_t g_dp_key_press_value = 0;
static uint32_t g_dp_key_release_value = 0;

// Global state for test_slow_callbacks
static uint32_t g_sc_event1_final_value = 0;
static uint32_t g_sc_event2_final_value = 0;

// --- Globals for new data ownership tests ---
// For test_data_copying_original_integrity
static uint32_t g_integrity_cb_received_value = 0;
static bool32_t g_integrity_cb_executed = false;

// For test_dispatch_data_size_zero
static bool32_t g_dsz_cb_data_is_null = false;
static bool32_t g_dsz_cb_data_size_is_zero = false;
static uint32_t g_dsz_cb_execution_count = 0;

// For test_data_lifetime_original_freed
static bool32_t g_lifetime_cb_executed_successfully = false;

// Test callbacks
static bool8_t test_callback1(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  data->processed = true;
  data->value += 1;
  return true;
}

static bool8_t test_callback2(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  data->value *= 2;
  return true;
}

static bool8_t test_callback3(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  data->value -= 1;
  return true;
}

// Callbacks specifically for test_event_dispatch_processing
static bool8_t dp_test_callback1(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  g_dp_key_press_processed_flag = true;
  data->value += 1;
  g_dp_key_press_value = data->value; // Store intermediate value
  return true;
}

static bool8_t dp_test_callback2(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  data->value *= 2;
  g_dp_key_press_value = data->value; // Store final value
  return true;
}

static bool8_t dp_test_callback3(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  data->value -= 1;
  g_dp_key_release_value = data->value;
  return true;
}

// Callbacks for the new tests
static bool8_t order_callback(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  test_process_order[test_next_index++] = data->value;
  return true;
}

static bool8_t self_unsubscribe_callback(Event *event) {
  callback1_count++;
  // Unsubscribe after first call
  event_manager_unsubscribe(self_unsubscribe_manager, EVENT_TYPE_KEY_PRESS,
                            self_unsubscribe_callback);
  return true;
}

static bool8_t persistent_callback(Event *event) {
  callback2_count++;
  return true;
}

static bool8_t counting_callback(Event *event) {
  pthread_mutex_lock(&count_mutex);
  processed_count++;
  pthread_mutex_unlock(&count_mutex);
  return true;
}

static bool8_t slow_callback(Event *event) {
  // Sleep to simulate long processing
  platform_sleep(100);

  TestEventData *data = (TestEventData *)event->data;
  data->value += 1;
  // Update global based on initial value before modification by this chain
  if (data->value == 5 + 1) { // Assuming initial was 5 for event1
    g_sc_event1_final_value = data->value;
  } else if (data->value == 10 + 1) { // Assuming initial was 10 for event2
    g_sc_event2_final_value = data->value;
  }
  slow_callback_executed = true;
  return true;
}

static bool8_t fast_callback(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  data->value *= 2;
  // Update global based on initial value before modification by this chain
  if (data->value == (5 + 1) * 2) { // Assuming initial was 5 for event1,
                                    // processed by slow_callback first
    g_sc_event1_final_value = data->value;
  } else if (data->value ==
             (10 + 1) * 2) { // Assuming initial was 10 for event2, processed by
                             // slow_callback first
    g_sc_event2_final_value = data->value;
  }
  fast_callback_executed = true;
  return true;
}

// --- Callbacks for new data ownership tests ---
static bool8_t integrity_check_callback(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  if (data) { // data should not be null if data_size > 0 and copy occurred
    g_integrity_cb_received_value = data->value;
  }
  g_integrity_cb_executed = true;
  return true;
}

static bool8_t data_size_zero_check_callback(Event *event) {
  g_dsz_cb_data_is_null = (event->data == NULL);
  g_dsz_cb_data_size_is_zero = (event->data_size == 0);
  g_dsz_cb_execution_count++;
  return true;
}

static bool8_t lifetime_check_callback(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  // Attempt to access the copied data. If the original was incorrectly used,
  // this might crash or read garbage. A simple check of a known value.
  if (data &&
      data->value == 12345) { // 12345 is the magic value set before dispatch
    g_lifetime_cb_executed_successfully = true;
  }
  return true;
}

static void *dispatch_thread(void *arg) {
  ThreadData *data = (ThreadData *)arg;
  const uint32_t EVENTS_PER_THREAD = 50;

  for (uint32_t i = 0; i < EVENTS_PER_THREAD; i++) {
    TestEventData *event_data = arena_alloc(data->arena, sizeof(TestEventData),
                                            ARENA_MEMORY_TAG_UNKNOWN);
    event_data->value = (data->thread_id * 1000) + i;
    event_data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS,
                   .data = event_data,
                   .data_size = sizeof(TestEventData)};
    event_manager_dispatch(data->manager, event);
  }

  return NULL;
}

// Setup function called before each test function in this suite
static void setup_suite(void) { arena = arena_create(ARENA_SIZE, ARENA_SIZE); }

// Teardown function called after each test function in this suite
static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
  }
}

// Test event manager creation and destruction
static void test_event_manager_create_destroy(void) {
  printf("  Running test_event_manager_create_destroy...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  assert(manager.arena == arena && "Arena pointer mismatch");
  assert(manager.running == true && "Manager should be running");
  assert(manager.queue.data != NULL && "Queue data should be initialized");

  event_manager_destroy(&manager);

  teardown_suite();
  printf("  test_event_manager_create_destroy PASSED\n");
}

// Test event subscription and unsubscription
static void test_event_subscription(void) {
  printf("  Running test_event_subscription...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  // Subscribe to an event
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, test_callback1);

  // Check that callback was added
  assert(manager.callbacks[EVENT_TYPE_KEY_PRESS].length == 1 &&
         "Callback should be added to the manager");

  // Subscribe another callback to the same event
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, test_callback2);
  assert(manager.callbacks[EVENT_TYPE_KEY_PRESS].length == 2 &&
         "Second callback should be added to the manager");

  // Subscribe to a different event
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_RELEASE, test_callback3);
  assert(manager.callbacks[EVENT_TYPE_KEY_RELEASE].length == 1 &&
         "Callback should be added to different event type");

  // Test duplicate subscription (should be ignored)
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, test_callback1);
  assert(manager.callbacks[EVENT_TYPE_KEY_PRESS].length == 2 &&
         "Duplicate subscription should be ignored");

  // Unsubscribe from an event
  event_manager_unsubscribe(&manager, EVENT_TYPE_KEY_PRESS, test_callback1);
  assert(manager.callbacks[EVENT_TYPE_KEY_PRESS].length == 1 &&
         "Callback should be removed from manager");

  // Unsubscribe from event with a callback that doesn't exist
  event_manager_unsubscribe(&manager, EVENT_TYPE_KEY_PRESS, test_callback3);
  assert(manager.callbacks[EVENT_TYPE_KEY_PRESS].length == 1 &&
         "Nonexistent callback unsubscription should have no effect");

  event_manager_destroy(&manager);

  teardown_suite();
  printf("  test_event_subscription PASSED\n");
}

// Test event dispatch and processing
static void test_event_dispatch_processing(void) {
  printf("  Running test_event_dispatch_processing...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  // Reset global state for this test
  g_dp_key_press_processed_flag = false;
  g_dp_key_press_value = 0;
  g_dp_key_release_value = 0;

  // Subscribe to events using specific callbacks for this test
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, dp_test_callback1);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, dp_test_callback2);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_RELEASE, dp_test_callback3);

  // Create original test event data (can be on stack or temp arena)
  TestEventData original_key_press_data;
  original_key_press_data.value = 5;
  original_key_press_data.processed =
      false; // This field is not checked directly anymore

  // Create and dispatch event
  Event event = {.type = EVENT_TYPE_KEY_PRESS,
                 .data = &original_key_press_data,
                 .data_size = sizeof(TestEventData)};

  bool32_t dispatch_result = event_manager_dispatch(&manager, event);
  assert(dispatch_result && "Event dispatch should succeed");

  // Wait for event to be processed
  platform_sleep(100);

  // The callbacks should have: added 1 and then multiplied by 2
  // So 5 -> 6 -> 12. Check global state instead of original_key_press_data.
  assert(g_dp_key_press_processed_flag &&
         "Event should be processed by dp_test_callback1");
  assert(g_dp_key_press_value == 12 && "Event callbacks should modify data "
                                       "correctly, reflected in global state");

  // Create another event with different type
  TestEventData original_key_release_data;
  original_key_release_data.value = 10;
  // original_key_release_data.processed is not used by dp_test_callback3

  Event event2 = {.type = EVENT_TYPE_KEY_RELEASE,
                  .data = &original_key_release_data,
                  .data_size = sizeof(TestEventData)};

  dispatch_result = event_manager_dispatch(&manager, event2);
  assert(dispatch_result && "Second event dispatch should succeed");

  // Wait for event to be processed
  platform_sleep(100);

  // The callback should have: subtracted 1
  // So 10 -> 9. Check global state.
  assert(g_dp_key_release_value == 9 &&
         "Event callback should subtract 1, reflected in global state");

  event_manager_destroy(&manager);

  teardown_suite();
  printf("  test_event_dispatch_processing PASSED\n");
}

// Test queue full scenario
static void test_queue_full(void) {
  printf("  Running test_queue_full...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  // We need to fill the queue without processing events
  // First, stop the event processor
  pthread_mutex_lock(&manager.mutex);
  manager.running = false;
  pthread_mutex_unlock(&manager.mutex);
  pthread_cond_signal(&manager.cond);

  // Wait for the thread to exit
  pthread_join(manager.thread, NULL);

  // Now reinitialize queue and mutex manually without a thread
  manager.running = false;
  pthread_mutex_init(&manager.mutex, NULL);
  pthread_cond_init(&manager.cond, NULL);

  // Fill the queue
  bool32_t queue_full = false;
  for (uint32_t i = 0; i < 2000; i++) {
    TestEventData *data =
        arena_alloc(arena, sizeof(TestEventData), ARENA_MEMORY_TAG_UNKNOWN);
    data->value = i;
    data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS,
                   .data = data,
                   .data_size = sizeof(TestEventData)};

    bool32_t dispatch_result = false;
    pthread_mutex_lock(&manager.mutex);
    dispatch_result = queue_enqueue_Event(&manager.queue, event);
    pthread_mutex_unlock(&manager.mutex);

    if (!dispatch_result) {
      queue_full = true;
      break;
    }
  }

  assert(queue_full && "Queue should become full after many events");

  // Manually clean up resources
  pthread_mutex_destroy(&manager.mutex);
  pthread_cond_destroy(&manager.cond);

  for (uint16_t i = 0; i < EVENT_TYPE_MAX; i++) {
    if (manager.callbacks[i].data != NULL) {
      vector_destroy_EventCallback(&manager.callbacks[i]);
    }
  }

  teardown_suite();
  printf("  test_queue_full PASSED\n");
}

// Test event FIFO ordering
static void test_event_ordering(void) {
  printf("  Running test_event_ordering...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  // Create an array to record the order of processing
  const uint32_t EVENT_COUNT = 10;
  test_process_order = arena_alloc(arena, sizeof(uint32_t) * EVENT_COUNT,
                                   ARENA_MEMORY_TAG_UNKNOWN);
  test_next_index = 0;

  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, order_callback);

  // Dispatch events with sequential IDs
  for (uint32_t i = 0; i < EVENT_COUNT; i++) {
    TestEventData *data =
        arena_alloc(arena, sizeof(TestEventData), ARENA_MEMORY_TAG_UNKNOWN);
    data->value = i;
    data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS,
                   .data = data,
                   .data_size = sizeof(TestEventData)};
    event_manager_dispatch(&manager, event);
  }

  // Wait for events to be processed
  platform_sleep(100);

  // Verify events were processed in FIFO order
  for (uint32_t i = 0; i < EVENT_COUNT; i++) {
    assert(test_process_order[i] == i &&
           "Events should be processed in FIFO order");
  }

  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_event_ordering PASSED\n");
}

// Test dynamic unsubscription during processing
static void test_dynamic_unsubscribe(void) {
  printf("  Running test_dynamic_unsubscribe...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  // Reset counters
  callback1_count = 0;
  callback2_count = 0;
  self_unsubscribe_manager = &manager;

  // Subscribe both callbacks
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS,
                          self_unsubscribe_callback);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, persistent_callback);

  // Dispatch several events
  for (uint32_t i = 0; i < 5; i++) {
    TestEventData *data =
        arena_alloc(arena, sizeof(TestEventData), ARENA_MEMORY_TAG_UNKNOWN);
    data->value = i;
    data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS,
                   .data = data,
                   .data_size = sizeof(TestEventData)};
    event_manager_dispatch(&manager, event);

    // Sleep briefly to ensure processing completes
    platform_sleep(50);
  }

  // Verify first callback only received one event before unsubscribing
  assert(callback1_count == 1 &&
         "Self-unsubscribing callback should only execute once");
  // Verify second callback received all events
  assert(callback2_count == 5 &&
         "Persistent callback should receive all events");

  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_dynamic_unsubscribe PASSED\n");
}

// Test concurrent event dispatching from multiple threads
static void test_concurrent_dispatch(void) {
  printf("  Running test_concurrent_dispatch...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  // Reset counter
  processed_count = 0;
  pthread_mutex_init(&count_mutex, NULL);

  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, counting_callback);

  // Create and start multiple threads
  const uint32_t THREAD_COUNT = 4;
  const uint32_t EVENTS_PER_THREAD = 50;
  pthread_t threads[THREAD_COUNT];
  ThreadData thread_data[THREAD_COUNT];

  for (uint32_t i = 0; i < THREAD_COUNT; i++) {
    thread_data[i].manager = &manager;
    thread_data[i].arena = arena;
    thread_data[i].thread_id = i;
    pthread_create(&threads[i], NULL, dispatch_thread, &thread_data[i]);
  }

  // Wait for all threads to complete
  for (uint32_t i = 0; i < THREAD_COUNT; i++) {
    pthread_join(threads[i], NULL);
  }

  // Wait for event processing to complete
  platform_sleep(500);

  // Verify all events were processed
  pthread_mutex_lock(&count_mutex);
  assert(processed_count == THREAD_COUNT * EVENTS_PER_THREAD &&
         "All events from all threads should be processed");
  pthread_mutex_unlock(&count_mutex);

  pthread_mutex_destroy(&count_mutex);
  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_concurrent_dispatch PASSED\n");
}

// Test slow/blocking callbacks
static void test_slow_callbacks(void) {
  printf("  Running test_slow_callbacks...\n");
  setup_suite();

  EventManager manager;
  event_manager_create(arena, &manager);

  // Reset state
  slow_callback_executed = false;
  fast_callback_executed = false;
  g_sc_event1_final_value = 0;
  g_sc_event2_final_value = 0;

  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, slow_callback);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, fast_callback);

  // Create original event data (can be on stack or temp arena)
  TestEventData original_data1;
  original_data1.value = 5;

  TestEventData original_data2;
  original_data2.value = 10;

  // Dispatch events
  Event event1 = {.type = EVENT_TYPE_KEY_PRESS,
                  .data = &original_data1,
                  .data_size = sizeof(TestEventData)};
  Event event2 = {.type = EVENT_TYPE_KEY_PRESS,
                  .data = &original_data2,
                  .data_size = sizeof(TestEventData)};

  event_manager_dispatch(&manager, event1);
  event_manager_dispatch(&manager, event2);

  // Wait for processing
  platform_sleep(300);

  // Verify both callbacks were executed
  assert(slow_callback_executed && "Slow callback should execute");
  assert(fast_callback_executed && "Fast callback should execute");

  // Verify data was modified correctly using global state
  // Each event goes through both callbacks: +1 and *2
  // Event 1: (5+1)*2 = 12
  // Event 2: (10+1)*2 = 22
  assert(
      g_sc_event1_final_value == 12 &&
      "First event should be processed by both callbacks with correct value");
  assert(
      g_sc_event2_final_value == 22 &&
      "Second event should be processed by both callbacks with correct value");

  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_slow_callbacks PASSED\n");
}

// --- New test functions for data ownership ---

static void test_data_copying_original_integrity(void) {
  printf("  Running test_data_copying_original_integrity...\n");
  setup_suite();
  EventManager manager;
  event_manager_create(arena, &manager);

  g_integrity_cb_executed = false;
  g_integrity_cb_received_value = 0;

  event_manager_subscribe(&manager, EVENT_TYPE_MOUSE_MOVE,
                          integrity_check_callback);

  TestEventData *original_data =
      arena_alloc(arena, sizeof(TestEventData), ARENA_MEMORY_TAG_UNKNOWN);
  original_data->value = 100;
  original_data->processed = false;

  Event event = {.type = EVENT_TYPE_MOUSE_MOVE,
                 .data = original_data,
                 .data_size = sizeof(TestEventData)};
  event_manager_dispatch(&manager, event);

  // Modify original data AFTER dispatch
  original_data->value = 200;

  platform_sleep(100);

  assert(g_integrity_cb_executed && "Integrity callback should have executed");
  assert(
      g_integrity_cb_received_value == 100 &&
      "Callback should have received the data value at the time of dispatch");
  assert(original_data->value == 200 &&
         "Original data should retain its modification made after dispatch");

  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_data_copying_original_integrity PASSED\n");
}

static void test_dispatch_data_size_zero(void) {
  printf("  Running test_dispatch_data_size_zero...\n");
  setup_suite();
  EventManager manager;
  event_manager_create(arena, &manager);

  g_dsz_cb_execution_count = 0;
  g_dsz_cb_data_is_null = false;
  g_dsz_cb_data_size_is_zero = false;

  event_manager_subscribe(&manager, EVENT_TYPE_MOUSE_WHEEL,
                          data_size_zero_check_callback);

  // Case 1: data_size = 0, data is non-NULL (but should be ignored for copying)
  TestEventData dummy_data = {.value = 50, .processed = false};
  Event event1 = {
      .type = EVENT_TYPE_MOUSE_WHEEL, .data = &dummy_data, .data_size = 0};
  event_manager_dispatch(&manager, event1);

  platform_sleep(100);

  assert(g_dsz_cb_execution_count == 1 &&
         "DSZ Callback should have executed once for event1");
  assert(g_dsz_cb_data_is_null &&
         "Callback event data should be NULL for data_size=0 event1");
  assert(g_dsz_cb_data_size_is_zero &&
         "Callback event data_size should be 0 for event1");

  // Reset for next dispatch
  g_dsz_cb_data_is_null = false;
  g_dsz_cb_data_size_is_zero = false;

  // Case 2: data_size = 0, data is NULL
  Event event2 = {.type = EVENT_TYPE_MOUSE_WHEEL, .data = NULL, .data_size = 0};
  event_manager_dispatch(&manager, event2);
  platform_sleep(100);

  assert(g_dsz_cb_execution_count == 2 &&
         "DSZ Callback should have executed again for event2");
  assert(g_dsz_cb_data_is_null &&
         "Callback event data should be NULL for data_size=0 event2");
  assert(g_dsz_cb_data_size_is_zero &&
         "Callback event data_size should be 0 for event2");

  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_dispatch_data_size_zero PASSED\n");
}

static void test_data_lifetime_original_freed(void) {
  printf("  Running test_data_lifetime_original_freed...\n");
  setup_suite(); // Main arena
  EventManager manager;
  event_manager_create(arena, &manager);

  g_lifetime_cb_executed_successfully = false;
  event_manager_subscribe(&manager, EVENT_TYPE_BUTTON_PRESS,
                          lifetime_check_callback);

  // Create a temporary scratch arena for the original data
  Scratch scratch = scratch_create(arena);
  TestEventData *original_data_on_scratch = arena_alloc(
      scratch.arena, sizeof(TestEventData), ARENA_MEMORY_TAG_UNKNOWN);
  original_data_on_scratch->value = 12345; // Magic value
  original_data_on_scratch->processed = false;

  Event event = {.type = EVENT_TYPE_BUTTON_PRESS,
                 .data = original_data_on_scratch,
                 .data_size = sizeof(TestEventData)};
  event_manager_dispatch(&manager, event);

  // Destroy the scratch arena, freeing the original_data_on_scratch
  scratch_destroy(scratch, ARENA_MEMORY_TAG_UNKNOWN);
  // original_data_on_scratch is now a dangling pointer / points to freed memory

  platform_sleep(100);

  assert(
      g_lifetime_cb_executed_successfully &&
      "Lifetime callback should have executed successfully using copied data");

  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_data_lifetime_original_freed PASSED\n");
}

// Run all event system tests
bool32_t run_event_tests() {
  printf("--- Running Event System tests... ---\n");
  test_event_manager_create_destroy();
  test_event_subscription();
  test_event_dispatch_processing();
  test_queue_full();
  test_event_ordering();
  test_dynamic_unsubscribe();
  test_concurrent_dispatch();
  test_slow_callbacks();
  test_data_copying_original_integrity();
  test_dispatch_data_size_zero();
  test_data_lifetime_original_freed();
  printf("--- Event System tests completed. ---\n");
  return true;
}
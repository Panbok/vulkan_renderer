#include "event_test.h"
#include <stdio.h>

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
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000}; // 100ms
  nanosleep(&ts, NULL);

  TestEventData *data = (TestEventData *)event->data;
  data->value += 1;
  slow_callback_executed = true;
  return true;
}

static bool8_t fast_callback(Event *event) {
  TestEventData *data = (TestEventData *)event->data;
  data->value *= 2;
  fast_callback_executed = true;
  return true;
}

static void *dispatch_thread(void *arg) {
  ThreadData *data = (ThreadData *)arg;
  const uint32_t EVENTS_PER_THREAD = 50;

  for (uint32_t i = 0; i < EVENTS_PER_THREAD; i++) {
    TestEventData *event_data = arena_alloc(data->arena, sizeof(TestEventData));
    event_data->value = (data->thread_id * 1000) + i;
    event_data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS, .data = event_data};
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

  // Subscribe to events
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, test_callback1);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, test_callback2);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_RELEASE, test_callback3);

  // Create test event data
  TestEventData *key_press_data = arena_alloc(arena, sizeof(TestEventData));
  key_press_data->value = 5;
  key_press_data->processed = false;

  // Create and dispatch event
  Event event = {.type = EVENT_TYPE_KEY_PRESS, .data = key_press_data};

  bool32_t dispatch_result = event_manager_dispatch(&manager, event);
  assert(dispatch_result && "Event dispatch should succeed");

  // Wait for event to be processed
  // Need to sleep to allow the event processor thread to run
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000}; // 100ms
  nanosleep(&ts, NULL);

  // The callbacks should have: added 1 and then multiplied by 2
  // So 5 -> 6 -> 12
  assert(key_press_data->processed && "Event should be processed");
  assert(key_press_data->value == 12 &&
         "Event callbacks should modify data correctly");

  // Create another event with different type
  TestEventData *key_release_data = arena_alloc(arena, sizeof(TestEventData));
  key_release_data->value = 10;
  key_release_data->processed = false;

  Event event2 = {.type = EVENT_TYPE_KEY_RELEASE, .data = key_release_data};

  dispatch_result = event_manager_dispatch(&manager, event2);
  assert(dispatch_result && "Second event dispatch should succeed");

  // Wait for event to be processed
  nanosleep(&ts, NULL);

  // The callback should have: subtracted 1
  // So 10 -> 9
  assert(key_release_data->value == 9 && "Event callback should subtract 1");

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
    TestEventData *data = arena_alloc(arena, sizeof(TestEventData));
    data->value = i;
    data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS, .data = data};

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
  test_process_order = arena_alloc(arena, sizeof(uint32_t) * EVENT_COUNT);
  test_next_index = 0;

  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, order_callback);

  // Dispatch events with sequential IDs
  for (uint32_t i = 0; i < EVENT_COUNT; i++) {
    TestEventData *data = arena_alloc(arena, sizeof(TestEventData));
    data->value = i;
    data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS, .data = data};
    event_manager_dispatch(&manager, event);
  }

  // Wait for events to be processed
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000000}; // 200ms
  nanosleep(&ts, NULL);

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
    TestEventData *data = arena_alloc(arena, sizeof(TestEventData));
    data->value = i;
    data->processed = false;

    Event event = {.type = EVENT_TYPE_KEY_PRESS, .data = data};
    event_manager_dispatch(&manager, event);

    // Sleep briefly to ensure processing completes
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000}; // 50ms
    nanosleep(&ts, NULL);
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
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 500000000}; // 500ms
  nanosleep(&ts, NULL);

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

  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, slow_callback);
  event_manager_subscribe(&manager, EVENT_TYPE_KEY_PRESS, fast_callback);

  // Create events
  TestEventData *data1 = arena_alloc(arena, sizeof(TestEventData));
  data1->value = 5;

  TestEventData *data2 = arena_alloc(arena, sizeof(TestEventData));
  data2->value = 10;

  // Dispatch events
  Event event1 = {.type = EVENT_TYPE_KEY_PRESS, .data = data1};
  Event event2 = {.type = EVENT_TYPE_KEY_PRESS, .data = data2};

  event_manager_dispatch(&manager, event1);
  event_manager_dispatch(&manager, event2);

  // Wait for processing
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 300000000}; // 300ms
  nanosleep(&ts, NULL);

  // Verify both callbacks were executed
  assert(slow_callback_executed && "Slow callback should execute");
  assert(fast_callback_executed && "Fast callback should execute");

  // Verify data was modified correctly
  // Each event goes through both callbacks: +1 and *2
  assert(data1->value == 12 &&
         "First event should be processed by both callbacks");
  assert(data2->value == 22 &&
         "Second event should be processed by both callbacks");

  event_manager_destroy(&manager);
  teardown_suite();
  printf("  test_slow_callbacks PASSED\n");
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
  printf("--- Event System tests completed. ---\n");
  return true;
}
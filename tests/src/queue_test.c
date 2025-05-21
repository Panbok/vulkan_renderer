#include "queue_test.h"

static Arena *arena = NULL;
static const uint64_t ARENA_SIZE = 1024 * 1024; // 1MB

// Setup function called before each test function in this suite
static void setup_suite(void) { arena = arena_create(ARENA_SIZE); }

// Teardown function called after each test function in this suite
static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
  }
}

static void test_queue_create_uint32_t(void) {
  printf("  Running test_queue_create_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  assert(queue.arena == arena && "Arena pointer mismatch");
  assert(queue.capacity == capacity && "Capacity mismatch");
  assert(queue.size == 0 && "Size mismatch");
  assert(queue.data != NULL && "Data pointer mismatch");

  queue_destroy_uint32_t(&queue);

  teardown_suite();
  printf("  test_queue_create_uint32_t PASSED\n");
}

static void test_queue_enqueue_uint32_t(void) {
  printf("  Running test_queue_enqueue_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  for (uint64_t i = 0; i < capacity; ++i) {
    assert(queue_enqueue_uint32_t(&queue, i) && "Enqueue failed");
  }

  assert(queue_is_full_uint32_t(&queue) && "Queue should be full");

  queue_destroy_uint32_t(&queue);

  teardown_suite();
  printf("  test_queue_enqueue_uint32_t PASSED\n");
}

static void test_queue_dequeue_uint32_t(void) {
  printf("  Running test_queue_dequeue_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  for (uint64_t i = 0; i < capacity; ++i) {
    assert(queue_enqueue_uint32_t(&queue, i) && "Enqueue failed");
  }

  for (uint64_t i = 0; i < capacity; ++i) {
    assert(queue_dequeue_uint32_t(&queue, NULL) && "Dequeue failed");
  }

  assert(queue_is_empty_uint32_t(&queue) && "Queue should be empty");

  queue_destroy_uint32_t(&queue);

  teardown_suite();
  printf("  test_queue_dequeue_uint32_t PASSED\n");
}

static void test_queue_is_empty_uint32_t(void) {
  printf("  Running test_queue_is_empty_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  assert(queue_is_empty_uint32_t(&queue) && "Queue should be empty");

  queue_destroy_uint32_t(&queue);

  teardown_suite();
  printf("  test_queue_is_empty_uint32_t PASSED\n");
}

static void test_queue_is_full_uint32_t(void) {
  printf("  Running test_queue_is_full_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  for (uint64_t i = 0; i < capacity; ++i) {
    assert(queue_enqueue_uint32_t(&queue, i) && "Enqueue failed");
  }

  assert(queue_is_full_uint32_t(&queue) && "Queue should be full");

  queue_destroy_uint32_t(&queue);

  teardown_suite();
  printf("  test_queue_is_full_uint32_t PASSED\n");
}

static void test_queue_peek_uint32_t(void) {
  printf("  Running test_queue_peek_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  for (uint64_t i = 0; i < capacity; ++i) {
    assert(queue_enqueue_uint32_t(&queue, i) && "Enqueue failed");
  }

  assert(queue_peek_uint32_t(&queue) == 0 && "Peek value mismatch");

  queue_destroy_uint32_t(&queue);

  teardown_suite();
  printf("  test_queue_peek_uint32_t PASSED\n");
}

static void test_queue_clear_uint32_t(void) {
  printf("  Running test_queue_clear_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  for (uint64_t i = 0; i < capacity; ++i) {
    assert(queue_enqueue_uint32_t(&queue, i) && "Enqueue failed");
  }

  queue_clear_uint32_t(&queue);

  assert(queue_is_empty_uint32_t(&queue) && "Queue should be empty");

  queue_destroy_uint32_t(&queue);

  teardown_suite();
  printf("  test_queue_clear_uint32_t PASSED\n");
}

static void test_queue_destroy_uint32_t(void) {
  printf("  Running test_queue_destroy_uint32_t...\n");
  setup_suite();

  const uint64_t capacity = 10;
  Queue_uint32_t queue = queue_create_uint32_t(arena, capacity);

  queue_destroy_uint32_t(&queue);

  assert(queue.arena == NULL && "Arena pointer should be NULL");
  assert(queue.capacity == 0 && "Capacity should be 0");
  assert(queue.size == 0 && "Size should be 0");
  assert(queue.data == NULL && "Data pointer should be NULL");

  teardown_suite();
  printf("  test_queue_destroy_uint32_t PASSED\n");
}

bool32_t run_queue_tests() {
  printf("--- Running Queue tests... ---\n");
  test_queue_create_uint32_t();
  test_queue_enqueue_uint32_t();
  test_queue_dequeue_uint32_t();
  test_queue_is_empty_uint32_t();
  test_queue_is_full_uint32_t();
  test_queue_peek_uint32_t();
  test_queue_clear_uint32_t();
  test_queue_destroy_uint32_t();
  printf("--- Queue tests completed. ---\n");
  return true;
}

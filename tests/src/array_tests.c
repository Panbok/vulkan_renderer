#include "array_test.h"
#include "memory/vkr_arena_allocator.h"

static Arena *arena = NULL;
static VkrAllocator allocator = {0};
static const uint64_t ARENA_SIZE = 1024 * 1024; // 1MB

// Setup function called before each test function in this suite
static void setup_suite(void) {
  arena = arena_create(ARENA_SIZE);
  allocator = (VkrAllocator){.ctx = arena};
  vkr_allocator_arena(&allocator);
}

// Teardown function called after each test function in this suite
static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
    allocator = (VkrAllocator){0};
  }
}

static void test_array_create_int(void) {
  printf("  Running test_array_create_int...\n");
  setup_suite();

  const uint64_t length = 10;
  Array_uint32_t arr = array_create_uint32_t(&allocator, length);

  assert(arr.allocator == &allocator && "Allocator pointer mismatch");
  assert(arr.length == length && "Length mismatch");
  assert(arr.data != NULL && "Data is NULL");

  array_destroy_uint32_t(&arr);
  assert(arr.data == NULL && "Data not NULL after destroy");
  assert(arr.allocator == NULL && "Allocator not NULL after destroy");
  assert(arr.length == 0 && "Length not 0 after destroy");

  teardown_suite();
  printf("  test_array_create_int PASSED\n");
}

static void test_array_set_get_int(void) {
  printf("  Running test_array_set_get_int...\n");
  setup_suite();

  const uint64_t length = 5;
  // Correctly pass the allocator pointer
  Array_uint32_t arr = array_create_uint32_t(&allocator, length);

  for (uint64_t i = 0; i < length; ++i) {
    array_set_uint32_t(&arr, i, (uint32_t)(i * i));
  }

  for (uint64_t i = 0; i < length; ++i) {
    uint32_t *value_ptr = array_get_uint32_t(&arr, i);
    assert(value_ptr != NULL && "Got NULL pointer from get");
    assert(*value_ptr == (uint32_t)(i * i) && "Value mismatch");
  }

  array_destroy_uint32_t(&arr);

  teardown_suite();
  printf("  test_array_set_get_int PASSED\n");
}

static void *array_fail_alloc(void *ctx, uint64_t size, uint64_t alignment,
                              VkrAllocatorMemoryTag tag) {
  (void)size;
  (void)alignment;
  (void)tag;
  (*(uint32_t *)ctx)++;
  return NULL;
}

static void test_array_create_failure(void) {
  uint32_t calls = 0;
  VkrAllocator failing = {.ctx = &calls, .alloc_aligned = array_fail_alloc};
  Array_uint32_t array = array_create_uint32_t(&failing, 8);
  assert(calls == 1 && !array.data && !array.allocator && !array.length);
  array_destroy_uint32_t(&array);
  array = array_create_uint32_t(&failing, SIZE_MAX / sizeof(uint32_t) + 1);
  assert(calls == 1 && !array.data && !array.allocator && !array.length);
}

bool32_t run_array_tests() {
  printf("--- Starting Array Tests ---\n");

  test_array_create_failure();
  test_array_create_int();
  test_array_set_get_int();
  // Call other test functions here

  printf("--- Array Tests Completed ---\n");
  return true; // Assumes asserts halt on failure
}

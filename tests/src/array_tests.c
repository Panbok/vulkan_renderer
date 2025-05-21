#include "array_test.h"

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

static void test_array_create_int(void) {
  printf("  Running test_array_create_int...\n");
  setup_suite();

  const uint64_t length = 10;
  Array_uint32_t arr = array_create_uint32_t(arena, length);

  assert(arr.arena == arena && "Arena pointer mismatch");
  assert(arr.length == length && "Length mismatch");
  assert(arr.data != NULL && "Data is NULL");

  array_destroy_uint32_t(&arr);
  assert(arr.data == NULL && "Data not NULL after destroy");
  assert(arr.arena == NULL && "Arena not NULL after destroy");
  assert(arr.length == 0 && "Length not 0 after destroy");

  teardown_suite();
  printf("  test_array_create_int PASSED\n");
}

static void test_array_set_get_int(void) {
  printf("  Running test_array_set_get_int...\n");
  setup_suite();

  const uint64_t length = 5;
  // Correctly pass the arena pointer
  Array_uint32_t arr = array_create_uint32_t(arena, length);

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

// Add more tests here following the pattern:
// static void test_xxx() { ... }

// Test runner for this suite
bool32_t run_array_tests() {
  printf("--- Starting Array Tests ---\n");

  test_array_create_int();
  test_array_set_get_int();
  // Call other test functions here

  printf("--- Array Tests Completed ---\n");
  return true; // Assumes asserts halt on failure
}
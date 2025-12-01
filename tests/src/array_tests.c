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
  // Correctly pass the arena pointer
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

static void test_array_is_null(void) {
  printf("  Running test_array_is_null...\n");
  setup_suite();

  // Test 1: Uninitialized array (zero-initialized)
  Array_uint32_t uninitialized_arr = {0};
  assert(array_is_null_uint32_t(&uninitialized_arr) == true &&
         "Uninitialized array should be null");

  // Test 2: Properly created array should not be null
  const uint64_t length = 5;
  Array_uint32_t arr = array_create_uint32_t(&allocator, length);
  assert(array_is_null_uint32_t(&arr) == false &&
         "Created array should not be null");

  // Test 3: Destroyed array should be null
  array_destroy_uint32_t(&arr);
  assert(array_is_null_uint32_t(&arr) == true &&
         "Destroyed array should be null");

  teardown_suite();
  printf("  test_array_is_null PASSED\n");
}

static void test_array_is_empty(void) {
  printf("  Running test_array_is_empty...\n");
  setup_suite();

  // Test 1: Array with length 0 should be empty
  Array_uint32_t zero_length_arr = array_create_uint32_t(&allocator, 1);
  // Manually set length to 0 to test the empty condition
  zero_length_arr.length = 0;
  assert(array_is_empty_uint32_t(&zero_length_arr) == true &&
         "Array with length 0 should be empty");

  // Test 2: Array with length > 0 should not be empty
  Array_uint32_t arr = array_create_uint32_t(&allocator, 5);
  assert(array_is_empty_uint32_t(&arr) == false &&
         "Array with length > 0 should not be empty");

  // Test 3: Even after setting values, array with length > 0 is not empty
  array_set_uint32_t(&arr, 0, 42);
  assert(array_is_empty_uint32_t(&arr) == false &&
         "Array with elements should not be empty");

  array_destroy_uint32_t(&arr);

  teardown_suite();
  printf("  test_array_is_empty PASSED\n");
}

static void test_array_null_vs_empty_semantics(void) {
  printf("  Running test_array_null_vs_empty_semantics...\n");
  setup_suite();

  // Test the semantic difference between null and empty

  // Case 1: Uninitialized array
  Array_uint32_t uninitialized = {0};
  assert(array_is_null_uint32_t(&uninitialized) == true &&
         "Uninitialized array should be null");
  assert(array_is_empty_uint32_t(&uninitialized) == true &&
         "Uninitialized array should be empty (length 0)");

  // Case 2: Created array with length > 0
  Array_uint32_t normal_arr = array_create_uint32_t(&allocator, 3);
  assert(array_is_null_uint32_t(&normal_arr) == false &&
         "Created array should not be null");
  assert(array_is_empty_uint32_t(&normal_arr) == false &&
         "Created array with length > 0 should not be empty");

  // Case 3: Destroyed array
  array_destroy_uint32_t(&normal_arr);
  assert(array_is_null_uint32_t(&normal_arr) == true &&
         "Destroyed array should be null");
  assert(array_is_empty_uint32_t(&normal_arr) == true &&
         "Destroyed array should be empty (length set to 0)");

  teardown_suite();
  printf("  test_array_null_vs_empty_semantics PASSED\n");
}

// Add more tests here following the pattern:
// static void test_xxx() { ... }

// Test runner for this suite
bool32_t run_array_tests() {
  printf("--- Starting Array Tests ---\n");

  test_array_create_int();
  test_array_set_get_int();
  test_array_is_null();
  test_array_is_empty();
  test_array_null_vs_empty_semantics();
  // Call other test functions here

  printf("--- Array Tests Completed ---\n");
  return true; // Assumes asserts halt on failure
}

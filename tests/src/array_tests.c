#include "array_test.h"
#include "container_test_allocator.h"
#include "memory/vkr_arena_allocator.h"

Array(float);

/* An element type whose alignment exceeds every allocator's default. */
typedef struct ArrayTestAligned {
  _Alignas(64) float32_t value[4];
} ArrayTestAligned;
Array(ArrayTestAligned);

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

static void test_array_create_float(void) {
  printf("  Running test_array_create_float...\n");
  setup_suite();

  const uint64_t initial_capacity = 5;
  Array_float array = array_create_float(&allocator, initial_capacity);

  assert(array.allocator == &allocator && "Allocator pointer mismatch");
  assert(array.capacity == initial_capacity && "Capacity mismatch");
  assert(array.length == 0 && "Initial length non-zero");
  assert(array.data != NULL && "Data is NULL");

  array_destroy_float(&array);
  assert(array.data == NULL && "Data not NULL after destroy");
  assert(array.allocator == NULL && "Allocator not NULL after destroy");
  assert(array.length == 0 && "Length not 0 after destroy");
  assert(array.capacity == 0 && "Capacity not 0 after destroy");

  // Capacity 0 is an empty record that keeps its allocator and grows.
  array = array_create_float(&allocator, 0);
  assert(array.allocator == &allocator && !array.data && !array.capacity);
  assert(array_push_float(&array, 1.0f));
  assert(array.capacity == VKR_ARRAY_FIRST_GROWTH_CAPACITY);
  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_create_float PASSED\n");
}

static void test_array_create_filled_uint32_t(void) {
  printf("  Running test_array_create_filled_uint32_t...\n");
  ContainerTestAllocator state = {0};
  VkrAllocator alloc = container_test_allocator(&state);

  const uint64_t length = 10;
  Array_uint32_t array = array_create_filled_uint32_t(&alloc, length);
  assert(array.allocator == &alloc && "Allocator pointer mismatch");
  assert(array.length == length && array.capacity == length);
  assert(array.data != NULL && "Data is NULL");
  for (uint64_t i = 0; i < length; ++i) {
    assert(array.data[i] == 0u && "Filled element is not zero");
  }

  for (uint64_t i = 0; i < length; ++i) {
    array_set_uint32_t(&array, i, (uint32_t)(i * i));
  }
  for (uint64_t i = 0; i < length; ++i) {
    uint32_t *value = array_get_uint32_t(&array, i);
    assert(value != NULL && *value == (uint32_t)(i * i));
  }

  array_destroy_uint32_t(&array);
  assert(state.live_bytes == 0);

  array = array_create_filled_uint32_t(&alloc, 0);
  assert(array.allocator == &alloc && !array.data && !array.length);
  printf("  test_array_create_filled_uint32_t PASSED\n");
}

static void test_array_push_pop_float(void) {
  printf("  Running test_array_push_pop_float...\n");
  setup_suite();

  Array_float array = array_create_float(&allocator, 16);

  assert(array_push_float(&array, 1.0f));
  assert(array_push_float(&array, 2.5f));
  assert(array_push_float(&array, -3.0f));
  assert(array.length == 3 && "Length after pushes mismatch");

  float val = array_pop_float(&array);
  assert(val == -3.0f && "Pop 1 value mismatch");
  assert(array.length == 2 && "Length after pop 1 mismatch");

  val = array_pop_float(&array);
  assert(val == 2.5f && "Pop 2 value mismatch");
  assert(array.length == 1 && "Length after pop 2 mismatch");

  val = array_pop_float(&array);
  assert(val == 1.0f && "Pop 3 value mismatch");
  assert(array.length == 0 && "Length after pop 3 mismatch");

  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_push_pop_float PASSED\n");
}

static void test_array_get_set_float(void) {
  printf("  Running test_array_get_set_float...\n");
  setup_suite();

  Array_float array = array_create_float(&allocator, 16);
  assert(array_push_float(&array, 10.0f));
  assert(array_push_float(&array, 20.0f));

  float *val_ptr = array_get_float(&array, 0);
  assert(val_ptr != NULL && "Got NULL pointer from get 0");
  assert(*val_ptr == 10.0f && "Get 0 value mismatch");

  array_set_float(&array, 1, 30.0f);
  val_ptr = array_get_float(&array, 1);
  assert(val_ptr != NULL && "Got NULL pointer from get 1");
  assert(*val_ptr == 30.0f && "Get 1 value mismatch after set");

  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_get_set_float PASSED\n");
}

static void test_array_grow_float(void) {
  printf("  Running test_array_grow_float...\n");
  setup_suite();

  const uint64_t initial_capacity = 2;
  Array_float array = array_create_float(&allocator, initial_capacity);

  assert(array_push_float(&array, 1.0f));
  assert(array_push_float(&array, 2.0f));

  // Should trigger growth
  assert(array_push_float(&array, 3.0f));

  assert(array.length == 3 && "Length after growth mismatch");
  assert(array.capacity == initial_capacity * VKR_ARRAY_GROWTH_FACTOR &&
         "Capacity after growth mismatch");

  float *val_ptr = array_get_float(&array, 0);
  assert(*val_ptr == 1.0f && "Value 0 after growth mismatch");
  val_ptr = array_get_float(&array, 1);
  assert(*val_ptr == 2.0f && "Value 1 after growth mismatch");
  val_ptr = array_get_float(&array, 2);
  assert(*val_ptr == 3.0f && "Value 2 after growth mismatch");

  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_grow_float PASSED\n");
}

static void test_array_clear_float(void) {
  printf("  Running test_array_clear_float...\n");
  setup_suite();

  Array_float array = array_create_float(&allocator, 16);
  assert(array_push_float(&array, 1.0f));
  assert(array_push_float(&array, 2.0f));
  assert(array.length == 2 && "Length before clear mismatch");

  array_clear_float(&array);
  assert(array.length == 0 && "Length after clear mismatch");
  // Capacity should remain the same
  assert(array.capacity == 16 && "Capacity after clear mismatch");
  assert(array.data != NULL && "Data NULL after clear");

  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_clear_float PASSED\n");
}

static void test_array_pop_at_float(void) {
  printf("  Running test_array_pop_at_float...\n");
  setup_suite();

  Array_float array = array_create_float(&allocator, 16);
  assert(array_push_float(&array, 1.0f));
  assert(array_push_float(&array, 2.0f));
  assert(array_push_float(&array, 3.0f));

  float val = 0.0f;
  array_pop_at_float(&array, 1, &val);
  assert(val == 2.0f && "Pop at 1 value mismatch");
  assert(array.length == 2 && "Length after pop at 1 mismatch");

  array_pop_at_float(&array, 1, &val);
  assert(val == 3.0f && "Pop at 1 value mismatch");
  assert(array.length == 1 && "Length after pop at 1 mismatch");

  assert(array_push_float(&array, 4.0f));
  assert(array_push_float(&array, 5.0f));
  assert(array_push_float(&array, 6.0f));
  assert(array_push_float(&array, 7.0f));

  array_pop_at_float(&array, 1, &val);
  assert(val == 4.0f && "Pop at 1 value mismatch");
  assert(array.length == 4 && "Length after pop at 1 mismatch");

  assert(array.data[0] == 1.0f && "Element 0 mismatch");
  assert(array.data[1] == 5.0f && "Element 1 mismatch");
  assert(array.data[2] == 6.0f && "Element 2 mismatch");
  assert(array.data[3] == 7.0f && "Element 3 mismatch");

  array_pop_at_float(&array, 1, NULL);
  assert(array.length == 3 && "Length after pop at 1 mismatch");

  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_pop_at_float PASSED\n");
}

static bool8_t float_equals(float *current_value, float *value) {
  return *current_value == *value;
}

static bool8_t float_approx_equals(float *current_value, float *value) {
  const float tolerance = 0.01f;
  return fabsf(*current_value - *value) < tolerance;
}

static bool8_t float_greater_than(float *current_value, float *value) {
  return *current_value > *value;
}

static void test_array_find_float(void) {
  printf("  Running test_array_find_float...\n");
  setup_suite();

  Array_float array = array_create_float(&allocator, 16);
  assert(array_push_float(&array, 1.0f));
  assert(array_push_float(&array, 2.0f));
  assert(array_push_float(&array, 3.0f));

  float val = 2.0f;
  ArrayFindResult res = array_find_float(&array, &val, float_equals);
  assert(res.found && "Find 2.0f mismatch");
  assert(res.index == 1 && "Index of 2.0f mismatch");

  val = 4.0f;
  res = array_find_float(&array, &val, float_equals);
  assert(!res.found && "Find 4.0f mismatch");

  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_find_float PASSED\n");
}

static void test_array_find_with_custom_callbacks(void) {
  printf("  Running test_array_find_with_custom_callbacks...\n");
  setup_suite();

  Array_float array = array_create_float(&allocator, 16);
  assert(array_push_float(&array, 1.0f));
  assert(array_push_float(&array, 2.005f)); // Slightly off from 2.0
  assert(array_push_float(&array, 3.0f));
  assert(array_push_float(&array, 4.5f));

  // Test exact equality callback
  float val = 2.0f;
  ArrayFindResult res = array_find_float(&array, &val, float_equals);
  assert(!res.found && "Exact find should not match 2.005f");

  // Test approximate equality callback
  res = array_find_float(&array, &val, float_approx_equals);
  assert(res.found && "Approximate find should match 2.005f");
  assert(res.index == 1 && "Index of approximate match should be 1");

  // Test greater than callback
  val = 3.5f;
  res = array_find_float(&array, &val, float_greater_than);
  assert(res.found && "Should find first value greater than 3.5f");
  assert(res.index == 3 && "Index of first value > 3.5f should be 3 (4.5f)");

  // Test greater than callback with no matches
  val = 5.0f;
  res = array_find_float(&array, &val, float_greater_than);
  assert(!res.found && "Should not find any value greater than 5.0f");

  array_destroy_float(&array);

  teardown_suite();
  printf("  test_array_find_with_custom_callbacks PASSED\n");
}

static void test_array_find_edge_cases(void) {
  printf("  Running test_array_find_edge_cases...\n");
  setup_suite();

  // Test with empty array
  Array_float empty_array = array_create_float(&allocator, 16);
  float val = 1.0f;
  ArrayFindResult res = array_find_float(&empty_array, &val, float_equals);
  assert(!res.found && "Find in empty array should return not found");
  assert(res.index == 0 && "Index should be 0 when not found");

  // Test with single element
  assert(array_push_float(&empty_array, 42.0f));
  val = 42.0f;
  res = array_find_float(&empty_array, &val, float_equals);
  assert(res.found && "Should find single element");
  assert(res.index == 0 && "Index should be 0 for single element");

  val = 43.0f;
  res = array_find_float(&empty_array, &val, float_equals);
  assert(!res.found && "Should not find non-matching single element");

  array_destroy_float(&empty_array);

  teardown_suite();
  printf("  test_array_find_edge_cases PASSED\n");
}

static void test_array_alignment_survives_growth(void) {
  ContainerTestAllocator state = {0};
  VkrAllocator alloc = container_test_allocator(&state);
  Array_ArrayTestAligned array = array_create_ArrayTestAligned(&alloc, 1);
  assert(array.data && ((uintptr_t)array.data % 64u) == 0u);
  for (uint32_t i = 0; i < 40; ++i) {
    ArrayTestAligned element = {.value = {(float32_t)i}};
    assert(array_push_ArrayTestAligned(&array, element));
    assert(((uintptr_t)array.data % 64u) == 0u);
  }
  for (uint32_t i = 0; i < 40; ++i) {
    assert(array.data[i].value[0] == (float32_t)i);
  }
  array_destroy_ArrayTestAligned(&array);
  assert(state.live_bytes == 0);
}

static void test_array_allocation_failure_preserves_contents(void) {
  ContainerTestAllocator state = {.fail = true};
  VkrAllocator failing = container_test_allocator(&state);
  Array_float array = array_create_float(&failing, 16);
  assert(!array.data && !array.allocator && !array.capacity && !array.length);
  array_destroy_float(&array);
  Array_uint32_t filled = array_create_filled_uint32_t(&failing, 8);
  assert(!filled.data && !filled.allocator && !filled.length);

  uint64_t calls = state.calls;
  array = array_create_float(&failing, SIZE_MAX / sizeof(float) + 1);
  assert(!array.data && !array.allocator && state.calls == calls);
  filled =
      array_create_filled_uint32_t(&failing, SIZE_MAX / sizeof(uint32_t) + 1);
  assert(!filled.data && !filled.allocator && state.calls == calls);
  array = array_create_float(&failing, 0);
  assert(!array.data && array.allocator == &failing && state.calls == calls);

  array = (Array_float){.allocator = &failing};
  assert(!array_push_float(&array, 1.0f));
  assert(!array.data && !array.capacity && !array.length &&
         array.allocator == &failing);
  state.fail = false;
  assert(array_reserve_float(&array, 2));
  assert(array_push_float(&array, 3.0f));
  assert(array_push_float(&array, 7.0f));
  float *original = array.data;
  state.fail = true;
  assert(!array_push_float(&array, 11.0f));
  assert(!array_grow_float(&array));
  assert(!array_reserve_float(&array, 8));
  calls = state.calls;
  assert(!array_reserve_float(&array, SIZE_MAX / sizeof(float) + 1));
  assert(state.calls == calls);
  assert(array.data == original && array.capacity == 2 && array.length == 2);
  assert(array.data[0] == 3.0f && array.data[1] == 7.0f);
  assert(state.live_bytes == 2 * sizeof(float));

  state.fail = false;
  assert(array_reserve_float(&array, 4));
  state.fail = true;
  calls = state.calls;
  assert(array_reserve_float(&array, 3));
  assert(array_push_float(&array, 11.0f));
  assert(state.calls == calls && array.length == 3 && array.capacity == 4);
  assert(array.data[0] == 3.0f && array.data[1] == 7.0f &&
         array.data[2] == 11.0f);
  array_destroy_float(&array);
  assert(state.live_bytes == 0);
}

bool32_t run_array_tests() {
  printf("--- Starting Array Tests ---\n");

  test_array_allocation_failure_preserves_contents();
  test_array_alignment_survives_growth();
  test_array_create_float();
  test_array_create_filled_uint32_t();
  test_array_push_pop_float();
  test_array_get_set_float();
  test_array_grow_float();
  test_array_clear_float();
  test_array_pop_at_float();
  test_array_find_float();
  test_array_find_with_custom_callbacks();
  test_array_find_edge_cases();

  printf("--- Array Tests Completed ---\n");
  return true; // Assumes asserts halt on failure
}

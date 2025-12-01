#include "vector_test.h"
#include "memory/vkr_arena_allocator.h"

// Instantiate Vector for a specific type for testing
Vector(float);

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

static void test_vector_create_float(void) {
  printf("  Running test_vector_create_float...\n");
  setup_suite();

  Vector_float vec = vector_create_float(&allocator);

  assert(vec.allocator == &allocator && "Allocator pointer mismatch");
  assert(vec.capacity == DEFAULT_VECTOR_CAPACITY &&
         "Default capacity mismatch");
  assert(vec.length == 0 && "Initial length non-zero");
  assert(vec.data != NULL && "Data is NULL");

  vector_destroy_float(&vec);
  assert(vec.data == NULL && "Data not NULL after destroy");
  assert(vec.allocator == NULL && "Allocator not NULL after destroy");
  assert(vec.length == 0 && "Length not 0 after destroy");
  assert(vec.capacity == 0 && "Capacity not 0 after destroy");

  teardown_suite();
  printf("  test_vector_create_float PASSED\n");
}

static void test_vector_create_with_capacity_float(void) {
  printf("  Running test_vector_create_with_capacity_float...\n");
  setup_suite();

  const uint64_t initial_capacity = 5;
  Vector_float vec =
      vector_create_float_with_capacity(&allocator, initial_capacity);

  assert(vec.allocator == &allocator && "Allocator pointer mismatch");
  assert(vec.capacity == initial_capacity && "Capacity mismatch");
  assert(vec.length == 0 && "Initial length non-zero");
  assert(vec.data != NULL && "Data is NULL");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_create_with_capacity_float PASSED\n");
}

static void test_vector_push_pop_float(void) {
  printf("  Running test_vector_push_pop_float...\n");
  setup_suite();

  Vector_float vec = vector_create_float(&allocator);

  vector_push_float(&vec, 1.0f);
  vector_push_float(&vec, 2.5f);
  vector_push_float(&vec, -3.0f);

  assert(vec.length == 3 && "Length after pushes mismatch");

  float val = vector_pop_float(&vec);
  assert(val == -3.0f && "Pop 1 value mismatch");
  assert(vec.length == 2 && "Length after pop 1 mismatch");

  val = vector_pop_float(&vec);
  assert(val == 2.5f && "Pop 2 value mismatch");
  assert(vec.length == 1 && "Length after pop 2 mismatch");

  val = vector_pop_float(&vec);
  assert(val == 1.0f && "Pop 3 value mismatch");
  assert(vec.length == 0 && "Length after pop 3 mismatch");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_push_pop_float PASSED\n");
}

static void test_vector_get_set_float(void) {
  printf("  Running test_vector_get_set_float...\n");
  setup_suite();

  Vector_float vec = vector_create_float(&allocator);
  vector_push_float(&vec, 10.0f);
  vector_push_float(&vec, 20.0f);

  float *val_ptr = vector_get_float(&vec, 0);
  assert(val_ptr != NULL && "Got NULL pointer from get 0");
  assert(*val_ptr == 10.0f && "Get 0 value mismatch");

  vector_set_float(&vec, 1, 30.0f);
  val_ptr = vector_get_float(&vec, 1);
  assert(val_ptr != NULL && "Got NULL pointer from get 1");
  assert(*val_ptr == 30.0f && "Get 1 value mismatch after set");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_get_set_float PASSED\n");
}

static void test_vector_resize_float(void) {
  printf("  Running test_vector_resize_float...\n");
  setup_suite();

  const uint64_t initial_capacity = 2;
  Vector_float vec =
      vector_create_float_with_capacity(&allocator, initial_capacity);

  vector_push_float(&vec, 1.0f);
  vector_push_float(&vec, 2.0f);

  // Should trigger resize
  vector_push_float(&vec, 3.0f);

  assert(vec.length == 3 && "Length after resize mismatch");
  assert(vec.capacity == initial_capacity * DEFAULT_VECTOR_RESIZE_FACTOR &&
         "Capacity after resize mismatch");

  float *val_ptr = vector_get_float(&vec, 0);
  assert(*val_ptr == 1.0f && "Value 0 after resize mismatch");
  val_ptr = vector_get_float(&vec, 1);
  assert(*val_ptr == 2.0f && "Value 1 after resize mismatch");
  val_ptr = vector_get_float(&vec, 2);
  assert(*val_ptr == 3.0f && "Value 2 after resize mismatch");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_resize_float PASSED\n");
}

static void test_vector_clear_float(void) {
  printf("  Running test_vector_clear_float...\n");
  setup_suite();

  Vector_float vec = vector_create_float(&allocator);
  vector_push_float(&vec, 1.0f);
  vector_push_float(&vec, 2.0f);
  assert(vec.length == 2 && "Length before clear mismatch");

  vector_clear_float(&vec);
  assert(vec.length == 0 && "Length after clear mismatch");
  // Capacity should remain the same
  assert(vec.capacity == DEFAULT_VECTOR_CAPACITY &&
         "Capacity after clear mismatch");
  assert(vec.data != NULL && "Data NULL after clear");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_clear_float PASSED\n");
}

static void test_vector_pop_at_float(void) {
  printf("  Running test_vector_pop_at_float...\n");
  setup_suite();

  Vector_float vec = vector_create_float(&allocator);
  vector_push_float(&vec, 1.0f);
  vector_push_float(&vec, 2.0f);
  vector_push_float(&vec, 3.0f);

  float val = 0.0f;
  vector_pop_at_float(&vec, 1, &val);
  assert(val == 2.0f && "Pop at 1 value mismatch");
  assert(vec.length == 2 && "Length after pop at 1 mismatch");

  vector_pop_at_float(&vec, 1, &val);
  assert(val == 3.0f && "Pop at 1 value mismatch");
  assert(vec.length == 1 && "Length after pop at 1 mismatch");

  vector_push_float(&vec, 4.0f);
  vector_push_float(&vec, 5.0f);
  vector_push_float(&vec, 6.0f);
  vector_push_float(&vec, 7.0f);

  vector_pop_at_float(&vec, 1, &val);
  assert(val == 4.0f && "Pop at 1 value mismatch");
  assert(vec.length == 4 && "Length after pop at 1 mismatch");

  assert(vec.data[0] == 1.0f && "Element 0 mismatch");
  assert(vec.data[1] == 5.0f && "Element 1 mismatch");
  assert(vec.data[2] == 6.0f && "Element 2 mismatch");
  assert(vec.data[3] == 7.0f && "Element 3 mismatch");

  vector_pop_at_float(&vec, 1, NULL);
  assert(vec.length == 3 && "Length after pop at 1 mismatch");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_pop_at_float PASSED\n");
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

static void test_vector_find_float(void) {
  printf("  Running test_vector_find_float...\n");
  setup_suite();

  Vector_float vec = vector_create_float(&allocator);
  vector_push_float(&vec, 1.0f);
  vector_push_float(&vec, 2.0f);
  vector_push_float(&vec, 3.0f);

  float val = 2.0f;
  VectorFindResult res = vector_find_float(&vec, &val, float_equals);
  assert(res.found && "Find 2.0f mismatch");
  assert(res.index == 1 && "Index of 2.0f mismatch");

  val = 4.0f;
  res = vector_find_float(&vec, &val, float_equals);
  assert(!res.found && "Find 4.0f mismatch");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_find_float PASSED\n");
}

static void test_vector_find_with_custom_callbacks(void) {
  printf("  Running test_vector_find_with_custom_callbacks...\n");
  setup_suite();

  Vector_float vec = vector_create_float(&allocator);
  vector_push_float(&vec, 1.0f);
  vector_push_float(&vec, 2.005f); // Slightly off from 2.0
  vector_push_float(&vec, 3.0f);
  vector_push_float(&vec, 4.5f);

  // Test exact equality callback
  float val = 2.0f;
  VectorFindResult res = vector_find_float(&vec, &val, float_equals);
  assert(!res.found && "Exact find should not match 2.005f");

  // Test approximate equality callback
  res = vector_find_float(&vec, &val, float_approx_equals);
  assert(res.found && "Approximate find should match 2.005f");
  assert(res.index == 1 && "Index of approximate match should be 1");

  // Test greater than callback
  val = 3.5f;
  res = vector_find_float(&vec, &val, float_greater_than);
  assert(res.found && "Should find first value greater than 3.5f");
  assert(res.index == 3 && "Index of first value > 3.5f should be 3 (4.5f)");

  // Test greater than callback with no matches
  val = 5.0f;
  res = vector_find_float(&vec, &val, float_greater_than);
  assert(!res.found && "Should not find any value greater than 5.0f");

  vector_destroy_float(&vec);

  teardown_suite();
  printf("  test_vector_find_with_custom_callbacks PASSED\n");
}

static void test_vector_find_edge_cases(void) {
  printf("  Running test_vector_find_edge_cases...\n");
  setup_suite();

  // Test with empty vector
  Vector_float empty_vec = vector_create_float(&allocator);
  float val = 1.0f;
  VectorFindResult res = vector_find_float(&empty_vec, &val, float_equals);
  assert(!res.found && "Find in empty vector should return not found");
  assert(res.index == 0 && "Index should be 0 when not found");

  // Test with single element
  vector_push_float(&empty_vec, 42.0f);
  val = 42.0f;
  res = vector_find_float(&empty_vec, &val, float_equals);
  assert(res.found && "Should find single element");
  assert(res.index == 0 && "Index should be 0 for single element");

  val = 43.0f;
  res = vector_find_float(&empty_vec, &val, float_equals);
  assert(!res.found && "Should not find non-matching single element");

  vector_destroy_float(&empty_vec);

  teardown_suite();
  printf("  test_vector_find_edge_cases PASSED\n");
}

// Test runner for this suite
bool32_t run_vector_tests() {
  printf("--- Starting Vector Tests ---\n");

  test_vector_create_float();
  test_vector_create_with_capacity_float();
  test_vector_push_pop_float();
  test_vector_get_set_float();
  test_vector_resize_float();
  test_vector_clear_float();
  test_vector_pop_at_float();
  test_vector_find_float();
  test_vector_find_with_custom_callbacks();
  test_vector_find_edge_cases();

  printf("--- Vector Tests Completed ---\n");
  return true; // Assumes asserts halt on failure
}

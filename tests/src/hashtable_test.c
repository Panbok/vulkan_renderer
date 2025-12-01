#include "hashtable_test.h"
#include "memory/vkr_arena_allocator.h"

static Arena *arena = NULL;
static VkrAllocator allocator = {0};
static const uint64_t ARENA_SIZE = MB(1);

// Setup function called before each test function in this suite
static void setup_suite(void) {
  arena = arena_create(ARENA_SIZE, ARENA_SIZE);
  assert(arena && "arena_create failed");
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

/////////////////////
// HashTable Tests
/////////////////////

static void test_hash_table_create(void) {
  printf("  Running test_hash_table_create...\n");
  setup_suite();
  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&allocator, 10);
  assert(table.capacity == 10 && "Hash table capacity is not 10");
  assert(table.size == 0 && "Hash table size is not 0");
  assert(table.entries != NULL && "Hash table entries is NULL");
  vkr_hash_table_destroy_uint8_t(&table);
  teardown_suite();
  printf("  test_hash_table_create PASSED\n");
}

static void test_hash_table_insert_get_contains_remove(void) {
  printf("  Running test_hash_table_insert_get_contains_remove...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&allocator, 8);

  // Insert a few keys
  assert(vkr_hash_table_insert_uint8_t(&table, "alpha", 11));
  assert(vkr_hash_table_insert_uint8_t(&table, "beta", 22));
  assert(vkr_hash_table_insert_uint8_t(&table, "gamma", 33));
  assert(table.size == 3);

  // Contains
  assert(vkr_hash_table_contains_uint8_t(&table, "alpha"));
  assert(vkr_hash_table_contains_uint8_t(&table, "beta"));
  assert(vkr_hash_table_contains_uint8_t(&table, "gamma"));
  assert(!vkr_hash_table_contains_uint8_t(&table, "delta"));

  // Get
  uint8_t *val = NULL;
  val = vkr_hash_table_get_uint8_t(&table, "alpha");
  assert(val && *val == 11);
  val = vkr_hash_table_get_uint8_t(&table, "beta");
  assert(val && *val == 22);
  val = vkr_hash_table_get_uint8_t(&table, "gamma");
  assert(val && *val == 33);
  val = vkr_hash_table_get_uint8_t(&table, "delta");
  assert(val == NULL);

  // Remove
  assert(vkr_hash_table_remove_uint8_t(&table, "beta"));
  assert(table.size == 2);
  assert(!vkr_hash_table_contains_uint8_t(&table, "beta"));
  assert(vkr_hash_table_get_uint8_t(&table, "beta") == NULL);

  // Removing non-existent should fail
  assert(!vkr_hash_table_remove_uint8_t(&table, "does-not-exist"));

  vkr_hash_table_destroy_uint8_t(&table);
  teardown_suite();
  printf("  test_hash_table_insert_get_contains_remove PASSED\n");
}

static void test_hash_table_reset_and_empty(void) {
  printf("  Running test_hash_table_reset_and_empty...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&allocator, 4);
  assert(vkr_hash_table_is_empty_uint8_t(&table));
  assert(vkr_hash_table_insert_uint8_t(&table, "k1", 1));
  assert(vkr_hash_table_insert_uint8_t(&table, "k2", 2));
  assert(!vkr_hash_table_is_empty_uint8_t(&table));

  vkr_hash_table_reset_uint8_t(&table);
  assert(vkr_hash_table_is_empty_uint8_t(&table));
  assert(vkr_hash_table_get_uint8_t(&table, "k1") == NULL);
  assert(vkr_hash_table_get_uint8_t(&table, "k2") == NULL);

  vkr_hash_table_destroy_uint8_t(&table);
  teardown_suite();
  printf("  test_hash_table_reset_and_empty PASSED\n");
}

static void test_hash_table_collision_linear_probing(void) {
  printf("  Running test_hash_table_collision_linear_probing...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&allocator, 4);

  const char *candidates[] = {
      "a",  "b",  "c",  "d",  "e",  "f",  "g",  "h",  "i",  "j",  "k",
      "l",  "m",  "n",  "o",  "p",  "q",  "r",  "s",  "t",  "u",  "v",
      "w",  "x",  "y",  "z",  "aa", "ab", "ac", "ad", "ae", "af", "ag",
      "ah", "ai", "aj", "ak", "al", "am", "an", "ao", "ap", "aq", "ar",
      "as", "at", "au", "av", "aw", "ax", "ay", "az"};
  const size_t candidate_count = sizeof(candidates) / sizeof(candidates[0]);

  const char *k1 = NULL;
  const char *k2 = NULL;
  uint64_t seen_index[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
  const char *seen_key[4] = {NULL, NULL, NULL, NULL};
  for (size_t i = 0; i < candidate_count; i++) {
    uint64_t idx = vkr_hash_name_uint8_t(candidates[i], table.capacity);
    if (seen_index[idx] == UINT64_MAX) {
      seen_index[idx] = idx;
      seen_key[idx] = candidates[i];
    } else {
      k1 = seen_key[idx];
      k2 = candidates[i];
      break;
    }
  }
  assert(k1 && k2 && "Failed to find colliding keys for test");

  assert(vkr_hash_table_insert_uint8_t(&table, k1, 1));
  assert(vkr_hash_table_insert_uint8_t(&table, k2, 2));
  assert(table.size == 2);
  assert(vkr_hash_table_contains_uint8_t(&table, k1));
  assert(vkr_hash_table_contains_uint8_t(&table, k2));
  assert(*vkr_hash_table_get_uint8_t(&table, k1) == 1);
  assert(*vkr_hash_table_get_uint8_t(&table, k2) == 2);

  vkr_hash_table_destroy_uint8_t(&table);
  teardown_suite();
  printf("  test_hash_table_collision_linear_probing PASSED\n");
}

static void test_hash_table_resize_behavior(void) {
  printf("  Running test_hash_table_resize_behavior...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&allocator, 4);

  assert(vkr_hash_table_insert_uint8_t(&table, "k1", 1));
  assert(vkr_hash_table_insert_uint8_t(&table, "k2", 2));
  assert(vkr_hash_table_insert_uint8_t(&table, "k3", 3));
  // Next insert should trigger resize from 4 -> 8 due to 0.75 load factor
  assert(vkr_hash_table_insert_uint8_t(&table, "k4", 4));
  assert(table.capacity >= 8);
  assert(table.size == 4);
  assert(vkr_hash_table_contains_uint8_t(&table, "k1"));
  assert(vkr_hash_table_contains_uint8_t(&table, "k2"));
  assert(vkr_hash_table_contains_uint8_t(&table, "k3"));
  assert(vkr_hash_table_contains_uint8_t(&table, "k4"));

  assert(*vkr_hash_table_get_uint8_t(&table, "k1") == 1);
  assert(*vkr_hash_table_get_uint8_t(&table, "k2") == 2);
  assert(*vkr_hash_table_get_uint8_t(&table, "k3") == 3);
  assert(*vkr_hash_table_get_uint8_t(&table, "k4") == 4);

  vkr_hash_table_destroy_uint8_t(&table);
  teardown_suite();
  printf("  test_hash_table_resize_behavior PASSED\n");
}

static void test_hash_table_update_and_remove_reuse(void) {
  printf("  Running test_hash_table_update_and_remove_reuse...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&allocator, 4);

  assert(vkr_hash_table_insert_uint8_t(&table, "alpha", 1));
  assert(table.size == 1);
  // Update existing key should not change size
  assert(vkr_hash_table_insert_uint8_t(&table, "alpha", 2));
  assert(table.size == 1);
  assert(*vkr_hash_table_get_uint8_t(&table, "alpha") == 2);

  assert(vkr_hash_table_insert_uint8_t(&table, "beta", 3));
  assert(table.size == 2);

  // Remove and re-insert to verify tombstone reuse and correctness
  assert(vkr_hash_table_remove_uint8_t(&table, "alpha"));
  assert(table.size == 1);
  assert(!vkr_hash_table_contains_uint8_t(&table, "alpha"));
  assert(vkr_hash_table_get_uint8_t(&table, "alpha") == NULL);

  assert(vkr_hash_table_insert_uint8_t(&table, "alpha", 4));
  assert(table.size == 2);
  assert(vkr_hash_table_contains_uint8_t(&table, "alpha"));
  assert(*vkr_hash_table_get_uint8_t(&table, "alpha") == 4);

  vkr_hash_table_destroy_uint8_t(&table);
  teardown_suite();
  printf("  test_hash_table_update_and_remove_reuse PASSED\n");
}

bool32_t run_hashtable_tests() {
  printf("--- Starting HashTable Tests ---\n");
  test_hash_table_create();
  test_hash_table_insert_get_contains_remove();
  test_hash_table_reset_and_empty();
  test_hash_table_collision_linear_probing();
  test_hash_table_resize_behavior();
  test_hash_table_update_and_remove_reuse();
  printf("--- HashTable Tests Completed ---\n");
  return true;
}

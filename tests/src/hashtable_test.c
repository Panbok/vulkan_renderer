#include "hashtable_test.h"

static Arena *arena = NULL;
static const uint64_t ARENA_SIZE = MB(1);

// Setup function called before each test function in this suite
static void setup_suite(void) {
  arena = arena_create(ARENA_SIZE, ARENA_SIZE);
  assert(arena && "arena_create failed");
}

// Teardown function called after each test function in this suite
static void teardown_suite(void) {
  if (arena) {
    arena_destroy(arena);
    arena = NULL;
  }
}

/////////////////////
// HashTable Tests
/////////////////////

static void test_hash_table_create(void) {
  printf("  Running test_hash_table_create...\n");
  setup_suite();
  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(arena, 10);
  assert(table.capacity == 10 && "Hash table capacity is not 10");
  assert(table.size == 0 && "Hash table size is not 0");
  assert(table.data != NULL && "Hash table data is NULL");
  assert(table.occupied != NULL && "Hash table occupancy is NULL");
  teardown_suite();
  printf("  test_hash_table_create PASSED\n");
}

static void test_hash_table_insert_get_contains_remove(void) {
  printf("  Running test_hash_table_insert_get_contains_remove...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(arena, 8);

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

  teardown_suite();
  printf("  test_hash_table_insert_get_contains_remove PASSED\n");
}

static void test_hash_table_reset_and_empty(void) {
  printf("  Running test_hash_table_reset_and_empty...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(arena, 4);
  assert(vkr_hash_table_is_empty_uint8_t(&table));
  assert(vkr_hash_table_insert_uint8_t(&table, "k1", 1));
  assert(vkr_hash_table_insert_uint8_t(&table, "k2", 2));
  assert(!vkr_hash_table_is_empty_uint8_t(&table));

  vkr_hash_table_reset_uint8_t(&table);
  assert(vkr_hash_table_is_empty_uint8_t(&table));
  assert(vkr_hash_table_get_uint8_t(&table, "k1") == NULL);
  assert(vkr_hash_table_get_uint8_t(&table, "k2") == NULL);

  teardown_suite();
  printf("  test_hash_table_reset_and_empty PASSED\n");
}

bool32_t run_hashtable_tests() {
  printf("--- Starting HashTable Tests ---\n");
  test_hash_table_create();
  test_hash_table_insert_get_contains_remove();
  test_hash_table_reset_and_empty();
  printf("--- HashTable Tests Completed ---\n");
  return true;
}
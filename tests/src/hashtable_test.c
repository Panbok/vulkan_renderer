#include "hashtable_test.h"
#include "container_test_allocator.h"
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
  assert(table.capacity == 16 && "Capacity rounds up to a power of two");
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
    uint64_t idx =
        vkr_hash_table_hash_cstr(candidates[i]) & (table.capacity - 1u);
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

static void test_hash_table_get_string8_view(void) {
  printf("  Running test_hash_table_get_string8_view...\n");
  setup_suite();

  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&allocator, 16);
  assert(vkr_hash_table_insert_uint8_t(&table, "textures/a.png", 1));
  assert(vkr_hash_table_insert_uint8_t(&table, "textures/a", 2));

  // Views into a longer request are not terminated at their length; a C-string
  // lookup on the same bytes would read the query and miss.
  char request[] = "textures/a.png?cs=srgb";
  uint8_t *value = vkr_hash_table_get_string8_uint8_t(
      &table, string8_create((uint8_t *)request, 14));
  assert(value && *value == 1);
  value = vkr_hash_table_get_string8_uint8_t(
      &table, string8_create((uint8_t *)request, 10));
  assert(value && *value == 2);
  assert(!vkr_hash_table_get_string8_uint8_t(
      &table, string8_create((uint8_t *)request, 12)));
  assert(!vkr_hash_table_get_string8_uint8_t(&table, string8_lit(request)));
  assert(!vkr_hash_table_get_string8_uint8_t(&table, (String8){0}));

  vkr_hash_table_destroy_uint8_t(&table);
  teardown_suite();
  printf("  test_hash_table_get_string8_view PASSED\n");
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

static void test_hash_table_allocation_failure_preserves_mappings(void) {
  ContainerTestAllocator state = {.fail = true};
  VkrAllocator failing = container_test_allocator(&state);
  VkrHashTable_uint8_t table = vkr_hash_table_create_uint8_t(&failing, 4);
  assert(!table.entries && !table.allocator && !table.size && !table.capacity);
  vkr_hash_table_destroy_uint8_t(&table);
  uint64_t calls = state.calls;
  table = vkr_hash_table_create_uint8_t(
      &failing, SIZE_MAX / sizeof(VkrHashEntry_uint8_t) + 1);
  assert(!table.entries && !table.allocator && state.calls == calls);
  table = vkr_hash_table_create_uint8_t(&failing, 0);
  assert(!table.entries && !table.allocator && state.calls == calls);

  state.fail = false;
  table = vkr_hash_table_create_uint8_t(&failing, 4);
  assert(table.entries);
  assert(vkr_hash_table_insert_uint8_t(&table, "alpha", 3));
  assert(vkr_hash_table_insert_uint8_t(&table, "beta", 7));
  assert(vkr_hash_table_insert_uint8_t(&table, "gamma", 11));
  VkrHashEntry_uint8_t *original = table.entries;
  state.fail = true;
  assert(!vkr_hash_table_insert_uint8_t(&table, "delta", 17));
  assert(!vkr_hash_table_resize_uint8_t(&table, 8));
  calls = state.calls;
  assert(!vkr_hash_table_resize_uint8_t(
      &table, SIZE_MAX / sizeof(VkrHashEntry_uint8_t) + 1));
  assert(state.calls == calls);
  assert(table.entries == original && table.capacity == 4 && table.size == 3);
  assert(*vkr_hash_table_get_uint8_t(&table, "alpha") == 3);
  assert(*vkr_hash_table_get_uint8_t(&table, "beta") == 7);
  assert(*vkr_hash_table_get_uint8_t(&table, "gamma") == 11);
  assert(!vkr_hash_table_get_uint8_t(&table, "delta"));
  assert(state.live_bytes == 4 * sizeof(*table.entries));

  // An update at the growth threshold needs no allocation or key replacement.
  const char replacement_key[] = "beta";
  assert(vkr_hash_table_insert_uint8_t(&table, replacement_key, 19));
  assert(state.calls == calls && table.entries == original && table.size == 3);
  assert(*vkr_hash_table_get_uint8_t(&table, "beta") == 19);
  for (uint64_t i = 0; i < table.capacity; ++i) {
    assert(table.entries[i].key != replacement_key);
  }
  state.fail = false;
  assert(vkr_hash_table_insert_uint8_t(&table, "delta", 17));
  assert(table.capacity == 8 && table.size == 4);
  assert(*vkr_hash_table_get_uint8_t(&table, "beta") == 19);
  assert(*vkr_hash_table_get_uint8_t(&table, "delta") == 17);
  vkr_hash_table_destroy_uint8_t(&table);
  assert(state.live_bytes == 0);
}

/* Finds `count` keys whose hash maps to `home` in a table of `capacity`. */
static void hash_test_colliding_keys(char (*keys)[32], uint32_t count,
                                     const char *prefix, uint64_t capacity,
                                     uint64_t home, uint32_t *candidate) {
  for (uint32_t i = 0; i < count; ++i) {
    do {
      snprintf(keys[i], sizeof(keys[i]), "%s-%u", prefix, (*candidate)++);
    } while ((vkr_hash_table_hash_cstr(keys[i]) & (capacity - 1u)) != home);
  }
}

static void test_hash_table_failed_rehash_preserves_entries(void) {
  ContainerTestAllocator state = {0};
  VkrAllocator alloc = container_test_allocator(&state);
  VkrHashTable_uint32_t table = vkr_hash_table_create_uint32_t(&alloc, 1024);
  assert(table.entries);
  char keys[129][32];
  uint32_t candidate = 0;
  hash_test_colliding_keys(keys, ArrayCount(keys), "rehash", 256, 0,
                           &candidate);
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    assert(vkr_hash_table_insert_uint32_t(&table, keys[i], i));
  }
  VkrHashEntry_uint32_t *original = table.entries;
  uint64_t bytes = state.live_bytes;
  uint64_t calls = state.calls;
  // Three quarters of 128 slots cannot hold 129 keys; nothing is allocated.
  assert(!vkr_hash_table_resize_uint32_t(&table, 128));
  assert(state.calls == calls);
  state.fail = true;
  assert(!vkr_hash_table_resize_uint32_t(&table, 256));
  state.fail = false;
  assert(table.entries == original && table.capacity == 1024);
  assert(table.size == ArrayCount(keys) && state.live_bytes == bytes);
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    uint32_t *value = vkr_hash_table_get_uint32_t(&table, keys[i]);
    assert(value && *value == i);
  }
  // The colliding keys share one probe run after the shrink.
  assert(vkr_hash_table_resize_uint32_t(&table, 256));
  assert(table.capacity == 256 && table.size == ArrayCount(keys));
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    uint32_t *value = vkr_hash_table_get_uint32_t(&table, keys[i]);
    assert(value && *value == i);
  }
  vkr_hash_table_destroy_uint32_t(&table);
  assert(state.live_bytes == 0);
}

static void test_hash_table_long_probe_run_inserts(void) {
  ContainerTestAllocator state = {0};
  VkrAllocator alloc = container_test_allocator(&state);
  VkrHashTable_uint32_t table = vkr_hash_table_create_uint32_t(&alloc, 1024);
  assert(table.entries);
  // 200 keys with one home slot form a probe run longer than 128 slots while
  // the table is a fifth full. Every insert must succeed without growth.
  static char collisions[200][32];
  uint32_t candidate = 0;
  hash_test_colliding_keys(collisions, ArrayCount(collisions), "run", 1024, 0,
                           &candidate);
  for (uint32_t i = 0; i < ArrayCount(collisions); ++i) {
    assert(vkr_hash_table_insert_uint32_t(&table, collisions[i], i));
  }
  assert(table.size == ArrayCount(collisions) && table.capacity == 1024);
  for (uint32_t i = 0; i < ArrayCount(collisions); ++i) {
    uint32_t *value = vkr_hash_table_get_uint32_t(&table, collisions[i]);
    assert(value && *value == i);
  }
  // A key removed from the middle of the run returns without allocating.
  VkrHashEntry_uint32_t *original = table.entries;
  state.fail = true;
  assert(vkr_hash_table_remove_uint32_t(&table, collisions[100]));
  assert(!vkr_hash_table_get_uint32_t(&table, collisions[100]));
  for (uint32_t i = 101; i < ArrayCount(collisions); ++i) {
    uint32_t *value = vkr_hash_table_get_uint32_t(&table, collisions[i]);
    assert(value && *value == i);
  }
  assert(vkr_hash_table_insert_uint32_t(&table, collisions[100], 100));
  assert(table.entries == original && table.size == ArrayCount(collisions));
  state.fail = false;
  vkr_hash_table_destroy_uint32_t(&table);
  assert(state.live_bytes == 0);
}

static void test_hash_table_churn_keeps_capacity(void) {
  ContainerTestAllocator state = {0};
  VkrAllocator alloc = container_test_allocator(&state);
  VkrHashTable_uint32_t table = vkr_hash_table_create_uint32_t(&alloc, 128);
  assert(table.entries);
  // Unique keys pass through the table with at most 16 live, as resource
  // requests do. Removed keys stay absent, live keys stay found and the
  // capacity does not grow.
  static char keys[4000][32];
  const uint32_t live = 16;
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    snprintf(keys[i], sizeof(keys[i]), "assets/textures/tex_%05u.ktx2", i);
    assert(vkr_hash_table_insert_uint32_t(&table, keys[i], i));
    if (i >= live) {
      assert(vkr_hash_table_remove_uint32_t(&table, keys[i - live]));
      assert(!vkr_hash_table_get_uint32_t(&table, keys[i - live]));
    }
    assert(table.capacity == 128);
  }
  assert(table.size == live);
  for (uint32_t i = ArrayCount(keys) - live; i < ArrayCount(keys); ++i) {
    uint32_t *value = vkr_hash_table_get_uint32_t(&table, keys[i]);
    assert(value && *value == i);
  }
  vkr_hash_table_destroy_uint32_t(&table);
  assert(state.live_bytes == 0);
}

static void test_hash_table_string8_hash_matches_cstr(void) {
  char request[] = "materials/brick.mt?variant=wet";
  const String8 view = string8_create((uint8_t *)request, 18);
  assert(vkr_hash_table_key_string8(view).hash ==
         vkr_hash_table_hash_cstr("materials/brick.mt"));
  // Keys that differ only in the high bits of one byte take different slots
  // in a small table.
  const char *keys[] = {"mat_a", "mat_q", "mat_A", "mat_Q"};
  uint64_t distinct = 0;
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    bool8_t repeated = false_v;
    for (uint32_t j = 0; j < i; ++j) {
      repeated |= (vkr_hash_table_hash_cstr(keys[i]) & 15u) ==
                  (vkr_hash_table_hash_cstr(keys[j]) & 15u);
    }
    distinct += repeated ? 0u : 1u;
  }
  assert(distinct > 1u);
}

bool32_t run_hashtable_tests() {
  printf("--- Starting HashTable Tests ---\n");
  test_hash_table_allocation_failure_preserves_mappings();
  test_hash_table_failed_rehash_preserves_entries();
  test_hash_table_long_probe_run_inserts();
  test_hash_table_churn_keeps_capacity();
  test_hash_table_string8_hash_matches_cstr();
  test_hash_table_create();
  test_hash_table_insert_get_contains_remove();
  test_hash_table_reset_and_empty();
  test_hash_table_collision_linear_probing();
  test_hash_table_get_string8_view();
  test_hash_table_resize_behavior();
  test_hash_table_update_and_remove_reuse();
  printf("--- HashTable Tests Completed ---\n");
  return true;
}

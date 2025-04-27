#include "bitset_test.h"

typedef enum uint8_t {
  ENTITY_FLAG_NONE = 0,         // Useful for representing no flags
  ENTITY_FLAG_ACTIVE = 1 << 0,  // Bit 0 (Value 1)
  ENTITY_FLAG_VISIBLE = 1 << 1, // Bit 1 (Value 2)
  ENTITY_FLAG_STATIC = 1 << 2,  // Bit 2 (Value 4)
  ENTITY_FLAG_PLAYER = 1 << 3,  // Bit 3 (Value 8)
} EntityFlags;

static void test_bitset8_create() {
  printf("  Running test_bitset8_create...\n");
  Bitset8 bitset = bitset8_create();
  assert(bitset.set == ENTITY_FLAG_NONE);
  printf("  test_bitset8_create PASSED\n");
}

static void test_bitset8_set() {
  printf("  Running test_bitset8_set...\n");
  Bitset8 bitset = bitset8_create();
  bitset8_set(&bitset, ENTITY_FLAG_ACTIVE);
  assert(bitset.set == ENTITY_FLAG_ACTIVE);
  printf("  test_bitset8_set PASSED\n");
}

static void test_bitset8_clear() {
  printf("  Running test_bitset8_clear...\n");
  Bitset8 bitset = bitset8_create();
  bitset8_set(&bitset, ENTITY_FLAG_ACTIVE);
  bitset8_clear(&bitset, ENTITY_FLAG_ACTIVE);
  assert(bitset.set == ENTITY_FLAG_NONE);
  printf("  test_bitset8_clear PASSED\n");
}

static void test_bitset8_toggle() {
  printf("  Running test_bitset8_toggle...\n");
  Bitset8 bitset = bitset8_create();
  bitset8_set(&bitset, ENTITY_FLAG_ACTIVE);
  bitset8_toggle(&bitset, ENTITY_FLAG_ACTIVE);
  assert(bitset.set == ENTITY_FLAG_NONE);
  printf("  test_bitset8_toggle PASSED\n");
}

static void test_bitset8_is_set() {
  printf("  Running test_bitset8_is_set...\n");
  Bitset8 bitset = bitset8_create();
  bitset8_set(&bitset, ENTITY_FLAG_ACTIVE);
  assert(bitset8_is_set(&bitset, ENTITY_FLAG_ACTIVE));
  printf("  test_bitset8_is_set PASSED\n");
}

static void test_bitset8_get_value() {
  printf("  Running test_bitset8_get_value...\n");
  Bitset8 bitset = bitset8_create();
  bitset8_set(&bitset, ENTITY_FLAG_ACTIVE);
  assert(bitset8_get_value(&bitset) == ENTITY_FLAG_ACTIVE);
  printf("  test_bitset8_get_value PASSED\n");
}

bool32_t run_bitset_tests() {
  printf("--- Starting Bitset Tests ---\n");

  test_bitset8_create();
  test_bitset8_set();
  test_bitset8_clear();
  test_bitset8_toggle();
  test_bitset8_is_set();
  test_bitset8_get_value();

  printf("--- Bitset Tests Completed ---\n");
  return true;
}

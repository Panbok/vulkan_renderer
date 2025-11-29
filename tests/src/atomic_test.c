#include "atomic_test.h"

static void test_atomic_bool_ops(void) {
  printf("  Running test_atomic_bool_ops...\n");

  VkrAtomicBool flag = false_v;
  vkr_atomic_bool_store(&flag, true_v, VKR_MEMORY_ORDER_RELAXED);
  assert(vkr_atomic_bool_load(&flag, VKR_MEMORY_ORDER_RELAXED) == true_v);

  bool32_t prev =
      vkr_atomic_bool_exchange(&flag, false_v, VKR_MEMORY_ORDER_ACQ_REL);
  assert(prev == true_v);
  assert(vkr_atomic_bool_load(&flag, VKR_MEMORY_ORDER_RELAXED) == false_v);

  bool32_t expected = false_v;
  assert(vkr_atomic_bool_compare_exchange(&flag, &expected, true_v,
                                          VKR_MEMORY_ORDER_ACQ_REL,
                                          VKR_MEMORY_ORDER_ACQUIRE));
  assert(vkr_atomic_bool_load(&flag, VKR_MEMORY_ORDER_RELAXED) == true_v);

  expected = false_v;
  bool32_t swapped =
      vkr_atomic_bool_compare_exchange(&flag, &expected, false_v,
                                       VKR_MEMORY_ORDER_ACQ_REL,
                                       VKR_MEMORY_ORDER_ACQUIRE);
  assert(swapped == false_v);
  assert(expected == true_v);
  assert(vkr_atomic_bool_load(&flag, VKR_MEMORY_ORDER_RELAXED) == true_v);

  printf("  test_atomic_bool_ops PASSED\n");
}

static void test_atomic_int32_ops(void) {
  printf("  Running test_atomic_int32_ops...\n");

  VkrAtomicInt32 value = 0;
  int32_t prev = vkr_atomic_int32_fetch_add(&value, 5, VKR_MEMORY_ORDER_RELAXED);
  assert(prev == 0);
  assert(vkr_atomic_int32_load(&value, VKR_MEMORY_ORDER_RELAXED) == 5);

  prev = vkr_atomic_int32_fetch_sub(&value, 2, VKR_MEMORY_ORDER_RELAXED);
  assert(prev == 5);
  assert(vkr_atomic_int32_load(&value, VKR_MEMORY_ORDER_RELAXED) == 3);

  int32_t expected = 3;
  assert(vkr_atomic_int32_compare_exchange(&value, &expected, 8,
                                           VKR_MEMORY_ORDER_ACQ_REL,
                                           VKR_MEMORY_ORDER_ACQUIRE));
  assert(vkr_atomic_int32_load(&value, VKR_MEMORY_ORDER_RELAXED) == 8);

  expected = 1;
  bool32_t exchanged =
      vkr_atomic_int32_compare_exchange(&value, &expected, 12,
                                        VKR_MEMORY_ORDER_ACQ_REL,
                                        VKR_MEMORY_ORDER_ACQUIRE);
  assert(exchanged == false_v);
  assert(expected == 8);
  assert(vkr_atomic_int32_load(&value, VKR_MEMORY_ORDER_RELAXED) == 8);

  prev = vkr_atomic_int32_exchange(&value, -4, VKR_MEMORY_ORDER_SEQ_CST);
  assert(prev == 8);
  assert(vkr_atomic_int32_load(&value, VKR_MEMORY_ORDER_RELAXED) == -4);

  printf("  test_atomic_int32_ops PASSED\n");
}

static void test_atomic_uint64_ops(void) {
  printf("  Running test_atomic_uint64_ops...\n");

  VkrAtomicUint64 value = 0;
  uint64_t prev =
      vkr_atomic_uint64_fetch_add(&value, 100, VKR_MEMORY_ORDER_RELAXED);
  assert(prev == 0);
  assert(vkr_atomic_uint64_load(&value, VKR_MEMORY_ORDER_RELAXED) == 100);

  prev = vkr_atomic_uint64_fetch_sub(&value, 40, VKR_MEMORY_ORDER_RELAXED);
  assert(prev == 100);
  assert(vkr_atomic_uint64_load(&value, VKR_MEMORY_ORDER_RELAXED) == 60);

  uint64_t expected = 60;
  assert(vkr_atomic_uint64_compare_exchange(&value, &expected, 500,
                                            VKR_MEMORY_ORDER_ACQ_REL,
                                            VKR_MEMORY_ORDER_ACQUIRE));
  assert(vkr_atomic_uint64_load(&value, VKR_MEMORY_ORDER_RELAXED) == 500);

  expected = 10;
  bool32_t swapped =
      vkr_atomic_uint64_compare_exchange(&value, &expected, 900,
                                         VKR_MEMORY_ORDER_ACQ_REL,
                                         VKR_MEMORY_ORDER_ACQUIRE);
  assert(swapped == false_v);
  assert(expected == 500);
  assert(vkr_atomic_uint64_load(&value, VKR_MEMORY_ORDER_RELAXED) == 500);

  prev = vkr_atomic_uint64_exchange(&value, 42, VKR_MEMORY_ORDER_SEQ_CST);
  assert(prev == 500);
  assert(vkr_atomic_uint64_load(&value, VKR_MEMORY_ORDER_RELAXED) == 42);

  printf("  test_atomic_uint64_ops PASSED\n");
}

bool32_t run_atomic_tests(void) {
  printf("--- Running Atomic tests... ---\n");
  test_atomic_bool_ops();
  test_atomic_int32_ops();
  test_atomic_uint64_ops();
  printf("--- Atomic tests completed. ---\n");
  return true_v;
}

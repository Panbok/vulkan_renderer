#include "allocator_test.h"

static VkrAllocatorStatistics snapshot_global(void) {
  return vkr_allocator_get_global_statistics();
}

static void test_arena_scope_stats_reset(void) {
  printf("  Running test_arena_scope_stats_reset...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrAllocatorStatistics global_before = snapshot_global();
  VkrAllocatorStatistics local_before =
      vkr_allocator_get_statistics(&allocator);

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&scope));

  const uint64_t array_size = KB(4);
  const uint64_t string_size = KB(1);
  void *arr_mem = vkr_allocator_alloc(&allocator, array_size,
                                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void *str_mem = vkr_allocator_alloc(&allocator, string_size,
                                      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  assert(arr_mem != NULL && str_mem != NULL);

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  VkrAllocatorStatistics global_after = snapshot_global();
  VkrAllocatorStatistics local_after = vkr_allocator_get_statistics(&allocator);

  // Per-tag bytes should be restored after the scope.
  assert(global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  assert(global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);
  assert(local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  assert(local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
         local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);

  // Totals and temp bytes should also return to baseline.
  assert(global_after.total_allocated == global_before.total_allocated);
  assert(local_after.total_allocated == local_before.total_allocated);
  assert(global_after.total_temp_bytes == global_before.total_temp_bytes);
  assert(local_after.total_temp_bytes == local_before.total_temp_bytes);
  assert(vkr_allocator_scope_depth(&allocator) == 0);

  arena_destroy(arena);
  printf("  test_arena_scope_stats_reset PASSED\n");
}

static void test_allocator_aligned_alloc(void) {
  printf("  Running test_allocator_aligned_alloc...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrAllocatorStatistics global_before = snapshot_global();
  VkrAllocatorStatistics local_before =
      vkr_allocator_get_statistics(&allocator);

  const uint64_t alignment = 256;
  const uint64_t size = KB(1);

  void *ptr = vkr_allocator_alloc_aligned(&allocator, size, alignment,
                                          VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  assert(ptr != NULL);
  assert(((uintptr_t)ptr % alignment) == 0 &&
         "Allocator did not honor alignment request");

  vkr_allocator_free_aligned(&allocator, ptr, size, alignment,
                             VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  VkrAllocatorStatistics global_after = snapshot_global();
  VkrAllocatorStatistics local_after = vkr_allocator_get_statistics(&allocator);

  assert(global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER]);
  assert(local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER] ==
         local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER]);
  assert(global_after.total_allocated == global_before.total_allocated);
  assert(local_after.total_allocated == local_before.total_allocated);

  arena_destroy(arena);
  printf("  test_allocator_aligned_alloc PASSED\n");
}

static void test_allocator_aligned_realloc(void) {
  printf("  Running test_allocator_aligned_realloc...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  const uint64_t alignment = 128;
  const uint64_t size_small = KB(1);
  const uint64_t size_big = KB(2);

  VkrAllocatorStatistics global_before = snapshot_global();
  VkrAllocatorStatistics local_before =
      vkr_allocator_get_statistics(&allocator);

  void *ptr = vkr_allocator_alloc_aligned(&allocator, size_small, alignment,
                                          VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  assert(ptr != NULL && ((uintptr_t)ptr % alignment) == 0);
  MemSet(ptr, 0xAB, size_small);

  void *re =
      vkr_allocator_realloc_aligned(&allocator, ptr, size_small, size_big,
                                    alignment, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  assert(re != NULL && ((uintptr_t)re % alignment) == 0);
  // Ensure old contents survived.
  for (uint64_t i = 0; i < size_small; ++i) {
    assert(*((uint8_t *)re + i) == (uint8_t)0xAB);
  }

  vkr_allocator_free_aligned(&allocator, re, size_big, alignment,
                             VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  VkrAllocatorStatistics global_after = snapshot_global();
  VkrAllocatorStatistics local_after = vkr_allocator_get_statistics(&allocator);

  assert(global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER]);
  assert(local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER] ==
         local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_BUFFER]);
  assert(global_after.total_allocated == global_before.total_allocated);
  assert(local_after.total_allocated == local_before.total_allocated);

  arena_destroy(arena);
  printf("  test_allocator_aligned_realloc PASSED\n");
}

static void test_allocator_threadsafe_wrappers(void) {
  printf("  Running test_allocator_threadsafe_wrappers...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrMutex mutex = NULL;
  assert(vkr_mutex_create(&allocator, &mutex));

  void *ptr = vkr_allocator_alloc_ts(&allocator, KB(1),
                                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER, mutex);
  assert(ptr != NULL);

  void *aligned_ptr = vkr_allocator_alloc_aligned_ts(
      &allocator, KB(1), 32, VKR_ALLOCATOR_MEMORY_TAG_BUFFER, mutex);
  assert(aligned_ptr != NULL && ((uintptr_t)aligned_ptr % 32) == 0);

  vkr_allocator_free_ts(&allocator, ptr, KB(1), VKR_ALLOCATOR_MEMORY_TAG_BUFFER,
                        mutex);
  vkr_allocator_free_aligned_ts(&allocator, aligned_ptr, KB(1), 32,
                                VKR_ALLOCATOR_MEMORY_TAG_BUFFER, mutex);

  assert(vkr_mutex_destroy(&allocator, &mutex));
  arena_destroy(arena);
  printf("  test_allocator_threadsafe_wrappers PASSED\n");
}

static void test_dmemory_stats_roundtrip(void) {
  printf("  Running test_dmemory_stats_roundtrip...\n");

  VkrDMemory dmemory;
  assert(vkr_dmemory_create(MB(1), MB(2), &dmemory));

  VkrAllocator allocator = {.ctx = &dmemory};
  vkr_dmemory_allocator_create(&allocator);

  VkrAllocatorStatistics global_before = snapshot_global();
  VkrAllocatorStatistics local_before =
      vkr_allocator_get_statistics(&allocator);

  const uint64_t array_sz = KB(8);
  const uint64_t string_sz = KB(2);
  void *arr =
      vkr_allocator_alloc(&allocator, array_sz, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void *str = vkr_allocator_alloc(&allocator, string_sz,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  assert(arr != NULL && str != NULL);

  // Free and pass correct sizes so stats can decrement.
  vkr_allocator_free(&allocator, arr, array_sz, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(&allocator, str, string_sz,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);

  VkrAllocatorStatistics global_after = snapshot_global();
  VkrAllocatorStatistics local_after = vkr_allocator_get_statistics(&allocator);

  assert(global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  assert(global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);
  assert(local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  assert(local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
         local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);
  assert(global_after.total_allocated == global_before.total_allocated);
  assert(local_after.total_allocated == local_before.total_allocated);

  vkr_dmemory_allocator_destroy(&allocator);
  printf("  test_dmemory_stats_roundtrip PASSED\n");
}

static void test_arena_nested_scopes(void) {
  printf("  Running test_arena_nested_scopes...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrAllocatorStatistics global_before = snapshot_global();

  VkrAllocatorScope outer = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&outer));

  void *outer_buf =
      vkr_allocator_alloc(&allocator, KB(2), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert(outer_buf != NULL);

  VkrAllocatorStatistics mid = snapshot_global();
  assert(mid.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] + KB(2));

  VkrAllocatorScope inner = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&inner));

  void *inner_arr =
      vkr_allocator_alloc(&allocator, KB(1), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void *inner_str =
      vkr_allocator_alloc(&allocator, KB(1), VKR_ALLOCATOR_MEMORY_TAG_STRING);
  assert(inner_arr && inner_str);

  vkr_allocator_end_scope(&inner, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  VkrAllocatorStatistics after_inner = snapshot_global();
  // Inner allocations should be rolled back, outer buffer remains.
  assert(after_inner.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         mid.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  assert(after_inner.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);

  vkr_allocator_end_scope(&outer, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  VkrAllocatorStatistics after_outer = snapshot_global();
  assert(after_outer.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  assert(after_outer.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);
  assert(vkr_allocator_scope_depth(&allocator) == 0);

  arena_destroy(arena);
  printf("  test_arena_nested_scopes PASSED\n");
}

static void test_arena_scope_realloc(void) {
  printf("  Running test_arena_scope_realloc...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrAllocatorStatistics global_before = snapshot_global();

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&scope));

  uint64_t small_sz = KB(1);
  uint64_t big_sz = KB(4);
  void *ptr =
      vkr_allocator_alloc(&allocator, small_sz, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert(ptr != NULL);

  ptr = vkr_allocator_realloc(&allocator, ptr, small_sz, big_sz,
                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert(ptr != NULL);

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  VkrAllocatorStatistics global_after = snapshot_global();
  assert(global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
         global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  assert(global_after.total_allocated == global_before.total_allocated);
  assert(global_after.total_temp_bytes == global_before.total_temp_bytes);

  arena_destroy(arena);
  printf("  test_arena_scope_realloc PASSED\n");
}

static void test_allocator_report_manual(void) {
  printf("  Running test_allocator_report_manual...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrAllocatorStatistics global_before = snapshot_global();
  VkrAllocatorStatistics local_before =
      vkr_allocator_get_statistics(&allocator);

  const uint64_t delta = (uint64_t)KB(8);
  const VkrAllocatorMemoryTag tag = VKR_ALLOCATOR_MEMORY_TAG_RENDERER;

  vkr_allocator_report(&allocator, delta, tag, true_v);

  VkrAllocatorStatistics global_after_alloc = snapshot_global();
  VkrAllocatorStatistics local_after_alloc =
      vkr_allocator_get_statistics(&allocator);

  assert(global_after_alloc.total_allocated ==
         global_before.total_allocated + (uint64_t)delta);
  assert(local_after_alloc.total_allocated ==
         local_before.total_allocated + (uint64_t)delta);
  assert(global_after_alloc.tagged_allocs[tag] ==
         global_before.tagged_allocs[tag] + (uint64_t)delta);
  assert(local_after_alloc.tagged_allocs[tag] ==
         local_before.tagged_allocs[tag] + (uint64_t)delta);

  vkr_allocator_report(&allocator, delta, tag, false_v);

  VkrAllocatorStatistics global_after_free = snapshot_global();
  VkrAllocatorStatistics local_after_free =
      vkr_allocator_get_statistics(&allocator);

  assert(global_after_free.total_allocated == global_before.total_allocated);
  assert(local_after_free.total_allocated == local_before.total_allocated);
  assert(global_after_free.tagged_allocs[tag] ==
         global_before.tagged_allocs[tag]);
  assert(local_after_free.tagged_allocs[tag] ==
         local_before.tagged_allocs[tag]);

  arena_destroy(arena);
  printf("  test_allocator_report_manual PASSED\n");
}

bool32_t run_allocator_tests(void) {
  printf("--- Starting Allocator Interface Tests ---\n");

  test_arena_scope_stats_reset();
  test_allocator_aligned_alloc();
  test_allocator_aligned_realloc();
  test_allocator_threadsafe_wrappers();
  test_dmemory_stats_roundtrip();
  test_arena_nested_scopes();
  test_arena_scope_realloc();
  test_allocator_report_manual();

  printf("--- Allocator Interface Tests Completed ---\n");
  return true;
}

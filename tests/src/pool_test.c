#include "pool_test.h"
#include "test_stats.h"
#include <stdint.h>

static void test_pool_create(void) {
  printf("  Running test_pool_create...\n");

  VkrPool pool;
  uint64_t chunk_size = 32;
  uint32_t chunk_count = 8;

  assert(vkr_pool_create(chunk_size, chunk_count, &pool));
  assert(pool.memory != NULL);
  assert(pool.freelist_memory != NULL);
  assert(pool.chunk_count == chunk_count);
  assert(pool.chunk_size >= chunk_size);
  assert(vkr_pool_free_chunks(&pool) == chunk_count);

  vkr_pool_destroy(&pool);
  printf("  test_pool_create PASSED\n");
}

static void test_pool_alloc_and_reuse(void) {
  printf("  Running test_pool_alloc_and_reuse...\n");

  VkrPool pool;
  assert(vkr_pool_create(64, 4, &pool));

  void *a = vkr_pool_alloc(&pool);
  void *b = vkr_pool_alloc(&pool);
  void *c = vkr_pool_alloc(&pool);

  assert(a && b && c);
  assert(a != b && b != c && a != c);
  assert(vkr_pool_free_chunks(&pool) == 1);

  MemSet(a, 0xAB, pool.chunk_size);
  assert(vkr_pool_free(&pool, b));
  assert(vkr_pool_free_chunks(&pool) == 2);

  void *d = vkr_pool_alloc(&pool);
  assert(d == b); // Freed chunk should be reused first.
  for (uint64_t i = 0; i < pool.chunk_size; i++) {
    assert(((uint8_t *)a)[i] == 0xAB && "Data in another chunk corrupted");
  }

  vkr_pool_destroy(&pool);
  printf("  test_pool_alloc_and_reuse PASSED\n");
}

static void test_pool_out_of_memory(void) {
  printf("  Running test_pool_out_of_memory...\n");

  VkrPool pool;
  assert(vkr_pool_create(128, 2, &pool));

  void *p1 = vkr_pool_alloc(&pool);
  void *p2 = vkr_pool_alloc(&pool);
  assert(p1 && p2);
  assert(vkr_pool_alloc(&pool) == NULL);
  assert(vkr_pool_free_chunks(&pool) == 0);

  assert(vkr_pool_free(&pool, p1));
  assert(vkr_pool_free_chunks(&pool) == 1);
  void *p3 = vkr_pool_alloc(&pool);
  assert(p3 != NULL);

  vkr_pool_destroy(&pool);
  printf("  test_pool_out_of_memory PASSED\n");
}

static void test_pool_alignment(void) {
  printf("  Running test_pool_alignment...\n");

  VkrPool pool;
  assert(vkr_pool_create(256, 3, &pool));

  uint64_t alignment = 64;
  void *p = vkr_pool_alloc_aligned(&pool, alignment);
  assert(p != NULL);
  assert(((uintptr_t)p % alignment) == 0);

  void *invalid = vkr_pool_alloc_aligned(&pool, pool.chunk_size * 2);
  assert(invalid == NULL);

  assert(vkr_pool_free(&pool, p));
  vkr_pool_destroy(&pool);
  printf("  test_pool_alignment PASSED\n");
}

static void test_pool_allocator_adapter(void) {
  printf("  Running test_pool_allocator_adapter...\n");

  VkrPool pool;
  assert(vkr_pool_create(128, 4, &pool));

  VkrAllocator allocator = {.ctx = &pool};
  vkr_pool_allocator_create(&allocator);

  VkrAllocatorStatistics global_before = vkr_allocator_get_global_statistics();
  VkrAllocatorStatistics local_before =
      vkr_allocator_get_statistics(&allocator);

  void *arr =
      vkr_allocator_alloc(&allocator, 64, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void *str = vkr_allocator_alloc_aligned(&allocator, 32, 32,
                                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  assert(arr && str);
  assert(((uintptr_t)str % 32) == 0);
  assert(vkr_pool_free_chunks(&pool) == 2);

  vkr_allocator_free(&allocator, arr, 64, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free_aligned(&allocator, str, 32, 32,
                             VKR_ALLOCATOR_MEMORY_TAG_STRING);

  VkrAllocatorStatistics global_after = vkr_allocator_get_global_statistics();
  VkrAllocatorStatistics local_after = vkr_allocator_get_statistics(&allocator);

  VKR_TEST_ASSERT_STATS(
      global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
      global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  VKR_TEST_ASSERT_STATS(
      global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
      global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);
  VKR_TEST_ASSERT_STATS(
      local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
      local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);
  VKR_TEST_ASSERT_STATS(
      local_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
      local_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);
  VKR_TEST_ASSERT_STATS(global_after.total_allocated ==
                        global_before.total_allocated);
  VKR_TEST_ASSERT_STATS(local_after.total_allocated ==
                        local_before.total_allocated);

  vkr_pool_allocator_destroy(&allocator);
  printf("  test_pool_allocator_adapter PASSED\n");
}

static void test_arena_pool_result_storage(void) {
  printf("  Running test_arena_pool_result_storage...\n");

  Arena *backing = arena_create(MB(1), MB(1));
  assert(backing);
  VkrAllocator backing_allocator = {.ctx = backing};
  assert(vkr_allocator_arena(&backing_allocator));
  VkrArenaPool pool = {0};
  assert(vkr_arena_pool_create(KB(64), 2, &backing_allocator, &pool));
  const uint64_t free_before = vkr_pool_free_chunks(&pool.pool);
  const VkrAllocatorStatistics global_before =
      vkr_allocator_get_global_statistics();

  void *chunk = NULL;
  Arena *arena = NULL;
  VkrAllocator allocator = {0};
  assert(vkr_arena_pool_acquire_arena(&pool, &chunk, &arena, &allocator));
  assert(chunk && arena);
  assert(vkr_pool_free_chunks(&pool.pool) == free_before - 1u);

  // Loader results keep recording through a copy of the allocator.
  VkrAllocator result_allocator = allocator;
  assert(vkr_allocator_alloc(&result_allocator, 256,
                             VKR_ALLOCATOR_MEMORY_TAG_STRING));
  assert(vkr_allocator_alloc(&result_allocator, 128,
                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY));
  vkr_arena_pool_release_arena(&pool, chunk, arena, &result_allocator);

  const VkrAllocatorStatistics global_after =
      vkr_allocator_get_global_statistics();
  assert(vkr_pool_free_chunks(&pool.pool) == free_before);
  VKR_TEST_ASSERT_STATS(
      global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING] ==
      global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_STRING]);
  VKR_TEST_ASSERT_STATS(
      global_after.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY] ==
      global_before.tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_ARRAY]);

  VkrArenaPool uninitialized = {0};
  assert(!vkr_arena_pool_acquire_arena(&uninitialized, &chunk, &arena,
                                       &allocator));
  assert(!chunk && !arena);

  vkr_arena_pool_destroy(&backing_allocator, &pool);
  vkr_allocator_release_global_accounting(&backing_allocator);
  arena_destroy(backing);
  printf("  test_arena_pool_result_storage PASSED\n");
}

bool32_t run_pool_tests(void) {
  printf("--- Starting VkrPool Tests ---\n");

  test_pool_create();
  test_pool_alloc_and_reuse();
  test_pool_out_of_memory();
  test_pool_alignment();
  test_pool_allocator_adapter();
  test_arena_pool_result_storage();

  printf("--- VkrPool Tests Completed ---\n");
  return true_v;
}

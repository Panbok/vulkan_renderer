#include "vkr_pool.h"
#include "core/logger.h"
#include "platform/vkr_platform.h"

vkr_internal INLINE bool8_t
vkr_align_pow2_safe(uint64_t value, uint64_t alignment, uint64_t *out_value) {
  assert_log(out_value != NULL, "out_value must not be NULL");
  assert_log(alignment > 0, "alignment must be greater than 0");
  assert_log((alignment & (alignment - 1)) == 0,
             "alignment must be a power of two");

  if (value > UINT64_MAX - (alignment - 1)) {
    log_error("Value too large to align safely");
    return false_v;
  }

  *out_value = AlignPow2(value, alignment);
  return true_v;
}

vkr_internal INLINE bool8_t vkr_pool_ptr_offset(VkrPool *pool, void *ptr,
                                                uint64_t *out_offset) {
  assert_log(pool != NULL, "pool must not be NULL");
  assert_log(ptr != NULL, "ptr must not be NULL");
  assert_log(pool->chunk_size > 0, "pool not initialized");

  uintptr_t base = (uintptr_t)pool->memory;
  uintptr_t end = base + (uintptr_t)pool->pool_size;
  uintptr_t target = (uintptr_t)ptr;

  if (target < base || target >= end) {
    return false_v;
  }

  uint64_t offset = (uint64_t)(target - base);
  if ((offset % pool->chunk_size) != 0) {
    return false_v;
  }

  if (out_offset) {
    *out_offset = offset;
  }
  return true_v;
}

bool8_t vkr_pool_create(uint64_t chunk_size, uint32_t chunk_count,
                        VkrPool *out_pool) {
  assert_log(out_pool != NULL, "out_pool must not be NULL");
  assert_log(chunk_size > 0, "chunk_size must be greater than 0");
  assert_log(chunk_count > 0, "chunk_count must be greater than 0");

  MemZero(out_pool, sizeof(VkrPool));

  uint64_t page_size = vkr_platform_get_page_size();
  if (page_size == 0) {
    log_error("Failed to query platform page size");
    return false_v;
  }

  uint64_t aligned_chunk_size = 0;
  if (!vkr_align_pow2_safe(chunk_size, MaxAlign(), &aligned_chunk_size)) {
    return false_v;
  }

  if (aligned_chunk_size != 0 &&
      chunk_count > UINT64_MAX / aligned_chunk_size) {
    log_error("Pool size overflow (chunk_size=%llu, chunk_count=%u)",
              (uint64_t)aligned_chunk_size, (uint32_t)chunk_count);
    return false_v;
  }
  uint64_t pool_size = aligned_chunk_size * (uint64_t)chunk_count;

  uint64_t reserve_size = 0;
  if (!vkr_align_pow2_safe(pool_size, page_size, &reserve_size)) {
    return false_v;
  }

  void *memory = vkr_platform_mem_reserve(reserve_size);
  if (memory == NULL) {
    log_error("Failed to reserve %llu bytes for pool memory",
              (uint64_t)reserve_size);
    return false_v;
  }

  if (!vkr_platform_mem_commit(memory, reserve_size)) {
    log_error("Failed to commit %llu bytes for pool memory",
              (uint64_t)reserve_size);
    vkr_platform_mem_release(memory, reserve_size);
    return false_v;
  }

  uint64_t freelist_size =
      vkr_freelist_calculate_memory_requirement(pool_size);
  uint64_t freelist_reserve_size = 0;
  if (!vkr_align_pow2_safe(freelist_size, page_size, &freelist_reserve_size)) {
    vkr_platform_mem_release(memory, reserve_size);
    return false_v;
  }

  void *freelist_memory = vkr_platform_mem_reserve(freelist_reserve_size);
  if (freelist_memory == NULL) {
    log_error("Failed to reserve %llu bytes for pool freelist",
              (uint64_t)freelist_reserve_size);
    vkr_platform_mem_release(memory, reserve_size);
    return false_v;
  }

  if (!vkr_platform_mem_commit(freelist_memory, freelist_reserve_size)) {
    log_error("Failed to commit %llu bytes for pool freelist",
              (uint64_t)freelist_reserve_size);
    vkr_platform_mem_release(freelist_memory, freelist_reserve_size);
    vkr_platform_mem_release(memory, reserve_size);
    return false_v;
  }

  if (!vkr_freelist_create(freelist_memory, freelist_reserve_size, pool_size,
                           &out_pool->freelist)) {
    log_error("Failed to initialize freelist for pool");
    vkr_platform_mem_release(freelist_memory, freelist_reserve_size);
    vkr_platform_mem_release(memory, reserve_size);
    return false_v;
  }

  out_pool->memory = memory;
  out_pool->freelist_memory = freelist_memory;
  out_pool->memory_size = reserve_size;
  out_pool->freelist_memory_size = freelist_reserve_size;
  out_pool->pool_size = pool_size;
  out_pool->chunk_size = aligned_chunk_size;
  out_pool->chunk_count = chunk_count;
  out_pool->page_size = page_size;
  out_pool->allocated = 0;

  return true_v;
}

void vkr_pool_destroy(VkrPool *pool) {
  assert_log(pool != NULL, "pool must not be NULL");

  if (pool->freelist_memory != NULL) {
    vkr_freelist_destroy(&pool->freelist);
    vkr_platform_mem_release(pool->freelist_memory, pool->freelist_memory_size);
  }

  if (pool->memory != NULL) {
    vkr_platform_mem_decommit(pool->memory, pool->memory_size);
    vkr_platform_mem_release(pool->memory, pool->memory_size);
  }

  MemZero(pool, sizeof(VkrPool));
}

void *vkr_pool_alloc(VkrPool *pool) {
  assert_log(pool != NULL, "pool must not be NULL");
  assert_log(pool->chunk_size > 0, "pool not initialized");

  uint64_t offset = 0;
  if (!vkr_freelist_allocate(&pool->freelist, pool->chunk_size, &offset)) {
    log_error("Pool out of memory (chunk_size=%llu, allocated=%u/%u)",
              (uint64_t)pool->chunk_size, (uint32_t)pool->allocated,
              (uint32_t)pool->chunk_count);
    return NULL;
  }

  if (pool->allocated < pool->chunk_count) {
    pool->allocated++;
  }

  return (uint8_t *)pool->memory + offset;
}

void *vkr_pool_alloc_aligned(VkrPool *pool, uint64_t alignment) {
  assert_log(pool != NULL, "pool must not be NULL");
  assert_log(alignment > 0, "alignment must be greater than 0");
  assert_log((alignment & (alignment - 1)) == 0,
             "alignment must be a power of two");
  assert_log(pool->chunk_size > 0, "pool not initialized");

  if (alignment > pool->chunk_size) {
    log_error("Requested alignment (%llu) exceeds chunk size (%llu)",
              (uint64_t)alignment, (uint64_t)pool->chunk_size);
    return NULL;
  }

  if ((pool->chunk_size % alignment) != 0) {
    log_error("Chunk size (%llu) is not compatible with alignment (%llu)",
              (uint64_t)pool->chunk_size, (uint64_t)alignment);
    return NULL;
  }

  if (((uintptr_t)pool->memory % alignment) != 0) {
    log_error("Pool base memory is not aligned to requested alignment");
    return NULL;
  }

  return vkr_pool_alloc(pool);
}

bool8_t vkr_pool_free(VkrPool *pool, void *ptr) {
  assert_log(pool != NULL, "pool must not be NULL");
  assert_log(ptr != NULL, "ptr must not be NULL");
  assert_log(pool->chunk_size > 0, "pool not initialized");

  uint64_t offset = 0;
  if (!vkr_pool_ptr_offset(pool, ptr, &offset)) {
    log_error("Pointer %p does not belong to this pool", ptr);
    return false_v;
  }

  if (!vkr_freelist_free(&pool->freelist, pool->chunk_size, offset)) {
    log_error("Failed to free pool chunk at offset %llu", (uint64_t)offset);
    return false_v;
  }

  if (pool->allocated > 0) {
    pool->allocated--;
  }

  return true_v;
}

uint64_t vkr_pool_free_chunks(VkrPool *pool) {
  assert_log(pool != NULL, "pool must not be NULL");
  if (pool->chunk_size == 0) {
    return 0;
  }

  uint64_t free_bytes = vkr_freelist_free_space(&pool->freelist);
  return free_bytes / pool->chunk_size;
}

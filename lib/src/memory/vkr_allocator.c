#include "vkr_allocator.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "defines.h"

const char *VkrAllocatorMemoryTagNames[VKR_ALLOCATOR_MEMORY_TAG_MAX] = {
    "UNKNOWN",    "ARRAY",    "STRING",   "VECTOR", "QUEUE",
    "STRUCT",     "BUFFER",   "RENDERER", "FILE",   "TEXTURE",
    "HASH_TABLE", "FREELIST", "VULKAN",   "GPU"};

const char *VkrAllocatorTypeNames[VKR_ALLOCATOR_TYPE_MAX] = {
    "ARENA",
    "POOL",
    "DMEMORY",
    "UNKNOWN",
};

typedef struct VkrAllocatorStatisticsAtomic {
  VkrAtomicUint64 total_allocs;
  VkrAtomicUint64 total_frees;
  VkrAtomicUint64 total_reallocs;
  VkrAtomicUint64 total_zeros;
  VkrAtomicUint64 total_copies;
  VkrAtomicUint64 total_sets;

  VkrAtomicUint64 total_allocated;
  VkrAtomicUint64 tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_MAX];

  // Scope/temporary allocation tracking
  VkrAtomicUint64 total_scopes_created;
  VkrAtomicUint64 total_scopes_destroyed;
  VkrAtomicUint64 total_temp_bytes;
  VkrAtomicUint64 peak_temp_bytes;
} VkrAllocatorStatisticsAtomic;

vkr_global VkrAllocatorStatisticsAtomic g_vkr_allocator_stats = {0};

vkr_internal INLINE bool8_t vkr_allocator_lock(VkrMutex mutex) {
  if (mutex == NULL) {
    return true_v;
  }

  if (!vkr_mutex_lock(mutex)) {
    log_error("Failed to lock allocator mutex");
    return false_v;
  }

  return true_v;
}

vkr_internal INLINE void vkr_allocator_unlock(VkrMutex mutex) {
  if (mutex == NULL) {
    return;
  }

  if (!vkr_mutex_unlock(mutex)) {
    log_fatal("Failed to unlock allocator mutex");
  }
}

vkr_internal INLINE void vkr_atomic_uint64_sub_saturate(VkrAtomicUint64 *obj,
                                                        uint64_t dec) {
  uint64_t current = vkr_atomic_uint64_load(obj, VKR_MEMORY_ORDER_RELAXED);
  while (true) {
    uint64_t next = (dec >= current) ? 0 : current - dec;
    if (vkr_atomic_uint64_compare_exchange(obj, &current, next,
                                           VKR_MEMORY_ORDER_ACQ_REL,
                                           VKR_MEMORY_ORDER_RELAXED)) {
      return;
    }
    // current updated to latest value by compare_exchange on failure; loop.
  }
}

vkr_internal INLINE VkrAllocatorStatistics
vkr_allocator_stats_snapshot(const VkrAllocatorStatisticsAtomic *src) {
  VkrAllocatorStatistics stats = {0};
  stats.total_allocs =
      vkr_atomic_uint64_load(&src->total_allocs, VKR_MEMORY_ORDER_ACQUIRE);
  stats.total_frees =
      vkr_atomic_uint64_load(&src->total_frees, VKR_MEMORY_ORDER_ACQUIRE);
  stats.total_reallocs =
      vkr_atomic_uint64_load(&src->total_reallocs, VKR_MEMORY_ORDER_ACQUIRE);
  stats.total_zeros =
      vkr_atomic_uint64_load(&src->total_zeros, VKR_MEMORY_ORDER_ACQUIRE);
  stats.total_copies =
      vkr_atomic_uint64_load(&src->total_copies, VKR_MEMORY_ORDER_ACQUIRE);
  stats.total_sets =
      vkr_atomic_uint64_load(&src->total_sets, VKR_MEMORY_ORDER_ACQUIRE);

  stats.total_allocated =
      vkr_atomic_uint64_load(&src->total_allocated, VKR_MEMORY_ORDER_ACQUIRE);

  for (uint32_t i = 0; i < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++i) {
    stats.tagged_allocs[i] = vkr_atomic_uint64_load(&src->tagged_allocs[i],
                                                    VKR_MEMORY_ORDER_ACQUIRE);
  }

  // Scope tracking
  stats.total_scopes_created = vkr_atomic_uint64_load(
      &src->total_scopes_created, VKR_MEMORY_ORDER_ACQUIRE);
  stats.total_scopes_destroyed = vkr_atomic_uint64_load(
      &src->total_scopes_destroyed, VKR_MEMORY_ORDER_ACQUIRE);
  stats.total_temp_bytes =
      vkr_atomic_uint64_load(&src->total_temp_bytes, VKR_MEMORY_ORDER_ACQUIRE);
  stats.peak_temp_bytes =
      vkr_atomic_uint64_load(&src->peak_temp_bytes, VKR_MEMORY_ORDER_ACQUIRE);

  return stats;
}

vkr_internal INLINE char *
vkr_allocator_format_statistics(VkrAllocator *allocator,
                                const VkrAllocatorStatistics *stats) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(stats != NULL, "Stats must not be NULL");

  char line_buffer[256];
  uint64_t total_len = 0;
  uint32_t num_tags = (uint32_t)VKR_ALLOCATOR_MEMORY_TAG_MAX;

  for (uint32_t i = 0; i < num_tags; ++i) {
    const char *tag_name = VkrAllocatorMemoryTagNames[i];
    uint64_t size_stat = stats->tagged_allocs[i];
    uint32_t current_line_len = 0;

    current_line_len = vkr_allocator_format_size_to_buffer(
        line_buffer, sizeof(line_buffer), tag_name, size_stat);

    if (current_line_len > 0) {
      total_len += current_line_len;
    }
  }
  total_len += 1;

  char *result_str = (char *)vkr_allocator_alloc(
      allocator, total_len, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!result_str) {
    return NULL;
  }

  char *current_write_pos = result_str;
  uint64_t remaining_capacity = total_len;

  if (total_len == 1) {
    result_str[0] = '\0';
  } else {
    for (uint32_t i = 0; i < num_tags; ++i) {
      const char *tag_name = VkrAllocatorMemoryTagNames[i];
      uint64_t size_stat = stats->tagged_allocs[i];
      uint32_t current_line_len = 0;

      current_line_len = vkr_allocator_format_size_to_buffer(
          current_write_pos, remaining_capacity, tag_name, size_stat);

      if (current_line_len > 0 &&
          (uint64_t)current_line_len < remaining_capacity) {
        current_write_pos += current_line_len;
        remaining_capacity -= current_line_len;
      } else {
        *current_write_pos = '\0';
        break;
      }
    }

    if (current_write_pos < result_str + total_len) {
      *current_write_pos = '\0';
    }
  }

  return result_str;
}

uint32_t vkr_allocator_format_size_to_buffer(char *buffer, size_t buffer_size,
                                             const char *tag_name,
                                             uint64_t size_stat) {
  if (size_stat < KB(1)) {
    return snprintf(buffer, buffer_size, "%s: %llu Bytes\n", tag_name,
                    (unsigned long long)size_stat);
  } else if (size_stat < MB(1)) {
    return snprintf(buffer, buffer_size, "%s: %.2f KB\n", tag_name,
                    (double)size_stat / KB(1));
  } else if (size_stat < GB(1)) {
    return snprintf(buffer, buffer_size, "%s: %.2f MB\n", tag_name,
                    (double)size_stat / MB(1));
  } else {
    return snprintf(buffer, buffer_size, "%s: %.2f GB\n", tag_name,
                    (double)size_stat / GB(1));
  }
}

void *_vkr_allocator_alloc(VkrAllocator *allocator, uint64_t size,
                           VkrAllocatorMemoryTag tag, uint32_t alloc_line,
                           const char *alloc_file) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  if (allocator->alloc == NULL) {
    log_fatal(
        "Allocator->alloc must be set (requested at %s:%u, ctx=%p, type=%u)",
        alloc_file ? alloc_file : "<unknown>", alloc_line, allocator->ctx,
        allocator->type);
  }
  assert_log(allocator->alloc != NULL, "Allocator->alloc must be set");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  assert_log(alloc_line > 0, "Alloc line must be greater than 0");
  assert_log(alloc_file != NULL, "Alloc file must not be NULL");

#if !VKR_ALLOCATOR_DISABLE_STATS
  // Global counters
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocs, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.tagged_allocs[tag], size,
                              VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocated, size,
                              VKR_MEMORY_ORDER_RELAXED);

  // Per-allocator counters
  allocator->stats.total_allocs++;
  allocator->stats.tagged_allocs[tag] += size;
  allocator->stats.total_allocated += size;

  // Track temp allocations when inside a scope
  if (allocator->scope_depth > 0) {
    allocator->scope_bytes_allocated += size;
    allocator->stats.total_temp_bytes += size;

    // Update peak if needed
    if (allocator->scope_bytes_allocated > allocator->stats.peak_temp_bytes) {
      allocator->stats.peak_temp_bytes = allocator->scope_bytes_allocated;
    }

    // Global temp tracking
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_temp_bytes, size,
                                VKR_MEMORY_ORDER_RELAXED);

    // Update global peak (CAS loop for thread safety)
    uint64_t new_total = vkr_atomic_uint64_load(
        &g_vkr_allocator_stats.total_temp_bytes, VKR_MEMORY_ORDER_RELAXED);
    uint64_t current_peak = vkr_atomic_uint64_load(
        &g_vkr_allocator_stats.peak_temp_bytes, VKR_MEMORY_ORDER_RELAXED);
    while (new_total > current_peak) {
      if (vkr_atomic_uint64_compare_exchange(
              &g_vkr_allocator_stats.peak_temp_bytes, &current_peak, new_total,
              VKR_MEMORY_ORDER_RELAXED, VKR_MEMORY_ORDER_RELAXED)) {
        break;
      }
    }
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info("Allocated (%llu bytes) from allocator - [%s] for tag - [%s] at "
           "line - [%u] in file - [%s]",
           (uint64_t)size, VkrAllocatorTypeNames[allocator->type],
           VkrAllocatorMemoryTagNames[tag], alloc_line, alloc_file);
#endif

  return allocator->alloc(allocator->ctx, size, tag);
}

void *_vkr_allocator_alloc_aligned(VkrAllocator *allocator, uint64_t size,
                                   uint64_t alignment,
                                   VkrAllocatorMemoryTag tag,
                                   uint32_t alloc_line,
                                   const char *alloc_file) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  if (allocator->alloc_aligned == NULL) {
    log_fatal("Allocator->alloc_aligned must be set (requested at %s:%u, "
              "ctx=%p, type=%u)",
              alloc_file ? alloc_file : "<unknown>", alloc_line, allocator->ctx,
              allocator->type);
  }
  assert_log(allocator->alloc_aligned != NULL,
             "Allocator->alloc_aligned must be set");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(alignment > 0, "Alignment must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  assert_log(alloc_line > 0, "Alloc line must be greater than 0");
  assert_log(alloc_file != NULL, "Alloc file must not be NULL");

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocs, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.tagged_allocs[tag], size,
                              VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocated, size,
                              VKR_MEMORY_ORDER_RELAXED);

  allocator->stats.total_allocs++;
  allocator->stats.tagged_allocs[tag] += size;
  allocator->stats.total_allocated += size;

  if (allocator->scope_depth > 0) {
    allocator->scope_bytes_allocated += size;
    allocator->stats.total_temp_bytes += size;

    if (allocator->scope_bytes_allocated > allocator->stats.peak_temp_bytes) {
      allocator->stats.peak_temp_bytes = allocator->scope_bytes_allocated;
    }

    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_temp_bytes, size,
                                VKR_MEMORY_ORDER_RELAXED);

    uint64_t new_total = vkr_atomic_uint64_load(
        &g_vkr_allocator_stats.total_temp_bytes, VKR_MEMORY_ORDER_RELAXED);
    uint64_t current_peak = vkr_atomic_uint64_load(
        &g_vkr_allocator_stats.peak_temp_bytes, VKR_MEMORY_ORDER_RELAXED);
    while (new_total > current_peak) {
      if (vkr_atomic_uint64_compare_exchange(
              &g_vkr_allocator_stats.peak_temp_bytes, &current_peak, new_total,
              VKR_MEMORY_ORDER_RELAXED, VKR_MEMORY_ORDER_RELAXED)) {
        break;
      }
    }
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info(
      "Allocated (%llu bytes) aligned (%llu) from allocator - [%s] for tag - "
      "[%s] at line - [%u] in file - [%s]",
      (uint64_t)size, (uint64_t)alignment,
      VkrAllocatorTypeNames[allocator->type], VkrAllocatorMemoryTagNames[tag],
      alloc_line, alloc_file);
#endif

  return allocator->alloc_aligned(allocator->ctx, size, alignment, tag);
}

void *_vkr_allocator_alloc_ts(VkrAllocator *allocator, uint64_t size,
                              VkrAllocatorMemoryTag tag, VkrMutex mutex,
                              uint32_t alloc_line, const char *alloc_file) {
  if (!vkr_allocator_lock(mutex)) {
    return NULL;
  }
  void *ptr =
      _vkr_allocator_alloc(allocator, size, tag, alloc_line, alloc_file);
  vkr_allocator_unlock(mutex);
  return ptr;
}

void *_vkr_allocator_alloc_aligned_ts(VkrAllocator *allocator, uint64_t size,
                                      uint64_t alignment,
                                      VkrAllocatorMemoryTag tag, VkrMutex mutex,
                                      uint32_t alloc_line,
                                      const char *alloc_file) {
  if (!vkr_allocator_lock(mutex)) {
    return NULL;
  }
  void *ptr = _vkr_allocator_alloc_aligned(allocator, size, alignment, tag,
                                           alloc_line, alloc_file);
  vkr_allocator_unlock(mutex);
  return ptr;
}

void vkr_allocator_free(VkrAllocator *allocator, void *ptr, uint64_t old_size,
                        VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->free != NULL, "Allocator->free must be set");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_frees, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  allocator->stats.total_frees++;

  if (old_size > 0) {
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_allocated,
                                   old_size);

    uint64_t dec = vkr_min_u64(allocator->stats.total_allocated, old_size);
    allocator->stats.total_allocated -= dec;
  }

  if (old_size > 0) {
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.tagged_allocs[tag],
                                   old_size);

    uint64_t dec = vkr_min_u64(allocator->stats.tagged_allocs[tag], old_size);
    allocator->stats.tagged_allocs[tag] -= dec;
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info("Freed (%llu bytes) from allocator - [%s] for tag - [%s]",
           (unsigned long long)old_size, VkrAllocatorTypeNames[allocator->type],
           VkrAllocatorMemoryTagNames[tag]);
#endif

  allocator->free(allocator->ctx, ptr, old_size, tag);
}

void vkr_allocator_free_aligned(VkrAllocator *allocator, void *ptr,
                                uint64_t old_size, uint64_t alignment,
                                VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->free_aligned != NULL,
             "Allocator->free_aligned must be set");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(alignment > 0, "Alignment must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_frees, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  allocator->stats.total_frees++;

  if (old_size > 0) {
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_allocated,
                                   old_size);

    uint64_t dec = vkr_min_u64(allocator->stats.total_allocated, old_size);
    allocator->stats.total_allocated -= dec;
  }

  if (old_size > 0) {
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.tagged_allocs[tag],
                                   old_size);

    uint64_t dec = vkr_min_u64(allocator->stats.tagged_allocs[tag], old_size);
    allocator->stats.tagged_allocs[tag] -= dec;
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info(
      "Freed (%llu bytes) aligned (%llu) from allocator - [%s] for tag - [%s]",
      (unsigned long long)old_size, (unsigned long long)alignment,
      VkrAllocatorTypeNames[allocator->type], VkrAllocatorMemoryTagNames[tag]);
#endif

  allocator->free_aligned(allocator->ctx, ptr, old_size, alignment, tag);
}

void vkr_allocator_free_ts(VkrAllocator *allocator, void *ptr,
                           uint64_t old_size, VkrAllocatorMemoryTag tag,
                           VkrMutex mutex) {
  if (!vkr_allocator_lock(mutex)) {
    return;
  }
  vkr_allocator_free(allocator, ptr, old_size, tag);
  vkr_allocator_unlock(mutex);
}

void vkr_allocator_free_aligned_ts(VkrAllocator *allocator, void *ptr,
                                   uint64_t old_size, uint64_t alignment,
                                   VkrAllocatorMemoryTag tag, VkrMutex mutex) {
  if (!vkr_allocator_lock(mutex)) {
    return;
  }
  vkr_allocator_free_aligned(allocator, ptr, old_size, alignment, tag);
  vkr_allocator_unlock(mutex);
}

void *vkr_allocator_realloc(VkrAllocator *allocator, void *ptr,
                            uint64_t old_size, uint64_t new_size,
                            VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->realloc != NULL, "Allocator->realloc must be set");
  assert_log(new_size > 0, "New size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_reallocs, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  allocator->stats.total_reallocs++;
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info("Reallocated (%llu bytes) from allocator - [%s] for tag - [%s]",
           (unsigned long long)old_size, VkrAllocatorTypeNames[allocator->type],
           VkrAllocatorMemoryTagNames[tag]);
#endif

  if (old_size <= 0) {
    return allocator->realloc(allocator->ctx, ptr, old_size, new_size, tag);
  }

#if !VKR_ALLOCATOR_DISABLE_STATS
  if (new_size >= old_size) {
    uint64_t delta = new_size - old_size;
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocated, delta,
                                VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.tagged_allocs[tag],
                                delta, VKR_MEMORY_ORDER_RELAXED);
    allocator->stats.total_allocated += delta;
    allocator->stats.tagged_allocs[tag] += delta;
  } else {
    uint64_t delta = old_size - new_size;
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_allocated,
                                   delta);

    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.tagged_allocs[tag],
                                   delta);

    uint64_t dec = vkr_min_u64(allocator->stats.total_allocated, delta);
    allocator->stats.total_allocated -= dec;

    dec = vkr_min_u64(allocator->stats.tagged_allocs[tag], delta);
    allocator->stats.tagged_allocs[tag] -= dec;
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

  return allocator->realloc(allocator->ctx, ptr, old_size, new_size, tag);
}

void *vkr_allocator_realloc_ts(VkrAllocator *allocator, void *ptr,
                               uint64_t old_size, uint64_t new_size,
                               VkrAllocatorMemoryTag tag, VkrMutex mutex) {
  if (!vkr_allocator_lock(mutex)) {
    return NULL;
  }
  void *result = vkr_allocator_realloc(allocator, ptr, old_size, new_size, tag);
  vkr_allocator_unlock(mutex);
  return result;
}

void *vkr_allocator_realloc_aligned(VkrAllocator *allocator, void *ptr,
                                    uint64_t old_size, uint64_t new_size,
                                    uint64_t alignment,
                                    VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(alignment > 0, "Alignment must be greater than 0");
  assert_log(new_size > 0 || ptr != NULL,
             "Either new_size must be > 0 or ptr must be non-NULL");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  assert_log(allocator->realloc_aligned != NULL,
             "Allocator->realloc_aligned must be set");

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_reallocs, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  allocator->stats.total_reallocs++;
#endif // !VKR_ALLOCATOR_DISABLE_STATS

  if (old_size <= 0) {
    return allocator->realloc_aligned(allocator->ctx, ptr, old_size, new_size,
                                      alignment, tag);
  }

#if !VKR_ALLOCATOR_DISABLE_STATS
  if (new_size >= old_size) {
    uint64_t delta = new_size - old_size;
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocated, delta,
                                VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.tagged_allocs[tag],
                                delta, VKR_MEMORY_ORDER_RELAXED);
    allocator->stats.total_allocated += delta;
    allocator->stats.tagged_allocs[tag] += delta;
  } else {
    uint64_t delta = old_size - new_size;
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_allocated,
                                   delta);

    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.tagged_allocs[tag],
                                   delta);

    uint64_t dec = vkr_min_u64(allocator->stats.total_allocated, delta);
    allocator->stats.total_allocated -= dec;

    dec = vkr_min_u64(allocator->stats.tagged_allocs[tag], delta);
    allocator->stats.tagged_allocs[tag] -= dec;
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

  return allocator->realloc_aligned(allocator->ctx, ptr, old_size, new_size,
                                    alignment, tag);
}

void *vkr_allocator_realloc_aligned_ts(VkrAllocator *allocator, void *ptr,
                                       uint64_t old_size, uint64_t new_size,
                                       uint64_t alignment,
                                       VkrAllocatorMemoryTag tag,
                                       VkrMutex mutex) {
  if (!vkr_allocator_lock(mutex)) {
    return NULL;
  }
  void *result = vkr_allocator_realloc_aligned(allocator, ptr, old_size,
                                               new_size, alignment, tag);
  vkr_allocator_unlock(mutex);
  return result;
}

void vkr_allocator_set(VkrAllocator *allocator, void *ptr, uint32_t value,
                       uint64_t size) {
  assert_log(ptr != NULL, "Pointer must not be NULL");

  MemSet(ptr, value, size);

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_sets, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  if (allocator) {
    allocator->stats.total_sets++;
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  if (allocator) {
    log_info("Set (%llu bytes) from allocator - [%s]", (uint64_t)size,
             VkrAllocatorTypeNames[allocator->type]);
  }
#endif
}

void vkr_allocator_zero(VkrAllocator *allocator, void *ptr, uint64_t size) {
  assert_log(ptr != NULL, "Pointer must not be NULL");

  MemZero(ptr, size);

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_zeros, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  if (allocator) {
    allocator->stats.total_zeros++;
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  if (allocator) {
    log_info("Zeroed (%llu bytes) from allocator - [%s]", (uint64_t)size,
             VkrAllocatorTypeNames[allocator->type]);
  }
#endif
}

void vkr_allocator_copy(VkrAllocator *allocator, void *dst, const void *src,
                        uint64_t size) {
  assert_log(dst != NULL, "Destination pointer must not be NULL");
  assert_log(src != NULL, "Source pointer must not be NULL");

  MemCopy(dst, src, size);

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_copies, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  if (allocator) {
    allocator->stats.total_copies++;
  }
#endif // !VKR_ALLOCATOR_DISABLE_STATS

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info("Copied (%llu bytes) from allocator - [%s]",
           (unsigned long long)size, VkrAllocatorTypeNames[allocator->type]);
#endif
}

VkrAllocatorStatistics
vkr_allocator_get_statistics(const VkrAllocator *allocator) {
  assert_log(allocator != NULL, "Allocator must not be NULL");

  return allocator->stats;
}

char *vkr_allocator_print_statistics(VkrAllocator *allocator) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  return vkr_allocator_format_statistics(allocator, &allocator->stats);
}

VkrAllocatorStatistics vkr_allocator_get_global_statistics(void) {
  return vkr_allocator_stats_snapshot(&g_vkr_allocator_stats);
}

char *vkr_allocator_print_global_statistics(VkrAllocator *allocator) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  VkrAllocatorStatistics snapshot =
      vkr_allocator_stats_snapshot(&g_vkr_allocator_stats);
  return vkr_allocator_format_statistics(allocator, &snapshot);
}

void vkr_allocator_release_global_accounting(VkrAllocator *allocator) {
  if (!allocator) {
    return;
  }

#if !VKR_ALLOCATOR_DISABLE_STATS
  if (allocator->stats.total_allocated == 0) {
    return;
  }

  if (allocator->stats.total_temp_bytes > 0) {
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_temp_bytes,
                                   allocator->stats.total_temp_bytes);
    allocator->stats.total_temp_bytes = 0;
  }

  for (uint32_t tag = 0; tag < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++tag) {
    uint64_t bytes = allocator->stats.tagged_allocs[tag];
    if (bytes == 0) {
      continue;
    }
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.tagged_allocs[tag],
                                   bytes);
    allocator->stats.tagged_allocs[tag] = 0;
  }

  vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_allocated,
                                 allocator->stats.total_allocated);
  allocator->stats.total_allocated = 0;
#else
  (void)allocator;
#endif
}

void vkr_allocator_report(VkrAllocator *allocator, uint64_t size,
                          VkrAllocatorMemoryTag tag, bool8_t is_allocation) {
#if VKR_ALLOCATOR_DISABLE_STATS
  (void)allocator;
  (void)size;
  (void)tag;
  (void)is_allocation;
  return;
#else
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  if (size == 0) {
    return;
  }

  if (is_allocation) {
    uint64_t inc = size;
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocs, 1,
                                VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_allocated, inc,
                                VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.tagged_allocs[tag], inc,
                                VKR_MEMORY_ORDER_RELAXED);

    if (allocator) {
      allocator->stats.total_allocs++;
      allocator->stats.total_allocated += inc;
      allocator->stats.tagged_allocs[tag] += inc;
    }
    return;
  }

  uint64_t dec = size;
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_frees, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_allocated, dec);
  vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.tagged_allocs[tag],
                                 dec);

  if (allocator) {
    uint64_t local_dec = vkr_min_u64(allocator->stats.total_allocated, dec);
    allocator->stats.total_allocated -= local_dec;

    local_dec = vkr_min_u64(allocator->stats.tagged_allocs[tag], dec);
    allocator->stats.tagged_allocs[tag] -= local_dec;

    allocator->stats.total_frees++;
  }
#endif // VKR_ALLOCATOR_DISABLE_STATS
}

// =============================================================================
// Scope-based Temporary Allocation API
// =============================================================================

bool8_t vkr_allocator_supports_scopes(const VkrAllocator *allocator) {
  if (allocator == NULL) {
    return false_v;
  }
  return allocator->supports_scopes;
}

VkrAllocatorScope vkr_allocator_begin_scope(VkrAllocator *allocator) {
  VkrAllocatorScope scope = {0};

  if (allocator == NULL || !allocator->supports_scopes ||
      allocator->begin_scope == NULL) {
    return scope;
  }

  // Call allocator-specific begin_scope which handles:
  // - Incrementing scope_depth
  // - Storing the current bytes-allocated/offset for the scope
  // - Setting up allocator-specific state (e.g., scratch position for arena)
  scope = allocator->begin_scope(allocator);

  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_scopes_created, 1,
                              VKR_MEMORY_ORDER_RELAXED);

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info("Begin scope (depth=%u) on allocator [%s]", allocator->scope_depth,
           VkrAllocatorTypeNames[allocator->type]);
#endif

  return scope;
}

void vkr_allocator_end_scope(VkrAllocatorScope *scope,
                             VkrAllocatorMemoryTag tag) {
  if (scope == NULL || scope->allocator == NULL) {
    return;
  }

  VkrAllocator *allocator = scope->allocator;

  if (!allocator->supports_scopes || allocator->end_scope == NULL) {
    return;
  }

  if (allocator->scope_depth == 0) {
    assert_log(false, "end_scope called without matching begin_scope");
    return;
  }

  uint64_t tag_released[VKR_ALLOCATOR_MEMORY_TAG_MAX] = {0};
  uint64_t total_released = 0;
  if (scope->tags_snapshot_valid) {
    for (uint32_t i = 0; i < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++i) {
      if (allocator->stats.tagged_allocs[i] >
          scope->tagged_allocs_at_start[i]) {
        uint64_t released = allocator->stats.tagged_allocs[i] -
                            scope->tagged_allocs_at_start[i];
        tag_released[i] = released;
        total_released += released;
      }
    }
  } else {
    // Fallback: best-effort using total bytes difference.
    if (allocator->stats.total_allocated > scope->total_allocated_at_start) {
      total_released =
          allocator->stats.total_allocated - scope->total_allocated_at_start;
      if (tag < VKR_ALLOCATOR_MEMORY_TAG_MAX) {
        tag_released[tag] = total_released;
      }
    }
  }

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info("End scope (depth=%u) on allocator [%s]", allocator->scope_depth,
           VkrAllocatorTypeNames[allocator->type]);
#endif

  uint64_t bytes_released = 0;
  if (allocator->stats.total_allocated > scope->total_allocated_at_start) {
    bytes_released =
        allocator->stats.total_allocated - scope->total_allocated_at_start;
  }

  // Call allocator-specific end_scope which handles:
  // - Computing bytes released as (current_bytes - scope_start_bytes)
  // - Adding released bytes to scope_bytes_allocated
  // - Decrementing scope_depth with underflow protection
  // - Restoring allocator-specific state (e.g., arena position)
  // - Updating temp statistics
  allocator->end_scope(allocator, scope, tag);

  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_scopes_destroyed, 1,
                              VKR_MEMORY_ORDER_RELAXED);

  // Adjust stats for temporary scope allocations (arena frees are no-ops).
  if (total_released > 0) {
    // Per-tag stats
    for (uint32_t i = 0; i < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++i) {
      if (tag_released[i] == 0) {
        continue;
      }
      uint64_t dec =
          vkr_min_u64(allocator->stats.tagged_allocs[i], tag_released[i]);
      allocator->stats.tagged_allocs[i] -= dec;
      vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.tagged_allocs[i],
                                     tag_released[i]);
    }

    // Totals
    uint64_t dec =
        vkr_min_u64(allocator->stats.total_allocated, total_released);
    allocator->stats.total_allocated -= dec;
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_allocated,
                                   total_released);

    dec = vkr_min_u64(allocator->stats.total_temp_bytes, total_released);
    allocator->stats.total_temp_bytes -= dec;
    vkr_atomic_uint64_sub_saturate(&g_vkr_allocator_stats.total_temp_bytes,
                                   total_released);
  }

  scope->allocator = NULL;
  scope->scope_data = NULL;
}

bool8_t vkr_allocator_scope_is_valid(const VkrAllocatorScope *scope) {
  if (scope == NULL) {
    return false_v;
  }
  return scope->allocator != NULL;
}

bool8_t vkr_allocator_in_scope(const VkrAllocator *allocator) {
  if (allocator == NULL) {
    return false_v;
  }
  return allocator->scope_depth > 0;
}

uint32_t vkr_allocator_scope_depth(const VkrAllocator *allocator) {
  if (allocator == NULL) {
    return 0;
  }
  return allocator->scope_depth;
}

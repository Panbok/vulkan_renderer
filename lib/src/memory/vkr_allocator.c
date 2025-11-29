#include "vkr_allocator.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "defines.h"

const char *VkrAllocatorMemoryTagNames[VKR_ALLOCATOR_MEMORY_TAG_MAX] = {
    "UNKNOWN", "ARRAY",    "STRING", "VECTOR",  "QUEUE",      "STRUCT",
    "BUFFER",  "RENDERER", "FILE",   "TEXTURE", "HASH_TABLE", "FREELIST",
};

const char *VkrAllocatorTypeNames[VKR_ALLOCATOR_TYPE_MAX] = {
    "ARENA",
    "MMEMORY",
    "UNKNOWN",
};

#ifndef VKR_ALLOCATOR_ENABLE_LOGGING
#define VKR_ALLOCATOR_ENABLE_LOGGING 0
#endif

typedef struct VkrAllocatorStatisticsAtomic {
  VkrAtomicUint64 total_allocs;
  VkrAtomicUint64 total_frees;
  VkrAtomicUint64 total_reallocs;
  VkrAtomicUint64 total_zeros;
  VkrAtomicUint64 total_copies;
  VkrAtomicUint64 total_sets;

  VkrAtomicUint64 total_allocated;
  VkrAtomicUint64 tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_MAX];
} VkrAllocatorStatisticsAtomic;

vkr_global VkrAllocatorStatisticsAtomic g_vkr_allocator_stats = {0};

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

  return stats;
}

vkr_internal INLINE uint32_t
vkr_allocator_format_size_to_buffer(char *buffer, size_t buffer_size,
                                    const char *tag_name, uint64_t size_stat) {
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

void *_vkr_allocator_alloc(VkrAllocator *allocator, uint64_t size,
                           VkrAllocatorMemoryTag tag, uint32_t alloc_line,
                           const char *alloc_file) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->alloc != NULL, "Allocator->alloc must be set");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  assert_log(alloc_line > 0, "Alloc line must be greater than 0");
  assert_log(alloc_file != NULL, "Alloc file must not be NULL");

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

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_debug("Allocated (%llu bytes) from allocator - [%s] for tag - [%s] at "
            "line - [%u] in file - [%s]",
            (uint64_t)size, VkrAllocatorTypeNames[allocator->type],
            VkrAllocatorMemoryTagNames[tag], alloc_line, alloc_file);
#endif

  return allocator->alloc(allocator->ctx, size, tag);
}

void vkr_allocator_free(VkrAllocator *allocator, void *ptr, uint64_t old_size,
                        VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->free != NULL, "Allocator->free must be set");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

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

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_debug("Freed (%llu bytes) from allocator - [%s] for tag - [%s]",
            (unsigned long long)old_size,
            VkrAllocatorTypeNames[allocator->type],
            VkrAllocatorMemoryTagNames[tag]);
#endif

  allocator->free(allocator->ctx, ptr, old_size, tag);
}

void *vkr_allocator_realloc(VkrAllocator *allocator, void *ptr,
                            uint64_t old_size, uint64_t new_size,
                            VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->realloc != NULL, "Allocator->realloc must be set");
  assert_log(new_size > 0, "New size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_reallocs, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  allocator->stats.total_reallocs++;

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_debug("Reallocated (%llu bytes) from allocator - [%s] for tag - [%s]",
            (unsigned long long)old_size,
            VkrAllocatorTypeNames[allocator->type],
            VkrAllocatorMemoryTagNames[tag]);
#endif

  if (old_size <= 0) {
    return allocator->realloc(allocator->ctx, ptr, old_size, new_size, tag);
  }

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

  return allocator->realloc(allocator->ctx, ptr, old_size, new_size, tag);
}

void vkr_allocator_set(VkrAllocator *allocator, void *ptr, uint32_t value,
                       uint64_t size) {
  assert_log(ptr != NULL, "Pointer must not be NULL");

  MemSet(ptr, value, size);

  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_sets, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  if (allocator) {
    allocator->stats.total_sets++;
  }

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_debug("Set (%llu bytes) from allocator - [%s]", (unsigned long long)size,
            VkrAllocatorTypeNames[allocator->type]);
#endif
}

void vkr_allocator_zero(VkrAllocator *allocator, void *ptr, uint64_t size) {
  assert_log(ptr != NULL, "Pointer must not be NULL");

  MemZero(ptr, size);

  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_zeros, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  if (allocator) {
    allocator->stats.total_zeros++;
  }

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_debug("Zeroed (%llu bytes) from allocator - [%s]",
            (unsigned long long)size, VkrAllocatorTypeNames[allocator->type]);
#endif
}

void vkr_allocator_copy(VkrAllocator *allocator, void *dst, const void *src,
                        uint64_t size) {
  assert_log(dst != NULL, "Destination pointer must not be NULL");
  assert_log(src != NULL, "Source pointer must not be NULL");

  MemCopy(dst, src, size);

  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_copies, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  if (allocator) {
    allocator->stats.total_copies++;
  }

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_debug("Copied (%llu bytes) from allocator - [%s]",
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

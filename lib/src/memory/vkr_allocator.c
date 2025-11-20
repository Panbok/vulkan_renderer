#include "vkr_allocator.h"
#include "core/logger.h"
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

vkr_global VkrAllocatorStatistics g_vkr_allocator_stats = {0};

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
  g_vkr_allocator_stats.total_allocs++;
  g_vkr_allocator_stats.tagged_allocs[tag] += size;
  g_vkr_allocator_stats.total_allocated += size;

  // Per-allocator counters
  allocator->stats.total_allocs++;
  allocator->stats.tagged_allocs[tag] += size;
  allocator->stats.total_allocated += size;

  log_debug("Allocated (%llu bytes) from allocator - [%s] for tag - [%s] at "
            "line - [%u] in file - [%s]",
            (uint64_t)size, VkrAllocatorTypeNames[allocator->type],
            VkrAllocatorMemoryTagNames[tag], alloc_line, alloc_file);

  return allocator->alloc(allocator->ctx, size, tag);
}

void vkr_allocator_free(VkrAllocator *allocator, void *ptr, uint64_t old_size,
                        VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->free != NULL, "Allocator->free must be set");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  g_vkr_allocator_stats.total_frees++;
  allocator->stats.total_frees++;

  if (old_size > 0) {
    uint64_t dec = vkr_min_u64(g_vkr_allocator_stats.total_allocated, old_size);
    g_vkr_allocator_stats.total_allocated -= dec;

    dec = vkr_min_u64(allocator->stats.total_allocated, old_size);
    allocator->stats.total_allocated -= dec;
  }

  if (g_vkr_allocator_stats.tagged_allocs[tag] > 0) {
    g_vkr_allocator_stats.tagged_allocs[tag]--;
  }

  if (allocator->stats.tagged_allocs[tag] > 0) {
    allocator->stats.tagged_allocs[tag]--;
  }

  if (old_size > 0) {
    uint64_t dec =
        vkr_min_u64(g_vkr_allocator_stats.tagged_allocs[tag], old_size);
    g_vkr_allocator_stats.tagged_allocs[tag] -= dec;

    dec = vkr_min_u64(allocator->stats.tagged_allocs[tag], old_size);
    allocator->stats.tagged_allocs[tag] -= dec;
  }

  log_debug("Freed (%llu bytes) from allocator - [%s] for tag - [%s]",
            (unsigned long long)old_size,
            VkrAllocatorTypeNames[allocator->type],
            VkrAllocatorMemoryTagNames[tag]);

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

  g_vkr_allocator_stats.total_reallocs++;
  allocator->stats.total_reallocs++;

  log_debug("Reallocated (%llu bytes) from allocator - [%s] for tag - [%s]",
            (unsigned long long)old_size,
            VkrAllocatorTypeNames[allocator->type],
            VkrAllocatorMemoryTagNames[tag]);

  if (old_size <= 0) {
    return allocator->realloc(allocator->ctx, ptr, old_size, new_size, tag);
  }

  if (new_size >= old_size) {
    uint64_t delta = new_size - old_size;
    g_vkr_allocator_stats.total_allocated += delta;
    g_vkr_allocator_stats.tagged_allocs[tag] += delta;
    allocator->stats.total_allocated += delta;
    allocator->stats.tagged_allocs[tag] += delta;
  } else {
    uint64_t delta = old_size - new_size;
    uint64_t dec = vkr_min_u64(g_vkr_allocator_stats.total_allocated, delta);
    g_vkr_allocator_stats.total_allocated -= dec;

    dec = vkr_min_u64(g_vkr_allocator_stats.tagged_allocs[tag], delta);
    g_vkr_allocator_stats.tagged_allocs[tag] -= dec;

    dec = vkr_min_u64(allocator->stats.total_allocated, delta);
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

  g_vkr_allocator_stats.total_sets++;

  if (allocator) {
    allocator->stats.total_sets++;
  }

  log_debug("Set (%llu bytes) from allocator - [%s]", (unsigned long long)size,
            VkrAllocatorTypeNames[allocator->type]);
}

void vkr_allocator_zero(VkrAllocator *allocator, void *ptr, uint64_t size) {
  assert_log(ptr != NULL, "Pointer must not be NULL");

  MemZero(ptr, size);

  g_vkr_allocator_stats.total_zeros++;

  if (allocator) {
    allocator->stats.total_zeros++;
  }

  log_debug("Zeroed (%llu bytes) from allocator - [%s]",
            (unsigned long long)size, VkrAllocatorTypeNames[allocator->type]);
}

void vkr_allocator_copy(VkrAllocator *allocator, void *dst, const void *src,
                        uint64_t size) {
  assert_log(dst != NULL, "Destination pointer must not be NULL");
  assert_log(src != NULL, "Source pointer must not be NULL");

  MemCopy(dst, src, size);

  g_vkr_allocator_stats.total_copies++;

  if (allocator) {
    allocator->stats.total_copies++;
  }

  log_debug("Copied (%llu bytes) from allocator - [%s]",
            (unsigned long long)size, VkrAllocatorTypeNames[allocator->type]);
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
  return g_vkr_allocator_stats;
}

char *vkr_allocator_print_global_statistics(VkrAllocator *allocator) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  return vkr_allocator_format_statistics(allocator, &g_vkr_allocator_stats);
}
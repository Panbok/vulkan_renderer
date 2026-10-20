#include "vkr_allocator.h"
#include "arena.h"
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

#define VKR_ALLOCATOR_REGISTRY_NAME_WORDS                                      \
  (VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY / sizeof(uint64_t))

_Static_assert(VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY % sizeof(uint64_t) == 0,
               "Registry names are stored as whole 64-bit words");

/*
 * One registry slot. `claimed` serializes registrations. `generation` is odd
 * while the slot is live and changes on every claim and release, so a sampler
 * that reads the same odd value before and after its copy read one identity.
 * Counters have a single writer, the owner of the registered allocator or
 * arena, except the interval maxima, which the sampler exchanges.
 * Slots are aligned to 128 bytes, the Apple M-series cache line, so owners on
 * different threads do not share a line.
 */
typedef struct VkrAllocatorRegistryEntry {
  _Alignas(128) VkrAtomicUint32 claimed;
  VkrAtomicUint32 generation;
  VkrAtomicUint32 type;
  VkrAtomicUint32 owner;
  VkrAtomicUint64 name_words[VKR_ALLOCATOR_REGISTRY_NAME_WORDS];

  VkrAtomicUint64 alloc_count;
  VkrAtomicUint64 free_count;
  VkrAtomicUint64 realloc_count;
  VkrAtomicUint64 live_bytes;
  VkrAtomicUint64 scope_count;
  VkrAtomicUint64 scope_alloc_bytes;
  VkrAtomicUint64 scope_depth;
  VkrAtomicUint64 scope_depth_high;

  VkrAtomicUint64 has_arena;
  VkrAtomicUint64 arena_used_bytes;
  VkrAtomicUint64 arena_committed_bytes;
  VkrAtomicUint64 arena_reserved_bytes;
  VkrAtomicUint64 arena_peak_used_bytes;
  VkrAtomicUint64 arena_interval_peak_used_bytes;

  // The registered VkrAllocator or raw Arena, compared on release so a stale
  // handle cannot release a reused slot.
  VkrAtomicUint64 owner_address;
  // Arena whose registry_slot names this slot. Touched only by the owner while
  // registering, releasing or destroying the arena.
  Arena *bound_arena;
} VkrAllocatorRegistryEntry;

typedef struct VkrAllocatorRegistry {
  VkrAllocatorRegistryEntry entries[VKR_ALLOCATOR_REGISTRY_CAPACITY];
  VkrAtomicUint32 rejected_count;
} VkrAllocatorRegistry;

vkr_global VkrAllocatorRegistry g_vkr_allocator_registry = {0};

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
    size_t current_line_len = 0;

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
      size_t current_line_len = 0;

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

size_t vkr_allocator_format_size_to_buffer(char *buffer, size_t buffer_size,
                                           const char *tag_name,
                                           uint64_t size_stat) {
  if (size_stat < KB(1)) {
    return snprintf(buffer, buffer_size, "%s: %llu bytes\n", tag_name,
                    (unsigned long long)size_stat);
  } else if (size_stat < MB(1)) {
    return snprintf(buffer, buffer_size, "%s: %.1f KB\n", tag_name,
                    (double)size_stat / KB(1));
  } else if (size_stat < GB(1)) {
    return snprintf(buffer, buffer_size, "%s: %.1f MB\n", tag_name,
                    (double)size_stat / MB(1));
  } else {
    return snprintf(buffer, buffer_size, "%s: %.1f GB\n", tag_name,
                    (double)size_stat / GB(1));
  }
}

// =============================================================================
// Named allocator registry: writer side
// =============================================================================

/* Returns the entry for a 1-based slot. Slot 0 wraps above the capacity, so an
 * unregistered owner pays this single compare. */
vkr_internal INLINE VkrAllocatorRegistryEntry *
vkr_allocator_registry_entry(uint32_t registry_slot) {
  uint32_t index = registry_slot - 1u;
  if (index >= VKR_ALLOCATOR_REGISTRY_CAPACITY) {
    return NULL;
  }

  return &g_vkr_allocator_registry.entries[index];
}

/* Single-writer counter update: a relaxed load and store, no read-modify-write
 * instruction. */
vkr_internal INLINE void
vkr_allocator_registry_counter_add(VkrAtomicUint64 *counter, uint64_t value) {
  uint64_t current = vkr_atomic_uint64_load(counter, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(counter, current + value, VKR_MEMORY_ORDER_RELAXED);
}

/* Saturates at zero: frees of bytes allocated before registration must not
 * wrap the live size. */
vkr_internal INLINE void
vkr_allocator_registry_counter_sub(VkrAtomicUint64 *counter, uint64_t value) {
  uint64_t current = vkr_atomic_uint64_load(counter, VKR_MEMORY_ORDER_RELAXED);
  uint64_t next = (value >= current) ? 0 : current - value;
  vkr_atomic_uint64_store(counter, next, VKR_MEMORY_ORDER_RELAXED);
}

/* Raises an interval maximum that the sampler exchanges back to the current
 * value. The compare-exchange keeps a raise from overwriting that reset. */
vkr_internal INLINE void vkr_allocator_registry_raise(VkrAtomicUint64 *maximum,
                                                      uint64_t value) {
  uint64_t current = vkr_atomic_uint64_load(maximum, VKR_MEMORY_ORDER_RELAXED);
  while (value > current) {
    if (vkr_atomic_uint64_compare_exchange(maximum, &current, value,
                                           VKR_MEMORY_ORDER_RELAXED,
                                           VKR_MEMORY_ORDER_RELAXED)) {
      return;
    }
  }
}

vkr_internal void
vkr_allocator_registry_note_alloc(VkrAllocatorRegistryEntry *entry,
                                  const VkrAllocator *allocator,
                                  uint64_t size) {
  vkr_allocator_registry_counter_add(&entry->alloc_count, 1);
  vkr_allocator_registry_counter_add(&entry->live_bytes, size);

  if (allocator->scope_depth > 0) {
    vkr_allocator_registry_counter_add(&entry->scope_alloc_bytes, size);
  }
}

vkr_internal void
vkr_allocator_registry_note_free(VkrAllocatorRegistryEntry *entry,
                                 uint64_t old_size) {
  vkr_allocator_registry_counter_add(&entry->free_count, 1);
  vkr_allocator_registry_counter_sub(&entry->live_bytes, old_size);
}

vkr_internal void
vkr_allocator_registry_note_realloc(VkrAllocatorRegistryEntry *entry,
                                    const VkrAllocator *allocator,
                                    uint64_t old_size, uint64_t new_size) {
  vkr_allocator_registry_counter_add(&entry->realloc_count, 1);

  if (new_size >= old_size) {
    uint64_t growth = new_size - old_size;
    vkr_allocator_registry_counter_add(&entry->live_bytes, growth);

    if (allocator->scope_depth > 0) {
      vkr_allocator_registry_counter_add(&entry->scope_alloc_bytes, growth);
    }
  } else {
    vkr_allocator_registry_counter_sub(&entry->live_bytes, old_size - new_size);
  }
}

vkr_internal void
vkr_allocator_registry_note_scope_begin(VkrAllocatorRegistryEntry *entry,
                                        const VkrAllocator *allocator,
                                        VkrAllocatorScope *scope) {
  scope->registry_live_at_start =
      vkr_atomic_uint64_load(&entry->live_bytes, VKR_MEMORY_ORDER_RELAXED);

  vkr_allocator_registry_counter_add(&entry->scope_count, 1);
  vkr_atomic_uint64_store(&entry->scope_depth, allocator->scope_depth,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_allocator_registry_raise(&entry->scope_depth_high,
                               allocator->scope_depth);
}

vkr_internal void
vkr_allocator_registry_note_scope_end(VkrAllocatorRegistryEntry *entry,
                                      const VkrAllocator *allocator,
                                      const VkrAllocatorScope *scope) {
  vkr_atomic_uint64_store(&entry->scope_depth, allocator->scope_depth,
                          VKR_MEMORY_ORDER_RELAXED);

  // Bytes allocated inside the scope are released with it.
  uint64_t live =
      vkr_atomic_uint64_load(&entry->live_bytes, VKR_MEMORY_ORDER_RELAXED);
  if (live > scope->registry_live_at_start) {
    vkr_atomic_uint64_store(&entry->live_bytes, scope->registry_live_at_start,
                            VKR_MEMORY_ORDER_RELAXED);
  }
}

static void vkr_allocator_record_allocation(VkrAllocator *allocator,
                                            uint64_t size,
                                            VkrAllocatorMemoryTag tag) {
  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL) {
    vkr_allocator_registry_note_alloc(registry_entry, allocator, size);
  }

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

  void *result = allocator->alloc(allocator->ctx, size, tag);
  if (!result) {
    return NULL;
  }
  vkr_allocator_record_allocation(allocator, size, tag);

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info("Allocated (%llu bytes) from allocator - [%s] for tag - [%s] at "
           "line - [%u] in file - [%s]",
           (uint64_t)size, VkrAllocatorTypeNames[allocator->type],
           VkrAllocatorMemoryTagNames[tag], alloc_line, alloc_file);
#endif

  return result;
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

  void *result = allocator->alloc_aligned(allocator->ctx, size, alignment, tag);
  if (!result) {
    return NULL;
  }
  vkr_allocator_record_allocation(allocator, size, tag);

#if VKR_ALLOCATOR_ENABLE_LOGGING
  log_info(
      "Allocated (%llu bytes) aligned (%llu) from allocator - [%s] for tag - "
      "[%s] at line - [%u] in file - [%s]",
      (uint64_t)size, (uint64_t)alignment,
      VkrAllocatorTypeNames[allocator->type], VkrAllocatorMemoryTagNames[tag],
      alloc_line, alloc_file);
#endif

  return result;
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

  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL) {
    vkr_allocator_registry_note_free(registry_entry, old_size);
  }

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

  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL) {
    vkr_allocator_registry_note_free(registry_entry, old_size);
  }

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

  void *result =
      allocator->realloc(allocator->ctx, ptr, old_size, new_size, tag);
  if (!result && new_size > 0) {
    return NULL;
  }

  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL) {
    vkr_allocator_registry_note_realloc(registry_entry, allocator, old_size,
                                        new_size);
  }

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

  return result;
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

  void *result = allocator->realloc_aligned(allocator->ctx, ptr, old_size,
                                            new_size, alignment, tag);
  if (!result && new_size > 0) {
    return NULL;
  }

  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL) {
    vkr_allocator_registry_note_realloc(registry_entry, allocator, old_size,
                                        new_size);
  }

#if !VKR_ALLOCATOR_DISABLE_STATS
  vkr_atomic_uint64_fetch_add(&g_vkr_allocator_stats.total_reallocs, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  allocator->stats.total_reallocs++;
#endif // !VKR_ALLOCATOR_DISABLE_STATS

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

  return result;
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

  // Check if accounting has already been released to prevent double-decrement
  if (allocator->accounting_released) {
    assert_log(false, "vkr_allocator_release_global_accounting called on "
                      "allocator with accounting already released");
    return;
  }

  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL) {
    vkr_atomic_uint64_store(&registry_entry->live_bytes, 0,
                            VKR_MEMORY_ORDER_RELAXED);
  }

#if !VKR_ALLOCATOR_DISABLE_STATS
  // Idempotent check: if nothing was allocated, mark as released and return
  if (allocator->stats.total_allocated == 0) {
    allocator->accounting_released = true;
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
#endif

  // Mark accounting as released after adjusting global stats
  allocator->accounting_released = true;
}

void vkr_allocator_report(VkrAllocator *allocator, uint64_t size,
                          VkrAllocatorMemoryTag tag, bool8_t is_allocation) {
  if (allocator != NULL && size > 0) {
    VkrAllocatorRegistryEntry *registry_entry =
        vkr_allocator_registry_entry(allocator->registry_slot);
    if (registry_entry != NULL) {
      if (is_allocation) {
        vkr_allocator_registry_note_alloc(registry_entry, allocator, size);
      } else {
        vkr_allocator_registry_note_free(registry_entry, size);
      }
    }
  }

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

  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL && vkr_allocator_scope_is_valid(&scope)) {
    vkr_allocator_registry_note_scope_begin(registry_entry, allocator, &scope);
  }

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

  // Call allocator-specific end_scope which handles:
  // - Computing bytes released as (current_bytes - scope_start_bytes)
  // - Adding released bytes to scope_bytes_allocated
  // - Decrementing scope_depth with underflow protection
  // - Restoring allocator-specific state (e.g., arena position)
  // - Updating temp statistics
  allocator->end_scope(allocator, scope, tag);

  VkrAllocatorRegistryEntry *registry_entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  if (registry_entry != NULL) {
    vkr_allocator_registry_note_scope_end(registry_entry, allocator, scope);
  }

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

// =============================================================================
// Named allocator registry: registration and sampling
// =============================================================================

vkr_internal INLINE uint32_t
vkr_allocator_registry_slot_of(const VkrAllocatorRegistryEntry *entry) {
  return (uint32_t)(entry - g_vkr_allocator_registry.entries) + 1u;
}

vkr_internal void
vkr_allocator_registry_store_name(VkrAllocatorRegistryEntry *entry,
                                  const char *name) {
  char bytes[VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY] = {0};

  uint32_t length = 0;
  while (length + 1u < VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY &&
         name[length] != '\0') {
    length++;
  }
  MemCopy(bytes, name, length);

  for (uint32_t word_index = 0; word_index < VKR_ALLOCATOR_REGISTRY_NAME_WORDS;
       ++word_index) {
    uint64_t word = 0;
    MemCopy(&word, bytes + word_index * sizeof(uint64_t), sizeof(uint64_t));
    vkr_atomic_uint64_store(&entry->name_words[word_index], word,
                            VKR_MEMORY_ORDER_RELAXED);
  }
}

vkr_internal void
vkr_allocator_registry_clear_counters(VkrAllocatorRegistryEntry *entry) {
  VkrAtomicUint64 *counters[] = {
      &entry->alloc_count,
      &entry->free_count,
      &entry->realloc_count,
      &entry->live_bytes,
      &entry->scope_count,
      &entry->scope_alloc_bytes,
      &entry->scope_depth,
      &entry->scope_depth_high,
      &entry->has_arena,
      &entry->arena_used_bytes,
      &entry->arena_committed_bytes,
      &entry->arena_reserved_bytes,
      &entry->arena_peak_used_bytes,
      &entry->arena_interval_peak_used_bytes,
  };

  for (uint32_t i = 0; i < ArrayCount(counters); ++i) {
    vkr_atomic_uint64_store(counters[i], 0, VKR_MEMORY_ORDER_RELAXED);
  }
}

/* Claims a free slot and writes its identity. The slot stays invisible to
 * samplers until vkr_allocator_registry_publish(). Returns NULL when every
 * slot is in use. */
vkr_internal VkrAllocatorRegistryEntry *
vkr_allocator_registry_claim(const char *name, VkrAllocatorType type,
                             VkrAllocatorRegistryOwner owner,
                             const void *owner_address) {
  for (uint32_t index = 0; index < VKR_ALLOCATOR_REGISTRY_CAPACITY; ++index) {
    VkrAllocatorRegistryEntry *entry = &g_vkr_allocator_registry.entries[index];

    uint32_t expected = 0;
    if (!vkr_atomic_uint32_compare_exchange(&entry->claimed, &expected, 1,
                                            VKR_MEMORY_ORDER_ACQ_REL,
                                            VKR_MEMORY_ORDER_RELAXED)) {
      continue;
    }

    /* A sampler that reads any value written below must also read the
     * generation change of the slot's previous release, which happened
     * before this claim. The release fence pairs with the sampler's acquire
     * fence. stdatomic is used directly because vkr_atomic has no fence. */
    atomic_thread_fence(VKR_MEMORY_ORDER_RELEASE);

    vkr_allocator_registry_store_name(entry, name);
    vkr_atomic_uint32_store(&entry->type, (uint32_t)type,
                            VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint32_store(&entry->owner, (uint32_t)owner,
                            VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint64_store(&entry->owner_address,
                            (uint64_t)(uintptr_t)owner_address,
                            VKR_MEMORY_ORDER_RELAXED);
    vkr_allocator_registry_clear_counters(entry);
    entry->bound_arena = NULL;

    return entry;
  }

  vkr_atomic_uint32_fetch_add(&g_vkr_allocator_registry.rejected_count, 1,
                              VKR_MEMORY_ORDER_RELAXED);
  log_warn("Allocator registry is full (%u entries); '%s' is not registered",
           (uint32_t)VKR_ALLOCATOR_REGISTRY_CAPACITY, name);
  return NULL;
}

/* Makes a claimed slot visible to samplers: an odd generation is live. */
vkr_internal uint32_t
vkr_allocator_registry_publish(VkrAllocatorRegistryEntry *entry) {
  uint32_t generation =
      vkr_atomic_uint32_load(&entry->generation, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&entry->generation, generation + 1u,
                          VKR_MEMORY_ORDER_RELEASE);

  return vkr_allocator_registry_slot_of(entry);
}

/* Hides the slot from samplers (even generation), then frees it for reuse. */
vkr_internal void
vkr_allocator_registry_release(VkrAllocatorRegistryEntry *entry) {
  entry->bound_arena = NULL;
  vkr_atomic_uint64_store(&entry->owner_address, 0, VKR_MEMORY_ORDER_RELAXED);

  uint32_t generation =
      vkr_atomic_uint32_load(&entry->generation, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&entry->generation, generation + 1u,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_atomic_uint32_store(&entry->claimed, 0, VKR_MEMORY_ORDER_RELEASE);
}

vkr_internal void
vkr_allocator_registry_bind_arena(VkrAllocatorRegistryEntry *entry,
                                  Arena *arena) {
  ArenaFootprint footprint = arena_footprint(arena);

  vkr_atomic_uint64_store(&entry->has_arena, 1, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_used_bytes, footprint.used_bytes,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_committed_bytes,
                          footprint.committed_bytes, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_reserved_bytes,
                          footprint.reserved_bytes, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_peak_used_bytes, footprint.used_bytes,
                          VKR_MEMORY_ORDER_RELAXED);

  entry->bound_arena = arena;
  arena->registry_slot = vkr_allocator_registry_slot_of(entry);
}

bool8_t vkr_allocator_register_name(VkrAllocator *allocator, const char *name) {
  if (allocator == NULL || name == NULL) {
    return false_v;
  }

  if (allocator->registry_slot != 0) {
    log_warn("Allocator is already registered; '%s' is not registered", name);
    return false_v;
  }

  VkrAllocatorRegistryEntry *entry = vkr_allocator_registry_claim(
      name, allocator->type, VKR_ALLOCATOR_REGISTRY_OWNER_ALLOCATOR, allocator);
  if (entry == NULL) {
    return false_v;
  }

  // An arena already published through another entry keeps that entry.
  if (allocator->type == VKR_ALLOCATOR_TYPE_ARENA && allocator->ctx != NULL) {
    Arena *arena = (Arena *)allocator->ctx;
    if (arena->registry_slot == 0) {
      vkr_allocator_registry_bind_arena(entry, arena);
    }
  }

  vkr_atomic_uint64_store(&entry->scope_depth, allocator->scope_depth,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->scope_depth_high, allocator->scope_depth,
                          VKR_MEMORY_ORDER_RELAXED);

  allocator->registry_slot = vkr_allocator_registry_publish(entry);
  return true_v;
}

void vkr_allocator_unregister(VkrAllocator *allocator) {
  if (allocator == NULL) {
    return;
  }

  VkrAllocatorRegistryEntry *entry =
      vkr_allocator_registry_entry(allocator->registry_slot);
  allocator->registry_slot = 0;
  if (entry == NULL) {
    return;
  }

  uint64_t owner_address =
      vkr_atomic_uint64_load(&entry->owner_address, VKR_MEMORY_ORDER_RELAXED);
  if (owner_address != (uint64_t)(uintptr_t)allocator) {
    log_warn("Allocator registry slot %u belongs to another owner; the stale "
             "handle is cleared without releasing it",
             vkr_allocator_registry_slot_of(entry));
    return;
  }

  if (entry->bound_arena != NULL) {
    entry->bound_arena->registry_slot = 0;
  }

  vkr_allocator_registry_release(entry);
}

bool8_t vkr_allocator_register_arena_name(Arena *arena, const char *name) {
  if (arena == NULL || name == NULL) {
    return false_v;
  }

  if (arena->registry_slot != 0) {
    log_warn("Arena is already registered; '%s' is not registered", name);
    return false_v;
  }

  VkrAllocatorRegistryEntry *entry =
      vkr_allocator_registry_claim(name, VKR_ALLOCATOR_TYPE_ARENA,
                                   VKR_ALLOCATOR_REGISTRY_OWNER_ARENA, arena);
  if (entry == NULL) {
    return false_v;
  }

  vkr_allocator_registry_bind_arena(entry, arena);
  vkr_atomic_uint64_store(&entry->live_bytes,
                          vkr_atomic_uint64_load(&entry->arena_used_bytes,
                                                 VKR_MEMORY_ORDER_RELAXED),
                          VKR_MEMORY_ORDER_RELAXED);

  vkr_allocator_registry_publish(entry);
  return true_v;
}

void vkr_allocator_unregister_arena(Arena *arena) {
  if (arena == NULL) {
    return;
  }

  VkrAllocatorRegistryEntry *entry =
      vkr_allocator_registry_entry(arena->registry_slot);
  if (entry == NULL) {
    arena->registry_slot = 0;
    return;
  }

  if (vkr_atomic_uint32_load(&entry->owner, VKR_MEMORY_ORDER_RELAXED) !=
      VKR_ALLOCATOR_REGISTRY_OWNER_ARENA) {
    log_warn("Arena is published by allocator registry slot %u; unregister "
             "that allocator instead",
             vkr_allocator_registry_slot_of(entry));
    return;
  }

  arena->registry_slot = 0;
  vkr_allocator_registry_release(entry);
}

void vkr_allocator_registry_note_arena(Arena *arena,
                                       uint64_t rewound_from_bytes,
                                       bool8_t pushed) {
  VkrAllocatorRegistryEntry *entry =
      vkr_allocator_registry_entry(arena->registry_slot);
  if (entry == NULL) {
    return;
  }

  ArenaFootprint footprint = arena_footprint(arena);

  // The used size peaks just before it falls, so maxima move only on rewind;
  // the sampler adds the current size.
  if (rewound_from_bytes > footprint.used_bytes) {
    uint64_t peak = vkr_atomic_uint64_load(&entry->arena_peak_used_bytes,
                                           VKR_MEMORY_ORDER_RELAXED);
    if (rewound_from_bytes > peak) {
      vkr_atomic_uint64_store(&entry->arena_peak_used_bytes, rewound_from_bytes,
                              VKR_MEMORY_ORDER_RELAXED);
    }

    vkr_allocator_registry_raise(&entry->arena_interval_peak_used_bytes,
                                 rewound_from_bytes);
  }

  vkr_atomic_uint64_store(&entry->arena_used_bytes, footprint.used_bytes,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_committed_bytes,
                          footprint.committed_bytes, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_reserved_bytes,
                          footprint.reserved_bytes, VKR_MEMORY_ORDER_RELAXED);

  if (vkr_atomic_uint32_load(&entry->owner, VKR_MEMORY_ORDER_RELAXED) ==
      VKR_ALLOCATOR_REGISTRY_OWNER_ARENA) {
    if (pushed) {
      vkr_allocator_registry_counter_add(&entry->alloc_count, 1);
    }

    vkr_atomic_uint64_store(&entry->live_bytes, footprint.used_bytes,
                            VKR_MEMORY_ORDER_RELAXED);
  }
}

void vkr_allocator_registry_detach_arena(Arena *arena) {
  VkrAllocatorRegistryEntry *entry =
      vkr_allocator_registry_entry(arena->registry_slot);
  arena->registry_slot = 0;
  if (entry == NULL || entry->bound_arena != arena) {
    return;
  }

  if (vkr_atomic_uint32_load(&entry->owner, VKR_MEMORY_ORDER_RELAXED) ==
      VKR_ALLOCATOR_REGISTRY_OWNER_ARENA) {
    vkr_allocator_registry_release(entry);
    return;
  }

  // The allocator entry stays live until vkr_allocator_unregister().
  entry->bound_arena = NULL;
  vkr_atomic_uint64_store(&entry->has_arena, 0, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_used_bytes, 0,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_committed_bytes, 0,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&entry->arena_reserved_bytes, 0,
                          VKR_MEMORY_ORDER_RELAXED);
}

uint32_t vkr_allocator_registry_sample(VkrAllocatorRegistrySample *out,
                                       uint32_t capacity) {
  if (out == NULL) {
    capacity = 0;
  }

  uint32_t found = 0;
  for (uint32_t index = 0; index < VKR_ALLOCATOR_REGISTRY_CAPACITY; ++index) {
    VkrAllocatorRegistryEntry *entry = &g_vkr_allocator_registry.entries[index];

    uint32_t generation =
        vkr_atomic_uint32_load(&entry->generation, VKR_MEMORY_ORDER_ACQUIRE);
    if ((generation & 1u) == 0) {
      continue;
    }

    if (found >= capacity) {
      found++;
      continue;
    }

    VkrAllocatorRegistrySample *sample = &out[found];

    for (uint32_t word_index = 0;
         word_index < VKR_ALLOCATOR_REGISTRY_NAME_WORDS; ++word_index) {
      uint64_t word = vkr_atomic_uint64_load(&entry->name_words[word_index],
                                             VKR_MEMORY_ORDER_RELAXED);
      MemCopy(sample->name + word_index * sizeof(uint64_t), &word,
              sizeof(uint64_t));
    }
    sample->name[VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY - 1] = '\0';

    uint32_t type =
        vkr_atomic_uint32_load(&entry->type, VKR_MEMORY_ORDER_RELAXED);
    uint32_t owner =
        vkr_atomic_uint32_load(&entry->owner, VKR_MEMORY_ORDER_RELAXED);

    uint64_t alloc_count =
        vkr_atomic_uint64_load(&entry->alloc_count, VKR_MEMORY_ORDER_RELAXED);
    uint64_t free_count =
        vkr_atomic_uint64_load(&entry->free_count, VKR_MEMORY_ORDER_RELAXED);
    uint64_t realloc_count =
        vkr_atomic_uint64_load(&entry->realloc_count, VKR_MEMORY_ORDER_RELAXED);
    uint64_t live_bytes =
        vkr_atomic_uint64_load(&entry->live_bytes, VKR_MEMORY_ORDER_RELAXED);
    uint64_t scope_count =
        vkr_atomic_uint64_load(&entry->scope_count, VKR_MEMORY_ORDER_RELAXED);
    uint64_t scope_alloc_bytes = vkr_atomic_uint64_load(
        &entry->scope_alloc_bytes, VKR_MEMORY_ORDER_RELAXED);
    uint64_t scope_depth =
        vkr_atomic_uint64_load(&entry->scope_depth, VKR_MEMORY_ORDER_RELAXED);

    uint64_t has_arena =
        vkr_atomic_uint64_load(&entry->has_arena, VKR_MEMORY_ORDER_RELAXED);
    uint64_t used_bytes = vkr_atomic_uint64_load(&entry->arena_used_bytes,
                                                 VKR_MEMORY_ORDER_RELAXED);
    uint64_t committed_bytes = vkr_atomic_uint64_load(
        &entry->arena_committed_bytes, VKR_MEMORY_ORDER_RELAXED);
    uint64_t reserved_bytes = vkr_atomic_uint64_load(
        &entry->arena_reserved_bytes, VKR_MEMORY_ORDER_RELAXED);
    uint64_t peak_used_bytes = vkr_atomic_uint64_load(
        &entry->arena_peak_used_bytes, VKR_MEMORY_ORDER_RELAXED);

    /* Restart the interval maxima from the current values: a scope that
     * stays open into the next interval counts there too. If the slot was
     * reused during this copy, the reset lands on the new owner's first
     * interval and can only lose that interval's maximum. */
    uint64_t scope_depth_high = vkr_atomic_uint64_exchange(
        &entry->scope_depth_high, scope_depth, VKR_MEMORY_ORDER_RELAXED);
    uint64_t interval_peak_used_bytes =
        vkr_atomic_uint64_exchange(&entry->arena_interval_peak_used_bytes,
                                   used_bytes, VKR_MEMORY_ORDER_RELAXED);

    // Pairs with the release fence in vkr_allocator_registry_claim().
    atomic_thread_fence(VKR_MEMORY_ORDER_ACQUIRE);
    if (vkr_atomic_uint32_load(&entry->generation, VKR_MEMORY_ORDER_RELAXED) !=
        generation) {
      // Released or reused during the copy: the values mix two identities.
      continue;
    }

    scope_depth_high = vkr_max_u64(scope_depth_high, scope_depth);
    interval_peak_used_bytes =
        vkr_max_u64(interval_peak_used_bytes, used_bytes);
    peak_used_bytes = vkr_max_u64(peak_used_bytes, interval_peak_used_bytes);

    sample->type = (VkrAllocatorType)type;
    sample->owner = (VkrAllocatorRegistryOwner)owner;
    sample->slot = index + 1u;
    sample->generation = generation;
    sample->alloc_count = alloc_count;
    sample->free_count = free_count;
    sample->realloc_count = realloc_count;
    sample->live_bytes = live_bytes;
    sample->scope_count = scope_count;
    sample->scope_alloc_bytes = scope_alloc_bytes;
    sample->scope_depth = (uint32_t)scope_depth;
    sample->scope_depth_high = (uint32_t)scope_depth_high;
    sample->has_arena = has_arena != 0 ? true_v : false_v;
    sample->arena_used_bytes = used_bytes;
    sample->arena_committed_bytes = committed_bytes;
    sample->arena_reserved_bytes = reserved_bytes;
    sample->arena_peak_used_bytes = peak_used_bytes;
    sample->arena_interval_peak_used_bytes = interval_peak_used_bytes;

    found++;
  }

  return found;
}

uint32_t vkr_allocator_registry_rejected_count(void) {
  return vkr_atomic_uint32_load(&g_vkr_allocator_registry.rejected_count,
                                VKR_MEMORY_ORDER_RELAXED);
}

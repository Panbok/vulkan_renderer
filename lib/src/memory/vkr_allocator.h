/**
 * @file vkr_allocator.h
 * @brief Defines the abstract memory allocator system interface, that can be
 * used to allocate memory for the application.
 */

// todo: in the future we need to clean up arena memory from tags, formating,
// etc. since all of this will be handled by the allocator system

#include "defines.h"

#pragma once

/**
 * @brief Define VKR_ALLOCATOR_DISABLE_STATS to disable atomic statistics
 * tracking on every allocation. This can significantly improve performance
 * in release builds where detailed memory tracking is not needed.
 *
 * When disabled:
 * - Global atomic counters are not updated on alloc/free/realloc
 * - Per-allocator stats are still updated (non-atomic, per-allocator)
 * - vkr_allocator_report() becomes a no-op
 */
#ifndef VKR_ALLOCATOR_DISABLE_STATS
#define VKR_ALLOCATOR_DISABLE_STATS 0
#endif

/**
 * @brief Define VKR_ALLOCATOR_ENABLE_LOGGING to enable logging of every
 * allocation. This can significantly degrade performance in release builds.
 */
#ifndef VKR_ALLOCATOR_ENABLE_LOGGING
#define VKR_ALLOCATOR_ENABLE_LOGGING 0
#endif

// Forward declaration to avoid including vkr_threads.h (which depends on this
// header).
typedef struct s_VkrMutex *VkrMutex;

// Forward declaration to avoid including arena.h; the registry reads an
// arena's footprint for arena-backed registry entries.
typedef struct Arena Arena;

typedef enum VkrAllocatorMemoryTag {
  VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN,
  VKR_ALLOCATOR_MEMORY_TAG_ARRAY,
  VKR_ALLOCATOR_MEMORY_TAG_STRING,
  VKR_ALLOCATOR_MEMORY_TAG_VECTOR,
  VKR_ALLOCATOR_MEMORY_TAG_QUEUE,
  VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
  VKR_ALLOCATOR_MEMORY_TAG_BUFFER,
  VKR_ALLOCATOR_MEMORY_TAG_RENDERER,
  VKR_ALLOCATOR_MEMORY_TAG_FILE,
  VKR_ALLOCATOR_MEMORY_TAG_TEXTURE,
  VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE,
  VKR_ALLOCATOR_MEMORY_TAG_FREELIST,
  VKR_ALLOCATOR_MEMORY_TAG_VULKAN,
  VKR_ALLOCATOR_MEMORY_TAG_GPU,

  VKR_ALLOCATOR_MEMORY_TAG_MAX,
} VkrAllocatorMemoryTag;

typedef enum VkrAllocatorType {
  VKR_ALLOCATOR_TYPE_ARENA,
  VKR_ALLOCATOR_TYPE_POOL,
  VKR_ALLOCATOR_TYPE_DMEMORY,
  VKR_ALLOCATOR_TYPE_UNKNOWN,

  VKR_ALLOCATOR_TYPE_MAX,
} VkrAllocatorType;

/**
 * @brief Statistics for the allocator.
 */
typedef struct VkrAllocatorStatistics {
  uint64_t total_allocs;
  uint64_t total_frees;
  uint64_t total_reallocs;
  uint64_t total_zeros;
  uint64_t total_copies;
  uint64_t total_sets;

  uint64_t total_allocated;
  uint64_t tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_MAX];

  // Scope/temporary allocation tracking
  uint64_t total_scopes_created;
  uint64_t total_scopes_destroyed;
  uint64_t total_temp_bytes; // Current bytes allocated within active scopes
  uint64_t peak_temp_bytes;  // High-water mark for concurrent temp allocations
} VkrAllocatorStatistics;

// Forward declaration
typedef struct VkrAllocator VkrAllocator;

/**
 * @brief Handle representing a temporary allocation scope.
 *
 * Caller creates a scope, calls functions that allocate, then destroys the
 * scope. Functions being called don't need to know about the scope - they
 * just allocate normally via vkr_allocator_alloc().
 *
 * For arena allocators, this maps directly to scratch (position save/restore).
 * For other allocators, this can track allocations for bulk free.
 */
typedef struct VkrAllocatorScope {
  VkrAllocator *allocator;
  void *scope_data;        // Allocator-specific (e.g., Scratch* for arena)
  uint64_t bytes_at_start; // Bytes allocated when scope was created
  uint64_t total_allocated_at_start; // Allocator stats snapshot at scope start
  uint64_t tagged_allocs_at_start[VKR_ALLOCATOR_MEMORY_TAG_MAX];
  // Registry live bytes at scope start; ending the scope restores them.
  uint64_t registry_live_at_start;
  bool8_t tags_snapshot_valid;
} VkrAllocatorScope;

/**
 * @brief Abstract interface that every allocator must implement.
 *
 * @note Thread Safety: Individual VkrAllocator instances are NOT thread-safe.
 *       Each allocator should be used from a single thread, or callers must
 *       provide external synchronization. The global statistics
 *       (vkr_allocator_get_global_statistics) are thread-safe via atomics.
 */
struct VkrAllocator {
  VkrAllocatorType type;
  VkrAllocatorStatistics stats;
  void *ctx; // allocator-specific state, e.g., Arena*

  // Internal scope state
  uint32_t scope_depth;           // How many scopes deep we are (0 = none)
  uint64_t scope_bytes_allocated; // Bytes allocated in current scope stack

  // 1-based named-registry slot; 0 when unregistered. Zero-initialize the
  // allocator before creation and change this only through
  // vkr_allocator_register_name()/vkr_allocator_unregister().
  uint32_t registry_slot;

  // Allocate size bytes. Alignment can be handled inside if you prefer.
  void *(*alloc)(void *ctx, uint64_t size, VkrAllocatorMemoryTag tag);

  // Allocate size bytes with a specific alignment.
  void *(*alloc_aligned)(void *ctx, uint64_t size, uint64_t alignment,
                         VkrAllocatorMemoryTag tag);

  // Free with known old_size. For arenas, this can be a no-op.
  void (*free)(void *ctx, void *ptr, uint64_t old_size,
               VkrAllocatorMemoryTag tag);

  // Free with known old_size and alignment. For arenas, this can be a no-op.
  void (*free_aligned)(void *ctx, void *ptr, uint64_t old_size,
                       uint64_t alignment, VkrAllocatorMemoryTag tag);

  // Reallocate: returns new pointer. For arenas: alloc+copy+leave old as-is.
  void *(*realloc)(void *ctx, void *ptr, uint64_t old_size, uint64_t new_size,
                   VkrAllocatorMemoryTag tag);

  // Reallocate with alignment. For allocators that can't resize in-place,
  // this may perform alloc+copy+free_aligned internally.
  void *(*realloc_aligned)(void *ctx, void *ptr, uint64_t old_size,
                           uint64_t new_size, uint64_t alignment,
                           VkrAllocatorMemoryTag tag);

  // Optional: Scope-based temporary allocation support.
  // These callbacks receive the full VkrAllocator* so they can update
  // scope_depth and scope_bytes_allocated. The allocator's ctx is accessible
  // via allocator->ctx.
  VkrAllocatorScope (*begin_scope)(struct VkrAllocator *allocator);
  void (*end_scope)(struct VkrAllocator *allocator, VkrAllocatorScope *scope,
                    VkrAllocatorMemoryTag tag);

  bool8_t supports_scopes;
  bool8_t accounting_released; // Set to true after release_global_accounting
};

/**
 * @brief Allocates memory from the allocator.
 * @param allocator The allocator to use.
 * @param size The size of the memory to allocate.
 * @param tag The tag to associate with the allocation.
 * @param alloc_line The line number of the allocation.
 * @param alloc_file The file name of the allocation.
 * @return Pointer to the allocated memory, or NULL on failure.
 */
void *_vkr_allocator_alloc(VkrAllocator *allocator, uint64_t size,
                           VkrAllocatorMemoryTag tag, uint32_t alloc_line,
                           const char *alloc_file);

/**
 * @brief Allocates memory from the allocator with a specific alignment.
 * @param allocator The allocator to use.
 * @param size The size of the memory to allocate.
 * @param alignment The alignment to use for the allocation.
 * @param tag The tag to associate with the allocation.
 * @param alloc_line The line number of the allocation.
 * @param alloc_file The file name of the allocation.
 * @return Pointer to the allocated memory, or NULL on failure.
 */
void *_vkr_allocator_alloc_aligned(VkrAllocator *allocator, uint64_t size,
                                   uint64_t alignment,
                                   VkrAllocatorMemoryTag tag,
                                   uint32_t alloc_line, const char *alloc_file);

#define vkr_allocator_alloc(allocator, size, tag)                              \
  _vkr_allocator_alloc(allocator, size, tag, __LINE__, __FILE__)

#define vkr_allocator_alloc_aligned(allocator, size, alignment, tag)           \
  _vkr_allocator_alloc_aligned(allocator, size, alignment, tag, __LINE__,      \
                               __FILE__)

void *_vkr_allocator_alloc_ts(VkrAllocator *allocator, uint64_t size,
                              VkrAllocatorMemoryTag tag, VkrMutex mutex,
                              uint32_t alloc_line, const char *alloc_file);
void *_vkr_allocator_alloc_aligned_ts(VkrAllocator *allocator, uint64_t size,
                                      uint64_t alignment,
                                      VkrAllocatorMemoryTag tag, VkrMutex mutex,
                                      uint32_t alloc_line,
                                      const char *alloc_file);
#define vkr_allocator_alloc_ts(allocator, size, tag, mutex)                    \
  _vkr_allocator_alloc_ts(allocator, size, tag, mutex, __LINE__, __FILE__)
#define vkr_allocator_alloc_aligned_ts(allocator, size, alignment, tag, mutex) \
  _vkr_allocator_alloc_aligned_ts(allocator, size, alignment, tag, mutex,      \
                                  __LINE__, __FILE__)

/**
 * @brief Frees memory from the allocator.
 * @param allocator The allocator to use.
 * @param ptr The pointer to the memory to free.
 * @param tag The tag to associate with the allocation.
 * @param old_size The size of the memory to free.
 * @note If you don't know old_size, call with 0; stats will not adjust
 * bytes_current for that call.
 */
void vkr_allocator_free(VkrAllocator *allocator, void *ptr, uint64_t old_size,
                        VkrAllocatorMemoryTag tag);
void vkr_allocator_free_ts(VkrAllocator *allocator, void *ptr,
                           uint64_t old_size, VkrAllocatorMemoryTag tag,
                           VkrMutex mutex);
/**
 * @brief Frees memory from the allocator with a specific alignment.
 * @param allocator The allocator to use.
 * @param ptr The pointer to the memory to free.
 * @param old_size The size of the memory to free.
 * @param alignment The alignment to use for the free.
 * @param tag The tag to associate with the allocation.
 */
void vkr_allocator_free_aligned(VkrAllocator *allocator, void *ptr,
                                uint64_t old_size, uint64_t alignment,
                                VkrAllocatorMemoryTag tag);
void vkr_allocator_free_aligned_ts(VkrAllocator *allocator, void *ptr,
                                   uint64_t old_size, uint64_t alignment,
                                   VkrAllocatorMemoryTag tag, VkrMutex mutex);

/**
 * @brief Reallocates memory from the allocator.
 * @param allocator The allocator to use.
 * @param ptr The pointer to the memory to reallocate.
 * @param old_size The size of the memory to reallocate.
 * @param new_size The new size of the memory to reallocate.
 * @param tag The tag to associate with the allocation.
 */
void *vkr_allocator_realloc(VkrAllocator *allocator, void *ptr,
                            uint64_t old_size, uint64_t new_size,
                            VkrAllocatorMemoryTag tag);
void *vkr_allocator_realloc_ts(VkrAllocator *allocator, void *ptr,
                               uint64_t old_size, uint64_t new_size,
                               VkrAllocatorMemoryTag tag, VkrMutex mutex);

/**
 * @brief Sets the memory.
 * @param allocator The allocator to use. (Optional)
 * @param ptr The pointer to the memory to set.
 * @param value The value to set.
 * @param size The size of the memory to set.
 */
void vkr_allocator_set(VkrAllocator *allocator, void *ptr, uint32_t value,
                       uint64_t size);

/**
 * @brief Zeros the memory.
 * @param allocator The allocator to use. (Optional)
 * @param ptr The pointer to the memory to zero.
 * @param size The size of the memory to zero.
 */
void vkr_allocator_zero(VkrAllocator *allocator, void *ptr, uint64_t size);

/**
 * @brief Copies the memory.
 * @param allocator The allocator to use. (Optional)
 * @param dst The pointer to the memory to copy to.
 * @param src The pointer to the memory to copy from.
 * @param size The size of the memory to copy.
 */
void vkr_allocator_copy(VkrAllocator *allocator, void *dst, const void *src,
                        uint64_t size);

/**
 * @brief Gets the statistics from the allocator.
 * @param allocator The allocator to use.
 * @return The statistics from the allocator.
 */
VkrAllocatorStatistics
vkr_allocator_get_statistics(const VkrAllocator *allocator);

/**
 * @brief Prints the statistics from the allocator.
 * @param allocator The allocator to use.
 * @return Dynamically allocated string with statistics. Caller must free with
 * vkr_allocator_free. Returns NULL on allocation failure.
 */
char *vkr_allocator_print_statistics(VkrAllocator *allocator);

/**
 * @brief Gets the global statistics from the allocator.
 * @return The global statistics from the allocator.
 */
VkrAllocatorStatistics vkr_allocator_get_global_statistics();

/**
 * @brief Prints the global statistics from the allocator.
 * @param allocator The allocator to use.
 * @return Dynamically allocated string with global statistics. Caller must free
 * with vkr_allocator_free. Returns NULL on allocation failure.
 */
char *vkr_allocator_print_global_statistics(VkrAllocator *allocator);

/**
 * @brief Adjust global allocator byte statistics as if all outstanding
 *        allocations made through this allocator were freed.
 *
 * Intended for allocators that are destroyed/reset in bulk without calling
 * `vkr_allocator_free()` for each allocation.
 *
 * @note Call this exactly once, immediately before destroying the allocator's
 *       backing store. After this call, the allocator becomes unusable for
 *       accounting purposes (accounting_released flag is set). Subsequent calls
 *       to this function will assert/log and return early to prevent
 *       double-decrement of global statistics.
 *
 * @note Thread safety: Callers must synchronize with concurrent
 *       vkr_allocator_alloc/vkr_allocator_free users. This function modifies
 *       global atomic statistics and the allocator's accounting_released flag.
 *       It is idempotent if called multiple times (returns early if
 *       accounting_released is already set or if total_allocated is zero).
 */
void vkr_allocator_release_global_accounting(VkrAllocator *allocator);

/**
 * @brief Formats a size value to a human-readable string in a buffer.
 *
 * Formats the size using units: "bytes" (< 1 KB), "KB" (1 KB - 1 MB), "MB"
 * (1 MB - 1 GB), or "GB" (>= 1 GB). Decimal values use one decimal place
 * (e.g., "1.5 MB").
 *
 * @param buffer The buffer to write the formatted string to.
 * @param buffer_size The size of the buffer in bytes.
 * @param tag_name The tag name to include in the output (e.g., "RENDERER").
 * @param size_stat The size value in bytes to format.
 * @return The number of characters that would have been written (excluding the
 *         null terminator). If the return value >= buffer_size, the output was
 *         truncated. The buffer is always null-terminated when buffer_size > 0.
 */
size_t vkr_allocator_format_size_to_buffer(char *buffer, size_t buffer_size,
                                           const char *tag_name,
                                           uint64_t size_stat);

/**
 * @brief Reports externally allocated/freed memory to allocator statistics.
 *
 * @param allocator Allocator whose local stats should be updated (NULL to
 * update global stats only)
 * @param size Size of the allocation/free (in bytes)
 * @param tag Memory tag to update
 * @param is_allocation true_v to add bytes, false_v to subtract (saturates at
 * zero)
 */
void vkr_allocator_report(VkrAllocator *allocator, uint64_t size,
                          VkrAllocatorMemoryTag tag, bool8_t is_allocation);

/**
 * @brief Reallocates memory with a specific alignment.
 *
 * Falls back to alloc+copy+free_aligned when the allocator does not provide
 * an aligned realloc implementation.
 */
void *vkr_allocator_realloc_aligned(VkrAllocator *allocator, void *ptr,
                                    uint64_t old_size, uint64_t new_size,
                                    uint64_t alignment,
                                    VkrAllocatorMemoryTag tag);
void *vkr_allocator_realloc_aligned_ts(VkrAllocator *allocator, void *ptr,
                                       uint64_t old_size, uint64_t new_size,
                                       uint64_t alignment,
                                       VkrAllocatorMemoryTag tag,
                                       VkrMutex mutex);

// =============================================================================
// Scope-based Temporary Allocation API
// =============================================================================

/**
 * @brief Checks if the allocator supports scoped allocations.
 * @param allocator The allocator to check.
 * @return true if scopes are supported, false otherwise.
 */
bool8_t vkr_allocator_supports_scopes(const VkrAllocator *allocator);

/**
 * @brief Begins a temporary allocation scope.
 *
 * After this call, all allocations via vkr_allocator_alloc() are tracked as
 * temporary. Functions being called don't need any modification - they allocate
 * normally. The caller controls whether allocations are temporary by wrapping
 * calls in begin_scope/end_scope.
 *
 * @param allocator The allocator to create a scope on.
 * @return Scope handle for ending the scope later.
 *
 * @note Check with vkr_allocator_supports_scopes() before calling this
 * function. After this call, use vkr_allocator_scope_is_valid() to verify the
 *       returned scope handle is valid.
 */
VkrAllocatorScope vkr_allocator_begin_scope(VkrAllocator *allocator);

/**
 * @brief Ends a temporary allocation scope.
 *
 * For arena allocators: resets to saved position (like scratch_destroy).
 * For other allocators: may free tracked allocations.
 * Updates temp statistics.
 *
 * @param scope The scope to end.
 * @param tag Memory tag for statistics adjustment.
 */
void vkr_allocator_end_scope(VkrAllocatorScope *scope,
                             VkrAllocatorMemoryTag tag);

/**
 * @brief Checks if a scope handle is valid.
 * @param scope The scope to check.
 * @return true if the scope is valid, false otherwise.
 */
bool8_t vkr_allocator_scope_is_valid(const VkrAllocatorScope *scope);

/**
 * @brief Checks if allocator currently has active scopes.
 * @param allocator The allocator to check.
 * @return true if there are active scopes, false otherwise.
 */
bool8_t vkr_allocator_in_scope(const VkrAllocator *allocator);

/**
 * @brief Gets the current scope nesting depth.
 * @param allocator The allocator to check.
 * @return Current scope depth (0 = no active scope).
 */
uint32_t vkr_allocator_scope_depth(const VkrAllocator *allocator);

// =============================================================================
// Named allocator registry
// =============================================================================

/**
 * The registry publishes per-allocator counters to a sampler on any thread,
 * such as a recording that reads one sample per frame. It holds at most
 * VKR_ALLOCATOR_REGISTRY_CAPACITY live entries in static storage.
 *
 * Writers: the thread that owns a registered allocator or arena updates its
 * entry with relaxed atomic loads and stores. The owner is a single writer,
 * as the VkrAllocator contract already requires; `_ts` callers serialize
 * through their mutex. An unregistered allocator or arena tests one slot index
 * and does not touch the registry.
 *
 * Registration: register after creation and unregister before the backing
 * store is destroyed, from the owner thread or while the owner does not
 * allocate. Claiming and releasing a slot is lock-free and safe against other
 * registrations and samplers on any thread. Counting starts at registration.
 *
 * Sampling: vkr_allocator_registry_sample() takes no lock and does not
 * allocate. A slot generation detects a slot that was released or reused
 * during the copy; that entry is left out of the sample.
 */

#define VKR_ALLOCATOR_REGISTRY_CAPACITY 64
#define VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY 48

typedef enum VkrAllocatorRegistryOwner {
  // A VkrAllocator registered with vkr_allocator_register_name().
  VKR_ALLOCATOR_REGISTRY_OWNER_ALLOCATOR,
  // A raw Arena registered with vkr_allocator_register_arena_name(); its
  // alloc_count counts arena pushes and live_bytes is its used size.
  VKR_ALLOCATOR_REGISTRY_OWNER_ARENA,
} VkrAllocatorRegistryOwner;

/**
 * @brief One registry entry copied by vkr_allocator_registry_sample().
 *
 * Counts marked cumulative grow from registration; a consumer computes
 * per-frame values as the difference between two samples with the same slot
 * and generation.
 */
typedef struct VkrAllocatorRegistrySample {
  char name[VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY]; // Null-terminated.
  VkrAllocatorType type;
  VkrAllocatorRegistryOwner owner;
  uint32_t slot;       // 1-based slot.
  uint32_t generation; // Differs whenever the slot holds another entry.

  uint64_t alloc_count;   // Cumulative.
  uint64_t free_count;    // Cumulative.
  uint64_t realloc_count; // Cumulative.
  // Bytes requested through the allocator and not yet freed or rewound by a
  // scope end, counted from registration. Independent of
  // VKR_ALLOCATOR_DISABLE_STATS.
  uint64_t live_bytes;

  uint64_t scope_count;       // Cumulative scopes begun.
  uint64_t scope_alloc_bytes; // Cumulative bytes allocated inside scopes.
  uint32_t scope_depth;       // Depth when sampled.
  // Deepest scope nesting since the previous sample, including scope_depth.
  uint32_t scope_depth_high;

  // Arena footprint; zero unless has_arena. Used bytes sum every block in
  // the live chain, alignment padding included. Committed and reserved bytes
  // include blocks kept on the arena's free list.
  bool8_t has_arena;
  uint64_t arena_used_bytes;
  uint64_t arena_committed_bytes;
  uint64_t arena_reserved_bytes;
  uint64_t arena_peak_used_bytes; // Highest used size since registration.
  // Highest used size since the previous sample, including the current one.
  uint64_t arena_interval_peak_used_bytes;
} VkrAllocatorRegistrySample;

/**
 * @brief Registers an allocator under a display name.
 *
 * An arena-backed allocator also publishes its arena footprint unless that
 * arena is already registered through another entry. A name longer than
 * VKR_ALLOCATOR_REGISTRY_NAME_CAPACITY - 1 bytes is truncated.
 *
 * @return true_v on success; false_v when allocator or name is NULL, the
 * allocator is already registered, or every slot is in use (counted by
 * vkr_allocator_registry_rejected_count()).
 */
bool8_t vkr_allocator_register_name(VkrAllocator *allocator, const char *name);

/**
 * @brief Releases the allocator's registry slot. No-op when unregistered.
 * Call before destroying the allocator's backing store.
 */
void vkr_allocator_unregister(VkrAllocator *allocator);

/**
 * @brief Registers a raw arena that is not used through a VkrAllocator.
 *
 * arena_alloc() counts as an allocation and the used size is the live size.
 * arena_destroy() releases the slot when the caller has not.
 *
 * @return true_v on success; false_v when arena or name is NULL, the arena is
 * already registered, or every slot is in use.
 */
bool8_t vkr_allocator_register_arena_name(Arena *arena, const char *name);

/**
 * @brief Releases a raw arena's registry slot. No-op when the arena is
 * unregistered; an arena bound to a registered VkrAllocator is released by
 * vkr_allocator_unregister() instead.
 */
void vkr_allocator_unregister_arena(Arena *arena);

/**
 * @brief Copies every live registry entry and resets their interval maxima.
 *
 * Safe on any thread; takes no lock and does not allocate. Each copied entry's
 * scope_depth_high and arena_interval_peak_used_bytes restart from the current
 * depth and used size, so the next sample reports maxima since this one.
 * Entries beyond capacity are counted but neither copied nor reset. Sampling
 * from two threads splits the intervals between them.
 *
 * @param out Destination array; may be NULL when capacity is 0.
 * @param capacity Number of elements in out.
 * @return Number of live entries found. A value above capacity means the
 * sample was truncated.
 */
uint32_t vkr_allocator_registry_sample(VkrAllocatorRegistrySample *out,
                                       uint32_t capacity);

/**
 * @brief Number of registrations refused because every slot was in use.
 */
uint32_t vkr_allocator_registry_rejected_count(void);

/**
 * @brief Lib-internal arena hook: publishes a registered arena's footprint.
 * @param arena First block of a registered arena.
 * @param rewound_from_bytes Used size before a rewind, or 0 after a push.
 * @param pushed true_v after a successful arena push.
 */
void vkr_allocator_registry_note_arena(Arena *arena,
                                       uint64_t rewound_from_bytes,
                                       bool8_t pushed);

/**
 * @brief Lib-internal arena hook: arena_destroy() detaches a registered arena.
 * Releases a raw arena's slot; clears the footprint of an allocator entry.
 */
void vkr_allocator_registry_detach_arena(Arena *arena);

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

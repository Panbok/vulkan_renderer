#pragma once

#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"

#include <vulkan/vulkan.h>

// Default commit/reserve sizes for the Vulkan host allocator. These can be
// tuned later if needed.
#define VKR_VULKAN_ALLOCATOR_COMMIT_SIZE MB(32)
#define VKR_VULKAN_ALLOCATOR_RESERVE_SIZE MB(256)
#define VKR_VULKAN_ALLOCATOR_ARENA_RESERVE KB(512)
#define VKR_VULKAN_ALLOCATOR_ARENA_COMMIT KB(64)

typedef enum VulkanAllocationSource {
  VULKAN_ALLOCATION_SOURCE_UNKNOWN,
  VULKAN_ALLOCATION_SOURCE_DMEMORY,
  VULKAN_ALLOCATION_SOURCE_ARENA,
} VulkanAllocationSource;

typedef struct VulkanAllocator {
  // Thread-safe allocators
  VkrAllocator allocator;       // dmemory-backed
  VkrAllocator arena_allocator; // arena-backed for command scope
  VkrDMemory dmemory;
  Arena *arena;
  VkrMutex mutex;

  // Reference count for active arena (command-scope) allocations.
  // Arena is only cleared when this drops to zero, preventing use-after-free
  // when multiple command-scope allocations coexist. Protected by mutex.
  uint32_t arena_alloc_count;

  // Vulkan-facing callbacks; pUserData points back to this struct.
  VkAllocationCallbacks callbacks;
} VulkanAllocator;

/**
 * @brief Initialize a VulkanAllocator backed by VkrDMemory.
 *
 * @param host_allocator Allocator used for mutex allocation (must outlive this
 * allocator).
 * @param out_allocator Target allocator to initialize.
 * @param commit_size Initial committed size of the dmemory region.
 * @param reserve_size Maximum reserved size for the dmemory region.
 * @return true_v on success, false_v otherwise.
 */
bool32_t vulkan_allocator_create(VkrAllocator *host_allocator,
                                 VulkanAllocator *out_allocator,
                                 uint64_t commit_size, uint64_t reserve_size);

/**
 * @brief Destroy a VulkanAllocator and release its resources.
 *
 * @param host_allocator Allocator used to free the mutex memory.
 * @param allocator Allocator to destroy.
 */
void vulkan_allocator_destroy(VkrAllocator *host_allocator,
                              VulkanAllocator *allocator);

/**
 * @brief Retrieve the VkAllocationCallbacks for use with Vulkan API calls.
 *
 * @param allocator Target VulkanAllocator.
 * @return Pointer to VkAllocationCallbacks configured for the allocator.
 */
VkAllocationCallbacks *vulkan_allocator_callbacks(VulkanAllocator *allocator);

/**
 * @brief Identify the allocation source for a given pointer.
 *
 * @param allocator Target VulkanAllocator.
 * @param ptr Pointer to check against known allocation sources.
 * @return The allocation source (DMEMORY, ARENA, or UNKNOWN).
 */
VulkanAllocationSource
vulkan_allocator_source_from_ptr(VulkanAllocator *allocator, void *ptr);

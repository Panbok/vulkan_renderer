#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_gpu_memory.h"

#include <vulkan/vulkan.h>

typedef enum VkrBindlessVkMemoryClass {
  VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE = 0,
  VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
  VKR_BINDLESS_VK_MEMORY_CLASS_READBACK,
  VKR_BINDLESS_VK_MEMORY_CLASS_COUNT,
} VkrBindlessVkMemoryClass;

typedef enum VkrBindlessVkMemoryKind {
  VKR_BINDLESS_VK_MEMORY_KIND_BUFFER = 0,
  VKR_BINDLESS_VK_MEMORY_KIND_IMAGE,
  VKR_BINDLESS_VK_MEMORY_KIND_COUNT,
} VkrBindlessVkMemoryKind;

typedef struct VkrBindlessVkMemoryPoolKey {
  VkrBindlessVkMemoryClass memory_class;
  VkrBindlessVkMemoryKind kind;
  uint32_t memory_type_index;
  bool8_t device_address_required;
} VkrBindlessVkMemoryPoolKey;

typedef struct VkrBindlessVkMemoryPoolConfig {
  VkrAllocator *allocator;
  VkDevice device;
  uint64_t block_sizes[VKR_BINDLESS_VK_MEMORY_CLASS_COUNT]
                      [VKR_BINDLESS_VK_MEMORY_KIND_COUNT];
  uint32_t max_blocks;
  uint32_t max_blocks_per_pool;
  uint32_t max_allocations_per_block;
} VkrBindlessVkMemoryPoolConfig;

typedef struct VkrBindlessVkPooledAllocation {
  VkDeviceMemory memory;
  VkDeviceSize memory_size;
  VkDeviceSize offset;
  void *mapped;
  VkMemoryPropertyFlags properties;
  VkrGpuAllocationHandle handle;
  VkrBindlessVkMemoryPoolKey key;
  uint32_t block_index;
  bool8_t valid;
  bool8_t retired;
  uint64_t retire_value;
} VkrBindlessVkPooledAllocation;

typedef struct VkrBindlessVkMemoryPoolMetrics {
  uint64_t physical_allocations_live;
  uint64_t physical_allocations_peak;
  uint64_t physical_allocations_created;
  uint64_t physical_allocated_bytes;
  uint64_t physical_allocated_bytes_peak;
  uint64_t block_capacity_failures;
  VkrGpuMemoryMetrics aggregate;
} VkrBindlessVkMemoryPoolMetrics;

typedef struct VkrBindlessVkMemoryPoolManager VkrBindlessVkMemoryPoolManager;

typedef struct VkrBindlessVkMappedRange {
  uint64_t offset;
  uint64_t size;
} VkrBindlessVkMappedRange;

/**
 * Expands a mapped write/read to Vulkan's non-coherent atom boundary. The end
 * may equal allocation_size without being atom-aligned, as required by
 * VkMappedMemoryRange.
 */
bool8_t vkr_bindless_vulkan_noncoherent_range(
    uint64_t offset, uint64_t size, uint64_t allocation_size,
    uint64_t noncoherent_atom_size, VkrBindlessVkMappedRange *out_range);

/** Returns a lower-is-better placement rank, or -1 when incompatible. */
int32_t
vkr_bindless_vulkan_memory_type_rank(VkrBindlessVkMemoryClass memory_class,
                                     VkMemoryPropertyFlags properties);

bool8_t
vkr_bindless_vulkan_memory_pool_key_equal(VkrBindlessVkMemoryPoolKey left,
                                          VkrBindlessVkMemoryPoolKey right);

bool8_t vkr_bindless_vulkan_memory_block_size(uint64_t configured_size,
                                              uint64_t resource_size,
                                              uint64_t alignment,
                                              uint64_t *out_size);

bool8_t vkr_bindless_vulkan_memory_pool_create(
    const VkrBindlessVkMemoryPoolConfig *config,
    VkrBindlessVkMemoryPoolManager **out_manager);
void vkr_bindless_vulkan_memory_pool_destroy(
    VkrBindlessVkMemoryPoolManager *manager);

bool8_t vkr_bindless_vulkan_memory_pool_allocate(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    VkMemoryPropertyFlags properties, VkDeviceSize size, VkDeviceSize alignment,
    VkrBindlessVkPooledAllocation *out_allocation);

/** Releases a placement only after its submit value is proven complete. */
bool8_t vkr_bindless_vulkan_memory_pool_release(
    VkrBindlessVkMemoryPoolManager *manager,
    VkrBindlessVkPooledAllocation *allocation, uint64_t last_use_submit_value,
    uint64_t completed_submit_value);
bool8_t vkr_bindless_vulkan_memory_pool_retire(
    VkrBindlessVkMemoryPoolManager *manager,
    VkrBindlessVkPooledAllocation *allocation, uint64_t retire_value);

void vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    uint64_t size);
void vkr_bindless_vulkan_memory_pool_record_dedicated_release(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    uint64_t size, bool8_t retired);
bool8_t vkr_bindless_vulkan_memory_pool_record_dedicated_retire(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    uint64_t size);
void vkr_bindless_vulkan_memory_pool_record_native_failure(
    VkrBindlessVkMemoryPoolManager *manager);

void vkr_bindless_vulkan_memory_pool_get_metrics(
    const VkrBindlessVkMemoryPoolManager *manager,
    VkrBindlessVkMemoryPoolMetrics *out_metrics);

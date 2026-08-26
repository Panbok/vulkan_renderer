#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_renderer.h"

#include <vulkan/vulkan.h>

typedef enum VkrVulkanMemoryClass {
  VKR_VULKAN_MEMORY_CLASS_DEVICE = 0,
  /** Host-written memory the GPU reads directly: descriptor buffers, frame
      roots, material rows. Prefers device-local host-visible memory so the
      write lands where the GPU reads it. */
  VKR_VULKAN_MEMORY_CLASS_UPLOAD,
  /** Bulk transfer sources that are only ever a copy source. These are large
      and short-lived, so they must not compete for the small device-local
      host-visible heap that UPLOAD depends on. */
  VKR_VULKAN_MEMORY_CLASS_STAGING,
  VKR_VULKAN_MEMORY_CLASS_READBACK,
  VKR_VULKAN_MEMORY_CLASS_COUNT,
} VkrVulkanMemoryClass;

typedef enum VkrVulkanMemoryKind {
  VKR_VULKAN_MEMORY_KIND_BUFFER = 0,
  VKR_VULKAN_MEMORY_KIND_IMAGE,
  VKR_VULKAN_MEMORY_KIND_COUNT,
} VkrVulkanMemoryKind;

typedef struct VkrVulkanMemoryPoolKey {
  VkrVulkanMemoryClass memory_class;
  VkrVulkanMemoryKind kind;
  uint32_t memory_type_index;
  bool8_t device_address_required;
} VkrVulkanMemoryPoolKey;

typedef struct VkrVulkanMemoryPoolConfig {
  VkrAllocator *allocator;
  VkDevice device;
  uint64_t block_sizes[VKR_VULKAN_MEMORY_CLASS_COUNT]
                      [VKR_VULKAN_MEMORY_KIND_COUNT];
  uint32_t max_blocks;
  uint32_t max_blocks_per_pool;
  uint32_t max_allocations_per_block;
} VkrVulkanMemoryPoolConfig;

typedef struct VkrVulkanPooledAllocation {
  VkDeviceMemory memory;
  VkDeviceSize memory_size;
  VkDeviceSize offset;
  void *mapped;
  VkMemoryPropertyFlags properties;
  VkrGpuAllocationHandle handle;
  VkrVulkanMemoryPoolKey key;
  VkDeviceSize requested_size;
  VkrGpuAllocationOwner owner;
  uint32_t block_index;
  bool8_t valid;
  bool8_t retired;
  uint64_t retire_value;
} VkrVulkanPooledAllocation;

typedef struct VkrVulkanMemoryPoolMetrics {
  uint64_t physical_allocations_live;
  uint64_t physical_allocations_peak;
  uint64_t physical_allocations_created;
  uint64_t physical_allocated_bytes;
  uint64_t physical_allocated_bytes_peak;
  uint64_t block_capacity_failures;
  VkrGpuMemoryMetrics aggregate;
  VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT];
  uint64_t live_bytes_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];
  uint64_t live_count_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];
} VkrVulkanMemoryPoolMetrics;

typedef struct VkrVulkanMemoryPoolManager VkrVulkanMemoryPoolManager;

typedef struct VkrVulkanMappedRange {
  uint64_t offset;
  uint64_t size;
} VkrVulkanMappedRange;

/**
 * Expands a mapped write/read to Vulkan's non-coherent atom boundary. The end
 * may equal allocation_size without being atom-aligned, as required by
 * VkMappedMemoryRange.
 */
bool8_t vkr_vulkan_noncoherent_range(uint64_t offset, uint64_t size,
                                     uint64_t allocation_size,
                                     uint64_t noncoherent_atom_size,
                                     VkrVulkanMappedRange *out_range);

/** Returns a lower-is-better placement rank, or -1 when incompatible. */
int32_t vkr_vulkan_memory_type_rank(VkrVulkanMemoryClass memory_class,
                                    VkMemoryPropertyFlags properties);

bool8_t vkr_vulkan_memory_pool_key_equal(VkrVulkanMemoryPoolKey left,
                                         VkrVulkanMemoryPoolKey right);

bool8_t vkr_vulkan_memory_block_size(uint64_t configured_size,
                                     uint64_t resource_size, uint64_t alignment,
                                     uint64_t *out_size);

void vkr_vulkan_memory_owner_record_allocate(
    VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT],
    VkrGpuAllocationOwner owner, uint64_t size);
bool8_t vkr_vulkan_memory_owner_record_release(
    VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT],
    VkrGpuAllocationOwner owner, uint64_t size);

bool8_t vkr_vulkan_memory_pool_create(const VkrVulkanMemoryPoolConfig *config,
                                      VkrVulkanMemoryPoolManager **out_manager);
void vkr_vulkan_memory_pool_destroy(VkrVulkanMemoryPoolManager *manager);

bool8_t vkr_vulkan_memory_pool_allocate(
    VkrVulkanMemoryPoolManager *manager, VkrVulkanMemoryPoolKey key,
    VkMemoryPropertyFlags properties, VkDeviceSize size, VkDeviceSize alignment,
    VkrGpuAllocationOwner owner, VkrVulkanPooledAllocation *out_allocation);

/** Releases a placement only after its submit value is proven complete. */
bool8_t vkr_vulkan_memory_pool_release(VkrVulkanMemoryPoolManager *manager,
                                       VkrVulkanPooledAllocation *allocation,
                                       uint64_t last_use_submit_value,
                                       uint64_t completed_submit_value);
bool8_t vkr_vulkan_memory_pool_retire(VkrVulkanMemoryPoolManager *manager,
                                      VkrVulkanPooledAllocation *allocation,
                                      uint64_t retire_value);

void vkr_vulkan_memory_pool_record_dedicated_allocate(
    VkrVulkanMemoryPoolManager *manager, VkrVulkanMemoryPoolKey key,
    VkrGpuAllocationOwner owner, uint64_t size);
void vkr_vulkan_memory_pool_record_dedicated_release(
    VkrVulkanMemoryPoolManager *manager, VkrVulkanMemoryPoolKey key,
    VkrGpuAllocationOwner owner, uint64_t size, bool8_t retired);
bool8_t vkr_vulkan_memory_pool_record_dedicated_retire(
    VkrVulkanMemoryPoolManager *manager, VkrVulkanMemoryPoolKey key,
    VkrGpuAllocationOwner owner, uint64_t size);
void vkr_vulkan_memory_pool_record_native_failure(
    VkrVulkanMemoryPoolManager *manager);

void vkr_vulkan_memory_pool_get_metrics(
    const VkrVulkanMemoryPoolManager *manager,
    VkrVulkanMemoryPoolMetrics *out_metrics);

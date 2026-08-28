#pragma once

#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_submit_ring.h"

typedef VkrGpuMemoryCore VkrMetalMemoryCore;

typedef enum VkrMetalMemoryStatus {
  VKR_METAL_MEMORY_STATUS_OK = VKR_GPU_MEMORY_STATUS_OK,
  VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT =
      VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT,
  VKR_METAL_MEMORY_STATUS_OUT_OF_HANDLES = VKR_GPU_MEMORY_STATUS_OUT_OF_HANDLES,
  VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES = VKR_GPU_MEMORY_STATUS_OUT_OF_BYTES,
  VKR_METAL_MEMORY_STATUS_FRAGMENTED = VKR_GPU_MEMORY_STATUS_FRAGMENTED,
  VKR_METAL_MEMORY_STATUS_OUT_OF_RANGE_METADATA =
      VKR_GPU_MEMORY_STATUS_OUT_OF_RANGE_METADATA,
  VKR_METAL_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS =
      VKR_GPU_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS,
  VKR_METAL_MEMORY_STATUS_STALE_HANDLE = VKR_GPU_MEMORY_STATUS_STALE_HANDLE,
  VKR_METAL_MEMORY_STATUS_RING_BUSY = VKR_GPU_MEMORY_STATUS_RING_BUSY,
  VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED =
      VKR_GPU_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED,
} VkrMetalMemoryStatus;

typedef VkrGpuAllocationHandle VkrMetalAllocationHandle;
typedef VkrGpuMemoryConfig VkrMetalMemoryConfig;
typedef VkrGpuPlacement VkrMetalPlacement;

typedef enum VkrMetalMemoryClass {
  VKR_METAL_MEMORY_CLASS_UNKNOWN = VKR_GPU_MEMORY_CLASS_UNKNOWN,
  VKR_METAL_MEMORY_CLASS_BUFFER = VKR_GPU_MEMORY_CLASS_BUFFER,
  VKR_METAL_MEMORY_CLASS_TEXTURE = VKR_GPU_MEMORY_CLASS_TEXTURE,
  VKR_METAL_MEMORY_CLASS_COUNT = VKR_GPU_MEMORY_CLASS_COUNT,
} VkrMetalMemoryClass;

typedef VkrGpuMemoryClassMetrics VkrMetalMemoryClassMetrics;
typedef VkrGpuMemoryMetrics VkrMetalMemoryMetrics;
typedef VkrGpuRetirementReleaseFn VkrMetalRetirementReleaseFn;

uint64_t
vkr_metal_memory_storage_requirement(const VkrMetalMemoryConfig *config);
VkrMetalMemoryStatus vkr_metal_memory_create(const VkrMetalMemoryConfig *config,
                                             void *storage,
                                             uint64_t storage_size,
                                             VkrMetalMemoryCore **out_memory);
VkrMetalMemoryStatus
vkr_metal_memory_allocate(VkrMetalMemoryCore *memory, uint64_t resource_size,
                          uint64_t alignment, uint32_t kind,
                          VkrMetalAllocationHandle *out_handle,
                          VkrMetalPlacement *out_placement);
VkrMetalMemoryStatus vkr_metal_memory_resolve(VkrMetalMemoryCore *memory,
                                              VkrMetalAllocationHandle handle,
                                              VkrMetalPlacement *out_placement);
VkrMetalMemoryStatus vkr_metal_memory_retire(VkrMetalMemoryCore *memory,
                                             VkrMetalAllocationHandle handle,
                                             uint64_t last_use_submit_value);
VkrMetalMemoryStatus
vkr_metal_memory_collect(VkrMetalMemoryCore *memory,
                         uint64_t completed_submit_value,
                         VkrMetalRetirementReleaseFn release_fn,
                         void *release_context, uint32_t *out_collected_count);
void vkr_metal_memory_record_native_failure(VkrMetalMemoryCore *memory);
void vkr_metal_memory_get_metrics(VkrMetalMemoryCore *memory,
                                  VkrMetalMemoryMetrics *out_metrics);
uint64_t vkr_metal_memory_effective_budget(uint64_t placement_capacity,
                                           uint64_t driver_budget);
/**
 * Returns free placement bytes below the effective occupancy ceiling.
 * `MTLDevice.currentAllocatedSize` is deliberately absent: device accounting
 * already includes the preallocated heap and cannot describe its free ranges.
 */
uint64_t vkr_metal_memory_effective_free_bytes(uint64_t placement_capacity,
                                               uint64_t placement_free_bytes,
                                               uint64_t driver_budget);
bool8_t vkr_metal_memory_can_allocate_before_reserve(
    const VkrMetalMemoryMetrics *metrics, uint64_t requested_size,
    uint64_t reserve_size);

typedef VkrGpuSubmitRingSlot VkrMetalSubmitRingSlot;
typedef VkrGpuSubmitRing VkrMetalSubmitRing;
typedef VkrGpuRingSlice VkrMetalRingSlice;
typedef VkrGpuAddressPair VkrMetalAddressPair;

uint64_t vkr_metal_submit_ring_storage_requirement(uint32_t slot_count);
bool8_t vkr_metal_submit_ring_total_size(uint64_t required_slot_size,
                                         uint64_t minimum_slot_size,
                                         uint32_t slot_count,
                                         uint64_t *out_total_size);
VkrMetalMemoryStatus vkr_metal_submit_ring_create(VkrMetalSubmitRing *ring,
                                                  uint64_t total_size,
                                                  uint32_t slot_count,
                                                  void *storage,
                                                  uint64_t storage_size);
VkrMetalMemoryStatus
vkr_metal_submit_ring_acquire(VkrMetalSubmitRing *ring, uint64_t requested_size,
                              uint64_t completed_submit_value,
                              VkrMetalRingSlice *out_slice);
VkrMetalMemoryStatus vkr_metal_submit_ring_submit(VkrMetalSubmitRing *ring,
                                                  VkrMetalRingSlice slice,
                                                  uint64_t submit_value);
void vkr_metal_submit_ring_cancel(VkrMetalSubmitRing *ring,
                                  VkrMetalRingSlice slice);
VkrMetalAddressPair vkr_metal_address_pair_slice(VkrMetalAddressPair buffer,
                                                 VkrMetalRingSlice slice);
const char *vkr_metal_memory_status_string(VkrMetalMemoryStatus status);

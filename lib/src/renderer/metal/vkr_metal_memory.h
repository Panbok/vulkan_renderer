#pragma once

#include "defines.h"

typedef struct VkrMetalMemoryCore VkrMetalMemoryCore;

typedef enum VkrMetalMemoryStatus {
  VKR_METAL_MEMORY_STATUS_OK = 0,
  VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT,
  VKR_METAL_MEMORY_STATUS_OUT_OF_HANDLES,
  VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES,
  VKR_METAL_MEMORY_STATUS_FRAGMENTED,
  VKR_METAL_MEMORY_STATUS_OUT_OF_RANGE_METADATA,
  VKR_METAL_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS,
  VKR_METAL_MEMORY_STATUS_STALE_HANDLE,
  VKR_METAL_MEMORY_STATUS_RING_BUSY,
  VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED,
} VkrMetalMemoryStatus;

typedef struct VkrMetalAllocationHandle {
  uint32_t index;
  uint32_t generation;
} VkrMetalAllocationHandle;

typedef struct VkrMetalMemoryConfig {
  uint64_t heap_size;
  uint32_t max_allocations;
  uint32_t max_retirements;
  uint32_t max_free_ranges;
} VkrMetalMemoryConfig;

typedef struct VkrMetalPlacement {
  uint64_t reserved_offset;
  uint64_t reserved_size;
  uint64_t resource_offset;
  uint64_t resource_size;
  uint64_t alignment;
  uint32_t kind;
} VkrMetalPlacement;

typedef enum VkrMetalMemoryClass {
  VKR_METAL_MEMORY_CLASS_UNKNOWN = 0,
  VKR_METAL_MEMORY_CLASS_BUFFER,
  VKR_METAL_MEMORY_CLASS_TEXTURE,
  VKR_METAL_MEMORY_CLASS_COUNT,
} VkrMetalMemoryClass;

typedef struct VkrMetalMemoryClassMetrics {
  uint64_t live_requested_bytes;
  uint64_t live_reserved_bytes;
  uint64_t retired_requested_bytes;
  uint64_t retired_reserved_bytes;
  uint64_t peak_requested_bytes;
  uint64_t peak_reserved_bytes;
  uint64_t allocations_created;
  uint64_t live_allocations;
  uint64_t retired_allocations;
  uint64_t peak_allocations;
  uint64_t alignment_waste_bytes;
} VkrMetalMemoryClassMetrics;

typedef struct VkrMetalMemoryMetrics {
  uint64_t heap_size;
  uint64_t free_bytes;
  uint64_t largest_free_range;
  uint64_t live_requested_bytes;
  uint64_t live_reserved_bytes;
  uint64_t retired_requested_bytes;
  uint64_t retired_reserved_bytes;
  uint64_t peak_requested_bytes;
  uint64_t peak_reserved_bytes;
  uint64_t allocations_created;
  uint64_t retirements_collected;
  uint64_t live_allocations;
  uint64_t retired_allocations;
  uint64_t peak_allocations;
  uint64_t alignment_waste_bytes;
  uint64_t byte_exhaustion_failures;
  uint64_t fragmentation_failures;
  uint64_t handle_exhaustion_failures;
  uint64_t range_metadata_failures;
  uint64_t retirement_capacity_failures;
  uint64_t stale_handle_failures;
  uint64_t native_allocation_failures;
  VkrMetalMemoryClassMetrics classes[VKR_METAL_MEMORY_CLASS_COUNT];
} VkrMetalMemoryMetrics;

typedef void (*VkrMetalRetirementReleaseFn)(void *context, uint32_t slot_index,
                                            const VkrMetalPlacement *placement);

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

typedef struct VkrMetalSubmitRingSlot {
  uint64_t retire_submit_value;
  uint8_t state;
} VkrMetalSubmitRingSlot;

typedef struct VkrMetalSubmitRing {
  VkrMetalSubmitRingSlot *slots;
  uint64_t total_size;
  uint64_t slot_size;
  uint32_t slot_count;
  uint32_t next_slot;
  uint64_t acquires;
  uint64_t reuses;
  uint64_t busy_failures;
} VkrMetalSubmitRing;

typedef struct VkrMetalRingSlice {
  uint64_t offset;
  uint64_t size;
  uint32_t slot_index;
} VkrMetalRingSlice;

typedef struct VkrMetalAddressPair {
  void *cpu_address;
  uint64_t gpu_address;
  uint64_t size;
} VkrMetalAddressPair;

uint64_t vkr_metal_submit_ring_storage_requirement(uint32_t slot_count);

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

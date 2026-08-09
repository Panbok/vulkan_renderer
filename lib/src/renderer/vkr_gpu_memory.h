#pragma once

#include "defines.h"

typedef struct VkrGpuMemoryCore VkrGpuMemoryCore;

typedef enum VkrGpuMemoryStatus {
  VKR_GPU_MEMORY_STATUS_OK = 0,
  VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT,
  VKR_GPU_MEMORY_STATUS_OUT_OF_HANDLES,
  VKR_GPU_MEMORY_STATUS_OUT_OF_BYTES,
  VKR_GPU_MEMORY_STATUS_FRAGMENTED,
  VKR_GPU_MEMORY_STATUS_OUT_OF_RANGE_METADATA,
  VKR_GPU_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS,
  VKR_GPU_MEMORY_STATUS_STALE_HANDLE,
  VKR_GPU_MEMORY_STATUS_RING_BUSY,
  VKR_GPU_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED,
} VkrGpuMemoryStatus;

typedef struct VkrGpuAllocationHandle {
  uint32_t index;
  uint32_t generation;
} VkrGpuAllocationHandle;

typedef struct VkrGpuMemoryConfig {
  uint64_t heap_size;
  uint32_t max_allocations;
  uint32_t max_retirements;
  uint32_t max_free_ranges;
} VkrGpuMemoryConfig;

typedef struct VkrGpuPlacement {
  uint64_t reserved_offset;
  uint64_t reserved_size;
  uint64_t resource_offset;
  uint64_t resource_size;
  uint64_t alignment;
  uint32_t kind;
} VkrGpuPlacement;

typedef enum VkrGpuMemoryClass {
  VKR_GPU_MEMORY_CLASS_UNKNOWN = 0,
  VKR_GPU_MEMORY_CLASS_BUFFER,
  VKR_GPU_MEMORY_CLASS_TEXTURE,
  VKR_GPU_MEMORY_CLASS_COUNT,
} VkrGpuMemoryClass;

typedef struct VkrGpuMemoryClassMetrics {
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
} VkrGpuMemoryClassMetrics;

typedef struct VkrGpuMemoryMetrics {
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
  VkrGpuMemoryClassMetrics classes[VKR_GPU_MEMORY_CLASS_COUNT];
} VkrGpuMemoryMetrics;

typedef void (*VkrGpuRetirementReleaseFn)(void *context, uint32_t slot_index,
                                          const VkrGpuPlacement *placement);

uint64_t vkr_gpu_memory_storage_requirement(const VkrGpuMemoryConfig *config);

VkrGpuMemoryStatus vkr_gpu_memory_create(const VkrGpuMemoryConfig *config,
                                         void *storage, uint64_t storage_size,
                                         VkrGpuMemoryCore **out_memory);

VkrGpuMemoryStatus vkr_gpu_memory_allocate(VkrGpuMemoryCore *memory,
                                           uint64_t resource_size,
                                           uint64_t alignment, uint32_t kind,
                                           VkrGpuAllocationHandle *out_handle,
                                           VkrGpuPlacement *out_placement);

VkrGpuMemoryStatus vkr_gpu_memory_resolve(VkrGpuMemoryCore *memory,
                                          VkrGpuAllocationHandle handle,
                                          VkrGpuPlacement *out_placement);

VkrGpuMemoryStatus vkr_gpu_memory_retire(VkrGpuMemoryCore *memory,
                                         VkrGpuAllocationHandle handle,
                                         uint64_t last_use_submit_value);

VkrGpuMemoryStatus vkr_gpu_memory_collect(VkrGpuMemoryCore *memory,
                                          uint64_t completed_submit_value,
                                          VkrGpuRetirementReleaseFn release_fn,
                                          void *release_context,
                                          uint32_t *out_collected_count);

void vkr_gpu_memory_record_native_failure(VkrGpuMemoryCore *memory);

void vkr_gpu_memory_get_metrics(VkrGpuMemoryCore *memory,
                                VkrGpuMemoryMetrics *out_metrics);
void vkr_gpu_memory_metrics_accumulate(VkrGpuMemoryMetrics *total,
                                       const VkrGpuMemoryMetrics *addition);

const char *vkr_gpu_memory_status_string(VkrGpuMemoryStatus status);

#pragma once

#include "defines.h"

typedef enum VkrGpuSubmitRingStatus {
  VKR_GPU_SUBMIT_RING_STATUS_OK = 0,
  VKR_GPU_SUBMIT_RING_STATUS_INVALID_ARGUMENT,
  VKR_GPU_SUBMIT_RING_STATUS_BUSY,
} VkrGpuSubmitRingStatus;

typedef struct VkrGpuSubmitRingSlot {
  uint64_t retire_submit_value;
  uint8_t state;
} VkrGpuSubmitRingSlot;

typedef struct VkrGpuSubmitRing {
  VkrGpuSubmitRingSlot *slots;
  uint64_t total_size;
  uint64_t slot_size;
  uint32_t slot_count;
  uint32_t next_slot;
  uint64_t acquires;
  uint64_t reuses;
  uint64_t busy_failures;
} VkrGpuSubmitRing;

typedef struct VkrGpuRingSlice {
  uint64_t offset;
  uint64_t size;
  uint32_t slot_index;
} VkrGpuRingSlice;

typedef struct VkrGpuAddressPair {
  void *cpu_address;
  uint64_t gpu_address;
  uint64_t size;
} VkrGpuAddressPair;

uint64_t vkr_gpu_submit_ring_storage_requirement(uint32_t slot_count);
VkrGpuSubmitRingStatus vkr_gpu_submit_ring_create(VkrGpuSubmitRing *ring,
                                                  uint64_t total_size,
                                                  uint32_t slot_count,
                                                  void *storage,
                                                  uint64_t storage_size);
VkrGpuSubmitRingStatus
vkr_gpu_submit_ring_acquire(VkrGpuSubmitRing *ring, uint64_t requested_size,
                            uint64_t completed_submit_value,
                            VkrGpuRingSlice *out_slice);
VkrGpuSubmitRingStatus vkr_gpu_submit_ring_submit(VkrGpuSubmitRing *ring,
                                                  VkrGpuRingSlice slice,
                                                  uint64_t submit_value);
void vkr_gpu_submit_ring_cancel(VkrGpuSubmitRing *ring, VkrGpuRingSlice slice);
VkrGpuAddressPair vkr_gpu_address_pair_slice(VkrGpuAddressPair buffer,
                                             VkrGpuRingSlice slice);

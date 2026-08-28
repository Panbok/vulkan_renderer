#include "renderer/metal/vkr_metal_memory.h"

uint64_t
vkr_metal_memory_storage_requirement(const VkrMetalMemoryConfig *config) {
  return vkr_gpu_memory_storage_requirement(config);
}

VkrMetalMemoryStatus vkr_metal_memory_create(const VkrMetalMemoryConfig *config,
                                             void *storage,
                                             uint64_t storage_size,
                                             VkrMetalMemoryCore **out_memory) {
  return (VkrMetalMemoryStatus)vkr_gpu_memory_create(config, storage,
                                                     storage_size, out_memory);
}

VkrMetalMemoryStatus
vkr_metal_memory_allocate(VkrMetalMemoryCore *memory, uint64_t resource_size,
                          uint64_t alignment, uint32_t kind,
                          VkrMetalAllocationHandle *out_handle,
                          VkrMetalPlacement *out_placement) {
  return (VkrMetalMemoryStatus)vkr_gpu_memory_allocate(
      memory, resource_size, alignment, kind, out_handle, out_placement);
}

VkrMetalMemoryStatus
vkr_metal_memory_resolve(VkrMetalMemoryCore *memory,
                         VkrMetalAllocationHandle handle,
                         VkrMetalPlacement *out_placement) {
  return (VkrMetalMemoryStatus)vkr_gpu_memory_resolve(memory, handle,
                                                      out_placement);
}

VkrMetalMemoryStatus vkr_metal_memory_retire(VkrMetalMemoryCore *memory,
                                             VkrMetalAllocationHandle handle,
                                             uint64_t last_use_submit_value) {
  return (VkrMetalMemoryStatus)vkr_gpu_memory_retire(memory, handle,
                                                     last_use_submit_value);
}

VkrMetalMemoryStatus
vkr_metal_memory_collect(VkrMetalMemoryCore *memory,
                         uint64_t completed_submit_value,
                         VkrMetalRetirementReleaseFn release_fn,
                         void *release_context, uint32_t *out_collected_count) {
  return (VkrMetalMemoryStatus)vkr_gpu_memory_collect(
      memory, completed_submit_value, release_fn, release_context,
      out_collected_count);
}

void vkr_metal_memory_record_native_failure(VkrMetalMemoryCore *memory) {
  vkr_gpu_memory_record_native_failure(memory);
}

void vkr_metal_memory_get_metrics(VkrMetalMemoryCore *memory,
                                  VkrMetalMemoryMetrics *out_metrics) {
  vkr_gpu_memory_get_metrics(memory, out_metrics);
}

uint64_t vkr_metal_memory_effective_budget(uint64_t placement_capacity,
                                           uint64_t driver_budget) {
  return driver_budget > 0u && driver_budget < placement_capacity
             ? driver_budget
             : placement_capacity;
}

uint64_t vkr_metal_memory_effective_free_bytes(uint64_t placement_capacity,
                                               uint64_t placement_free_bytes,
                                               uint64_t driver_budget) {
  const uint64_t budget =
      vkr_metal_memory_effective_budget(placement_capacity, driver_budget);
  const uint64_t usage = placement_capacity - placement_free_bytes;
  return usage < budget ? budget - usage : 0u;
}

bool8_t vkr_metal_memory_can_allocate_before_reserve(
    const VkrMetalMemoryMetrics *metrics, uint64_t requested_size,
    uint64_t reserve_size) {
  return metrics && requested_size > 0u &&
         reserve_size <= metrics->free_bytes &&
         requested_size <= metrics->free_bytes - reserve_size &&
         requested_size <= metrics->largest_free_range;
}

uint64_t vkr_metal_submit_ring_storage_requirement(uint32_t slot_count) {
  return vkr_gpu_submit_ring_storage_requirement(slot_count);
}

bool8_t vkr_metal_submit_ring_total_size(uint64_t required_slot_size,
                                         uint64_t minimum_slot_size,
                                         uint32_t slot_count,
                                         uint64_t *out_total_size) {
  if (!required_slot_size || !minimum_slot_size || !slot_count ||
      !out_total_size)
    return false_v;
  const uint64_t slot_size = MAX(required_slot_size, minimum_slot_size);
  if (slot_size > UINT64_MAX / slot_count)
    return false_v;
  *out_total_size = slot_size * slot_count;
  return true_v;
}

VkrMetalMemoryStatus vkr_metal_submit_ring_create(VkrMetalSubmitRing *ring,
                                                  uint64_t total_size,
                                                  uint32_t slot_count,
                                                  void *storage,
                                                  uint64_t storage_size) {
  const VkrGpuSubmitRingStatus status = vkr_gpu_submit_ring_create(
      ring, total_size, slot_count, storage, storage_size);
  return status == VKR_GPU_SUBMIT_RING_STATUS_OK
             ? VKR_METAL_MEMORY_STATUS_OK
             : VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
}

VkrMetalMemoryStatus
vkr_metal_submit_ring_acquire(VkrMetalSubmitRing *ring, uint64_t requested_size,
                              uint64_t completed_submit_value,
                              VkrMetalRingSlice *out_slice) {
  const VkrGpuSubmitRingStatus status = vkr_gpu_submit_ring_acquire(
      ring, requested_size, completed_submit_value, out_slice);
  if (status == VKR_GPU_SUBMIT_RING_STATUS_OK)
    return VKR_METAL_MEMORY_STATUS_OK;
  return status == VKR_GPU_SUBMIT_RING_STATUS_BUSY
             ? VKR_METAL_MEMORY_STATUS_RING_BUSY
             : VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
}

VkrMetalMemoryStatus vkr_metal_submit_ring_submit(VkrMetalSubmitRing *ring,
                                                  VkrMetalRingSlice slice,
                                                  uint64_t submit_value) {
  return vkr_gpu_submit_ring_submit(ring, slice, submit_value) ==
                 VKR_GPU_SUBMIT_RING_STATUS_OK
             ? VKR_METAL_MEMORY_STATUS_OK
             : VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
}

void vkr_metal_submit_ring_cancel(VkrMetalSubmitRing *ring,
                                  VkrMetalRingSlice slice) {
  vkr_gpu_submit_ring_cancel(ring, slice);
}

VkrMetalAddressPair vkr_metal_address_pair_slice(VkrMetalAddressPair buffer,
                                                 VkrMetalRingSlice slice) {
  return vkr_gpu_address_pair_slice(buffer, slice);
}

const char *vkr_metal_memory_status_string(VkrMetalMemoryStatus status) {
  return vkr_gpu_memory_status_string((VkrGpuMemoryStatus)status);
}

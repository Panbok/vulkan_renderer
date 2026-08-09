#include "renderer/vkr_gpu_submit_ring.h"

uint64_t vkr_gpu_submit_ring_storage_requirement(uint32_t slot_count) {
  return (uint64_t)slot_count * sizeof(VkrGpuSubmitRingSlot);
}

VkrGpuSubmitRingStatus vkr_gpu_submit_ring_create(VkrGpuSubmitRing *ring,
                                                  uint64_t total_size,
                                                  uint32_t slot_count,
                                                  void *storage,
                                                  uint64_t storage_size) {
  const uint64_t required = vkr_gpu_submit_ring_storage_requirement(slot_count);
  if (!ring || !storage || !slot_count || total_size < slot_count ||
      storage_size < required)
    return VKR_GPU_SUBMIT_RING_STATUS_INVALID_ARGUMENT;
  MemZero(ring, sizeof(*ring));
  MemZero(storage, required);
  ring->slots = storage;
  ring->total_size = total_size;
  ring->slot_count = slot_count;
  ring->slot_size = total_size / slot_count;
  return VKR_GPU_SUBMIT_RING_STATUS_OK;
}

VkrGpuSubmitRingStatus
vkr_gpu_submit_ring_acquire(VkrGpuSubmitRing *ring, uint64_t requested_size,
                            uint64_t completed_submit_value,
                            VkrGpuRingSlice *out_slice) {
  if (!ring || !out_slice || !requested_size ||
      requested_size > ring->slot_size)
    return VKR_GPU_SUBMIT_RING_STATUS_INVALID_ARGUMENT;
  const uint32_t slot_index = ring->next_slot;
  VkrGpuSubmitRingSlot *slot = &ring->slots[slot_index];
  if (slot->state == 1u || (slot->state == 2u && slot->retire_submit_value >
                                                     completed_submit_value)) {
    ring->busy_failures++;
    return VKR_GPU_SUBMIT_RING_STATUS_BUSY;
  }
  if (slot->state == 2u)
    ring->reuses++;
  slot->state = 1u;
  slot->retire_submit_value = 0u;
  *out_slice = (VkrGpuRingSlice){
      .offset = (uint64_t)slot_index * ring->slot_size,
      .size = requested_size,
      .slot_index = slot_index,
  };
  ring->next_slot = (slot_index + 1u) % ring->slot_count;
  ring->acquires++;
  return VKR_GPU_SUBMIT_RING_STATUS_OK;
}

VkrGpuSubmitRingStatus vkr_gpu_submit_ring_submit(VkrGpuSubmitRing *ring,
                                                  VkrGpuRingSlice slice,
                                                  uint64_t submit_value) {
  if (!ring || slice.slot_index >= ring->slot_count ||
      ring->slots[slice.slot_index].state != 1u)
    return VKR_GPU_SUBMIT_RING_STATUS_INVALID_ARGUMENT;
  ring->slots[slice.slot_index].state = 2u;
  ring->slots[slice.slot_index].retire_submit_value = submit_value;
  return VKR_GPU_SUBMIT_RING_STATUS_OK;
}

void vkr_gpu_submit_ring_cancel(VkrGpuSubmitRing *ring, VkrGpuRingSlice slice) {
  if (!ring || slice.slot_index >= ring->slot_count ||
      ring->slots[slice.slot_index].state != 1u)
    return;
  ring->slots[slice.slot_index] = (VkrGpuSubmitRingSlot){0};
}

VkrGpuAddressPair vkr_gpu_address_pair_slice(VkrGpuAddressPair buffer,
                                             VkrGpuRingSlice slice) {
  if (!buffer.cpu_address || slice.offset > buffer.size ||
      slice.size > buffer.size - slice.offset)
    return (VkrGpuAddressPair){0};
  return (VkrGpuAddressPair){
      .cpu_address = (uint8_t *)buffer.cpu_address + slice.offset,
      .gpu_address = buffer.gpu_address + slice.offset,
      .size = slice.size,
  };
}

#include "renderer/metal/vkr_metal_memory.h"

#include <stddef.h>

typedef struct VkrMetalFreeRange {
  uint64_t offset;
  uint64_t size;
} VkrMetalFreeRange;

typedef enum VkrMetalAllocationState {
  VKR_METAL_ALLOCATION_STATE_FREE = 0,
  VKR_METAL_ALLOCATION_STATE_LIVE,
  VKR_METAL_ALLOCATION_STATE_RETIRED,
} VkrMetalAllocationState;

typedef struct VkrMetalAllocationSlot {
  VkrMetalPlacement placement;
  uint32_t generation;
  VkrMetalAllocationState state;
} VkrMetalAllocationSlot;

typedef struct VkrMetalRetirement {
  uint64_t submit_value;
  uint32_t slot_index;
} VkrMetalRetirement;

struct VkrMetalMemoryCore {
  VkrMetalMemoryConfig config;
  VkrMetalAllocationSlot *slots;
  VkrMetalRetirement *retirements;
  VkrMetalFreeRange *free_ranges;
  uint32_t retirement_count;
  uint32_t free_range_count;
  VkrMetalMemoryMetrics metrics;
};

static bool8_t vkr_metal_is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static bool8_t vkr_metal_add_size(uint64_t *size, uint64_t count,
                                  uint64_t element_size, uint64_t alignment) {
  if (*size > UINT64_MAX - (alignment - 1))
    return false_v;
  *size = AlignPow2(*size, alignment);
  if (count != 0 && element_size > (UINT64_MAX - *size) / count)
    return false_v;
  *size += count * element_size;
  return true_v;
}

uint64_t
vkr_metal_memory_storage_requirement(const VkrMetalMemoryConfig *config) {
  if (!config || config->heap_size == 0 || config->max_allocations == 0 ||
      config->max_retirements == 0 || config->max_free_ranges == 0)
    return 0;

  uint64_t size = _Alignof(VkrMetalMemoryCore) - 1;
  if (!vkr_metal_add_size(&size, 1, sizeof(VkrMetalMemoryCore),
                          _Alignof(VkrMetalMemoryCore)) ||
      !vkr_metal_add_size(&size, config->max_allocations,
                          sizeof(VkrMetalAllocationSlot),
                          _Alignof(VkrMetalAllocationSlot)) ||
      !vkr_metal_add_size(&size, config->max_retirements,
                          sizeof(VkrMetalRetirement),
                          _Alignof(VkrMetalRetirement)) ||
      !vkr_metal_add_size(&size, config->max_free_ranges,
                          sizeof(VkrMetalFreeRange),
                          _Alignof(VkrMetalFreeRange)))
    return 0;
  return size;
}

static void *vkr_metal_take_storage(uint8_t **cursor, uint8_t *end,
                                    uint64_t count, uint64_t element_size,
                                    uint64_t alignment) {
  const uintptr_t aligned = AlignPow2((uintptr_t)*cursor, alignment);
  if (aligned > (uintptr_t)end ||
      count > ((uint64_t)((uintptr_t)end - aligned) / element_size))
    return NULL;
  *cursor = (uint8_t *)(aligned + count * element_size);
  return (void *)aligned;
}

VkrMetalMemoryStatus vkr_metal_memory_create(const VkrMetalMemoryConfig *config,
                                             void *storage,
                                             uint64_t storage_size,
                                             VkrMetalMemoryCore **out_memory) {
  const uint64_t required = vkr_metal_memory_storage_requirement(config);
  if (!out_memory || !storage || required == 0 || storage_size < required)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  *out_memory = NULL;

  uint8_t *cursor = storage;
  uint8_t *end = cursor + storage_size;
  VkrMetalMemoryCore *memory = vkr_metal_take_storage(
      &cursor, end, 1, sizeof(*memory), _Alignof(VkrMetalMemoryCore));
  VkrMetalAllocationSlot *slots =
      vkr_metal_take_storage(&cursor, end, config->max_allocations,
                             sizeof(*slots), _Alignof(VkrMetalAllocationSlot));
  VkrMetalRetirement *retirements = vkr_metal_take_storage(
      &cursor, end, config->max_retirements, sizeof(*retirements),
      _Alignof(VkrMetalRetirement));
  VkrMetalFreeRange *ranges =
      vkr_metal_take_storage(&cursor, end, config->max_free_ranges,
                             sizeof(*ranges), _Alignof(VkrMetalFreeRange));
  if (!memory || !slots || !retirements || !ranges)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;

  MemZero(memory, sizeof(*memory));
  MemZero(slots, sizeof(*slots) * config->max_allocations);
  MemZero(retirements, sizeof(*retirements) * config->max_retirements);
  MemZero(ranges, sizeof(*ranges) * config->max_free_ranges);
  memory->config = *config;
  memory->slots = slots;
  memory->retirements = retirements;
  memory->free_ranges = ranges;
  memory->free_range_count = 1;
  memory->free_ranges[0] = (VkrMetalFreeRange){0, config->heap_size};
  memory->metrics.heap_size = config->heap_size;
  for (uint32_t i = 0; i < config->max_allocations; ++i)
    memory->slots[i].generation = 1;
  *out_memory = memory;
  return VKR_METAL_MEMORY_STATUS_OK;
}

static int32_t vkr_metal_find_free_slot(VkrMetalMemoryCore *memory) {
  for (uint32_t i = 0; i < memory->config.max_allocations; ++i) {
    if (memory->slots[i].state == VKR_METAL_ALLOCATION_STATE_FREE)
      return (int32_t)i;
  }
  return -1;
}

VkrMetalMemoryStatus
vkr_metal_memory_allocate(VkrMetalMemoryCore *memory, uint64_t resource_size,
                          uint64_t alignment, uint32_t kind,
                          VkrMetalAllocationHandle *out_handle,
                          VkrMetalPlacement *out_placement) {
  if (!memory || !out_handle || !out_placement || resource_size == 0 ||
      !vkr_metal_is_power_of_two(alignment) ||
      resource_size > memory->config.heap_size)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;

  const int32_t slot_index = vkr_metal_find_free_slot(memory);
  if (slot_index < 0) {
    memory->metrics.handle_exhaustion_failures++;
    return VKR_METAL_MEMORY_STATUS_OUT_OF_HANDLES;
  }

  uint64_t total_free = 0;
  int32_t range_index = -1;
  uint64_t resource_offset = 0;
  uint64_t reserved_size = 0;
  for (uint32_t i = 0; i < memory->free_range_count; ++i) {
    const VkrMetalFreeRange range = memory->free_ranges[i];
    total_free += range.size;
    if (range_index >= 0 || range.offset > UINT64_MAX - (alignment - 1))
      continue;
    const uint64_t aligned_offset = AlignPow2(range.offset, alignment);
    const uint64_t padding = aligned_offset - range.offset;
    if (padding <= range.size && resource_size <= range.size - padding) {
      range_index = (int32_t)i;
      resource_offset = aligned_offset;
      reserved_size = padding + resource_size;
    }
  }
  if (range_index < 0) {
    if (total_free < resource_size) {
      memory->metrics.byte_exhaustion_failures++;
      return VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES;
    }
    memory->metrics.fragmentation_failures++;
    return VKR_METAL_MEMORY_STATUS_FRAGMENTED;
  }

  VkrMetalFreeRange *range = &memory->free_ranges[range_index];
  const uint64_t reserved_offset = range->offset;
  range->offset += reserved_size;
  range->size -= reserved_size;
  if (range->size == 0) {
    for (uint32_t i = (uint32_t)range_index + 1; i < memory->free_range_count;
         ++i)
      memory->free_ranges[i - 1] = memory->free_ranges[i];
    memory->free_range_count--;
  }

  VkrMetalAllocationSlot *slot = &memory->slots[slot_index];
  slot->state = VKR_METAL_ALLOCATION_STATE_LIVE;
  slot->placement = (VkrMetalPlacement){
      .reserved_offset = reserved_offset,
      .reserved_size = reserved_size,
      .resource_offset = resource_offset,
      .resource_size = resource_size,
      .alignment = alignment,
      .kind = kind,
  };
  *out_handle =
      (VkrMetalAllocationHandle){(uint32_t)slot_index, slot->generation};
  *out_placement = slot->placement;

  VkrMetalMemoryMetrics *metrics = &memory->metrics;
  const uint32_t class_index = kind < VKR_METAL_MEMORY_CLASS_COUNT
                                   ? kind
                                   : VKR_METAL_MEMORY_CLASS_UNKNOWN;
  VkrMetalMemoryClassMetrics *class_metrics = &metrics->classes[class_index];
  metrics->allocations_created++;
  metrics->live_allocations++;
  metrics->live_requested_bytes += resource_size;
  metrics->live_reserved_bytes += reserved_size;
  metrics->alignment_waste_bytes += reserved_size - resource_size;
  class_metrics->allocations_created++;
  class_metrics->live_allocations++;
  class_metrics->live_requested_bytes += resource_size;
  class_metrics->live_reserved_bytes += reserved_size;
  class_metrics->alignment_waste_bytes += reserved_size - resource_size;
  const uint64_t total_allocations =
      metrics->live_allocations + metrics->retired_allocations;
  const uint64_t total_requested =
      metrics->live_requested_bytes + metrics->retired_requested_bytes;
  const uint64_t total_reserved =
      metrics->live_reserved_bytes + metrics->retired_reserved_bytes;
  metrics->peak_allocations = Max(metrics->peak_allocations, total_allocations);
  metrics->peak_requested_bytes =
      Max(metrics->peak_requested_bytes, total_requested);
  metrics->peak_reserved_bytes =
      Max(metrics->peak_reserved_bytes, total_reserved);
  class_metrics->peak_allocations =
      Max(class_metrics->peak_allocations,
          class_metrics->live_allocations + class_metrics->retired_allocations);
  class_metrics->peak_requested_bytes =
      Max(class_metrics->peak_requested_bytes,
          class_metrics->live_requested_bytes +
              class_metrics->retired_requested_bytes);
  class_metrics->peak_reserved_bytes =
      Max(class_metrics->peak_reserved_bytes,
          class_metrics->live_reserved_bytes +
              class_metrics->retired_reserved_bytes);
  return VKR_METAL_MEMORY_STATUS_OK;
}

static bool8_t vkr_metal_handle_is_live(VkrMetalMemoryCore *memory,
                                        VkrMetalAllocationHandle handle) {
  return handle.index < memory->config.max_allocations &&
         memory->slots[handle.index].state == VKR_METAL_ALLOCATION_STATE_LIVE &&
         memory->slots[handle.index].generation == handle.generation;
}

VkrMetalMemoryStatus
vkr_metal_memory_resolve(VkrMetalMemoryCore *memory,
                         VkrMetalAllocationHandle handle,
                         VkrMetalPlacement *out_placement) {
  if (!memory || !out_placement)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  if (!vkr_metal_handle_is_live(memory, handle)) {
    memory->metrics.stale_handle_failures++;
    return VKR_METAL_MEMORY_STATUS_STALE_HANDLE;
  }
  *out_placement = memory->slots[handle.index].placement;
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus vkr_metal_memory_retire(VkrMetalMemoryCore *memory,
                                             VkrMetalAllocationHandle handle,
                                             uint64_t last_use_submit_value) {
  if (!memory)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  if (!vkr_metal_handle_is_live(memory, handle)) {
    memory->metrics.stale_handle_failures++;
    return VKR_METAL_MEMORY_STATUS_STALE_HANDLE;
  }
  if (memory->retirement_count == memory->config.max_retirements) {
    memory->metrics.retirement_capacity_failures++;
    return VKR_METAL_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS;
  }

  VkrMetalAllocationSlot *slot = &memory->slots[handle.index];
  memory->retirements[memory->retirement_count++] =
      (VkrMetalRetirement){last_use_submit_value, handle.index};
  slot->state = VKR_METAL_ALLOCATION_STATE_RETIRED;
  slot->generation++;
  if (slot->generation == 0)
    slot->generation = 1;

  memory->metrics.live_allocations--;
  memory->metrics.retired_allocations++;
  memory->metrics.live_requested_bytes -= slot->placement.resource_size;
  memory->metrics.live_reserved_bytes -= slot->placement.reserved_size;
  memory->metrics.retired_requested_bytes += slot->placement.resource_size;
  memory->metrics.retired_reserved_bytes += slot->placement.reserved_size;
  const uint32_t class_index =
      slot->placement.kind < VKR_METAL_MEMORY_CLASS_COUNT
          ? slot->placement.kind
          : VKR_METAL_MEMORY_CLASS_UNKNOWN;
  VkrMetalMemoryClassMetrics *class_metrics =
      &memory->metrics.classes[class_index];
  class_metrics->live_allocations--;
  class_metrics->retired_allocations++;
  class_metrics->live_requested_bytes -= slot->placement.resource_size;
  class_metrics->live_reserved_bytes -= slot->placement.reserved_size;
  class_metrics->retired_requested_bytes += slot->placement.resource_size;
  class_metrics->retired_reserved_bytes += slot->placement.reserved_size;
  return VKR_METAL_MEMORY_STATUS_OK;
}

static bool8_t vkr_metal_can_free_range(VkrMetalMemoryCore *memory,
                                        VkrMetalFreeRange freed) {
  for (uint32_t i = 0; i < memory->free_range_count; ++i) {
    const VkrMetalFreeRange range = memory->free_ranges[i];
    if (range.offset + range.size == freed.offset ||
        freed.offset + freed.size == range.offset)
      return true_v;
  }
  return memory->free_range_count < memory->config.max_free_ranges;
}

static void vkr_metal_free_range(VkrMetalMemoryCore *memory,
                                 VkrMetalFreeRange freed) {
  uint32_t next = 0;
  while (next < memory->free_range_count &&
         memory->free_ranges[next].offset < freed.offset)
    next++;
  const bool8_t merge_previous =
      next > 0 && memory->free_ranges[next - 1].offset +
                          memory->free_ranges[next - 1].size ==
                      freed.offset;
  const bool8_t merge_next =
      next < memory->free_range_count &&
      freed.offset + freed.size == memory->free_ranges[next].offset;

  if (merge_previous) {
    memory->free_ranges[next - 1].size += freed.size;
    if (merge_next) {
      memory->free_ranges[next - 1].size += memory->free_ranges[next].size;
      for (uint32_t i = next + 1; i < memory->free_range_count; ++i)
        memory->free_ranges[i - 1] = memory->free_ranges[i];
      memory->free_range_count--;
    }
    return;
  }
  if (merge_next) {
    memory->free_ranges[next].offset = freed.offset;
    memory->free_ranges[next].size += freed.size;
    return;
  }
  for (uint32_t i = memory->free_range_count; i > next; --i)
    memory->free_ranges[i] = memory->free_ranges[i - 1];
  memory->free_ranges[next] = freed;
  memory->free_range_count++;
}

VkrMetalMemoryStatus
vkr_metal_memory_collect(VkrMetalMemoryCore *memory,
                         uint64_t completed_submit_value,
                         VkrMetalRetirementReleaseFn release_fn,
                         void *release_context, uint32_t *out_collected_count) {
  if (!memory)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  if (out_collected_count)
    *out_collected_count = 0;

  uint32_t collected = 0;
  for (uint32_t i = 0; i < memory->retirement_count;) {
    const VkrMetalRetirement retirement = memory->retirements[i];
    if (retirement.submit_value > completed_submit_value) {
      i++;
      continue;
    }

    VkrMetalAllocationSlot *slot = &memory->slots[retirement.slot_index];
    const VkrMetalFreeRange freed = {slot->placement.reserved_offset,
                                     slot->placement.reserved_size};
    if (!vkr_metal_can_free_range(memory, freed)) {
      memory->metrics.range_metadata_failures++;
      if (out_collected_count)
        *out_collected_count = collected;
      return VKR_METAL_MEMORY_STATUS_OUT_OF_RANGE_METADATA;
    }

    if (release_fn)
      release_fn(release_context, retirement.slot_index, &slot->placement);
    vkr_metal_free_range(memory, freed);
    memory->metrics.retired_allocations--;
    memory->metrics.retired_requested_bytes -= slot->placement.resource_size;
    memory->metrics.retired_reserved_bytes -= slot->placement.reserved_size;
    memory->metrics.retirements_collected++;
    const uint32_t class_index =
        slot->placement.kind < VKR_METAL_MEMORY_CLASS_COUNT
            ? slot->placement.kind
            : VKR_METAL_MEMORY_CLASS_UNKNOWN;
    VkrMetalMemoryClassMetrics *class_metrics =
        &memory->metrics.classes[class_index];
    class_metrics->retired_allocations--;
    class_metrics->retired_requested_bytes -= slot->placement.resource_size;
    class_metrics->retired_reserved_bytes -= slot->placement.reserved_size;
    slot->placement = (VkrMetalPlacement){0};
    slot->state = VKR_METAL_ALLOCATION_STATE_FREE;
    for (uint32_t j = i + 1; j < memory->retirement_count; ++j)
      memory->retirements[j - 1] = memory->retirements[j];
    memory->retirement_count--;
    collected++;
  }
  if (out_collected_count)
    *out_collected_count = collected;
  return VKR_METAL_MEMORY_STATUS_OK;
}

void vkr_metal_memory_record_native_failure(VkrMetalMemoryCore *memory) {
  if (memory)
    memory->metrics.native_allocation_failures++;
}

void vkr_metal_memory_get_metrics(VkrMetalMemoryCore *memory,
                                  VkrMetalMemoryMetrics *out_metrics) {
  if (!out_metrics)
    return;
  if (!memory) {
    *out_metrics = (VkrMetalMemoryMetrics){0};
    return;
  }
  *out_metrics = memory->metrics;
  out_metrics->free_bytes = 0;
  out_metrics->largest_free_range = 0;
  for (uint32_t i = 0; i < memory->free_range_count; ++i) {
    out_metrics->free_bytes += memory->free_ranges[i].size;
    out_metrics->largest_free_range =
        Max(out_metrics->largest_free_range, memory->free_ranges[i].size);
  }
}

uint64_t vkr_metal_submit_ring_storage_requirement(uint32_t slot_count) {
  return (uint64_t)slot_count * sizeof(VkrMetalSubmitRingSlot);
}

VkrMetalMemoryStatus vkr_metal_submit_ring_create(VkrMetalSubmitRing *ring,
                                                  uint64_t total_size,
                                                  uint32_t slot_count,
                                                  void *storage,
                                                  uint64_t storage_size) {
  const uint64_t required =
      vkr_metal_submit_ring_storage_requirement(slot_count);
  if (!ring || !storage || slot_count == 0 || total_size < slot_count ||
      storage_size < required)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  MemZero(ring, sizeof(*ring));
  MemZero(storage, required);
  ring->slots = storage;
  ring->total_size = total_size;
  ring->slot_count = slot_count;
  ring->slot_size = total_size / slot_count;
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus
vkr_metal_submit_ring_acquire(VkrMetalSubmitRing *ring, uint64_t requested_size,
                              uint64_t completed_submit_value,
                              VkrMetalRingSlice *out_slice) {
  if (!ring || !out_slice || requested_size == 0 ||
      requested_size > ring->slot_size)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  const uint32_t slot_index = ring->next_slot;
  VkrMetalSubmitRingSlot *slot = &ring->slots[slot_index];
  if (slot->state == 1 || (slot->state == 2 && slot->retire_submit_value >
                                                   completed_submit_value)) {
    ring->busy_failures++;
    return VKR_METAL_MEMORY_STATUS_RING_BUSY;
  }
  if (slot->state == 2)
    ring->reuses++;
  slot->state = 1;
  slot->retire_submit_value = 0;
  *out_slice = (VkrMetalRingSlice){
      .offset = (uint64_t)slot_index * ring->slot_size,
      .size = requested_size,
      .slot_index = slot_index,
  };
  ring->next_slot = (slot_index + 1) % ring->slot_count;
  ring->acquires++;
  return VKR_METAL_MEMORY_STATUS_OK;
}

VkrMetalMemoryStatus vkr_metal_submit_ring_submit(VkrMetalSubmitRing *ring,
                                                  VkrMetalRingSlice slice,
                                                  uint64_t submit_value) {
  if (!ring || slice.slot_index >= ring->slot_count ||
      ring->slots[slice.slot_index].state != 1)
    return VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT;
  ring->slots[slice.slot_index].state = 2;
  ring->slots[slice.slot_index].retire_submit_value = submit_value;
  return VKR_METAL_MEMORY_STATUS_OK;
}

void vkr_metal_submit_ring_cancel(VkrMetalSubmitRing *ring,
                                  VkrMetalRingSlice slice) {
  if (!ring || slice.slot_index >= ring->slot_count ||
      ring->slots[slice.slot_index].state != 1)
    return;
  ring->slots[slice.slot_index] = (VkrMetalSubmitRingSlot){0};
}

VkrMetalAddressPair vkr_metal_address_pair_slice(VkrMetalAddressPair buffer,
                                                 VkrMetalRingSlice slice) {
  if (!buffer.cpu_address || slice.offset > buffer.size ||
      slice.size > buffer.size - slice.offset)
    return (VkrMetalAddressPair){0};
  return (VkrMetalAddressPair){
      .cpu_address = (uint8_t *)buffer.cpu_address + slice.offset,
      .gpu_address = buffer.gpu_address + slice.offset,
      .size = slice.size,
  };
}

const char *vkr_metal_memory_status_string(VkrMetalMemoryStatus status) {
  switch (status) {
  case VKR_METAL_MEMORY_STATUS_OK:
    return "ok";
  case VKR_METAL_MEMORY_STATUS_INVALID_ARGUMENT:
    return "invalid argument";
  case VKR_METAL_MEMORY_STATUS_OUT_OF_HANDLES:
    return "out of handles";
  case VKR_METAL_MEMORY_STATUS_OUT_OF_BYTES:
    return "out of bytes";
  case VKR_METAL_MEMORY_STATUS_FRAGMENTED:
    return "fragmented";
  case VKR_METAL_MEMORY_STATUS_OUT_OF_RANGE_METADATA:
    return "out of range metadata";
  case VKR_METAL_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS:
    return "out of retirement records";
  case VKR_METAL_MEMORY_STATUS_STALE_HANDLE:
    return "stale handle";
  case VKR_METAL_MEMORY_STATUS_RING_BUSY:
    return "ring busy";
  case VKR_METAL_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED:
    return "native allocation failed";
  }
  return "unknown";
}

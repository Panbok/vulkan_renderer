#include "renderer/vkr_gpu_memory.h"

#include <stddef.h>

typedef struct VkrGpuFreeRange {
  uint64_t offset;
  uint64_t size;
} VkrGpuFreeRange;

typedef enum VkrGpuAllocationState {
  VKR_GPU_ALLOCATION_STATE_FREE = 0,
  VKR_GPU_ALLOCATION_STATE_LIVE,
  VKR_GPU_ALLOCATION_STATE_RETIRED,
} VkrGpuAllocationState;

typedef struct VkrGpuAllocationSlot {
  VkrGpuPlacement placement;
  uint32_t generation;
  VkrGpuAllocationState state;
} VkrGpuAllocationSlot;

typedef struct VkrGpuRetirement {
  uint64_t submit_value;
  uint32_t slot_index;
} VkrGpuRetirement;

struct VkrGpuMemoryCore {
  VkrGpuMemoryConfig config;
  VkrGpuAllocationSlot *slots;
  VkrGpuRetirement *retirements;
  VkrGpuFreeRange *free_ranges;
  uint32_t retirement_count;
  uint32_t free_range_count;
  VkrGpuMemoryMetrics metrics;
};

static bool8_t vkr_gpu_is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static bool8_t vkr_gpu_add_size(uint64_t *size, uint64_t count,
                                uint64_t element_size, uint64_t alignment) {
  if (*size > UINT64_MAX - (alignment - 1))
    return false_v;
  *size = AlignPow2(*size, alignment);
  if (count != 0 && element_size > (UINT64_MAX - *size) / count)
    return false_v;
  *size += count * element_size;
  return true_v;
}

uint64_t vkr_gpu_memory_storage_requirement(const VkrGpuMemoryConfig *config) {
  if (!config || config->heap_size == 0 || config->max_allocations == 0 ||
      config->max_retirements == 0 || config->max_free_ranges == 0)
    return 0;

  uint64_t size = _Alignof(VkrGpuMemoryCore) - 1;
  if (!vkr_gpu_add_size(&size, 1, sizeof(VkrGpuMemoryCore),
                        _Alignof(VkrGpuMemoryCore)) ||
      !vkr_gpu_add_size(&size, config->max_allocations,
                        sizeof(VkrGpuAllocationSlot),
                        _Alignof(VkrGpuAllocationSlot)) ||
      !vkr_gpu_add_size(&size, config->max_retirements,
                        sizeof(VkrGpuRetirement), _Alignof(VkrGpuRetirement)) ||
      !vkr_gpu_add_size(&size, config->max_free_ranges, sizeof(VkrGpuFreeRange),
                        _Alignof(VkrGpuFreeRange)))
    return 0;
  return size;
}

static void *vkr_gpu_take_storage(uint8_t **cursor, uint8_t *end,
                                  uint64_t count, uint64_t element_size,
                                  uint64_t alignment) {
  const uintptr_t aligned = AlignPow2((uintptr_t)*cursor, alignment);
  if (aligned > (uintptr_t)end ||
      count > ((uint64_t)((uintptr_t)end - aligned) / element_size))
    return NULL;
  *cursor = (uint8_t *)(aligned + count * element_size);
  return (void *)aligned;
}

VkrGpuMemoryStatus vkr_gpu_memory_create(const VkrGpuMemoryConfig *config,
                                         void *storage, uint64_t storage_size,
                                         VkrGpuMemoryCore **out_memory) {
  const uint64_t required = vkr_gpu_memory_storage_requirement(config);
  if (!out_memory || !storage || required == 0 || storage_size < required)
    return VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT;
  *out_memory = NULL;

  uint8_t *cursor = storage;
  uint8_t *end = cursor + storage_size;
  VkrGpuMemoryCore *memory = vkr_gpu_take_storage(
      &cursor, end, 1, sizeof(*memory), _Alignof(VkrGpuMemoryCore));
  VkrGpuAllocationSlot *slots =
      vkr_gpu_take_storage(&cursor, end, config->max_allocations,
                           sizeof(*slots), _Alignof(VkrGpuAllocationSlot));
  VkrGpuRetirement *retirements =
      vkr_gpu_take_storage(&cursor, end, config->max_retirements,
                           sizeof(*retirements), _Alignof(VkrGpuRetirement));
  VkrGpuFreeRange *ranges =
      vkr_gpu_take_storage(&cursor, end, config->max_free_ranges,
                           sizeof(*ranges), _Alignof(VkrGpuFreeRange));
  if (!memory || !slots || !retirements || !ranges)
    return VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT;

  MemZero(memory, sizeof(*memory));
  MemZero(slots, sizeof(*slots) * config->max_allocations);
  MemZero(retirements, sizeof(*retirements) * config->max_retirements);
  MemZero(ranges, sizeof(*ranges) * config->max_free_ranges);
  memory->config = *config;
  memory->slots = slots;
  memory->retirements = retirements;
  memory->free_ranges = ranges;
  memory->free_range_count = 1;
  memory->free_ranges[0] = (VkrGpuFreeRange){0, config->heap_size};
  memory->metrics.heap_size = config->heap_size;
  for (uint32_t i = 0; i < config->max_allocations; ++i)
    memory->slots[i].generation = 1;
  *out_memory = memory;
  return VKR_GPU_MEMORY_STATUS_OK;
}

static int32_t vkr_gpu_find_free_slot(VkrGpuMemoryCore *memory) {
  for (uint32_t i = 0; i < memory->config.max_allocations; ++i) {
    if (memory->slots[i].state == VKR_GPU_ALLOCATION_STATE_FREE)
      return (int32_t)i;
  }
  return -1;
}

VkrGpuMemoryStatus vkr_gpu_memory_allocate(VkrGpuMemoryCore *memory,
                                           uint64_t resource_size,
                                           uint64_t alignment, uint32_t kind,
                                           VkrGpuAllocationHandle *out_handle,
                                           VkrGpuPlacement *out_placement) {
  if (!memory || !out_handle || !out_placement || resource_size == 0 ||
      !vkr_gpu_is_power_of_two(alignment) ||
      resource_size > memory->config.heap_size)
    return VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT;

  const int32_t slot_index = vkr_gpu_find_free_slot(memory);
  if (slot_index < 0) {
    memory->metrics.handle_exhaustion_failures++;
    return VKR_GPU_MEMORY_STATUS_OUT_OF_HANDLES;
  }

  uint64_t total_free = 0;
  int32_t range_index = -1;
  uint64_t resource_offset = 0;
  uint64_t reserved_size = 0;
  for (uint32_t i = 0; i < memory->free_range_count; ++i) {
    const VkrGpuFreeRange range = memory->free_ranges[i];
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
      return VKR_GPU_MEMORY_STATUS_OUT_OF_BYTES;
    }
    memory->metrics.fragmentation_failures++;
    return VKR_GPU_MEMORY_STATUS_FRAGMENTED;
  }

  VkrGpuFreeRange *range = &memory->free_ranges[range_index];
  const uint64_t reserved_offset = range->offset;
  range->offset += reserved_size;
  range->size -= reserved_size;
  if (range->size == 0) {
    for (uint32_t i = (uint32_t)range_index + 1; i < memory->free_range_count;
         ++i)
      memory->free_ranges[i - 1] = memory->free_ranges[i];
    memory->free_range_count--;
  }

  VkrGpuAllocationSlot *slot = &memory->slots[slot_index];
  slot->state = VKR_GPU_ALLOCATION_STATE_LIVE;
  slot->placement = (VkrGpuPlacement){
      .reserved_offset = reserved_offset,
      .reserved_size = reserved_size,
      .resource_offset = resource_offset,
      .resource_size = resource_size,
      .alignment = alignment,
      .kind = kind,
  };
  *out_handle =
      (VkrGpuAllocationHandle){(uint32_t)slot_index, slot->generation};
  *out_placement = slot->placement;

  VkrGpuMemoryMetrics *metrics = &memory->metrics;
  const uint32_t class_index =
      kind < VKR_GPU_MEMORY_CLASS_COUNT ? kind : VKR_GPU_MEMORY_CLASS_UNKNOWN;
  VkrGpuMemoryClassMetrics *class_metrics = &metrics->classes[class_index];
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
  return VKR_GPU_MEMORY_STATUS_OK;
}

static bool8_t vkr_gpu_handle_is_live(VkrGpuMemoryCore *memory,
                                      VkrGpuAllocationHandle handle) {
  return handle.index < memory->config.max_allocations &&
         memory->slots[handle.index].state == VKR_GPU_ALLOCATION_STATE_LIVE &&
         memory->slots[handle.index].generation == handle.generation;
}

VkrGpuMemoryStatus vkr_gpu_memory_resolve(VkrGpuMemoryCore *memory,
                                          VkrGpuAllocationHandle handle,
                                          VkrGpuPlacement *out_placement) {
  if (!memory || !out_placement)
    return VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT;
  if (!vkr_gpu_handle_is_live(memory, handle)) {
    memory->metrics.stale_handle_failures++;
    return VKR_GPU_MEMORY_STATUS_STALE_HANDLE;
  }
  *out_placement = memory->slots[handle.index].placement;
  return VKR_GPU_MEMORY_STATUS_OK;
}

VkrGpuMemoryStatus vkr_gpu_memory_retire(VkrGpuMemoryCore *memory,
                                         VkrGpuAllocationHandle handle,
                                         uint64_t last_use_submit_value) {
  if (!memory)
    return VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT;
  if (!vkr_gpu_handle_is_live(memory, handle)) {
    memory->metrics.stale_handle_failures++;
    return VKR_GPU_MEMORY_STATUS_STALE_HANDLE;
  }
  if (memory->retirement_count == memory->config.max_retirements) {
    memory->metrics.retirement_capacity_failures++;
    return VKR_GPU_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS;
  }

  VkrGpuAllocationSlot *slot = &memory->slots[handle.index];
  memory->retirements[memory->retirement_count++] =
      (VkrGpuRetirement){last_use_submit_value, handle.index};
  slot->state = VKR_GPU_ALLOCATION_STATE_RETIRED;
  slot->generation++;
  if (slot->generation == 0)
    slot->generation = 1;

  memory->metrics.live_allocations--;
  memory->metrics.retired_allocations++;
  memory->metrics.live_requested_bytes -= slot->placement.resource_size;
  memory->metrics.live_reserved_bytes -= slot->placement.reserved_size;
  memory->metrics.retired_requested_bytes += slot->placement.resource_size;
  memory->metrics.retired_reserved_bytes += slot->placement.reserved_size;
  const uint32_t class_index = slot->placement.kind < VKR_GPU_MEMORY_CLASS_COUNT
                                   ? slot->placement.kind
                                   : VKR_GPU_MEMORY_CLASS_UNKNOWN;
  VkrGpuMemoryClassMetrics *class_metrics =
      &memory->metrics.classes[class_index];
  class_metrics->live_allocations--;
  class_metrics->retired_allocations++;
  class_metrics->live_requested_bytes -= slot->placement.resource_size;
  class_metrics->live_reserved_bytes -= slot->placement.reserved_size;
  class_metrics->retired_requested_bytes += slot->placement.resource_size;
  class_metrics->retired_reserved_bytes += slot->placement.reserved_size;
  return VKR_GPU_MEMORY_STATUS_OK;
}

static bool8_t vkr_gpu_can_free_range(VkrGpuMemoryCore *memory,
                                      VkrGpuFreeRange freed) {
  for (uint32_t i = 0; i < memory->free_range_count; ++i) {
    const VkrGpuFreeRange range = memory->free_ranges[i];
    if (range.offset + range.size == freed.offset ||
        freed.offset + freed.size == range.offset)
      return true_v;
  }
  return memory->free_range_count < memory->config.max_free_ranges;
}

static void vkr_gpu_free_range(VkrGpuMemoryCore *memory,
                               VkrGpuFreeRange freed) {
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

VkrGpuMemoryStatus vkr_gpu_memory_collect(VkrGpuMemoryCore *memory,
                                          uint64_t completed_submit_value,
                                          VkrGpuRetirementReleaseFn release_fn,
                                          void *release_context,
                                          uint32_t *out_collected_count) {
  if (!memory)
    return VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT;
  if (out_collected_count)
    *out_collected_count = 0;

  uint32_t collected = 0;
  for (uint32_t i = 0; i < memory->retirement_count;) {
    const VkrGpuRetirement retirement = memory->retirements[i];
    if (retirement.submit_value > completed_submit_value) {
      i++;
      continue;
    }

    VkrGpuAllocationSlot *slot = &memory->slots[retirement.slot_index];
    const VkrGpuFreeRange freed = {slot->placement.reserved_offset,
                                   slot->placement.reserved_size};
    if (!vkr_gpu_can_free_range(memory, freed)) {
      memory->metrics.range_metadata_failures++;
      if (out_collected_count)
        *out_collected_count = collected;
      return VKR_GPU_MEMORY_STATUS_OUT_OF_RANGE_METADATA;
    }

    if (release_fn)
      release_fn(release_context, retirement.slot_index, &slot->placement);
    vkr_gpu_free_range(memory, freed);
    memory->metrics.retired_allocations--;
    memory->metrics.retired_requested_bytes -= slot->placement.resource_size;
    memory->metrics.retired_reserved_bytes -= slot->placement.reserved_size;
    memory->metrics.retirements_collected++;
    const uint32_t class_index =
        slot->placement.kind < VKR_GPU_MEMORY_CLASS_COUNT
            ? slot->placement.kind
            : VKR_GPU_MEMORY_CLASS_UNKNOWN;
    VkrGpuMemoryClassMetrics *class_metrics =
        &memory->metrics.classes[class_index];
    class_metrics->retired_allocations--;
    class_metrics->retired_requested_bytes -= slot->placement.resource_size;
    class_metrics->retired_reserved_bytes -= slot->placement.reserved_size;
    slot->placement = (VkrGpuPlacement){0};
    slot->state = VKR_GPU_ALLOCATION_STATE_FREE;
    for (uint32_t j = i + 1; j < memory->retirement_count; ++j)
      memory->retirements[j - 1] = memory->retirements[j];
    memory->retirement_count--;
    collected++;
  }
  if (out_collected_count)
    *out_collected_count = collected;
  return VKR_GPU_MEMORY_STATUS_OK;
}

void vkr_gpu_memory_record_native_failure(VkrGpuMemoryCore *memory) {
  if (memory)
    memory->metrics.native_allocation_failures++;
}

void vkr_gpu_memory_get_metrics(VkrGpuMemoryCore *memory,
                                VkrGpuMemoryMetrics *out_metrics) {
  if (!out_metrics)
    return;
  if (!memory) {
    *out_metrics = (VkrGpuMemoryMetrics){0};
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

void vkr_gpu_memory_metrics_accumulate(VkrGpuMemoryMetrics *total,
                                       const VkrGpuMemoryMetrics *addition) {
  if (!total || !addition)
    return;
#define VKR_GPU_MEMORY_ADD(FIELD) total->FIELD += addition->FIELD
  VKR_GPU_MEMORY_ADD(heap_size);
  VKR_GPU_MEMORY_ADD(free_bytes);
  total->largest_free_range =
      Max(total->largest_free_range, addition->largest_free_range);
  VKR_GPU_MEMORY_ADD(live_requested_bytes);
  VKR_GPU_MEMORY_ADD(live_reserved_bytes);
  VKR_GPU_MEMORY_ADD(retired_requested_bytes);
  VKR_GPU_MEMORY_ADD(retired_reserved_bytes);
  VKR_GPU_MEMORY_ADD(peak_requested_bytes);
  VKR_GPU_MEMORY_ADD(peak_reserved_bytes);
  VKR_GPU_MEMORY_ADD(allocations_created);
  VKR_GPU_MEMORY_ADD(retirements_collected);
  VKR_GPU_MEMORY_ADD(live_allocations);
  VKR_GPU_MEMORY_ADD(retired_allocations);
  VKR_GPU_MEMORY_ADD(peak_allocations);
  VKR_GPU_MEMORY_ADD(alignment_waste_bytes);
  VKR_GPU_MEMORY_ADD(byte_exhaustion_failures);
  VKR_GPU_MEMORY_ADD(fragmentation_failures);
  VKR_GPU_MEMORY_ADD(handle_exhaustion_failures);
  VKR_GPU_MEMORY_ADD(range_metadata_failures);
  VKR_GPU_MEMORY_ADD(retirement_capacity_failures);
  VKR_GPU_MEMORY_ADD(stale_handle_failures);
  VKR_GPU_MEMORY_ADD(native_allocation_failures);
  for (uint32_t class_index = 0; class_index < VKR_GPU_MEMORY_CLASS_COUNT;
       ++class_index) {
    VkrGpuMemoryClassMetrics *target = &total->classes[class_index];
    const VkrGpuMemoryClassMetrics *source = &addition->classes[class_index];
    target->live_requested_bytes += source->live_requested_bytes;
    target->live_reserved_bytes += source->live_reserved_bytes;
    target->retired_requested_bytes += source->retired_requested_bytes;
    target->retired_reserved_bytes += source->retired_reserved_bytes;
    target->peak_requested_bytes += source->peak_requested_bytes;
    target->peak_reserved_bytes += source->peak_reserved_bytes;
    target->allocations_created += source->allocations_created;
    target->live_allocations += source->live_allocations;
    target->retired_allocations += source->retired_allocations;
    target->peak_allocations += source->peak_allocations;
    target->alignment_waste_bytes += source->alignment_waste_bytes;
  }
#undef VKR_GPU_MEMORY_ADD
}

const char *vkr_gpu_memory_status_string(VkrGpuMemoryStatus status) {
  switch (status) {
  case VKR_GPU_MEMORY_STATUS_OK:
    return "ok";
  case VKR_GPU_MEMORY_STATUS_INVALID_ARGUMENT:
    return "invalid argument";
  case VKR_GPU_MEMORY_STATUS_OUT_OF_HANDLES:
    return "out of handles";
  case VKR_GPU_MEMORY_STATUS_OUT_OF_BYTES:
    return "out of bytes";
  case VKR_GPU_MEMORY_STATUS_FRAGMENTED:
    return "fragmented";
  case VKR_GPU_MEMORY_STATUS_OUT_OF_RANGE_METADATA:
    return "out of range metadata";
  case VKR_GPU_MEMORY_STATUS_OUT_OF_RETIREMENT_RECORDS:
    return "out of retirement records";
  case VKR_GPU_MEMORY_STATUS_STALE_HANDLE:
    return "stale handle";
  case VKR_GPU_MEMORY_STATUS_RING_BUSY:
    return "ring busy";
  case VKR_GPU_MEMORY_STATUS_NATIVE_ALLOCATION_FAILED:
    return "native allocation failed";
  }
  return "unknown";
}

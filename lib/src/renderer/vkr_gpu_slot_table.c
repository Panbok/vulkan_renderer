#include "renderer/vkr_gpu_slot_table.h"

#include <stdatomic.h>

typedef enum VkrGpuSlotState {
  VKR_GPU_SLOT_FREE = 0,
  VKR_GPU_SLOT_LIVE,
  VKR_GPU_SLOT_RETIRED,
} VkrGpuSlotState;

typedef struct VkrGpuSlot {
  uint32_t generation;
  VkrGpuSlotState state;
} VkrGpuSlot;

typedef struct VkrGpuSlotRetirement {
  uint64_t submit_value;
  uint32_t slot_index;
} VkrGpuSlotRetirement;

struct VkrGpuSlotTable {
  VkrGpuSlotTableConfig config;
  uint8_t *mapped_rows;
  VkrGpuSlot *slots;
  VkrGpuSlotRetirement *retirements;
  uint32_t retirement_count;
  VkrGpuSlotTableMetrics metrics;
};

static bool8_t vkr_gpu_slot_add_storage(uint64_t *size, uint64_t count,
                                        uint64_t element_size,
                                        uint64_t alignment) {
  if (*size > UINT64_MAX - (alignment - 1u))
    return false_v;
  *size = AlignPow2(*size, alignment);
  if (count && element_size > (UINT64_MAX - *size) / count)
    return false_v;
  *size += count * element_size;
  return true_v;
}

uint64_t
vkr_gpu_slot_table_storage_requirement(const VkrGpuSlotTableConfig *config) {
  if (!config || !config->max_slots || !config->max_retirements ||
      !config->row_size)
    return 0;
  uint64_t size = _Alignof(VkrGpuSlotTable) - 1u;
  if (!vkr_gpu_slot_add_storage(&size, 1u, sizeof(VkrGpuSlotTable),
                                _Alignof(VkrGpuSlotTable)) ||
      !vkr_gpu_slot_add_storage(&size, config->max_slots, sizeof(VkrGpuSlot),
                                _Alignof(VkrGpuSlot)) ||
      !vkr_gpu_slot_add_storage(&size, config->max_retirements,
                                sizeof(VkrGpuSlotRetirement),
                                _Alignof(VkrGpuSlotRetirement)))
    return 0;
  return size;
}

static void *vkr_gpu_slot_take_storage(uint8_t **cursor, uint8_t *end,
                                       uint64_t count, uint64_t element_size,
                                       uint64_t alignment) {
  const uintptr_t aligned = AlignPow2((uintptr_t)*cursor, alignment);
  if (aligned > (uintptr_t)end ||
      count > (uint64_t)((uintptr_t)end - aligned) / element_size)
    return NULL;
  *cursor = (uint8_t *)(aligned + count * element_size);
  return (void *)aligned;
}

VkrGpuSlotStatus vkr_gpu_slot_table_create(const VkrGpuSlotTableConfig *config,
                                           void *storage, uint64_t storage_size,
                                           void *mapped_rows,
                                           VkrGpuSlotTable **out_table) {
  const uint64_t required = vkr_gpu_slot_table_storage_requirement(config);
  if (!storage || !mapped_rows || !out_table || !required ||
      storage_size < required)
    return VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT;
  *out_table = NULL;

  uint8_t *cursor = storage;
  uint8_t *end = cursor + storage_size;
  VkrGpuSlotTable *table = vkr_gpu_slot_take_storage(
      &cursor, end, 1u, sizeof(*table), _Alignof(VkrGpuSlotTable));
  VkrGpuSlot *slots = vkr_gpu_slot_take_storage(
      &cursor, end, config->max_slots, sizeof(*slots), _Alignof(VkrGpuSlot));
  VkrGpuSlotRetirement *retirements = vkr_gpu_slot_take_storage(
      &cursor, end, config->max_retirements, sizeof(*retirements),
      _Alignof(VkrGpuSlotRetirement));
  if (!table || !slots || !retirements)
    return VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT;

  MemZero(table, sizeof(*table));
  MemZero(slots, sizeof(*slots) * config->max_slots);
  MemZero(retirements, sizeof(*retirements) * config->max_retirements);
  MemZero(mapped_rows, (uint64_t)config->row_size * config->max_slots);
  table->config = *config;
  table->mapped_rows = mapped_rows;
  table->slots = slots;
  table->retirements = retirements;
  for (uint32_t i = 0; i < config->max_slots; ++i)
    slots[i].generation = 1u;
  *out_table = table;
  return VKR_GPU_SLOT_STATUS_OK;
}

static int32_t vkr_gpu_slot_find_free(VkrGpuSlotTable *table) {
  for (uint32_t i = 0; i < table->config.max_slots; ++i) {
    if (table->slots[i].state == VKR_GPU_SLOT_FREE)
      return (int32_t)i;
  }
  return -1;
}

static bool8_t vkr_gpu_slot_handle_is_live(const VkrGpuSlotTable *table,
                                           VkrGpuSlotHandle handle) {
  return handle.index < table->config.max_slots &&
         table->slots[handle.index].state == VKR_GPU_SLOT_LIVE &&
         table->slots[handle.index].generation == handle.generation;
}

VkrGpuSlotStatus vkr_gpu_slot_table_can_retire(const VkrGpuSlotTable *table,
                                               VkrGpuSlotHandle handle) {
  if (!table)
    return VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT;
  if (!vkr_gpu_slot_handle_is_live(table, handle))
    return VKR_GPU_SLOT_STATUS_STALE_HANDLE;
  if (table->retirement_count == table->config.max_retirements)
    return VKR_GPU_SLOT_STATUS_RETIREMENT_CAPACITY_EXHAUSTED;
  return VKR_GPU_SLOT_STATUS_OK;
}

static void vkr_gpu_slot_publish_at(VkrGpuSlotTable *table, uint32_t slot_index,
                                    const void *row,
                                    VkrGpuSlotHandle *out_handle) {
  MemCopy(table->mapped_rows + (uint64_t)slot_index * table->config.row_size,
          row, table->config.row_size);
  atomic_thread_fence(memory_order_release);
  table->slots[slot_index].state = VKR_GPU_SLOT_LIVE;
  *out_handle =
      (VkrGpuSlotHandle){slot_index, table->slots[slot_index].generation};
  table->metrics.slots_live++;
  table->metrics.slots_published++;
  table->metrics.slots_peak =
      Max(table->metrics.slots_peak,
          table->metrics.slots_live + table->metrics.slots_retired);
}

VkrGpuSlotStatus vkr_gpu_slot_table_publish(VkrGpuSlotTable *table,
                                            const void *row,
                                            VkrGpuSlotHandle *out_handle) {
  if (!table || !row || !out_handle)
    return VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT;
  const int32_t index = vkr_gpu_slot_find_free(table);
  if (index < 0) {
    table->metrics.capacity_failures++;
    return VKR_GPU_SLOT_STATUS_CAPACITY_EXHAUSTED;
  }
  vkr_gpu_slot_publish_at(table, (uint32_t)index, row, out_handle);
  return VKR_GPU_SLOT_STATUS_OK;
}

static void vkr_gpu_slot_retire_live(VkrGpuSlotTable *table,
                                     VkrGpuSlotHandle handle,
                                     uint64_t submit_value) {
  VkrGpuSlot *slot = &table->slots[handle.index];
  table->retirements[table->retirement_count++] =
      (VkrGpuSlotRetirement){submit_value, handle.index};
  slot->state = VKR_GPU_SLOT_RETIRED;
  if (++slot->generation == 0u)
    slot->generation = 1u;
  table->metrics.slots_live--;
  table->metrics.slots_retired++;
  table->metrics.slots_retirements++;
}

VkrGpuSlotStatus vkr_gpu_slot_table_replace(VkrGpuSlotTable *table,
                                            VkrGpuSlotHandle old_handle,
                                            const void *new_row,
                                            uint64_t old_last_use_submit_value,
                                            VkrGpuSlotHandle *out_new_handle) {
  if (!table || !new_row || !out_new_handle)
    return VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT;
  if (!vkr_gpu_slot_handle_is_live(table, old_handle)) {
    table->metrics.stale_handle_failures++;
    return VKR_GPU_SLOT_STATUS_STALE_HANDLE;
  }
  if (table->retirement_count == table->config.max_retirements) {
    table->metrics.retirement_capacity_failures++;
    return VKR_GPU_SLOT_STATUS_RETIREMENT_CAPACITY_EXHAUSTED;
  }
  const int32_t index = vkr_gpu_slot_find_free(table);
  if (index < 0) {
    table->metrics.capacity_failures++;
    return VKR_GPU_SLOT_STATUS_CAPACITY_EXHAUSTED;
  }
  vkr_gpu_slot_publish_at(table, (uint32_t)index, new_row, out_new_handle);
  vkr_gpu_slot_retire_live(table, old_handle, old_last_use_submit_value);
  table->metrics.slots_replaced++;
  return VKR_GPU_SLOT_STATUS_OK;
}

VkrGpuSlotStatus vkr_gpu_slot_table_retire(VkrGpuSlotTable *table,
                                           VkrGpuSlotHandle handle,
                                           uint64_t last_use_submit_value) {
  const VkrGpuSlotStatus status = vkr_gpu_slot_table_can_retire(table, handle);
  if (status == VKR_GPU_SLOT_STATUS_STALE_HANDLE) {
    table->metrics.stale_handle_failures++;
    return status;
  }
  if (status == VKR_GPU_SLOT_STATUS_RETIREMENT_CAPACITY_EXHAUSTED) {
    table->metrics.retirement_capacity_failures++;
    return status;
  }
  if (status != VKR_GPU_SLOT_STATUS_OK)
    return status;
  vkr_gpu_slot_retire_live(table, handle, last_use_submit_value);
  return VKR_GPU_SLOT_STATUS_OK;
}

VkrGpuSlotStatus vkr_gpu_slot_table_resolve(VkrGpuSlotTable *table,
                                            VkrGpuSlotHandle handle,
                                            uint32_t *out_slot_index) {
  if (!table || !out_slot_index)
    return VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT;
  if (!vkr_gpu_slot_handle_is_live(table, handle)) {
    table->metrics.stale_handle_failures++;
    return VKR_GPU_SLOT_STATUS_STALE_HANDLE;
  }
  atomic_thread_fence(memory_order_acquire);
  *out_slot_index = handle.index;
  return VKR_GPU_SLOT_STATUS_OK;
}

VkrGpuSlotStatus vkr_gpu_slot_table_collect(VkrGpuSlotTable *table,
                                            uint64_t completed_submit_value,
                                            uint32_t *out_collected_count) {
  if (!table)
    return VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT;
  uint32_t collected = 0;
  for (uint32_t i = 0; i < table->retirement_count;) {
    const VkrGpuSlotRetirement retirement = table->retirements[i];
    if (retirement.submit_value > completed_submit_value) {
      ++i;
      continue;
    }
    MemZero(table->mapped_rows +
                (uint64_t)retirement.slot_index * table->config.row_size,
            table->config.row_size);
    table->slots[retirement.slot_index].state = VKR_GPU_SLOT_FREE;
    table->metrics.slots_retired--;
    table->metrics.slots_collected++;
    for (uint32_t j = i + 1u; j < table->retirement_count; ++j)
      table->retirements[j - 1u] = table->retirements[j];
    table->retirement_count--;
    collected++;
  }
  if (out_collected_count)
    *out_collected_count = collected;
  return VKR_GPU_SLOT_STATUS_OK;
}

void vkr_gpu_slot_table_get_metrics(const VkrGpuSlotTable *table,
                                    VkrGpuSlotTableMetrics *out_metrics) {
  if (out_metrics) {
    *out_metrics = table ? table->metrics : (VkrGpuSlotTableMetrics){0};
    if (table)
      out_metrics->slots_capacity = table->config.max_slots;
  }
}

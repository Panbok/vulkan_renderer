#include "renderer/metal/vkr_metal_material_table.h"

#include <stdatomic.h>

typedef enum VkrMetalMaterialRowState {
  VKR_METAL_MATERIAL_ROW_FREE = 0,
  VKR_METAL_MATERIAL_ROW_LIVE,
  VKR_METAL_MATERIAL_ROW_RETIRED,
} VkrMetalMaterialRowState;

typedef struct VkrMetalMaterialSlot {
  uint32_t generation;
  VkrMetalMaterialRowState state;
} VkrMetalMaterialSlot;

typedef struct VkrMetalMaterialRetirement {
  uint64_t submit_value;
  uint32_t row_index;
} VkrMetalMaterialRetirement;

struct VkrMetalMaterialTableCore {
  VkrMetalMaterialTableConfig config;
  VkrMetalMaterialGpuRow *mapped_rows;
  VkrMetalMaterialSlot *slots;
  VkrMetalMaterialRetirement *retirements;
  uint32_t retirement_count;
  VkrMetalMaterialTableMetrics metrics;
};

_Static_assert(sizeof(VkrMetalMaterialGpuRow) == 96,
               "Metal material row ABI must remain 96 bytes");
_Static_assert(offsetof(VkrMetalMaterialGpuRow, base_color_texture_id) == 16,
               "Metal material base texture offset changed");
_Static_assert(offsetof(VkrMetalMaterialGpuRow, base_color_sampler_id) == 48,
               "Metal material sampler offset changed");
_Static_assert(offsetof(VkrMetalMaterialGpuRow, material_id) == 80,
               "Metal material identifier offset changed");

static bool8_t vkr_metal_material_add_storage(uint64_t *size, uint64_t count,
                                              uint64_t element_size,
                                              uint64_t alignment) {
  if (*size > UINT64_MAX - (alignment - 1))
    return false_v;
  *size = AlignPow2(*size, alignment);
  if (count != 0 && element_size > (UINT64_MAX - *size) / count)
    return false_v;
  *size += count * element_size;
  return true_v;
}

uint64_t vkr_metal_material_table_storage_requirement(
    const VkrMetalMaterialTableConfig *config) {
  if (!config || config->max_rows == 0 || config->max_retirements == 0)
    return 0;
  uint64_t size = _Alignof(VkrMetalMaterialTableCore) - 1;
  if (!vkr_metal_material_add_storage(&size, 1,
                                      sizeof(VkrMetalMaterialTableCore),
                                      _Alignof(VkrMetalMaterialTableCore)) ||
      !vkr_metal_material_add_storage(&size, config->max_rows,
                                      sizeof(VkrMetalMaterialSlot),
                                      _Alignof(VkrMetalMaterialSlot)) ||
      !vkr_metal_material_add_storage(&size, config->max_retirements,
                                      sizeof(VkrMetalMaterialRetirement),
                                      _Alignof(VkrMetalMaterialRetirement)))
    return 0;
  return size;
}

static void *vkr_metal_material_take_storage(uint8_t **cursor, uint8_t *end,
                                             uint64_t count,
                                             uint64_t element_size,
                                             uint64_t alignment) {
  const uintptr_t aligned = AlignPow2((uintptr_t)*cursor, alignment);
  if (aligned > (uintptr_t)end ||
      count > (uint64_t)((uintptr_t)end - aligned) / element_size)
    return NULL;
  *cursor = (uint8_t *)(aligned + count * element_size);
  return (void *)aligned;
}

VkrMetalMaterialStatus
vkr_metal_material_table_create(const VkrMetalMaterialTableConfig *config,
                                void *storage, uint64_t storage_size,
                                VkrMetalMaterialGpuRow *mapped_rows,
                                VkrMetalMaterialTableCore **out_table) {
  const uint64_t required =
      vkr_metal_material_table_storage_requirement(config);
  if (!storage || !mapped_rows || !out_table || required == 0 ||
      storage_size < required)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
  *out_table = NULL;

  uint8_t *cursor = storage;
  uint8_t *end = cursor + storage_size;
  VkrMetalMaterialTableCore *table = vkr_metal_material_take_storage(
      &cursor, end, 1, sizeof(*table), _Alignof(VkrMetalMaterialTableCore));
  VkrMetalMaterialSlot *slots = vkr_metal_material_take_storage(
      &cursor, end, config->max_rows, sizeof(*slots),
      _Alignof(VkrMetalMaterialSlot));
  VkrMetalMaterialRetirement *retirements = vkr_metal_material_take_storage(
      &cursor, end, config->max_retirements, sizeof(*retirements),
      _Alignof(VkrMetalMaterialRetirement));
  if (!table || !slots || !retirements)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;

  MemZero(table, sizeof(*table));
  MemZero(slots, sizeof(*slots) * config->max_rows);
  MemZero(retirements, sizeof(*retirements) * config->max_retirements);
  MemZero(mapped_rows, sizeof(*mapped_rows) * config->max_rows);
  table->config = *config;
  table->mapped_rows = mapped_rows;
  table->slots = slots;
  table->retirements = retirements;
  for (uint32_t i = 0; i < config->max_rows; ++i)
    slots[i].generation = 1;
  *out_table = table;
  return VKR_METAL_MATERIAL_STATUS_OK;
}

static int32_t
vkr_metal_material_find_free_row(VkrMetalMaterialTableCore *table) {
  for (uint32_t i = 0; i < table->config.max_rows; ++i) {
    if (table->slots[i].state == VKR_METAL_MATERIAL_ROW_FREE)
      return (int32_t)i;
  }
  return -1;
}

static bool8_t
vkr_metal_material_handle_is_live(VkrMetalMaterialTableCore *table,
                                  VkrMetalMaterialHandle handle) {
  return handle.index < table->config.max_rows &&
         table->slots[handle.index].state == VKR_METAL_MATERIAL_ROW_LIVE &&
         table->slots[handle.index].generation == handle.generation;
}

static void vkr_metal_material_publish_at(VkrMetalMaterialTableCore *table,
                                          uint32_t row_index,
                                          const VkrMetalMaterialGpuRow *row,
                                          VkrMetalMaterialHandle *out_handle) {
  table->mapped_rows[row_index] = *row;
  atomic_thread_fence(memory_order_release);
  table->slots[row_index].state = VKR_METAL_MATERIAL_ROW_LIVE;
  *out_handle =
      (VkrMetalMaterialHandle){row_index, table->slots[row_index].generation};
  table->metrics.rows_live++;
  table->metrics.rows_published++;
  table->metrics.rows_peak =
      Max(table->metrics.rows_peak,
          table->metrics.rows_live + table->metrics.rows_retired);
}

VkrMetalMaterialStatus
vkr_metal_material_table_publish(VkrMetalMaterialTableCore *table,
                                 const VkrMetalMaterialGpuRow *row,
                                 VkrMetalMaterialHandle *out_handle) {
  if (!table || !row || !out_handle)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
  const int32_t row_index = vkr_metal_material_find_free_row(table);
  if (row_index < 0) {
    table->metrics.capacity_failures++;
    return VKR_METAL_MATERIAL_STATUS_CAPACITY_EXHAUSTED;
  }
  vkr_metal_material_publish_at(table, (uint32_t)row_index, row, out_handle);
  return VKR_METAL_MATERIAL_STATUS_OK;
}

static void vkr_metal_material_retire_live(VkrMetalMaterialTableCore *table,
                                           VkrMetalMaterialHandle handle,
                                           uint64_t last_use_submit_value) {
  VkrMetalMaterialSlot *slot = &table->slots[handle.index];
  table->retirements[table->retirement_count++] =
      (VkrMetalMaterialRetirement){last_use_submit_value, handle.index};
  slot->state = VKR_METAL_MATERIAL_ROW_RETIRED;
  slot->generation++;
  if (slot->generation == 0)
    slot->generation = 1;
  table->metrics.rows_live--;
  table->metrics.rows_retired++;
}

VkrMetalMaterialStatus vkr_metal_material_table_replace(
    VkrMetalMaterialTableCore *table, VkrMetalMaterialHandle old_handle,
    const VkrMetalMaterialGpuRow *new_row, uint64_t old_last_use_submit_value,
    VkrMetalMaterialHandle *out_new_handle) {
  if (!table || !new_row || !out_new_handle)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
  if (!vkr_metal_material_handle_is_live(table, old_handle)) {
    table->metrics.stale_handle_failures++;
    return VKR_METAL_MATERIAL_STATUS_STALE_HANDLE;
  }
  if (table->retirement_count == table->config.max_retirements) {
    table->metrics.retirement_capacity_failures++;
    return VKR_METAL_MATERIAL_STATUS_RETIREMENT_CAPACITY_EXHAUSTED;
  }
  const int32_t row_index = vkr_metal_material_find_free_row(table);
  if (row_index < 0) {
    table->metrics.capacity_failures++;
    return VKR_METAL_MATERIAL_STATUS_CAPACITY_EXHAUSTED;
  }

  vkr_metal_material_publish_at(table, (uint32_t)row_index, new_row,
                                out_new_handle);
  vkr_metal_material_retire_live(table, old_handle, old_last_use_submit_value);
  table->metrics.rows_replaced++;
  return VKR_METAL_MATERIAL_STATUS_OK;
}

VkrMetalMaterialStatus
vkr_metal_material_table_retire(VkrMetalMaterialTableCore *table,
                                VkrMetalMaterialHandle handle,
                                uint64_t last_use_submit_value) {
  if (!table)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
  if (!vkr_metal_material_handle_is_live(table, handle)) {
    table->metrics.stale_handle_failures++;
    return VKR_METAL_MATERIAL_STATUS_STALE_HANDLE;
  }
  if (table->retirement_count == table->config.max_retirements) {
    table->metrics.retirement_capacity_failures++;
    return VKR_METAL_MATERIAL_STATUS_RETIREMENT_CAPACITY_EXHAUSTED;
  }
  vkr_metal_material_retire_live(table, handle, last_use_submit_value);
  return VKR_METAL_MATERIAL_STATUS_OK;
}

VkrMetalMaterialStatus
vkr_metal_material_table_resolve(VkrMetalMaterialTableCore *table,
                                 VkrMetalMaterialHandle handle,
                                 uint32_t *out_row_index) {
  if (!table || !out_row_index)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
  if (!vkr_metal_material_handle_is_live(table, handle)) {
    table->metrics.stale_handle_failures++;
    return VKR_METAL_MATERIAL_STATUS_STALE_HANDLE;
  }
  atomic_thread_fence(memory_order_acquire);
  *out_row_index = handle.index;
  return VKR_METAL_MATERIAL_STATUS_OK;
}

VkrMetalMaterialStatus
vkr_metal_material_table_collect(VkrMetalMaterialTableCore *table,
                                 uint64_t completed_submit_value,
                                 uint32_t *out_collected_count) {
  if (!table)
    return VKR_METAL_MATERIAL_STATUS_INVALID_ARGUMENT;
  uint32_t collected = 0;
  for (uint32_t i = 0; i < table->retirement_count;) {
    const VkrMetalMaterialRetirement retirement = table->retirements[i];
    if (retirement.submit_value > completed_submit_value) {
      i++;
      continue;
    }
    MemZero(&table->mapped_rows[retirement.row_index],
            sizeof(VkrMetalMaterialGpuRow));
    table->slots[retirement.row_index].state = VKR_METAL_MATERIAL_ROW_FREE;
    table->metrics.rows_retired--;
    table->metrics.rows_collected++;
    for (uint32_t j = i + 1; j < table->retirement_count; ++j)
      table->retirements[j - 1] = table->retirements[j];
    table->retirement_count--;
    collected++;
  }
  if (out_collected_count)
    *out_collected_count = collected;
  return VKR_METAL_MATERIAL_STATUS_OK;
}

void vkr_metal_material_table_get_metrics(
    const VkrMetalMaterialTableCore *table,
    VkrMetalMaterialTableMetrics *out_metrics) {
  if (!out_metrics)
    return;
  *out_metrics = table ? table->metrics : (VkrMetalMaterialTableMetrics){0};
}

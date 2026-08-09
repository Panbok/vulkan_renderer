#include "renderer/metal/vkr_metal_material_table.h"

#include "renderer/vkr_gpu_slot_table.h"

_Static_assert(sizeof(VkrMetalMaterialGpuRow) == 96,
               "Metal material row ABI must remain 96 bytes");
_Static_assert(offsetof(VkrMetalMaterialGpuRow, base_color_texture_id) == 16,
               "Metal material base texture offset changed");
_Static_assert(offsetof(VkrMetalMaterialGpuRow, base_color_sampler_id) == 48,
               "Metal material sampler offset changed");
_Static_assert(offsetof(VkrMetalMaterialGpuRow, material_id) == 80,
               "Metal material identifier offset changed");

static VkrGpuSlotTableConfig
vkr_metal_material_core_config(const VkrMetalMaterialTableConfig *config) {
  return config ? (VkrGpuSlotTableConfig){
                      .max_slots = config->max_rows,
                      .max_retirements = config->max_retirements,
                      .row_size = sizeof(VkrMetalMaterialGpuRow),
                  }
                : (VkrGpuSlotTableConfig){0};
}

uint64_t vkr_metal_material_table_storage_requirement(
    const VkrMetalMaterialTableConfig *config) {
  const VkrGpuSlotTableConfig core_config =
      vkr_metal_material_core_config(config);
  return vkr_gpu_slot_table_storage_requirement(&core_config);
}

VkrMetalMaterialStatus
vkr_metal_material_table_create(const VkrMetalMaterialTableConfig *config,
                                void *storage, uint64_t storage_size,
                                VkrMetalMaterialGpuRow *mapped_rows,
                                VkrMetalMaterialTableCore **out_table) {
  const VkrGpuSlotTableConfig core_config =
      vkr_metal_material_core_config(config);
  return (VkrMetalMaterialStatus)vkr_gpu_slot_table_create(
      &core_config, storage, storage_size, mapped_rows,
      (VkrGpuSlotTable **)out_table);
}

VkrMetalMaterialStatus
vkr_metal_material_table_publish(VkrMetalMaterialTableCore *table,
                                 const VkrMetalMaterialGpuRow *row,
                                 VkrMetalMaterialHandle *out_handle) {
  return (VkrMetalMaterialStatus)vkr_gpu_slot_table_publish(
      (VkrGpuSlotTable *)table, row, (VkrGpuSlotHandle *)out_handle);
}

VkrMetalMaterialStatus vkr_metal_material_table_replace(
    VkrMetalMaterialTableCore *table, VkrMetalMaterialHandle old_handle,
    const VkrMetalMaterialGpuRow *new_row, uint64_t old_last_use_submit_value,
    VkrMetalMaterialHandle *out_new_handle) {
  return (VkrMetalMaterialStatus)vkr_gpu_slot_table_replace(
      (VkrGpuSlotTable *)table,
      (VkrGpuSlotHandle){old_handle.index, old_handle.generation}, new_row,
      old_last_use_submit_value, (VkrGpuSlotHandle *)out_new_handle);
}

VkrMetalMaterialStatus
vkr_metal_material_table_retire(VkrMetalMaterialTableCore *table,
                                VkrMetalMaterialHandle handle,
                                uint64_t last_use_submit_value) {
  return (VkrMetalMaterialStatus)vkr_gpu_slot_table_retire(
      (VkrGpuSlotTable *)table,
      (VkrGpuSlotHandle){handle.index, handle.generation},
      last_use_submit_value);
}

VkrMetalMaterialStatus
vkr_metal_material_table_resolve(VkrMetalMaterialTableCore *table,
                                 VkrMetalMaterialHandle handle,
                                 uint32_t *out_row_index) {
  return (VkrMetalMaterialStatus)vkr_gpu_slot_table_resolve(
      (VkrGpuSlotTable *)table,
      (VkrGpuSlotHandle){handle.index, handle.generation}, out_row_index);
}

VkrMetalMaterialStatus
vkr_metal_material_table_collect(VkrMetalMaterialTableCore *table,
                                 uint64_t completed_submit_value,
                                 uint32_t *out_collected_count) {
  return (VkrMetalMaterialStatus)vkr_gpu_slot_table_collect(
      (VkrGpuSlotTable *)table, completed_submit_value, out_collected_count);
}

void vkr_metal_material_table_get_metrics(
    const VkrMetalMaterialTableCore *table,
    VkrMetalMaterialTableMetrics *out_metrics) {
  if (!out_metrics)
    return;
  VkrGpuSlotTableMetrics metrics = {0};
  vkr_gpu_slot_table_get_metrics((const VkrGpuSlotTable *)table, &metrics);
  *out_metrics = (VkrMetalMaterialTableMetrics){
      .rows_live = metrics.slots_live,
      .rows_retired = metrics.slots_retired,
      .rows_peak = metrics.slots_peak,
      .rows_published = metrics.slots_published,
      .rows_replaced = metrics.slots_replaced,
      .rows_collected = metrics.slots_collected,
      .capacity_failures = metrics.capacity_failures,
      .retirement_capacity_failures = metrics.retirement_capacity_failures,
      .stale_handle_failures = metrics.stale_handle_failures,
  };
}

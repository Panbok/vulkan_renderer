#include "vkr_light_table.h"

#include "memory/vkr_heap.h"
#include "vkr_lighting.h"

struct s_VkrLightTable {
  /* What the backend's table holds after the last submitted frame. */
  VkrGpuPointLightRow committed[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t committed_count;
  bool8_t committed_valid;
  /* The prepared frame, which commit makes the committed table. */
  VkrGpuPointLightRow staged[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t staged_count;
  bool8_t staged_valid;
  VkrLightRowUpdate updates[VKR_MAX_SCENE_POINT_LIGHTS];
};

VkrLightTable *vkr_light_table_create(void) {
  return (VkrLightTable *)vkr_heap_alloc_zeroed(1u, sizeof(VkrLightTable));
}

void vkr_light_table_destroy(VkrLightTable *table) {
  if (table) {
    vkr_heap_free(table);
  }
}

void vkr_light_table_prepare(VkrLightTable *table,
                             const VkrFrameLighting *lighting,
                             const VkrLocalShadowPassPayload *local_shadow,
                             VkrLightTableFrame *out_frame) {
  const uint32_t count = lighting ? lighting->point_light_count : 0u;
  for (uint32_t row = 0u; row < count; ++row) {
    vkr_point_light_pack(&lighting->point_lights[row], 1.0f,
                         &table->staged[row]);
  }
  /* Only the light head has shadow views (ADR-019). */
  if (local_shadow) {
    const uint32_t head = Min(count, VKR_POINT_LIGHT_GRID_LIGHT_MAX);
    for (uint32_t row = 0u; row < head; ++row) {
      table->staged[row].p3.w = (float32_t)local_shadow->light_first_view[row];
    }
  }
  table->staged_count = count;
  table->staged_valid = true_v;

  const bool8_t full = !table->committed_valid;
  uint32_t update_count = 0u;
  for (uint32_t row = 0u; row < count; ++row) {
    /* Rows past the committed count were never written for this table. */
    if (full || row >= table->committed_count ||
        MemCompare(&table->staged[row], &table->committed[row],
                   sizeof(VkrGpuPointLightRow)) != 0) {
      table->updates[update_count++] = (VkrLightRowUpdate){
          .row = row,
          .value = table->staged[row],
      };
    }
  }
  *out_frame = (VkrLightTableFrame){
      .rows = table->staged,
      .count = count,
      .updates = table->updates,
      .update_count = update_count,
      .full = full,
  };
}

void vkr_light_table_commit(VkrLightTable *table) {
  if (!table->staged_valid) {
    return;
  }
  MemCopy(table->committed, table->staged,
          (uint64_t)table->staged_count * sizeof(VkrGpuPointLightRow));
  table->committed_count = table->staged_count;
  table->committed_valid = true_v;
  table->staged_valid = false_v;
}

void vkr_light_table_invalidate(VkrLightTable *table) {
  table->committed_valid = false_v;
  table->committed_count = 0u;
}

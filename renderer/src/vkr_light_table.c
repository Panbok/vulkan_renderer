#include "vkr_light_table.h"

#include "memory/vkr_heap.h"
#include "vkr_lighting.h"

/* Each array holds `capacity` rows, grown with the frames' row counts up
   to VKR_MAX_SCENE_POINT_LIGHTS and kept until the table is destroyed. */
struct s_VkrLightTable {
  /* What the backend's table holds after the last submitted frame. */
  VkrGpuPointLightRow *committed;
  uint32_t committed_count;
  bool8_t committed_valid;
  /* The prepared frame, which commit makes the committed table. */
  VkrGpuPointLightRow *staged;
  uint32_t staged_count;
  bool8_t staged_valid;
  VkrLightRowUpdate *updates;
  uint32_t capacity;
};

/* Grows the table's arrays to `count` rows, keeping the committed ones. */
vkr_internal bool8_t vkr_light_table_reserve(VkrLightTable *table,
                                             uint32_t count) {
  if (count <= table->capacity) {
    return true_v;
  }
  if (count > VKR_MAX_SCENE_POINT_LIGHTS) {
    return false_v;
  }
  const uint32_t capacity = (uint32_t)Min(
      Max((uint64_t)count, Max((uint64_t)table->capacity * 2u, 64u)),
      (uint64_t)VKR_MAX_SCENE_POINT_LIGHTS);
  VkrGpuPointLightRow *committed = vkr_heap_realloc(
      table->committed, (uint64_t)capacity * sizeof(*committed));
  if (!committed) {
    return false_v;
  }
  table->committed = committed;
  VkrGpuPointLightRow *staged =
      vkr_heap_realloc(table->staged, (uint64_t)capacity * sizeof(*staged));
  if (!staged) {
    return false_v;
  }
  table->staged = staged;
  VkrLightRowUpdate *updates =
      vkr_heap_realloc(table->updates, (uint64_t)capacity * sizeof(*updates));
  if (!updates) {
    return false_v;
  }
  table->updates = updates;
  table->capacity = capacity;
  return true_v;
}

VkrLightTable *vkr_light_table_create(void) {
  return (VkrLightTable *)vkr_heap_alloc_zeroed(1u, sizeof(VkrLightTable));
}

void vkr_light_table_destroy(VkrLightTable *table) {
  if (table) {
    vkr_heap_free(table->committed);
    vkr_heap_free(table->staged);
    vkr_heap_free(table->updates);
    vkr_heap_free(table);
  }
}

bool8_t vkr_light_table_prepare(VkrLightTable *table,
                                const VkrFrameLighting *lighting,
                                const VkrLocalShadowPassPayload *local_shadow,
                                VkrLightTableFrame *out_frame) {
  *out_frame = (VkrLightTableFrame){0};
  table->staged_valid = false_v;
  const uint32_t count = lighting ? lighting->point_light_count : 0u;
  if (!vkr_light_table_reserve(table, count)) {
    return false_v;
  }
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
  return true_v;
}

void vkr_light_table_commit(VkrLightTable *table) {
  if (!table->staged_valid) {
    return;
  }
  /* The arrays trade places; the next prepare writes every staged row. */
  VkrGpuPointLightRow *committed = table->committed;
  table->committed = table->staged;
  table->staged = committed;
  table->committed_count = table->staged_count;
  table->committed_valid = true_v;
  table->staged_valid = false_v;
}

void vkr_light_table_invalidate(VkrLightTable *table) {
  table->committed_valid = false_v;
  table->committed_count = 0u;
}

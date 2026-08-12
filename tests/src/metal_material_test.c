#include "metal_material_test.h"

#include "renderer/metal/vkr_metal_material_table.h"
#include "renderer/vkr_gpu_slot_table.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MaterialFixture {
  void *storage;
  VkrMetalMaterialGpuRow *rows;
  VkrMetalMaterialTableCore *table;
} MaterialFixture;

static MaterialFixture material_fixture(VkrMetalMaterialTableConfig config) {
  const uint64_t storage_size =
      vkr_metal_material_table_storage_requirement(&config);
  void *storage = malloc(storage_size);
  VkrMetalMaterialGpuRow *rows = calloc(config.max_rows, sizeof(*rows));
  assert(storage && rows);
  VkrMetalMaterialTableCore *table = NULL;
  assert(vkr_metal_material_table_create(&config, storage, storage_size, rows,
                                         &table) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  return (MaterialFixture){storage, rows, table};
}

static VkrMetalMaterialGpuRow material_row(uint32_t id, uint64_t texture) {
  VkrMetalMaterialGpuRow row = {
      .tint = {1.0f, 0.5f, 0.25f, 1.0f},
      .base_color_texture_id = texture,
      .normal_texture_id = texture + 1,
      .orm_texture_id = texture + 2,
      .emissive_texture_id = texture + 3,
      .material_id = id,
      .alpha_mode = id % 3u,
      .material_emissive = {(float32_t)id, 2.0f, 3.0f, 0.0f},
      .material_surface = {0.25f, 0.5f, 1.0f, 1.0f},
      .material_alpha = {0.5f, 0.0f, 1.5f, 0.0f},
      .material_attenuation_color = {1.0f, 0.5f, 0.25f, (float32_t)texture},
  };
  return row;
}

static void test_material_publication_and_pending_replacement(void) {
  printf("  Running test_material_publication_and_pending_replacement...\n");
  MaterialFixture fixture =
      material_fixture((VkrMetalMaterialTableConfig){3, 3});
  VkrMetalMaterialGpuRow red = material_row(10, 100);
  VkrMetalMaterialGpuRow blue = material_row(20, 200);
  VkrMetalMaterialGpuRow green = material_row(30, 300);
  VkrMetalMaterialHandle a = {0}, b = {0}, replacement = {0};
  assert(vkr_metal_material_table_publish(fixture.table, &red, &a) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(vkr_metal_material_table_publish(fixture.table, &blue, &b) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(vkr_metal_material_table_replace(fixture.table, a, &green, 7,
                                          &replacement) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(replacement.index != a.index);
  assert(fixture.rows[a.index].material_id == red.material_id);
  assert(fixture.rows[replacement.index].material_id == green.material_id);
  assert(fixture.rows[replacement.index].material_emissive.x == 30.0f);
  assert(fixture.rows[replacement.index].material_surface.y == 0.5f);
  assert(fixture.rows[replacement.index].material_attenuation_color.w ==
         300.0f);
  uint32_t row_index = 0;
  assert(vkr_metal_material_table_resolve(fixture.table, a, &row_index) ==
         VKR_METAL_MATERIAL_STATUS_STALE_HANDLE);
  assert(vkr_metal_material_table_resolve(fixture.table, replacement,
                                          &row_index) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(vkr_metal_material_table_collect(fixture.table, 6, NULL) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(fixture.rows[a.index].material_id == red.material_id);
  uint32_t collected = 0;
  assert(vkr_metal_material_table_collect(fixture.table, 7, &collected) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(collected == 1 && fixture.rows[a.index].material_id == 0);

  assert(vkr_metal_material_table_retire(fixture.table, b, 8) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(vkr_metal_material_table_retire(fixture.table, replacement, 8) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(vkr_metal_material_table_collect(fixture.table, 8, &collected) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(collected == 2);
  VkrMetalMaterialTableMetrics metrics = {0};
  vkr_metal_material_table_get_metrics(fixture.table, &metrics);
  assert(metrics.rows_live == 0 && metrics.rows_retired == 0);
  assert(metrics.rows_published == 3 && metrics.rows_replaced == 1);
  assert(metrics.rows_collected == 3 && metrics.rows_peak == 3);
  free(fixture.rows);
  free(fixture.storage);
  printf("  test_material_publication_and_pending_replacement PASSED\n");
}

static void test_material_replacement_capacity_is_transactional(void) {
  printf("  Running test_material_replacement_capacity_is_transactional...\n");
  MaterialFixture fixture =
      material_fixture((VkrMetalMaterialTableConfig){2, 2});
  VkrMetalMaterialGpuRow a_row = material_row(1, 10);
  VkrMetalMaterialGpuRow b_row = material_row(2, 20);
  VkrMetalMaterialGpuRow replacement_row = material_row(3, 30);
  VkrMetalMaterialHandle a = {0}, b = {0}, replacement = {0};
  assert(vkr_metal_material_table_publish(fixture.table, &a_row, &a) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(vkr_metal_material_table_publish(fixture.table, &b_row, &b) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(vkr_metal_material_table_replace(fixture.table, a, &replacement_row, 1,
                                          &replacement) ==
         VKR_METAL_MATERIAL_STATUS_CAPACITY_EXHAUSTED);
  uint32_t row_index = VKR_INVALID_ID;
  assert(vkr_metal_material_table_resolve(fixture.table, a, &row_index) ==
         VKR_METAL_MATERIAL_STATUS_OK);
  assert(row_index == a.index && fixture.rows[a.index].material_id == 1);
  VkrMetalMaterialTableMetrics metrics = {0};
  vkr_metal_material_table_get_metrics(fixture.table, &metrics);
  assert(metrics.rows_live == 2 && metrics.rows_retired == 0);
  assert(metrics.capacity_failures == 1 && metrics.rows_replaced == 0);
  free(fixture.rows);
  free(fixture.storage);
  printf("  test_material_replacement_capacity_is_transactional PASSED\n");
}

static void test_shared_slot_table_row_size_and_sentinel_rule(void) {
  printf("  Running test_shared_slot_table_row_size_and_sentinel_rule...\n");
  const VkrGpuSlotTableConfig config = {
      .max_slots = 3u,
      .max_retirements = 3u,
      .row_size = 37u,
  };
  const uint64_t storage_size = vkr_gpu_slot_table_storage_requirement(&config);
  void *storage = malloc(storage_size);
  uint8_t rows[3][37] = {0};
  VkrGpuSlotTable *table = NULL;
  assert(storage &&
         vkr_gpu_slot_table_create(&config, storage, storage_size, rows,
                                   &table) == VKR_GPU_SLOT_STATUS_OK);
  uint8_t sentinel[37], first[37], replacement[37];
  memset(sentinel, 0x11, sizeof(sentinel));
  memset(first, 0x22, sizeof(first));
  memset(replacement, 0x33, sizeof(replacement));
  VkrGpuSlotHandle sentinel_handle = {0}, first_handle = {0}, new_handle = {0};
  assert(vkr_gpu_slot_table_publish(table, sentinel, &sentinel_handle) ==
             VKR_GPU_SLOT_STATUS_OK &&
         sentinel_handle.index == 0u);
  assert(vkr_gpu_slot_table_publish(table, first, &first_handle) ==
             VKR_GPU_SLOT_STATUS_OK &&
         first_handle.index == 1u);
  assert(vkr_gpu_slot_table_replace(table, first_handle, replacement, 9u,
                                    &new_handle) == VKR_GPU_SLOT_STATUS_OK);
  assert(new_handle.index == 2u && rows[1][0] == 0x22 && rows[2][0] == 0x33);
  VkrGpuSlotHandle exhausted = {0};
  assert(vkr_gpu_slot_table_publish(table, first, &exhausted) ==
         VKR_GPU_SLOT_STATUS_CAPACITY_EXHAUSTED);
  VkrGpuSlotTableMetrics capacity_metrics = {0};
  vkr_gpu_slot_table_get_metrics(table, &capacity_metrics);
  assert(capacity_metrics.capacity_failures == 1u);
  assert(vkr_gpu_slot_table_collect(table, 8u, NULL) ==
             VKR_GPU_SLOT_STATUS_OK &&
         rows[1][0] == 0x22);
  assert(vkr_gpu_slot_table_collect(table, 9u, NULL) ==
             VKR_GPU_SLOT_STATUS_OK &&
         rows[1][0] == 0x00 && rows[0][0] == 0x11);
  uint32_t sentinel_index = VKR_INVALID_ID;
  assert(vkr_gpu_slot_table_resolve(table, sentinel_handle, &sentinel_index) ==
             VKR_GPU_SLOT_STATUS_OK &&
         sentinel_index == 0u && rows[0][0] == 0x11);
  free(storage);
  printf("  test_shared_slot_table_row_size_and_sentinel_rule PASSED\n");
}

/*
 * Free slots are handed out from a stack rather than found by scanning, so the
 * two invariants that used to fall out of the scan are pinned explicitly here:
 * a virgin table must still hand out 0, 1, 2, ... (the sentinel contract), and
 * collection must return every slot to the pool so a long publish/retire cycle
 * cannot exhaust a table that is logically empty.
 */
static void test_shared_slot_table_recycles_every_collected_slot(void) {
  printf("  Running test_shared_slot_table_recycles_every_collected_slot...\n");
  enum { SLOT_COUNT = 8u };
  const VkrGpuSlotTableConfig config = {
      .max_slots = SLOT_COUNT,
      .max_retirements = SLOT_COUNT,
      .row_size = sizeof(uint32_t),
  };
  const uint64_t storage_size = vkr_gpu_slot_table_storage_requirement(&config);
  void *storage = malloc(storage_size);
  uint32_t rows[SLOT_COUNT] = {0};
  VkrGpuSlotTable *table = NULL;
  assert(storage &&
         vkr_gpu_slot_table_create(&config, storage, storage_size, rows,
                                   &table) == VKR_GPU_SLOT_STATUS_OK);

  /* A virgin table allocates ascending from zero. */
  VkrGpuSlotHandle handles[SLOT_COUNT] = {0};
  for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
    const uint32_t row = 0xA0u + i;
    assert(vkr_gpu_slot_table_publish(table, &row, &handles[i]) ==
           VKR_GPU_SLOT_STATUS_OK);
    assert(handles[i].index == i);
  }
  VkrGpuSlotHandle overflow = {0};
  assert(vkr_gpu_slot_table_publish(table, &(uint32_t){0}, &overflow) ==
         VKR_GPU_SLOT_STATUS_CAPACITY_EXHAUSTED);

  /* Ten full drain/refill cycles must never exhaust the table. */
  for (uint32_t cycle = 0; cycle < 10u; ++cycle) {
    const uint64_t submit = cycle + 1u;
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
      assert(vkr_gpu_slot_table_retire(table, handles[i], submit) ==
             VKR_GPU_SLOT_STATUS_OK);
    }
    uint32_t collected = 0;
    assert(vkr_gpu_slot_table_collect(table, submit, &collected) ==
           VKR_GPU_SLOT_STATUS_OK);
    assert(collected == SLOT_COUNT);
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
      const uint32_t row = 0xB0u + i;
      assert(vkr_gpu_slot_table_publish(table, &row, &handles[i]) ==
             VKR_GPU_SLOT_STATUS_OK);
      assert(handles[i].index < SLOT_COUNT);
    }
  }

  VkrGpuSlotTableMetrics metrics = {0};
  vkr_gpu_slot_table_get_metrics(table, &metrics);
  assert(metrics.slots_live == SLOT_COUNT && metrics.slots_retired == 0u);
  assert(metrics.slots_collected == (uint64_t)SLOT_COUNT * 10u);

  /* Every live handle must still resolve to a distinct slot. */
  bool8_t seen[SLOT_COUNT] = {0};
  for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
    uint32_t index = VKR_INVALID_ID;
    assert(vkr_gpu_slot_table_resolve(table, handles[i], &index) ==
           VKR_GPU_SLOT_STATUS_OK);
    assert(index < SLOT_COUNT && !seen[index]);
    seen[index] = true_v;
  }

  free(storage);
  printf("  test_shared_slot_table_recycles_every_collected_slot PASSED\n");
}

bool32_t run_metal_material_tests(void) {
  printf("Running Metal material tests...\n");
  test_material_publication_and_pending_replacement();
  test_material_replacement_capacity_is_transactional();
  test_shared_slot_table_row_size_and_sentinel_rule();
  test_shared_slot_table_recycles_every_collected_slot();
  printf("Metal material tests PASSED\n");
  return true_v;
}

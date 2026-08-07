#include "metal_material_test.h"

#include "renderer/metal/vkr_metal_material_table.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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

bool32_t run_metal_material_tests(void) {
  printf("Running Metal material tests...\n");
  test_material_publication_and_pending_replacement();
  test_material_replacement_capacity_is_transactional();
  printf("Metal material tests PASSED\n");
  return true_v;
}

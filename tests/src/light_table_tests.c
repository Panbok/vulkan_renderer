#include "light_table_tests.h"

#include "vkr_light_table.h"
#include "vkr_lighting.h"

#include <assert.h>
#include <stdio.h>

vkr_internal VkrPointLight light_table_test_light(float32_t x) {
  return (VkrPointLight){
      .position = vec3_new(x, 1.0f, 0.0f),
      .color = vec3_one(),
      .intensity = 2.0f,
      .range = 4.0f,
      .direction = vec3_new(0.0f, -1.0f, 0.0f),
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
  };
}

/* Whether the frame's updates are exactly `rows`, ascending. */
vkr_internal bool8_t light_table_test_updates(const VkrLightTableFrame *frame,
                                              const uint32_t *rows,
                                              uint32_t count) {
  if (frame->update_count != count) {
    return false_v;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (frame->updates[i].row != rows[i] ||
        MemCompare(&frame->updates[i].value, &frame->rows[rows[i]],
                   sizeof(VkrGpuPointLightRow)) != 0) {
      return false_v;
    }
  }
  return true_v;
}

/* A backend that keeps its table between frames writes only what changed
   since the last submitted frame: nothing for a repeated frame, one row for
   a moved light, the rows a table grows by, every change of a frame that
   was never submitted, a changed shadow view, and every row after an
   invalidation. */
vkr_internal bool32_t test_light_table_sends_only_changed_rows(void) {
  printf("  Running test_light_table_sends_only_changed_rows...\n");
  VkrLightTable *table = vkr_light_table_create();
  assert(table);
  VkrPointLight lights[4] = {
      light_table_test_light(0.0f), light_table_test_light(5.0f),
      light_table_test_light(10.0f), light_table_test_light(15.0f)};
  VkrFrameLighting lighting = {.point_lights = lights, .point_light_count = 3u};
  VkrLightTableFrame frame;

  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(frame.full && frame.count == 3u);
  assert(light_table_test_updates(&frame, (uint32_t[]){0u, 1u, 2u}, 3u));
  VkrGpuPointLightRow expected;
  vkr_point_light_pack(&lights[1], 1.0f, &expected);
  assert(MemCompare(&frame.rows[1], &expected, sizeof(expected)) == 0);
  vkr_light_table_commit(table);

  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(!frame.full && frame.update_count == 0u);
  vkr_light_table_commit(table);

  lights[1].position.x = 6.0f;
  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(light_table_test_updates(&frame, (uint32_t[]){1u}, 1u));
  vkr_light_table_commit(table);

  /* Growth writes the new row; a shrink writes nothing, and the row that
     returns is written again although its value did not change. */
  lighting.point_light_count = 4u;
  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(light_table_test_updates(&frame, (uint32_t[]){3u}, 1u));
  vkr_light_table_commit(table);
  lighting.point_light_count = 2u;
  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(frame.count == 2u && frame.update_count == 0u);
  vkr_light_table_commit(table);
  lighting.point_light_count = 3u;
  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(light_table_test_updates(&frame, (uint32_t[]){2u}, 1u));
  vkr_light_table_commit(table);

  /* A frame never submitted leaves its change to the next one. */
  lights[0].intensity = 3.0f;
  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(light_table_test_updates(&frame, (uint32_t[]){0u}, 1u));
  lights[2].intensity = 3.0f;
  vkr_light_table_prepare(table, &lighting, NULL, &frame);
  assert(light_table_test_updates(&frame, (uint32_t[]){0u, 2u}, 2u));
  vkr_light_table_commit(table);

  /* A head row's shadow view is part of its row. */
  static VkrLocalShadowPassPayload local_shadow;
  local_shadow.light_first_view[1] = 5u;
  vkr_light_table_prepare(table, &lighting, &local_shadow, &frame);
  assert(light_table_test_updates(&frame, (uint32_t[]){1u}, 1u));
  assert(frame.rows[1].p3.w == 5.0f);
  vkr_light_table_commit(table);

  vkr_light_table_invalidate(table);
  vkr_light_table_prepare(table, &lighting, &local_shadow, &frame);
  assert(frame.full &&
         light_table_test_updates(&frame, (uint32_t[]){0u, 1u, 2u}, 3u));

  vkr_light_table_destroy(table);
  printf("  test_light_table_sends_only_changed_rows PASSED\n");
  return true_v;
}

bool32_t run_light_table_tests(void) {
  printf("--- Running Light table tests... ---\n");
  bool32_t passed = true_v;
  passed &= test_light_table_sends_only_changed_rows();
  printf("--- Light table tests completed. ---\n");
  return passed;
}

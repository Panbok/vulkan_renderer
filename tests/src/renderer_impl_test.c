#include "renderer_impl_test.h"

#include "renderer/systems/vkr_render_assets.h"
#include "renderer/vkr_renderer_impl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

vkr_internal void test_texture_pressure_budget_hysteresis(void) {
  printf("  Running test_texture_pressure_budget_hysteresis...\n");
  VkrDeviceMemoryStats stats = {
      .heap_count = 1u,
      .heap_usage_valid = true_v,
      .heap_usage_bytes = {900u},
      .heap_budget_bytes = {1000u},
  };
  stats.owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes = 300u;
  uint64_t budget = 0u;
  bool8_t active = false_v;
  assert(vkr_render_assets_texture_pressure_budget(&stats, false_v, &budget,
                                                   &active) == true_v);
  assert(active == true_v);
  assert(budget == 200u);

  stats.heap_usage_bytes[0] = 850u;
  stats.pending_texture_upload_bytes = 50u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, false_v, &budget,
                                                   &active) == true_v);
  assert(active == true_v);
  assert(budget == 250u);
  stats.pending_texture_upload_bytes = 0u;

  /* Releasing 100 non-texture bytes raises the texture allowance by 100,
     while pressure remains active until the existing 75% exit threshold. */
  stats.heap_usage_bytes[0] = 800u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active) == true_v);
  assert(active == true_v);
  assert(budget == 300u);
  stats.heap_usage_bytes[0] = 750u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active) == true_v);
  assert(active == false_v);
  assert(budget == UINT64_MAX);
  printf("  test_texture_pressure_budget_hysteresis PASSED\n");
}

vkr_internal void test_texture_heap_capacity_pressure_budget(void) {
  printf("  Running test_texture_heap_capacity_pressure_budget...\n");
  VkrDeviceMemoryStats stats = {
      .heap_count = 1u,
      .heap_usage_valid = true_v,
      .heap_usage_bytes = {900u},
      .heap_budget_bytes = {1000u},
      .texture_heap_capacity_bytes = 600u,
      .texture_heap_capacity_valid = true_v,
  };
  uint64_t budget = 0u;
  bool8_t active = false_v;
  assert(vkr_render_assets_texture_pressure_budget(&stats, false_v, &budget,
                                                   &active));
  assert(active && budget == 600u);

  /* Publication and material residency can change while the same native
   * asset heaps remain charged. Neither state creates non-asset pressure. */
  stats.owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes = 300u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active));
  assert(active && budget == 600u);
  stats.owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes = 100u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active));
  assert(active && budget == 600u);

  /* Releasing an empty asset heap reduces both charged totals by 100.
   * It cannot create an allowance larger than the prior 600. */
  stats.heap_usage_bytes[0] = 800u;
  stats.texture_heap_capacity_bytes = 500u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active));
  assert(active && budget == 500u);

  /* Existing capacity remains usable above the 80% target. */
  stats.heap_usage_bytes[0] = 830u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active));
  assert(active && budget == 500u);

  /* A real 100-byte Scene reduction adds 100 bytes of asset allowance.
   * Hysteresis exit must preserve that finite value for retry accounting. */
  stats.heap_usage_bytes[0] = 800u;
  stats.texture_heap_capacity_bytes = 300u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active));
  assert(active && budget == 300u);
  stats.heap_usage_bytes[0] = 700u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active));
  assert(!active && budget == 400u);
  assert(vkr_render_assets_texture_pressure_budget(&stats, false_v, &budget,
                                                   &active));
  assert(!active && budget == 400u);
  printf("  test_texture_heap_capacity_pressure_budget PASSED\n");
}

bool32_t run_renderer_impl_tests(void) {
  printf("--- Running renderer implementation tests... ---\n");
  test_texture_pressure_budget_hysteresis();
  test_texture_heap_capacity_pressure_budget();
  printf("--- Renderer implementation tests completed. ---\n");
  return true_v;
}

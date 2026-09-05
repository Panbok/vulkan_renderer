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

  stats.heap_usage_bytes[0] = 800u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active) == false_v);
  stats.heap_usage_bytes[0] = 750u;
  assert(vkr_render_assets_texture_pressure_budget(&stats, true_v, &budget,
                                                   &active) == true_v);
  assert(active == false_v);
  assert(budget == UINT64_MAX);
  printf("  test_texture_pressure_budget_hysteresis PASSED\n");
}

bool32_t run_renderer_impl_tests(void) {
  printf("--- Running renderer implementation tests... ---\n");
  test_texture_pressure_budget_hysteresis();
  printf("--- Renderer implementation tests completed. ---\n");
  return true_v;
}

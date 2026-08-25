#include "renderer_impl_test.h"

#include "renderer/renderer_frontend.h"
#include "renderer/vkr_renderer_impl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_renderer_impl_capability_partition(void) {
  printf("  Running test_renderer_impl_capability_partition...\n");
  VkrRendererImpl vulkan = {0};
  VkrRendererImpl metal = {0};
  const VkrRendererImplOps metal_ops = {0};
  const VkrRendererImplOps vulkan_ops = {0};
  const VkrRendererImplStrategies strategies = {
      .metal = &metal_ops,
      .vulkan = &vulkan_ops,
  };
  assert(vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_VULKAN,
                                  VKR_PRESENT_TARGET_WINDOWED, &strategies,
                                  &vulkan));
  assert(vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_METAL,
                                  VKR_PRESENT_TARGET_OFFSCREEN, &strategies,
                                  &metal));
  assert(vulkan.kind == VKR_RENDERER_IMPL_VULKAN);
  assert(vulkan.ops == &vulkan_ops);
  assert(vulkan.caps.present_target_kind == VKR_PRESENT_TARGET_WINDOWED);
  assert(vulkan.caps.present_color_format == VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB);
  assert(vulkan.caps.frame_in_flight_count == 3u);
  assert(metal.kind == VKR_RENDERER_IMPL_METAL);
  assert(metal.ops == &metal_ops);
  assert(metal.caps.present_target_kind == VKR_PRESENT_TARGET_OFFSCREEN);
  assert(metal.caps.present_color_format == VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB);
  assert(metal.caps.frame_in_flight_count == 3u);
  printf("  test_renderer_impl_capability_partition PASSED\n");
}

static void test_vulkan_platform_availability(void) {
  printf("  Running test_vulkan_platform_availability...\n");
  VkrRendererImpl impl = {0};
  const VkrRendererImplOps metal_ops = {0};
  const VkrRendererImplOps vulkan_ops = {0};
  const VkrRendererImplStrategies strategies = {
      .metal = &metal_ops,
      .vulkan = &vulkan_ops,
  };
  assert(vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_VULKAN,
                                  VKR_PRESENT_TARGET_WINDOWED, &strategies,
                                  &impl));
  assert(impl.kind == VKR_RENDERER_IMPL_VULKAN);
  assert(impl.ops == &vulkan_ops);
#if defined(_WIN32)
  assert(impl.initialization_supported);
#else
  assert(!impl.initialization_supported);
#endif
  assert(!vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_DX12,
                                   VKR_PRESENT_TARGET_WINDOWED, &strategies,
                                   &impl));
  printf("  test_vulkan_platform_availability PASSED\n");
}

static void test_gpu_timing_reason_names(void) {
  printf("  Running test_gpu_timing_reason_names...\n");
  assert(strcmp(vkr_renderer_impl_gpu_timing_reason_name(
                    VKR_RENDERER_IMPL_GPU_TIMING_REASON_NONE),
                "none") == 0);
  assert(strcmp(vkr_renderer_impl_gpu_timing_reason_name(
                    VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED),
                "disabled") == 0);
  assert(strcmp(vkr_renderer_impl_gpu_timing_reason_name(
                    VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY),
                "not_ready") == 0);
  assert(
      strcmp(
          vkr_renderer_impl_gpu_timing_reason_name(
              VKR_RENDERER_IMPL_GPU_TIMING_REASON_UNSUPPORTED_TIMESTAMP_SCOPE),
          "unsupported_timestamp_scope") == 0);
  assert(strcmp(vkr_renderer_impl_gpu_timing_reason_name(
                    VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_UNAVAILABLE),
                "feedback_unavailable") == 0);
  assert(strcmp(vkr_renderer_impl_gpu_timing_reason_name(
                    VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_ERROR),
                "feedback_error") == 0);
  assert(strcmp(vkr_renderer_impl_gpu_timing_reason_name(
                    (VkrRendererImplGpuTimingReason)UINT32_MAX),
                "unknown") == 0);
  printf("  test_gpu_timing_reason_names PASSED\n");
}

static void test_texture_pressure_budget_hysteresis(void) {
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
  assert(vkr_renderer_texture_pressure_budget(&stats, false_v, &budget,
                                              &active) == true_v);
  assert(active == true_v);
  assert(budget == 200u);

  stats.heap_usage_bytes[0] = 800u;
  assert(vkr_renderer_texture_pressure_budget(&stats, true_v, &budget,
                                              &active) == false_v);
  stats.heap_usage_bytes[0] = 750u;
  assert(vkr_renderer_texture_pressure_budget(&stats, true_v, &budget,
                                              &active) == true_v);
  assert(active == false_v);
  assert(budget == UINT64_MAX);
  printf("  test_texture_pressure_budget_hysteresis PASSED\n");
}

bool32_t run_renderer_impl_tests(void) {
  printf("--- Running renderer implementation tests... ---\n");
  test_renderer_impl_capability_partition();
  test_vulkan_platform_availability();
  test_gpu_timing_reason_names();
  test_texture_pressure_budget_hysteresis();
  printf("--- Renderer implementation tests completed. ---\n");
  return true_v;
}

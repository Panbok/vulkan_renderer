#include "renderer_impl_test.h"

#include "renderer/vkr_renderer_impl.h"

#include <assert.h>
#include <stdio.h>

static void test_renderer_impl_capability_partition(void) {
  printf("  Running test_renderer_impl_capability_partition...\n");
  VkrRendererImpl legacy = {0};
  VkrRendererImpl metal = {0};
  const VkrRendererImplOps legacy_ops = {0};
  const VkrRendererImplOps metal_ops = {0};
  const VkrRendererImplOps bindless_ops = {0};
  const VkrRendererImplStrategies strategies = {
      .legacy_vulkan = &legacy_ops,
      .metal = &metal_ops,
      .bindless_vulkan = &bindless_ops,
  };
  assert(vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_VULKAN,
                                  VKR_PRESENT_TARGET_WINDOWED, &strategies,
                                  &legacy));
  assert(vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_METAL,
                                  VKR_PRESENT_TARGET_OFFSCREEN, &strategies,
                                  &metal));
  assert(legacy.kind == VKR_RENDERER_IMPL_LEGACY_VULKAN);
  assert(legacy.caps.uses_legacy_pipeline_state);
  assert(legacy.ops == &legacy_ops);
  assert(metal.kind == VKR_RENDERER_IMPL_METAL);
  assert(!metal.caps.uses_legacy_pipeline_state);
  assert(metal.ops == &metal_ops);
  assert(metal.caps.present_target_kind == VKR_PRESENT_TARGET_OFFSCREEN);
  assert(metal.caps.present_color_format == VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB);
  assert(metal.caps.frame_in_flight_count == 3u);
  printf("  test_renderer_impl_capability_partition PASSED\n");
}

static void test_bindless_vulkan_platform_availability(void) {
  printf("  Running test_bindless_vulkan_platform_availability...\n");
  VkrRendererImpl impl = {0};
  const VkrRendererImplOps legacy_ops = {0};
  const VkrRendererImplOps metal_ops = {0};
  const VkrRendererImplOps bindless_ops = {0};
  const VkrRendererImplStrategies strategies = {
      .legacy_vulkan = &legacy_ops,
      .metal = &metal_ops,
      .bindless_vulkan = &bindless_ops,
  };
  assert(vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_BINDLESS_VULKAN,
                                  VKR_PRESENT_TARGET_WINDOWED, &strategies,
                                  &impl));
  assert(impl.kind == VKR_RENDERER_IMPL_BINDLESS_VULKAN);
  assert(!impl.caps.uses_legacy_pipeline_state);
  assert(impl.ops == &bindless_ops);
#if defined(_WIN32)
  assert(impl.initialization_supported);
#else
  assert(!impl.initialization_supported);
#endif
  assert(!vkr_renderer_impl_select(VKR_RENDERER_BACKEND_TYPE_DX12,
                                   VKR_PRESENT_TARGET_WINDOWED, &strategies,
                                   &impl));
  printf("  test_bindless_vulkan_platform_availability PASSED\n");
}

bool32_t run_renderer_impl_tests(void) {
  printf("--- Running renderer implementation tests... ---\n");
  test_renderer_impl_capability_partition();
  test_bindless_vulkan_platform_availability();
  printf("--- Renderer implementation tests completed. ---\n");
  return true_v;
}

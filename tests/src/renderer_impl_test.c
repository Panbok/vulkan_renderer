#include "renderer_impl_test.h"

#include "renderer/vkr_renderer_impl.h"

#include <assert.h>
#include <stdio.h>

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

bool32_t run_renderer_impl_tests(void) {
  printf("--- Running renderer implementation tests... ---\n");
  test_renderer_impl_capability_partition();
  test_vulkan_platform_availability();
  printf("--- Renderer implementation tests completed. ---\n");
  return true_v;
}

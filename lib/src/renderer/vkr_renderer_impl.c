#include "renderer/vkr_renderer_impl.h"

static VkrRendererImplCapabilities
vkr_renderer_impl_bindless_caps(VkrPresentTargetKind target_kind) {
  return (VkrRendererImplCapabilities){
      .uses_legacy_pipeline_state = false_v,
      .renderer_arena_size = MB(64),
      .scratch_arena_size = MB(32),
      .scratch_arena_block_size = MB(1),
      .frame_in_flight_count = 3u,
      .present_target_image_count = 3u,
      .present_target_kind = target_kind,
      .present_color_format = target_kind == VKR_PRESENT_TARGET_OFFSCREEN
                                  ? VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB
                                  : VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB,
      .present_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
  };
}

bool8_t vkr_renderer_impl_select(VkrRendererBackendType backend_type,
                                 VkrPresentTargetKind target_kind,
                                 const VkrRendererImplStrategies *strategies,
                                 VkrRendererImpl *out_impl) {
  if (!strategies || !out_impl) {
    return false_v;
  }

  *out_impl = (VkrRendererImpl){0};
  switch (backend_type) {
  case VKR_RENDERER_BACKEND_TYPE_VULKAN:
    *out_impl = (VkrRendererImpl){
        .kind = VKR_RENDERER_IMPL_LEGACY_VULKAN,
        .ops = strategies->legacy_vulkan,
        .caps =
            {
                .uses_legacy_pipeline_state = true_v,
                .renderer_arena_size = MB(6),
                .scratch_arena_size = MB(1),
                .scratch_arena_block_size = KB(8),
                .frame_in_flight_count = 1u,
                .present_target_image_count = 0u,
                .present_target_kind = target_kind,
                .present_color_format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,
                .present_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
                .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
            },
        .initialization_supported = true_v,
    };
    return true_v;

  case VKR_RENDERER_BACKEND_TYPE_METAL:
    *out_impl = (VkrRendererImpl){
        .kind = VKR_RENDERER_IMPL_METAL,
        .ops = strategies->metal,
        .caps = vkr_renderer_impl_bindless_caps(target_kind),
#if defined(PLATFORM_APPLE)
        .initialization_supported = true_v,
#else
        .initialization_supported = false_v,
#endif
    };
    return true_v;

  case VKR_RENDERER_BACKEND_TYPE_BINDLESS_VULKAN:
    *out_impl = (VkrRendererImpl){
        .kind = VKR_RENDERER_IMPL_BINDLESS_VULKAN,
        .ops = strategies->bindless_vulkan,
        .caps = vkr_renderer_impl_bindless_caps(target_kind),
        .initialization_supported = false_v,
    };
    return true_v;

  case VKR_RENDERER_BACKEND_TYPE_DX12:
  case VKR_RENDERER_BACKEND_TYPE_COUNT:
    return false_v;
  }

  return false_v;
}

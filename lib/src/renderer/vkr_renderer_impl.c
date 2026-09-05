#include "renderer/vkr_renderer_impl.h"

/**
 * Seed values only. Selection runs before any renderer exists, so anything the
 * chosen implementation actually determines is corrected during its
 * initialization. `frame_in_flight_count` in particular is a per-backend fact:
 * Vulkan builds VKR_VULKAN_FRAME_SLOT_COUNT slots and Metal builds fewer, so
 * both restate it after creating their renderer. Do not read these as the
 * shipped values.
 */
static VkrRendererImplCapabilities
vkr_renderer_impl_default_caps(VkrPresentTargetKind target_kind) {
  return (VkrRendererImplCapabilities){
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
                                 VkrRendererImpl *out_impl) {
  if (!out_impl) {
    return false_v;
  }

  *out_impl = (VkrRendererImpl){0};
  switch (backend_type) {
  case VKR_RENDERER_BACKEND_TYPE_VULKAN:
    *out_impl = (VkrRendererImpl){
        .kind = VKR_RENDERER_IMPL_VULKAN,
        .caps = vkr_renderer_impl_default_caps(target_kind),
#if defined(PLATFORM_WINDOWS)
        .initialization_supported = true_v,
#else
        .initialization_supported = false_v,
#endif
    };
    return true_v;

  case VKR_RENDERER_BACKEND_TYPE_METAL:
    *out_impl = (VkrRendererImpl){
        .kind = VKR_RENDERER_IMPL_METAL,
        .caps = vkr_renderer_impl_default_caps(target_kind),
#if defined(PLATFORM_APPLE)
        .initialization_supported = true_v,
#else
        .initialization_supported = false_v,
#endif
    };
    return true_v;

  case VKR_RENDERER_BACKEND_TYPE_DX12:
  case VKR_RENDERER_BACKEND_TYPE_COUNT:
    return false_v;
  }

  return false_v;
}

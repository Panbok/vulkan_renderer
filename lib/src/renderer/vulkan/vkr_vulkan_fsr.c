#include "renderer/vulkan/vkr_vulkan_fsr_sdk.h"
#include "renderer/vulkan/vkr_vulkan_internal.h"

bool8_t vkr_vk_prepare_fsr31_context(VkrVulkanRenderer *renderer) {
  const VkrRenderGraphFrameInfo *frame = &renderer->prepared_frame;
  if (!frame->scene_rendering || !frame->fsr31_enabled)
    return true_v;

#if !VKR_HAS_FSR3_UPSCALER
  log_error("FSR 3.1 upscaling is unavailable in this Vulkan build");
  return false_v;
#else
  if (renderer->fsr31 &&
      (renderer->fsr31_recreate ||
       renderer->fsr31_output_width != frame->scene_output_width ||
       renderer->fsr31_output_height != frame->scene_output_height)) {
    // SDK history, descriptors and imported views outlive individual frames.
    // An output resize or cancelled SDK recording retires the whole context.
    if (!vkr_vulkan_renderer_wait_idle(renderer))
      return false_v;
    vkr_vulkan_fsr_sdk_destroy(renderer->fsr31);
    renderer->fsr31 = NULL;
  }
  if (!renderer->fsr31) {
    const VkExtent2D extent = {frame->scene_output_width,
                               frame->scene_output_height};
    const VkrVulkanFsrSdkConfig config = {
        .allocator = renderer->allocator,
        .device = vkr_vk_renderer_device(renderer),
        .physical_device = vkr_vulkan_device_physical(renderer->device),
        .get_device_proc_addr = vkGetDeviceProcAddr,
        .max_render_extent = extent,
        .max_output_extent = extent,
    };
    if (vkr_vulkan_fsr_sdk_create(&config, &renderer->fsr31) !=
        VKR_VULKAN_FSR_SDK_SUCCESS) {
      log_error("Vulkan failed to create the FSR 3.1 context (%ux%u)",
                extent.width, extent.height);
      return false_v;
    }
    renderer->fsr31_output_width = extent.width;
    renderer->fsr31_output_height = extent.height;
    renderer->fsr31_history = (VkrVulkanFsr31History){0};
    renderer->fsr31_dispatch_slot = 0u;
    MemZero(renderer->fsr31_dispatch_uses,
            sizeof(renderer->fsr31_dispatch_uses));
    renderer->fsr31_recreate = false_v;
    log_info("FSR 3.1.4 Vulkan context: output %ux%u", extent.width,
             extent.height);
  }

  const uint64_t use =
      renderer->fsr31_dispatch_uses[renderer->fsr31_dispatch_slot];
  if (use > vkr_vk_refresh_completed(renderer)) {
    const VkSemaphoreWaitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1u,
        .pSemaphores = &renderer->timeline,
        .pValues = &use,
    };
    if (vkWaitSemaphores(vkr_vk_renderer_device(renderer), &wait, UINT64_MAX) !=
        VK_SUCCESS)
      return false_v;
    vkr_vk_refresh_completed(renderer);
  }
  return true_v;
#endif
}

void vkr_vk_cancel_fsr31(VkrVulkanRenderer *renderer) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (slot->fsr31_recorded) {
    // SDK dispatch mutates CPU-side image states and ping-pong indices. A
    // reset flag alone cannot roll back unsubmitted layout transitions.
    renderer->fsr31_recreate = true_v;
    renderer->fsr31_history.valid = false_v;
    slot->fsr31_recorded = false_v;
  }
}

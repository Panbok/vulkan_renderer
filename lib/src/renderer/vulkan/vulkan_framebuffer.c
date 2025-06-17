#include "vulkan_framebuffer.h"
#include "defines.h"

bool8_t vulkan_framebuffer_create(VulkanBackendState *state,
                                  VulkanRenderPass *renderpass, uint32_t width,
                                  uint32_t height,
                                  Array_VkImageView *attachments,
                                  VulkanFramebuffer *out_framebuffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(renderpass != NULL, "Renderpass is NULL");
  assert_log(out_framebuffer != NULL, "Out framebuffer is NULL");
  assert_log(attachments != NULL, "Attachments is NULL");
  assert_log(attachments->length > 0,
             "Attachments must have at least 1 attachment");

  VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = renderpass->handle,
      .attachmentCount = attachments->length,
      .pAttachments = attachments->data,
      .width = width,
      .height = height,
      .layers = 1,
  };

  VkFramebuffer framebuffer;
  if (vkCreateFramebuffer(state->device.logical_device, &framebuffer_info,
                          state->allocator, &framebuffer) != VK_SUCCESS) {
    log_fatal("Failed to create framebuffer");
    return false_v;
  }

  out_framebuffer->handle = framebuffer;
  out_framebuffer->renderpass = renderpass;

  log_debug("Created Vulkan framebuffer: %p", framebuffer);

  return true_v;
}

void vulkan_framebuffer_destroy(VulkanBackendState *state,
                                VulkanFramebuffer *framebuffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(framebuffer != NULL, "Framebuffer is NULL");
  assert_log(framebuffer->handle != VK_NULL_HANDLE,
             "Framebuffer handle is NULL");

  log_debug("Destroy Vulkan framebuffer: %p", framebuffer->handle);

  if (framebuffer->attachments.length > 0) {
    array_destroy_VkImageView(&framebuffer->attachments);
  }

  vkDestroyFramebuffer(state->device.logical_device, framebuffer->handle,
                       state->allocator);
}

bool32_t vulkan_framebuffer_regenerate(VulkanBackendState *state,
                                       VulkanSwapchain *swapchain,
                                       VulkanRenderPass *renderpass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(swapchain != NULL, "Swapchain is NULL");
  assert_log(renderpass != NULL, "Renderpass is NULL");

  for (uint32_t i = 0; i < swapchain->image_count; ++i) {
    VulkanFramebuffer *framebuffer =
        array_get_VulkanFramebuffer(&swapchain->framebuffers, i);

    framebuffer->attachments = array_create_VkImageView(
        state->swapchain_arena, 2); // todo: make this dynamic

    // Only use 2 attachments to match the render pass setup
    array_set_VkImageView(&framebuffer->attachments, 0,
                          *array_get_VkImageView(&swapchain->image_views, i));
    array_set_VkImageView(&framebuffer->attachments, 1,
                          swapchain->depth_attachment.view);

    if (!vulkan_framebuffer_create(state, renderpass, swapchain->extent.width,
                                   swapchain->extent.height,
                                   &framebuffer->attachments, framebuffer)) {
      log_fatal("Failed to create Vulkan framebuffer");
      return false;
    }
  }

  return true;
}
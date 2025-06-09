#include "vulkan_framebuffer.h"

bool8_t vulkan_framebuffer_create(VulkanBackendState *state,
                                  VkImageView *image_view,
                                  VkFramebuffer *out_framebuffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image_view != NULL, "Image view is NULL");
  assert_log(out_framebuffer != NULL, "Out framebuffer is NULL");

  VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = state->main_render_pass->handle,
      .attachmentCount = 1,
      .pAttachments = image_view,
      .width = state->swapChainExtent.width,
      .height = state->swapChainExtent.height,
      .layers = 1,
  };

  VkFramebuffer framebuffer;
  if (vkCreateFramebuffer(state->device, &framebuffer_info, NULL,
                          &framebuffer) != VK_SUCCESS) {
    log_fatal("Failed to create framebuffer");
    return false_v;
  }

  *out_framebuffer = framebuffer;

  log_debug("Created Vulkan framebuffer: %p", framebuffer);

  return true_v;
}

void vulkan_framebuffer_destroy(VulkanBackendState *state,
                                VkFramebuffer *framebuffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(framebuffer != VK_NULL_HANDLE, "Framebuffer is NULL");

  log_debug("Destroy Vulkan framebuffer");

  vkDestroyFramebuffer(state->device, *framebuffer, NULL);
}
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

  if (framebuffer->handle == VK_NULL_HANDLE) {
    return;
  }

  log_debug("Destroy Vulkan framebuffer: %p", framebuffer->handle);

  if (framebuffer->attachments.length > 0) {
    array_destroy_VkImageView(&framebuffer->attachments);
  }

  vkDestroyFramebuffer(state->device.logical_device, framebuffer->handle,
                       state->allocator);
}

bool32_t vulkan_framebuffer_regenerate_for_domain(
    VulkanBackendState *state, VulkanSwapchain *swapchain,
    VulkanRenderPass *renderpass, VkrPipelineDomain domain,
    Array_VulkanFramebuffer *framebuffers) {
  assert_log(state != NULL, "State is NULL");
  assert_log(swapchain != NULL, "Swapchain is NULL");
  assert_log(renderpass != NULL, "Renderpass is NULL");
  assert_log(framebuffers != NULL, "Framebuffers is NULL");

  uint32_t attachment_count;
  bool use_color = false;
  bool use_depth = false;

  switch (domain) {
  case VKR_PIPELINE_DOMAIN_WORLD:
  case VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT:
    attachment_count = 2;
    use_color = true;
    use_depth = true;
    break;

  case VKR_PIPELINE_DOMAIN_UI:
    attachment_count = 1;
    use_color = true;
    use_depth = false;
    break;

  case VKR_PIPELINE_DOMAIN_SHADOW:
    attachment_count = 1;
    use_color = false;
    use_depth = true;
    break;

  case VKR_PIPELINE_DOMAIN_POST:
    attachment_count = 1;
    use_color = true;
    use_depth = false;
    break;

  case VKR_PIPELINE_DOMAIN_COMPUTE:
    log_warn("COMPUTE domain doesn't use traditional framebuffers");
    return true;

  case VKR_PIPELINE_DOMAIN_SKYBOX:
    // Skybox uses color and depth (same as WORLD)
    attachment_count = 2;
    use_color = true;
    use_depth = true;
    break;

  default:
    log_fatal("Unknown pipeline domain: %d", domain);
    return false;
  }

  for (uint32_t i = 0; i < swapchain->image_count; ++i) {
    VulkanFramebuffer *framebuffer =
        array_get_VulkanFramebuffer(framebuffers, i);

    framebuffer->attachments =
        array_create_VkImageView(state->swapchain_arena, attachment_count);

    uint32_t attachment_index = 0;

    if (use_color) {
      array_set_VkImageView(&framebuffer->attachments, attachment_index,
                            *array_get_VkImageView(&swapchain->image_views, i));
      attachment_index++;
    }

    if (use_depth) {
      array_set_VkImageView(&framebuffer->attachments, attachment_index,
                            swapchain->depth_attachment.view);
      attachment_index++;
    }

    if (!vulkan_framebuffer_create(state, renderpass, swapchain->extent.width,
                                   swapchain->extent.height,
                                   &framebuffer->attachments, framebuffer)) {
      log_fatal("Failed to create Vulkan framebuffer for domain %d", domain);
      return false;
    }
  }

  log_debug("Created framebuffers for domain %d with %d attachments", domain,
            attachment_count);
  return true;
}
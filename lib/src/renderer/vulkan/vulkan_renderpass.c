#include "vulkan_renderpass.h"

bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 struct s_GraphicsPipeline *pipeline) {
  assert_log(state != NULL, "State is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  VkAttachmentDescription color_attachment = {
      .format = state->swapChainImageFormat,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
  };

  VkAttachmentReference color_attachment_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment_ref,
  };

  VkSubpassDependency deps[2] = {
      {.srcSubpass = VK_SUBPASS_EXTERNAL,
       .dstSubpass = 0,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .srcAccessMask = 0,
       .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT},
      {.srcSubpass = 0,
       .dstSubpass = VK_SUBPASS_EXTERNAL,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
       .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dstAccessMask = 0,
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT}};

  VkRenderPassCreateInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &color_attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = ArrayCount(deps),
      .pDependencies = deps,
  };

  VkRenderPass render_pass;
  if (vkCreateRenderPass(state->device, &render_pass_info, NULL,
                         &render_pass) != VK_SUCCESS) {
    log_fatal("Failed to create render pass");
    return false_v;
  }

  pipeline->render_pass = render_pass;

  return true_v;
}

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               struct s_GraphicsPipeline *pipeline) {
  assert_log(state != NULL, "State is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  if (pipeline->render_pass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(state->device, pipeline->render_pass, NULL);
    pipeline->render_pass = VK_NULL_HANDLE;
  }
}
#include "vulkan_renderpass.h"
#include "defines.h"

// todo: for now we are only supporting a single render pass (main render pass),
// but we should support multiple render passes in the future
bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 VulkanRenderPass *out_render_pass, float32_t x,
                                 float32_t y, float32_t w, float32_t h,
                                 float32_t r, float32_t g, float32_t b,
                                 float32_t a, float32_t depth,
                                 uint32_t stencil) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  out_render_pass->state = RENDER_PASS_STATE_NOT_ALLOCATED;
  out_render_pass->handle = VK_NULL_HANDLE;

  out_render_pass->x = x;
  out_render_pass->y = y;

  out_render_pass->width = w;
  out_render_pass->height = h;

  out_render_pass->r = r;
  out_render_pass->g = g;
  out_render_pass->b = b;
  out_render_pass->a = a;

  out_render_pass->depth = depth;
  out_render_pass->stencil = stencil;

  const uint32_t attachment_description_count = 2; // todo: make this dynamic
  VkAttachmentDescription attachment_descriptions[attachment_description_count];

  VkAttachmentDescription color_attachment = {
      .format = state->swapchain.format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
  };

  attachment_descriptions[0] = color_attachment;

  VkAttachmentReference color_attachment_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkAttachmentDescription depth_attachment = {
      .format = state->device.depth_format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkAttachmentReference depth_attachment_ref = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,

  };

  attachment_descriptions[1] = depth_attachment;

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment_ref,
      .pDepthStencilAttachment = &depth_attachment_ref,
  };

  VkSubpassDependency deps[2] = {
      {.srcSubpass = VK_SUBPASS_EXTERNAL,
       .dstSubpass = 0,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .srcAccessMask = 0,
       .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT},
      {.srcSubpass = 0,
       .dstSubpass = VK_SUBPASS_EXTERNAL,
       .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
       .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       .dstAccessMask = 0,
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT}};

  VkRenderPassCreateInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = attachment_description_count,
      .pAttachments = attachment_descriptions,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = ArrayCount(deps),
      .pDependencies = deps,
  };

  if (vkCreateRenderPass(state->device.logical_device, &render_pass_info,
                         state->allocator,
                         &out_render_pass->handle) != VK_SUCCESS) {
    log_fatal("Failed to create render pass");
    return false_v;
  }

  out_render_pass->state = RENDER_PASS_STATE_READY;

  log_debug("Created Vulkan render pass: %p", out_render_pass->handle);

  return true_v;
}

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               VulkanRenderPass *render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(render_pass != NULL, "Render pass is NULL");

  log_debug("Destroying Vulkan render pass");

  vkDestroyRenderPass(state->device.logical_device, render_pass->handle,
                      state->allocator);

  render_pass->handle = VK_NULL_HANDLE;
  render_pass->state = RENDER_PASS_STATE_NOT_ALLOCATED;
}

bool8_t vulkan_renderpass_begin(VulkanCommandBuffer *command_buffer,
                                VulkanRenderPass *render_pass,
                                VkFramebuffer framebuffer) {
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(render_pass != NULL, "Render pass is NULL");

  VkClearValue clear_values[2];
  clear_values[0].color.float32[0] = render_pass->r;
  clear_values[0].color.float32[1] = render_pass->g;
  clear_values[0].color.float32[2] = render_pass->b;
  clear_values[0].color.float32[3] = render_pass->a;
  clear_values[1].depthStencil.depth = render_pass->depth;
  clear_values[1].depthStencil.stencil = render_pass->stencil;

  VkRenderPassBeginInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass->handle,
      .framebuffer = framebuffer,
      .renderArea = {.offset = {(int32_t)render_pass->x,
                                (int32_t)render_pass->y},
                     .extent = {(uint32_t)render_pass->width,
                                (uint32_t)render_pass->height}},
      .clearValueCount = 2,
      .pClearValues = clear_values,
  };

  vkCmdBeginRenderPass(command_buffer->handle, &render_pass_info,
                       VK_SUBPASS_CONTENTS_INLINE);

  render_pass->state = RENDER_PASS_STATE_RECORDING;

  return true_v;
}

bool8_t vulkan_renderpass_end(VulkanCommandBuffer *command_buffer) {
  assert_log(command_buffer != NULL, "Command buffer is NULL");

  vkCmdEndRenderPass(command_buffer->handle);

  command_buffer->state = COMMAND_BUFFER_STATE_RECORDING;

  return true_v;
}
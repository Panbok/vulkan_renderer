#include "vulkan_renderpass.h"
#include "defines.h"

// todo: for now we are only supporting a single render pass (main render pass),
// but we should support multiple render passes in the future
bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 VulkanRenderPass *out_render_pass,
                                 Vec2 position, Vec4 color, float32_t w,
                                 float32_t h, float32_t depth,
                                 uint32_t stencil) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  out_render_pass->state = RENDER_PASS_STATE_NOT_ALLOCATED;
  out_render_pass->handle = VK_NULL_HANDLE;

  out_render_pass->position = position;
  out_render_pass->color = color;

  out_render_pass->width = w;
  out_render_pass->height = h;

  out_render_pass->depth = depth;
  out_render_pass->stencil = stencil;
  out_render_pass->domain = VKR_PIPELINE_DOMAIN_WORLD; // Default to WORLD

  VkAttachmentDescription attachment_descriptions[2] = {0};

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
      .attachmentCount = 2,
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

bool8_t vulkan_renderpass_create_for_domain(VulkanBackendState *state,
                                            VkrPipelineDomain domain,
                                            VulkanRenderPass *out_render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  out_render_pass->state = RENDER_PASS_STATE_NOT_ALLOCATED;
  out_render_pass->handle = VK_NULL_HANDLE;
  out_render_pass->position = (Vec2){0.0f, 0.0f};
  out_render_pass->color = (Vec4){0.0f, 0.0f, 0.2f, 1.0f};
  out_render_pass->width = (float32_t)state->swapchain.extent.width;
  out_render_pass->height = (float32_t)state->swapchain.extent.height;
  out_render_pass->depth = 1.0f;
  out_render_pass->stencil = 0;
  out_render_pass->domain = domain; // Set the domain

  switch (domain) {
  case VKR_PIPELINE_DOMAIN_WORLD:
    return vulkan_renderpass_create_world(state, out_render_pass);

  case VKR_PIPELINE_DOMAIN_UI:
    return vulkan_renderpass_create_ui(state, out_render_pass);

  case VKR_PIPELINE_DOMAIN_SHADOW:
    return vulkan_renderpass_create_shadow(state, out_render_pass);

  case VKR_PIPELINE_DOMAIN_POST:
    return vulkan_renderpass_create_post(state, out_render_pass);

  case VKR_PIPELINE_DOMAIN_COMPUTE:
    log_warn("Compute domain doesn't use traditional render passes");
    return true;

  default:
    log_fatal("Unknown pipeline domain: %d", domain);
    return false;
  }
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
  uint32_t clear_value_count = 0;

  switch (render_pass->domain) {
  case VKR_PIPELINE_DOMAIN_WORLD:
    // Color + depth attachments
    clear_values[0].color.float32[0] = render_pass->color.r;
    clear_values[0].color.float32[1] = render_pass->color.g;
    clear_values[0].color.float32[2] = render_pass->color.b;
    clear_values[0].color.float32[3] = render_pass->color.a;
    clear_values[1].depthStencil.depth = render_pass->depth;
    clear_values[1].depthStencil.stencil = render_pass->stencil;
    clear_value_count = 2;
    break;

  case VKR_PIPELINE_DOMAIN_UI:
  case VKR_PIPELINE_DOMAIN_POST:
    // Color attachment only (UI doesn't clear, POST does clear)
    clear_values[0].color.float32[0] = render_pass->color.r;
    clear_values[0].color.float32[1] = render_pass->color.g;
    clear_values[0].color.float32[2] = render_pass->color.b;
    clear_values[0].color.float32[3] = render_pass->color.a;
    clear_value_count = 1;
    break;

  case VKR_PIPELINE_DOMAIN_SHADOW:
    // Depth attachment only
    clear_values[0].depthStencil.depth = render_pass->depth;
    clear_values[0].depthStencil.stencil = render_pass->stencil;
    clear_value_count = 1;
    break;

  case VKR_PIPELINE_DOMAIN_COMPUTE:
    log_warn("COMPUTE domain doesn't use traditional render pass begin");
    return true_v;

  default:
    log_fatal("Unknown render pass domain: %d", render_pass->domain);
    return false;
  }

  VkRenderPassBeginInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass->handle,
      .framebuffer = framebuffer,
      .renderArea = {.offset = {(int32_t)render_pass->position.x,
                                (int32_t)render_pass->position.y},
                     .extent = {(uint32_t)render_pass->width,
                                (uint32_t)render_pass->height}},
      .clearValueCount = clear_value_count,
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

// =========================================================================
// Domain-specific render pass creation functions
// =========================================================================

/**
 * @brief Creates a render pass for the WORLD domain
 * @details World domain: color + depth attachments
 * @details Color attachment: CLEAR → COLOR_ATTACHMENT_OPTIMAL (for sharing with
 * UI)
 * @details Depth attachment: CLEAR → DEPTH_STENCIL_ATTACHMENT_OPTIMAL
 * @param state The Vulkan backend state
 * @param out_render_pass The render pass to create
 * @return true if the render pass was created successfully, false otherwise
 */
bool8_t vulkan_renderpass_create_world(VulkanBackendState *state,
                                       VulkanRenderPass *out_render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  VkAttachmentDescription attachments[2] = {0};

  // Color attachment
  attachments[0] = (VkAttachmentDescription){
      .format = state->swapchain.format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  // Depth attachment
  attachments[1] = (VkAttachmentDescription){
      .format = state->device.depth_format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkAttachmentReference depth_ref = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = &depth_ref,
  };

  VkSubpassDependency dependencies[2] = {
      // External → subpass 0 (start of render pass)
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
      // subpass 0 → external (end of render pass, transition for UI pass)
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      }};

  VkRenderPassCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = dependencies,
  };

  if (vkCreateRenderPass(state->device.logical_device, &create_info,
                         state->allocator,
                         &out_render_pass->handle) != VK_SUCCESS) {
    log_fatal("Failed to create WORLD domain render pass");
    return false;
  }

  out_render_pass->state = RENDER_PASS_STATE_READY;
  log_debug("Created WORLD domain render pass: %p", out_render_pass->handle);
  return true;
}

/**
 * @brief Creates a render pass for the UI domain
 * @details UI domain: color attachment only
 * @details Color attachment: LOAD → PRESENT_SRC_KHR (preserve world contents,
 * final output) No depth attachment (UI renders on top)
 * @param state The Vulkan backend state
 * @param out_render_pass The render pass to create
 * @return true if the render pass was created successfully, false otherwise
 */
bool8_t vulkan_renderpass_create_ui(VulkanBackendState *state,
                                    VulkanRenderPass *out_render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  VkAttachmentDescription attachment = {
      .format = state->swapchain.format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, // Preserve world rendering
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, // From world
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,            // Final output
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = NULL, // No depth for UI
  };

  VkSubpassDependency dependencies[2] = {
      // External (world pass) → UI subpass
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
      // UI subpass → external (present)
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = 0,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      }};

  VkRenderPassCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = dependencies,
  };

  if (vkCreateRenderPass(state->device.logical_device, &create_info,
                         state->allocator,
                         &out_render_pass->handle) != VK_SUCCESS) {
    log_fatal("Failed to create UI domain render pass");
    return false;
  }

  out_render_pass->state = RENDER_PASS_STATE_READY;
  log_debug("Created UI domain render pass: %p", out_render_pass->handle);
  return true;
}

/**
 * @brief Creates a render pass for the SHADOW domain
 * @details SHADOW domain: depth attachment only
 * @details Depth attachment: CLEAR → DEPTH_STENCIL_ATTACHMENT_OPTIMAL (for
 * shadow mapping) No color attachment
 * @param state The Vulkan backend state
 * @param out_render_pass The render pass to create
 * @return true if the render pass was created successfully, false otherwise
 */
bool8_t vulkan_renderpass_create_shadow(VulkanBackendState *state,
                                        VulkanRenderPass *out_render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  VkAttachmentDescription attachment = {
      .format = state->device.depth_format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE, // Store for shadow map texture
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout =
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, // For sampling
  };

  VkAttachmentReference depth_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 0,
      .pColorAttachments = NULL,
      .pDepthStencilAttachment = &depth_ref,
  };

  VkSubpassDependency dependencies[2] = {
      // External → shadow subpass
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
      // Shadow subpass → external (for sampling)
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      }};

  VkRenderPassCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = dependencies,
  };

  if (vkCreateRenderPass(state->device.logical_device, &create_info,
                         state->allocator,
                         &out_render_pass->handle) != VK_SUCCESS) {
    log_fatal("Failed to create SHADOW domain render pass");
    return false;
  }

  out_render_pass->state = RENDER_PASS_STATE_READY;
  log_debug("Created SHADOW domain render pass: %p", out_render_pass->handle);
  return true;
}

/**
 * @brief Creates a render pass for the POST domain
 * @details POST domain: color attachment only
 * @details Color attachment: CLEAR → PRESENT_SRC_KHR (final output) No depth
 * attachment (post-processing is screen-space)
 * @param state The Vulkan backend state
 * @param out_render_pass The render pass to create
 * @return true if the render pass was created successfully, false otherwise
 */
bool8_t vulkan_renderpass_create_post(VulkanBackendState *state,
                                      VulkanRenderPass *out_render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  VkAttachmentDescription attachment = {
      .format = state->swapchain.format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, // Final output
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = NULL, // No depth for post-processing
  };

  VkSubpassDependency dependencies[2] = {
      // External → post subpass
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
      // Post subpass → external (present)
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = 0,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      }};

  VkRenderPassCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = dependencies,
  };

  if (vkCreateRenderPass(state->device.logical_device, &create_info,
                         state->allocator,
                         &out_render_pass->handle) != VK_SUCCESS) {
    log_fatal("Failed to create POST domain render pass");
    return false;
  }

  out_render_pass->state = RENDER_PASS_STATE_READY;
  log_debug("Created POST domain render pass: %p", out_render_pass->handle);
  return true;
}
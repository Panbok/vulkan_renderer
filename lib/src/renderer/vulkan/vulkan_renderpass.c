#include "vulkan_renderpass.h"
#include "defines.h"

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

bool8_t
vulkan_renderpass_create_from_config(VulkanBackendState *state,
                                     const VkrRenderPassConfig *cfg,
                                     VulkanRenderPass *out_render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");
  assert_log(cfg != NULL, "Config is NULL");

  MemZero(out_render_pass, sizeof(VulkanRenderPass));
  out_render_pass->state = RENDER_PASS_STATE_NOT_ALLOCATED;
  out_render_pass->handle = VK_NULL_HANDLE;

  out_render_pass->position = (Vec2){cfg->render_area.x, cfg->render_area.y};
  out_render_pass->width = (cfg->render_area.z > 0.0f)
                               ? cfg->render_area.z
                               : (float32_t)state->swapchain.extent.width;
  out_render_pass->height = (cfg->render_area.w > 0.0f)
                                ? cfg->render_area.w
                                : (float32_t)state->swapchain.extent.height;
  out_render_pass->color = cfg->clear_color;
  out_render_pass->depth = 1.0f;
  out_render_pass->stencil = 0;
  out_render_pass->domain = cfg->domain;

  bool use_depth = (cfg->clear_flags &
                    (VKR_RENDERPASS_CLEAR_DEPTH | VKR_RENDERPASS_CLEAR_STENCIL |
                     VKR_RENDERPASS_USE_DEPTH)) != 0;

  VkAttachmentDescription attachments[2] = {0};
  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  bool8_t has_prev_pass = cfg->prev_name.length > 0;
  bool8_t has_next_pass = cfg->next_name.length > 0;

  VkAttachmentDescription color_attachment = {
      .format = state->swapchain.format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = (cfg->clear_flags & VKR_RENDERPASS_CLEAR_COLOR)
                    ? VK_ATTACHMENT_LOAD_OP_CLEAR
                    : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = has_prev_pass ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = has_next_pass ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                   : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
  };

  attachments[0] = color_attachment;

  VkAttachmentReference depth_ref = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,

  };

  uint32_t attachment_count = 1;
  if (use_depth) {
    // If loading depth (not clearing), initialLayout must not be UNDEFINED
    bool clear_depth = (cfg->clear_flags & VKR_RENDERPASS_CLEAR_DEPTH) != 0;
    VkImageLayout depth_initial_layout =
        (clear_depth || !has_prev_pass)
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depth_attachment = {
        .format = state->device.depth_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR
                              : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = has_next_pass ? VK_ATTACHMENT_STORE_OP_STORE
                                 : VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = (cfg->clear_flags & VKR_RENDERPASS_CLEAR_STENCIL)
                             ? VK_ATTACHMENT_LOAD_OP_CLEAR
                             : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = depth_initial_layout,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    attachments[attachment_count++] = depth_attachment;
  }

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = use_depth ? &depth_ref : NULL,
  };

  VkSubpassDependency deps[2] = {
      {.srcSubpass = VK_SUBPASS_EXTERNAL,
       .dstSubpass = 0,
       .srcStageMask =
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
           (use_depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0),
       .dstStageMask =
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
           (use_depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0),
       .srcAccessMask = 0,
       .dstAccessMask =
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
           (use_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0),
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT},
      {.srcSubpass = 0,
       .dstSubpass = VK_SUBPASS_EXTERNAL,
       .srcStageMask =
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
           (use_depth ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0),
       .dstStageMask = (cfg->next_name.length > 0)
                           ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                           : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
       .srcAccessMask =
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
           (use_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0),
       .dstAccessMask = (cfg->next_name.length > 0)
                            ? (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                            : 0,
       .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT}};

  VkRenderPassCreateInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = attachment_count,
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = ArrayCount(deps),
      .pDependencies = deps,
  };

  if (vkCreateRenderPass(state->device.logical_device, &render_pass_info,
                         state->allocator,
                         &out_render_pass->handle) != VK_SUCCESS) {
    log_fatal("Failed to create render pass from config");
    return false_v;
  }

  out_render_pass->state = RENDER_PASS_STATE_READY;
  return true_v;
}

/**
 * @brief Create a domain-specific render pass
 *
 * DOMAIN-SPECIFIC RENDER PASS SYSTEM:
 * This function creates render passes tailored to specific rendering domains.
 * Each domain has unique attachment configurations, load/store operations,
 * and image layout transitions optimized for its use case.
 *
 * DOMAIN CONFIGURATIONS:
 *
 * 1. WORLD Domain (3D World Geometry):
 *    - Attachments: Color + Depth
 *    - Load Ops: CLEAR both (fresh frame)
 *    - Color Final Layout: COLOR_ATTACHMENT_OPTIMAL
 *      → Optimized for chaining to UI pass without transition
 *      → If no UI pass: Manual transition to PRESENT in end_frame
 *    - Depth Final Layout: DEPTH_STENCIL_ATTACHMENT_OPTIMAL
 *    - Use Case: Primary 3D scene rendering with depth testing
 *
 * 2. UI Domain (2D User Interface):
 *    - Attachments: Color only (no depth)
 *    - Load Op: LOAD (preserves world rendering underneath)
 *    - Color Initial Layout: COLOR_ATTACHMENT_OPTIMAL (from WORLD)
 *    - Color Final Layout: PRESENT_SRC_KHR (ready for presentation)
 *      → Sets swapchain_image_is_present_ready = true in end
 *      → Eliminates manual transition in end_frame
 *    - Use Case: Render UI elements on top of 3D world
 *
 * 3. SHADOW Domain (Shadow Map Generation):
 *    - Attachments: Depth only (no color)
 *    - Load Op: CLEAR
 *    - Use Case: Render scene from light's perspective to depth texture
 *
 * 4. POST Domain (Post-Processing Effects):
 *    - Attachments: Color only
 *    - Load Op: CLEAR (fresh start for effects)
 *    - Final Layout: PRESENT_SRC_KHR
 *    - Use Case: Fullscreen effects (bloom, tone mapping, etc.)
 *
 * 5. COMPUTE Domain:
 *    - No traditional render pass (uses compute pipelines)
 *    - Use Case: GPU compute operations
 *
 * RENDER PASS CHAINING OPTIMIZATION:
 * The WORLD → UI chain is optimized for zero-cost composition:
 * - WORLD ends in COLOR_ATTACHMENT_OPTIMAL layout
 * - UI starts in COLOR_ATTACHMENT_OPTIMAL layout (no transition)
 * - UI loads existing color data (preserves world rendering)
 * - UI ends in PRESENT_SRC_KHR layout (ready for swapchain present)
 *
 * This eliminates the need for explicit layout transitions between world
 * and UI rendering, reducing GPU overhead for common rendering patterns.
 *
 * @param state Vulkan backend state
 * @param domain Pipeline domain (WORLD, UI, SHADOW, POST, COMPUTE)
 * @param out_render_pass Output render pass object
 * @return true on success, false on failure
 */
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
  out_render_pass->domain = domain; // Store domain for later reference

  switch (domain) {
  case VKR_PIPELINE_DOMAIN_WORLD:
  case VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT:
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

  if (render_pass->handle == VK_NULL_HANDLE) {
    render_pass->state = RENDER_PASS_STATE_NOT_ALLOCATED;
    return;
  }

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
  case VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT:
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

bool8_t vulkan_renderpass_end(VulkanCommandBuffer *command_buffer,
                              VulkanBackendState *state) {
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(state != NULL, "State is NULL");

  vkCmdEndRenderPass(command_buffer->handle);

  // Set the present ready flag if this was a UI or POST render pass
  if (state->current_render_pass_domain == VKR_PIPELINE_DOMAIN_UI ||
      state->current_render_pass_domain == VKR_PIPELINE_DOMAIN_POST) {
    state->swapchain_image_is_present_ready = true;
  }

  command_buffer->state = COMMAND_BUFFER_STATE_RECORDING;

  return true_v;
}

// =========================================================================
// Domain-specific render pass creation functions
// =========================================================================

/**
 * @brief Create render pass for WORLD domain (3D scene geometry)
 *
 * WORLD DOMAIN SPECIFICATION:
 * This render pass is designed for rendering 3D world geometry with depth
 * testing and potential alpha blending for transparent objects.
 *
 * ATTACHMENT CONFIGURATION:
 * - Color Attachment:
 *   - Format: Swapchain format (typically BGRA8_SRGB)
 *   - Load Op: CLEAR (start fresh each frame)
 *   - Store Op: STORE (preserve for UI pass or presentation)
 *   - Initial Layout: UNDEFINED (don't care about previous contents)
 *   - Final Layout: COLOR_ATTACHMENT_OPTIMAL
 *     → CRITICAL: Stays in attachment-optimal layout for efficient UI chaining
 *     → If UI pass runs next: Zero-cost transition (same layout)
 *     → If no UI pass: Manual transition to PRESENT in end_frame
 *
 * - Depth Attachment:
 *   - Format: Device depth format (D32_SFLOAT or D24_UNORM_S8_UINT)
 *   - Load Op: CLEAR (start fresh each frame)
 *   - Store Op: DONT_CARE (depth not needed after world pass)
 *   - Initial Layout: UNDEFINED
 *   - Final Layout: DEPTH_STENCIL_ATTACHMENT_OPTIMAL
 *
 * SUBPASS DEPENDENCIES:
 * 1. External → Subpass 0:
 *    - Ensures previous frame's color/depth writes complete before this pass
 *    - Synchronizes COLOR_ATTACHMENT_OUTPUT and EARLY_FRAGMENT_TESTS stages
 *
 * 2. Subpass 0 → External:
 *    - Allows subsequent passes (UI) to read color attachment
 *    - Synchronizes for WORLD→UI transition
 *
 * PIPELINE COMPATIBILITY:
 * Pipelines using this render pass should have:
 * - Depth testing enabled (depthTestEnable = VK_TRUE)
 * - Depth writing enabled (depthWriteEnable = VK_TRUE)
 * - Blend mode: Typically opaque (blendEnable = VK_FALSE)
 *   - Can enable for transparent objects (per-material)
 *
 * @param state Vulkan backend state
 * @param out_render_pass Output render pass object
 * @return true on success, false on failure
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
 * @brief Create render pass for UI domain (2D user interface overlay)
 *
 * UI DOMAIN SPECIFICATION:
 * This render pass is optimized for rendering 2D UI elements on top of the
 * 3D world geometry. It preserves the world rendering and composites UI with
 * alpha blending.
 *
 * ATTACHMENT CONFIGURATION:
 * - Color Attachment Only (no depth):
 *   - Format: Swapchain format (matches WORLD pass)
 *   - Load Op: LOAD
 *     → CRITICAL: Preserves world rendering underneath
 *     → UI elements are composited on top of existing color data
 *   - Store Op: STORE (save final composed image)
 *   - Initial Layout: COLOR_ATTACHMENT_OPTIMAL
 *     → Matches WORLD final layout (zero-cost transition)
 *     → Efficient chaining from WORLD pass
 *   - Final Layout: PRESENT_SRC_KHR
 *     → Image is ready for swapchain presentation
 *     → Sets swapchain_image_is_present_ready = true
 *     → Eliminates manual transition in end_frame
 *
 * NO DEPTH ATTACHMENT:
 * - UI rendering typically doesn't need depth testing
 * - UI elements are drawn in painter's order (back-to-front)
 * - Omitting depth saves memory bandwidth
 *
 * SUBPASS DEPENDENCIES:
 * 1. External (WORLD pass) → UI Subpass:
 *    - Ensures WORLD color writes complete before UI reads
 *    - Synchronizes COLOR_ATTACHMENT_OUTPUT stages
 *
 * 2. UI Subpass → External (Present):
 *    - Ensures UI writes complete before presentation
 *    - Synchronizes for swapchain present operation
 *
 * PIPELINE COMPATIBILITY:
 * Pipelines using this render pass should have:
 * - Depth testing disabled (depthTestEnable = VK_FALSE)
 * - Depth writing disabled (depthWriteEnable = VK_FALSE)
 * - Alpha blending enabled (blendEnable = VK_TRUE)
 *   - Src factor: VK_BLEND_FACTOR_SRC_ALPHA
 *   - Dst factor: VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
 *   - Blend op: VK_BLEND_OP_ADD
 *
 * TYPICAL RENDERING FLOW:
 * 1. WORLD pass renders 3D geometry (color in COLOR_ATTACHMENT_OPTIMAL)
 * 2. Automatic transition to UI pass (same layout, no GPU cost)
 * 3. UI pass LOADs existing color data
 * 4. UI elements drawn with alpha blending
 * 5. Final image transitioned to PRESENT_SRC_KHR
 * 6. Frame submitted to swapchain for display
 *
 * @param state Vulkan backend state
 * @param out_render_pass Output render pass object
 * @return true on success, false on failure
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

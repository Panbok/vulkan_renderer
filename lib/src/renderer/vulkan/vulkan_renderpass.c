#include "vulkan_renderpass.h"
#include "defines.h"
#include "vulkan_utils.h"

// ============================================================================
// VkrRenderPassDesc Conversion Helpers
// ============================================================================

/**
 * @brief Convert VkrAttachmentLoadOp to Vulkan VkAttachmentLoadOp.
 */
vkr_internal VkAttachmentLoadOp vkr_load_op_to_vk(VkrAttachmentLoadOp op) {
  switch (op) {
  case VKR_ATTACHMENT_LOAD_OP_LOAD:
    return VK_ATTACHMENT_LOAD_OP_LOAD;
  case VKR_ATTACHMENT_LOAD_OP_CLEAR:
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
  case VKR_ATTACHMENT_LOAD_OP_DONT_CARE:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  default:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
}

/**
 * @brief Convert VkrAttachmentStoreOp to Vulkan VkAttachmentStoreOp.
 */
vkr_internal VkAttachmentStoreOp vkr_store_op_to_vk(VkrAttachmentStoreOp op) {
  switch (op) {
  case VKR_ATTACHMENT_STORE_OP_STORE:
    return VK_ATTACHMENT_STORE_OP_STORE;
  case VKR_ATTACHMENT_STORE_OP_DONT_CARE:
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  default:
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
}

/**
 * @brief Convert VkrSampleCount to Vulkan VkSampleCountFlagBits.
 */
vkr_internal VkSampleCountFlagBits
vkr_sample_count_to_vk(VkrSampleCount samples) {
  switch (samples) {
  case VKR_SAMPLE_COUNT_1:
    return VK_SAMPLE_COUNT_1_BIT;
  case VKR_SAMPLE_COUNT_2:
    return VK_SAMPLE_COUNT_2_BIT;
  case VKR_SAMPLE_COUNT_4:
    return VK_SAMPLE_COUNT_4_BIT;
  case VKR_SAMPLE_COUNT_8:
    return VK_SAMPLE_COUNT_8_BIT;
  case VKR_SAMPLE_COUNT_16:
    return VK_SAMPLE_COUNT_16_BIT;
  case VKR_SAMPLE_COUNT_32:
    return VK_SAMPLE_COUNT_32_BIT;
  case VKR_SAMPLE_COUNT_64:
    return VK_SAMPLE_COUNT_64_BIT;
  default:
    return VK_SAMPLE_COUNT_1_BIT;
  }
}

/**
 * @brief Convert VkrTextureLayout to Vulkan VkImageLayout.
 */
vkr_internal VkImageLayout vkr_texture_layout_to_vk(VkrTextureLayout layout) {
  switch (layout) {
  case VKR_TEXTURE_LAYOUT_UNDEFINED:
    return VK_IMAGE_LAYOUT_UNDEFINED;
  case VKR_TEXTURE_LAYOUT_GENERAL:
    return VK_IMAGE_LAYOUT_GENERAL;
  case VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR:
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  default:
    log_warn("Unknown VkrTextureLayout %d, defaulting to UNDEFINED", layout);
    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

// ============================================================================
// VkrRenderPassDesc-based Creation
// ============================================================================

bool8_t vulkan_renderpass_create_from_desc(VulkanBackendState *state,
                                           const VkrRenderPassDesc *desc,
                                           VulkanRenderPass *out_render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(desc != NULL, "Descriptor is NULL");
  assert_log(out_render_pass != NULL, "Out render pass is NULL");

  if (desc->color_attachment_count > 0 && !desc->color_attachments) {
    log_error("Color attachment descriptors missing");
    return false_v;
  }
  if (desc->resolve_attachment_count > 0 && !desc->resolve_attachments) {
    log_error("Resolve attachment descriptors missing");
    return false_v;
  }
  if (desc->color_attachment_count > VKR_MAX_COLOR_ATTACHMENTS) {
    log_error("Too many color attachments: %u (max %u)",
              desc->color_attachment_count, VKR_MAX_COLOR_ATTACHMENTS);
    return false_v;
  }
  if (desc->resolve_attachment_count > desc->color_attachment_count) {
    log_error("Resolve attachment count %u exceeds color attachment count %u",
              desc->resolve_attachment_count, desc->color_attachment_count);
    return false_v;
  }

  VkrSampleCount expected_samples = VKR_SAMPLE_COUNT_1;
  if (desc->color_attachment_count > 0) {
    expected_samples = desc->color_attachments[0].samples;
    for (uint8_t i = 1; i < desc->color_attachment_count; ++i) {
      if (desc->color_attachments[i].samples != expected_samples) {
        log_error("Color attachment sample counts must match");
        return false_v;
      }
    }
  }
  if (desc->depth_stencil_attachment && desc->color_attachment_count > 0 &&
      desc->depth_stencil_attachment->samples != expected_samples) {
    log_error("Depth/stencil sample count must match color attachments");
    return false_v;
  }

  bool8_t resolve_src_used[VKR_MAX_COLOR_ATTACHMENTS] = {0};
  bool8_t resolve_dst_used[VKR_MAX_COLOR_ATTACHMENTS] = {0};
  for (uint8_t i = 0; i < desc->resolve_attachment_count; ++i) {
    uint8_t src = desc->resolve_attachments[i].src_attachment_index;
    uint8_t dst = desc->resolve_attachments[i].dst_attachment_index;
    if (src >= desc->color_attachment_count ||
        dst >= desc->resolve_attachment_count) {
      log_error("Resolve attachment index out of range");
      return false_v;
    }
    if (resolve_src_used[src] || resolve_dst_used[dst]) {
      log_error("Resolve attachment indices must be unique");
      return false_v;
    }
    resolve_src_used[src] = true_v;
    resolve_dst_used[dst] = true_v;
    if (desc->color_attachment_count > 0 &&
        desc->color_attachments[src].samples == VKR_SAMPLE_COUNT_1) {
      log_warn("Resolve attachment %u uses single-sample source", i);
    }
  }

  MemZero(out_render_pass, sizeof(VulkanRenderPass));
  out_render_pass->domain = desc->domain;

  bool8_t has_depth = desc->depth_stencil_attachment != NULL;

  // Build attachment descriptions
  VkAttachmentDescription
      attachments[VKR_MAX_COLOR_ATTACHMENTS + 1 + VKR_MAX_COLOR_ATTACHMENTS];
  VkAttachmentReference color_refs[VKR_MAX_COLOR_ATTACHMENTS];
  VkAttachmentReference resolve_refs[VKR_MAX_COLOR_ATTACHMENTS];
  VkAttachmentReference depth_ref = {0};

  uint32_t attachment_idx = 0;

  // Color attachments
  for (uint8_t i = 0; i < desc->color_attachment_count; i++) {
    const VkrRenderPassAttachmentDesc *att = &desc->color_attachments[i];
    attachments[attachment_idx] = (VkAttachmentDescription){
        .format = vulkan_image_format_from_texture_format(att->format),
        .samples = vkr_sample_count_to_vk(att->samples),
        .loadOp = vkr_load_op_to_vk(att->load_op),
        .storeOp = vkr_store_op_to_vk(att->store_op),
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = vkr_texture_layout_to_vk(att->initial_layout),
        .finalLayout = vkr_texture_layout_to_vk(att->final_layout),
    };
    color_refs[i] = (VkAttachmentReference){
        .attachment = attachment_idx,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    attachment_idx++;
  }

  // Depth/stencil attachment
  uint32_t depth_attachment_idx = VK_ATTACHMENT_UNUSED;
  if (has_depth) {
    const VkrRenderPassAttachmentDesc *att = desc->depth_stencil_attachment;
    depth_attachment_idx = attachment_idx;
    attachments[attachment_idx] = (VkAttachmentDescription){
        .format = vulkan_image_format_from_texture_format(att->format),
        .samples = vkr_sample_count_to_vk(att->samples),
        .loadOp = vkr_load_op_to_vk(att->load_op),
        .storeOp = vkr_store_op_to_vk(att->store_op),
        .stencilLoadOp = vkr_load_op_to_vk(att->stencil_load_op),
        .stencilStoreOp = vkr_store_op_to_vk(att->stencil_store_op),
        .initialLayout = vkr_texture_layout_to_vk(att->initial_layout),
        .finalLayout = vkr_texture_layout_to_vk(att->final_layout),
    };
    depth_ref = (VkAttachmentReference){
        .attachment = depth_attachment_idx,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    attachment_idx++;
  }

  // Resolve attachments (for MSAA)
  uint32_t resolve_base_idx = attachment_idx;
  const VkrResolveAttachmentRef *resolve_order[VKR_MAX_COLOR_ATTACHMENTS] = {0};
  for (uint8_t i = 0; i < desc->resolve_attachment_count; ++i) {
    resolve_order[desc->resolve_attachments[i].dst_attachment_index] =
        &desc->resolve_attachments[i];
  }
  for (uint8_t i = 0; i < desc->resolve_attachment_count; i++) {
    const VkrResolveAttachmentRef *resolve_ref = resolve_order[i];
    if (!resolve_ref) {
      log_error("Resolve attachment ordering incomplete");
      return false_v;
    }
    // Resolve attachments use same format as source color attachment but with 1
    // sample
    uint8_t src_idx = resolve_ref->src_attachment_index;
    const VkrRenderPassAttachmentDesc *src_att =
        &desc->color_attachments[src_idx];
    attachments[attachment_idx] = (VkAttachmentDescription){
        .format = vulkan_image_format_from_texture_format(src_att->format),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = vkr_texture_layout_to_vk(src_att->final_layout),
    };
    attachment_idx++;
  }

  // Build resolve refs array - must match color attachment count with
  // VK_ATTACHMENT_UNUSED for slots without resolve
  for (uint8_t i = 0; i < desc->color_attachment_count; i++) {
    resolve_refs[i].attachment = VK_ATTACHMENT_UNUSED;
    resolve_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  for (uint8_t i = 0; i < desc->resolve_attachment_count; i++) {
    const VkrResolveAttachmentRef *resolve_ref = resolve_order[i];
    uint8_t src_idx = resolve_ref->src_attachment_index;
    resolve_refs[src_idx].attachment = resolve_base_idx + i;
  }

  // Subpass description
  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = desc->color_attachment_count,
      .pColorAttachments = desc->color_attachment_count > 0 ? color_refs : NULL,
      .pDepthStencilAttachment = has_depth ? &depth_ref : NULL,
      .pResolveAttachments =
          desc->resolve_attachment_count > 0 ? resolve_refs : NULL,
  };

  // Subpass dependencies - generic synchronization
  VkSubpassDependency dependencies[2] = {
      // External → subpass 0
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask =
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
              (has_depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0),
          .dstStageMask =
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
              (has_depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0),
          .srcAccessMask = 0,
          .dstAccessMask =
              (desc->color_attachment_count > 0
                   ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                   : 0) |
              (has_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0),
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
      // Subpass 0 → external
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask =
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
              (has_depth ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0),
          .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          .srcAccessMask =
              (desc->color_attachment_count > 0
                   ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                   : 0) |
              (has_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0),
          .dstAccessMask = 0,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
  };

  VkRenderPassCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = attachment_idx,
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = dependencies,
  };

  if (vkCreateRenderPass(state->device.logical_device, &create_info,
                         state->allocator,
                         &out_render_pass->handle) != VK_SUCCESS) {
    log_error("Failed to create render pass from descriptor");
    return false_v;
  }

  // Populate signature from descriptor
  VkrRenderPassSignature *sig = &out_render_pass->signature;
  MemZero(sig, sizeof(*sig));
  sig->color_attachment_count = desc->color_attachment_count;
  for (uint8_t i = 0; i < desc->color_attachment_count; i++) {
    sig->color_formats[i] = desc->color_attachments[i].format;
    sig->color_samples[i] = desc->color_attachments[i].samples;
  }
  sig->has_depth_stencil = has_depth;
  if (has_depth) {
    sig->depth_stencil_format = desc->depth_stencil_attachment->format;
    sig->depth_stencil_samples = desc->depth_stencil_attachment->samples;
  }
  sig->has_resolve_attachments = desc->resolve_attachment_count > 0;
  sig->resolve_attachment_count = desc->resolve_attachment_count;

  log_debug("Created render pass from descriptor: %p (domain=%d, colors=%u, "
            "depth=%d, resolves=%u)",
            out_render_pass->handle, desc->domain, desc->color_attachment_count,
            has_depth, desc->resolve_attachment_count);

  return true_v;
}

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               VulkanRenderPass *render_pass) {
  assert_log(state != NULL, "State is NULL");
  assert_log(render_pass != NULL, "Render pass is NULL");

  if (render_pass->handle == VK_NULL_HANDLE) {
    return;
  }

  vkDestroyRenderPass(state->device.logical_device, render_pass->handle,
                      state->allocator);
  render_pass->handle = VK_NULL_HANDLE;
}

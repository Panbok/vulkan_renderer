#pragma once

#include "defines.h"
#include "vulkan_types.h"

/**
 * @brief Create a render pass from an explicit descriptor.
 *
 * Creates a render pass with full control over attachment configuration
 * including load/store operations, layouts, and MSAA resolve attachments.
 *
 * @param state Vulkan backend state
 * @param desc Render pass descriptor
 * @param out_render_pass Output render pass object
 * @return true on success, false on failure
 */
bool8_t vulkan_renderpass_create_from_desc(VulkanBackendState *state,
                                           const VkrRenderPassDesc *desc,
                                           VulkanRenderPass *out_render_pass);

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               VulkanRenderPass *render_pass);

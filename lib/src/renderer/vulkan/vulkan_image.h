#pragma once

#include "vulkan_backend.h"
#include "vulkan_utils.h"

bool32_t vulkan_image_create(VulkanBackendState *state, VkImageType image_type,
                             uint32_t width, uint32_t height, VkFormat format,
                             VkImageTiling tiling, VkImageUsageFlags usage,
                             VkMemoryPropertyFlags memory_flags,
                             uint32_t mip_levels, uint32_t array_layers,
                             VkImageViewType view_type,
                             VkImageAspectFlags view_aspect_flags,
                             VulkanImage *out_image);

void vulkan_image_destroy(VulkanBackendState *state, VulkanImage *image);

bool8_t vulkan_image_transition_layout(VulkanBackendState *state,
                                       VulkanImage *image,
                                       VulkanCommandBuffer *command_buffer,
                                       VkFormat format,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout);
bool8_t vulkan_image_transition_layout_range(
    VulkanBackendState *state, VulkanImage *image,
    VulkanCommandBuffer *command_buffer, VkFormat format,
    VkImageLayout old_layout, VkImageLayout new_layout,
    const VkImageSubresourceRange *subresource_range);

bool8_t vulkan_image_copy_from_buffer(VulkanBackendState *state,
                                      VulkanImage *image, VkBuffer buffer,
                                      VulkanCommandBuffer *command_buffer);

bool8_t vulkan_image_copy_cube_faces_from_buffer(
    VulkanBackendState *state, VulkanImage *image, VkBuffer buffer,
    VulkanCommandBuffer *command_buffer, uint64_t face_size);

bool32_t vulkan_create_image_view(VulkanBackendState *state, VkFormat format,
                                  VkImageViewType view_type, VulkanImage *image,
                                  VkImageAspectFlags aspect_flags);

bool8_t vulkan_image_generate_mipmaps(VulkanBackendState *state,
                                      VulkanImage *image, VkFormat image_format,
                                      VulkanCommandBuffer *cmd);

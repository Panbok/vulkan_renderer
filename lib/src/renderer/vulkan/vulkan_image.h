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

bool32_t vulkan_create_image_view(VulkanBackendState *state, VkFormat format,
                                  VkImageViewType view_type, VulkanImage *image,
                                  VkImageAspectFlags aspect_flags);
#pragma once

#include "vulkan_backend.h"
#include "vulkan_utils.h"

void vulkan_image_create(VulkanBackendState *state, VkImageType image_type,
                         uint32_t width, uint32_t height, VkFormat format,
                         VkImageTiling tiling, VkImageUsageFlags usage,
                         VkMemoryPropertyFlags memory_flags,
                         bool32_t create_view,
                         VkImageAspectFlags view_aspect_flags,
                         VulkanImage *out_image);

void vulkan_image_destroy(VulkanBackendState *state, VulkanImage *image);

void vulkan_create_image_view(VulkanBackendState *state, VkFormat format,
                              VulkanImage *image,
                              VkImageAspectFlags aspect_flags);
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

/**
 * @brief Uploads image data from a staging buffer using the transfer queue.
 *
 * Uses the dedicated transfer queue if available for better performance.
 * Handles image layout transitions and synchronization automatically.
 * Does NOT generate mipmaps - only uploads base level.
 *
 * @param state The Vulkan backend state
 * @param image The destination image
 * @param staging_buffer The source staging buffer
 * @param image_format The image format for layout transitions
 * @return true on success, false on failure
 */
bool8_t vulkan_image_upload_via_transfer(VulkanBackendState *state,
                                         VulkanImage *image,
                                         VkBuffer staging_buffer,
                                         VkFormat image_format);

/**
 * @brief Uploads base level via transfer queue, then generates mipmaps on
 * graphics queue.
 *
 * Two-phase upload:
 * 1. Transfer queue: uploads base level
 * 2. Graphics queue: generates mipmaps (if needed) and final layout transition
 *
 * @param state The Vulkan backend state
 * @param image The destination image
 * @param staging_buffer The source staging buffer
 * @param image_format The image format
 * @param generate_mipmaps Whether to generate mipmaps
 * @return true on success, false on failure
 */
bool8_t vulkan_image_upload_with_mipmaps(VulkanBackendState *state,
                                         VulkanImage *image,
                                         VkBuffer staging_buffer,
                                         VkFormat image_format,
                                         bool8_t generate_mipmaps);

/**
 * @brief Uploads cube map faces via transfer queue.
 *
 * @param state The Vulkan backend state
 * @param image The destination cube map image (must have 6 array layers)
 * @param staging_buffer Buffer containing 6 faces sequentially
 * @param image_format The image format
 * @param face_size Size of each face in bytes
 * @return true on success, false on failure
 */
bool8_t vulkan_image_upload_cube_via_transfer(VulkanBackendState *state,
                                              VulkanImage *image,
                                              VkBuffer staging_buffer,
                                              VkFormat image_format,
                                              uint64_t face_size);

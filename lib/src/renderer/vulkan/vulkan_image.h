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

/**
 * @brief Copy a region of an image to a buffer.
 *
 * Used for pixel readback (e.g., object picking). The image must be in
 * TRANSFER_SRC_OPTIMAL layout before calling this function.
 * Copies from mip level 0, array layer 0.
 *
 * This function uses VK_IMAGE_ASPECT_COLOR_BIT by default. For depth/stencil
 * images or other aspect flags, use vulkan_image_copy_to_buffer_ex instead.
 *
 * @param state The Vulkan backend state
 * @param image The source image
 * @param buffer The destination buffer (must be HOST_VISIBLE for readback)
 * @param buffer_offset Offset into the destination buffer
 * @param x X coordinate of the region to copy (in pixels)
 * @param y Y coordinate of the region to copy (in pixels)
 * @param width Width of the region to copy (in pixels)
 * @param height Height of the region to copy (in pixels)
 * @param command_buffer The command buffer to record the copy command (must be
 * in recording state)
 * @return true on success, false on failure
 *
 * @note All coordinates and dimensions are in pixels. The function validates
 * that the region does not exceed image bounds (x + width <= image->width,
 * y + height <= image->height). Use vulkan_image_copy_to_buffer_ex when you
 * need to specify a different image aspect (e.g., depth or stencil).
 */
bool8_t vulkan_image_copy_to_buffer(VulkanBackendState *state,
                                    VulkanImage *image, VkBuffer buffer,
                                    uint64_t buffer_offset, uint32_t x,
                                    uint32_t y, uint32_t width, uint32_t height,
                                    VulkanCommandBuffer *command_buffer);

/**
 * @brief Copy a region of an image to a buffer with specified aspect flags.
 *
 * Extended version that allows specifying the image aspect (color, depth, etc).
 * The image must be in TRANSFER_SRC_OPTIMAL layout before calling this
 * function. Copies from mip level 0, array layer 0.
 *
 * @param state The Vulkan backend state
 * @param image The source image
 * @param buffer The destination buffer (must be HOST_VISIBLE for readback)
 * @param buffer_offset Offset into the destination buffer
 * @param x X coordinate of the region to copy (in pixels)
 * @param y Y coordinate of the region to copy (in pixels)
 * @param width Width of the region to copy (in pixels)
 * @param height Height of the region to copy (in pixels)
 * @param aspect_flags The image aspect flags (e.g., VK_IMAGE_ASPECT_COLOR_BIT,
 * VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT)
 * @param command_buffer The command buffer to record the copy command (must be
 * in recording state)
 * @return true on success, false on failure
 *
 * @note All coordinates and dimensions are in pixels. The function validates
 * that the region does not exceed image bounds (x + width <= image->width,
 * y + height <= image->height). Use this function instead of
 * vulkan_image_copy_to_buffer when you need to copy depth/stencil data or
 * specify a different image aspect.
 */
bool8_t vulkan_image_copy_to_buffer_ex(VulkanBackendState *state,
                                       VulkanImage *image, VkBuffer buffer,
                                       uint64_t buffer_offset, uint32_t x,
                                       uint32_t y, uint32_t width,
                                       uint32_t height,
                                       VkImageAspectFlags aspect_flags,
                                       VulkanCommandBuffer *command_buffer);

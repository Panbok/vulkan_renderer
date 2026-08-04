#pragma once

#include "vulkan_types.h"

typedef struct VulkanShaderStageFlagResult {
  VkShaderStageFlagBits flag;
  bool8_t is_valid;
} VulkanShaderStageFlagResult;

typedef struct QueueFamilyIndexResult {
  QueueFamilyIndex indices[QUEUE_FAMILY_TYPE_COUNT];
  uint32_t length;
} QueueFamilyIndexResult;

VulkanShaderStageFlagResult
vulkan_shader_stage_to_vk(VkrShaderStageFlags stage);

QueueFamilyIndexResult find_queue_family_indices(VulkanBackendState *state,
                                                 VkPhysicalDevice device);

int32_t find_memory_index(VkPhysicalDevice device, uint32_t type_filter,
                          uint32_t property_flags);

/**
 * @brief find_memory_index with a defined fallback order for optional bits.
 *
 * Drops performance-only property bits (DEVICE_LOCAL, then HOST_CACHED) until a
 * memory type matches, and reports the selected type's complete property flags
 * through @p out_used_flags. Every device allocation should use this rather
 * than hand-rolling its own retry, which is how one call site ended up with no
 * fallback at all.
 *
 * @return Memory type index, or -1 when nothing matches.
 */
int32_t find_memory_index_with_fallback(VkPhysicalDevice device,
                                        uint32_t type_filter,
                                        uint32_t property_flags,
                                        uint32_t *out_used_flags);

VkFormat vulkan_vertex_format_to_vk(VkrVertexFormat format);

VkPrimitiveTopology
vulkan_primitive_topology_to_vk(VkrPrimitiveTopology topology);

VkPolygonMode vulkan_polygon_mode_to_vk(VkrPolygonMode mode);

VkBufferUsageFlags vulkan_buffer_usage_to_vk(VkrBufferUsageFlags usage);

VkMemoryPropertyFlags
vulkan_memory_property_flags_to_vk(VkrMemoryPropertyFlags flags);

VkFormat vulkan_image_format_from_texture_format(VkrTextureFormat format);

VkrTextureFormat vulkan_texture_format_from_image_format(VkFormat format);

VkImageUsageFlags
vulkan_image_usage_from_texture_usage(VkrTextureUsageFlags usage);

bool8_t vulkan_attachment_needs_subresource_view(
    uint32_t image_mip_levels, uint32_t image_array_layers,
    uint32_t attachment_mip_level, uint32_t attachment_base_layer,
    uint32_t attachment_layer_count);

VkSamplerAddressMode
vulkan_sampler_address_mode_from_repeat(VkrTextureRepeatMode mode);

VkCullModeFlags vulkan_cull_mode_to_vk(VkrCullMode mode);

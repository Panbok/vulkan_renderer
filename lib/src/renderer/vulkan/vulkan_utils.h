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

VkFormat vulkan_vertex_format_to_vk(VkrVertexFormat format);

VkPrimitiveTopology
vulkan_primitive_topology_to_vk(VkrPrimitiveTopology topology);

VkPolygonMode vulkan_polygon_mode_to_vk(VkrPolygonMode mode);

VkBufferUsageFlags vulkan_buffer_usage_to_vk(VkrBufferUsageFlags usage);

VkMemoryPropertyFlags
vulkan_memory_property_flags_to_vk(VkrMemoryPropertyFlags flags);

VkFormat vulkan_image_format_from_texture_format(VkrTextureFormat format);

VkSamplerAddressMode
vulkan_sampler_address_mode_from_repeat(VkrTextureRepeatMode mode);

VkCullModeFlags vulkan_cull_mode_to_vk(VkrCullMode mode);

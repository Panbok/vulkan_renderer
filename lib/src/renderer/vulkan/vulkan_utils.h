#pragma once

#include "vulkan_types.h"

typedef struct VulkanShaderStageFlagResult {
  VkShaderStageFlagBits flag;
  bool8_t is_valid;
} VulkanShaderStageFlagResult;

VulkanShaderStageFlagResult vulkan_shader_stage_to_vk(ShaderStageFlags stage);

Array_QueueFamilyIndex find_queue_family_indices(VulkanBackendState *state,
                                                 VkPhysicalDevice device);

int32_t find_memory_index(VkPhysicalDevice device, uint32_t type_filter,
                          uint32_t property_flags);

VkFormat vulkan_vertex_format_to_vk(VertexFormat format);

VkPrimitiveTopology vulkan_primitive_topology_to_vk(PrimitiveTopology topology);

VkPolygonMode vulkan_polygon_mode_to_vk(PolygonMode mode);

VkBufferUsageFlags vulkan_buffer_usage_to_vk(BufferUsageFlags usage);

VkMemoryPropertyFlags
vulkan_memory_property_flags_to_vk(MemoryPropertyFlags flags);

VkFormat vulkan_image_format_from_texture_format(TextureFormat format);
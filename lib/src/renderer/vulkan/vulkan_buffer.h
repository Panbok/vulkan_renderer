#pragma once

#include "vulkan_types.h"

bool8_t vulkan_buffer_create(VulkanBackendState *state, uint64_t size,
                             VkBufferUsageFlagBits usage,
                             uint32_t memory_property_flags,
                             bool8_t bind_on_create, VulkanBuffer *out_buffer);

void vulkan_buffer_destroy(VulkanBackendState *state, VulkanBuffer *buffer);

bool8_t vulkan_buffer_resize(VulkanBackendState *state, uint64_t new_size,
                             VulkanBuffer *buffer, VkQueue queue,
                             VkCommandPool pool);

void vulkan_buffer_bind(VulkanBackendState *state, VulkanBuffer *buffer,
                        uint64_t offset);

void *vulkan_buffer_lock_memory(VulkanBackendState *state, VulkanBuffer *buffer,
                                uint64_t offset, uint64_t size, uint32_t flags);
void vulkan_buffer_unlock_memory(VulkanBackendState *state,
                                 VulkanBuffer *buffer);

void vulkan_buffer_load_data(VulkanBackendState *state, VulkanBuffer *buffer,
                             uint64_t offset, uint64_t size, uint32_t flags,
                             const void *data);

void vulkan_buffer_copy_to(VulkanBackendState *state, VkCommandPool pool,
                           VkFence fence, VkQueue queue, VkBuffer source,
                           uint64_t source_offset, VkBuffer dest,
                           uint64_t dest_offset, uint64_t size);
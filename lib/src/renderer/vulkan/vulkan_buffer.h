#pragma once

#include "vulkan_command.h"
#include "vulkan_types.h"

bool8_t vulkan_buffer_create(VulkanBackendState *state,
                             const VkrBufferDescription *desc,
                             struct s_BufferHandle *out_buffer);

void vulkan_buffer_destroy(VulkanBackendState *state, VulkanBuffer *buffer);

bool8_t vulkan_buffer_resize(VulkanBackendState *state, uint64_t new_size,
                             VulkanBuffer *buffer, VkQueue queue,
                             VkCommandPool pool);

bool8_t vulkan_buffer_bind(VulkanBackendState *state, VulkanBuffer *buffer,
                           uint64_t offset);

void vulkan_buffer_bind_vertex_buffers(VulkanBackendState *state,
                                       VulkanCommandBuffer *command_buffer,
                                       uint32_t first_binding,
                                       uint32_t buffer_count,
                                       const VkBuffer *buffers,
                                       const VkDeviceSize *offsets);

void vulkan_buffer_bind_vertex_buffer(VulkanBackendState *state,
                                      VulkanCommandBuffer *command_buffer,
                                      uint32_t binding, VkBuffer buffer,
                                      VkDeviceSize offset);

void vulkan_buffer_bind_index_buffer(VulkanBackendState *state,
                                     VulkanCommandBuffer *command_buffer,
                                     VkBuffer buffer, VkDeviceSize offset,
                                     VkIndexType index_type);

void *vulkan_buffer_lock_memory(VulkanBackendState *state, VulkanBuffer *buffer,
                                uint64_t offset, uint64_t size, uint32_t flags);
bool8_t vulkan_buffer_unlock_memory(VulkanBackendState *state,
                                    VulkanBuffer *buffer);

bool8_t vulkan_buffer_load_data(VulkanBackendState *state, VulkanBuffer *buffer,
                                uint64_t offset, uint64_t size, uint32_t flags,
                                const void *data);
void vulkan_buffer_flush(VulkanBackendState *state, VulkanBuffer *buffer,
                         uint64_t offset, uint64_t size);

bool8_t vulkan_buffer_copy_to(VulkanBackendState *state,
                              VulkanBuffer *buffer_handle, VkBuffer source,
                              uint64_t source_offset, VkBuffer dest,
                              uint64_t dest_offset, uint64_t size);

bool8_t vulkan_buffer_allocate(VulkanBackendState *state, VulkanBuffer *buffer,
                               uint64_t size, uint64_t *out_offset);

bool8_t vulkan_buffer_free(VulkanBackendState *state, VulkanBuffer *buffer,
                           uint64_t size, uint64_t offset);

uint64_t vulkan_buffer_free_space(VulkanBuffer *buffer);

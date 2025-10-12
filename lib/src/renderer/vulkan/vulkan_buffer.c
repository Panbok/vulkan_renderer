#include "vulkan_buffer.h"
#include "memory/vkr_dmemory_allocator.h"

bool8_t vulkan_buffer_create(VulkanBackendState *state,
                             const VkrBufferDescription *desc,
                             struct s_BufferHandle *out_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_buffer != NULL, "Out buffer is NULL");

  VkBufferUsageFlags usage = vulkan_buffer_usage_to_vk(desc->usage);
  VkMemoryPropertyFlags memory_property_flags =
      vulkan_memory_property_flags_to_vk(desc->memory_properties);

  out_buffer->buffer.total_size = desc->size;
  out_buffer->buffer.usage = usage;
  out_buffer->buffer.memory_property_flags = memory_property_flags;

  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = desc->size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  if (vkCreateBuffer(state->device.logical_device, &buffer_info,
                     state->allocator,
                     &out_buffer->buffer.handle) != VK_SUCCESS) {
    log_error("Failed to create buffer");
    return false_v;
  }

  VkMemoryRequirements memory_requirements;
  vkGetBufferMemoryRequirements(state->device.logical_device,
                                out_buffer->buffer.handle,
                                &memory_requirements);
  out_buffer->buffer.memory_index = find_memory_index(
      state->device.physical_device, memory_requirements.memoryTypeBits,
      out_buffer->buffer.memory_property_flags);

  if (out_buffer->buffer.memory_index == -1) {
    log_error("Failed to find memory index for buffer");
    vkDestroyBuffer(state->device.logical_device, out_buffer->buffer.handle,
                    state->allocator);
    return false_v;
  }

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = memory_requirements.size,
      .memoryTypeIndex = out_buffer->buffer.memory_index,
  };

  if (vkAllocateMemory(state->device.logical_device, &alloc_info,
                       state->allocator,
                       &out_buffer->buffer.memory) != VK_SUCCESS) {
    log_error("Failed to allocate memory for buffer");
    vkDestroyBuffer(state->device.logical_device, out_buffer->buffer.handle,
                    state->allocator);
    return false_v;
  }

  if (bitset8_is_set(&desc->buffer_type, VKR_BUFFER_TYPE_GRAPHICS)) {
    out_buffer->buffer.command_pool = state->device.graphics_command_pool;
    out_buffer->buffer.queue = state->device.graphics_queue;
    // Note: We use temporary fences for buffer operations, no need for
    // per-buffer fences
  }

  log_debug("Created Vulkan buffer: %p", out_buffer->buffer.handle);

  // Initialize offset allocator for sub-allocations
  // Create dmemory for offset tracking (manages virtual address space, not real
  // memory)
  if (!vkr_dmemory_create(desc->size, &out_buffer->buffer.offset_allocator)) {
    log_error("Failed to create offset allocator for buffer");
    vkFreeMemory(state->device.logical_device, out_buffer->buffer.memory,
                 state->allocator);
    vkDestroyBuffer(state->device.logical_device, out_buffer->buffer.handle,
                    state->allocator);
    return false_v;
  }

  out_buffer->buffer.allocator.ctx = &out_buffer->buffer.offset_allocator;
  vkr_dmemory_allocator_create(&out_buffer->buffer.allocator);

  if (desc->bind_on_create) {
    return vulkan_buffer_bind(state, &out_buffer->buffer, 0);
  }

  return true_v;
}

void vulkan_buffer_destroy(VulkanBackendState *state, VulkanBuffer *buffer) {
  log_debug("Destroying Vulkan buffer: %p", buffer->handle);

  vkr_dmemory_allocator_destroy(&buffer->allocator);

  if (buffer->handle != VK_NULL_HANDLE) {
    vkDestroyBuffer(state->device.logical_device, buffer->handle,
                    state->allocator);
  }
  if (buffer->memory != VK_NULL_HANDLE) {
    vkFreeMemory(state->device.logical_device, buffer->memory,
                 state->allocator);
  }

  buffer->handle = VK_NULL_HANDLE;
  buffer->memory = VK_NULL_HANDLE;
}

bool8_t vulkan_buffer_bind(VulkanBackendState *state, VulkanBuffer *buffer,
                           uint64_t offset) {
  assert_log(state != NULL, "State is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  if (vkBindBufferMemory(state->device.logical_device, buffer->handle,
                         buffer->memory, offset) != VK_SUCCESS) {
    log_error("Failed to bind buffer memory");
    return false_v;
  }

  return true_v;
}

bool8_t vulkan_buffer_resize(VulkanBackendState *state, uint64_t new_size,
                             VulkanBuffer *buffer, VkQueue queue,
                             VkCommandPool pool) {
  assert_log(state != NULL, "State is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");
  assert_log(new_size > buffer->total_size,
             "New size must be greater than current size");

  log_debug("Resizing buffer from %llu to %llu bytes",
            (uint64_t)buffer->total_size, (uint64_t)new_size);

  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = new_size,
      .usage = buffer->usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VkBuffer new_buffer;
  if (vkCreateBuffer(state->device.logical_device, &buffer_info,
                     state->allocator, &new_buffer) != VK_SUCCESS) {
    log_error("Failed to create new buffer during resize");
    return false_v;
  }

  VkMemoryRequirements new_requirements;
  vkGetBufferMemoryRequirements(state->device.logical_device, new_buffer,
                                &new_requirements);

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = new_requirements.size,
      .memoryTypeIndex = buffer->memory_index,
  };

  VkDeviceMemory new_memory;
  if (vkAllocateMemory(state->device.logical_device, &alloc_info,
                       state->allocator, &new_memory) != VK_SUCCESS) {
    log_error("Failed to allocate memory for new buffer during resize");
    vkDestroyBuffer(state->device.logical_device, new_buffer, state->allocator);
    return false_v;
  }

  if (vkBindBufferMemory(state->device.logical_device, new_buffer, new_memory,
                         0) != VK_SUCCESS) {
    log_error("Failed to bind buffer memory during resize");
    vkDestroyBuffer(state->device.logical_device, new_buffer, state->allocator);
    vkFreeMemory(state->device.logical_device, new_memory, state->allocator);
    return false_v;
  }

  if (!vulkan_buffer_copy_to(state, buffer, buffer->handle, 0, new_buffer, 0,
                             buffer->total_size)) {
    log_error("Failed to copy buffer data during resize");
    vkDestroyBuffer(state->device.logical_device, new_buffer, state->allocator);
    vkFreeMemory(state->device.logical_device, new_memory, state->allocator);
    return false_v;
  }

  if (!vkr_dmemory_resize(&buffer->offset_allocator, new_size)) {
    log_error("Failed to resize offset allocator during buffer resize");
    vkDestroyBuffer(state->device.logical_device, new_buffer, state->allocator);
    vkFreeMemory(state->device.logical_device, new_memory, state->allocator);
    return false_v;
  }

  // Cleanup old Vulkan resources (don't use vulkan_buffer_destroy as it would
  // destroy offset_allocator)
  VkBuffer old_buffer = buffer->handle;
  VkDeviceMemory old_memory = buffer->memory;

  if (old_buffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(state->device.logical_device, old_buffer, state->allocator);
  }
  if (old_memory != VK_NULL_HANDLE) {
    vkFreeMemory(state->device.logical_device, old_memory, state->allocator);
  }

  buffer->handle = new_buffer;
  buffer->memory = new_memory;
  buffer->total_size = new_size;

  log_debug("Buffer resized successfully to %llu bytes", (uint64_t)new_size);

  return true_v;
}

void *vulkan_buffer_lock_memory(VulkanBackendState *state, VulkanBuffer *buffer,
                                uint64_t offset, uint64_t size,
                                uint32_t flags) {
  assert_log(state != NULL, "State is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  void *data;
  if (vkMapMemory(state->device.logical_device, buffer->memory, offset, size,
                  flags, &data) != VK_SUCCESS) {
    log_error("Failed to lock memory");
    return NULL;
  }

  return data;
}

bool8_t vulkan_buffer_unlock_memory(VulkanBackendState *state,
                                    VulkanBuffer *buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  vkUnmapMemory(state->device.logical_device, buffer->memory);

  return true_v;
}

bool8_t vulkan_buffer_load_data(VulkanBackendState *state, VulkanBuffer *buffer,
                                uint64_t offset, uint64_t size, uint32_t flags,
                                const void *data) {
  assert_log(state != NULL, "State is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  void *mapped_memory =
      vulkan_buffer_lock_memory(state, buffer, offset, size, flags);
  if (mapped_memory == NULL) {
    log_error("Failed to lock memory");
    return false_v;
  }

  MemCopy(mapped_memory, data, size);

  vulkan_buffer_unlock_memory(state, buffer);

  return true_v;
}

bool8_t vulkan_buffer_copy_to(VulkanBackendState *state,
                              VulkanBuffer *buffer_handle, VkBuffer source,
                              uint64_t source_offset, VkBuffer dest,
                              uint64_t dest_offset, uint64_t size) {
  VkCommandBufferAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandPool = buffer_handle->command_pool,
      .commandBufferCount = 1,
  };

  VkCommandBuffer command_buffer;
  if (vkAllocateCommandBuffers(state->device.logical_device, &alloc_info,
                               &command_buffer) != VK_SUCCESS) {
    log_error("Failed to allocate command buffer");
    return false_v;
  }

  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
    log_error("Failed to begin command buffer");
    vkFreeCommandBuffers(state->device.logical_device,
                         buffer_handle->command_pool, 1, &command_buffer);
    return false_v;
  }

  VkBufferCopy copy_region = {
      .srcOffset = source_offset, .dstOffset = dest_offset, .size = size};

  vkCmdCopyBuffer(command_buffer, source, dest, 1, &copy_region);

  if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
    log_error("Failed to end command buffer");
    vkFreeCommandBuffers(state->device.logical_device,
                         buffer_handle->command_pool, 1, &command_buffer);
    return false_v;
  }

  VulkanFence temp_fence;
  vulkan_fence_create(state, false_v, &temp_fence);
  if (temp_fence.handle == VK_NULL_HANDLE) {
    log_error("Failed to create temporary fence");
    vkFreeCommandBuffers(state->device.logical_device,
                         buffer_handle->command_pool, 1, &command_buffer);
    return false_v;
  }

  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
  };

  if (vkQueueSubmit(buffer_handle->queue, 1, &submit_info, temp_fence.handle) !=
      VK_SUCCESS) {
    log_error("Failed to submit command buffer");
    vulkan_fence_destroy(state, &temp_fence);
    vkFreeCommandBuffers(state->device.logical_device,
                         buffer_handle->command_pool, 1, &command_buffer);
    return false_v;
  }

  // Wait for the temporary fence
  if (!vulkan_fence_wait(state, UINT64_MAX, &temp_fence)) {
    log_error("Failed to wait for fence");
    vulkan_fence_destroy(state, &temp_fence);
    vkFreeCommandBuffers(state->device.logical_device,
                         buffer_handle->command_pool, 1, &command_buffer);
    return false_v;
  }

  // Ensure queue is completely idle before cleanup to avoid validation errors
  vkQueueWaitIdle(buffer_handle->queue);

  vulkan_fence_destroy(state, &temp_fence);
  vkFreeCommandBuffers(state->device.logical_device,
                       buffer_handle->command_pool, 1, &command_buffer);

  return true_v;
}

void vulkan_buffer_bind_vertex_buffers(VulkanBackendState *state,
                                       VulkanCommandBuffer *command_buffer,
                                       uint32_t first_binding,
                                       uint32_t buffer_count,
                                       const VkBuffer *buffers,
                                       const VkDeviceSize *offsets) {
  assert_log(state != NULL, "State is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(buffers != NULL, "Buffers array is NULL");
  assert_log(offsets != NULL, "Offsets array is NULL");
  assert_log(buffer_count > 0, "Buffer count must be greater than 0");

  vkCmdBindVertexBuffers(command_buffer->handle, first_binding, buffer_count,
                         buffers, offsets);

  log_debug("Bound %u vertex buffers starting at binding %u", buffer_count,
            first_binding);
}

void vulkan_buffer_bind_vertex_buffer(VulkanBackendState *state,
                                      VulkanCommandBuffer *command_buffer,
                                      uint32_t binding, VkBuffer buffer,
                                      VkDeviceSize offset) {
  assert_log(state != NULL, "State is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(buffer != VK_NULL_HANDLE, "Buffer is NULL");

  VkBuffer buffers[] = {buffer};
  VkDeviceSize offsets[] = {offset};

  vkCmdBindVertexBuffers(command_buffer->handle, binding, 1, buffers, offsets);

  // log_debug("Bound vertex buffer to binding %u", binding);
}

void vulkan_buffer_bind_index_buffer(VulkanBackendState *state,
                                     VulkanCommandBuffer *command_buffer,
                                     VkBuffer buffer, VkDeviceSize offset,
                                     VkIndexType index_type) {
  assert_log(state != NULL, "State is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(buffer != VK_NULL_HANDLE, "Buffer is NULL");

  vkCmdBindIndexBuffer(command_buffer->handle, buffer, offset, index_type);

  const char *index_type_str =
      (index_type == VK_INDEX_TYPE_UINT16) ? "uint16" : "uint32";
  // log_debug("Bound index buffer with type %s", index_type_str);
}

bool8_t vulkan_buffer_allocate(VulkanBackendState *state, VulkanBuffer *buffer,
                               uint64_t size, uint64_t *out_offset) {
  assert_log(buffer != NULL, "Buffer is NULL");
  assert_log(out_offset != NULL, "Output offset is NULL");

  void *ptr = vkr_allocator_alloc(&buffer->allocator, size,
                                  VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (ptr == NULL) {
    log_error("Failed to allocate %llu bytes from buffer offset allocator",
              (uint64_t)size);
    return false_v;
  }

  // Convert pointer to offset
  uintptr_t base = (uintptr_t)buffer->offset_allocator.base_memory;
  uintptr_t allocated = (uintptr_t)ptr;
  *out_offset = (uint64_t)(allocated - base);

  log_debug("Allocated %llu bytes at offset %llu in buffer", (uint64_t)size,
            (uint64_t)*out_offset);

  return true_v;
}

bool8_t vulkan_buffer_free(VulkanBackendState *state, VulkanBuffer *buffer,
                           uint64_t size, uint64_t offset) {
  assert_log(buffer != NULL, "Buffer is NULL");

  // Convert offset to pointer
  void *ptr =
      (void *)((uint8_t *)buffer->offset_allocator.base_memory + offset);

  vkr_allocator_free(&buffer->allocator, ptr, size,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  return true_v;
}

uint64_t vulkan_buffer_free_space(VulkanBuffer *buffer) {
  assert_log(buffer != NULL, "Buffer is NULL");
  return vkr_dmemory_get_free_space(&buffer->offset_allocator);
}
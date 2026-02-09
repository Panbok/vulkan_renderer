#include "vulkan_command.h"
#include "vulkan_backend.h"
#include "vulkan_fence.h"

bool8_t
vulkan_command_buffer_allocate(VulkanBackendState *state,
                               VulkanCommandBuffer *out_command_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_command_buffer != NULL, "Out command buffer is NULL");

  VkCommandBufferAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = state->device.graphics_command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };

  if (vkAllocateCommandBuffers(state->device.logical_device, &alloc_info,
                               &out_command_buffer->handle) != VK_SUCCESS) {
    log_fatal("Failed to allocate Vulkan command buffer");
    return false_v;
  }

  out_command_buffer->state = COMMAND_BUFFER_STATE_READY;
  out_command_buffer->bound_global_descriptor_set = VK_NULL_HANDLE;
  out_command_buffer->bound_global_pipeline_layout = VK_NULL_HANDLE;

  // log_debug("Created Vulkan command buffer: %p", out_command_buffer->handle);

  return true_v;
}

void vulkan_command_buffer_free(VulkanBackendState *state,
                                VulkanCommandBuffer *command_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");

  // log_debug("Destroying Vulkan command buffer");

  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1,
                       &command_buffer->handle);

  command_buffer->handle = VK_NULL_HANDLE;
  command_buffer->state = COMMAND_BUFFER_STATE_NOT_ALLOCATED;
  command_buffer->bound_global_descriptor_set = VK_NULL_HANDLE;
  command_buffer->bound_global_pipeline_layout = VK_NULL_HANDLE;
}

bool8_t vulkan_command_buffer_begin(VulkanCommandBuffer *command_buffer) {
  assert_log(command_buffer != NULL, "Command buffer is NULL");

  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  if (vkBeginCommandBuffer(command_buffer->handle, &begin_info) != VK_SUCCESS) {
    log_fatal("Failed to begin Vulkan command buffer");
    return false_v;
  }

  command_buffer->state = COMMAND_BUFFER_STATE_RECORDING;
  command_buffer->bound_global_descriptor_set = VK_NULL_HANDLE;
  command_buffer->bound_global_pipeline_layout = VK_NULL_HANDLE;

  return true_v;
}

bool8_t vulkan_command_buffer_end(VulkanCommandBuffer *command_buffer) {
  assert_log(command_buffer != VK_NULL_HANDLE, "Command buffer is NULL");

  if (vkEndCommandBuffer(command_buffer->handle) != VK_SUCCESS) {
    log_fatal("Failed to end Vulkan command buffer");
    return false_v;
  }

  command_buffer->state = COMMAND_BUFFER_STATE_RECORDING_ENDED;

  return true_v;
}

void vulkan_command_buffer_update_submitted(
    VulkanCommandBuffer *command_buffer) {
  assert_log(command_buffer != VK_NULL_HANDLE, "Command buffer is NULL");

  command_buffer->state = COMMAND_BUFFER_STATE_SUBMITTED;
}

void vulkan_command_buffer_reset(VulkanCommandBuffer *command_buffer) {
  assert_log(command_buffer != VK_NULL_HANDLE, "Command buffer is NULL");

  command_buffer->state = COMMAND_BUFFER_STATE_READY;
  command_buffer->bound_global_descriptor_set = VK_NULL_HANDLE;
  command_buffer->bound_global_pipeline_layout = VK_NULL_HANDLE;
}

bool8_t vulkan_command_buffer_allocate_and_begin_single_use(
    VulkanBackendState *state, VulkanCommandBuffer *command_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");

  if (!vulkan_command_buffer_allocate(state, command_buffer)) {
    log_error("Failed to allocate Vulkan command buffer");
    return false_v;
  }

  if (!vulkan_command_buffer_begin(command_buffer)) {
    log_error("Failed to begin Vulkan command buffer");
    return false_v;
  }

  return true_v;
}

bool8_t
vulkan_command_buffer_end_single_use(VulkanBackendState *state,
                                     VulkanCommandBuffer *command_buffer,
                                     VkQueue queue, VkFence fence) {
  assert_log(state != NULL, "State is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  (void)fence;

  if (!vulkan_command_buffer_end(command_buffer)) {
    log_error("Failed to end Vulkan command buffer");
    vulkan_command_buffer_free(state, command_buffer);
    return false_v;
  }

  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer->handle,
  };

  VulkanFence temp_fence;
  vulkan_fence_create(state, false_v, &temp_fence);
  if (temp_fence.handle == VK_NULL_HANDLE) {
    log_error("Failed to create fence for single-use command submission");
    vulkan_command_buffer_free(state, command_buffer);
    return false_v;
  }

  if (vulkan_backend_queue_submit_locked(state, queue, 1, &submit_info,
                                         temp_fence.handle) != VK_SUCCESS) {
    log_error("Failed to submit Vulkan command buffer");
    vulkan_fence_destroy(state, &temp_fence);
    vulkan_command_buffer_free(state, command_buffer);
    return false_v;
  }

#if VKR_VULKAN_PARALLEL_UPLOAD
  if (!vulkan_fence_wait(state, UINT64_MAX, &temp_fence)) {
    log_error("Failed waiting on single-use command fence");
    vulkan_fence_destroy(state, &temp_fence);
    vulkan_command_buffer_free(state, command_buffer);
    return false_v;
  }
#else
  if (vulkan_backend_queue_wait_idle_locked(state, queue) != VK_SUCCESS) {
    log_error("Failed to wait for Vulkan queue to become idle");
    vulkan_fence_destroy(state, &temp_fence);
    vulkan_command_buffer_free(state, command_buffer);
    return false_v;
  }
#endif

  vulkan_fence_destroy(state, &temp_fence);
  vulkan_command_buffer_free(state, command_buffer);

  return true_v;
}

#include "vulkan_command.h"

bool8_t vulkan_command_buffer_create(VulkanBackendState *state,
                                     VulkanCommandBuffer *out_command_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_command_buffer != NULL, "Out command buffer is NULL");

  VkCommandBufferAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = state->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };

  if (vkAllocateCommandBuffers(state->device, &alloc_info,
                               &out_command_buffer->handle) != VK_SUCCESS) {
    log_fatal("Failed to allocate Vulkan command buffer");
    return false_v;
  }

  out_command_buffer->state = COMMAND_BUFFER_STATE_READY;

  log_debug("Created Vulkan command buffer: %p", out_command_buffer->handle);

  return true_v;
}

void vulkan_command_buffer_destroy(VulkanBackendState *state,
                                   VulkanCommandBuffer *command_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(command_buffer != VK_NULL_HANDLE, "Command buffer is NULL");

  log_debug("Destroying Vulkan command buffer");

  vkFreeCommandBuffers(state->device, state->command_pool, 1,
                       &command_buffer->handle);

  command_buffer->handle = VK_NULL_HANDLE;
  command_buffer->state = COMMAND_BUFFER_STATE_NOT_ALLOCATED;
}

bool8_t vulkan_command_buffer_begin(VulkanCommandBuffer *command_buffer) {
  assert_log(command_buffer != VK_NULL_HANDLE, "Command buffer is NULL");

  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  if (vkBeginCommandBuffer(command_buffer->handle, &begin_info) != VK_SUCCESS) {
    log_fatal("Failed to begin Vulkan command buffer");
    return false_v;
  }

  command_buffer->state = COMMAND_BUFFER_STATE_RECORDING;

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
}
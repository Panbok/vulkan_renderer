#include "renderer/vulkan/vkr_vulkan_wsi.h"

#include "containers/str.h"

VkrVulkanPresentResult vkr_vulkan_present_result_classify(VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return (VkrVulkanPresentResult){
        .enqueue_state_known = true_v,
        .queue_operations_enqueued = true_v,
        .present_completion_tracking_required = true_v,
    };

  case VK_SUBOPTIMAL_KHR:
  case VK_ERROR_OUT_OF_DATE_KHR:
  case VK_ERROR_SURFACE_LOST_KHR:
  case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
  case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT:
    return (VkrVulkanPresentResult){
        .enqueue_state_known = true_v,
        .queue_operations_enqueued = true_v,
        .present_completion_tracking_required = true_v,
        .target_recreate_required = true_v,
    };

  case VK_ERROR_OUT_OF_HOST_MEMORY:
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return (VkrVulkanPresentResult){
        .enqueue_state_known = true_v,
        .acquired_image_recovery_required = true_v,
    };

  case VK_ERROR_DEVICE_LOST:
    return (VkrVulkanPresentResult){.device_lost = true_v};

  default:
    return (VkrVulkanPresentResult){0};
  }
}

void vkr_vulkan_reacquire_record(VkrVulkanReacquireState *state,
                                 bool8_t image_was_presented,
                                 uint64_t wait_submit_value) {
  if (!state || !image_was_presented || !wait_submit_value ||
      state->successor_present_complete || state->pending_wait_submit_value)
    return;
  state->pending_wait_submit_value = wait_submit_value;
}

VkrVulkanReacquireResult
vkr_vulkan_reacquire_complete(VkrVulkanReacquireState *state,
                              uint64_t completed_submit_value) {
  if (!state || state->successor_present_complete ||
      !state->pending_wait_submit_value ||
      completed_submit_value < state->pending_wait_submit_value)
    return (VkrVulkanReacquireResult){0};
  state->successor_present_complete = true_v;
  state->pending_wait_submit_value = 0u;
  return (VkrVulkanReacquireResult){
      .image_present_complete = true_v,
      .collect_retired_swapchains = true_v,
  };
}

bool8_t vkr_vulkan_instance_extension_is_surface(const char *name) {
  return name &&
         (string_equals(name, VK_KHR_SURFACE_EXTENSION_NAME) ||
          string_equals(name,
                        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ||
          string_equals(name, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) ||
          string_equals(name, "VK_KHR_win32_surface"));
}

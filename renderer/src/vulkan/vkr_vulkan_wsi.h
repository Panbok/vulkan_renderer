#pragma once

#include "defines.h"

#include <vulkan/vulkan.h>

/**
 * Queue-present ownership facts used by the windowed Vulkan target. Unknown
 * results deliberately prove nothing: callers must not recycle an acquired
 * image or its per-image present semaphore without a completion proof.
 */
typedef struct VkrVulkanPresentResult {
  bool8_t enqueue_state_known;
  bool8_t queue_operations_enqueued;
  bool8_t present_completion_tracking_required;
  bool8_t acquired_image_recovery_required;
  bool8_t target_recreate_required;
  bool8_t device_lost;
} VkrVulkanPresentResult;

/** State proven after a submitted acquire wait reaches GPU completion. */
typedef struct VkrVulkanReacquireState {
  uint64_t pending_wait_submit_value;
  bool8_t successor_present_complete;
} VkrVulkanReacquireState;

typedef struct VkrVulkanReacquireResult {
  bool8_t image_present_complete;
  bool8_t collect_retired_swapchains;
} VkrVulkanReacquireResult;

/** Classifies every queue-present result whose enqueue contract VKR handles. */
VkrVulkanPresentResult vkr_vulkan_present_result_classify(VkResult result);

/**
 * Records the submission that waits on an acquire semaphore for an image that
 * was previously presented. Acquisition alone is not a completion proof; the
 * submission's timeline value must complete before predecessor collection.
 */
void vkr_vulkan_reacquire_record(VkrVulkanReacquireState *state,
                                 bool8_t image_was_presented,
                                 uint64_t wait_submit_value);

/**
 * Promotes a pending acquire wait to a WSI completion proof once its timeline
 * value completes. The first successor proof also permits predecessor
 * swapchain collection because presentation operations share the same queue.
 */
VkrVulkanReacquireResult
vkr_vulkan_reacquire_complete(VkrVulkanReacquireState *state,
                              uint64_t completed_submit_value);

/** True only for instance extensions omitted by a strictly offscreen target. */
bool8_t vkr_vulkan_instance_extension_is_surface(const char *name);

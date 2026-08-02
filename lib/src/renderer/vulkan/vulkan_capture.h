#pragma once

#include "vulkan_types.h"

bool8_t vulkan_capture_ring_init(VulkanBackendState *state,
                                 uint32_t capacity,
                                 uint64_t max_batch_bytes);
void vulkan_capture_ring_shutdown(VulkanBackendState *state);
VkrRendererError vulkan_capture_reserve(
    void *backend_state, const VkrCaptureBatchRequest *request,
    const VkrCaptureBackendItemPlan *plans, uint64_t source_frame_index,
    VkrBackendResourceHandle *out_buffer);
VkrRendererError vulkan_capture_record_item(
    void *backend_state, VkrCaptureRequestId request_id, uint32_t item_index,
    VkrBackendResourceHandle texture);
VkrCaptureStatus vulkan_capture_poll(void *backend_state,
                                     VkrCaptureRequestId request_id,
                                     VkrCapturePollResult *out_result);
bool8_t vulkan_capture_release(void *backend_state,
                               VkrCaptureRequestId request_id);
void vulkan_capture_submit_active(VulkanBackendState *state,
                                  uint32_t frame_slot,
                                  uint64_t submit_serial);
void vulkan_capture_fail_unsubmitted(VulkanBackendState *state,
                                     VkrRendererError error);

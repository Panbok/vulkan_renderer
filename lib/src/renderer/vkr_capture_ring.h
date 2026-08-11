#pragma once

#include "renderer/vkr_renderer.h"

#define VKR_CAPTURE_RING_CAPACITY_MAX 8u

typedef enum VkrCaptureSlotState {
  VKR_CAPTURE_SLOT_IDLE = 0,
  VKR_CAPTURE_SLOT_RESERVED,
  VKR_CAPTURE_SLOT_SUBMITTED,
  VKR_CAPTURE_SLOT_READY,
  VKR_CAPTURE_SLOT_ACQUIRED,
  VKR_CAPTURE_SLOT_FAILED,
  VKR_CAPTURE_SLOT_ABANDONED,
} VkrCaptureSlotState;

typedef struct VkrCaptureSlot {
  VkrCaptureSlotState state;
  VkrCaptureRequestId request_id;
  VkrRendererError error;
  uint64_t source_frame_index;
  uint64_t submit_serial;
  uint32_t item_count;
  VkrCaptureBackendItemPlan plans[VKR_CAPTURE_MAX_ITEMS];
  VkrCaptureItemResult results[VKR_CAPTURE_MAX_ITEMS];
  const uint8_t *staging;
  uint8_t *storage;
} VkrCaptureSlot;

typedef struct VkrCaptureRing {
  VkrCaptureSlot slots[VKR_CAPTURE_RING_CAPACITY_MAX];
  uint32_t capacity;
  uint64_t max_batch_bytes;
  VkrCaptureRequestId last_request_id;
  bool8_t initialized;
} VkrCaptureRing;

uint64_t vkr_capture_ring_storage_requirement(uint32_t capacity,
                                              uint64_t max_batch_bytes);

bool8_t vkr_capture_ring_init(VkrCaptureRing *ring, uint32_t capacity,
                              uint64_t max_batch_bytes, void *storage,
                              uint64_t storage_size);

VkrRendererError vkr_capture_ring_reserve(
    VkrCaptureRing *ring, const VkrCaptureBatchRequest *request,
    const VkrCaptureBackendItemPlan *plans, uint64_t source_frame_index);

bool8_t vkr_capture_ring_submit(VkrCaptureRing *ring,
                                VkrCaptureRequestId request_id,
                                uint64_t submit_serial, const void *staging);

bool8_t vkr_capture_ring_fail(VkrCaptureRing *ring,
                              VkrCaptureRequestId request_id,
                              VkrRendererError error);

void vkr_capture_ring_collect(VkrCaptureRing *ring,
                              uint64_t completed_submit_serial);

VkrCaptureStatus vkr_capture_ring_poll(VkrCaptureRing *ring,
                                       VkrCaptureRequestId request_id,
                                       uint64_t completed_submit_serial,
                                       VkrCapturePollResult *out_result);

bool8_t vkr_capture_ring_release(VkrCaptureRing *ring,
                                 VkrCaptureRequestId request_id);

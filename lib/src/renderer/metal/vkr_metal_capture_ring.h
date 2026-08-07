#pragma once

#include "renderer/vkr_renderer.h"

#define VKR_METAL_CAPTURE_RING_CAPACITY_MAX 8u

typedef enum VkrMetalCaptureSlotState {
  VKR_METAL_CAPTURE_SLOT_IDLE = 0,
  VKR_METAL_CAPTURE_SLOT_RESERVED,
  VKR_METAL_CAPTURE_SLOT_SUBMITTED,
  VKR_METAL_CAPTURE_SLOT_READY,
  VKR_METAL_CAPTURE_SLOT_ACQUIRED,
  VKR_METAL_CAPTURE_SLOT_FAILED,
  VKR_METAL_CAPTURE_SLOT_ABANDONED,
} VkrMetalCaptureSlotState;

typedef struct VkrMetalCaptureSlot {
  VkrMetalCaptureSlotState state;
  VkrCaptureRequestId request_id;
  VkrRendererError error;
  uint64_t source_frame_index;
  uint64_t submit_serial;
  uint32_t item_count;
  VkrCaptureBackendItemPlan plans[VKR_CAPTURE_MAX_ITEMS];
  VkrCaptureItemResult results[VKR_CAPTURE_MAX_ITEMS];
  const uint8_t *staging;
  uint8_t *storage;
} VkrMetalCaptureSlot;

typedef struct VkrMetalCaptureRing {
  VkrMetalCaptureSlot slots[VKR_METAL_CAPTURE_RING_CAPACITY_MAX];
  uint32_t capacity;
  uint64_t max_batch_bytes;
  VkrCaptureRequestId last_request_id;
  bool8_t initialized;
} VkrMetalCaptureRing;

uint64_t vkr_metal_capture_ring_storage_requirement(uint32_t capacity,
                                                    uint64_t max_batch_bytes);

bool8_t vkr_metal_capture_ring_init(VkrMetalCaptureRing *ring,
                                    uint32_t capacity, uint64_t max_batch_bytes,
                                    void *storage, uint64_t storage_size);

VkrRendererError vkr_metal_capture_ring_reserve(
    VkrMetalCaptureRing *ring, const VkrCaptureBatchRequest *request,
    const VkrCaptureBackendItemPlan *plans, uint64_t source_frame_index);

bool8_t vkr_metal_capture_ring_submit(VkrMetalCaptureRing *ring,
                                      VkrCaptureRequestId request_id,
                                      uint64_t submit_serial,
                                      const void *staging);

bool8_t vkr_metal_capture_ring_fail(VkrMetalCaptureRing *ring,
                                    VkrCaptureRequestId request_id,
                                    VkrRendererError error);

void vkr_metal_capture_ring_collect(VkrMetalCaptureRing *ring,
                                    uint64_t completed_submit_serial);

VkrCaptureStatus vkr_metal_capture_ring_poll(VkrMetalCaptureRing *ring,
                                             VkrCaptureRequestId request_id,
                                             uint64_t completed_submit_serial,
                                             VkrCapturePollResult *out_result);

bool8_t vkr_metal_capture_ring_release(VkrMetalCaptureRing *ring,
                                       VkrCaptureRequestId request_id);

#include "renderer/vkr_capture_ring.h"

vkr_internal void vkr_capture_slot_reset(VkrCaptureSlot *slot) {
  uint8_t *storage = slot->storage;
  MemZero(slot, sizeof(*slot));
  slot->storage = storage;
}

vkr_internal VkrCaptureSlot *
vkr_capture_slot_find(VkrCaptureRing *ring, VkrCaptureRequestId request_id) {
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    if (ring->slots[i].state != VKR_CAPTURE_SLOT_IDLE &&
        ring->slots[i].request_id == request_id) {
      return &ring->slots[i];
    }
  }
  return NULL;
}

uint64_t vkr_capture_ring_storage_requirement(uint32_t capacity,
                                              uint64_t max_batch_bytes) {
  if (capacity == 0 || capacity > VKR_CAPTURE_RING_CAPACITY_MAX ||
      max_batch_bytes == 0 || max_batch_bytes > UINT64_MAX / capacity) {
    return 0;
  }
  return (uint64_t)capacity * max_batch_bytes;
}

bool8_t vkr_capture_ring_init(VkrCaptureRing *ring, uint32_t capacity,
                              uint64_t max_batch_bytes, void *storage,
                              uint64_t storage_size) {
  const uint64_t required =
      vkr_capture_ring_storage_requirement(capacity, max_batch_bytes);
  if (!ring || !storage || required == 0 || storage_size < required) {
    return false_v;
  }
  MemZero(ring, sizeof(*ring));
  ring->capacity = capacity;
  ring->max_batch_bytes = max_batch_bytes;
  ring->initialized = true_v;
  for (uint32_t i = 0; i < capacity; ++i) {
    ring->slots[i].storage = (uint8_t *)storage + i * max_batch_bytes;
  }
  return true_v;
}

VkrRendererError vkr_capture_ring_reserve(
    VkrCaptureRing *ring, const VkrCaptureBatchRequest *request,
    const VkrCaptureBackendItemPlan *plans, uint64_t source_frame_index) {
  if (!ring || !ring->initialized || !request || request->request_id == 0 ||
      !request->items || !plans || request->item_count == 0 ||
      request->item_count > VKR_CAPTURE_MAX_ITEMS ||
      request->request_id <= ring->last_request_id ||
      vkr_capture_slot_find(ring, request->request_id)) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t required = 0;
  for (uint32_t i = 0; i < request->item_count; ++i) {
    const VkrCaptureBackendItemPlan *plan = &plans[i];
    if (plan->result.data_size == 0 ||
        plan->buffer_offset > UINT64_MAX - plan->result.data_size) {
      return VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    const uint64_t end = plan->buffer_offset + plan->result.data_size;
    if (end > required) {
      required = end;
    }
  }
  if (required > ring->max_batch_bytes) {
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }

  VkrCaptureSlot *slot = NULL;
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    if (ring->slots[i].state == VKR_CAPTURE_SLOT_IDLE) {
      slot = &ring->slots[i];
      break;
    }
  }
  if (!slot) {
    return VKR_RENDERER_ERROR_CAPTURE_BUSY;
  }

  slot->state = VKR_CAPTURE_SLOT_RESERVED;
  slot->request_id = request->request_id;
  slot->source_frame_index = source_frame_index;
  slot->item_count = request->item_count;
  MemCopy(slot->plans, plans,
          request->item_count * sizeof(VkrCaptureBackendItemPlan));
  for (uint32_t i = 0; i < request->item_count; ++i) {
    slot->results[i] = plans[i].result;
    slot->results[i].data = NULL;
  }
  ring->last_request_id = request->request_id;
  return VKR_RENDERER_ERROR_NONE;
}

bool8_t vkr_capture_ring_submit(VkrCaptureRing *ring,
                                VkrCaptureRequestId request_id,
                                uint64_t submit_serial, const void *staging) {
  VkrCaptureSlot *slot = ring ? vkr_capture_slot_find(ring, request_id) : NULL;
  if (!slot || slot->state != VKR_CAPTURE_SLOT_RESERVED || submit_serial == 0 ||
      !staging) {
    return false_v;
  }
  slot->state = VKR_CAPTURE_SLOT_SUBMITTED;
  slot->submit_serial = submit_serial;
  slot->staging = staging;
  return true_v;
}

bool8_t vkr_capture_ring_fail(VkrCaptureRing *ring,
                              VkrCaptureRequestId request_id,
                              VkrRendererError error) {
  VkrCaptureSlot *slot = ring ? vkr_capture_slot_find(ring, request_id) : NULL;
  if (!slot || slot->state != VKR_CAPTURE_SLOT_RESERVED ||
      error == VKR_RENDERER_ERROR_NONE) {
    return false_v;
  }
  slot->state = VKR_CAPTURE_SLOT_FAILED;
  slot->error = error;
  return true_v;
}

void vkr_capture_ring_collect(VkrCaptureRing *ring,
                              uint64_t completed_submit_serial) {
  if (!ring || !ring->initialized) {
    return;
  }
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    VkrCaptureSlot *slot = &ring->slots[i];
    if ((slot->state != VKR_CAPTURE_SLOT_SUBMITTED &&
         slot->state != VKR_CAPTURE_SLOT_ABANDONED) ||
        slot->submit_serial == 0 ||
        slot->submit_serial > completed_submit_serial) {
      continue;
    }
    if (slot->state == VKR_CAPTURE_SLOT_ABANDONED) {
      vkr_capture_slot_reset(slot);
      continue;
    }
    for (uint32_t item = 0; item < slot->item_count; ++item) {
      const VkrCaptureBackendItemPlan *plan = &slot->plans[item];
      MemCopy(slot->storage + plan->buffer_offset,
              slot->staging + plan->buffer_offset, plan->result.data_size);
      slot->results[item].data = slot->storage + plan->buffer_offset;
    }
    slot->staging = NULL;
    slot->state = VKR_CAPTURE_SLOT_READY;
  }
}

VkrCaptureStatus vkr_capture_ring_poll(VkrCaptureRing *ring,
                                       VkrCaptureRequestId request_id,
                                       uint64_t completed_submit_serial,
                                       VkrCapturePollResult *out_result) {
  if (out_result) {
    MemZero(out_result, sizeof(*out_result));
  }
  if (!ring || !out_result || request_id == 0) {
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  vkr_capture_ring_collect(ring, completed_submit_serial);
  VkrCaptureSlot *slot = vkr_capture_slot_find(ring, request_id);
  if (!slot || slot->state == VKR_CAPTURE_SLOT_ABANDONED) {
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }

  out_result->error = slot->error;
  out_result->item_count = slot->item_count;
  out_result->source_frame_index = slot->source_frame_index;
  out_result->submit_serial = slot->submit_serial;
  if (slot->state == VKR_CAPTURE_SLOT_FAILED) {
    out_result->status = VKR_CAPTURE_STATUS_FAILED;
  } else if (slot->state == VKR_CAPTURE_SLOT_READY ||
             slot->state == VKR_CAPTURE_SLOT_ACQUIRED) {
    slot->state = VKR_CAPTURE_SLOT_ACQUIRED;
    out_result->status = VKR_CAPTURE_STATUS_READY;
    out_result->items = slot->results;
  } else {
    out_result->status = VKR_CAPTURE_STATUS_PENDING;
  }
  return out_result->status;
}

bool8_t vkr_capture_ring_release(VkrCaptureRing *ring,
                                 VkrCaptureRequestId request_id) {
  VkrCaptureSlot *slot = ring ? vkr_capture_slot_find(ring, request_id) : NULL;
  if (!slot) {
    return false_v;
  }
  if (slot->state == VKR_CAPTURE_SLOT_SUBMITTED) {
    slot->state = VKR_CAPTURE_SLOT_ABANDONED;
  } else {
    vkr_capture_slot_reset(slot);
  }
  return true_v;
}

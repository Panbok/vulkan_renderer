#include "vulkan_capture.h"

#include "vulkan_buffer.h"
#include "vulkan_image.h"

vkr_internal bool8_t vulkan_capture_slot_complete(VulkanBackendState *state,
                                                  VulkanCaptureSlot *slot);

vkr_internal void vulkan_capture_slot_reset(VulkanCaptureSlot *slot) {
  slot->state = VULKAN_CAPTURE_SLOT_IDLE;
  slot->request_id = 0u;
  slot->error = VKR_RENDERER_ERROR_NONE;
  slot->submit_serial = 0u;
  slot->recorded_count = 0u;
  slot->recorded_mask = 0u;
}

vkr_internal VulkanCaptureSlot *
vulkan_capture_slot_find(VulkanCaptureRing *ring,
                         VkrCaptureRequestId request_id) {
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    if (ring->slots[i].state != VULKAN_CAPTURE_SLOT_IDLE &&
        ring->slots[i].request_id == request_id) {
      return &ring->slots[i];
    }
  }
  return NULL;
}

bool8_t vulkan_capture_ring_init(VulkanBackendState *state, uint32_t capacity,
                                 uint64_t max_batch_bytes) {
  if (!state || capacity < BUFFERING_FRAMES ||
      capacity > VKR_CAPTURE_RING_CAPACITY_MAX || max_batch_bytes == 0) {
    return false_v;
  }
  VulkanCaptureRing *ring = &state->capture_ring;
  MemZero(ring, sizeof(*ring));
  ring->capacity = capacity;
  ring->max_batch_bytes = max_batch_bytes;

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  const VkrBufferDescription desc = {
      .size = max_batch_bytes,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_DST),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_CACHED),
      .buffer_type = buffer_type,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_READBACK,
      .bind_on_create = true_v,
      .persistently_mapped = true_v,
  };
  for (uint32_t i = 0; i < capacity; ++i) {
    VulkanCaptureSlot *slot = &ring->slots[i];
    if (!vulkan_buffer_create(state, &desc, &slot->buffer)) {
      vulkan_capture_ring_shutdown(state);
      return false_v;
    }
    slot->buffer.description = desc;
  }
  ring->initialized = true_v;
  return true_v;
}

void vulkan_capture_ring_shutdown(VulkanBackendState *state) {
  if (!state) {
    return;
  }
  VulkanCaptureRing *ring = &state->capture_ring;
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    vulkan_buffer_destroy(state, &ring->slots[i].buffer.buffer);
  }
  MemZero(ring, sizeof(*ring));
}

VkrRendererError vulkan_capture_reserve(void *backend_state,
                                        const VkrCaptureBatchRequest *request,
                                        const VkrCaptureBackendItemPlan *plans,
                                        uint64_t source_frame_index,
                                        VkrBackendResourceHandle *out_buffer) {
  VulkanBackendState *state = backend_state;
  if (!state || !request || request->request_id == 0 || !request->items ||
      !plans || !out_buffer || request->item_count == 0 ||
      request->item_count > VKR_CAPTURE_MAX_ITEMS ||
      !state->capture_ring.initialized) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  VulkanCaptureRing *ring = &state->capture_ring;
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    VulkanCaptureSlot *candidate = &ring->slots[i];
    if (candidate->state == VULKAN_CAPTURE_SLOT_ABANDONED &&
        candidate->submit_serial != 0u &&
        vulkan_capture_slot_complete(state, candidate)) {
      vulkan_capture_slot_reset(candidate);
    }
  }
  /* Renderer-issued IDs are monotonic. This bounded rule rejects both stale
     and duplicate IDs without an ever-growing lifetime registry. */
  if (request->request_id <= ring->last_request_id) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  if (vulkan_capture_slot_find(ring, request->request_id)) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  VulkanCaptureSlot *slot = NULL;
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    if (ring->slots[i].state == VULKAN_CAPTURE_SLOT_IDLE) {
      slot = &ring->slots[i];
      break;
    }
  }
  if (!slot) {
    return VKR_RENDERER_ERROR_CAPTURE_BUSY;
  }

  uint64_t required = 0;
  for (uint32_t i = 0; i < request->item_count; ++i) {
    if (plans[i].result.data_size == 0 ||
        plans[i].buffer_offset > UINT64_MAX - plans[i].result.data_size) {
      return VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    uint64_t end = plans[i].buffer_offset + plans[i].result.data_size;
    if (end > required) {
      required = end;
    }
  }
  if (required > ring->max_batch_bytes) {
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }

  slot->state = VULKAN_CAPTURE_SLOT_RESERVED;
  slot->request_id = request->request_id;
  slot->error = VKR_RENDERER_ERROR_NONE;
  slot->source_frame_index = source_frame_index;
  slot->submit_serial = 0;
  slot->item_count = request->item_count;
  slot->recorded_count = 0;
  slot->recorded_mask = 0u;
  MemCopy(slot->plans, plans,
          sizeof(VkrCaptureBackendItemPlan) * request->item_count);
  for (uint32_t i = 0; i < request->item_count; ++i) {
    slot->results[i] = plans[i].result;
    slot->results[i].data = NULL;
  }
  *out_buffer = (VkrBackendResourceHandle){.ptr = &slot->buffer};
  ring->last_request_id = request->request_id;
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError
vulkan_capture_record_item(void *backend_state, VkrCaptureRequestId request_id,
                           uint32_t item_index,
                           VkrBackendResourceHandle texture_handle) {
  VulkanBackendState *state = backend_state;
  if (!state || !texture_handle.ptr) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  VulkanCaptureSlot *slot =
      vulkan_capture_slot_find(&state->capture_ring, request_id);
  if (!slot || slot->state != VULKAN_CAPTURE_SLOT_RESERVED ||
      item_index >= slot->item_count ||
      (slot->recorded_mask & (1ull << item_index)) != 0u) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  struct s_TextureHandle *texture = texture_handle.ptr;
  const VkrCaptureBackendItemPlan *plan = &slot->plans[item_index];
  /* The plan was laid out before graph construction from the producer's
     declared configuration. Anything the resolved texture disagrees with is an
     unavailable channel, not a recording failure: the reserved byte range would
     not describe this image. */
  const VulkanImage *image = &texture->texture.image;
  const uint32_t mip_width = vkr_max_u32(1u, image->width >> plan->result.mip);
  const uint32_t mip_height =
      vkr_max_u32(1u, image->height >> plan->result.mip);
  if (texture->description.sample_count != VKR_SAMPLE_COUNT_1 ||
      plan->result.mip >= image->mip_levels ||
      plan->result.layer >= image->array_layers ||
      texture->description.format != plan->result.format ||
      plan->result.width != mip_width || plan->result.height != mip_height) {
    slot->state = VULKAN_CAPTURE_SLOT_FAILED;
    slot->error = VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE;
    return slot->error;
  }
  VulkanCommandBuffer *command =
      vulkan_backend_get_active_graphics_command_buffer(state);
  /* Rows are tightly packed at the plan's pitch, so the buffer row length is
     the copied width in texels. */
  const VulkanImageCopyToBufferRegion region = {
      .buffer_offset = plan->buffer_offset,
      .buffer_row_length = plan->result.width,
      .buffer_image_height = plan->result.height,
      .width = plan->result.width,
      .height = plan->result.height,
      .mip_level = plan->result.mip,
      .base_array_layer = plan->result.layer,
      .layer_count = 1,
      .aspect_flags = plan->result.value_kind == VKR_CAPTURE_VALUE_DEPTH
                          ? VK_IMAGE_ASPECT_DEPTH_BIT
                          : VK_IMAGE_ASPECT_COLOR_BIT,
  };
  if (!command || !vulkan_image_copy_to_buffer_region(
                      state, &texture->texture.image,
                      slot->buffer.buffer.handle, &region, command)) {
    slot->state = VULKAN_CAPTURE_SLOT_FAILED;
    slot->error = VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
    return slot->error;
  }
  slot->recorded_count++;
  slot->recorded_mask |= 1ull << item_index;
  if (slot->recorded_count == slot->item_count) {
    slot->state = VULKAN_CAPTURE_SLOT_RECORDED;
  }
  return VKR_RENDERER_ERROR_NONE;
}

void vulkan_capture_submit_active(VulkanBackendState *state,
                                  uint32_t frame_slot, uint64_t submit_serial) {
  VulkanCaptureRing *ring = &state->capture_ring;
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    VulkanCaptureSlot *slot = &ring->slots[i];
    if (slot->state == VULKAN_CAPTURE_SLOT_RECORDED ||
        (slot->state == VULKAN_CAPTURE_SLOT_ABANDONED &&
         slot->submit_serial == 0u &&
         slot->recorded_count == slot->item_count)) {
      if (slot->state == VULKAN_CAPTURE_SLOT_RECORDED) {
        slot->state = VULKAN_CAPTURE_SLOT_SUBMITTED;
      }
      slot->submit_frame_slot = frame_slot;
      slot->submit_serial = submit_serial;
    }
  }
}

void vulkan_capture_fail_unsubmitted(VulkanBackendState *state,
                                     VkrRendererError error) {
  VulkanCaptureRing *ring = &state->capture_ring;
  for (uint32_t i = 0; i < ring->capacity; ++i) {
    VulkanCaptureSlot *slot = &ring->slots[i];
    if (slot->state == VULKAN_CAPTURE_SLOT_RESERVED ||
        slot->state == VULKAN_CAPTURE_SLOT_RECORDED) {
      slot->state = VULKAN_CAPTURE_SLOT_FAILED;
      slot->error = error;
    } else if (slot->state == VULKAN_CAPTURE_SLOT_ABANDONED &&
               slot->submit_serial == 0u) {
      vulkan_capture_slot_reset(slot);
    }
  }
}

vkr_internal bool8_t vulkan_capture_slot_complete(VulkanBackendState *state,
                                                  VulkanCaptureSlot *slot) {
  if (state->completed_submit_serial >= slot->submit_serial) {
    return true_v;
  }
  if (slot->submit_frame_slot >= state->in_flight_fences.length) {
    return false_v;
  }
  VulkanFence *fence =
      array_get_VulkanFence(&state->in_flight_fences, slot->submit_frame_slot);
  return vkGetFenceStatus(state->device.logical_device, fence->handle) ==
         VK_SUCCESS;
}

VkrCaptureStatus vulkan_capture_poll(void *backend_state,
                                     VkrCaptureRequestId request_id,
                                     VkrCapturePollResult *out_result) {
  VulkanBackendState *state = backend_state;
  if (out_result) {
    MemZero(out_result, sizeof(*out_result));
  }
  if (!state || !out_result || request_id == 0) {
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  VulkanCaptureSlot *slot =
      vulkan_capture_slot_find(&state->capture_ring, request_id);
  if (!slot) {
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  /* Release may race the narrow interval between command recording and queue
     association. Serial zero has no completion meaning; retain the abandoned
     slot until submit or frame-cancel assigns its real terminal owner. */
  if (slot->state == VULKAN_CAPTURE_SLOT_ABANDONED &&
      slot->submit_serial == 0u) {
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  if ((slot->state == VULKAN_CAPTURE_SLOT_SUBMITTED ||
       slot->state == VULKAN_CAPTURE_SLOT_ABANDONED) &&
      vulkan_capture_slot_complete(state, slot)) {
    if (slot->state == VULKAN_CAPTURE_SLOT_ABANDONED) {
      vulkan_capture_slot_reset(slot);
      return VKR_CAPTURE_STATUS_NOT_FOUND;
    }
    if (!(slot->buffer.buffer.memory_property_flags &
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      VkMappedMemoryRange range = {
          .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
          .memory = slot->buffer.buffer.memory,
          .offset = 0,
          .size = VK_WHOLE_SIZE,
      };
      if (vkInvalidateMappedMemoryRanges(state->device.logical_device, 1,
                                         &range) != VK_SUCCESS) {
        slot->state = VULKAN_CAPTURE_SLOT_FAILED;
        slot->error = VKR_RENDERER_ERROR_DEVICE_ERROR;
      }
    }
    if (slot->state == VULKAN_CAPTURE_SLOT_SUBMITTED) {
      for (uint32_t i = 0; i < slot->item_count; ++i) {
        slot->results[i].data = (uint8_t *)slot->buffer.buffer.mapped_ptr +
                                slot->plans[i].buffer_offset;
      }
      slot->state = VULKAN_CAPTURE_SLOT_READY;
    }
  }

  out_result->error = slot->error;
  out_result->item_count = slot->item_count;
  out_result->source_frame_index = slot->source_frame_index;
  out_result->submit_serial = slot->submit_serial;
  if (slot->state == VULKAN_CAPTURE_SLOT_FAILED) {
    out_result->status = VKR_CAPTURE_STATUS_FAILED;
  } else if (slot->state == VULKAN_CAPTURE_SLOT_READY ||
             slot->state == VULKAN_CAPTURE_SLOT_ACQUIRED) {
    slot->state = VULKAN_CAPTURE_SLOT_ACQUIRED;
    out_result->status = VKR_CAPTURE_STATUS_READY;
    out_result->items = slot->results;
  } else {
    out_result->status = VKR_CAPTURE_STATUS_PENDING;
  }
  return out_result->status;
}

bool8_t vulkan_capture_release(void *backend_state,
                               VkrCaptureRequestId request_id) {
  VulkanBackendState *state = backend_state;
  if (!state) {
    return false_v;
  }
  VulkanCaptureSlot *slot =
      vulkan_capture_slot_find(&state->capture_ring, request_id);
  if (!slot) {
    return false_v;
  }
  if (slot->state == VULKAN_CAPTURE_SLOT_SUBMITTED ||
      slot->state == VULKAN_CAPTURE_SLOT_RECORDED) {
    slot->state = VULKAN_CAPTURE_SLOT_ABANDONED;
    return true_v;
  }
  vulkan_capture_slot_reset(slot);
  return true_v;
}

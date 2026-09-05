/* Bounded command-slot acquisition, completion, timing retention, and uploads.
 */
vkr_internal bool8_t vkr_metal_packet_wait(VkrMetalPacketRenderer *renderer,
                                           uint64_t value) {
  return
      [renderer->completion waitUntilSignaledValue:value
                                         timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS]
          ? true_v
          : false_v;
}

vkr_internal void
vkr_metal_packet_collect_encoder_timings(VkrMetalPacketRenderer *renderer,
                                         VkrMetalPacketCommandSlot *slot) {
  if (!slot->pass_timing_requested || !slot->timestamp_heap ||
      slot->timestamp_entry_count == 0u || renderer->timestamp_frequency == 0u)
    return;
  NSData *resolved = [slot->timestamp_heap
      resolveCounterRange:NSMakeRange(0u, slot->timestamp_entry_count)];
  if (!resolved || resolved.length < (NSUInteger)slot->timestamp_entry_count *
                                         sizeof(MTL4TimestampHeapEntry))
    return;
  const MTL4TimestampHeapEntry *entries = resolved.bytes;
  const float64_t to_ms = 1000.0 / (float64_t)renderer->timestamp_frequency;
  for (uint32_t i = 0u; i < slot->pending_result.pass_timing_count; ++i) {
    VkrMetalPacketPassTiming *timing = &slot->pending_result.pass_timings[i];
    if (timing->unavailable_reason !=
        VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY)
      continue;
    const uint32_t begin_index = timing->pass_index * 2u;
    if (begin_index + 1u >= slot->timestamp_entry_count)
      continue;
    const uint64_t begin = entries[begin_index].timestamp;
    const uint64_t end = entries[begin_index + 1u].timestamp;
    if (begin > 0u && end > begin) {
      timing->gpu_ms = (float64_t)(end - begin) * to_ms;
      timing->valid = true_v;
      timing->unavailable_reason = VKR_RENDERER_IMPL_GPU_TIMING_REASON_NONE;
    } else {
      timing->unavailable_reason =
          VKR_RENDERER_IMPL_GPU_TIMING_REASON_UNSUPPORTED_TIMESTAMP_SCOPE;
    }
  }
}

vkr_internal void
vkr_metal_packet_cache_completed_result(VkrMetalPacketRenderer *renderer,
                                        const VkrMetalPacketResult *result) {
  renderer->completed_timing_results[renderer->next_completed_timing] = *result;
  renderer->next_completed_timing = (renderer->next_completed_timing + 1u) %
                                    VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
}

vkr_internal VkrMetalPacketCommitFeedbackRecord *
vkr_metal_packet_acquire_commit_feedback(VkrMetalPacketRenderer *renderer) {
  for (uint32_t offset = 0u; offset < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
       ++offset) {
    const uint32_t index = (renderer->next_commit_feedback + offset) %
                           VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
    VkrMetalPacketCommitFeedbackRecord *record =
        &renderer->commit_feedback_records[index];
    if (record->in_use)
      continue;
    record->result = (VkrMetalPacketResult){0};
    record->gpu_start_time = 0.0;
    record->gpu_end_time = 0.0;
    record->has_error = false_v;
    vkr_atomic_bool_store(&record->feedback_ready, false_v,
                          VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_bool_store(&record->result_ready, false_v,
                          VKR_MEMORY_ORDER_RELAXED);
    [record->options addFeedbackHandler:record->handler];
    record->in_use = true_v;
    renderer->next_commit_feedback =
        (index + 1u) % VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
    return record;
  }
  return NULL;
}

vkr_internal void
vkr_metal_packet_harvest_commit_feedback(VkrMetalPacketRenderer *renderer) {
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY; ++i) {
    VkrMetalPacketCommitFeedbackRecord *record =
        &renderer->commit_feedback_records[i];
    if (!record->in_use ||
        !vkr_atomic_bool_load(&record->feedback_ready,
                              VKR_MEMORY_ORDER_ACQUIRE) ||
        !vkr_atomic_bool_load(&record->result_ready, VKR_MEMORY_ORDER_ACQUIRE))
      continue;
    VkrMetalPacketResult *result = &record->result;
    const CFTimeInterval duration =
        record->gpu_end_time - record->gpu_start_time;
    if (!record->has_error && isfinite(record->gpu_start_time) &&
        isfinite(record->gpu_end_time) && record->gpu_start_time > 0.0 &&
        duration > 0.0 &&
        duration <= (CFTimeInterval)UINT64_MAX / 1000000000.0) {
      result->gpu_submission_ns = (uint64_t)(duration * 1000000000.0 + 0.5);
      result->gpu_submission_valid = true_v;
      result->gpu_submission_unavailable_reason =
          VKR_RENDERER_IMPL_GPU_TIMING_REASON_NONE;
    } else {
      result->gpu_submission_ns = 0u;
      result->gpu_submission_valid = false_v;
      result->gpu_submission_unavailable_reason =
          VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_ERROR;
    }
    vkr_metal_packet_cache_completed_result(renderer, result);
    record->in_use = false_v;
  }
}

vkr_internal void
vkr_metal_packet_cache_slot_result(VkrMetalPacketRenderer *renderer,
                                   VkrMetalPacketCommandSlot *slot) {
  if (!slot->result_pending || slot->result_collected)
    return;
  vkr_metal_packet_collect_encoder_timings(renderer, slot);
  if (slot->gpu_draw_diagnostics_requested &&
      slot->gpu_draw_diagnostics_readback) {
    vkr_metal_packet_decode_gpu_draw_diagnostics(
        &slot->pending_result, slot->gpu_draw_diagnostics_readback,
        slot->shadow_cascade_count, slot->transmission_coverage_readback);
  }
  if (slot->sdsm_requested && slot->sdsm_readback)
    vkr_metal_packet_decode_sdsm(&slot->pending_result, slot->sdsm_readback);
  if (slot->exposure_requested && slot->exposure_readback) {
    const uint8_t *readback = slot->exposure_readback;
    MemCopy(&slot->pending_result.exposure.state, readback,
            sizeof(slot->pending_result.exposure.state));
    MemCopy(&slot->pending_result.exposure.histogram,
            readback + sizeof(VkrExposureGpuState),
            sizeof(slot->pending_result.exposure.histogram));
    slot->pending_result.exposure.source_frame_index =
        slot->pending_result.source_frame_index;
    slot->pending_result.exposure.source_submit_value = slot->submit_value;
    slot->pending_result.exposure.valid = true_v;
    (void)vkr_capture_ring_set_display_exposure(
        &renderer->capture_ring, slot->submit_value,
        slot->pending_result.exposure.state.exposure_multiplier);
  }
  if (slot->transmission_coverage_requested &&
      slot->transmission_coverage_readback) {
    const VkrMetalPacketTransmissionDiagnostics *diagnostics =
        (const VkrMetalPacketTransmissionDiagnostics *)
            slot->transmission_coverage_readback;
    MemCopy(slot->pending_result.transmission_covered_pixels,
            diagnostics->covered_pixels,
            sizeof(slot->pending_result.transmission_covered_pixels));
    MemCopy(slot->pending_result.transmission_coverage_extent,
            slot->transmission_coverage_extent,
            sizeof(slot->pending_result.transmission_coverage_extent));
    for (uint32_t layer = 0u; layer < VKR_METAL_PACKET_TRANSMISSION_LAYER_COUNT;
         ++layer) {
      slot->pending_result.transmission_compact_overflow_count +=
          diagnostics->compact_overflow[layer];
    }
    slot->pending_result.has_transmission_coverage = true_v;
  }
  if (slot->commit_feedback) {
    slot->commit_feedback->result = slot->pending_result;
    vkr_atomic_bool_store(&slot->commit_feedback->result_ready, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
  } else {
    vkr_metal_packet_cache_completed_result(renderer, &slot->pending_result);
  }
  slot->result_collected = true_v;
}

vkr_internal void
vkr_metal_packet_collect_completed_results(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  const uint64_t completed = renderer->completion.signaledValue;
  for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
    VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
    if (slot->submit_value != 0 && slot->submit_value <= completed)
      vkr_metal_packet_cache_slot_result(renderer, slot);
  }
  vkr_metal_packet_harvest_commit_feedback(renderer);
  /* Retired coefficient slots return to the pool only once their greatest
     reader serial is proven complete -- no assumed frame lag. */
  (void)vkr_ibl_sh_pool_collect(&renderer->sh_pool, completed);
}

vkr_internal const VkrMetalPacketResult *
vkr_metal_packet_find_completed_timing(const VkrMetalPacketRenderer *renderer,
                                       uint64_t submit_value) {
  for (uint32_t i = 0; i < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY; ++i) {
    const VkrMetalPacketResult *result = &renderer->completed_timing_results[i];
    if (result->submit_value == submit_value)
      return result;
  }
  return NULL;
}

vkr_internal bool8_t vkr_metal_packet_complete_command_slot(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketCommandSlot *slot,
    bool8_t upload_path) {
  const uint64_t wait_value =
      MAX(slot->submit_value, slot->resource_reuse_submit_value);
  if (wait_value == 0)
    return true_v;
  if (renderer->completion.signaledValue < wait_value) {
    vkr_metal_packet_wait_counters_record(&renderer->wait_counters,
                                          upload_path);
    if (!vkr_metal_packet_wait(renderer, wait_value))
      return false_v;
  }
  vkr_metal_packet_cache_slot_result(renderer, slot);
  vkr_capture_ring_collect(&renderer->capture_ring,
                           renderer->completion.signaledValue);
  [slot->allocator reset];
  slot->submit_value = 0;
  slot->resource_reuse_submit_value = 0;
  slot->timestamp_entry_count = 0u;
  slot->pass_timing_requested = false_v;
  slot->result_pending = false_v;
  slot->result_collected = false_v;
  slot->commit_feedback = NULL;
  slot->gpu_draw_diagnostics_readback = NULL;
  slot->transmission_coverage_readback = NULL;
  slot->sdsm_readback = NULL;
  slot->exposure_readback = NULL;
  slot->shadow_cascade_count = 0u;
  slot->transmission_coverage_extent[0] = 0u;
  slot->transmission_coverage_extent[1] = 0u;
  slot->gpu_draw_diagnostics_requested = false_v;
  slot->transmission_coverage_requested = false_v;
  slot->sdsm_requested = false_v;
  slot->exposure_requested = false_v;
  slot->pending_result = (VkrMetalPacketResult){0};
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_acquire_command_slot(
    VkrMetalPacketRenderer *renderer, bool8_t upload_path) {
  if (renderer->active_command_slot)
    return false_v;
  VkrMetalPacketCommandSlot *slot =
      &renderer->command_slots[renderer->next_command_slot];
  if (!vkr_metal_packet_complete_command_slot(renderer, slot, upload_path))
    return false_v;
  renderer->next_command_slot =
      (renderer->next_command_slot + 1u) % renderer->command_slot_count;
  renderer->active_command_slot = slot;
  renderer->command_allocator = slot->allocator;
  renderer->command_buffer = slot->buffer;
  return true_v;
}

vkr_internal void vkr_metal_packet_release_unsubmitted_command_slot(
    VkrMetalPacketRenderer *renderer) {
  [renderer->active_command_slot->allocator reset];
  renderer->active_command_slot = NULL;
  renderer->command_allocator = nil;
  renderer->command_buffer = nil;
}

vkr_internal void
vkr_metal_packet_detach_active_command_slot(VkrMetalPacketRenderer *renderer) {
  renderer->active_command_slot = NULL;
  renderer->command_allocator = nil;
  renderer->command_buffer = nil;
}

vkr_internal void
vkr_metal_packet_discard_unsubmitted_uses(VkrMetalPacketRenderer *renderer) {
  const uint64_t submitted = renderer->submit_value;
  for (uint32_t i = 0; i < renderer->max_meshes; ++i) {
    if (renderer->meshes[i].last_use_submit_value > submitted)
      renderer->meshes[i].last_use_submit_value = submitted;
  }
  for (uint32_t i = 0; i < renderer->max_materials; ++i) {
    if (renderer->packet_materials[i].last_use_submit_value > submitted)
      renderer->packet_materials[i].last_use_submit_value = submitted;
  }
  for (uint32_t i = 0; i < renderer->max_textures; ++i) {
    if (renderer->textures[i].last_use_submit_value > submitted)
      renderer->textures[i].last_use_submit_value = submitted;
  }
  if (renderer->ibl_last_use_submit_value > submitted)
    renderer->ibl_last_use_submit_value = submitted;
}

vkr_internal void
vkr_metal_packet_finish_frame(VkrMetalPacketRenderer *renderer,
                              bool8_t graph_active, bool8_t submitted) {
  if (!submitted)
    vkr_metal_packet_discard_unsubmitted_uses(renderer);
  if (graph_active)
    vkr_rg_end_frame(renderer->graph);
  [renderer->drawable release];
  renderer->drawable = nil;
  renderer->frame_prepared = false_v;
  if (!submitted)
    vkr_metal_packet_release_unsubmitted_command_slot(renderer);
}

vkr_internal bool8_t vkr_metal_packet_texture_upload_batch_start(
    VkrMetalPacketRenderer *renderer, uint64_t capacity)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketTextureUploadBatch *batch = &renderer->texture_upload_batch;
  if (batch->active)
    return true_v;
  if (!batch->enabled || renderer->frame_prepared ||
      !vkr_metal_packet_acquire_command_slot(renderer, true_v))
    return false_v;

  void *upload_buffer = NULL;
  if (vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_UPLOAD, capacity,
          renderer->completion.signaledValue, &batch->slice, &batch->upload,
          &upload_buffer) != VKR_METAL_MEMORY_STATUS_OK) {
    vkr_metal_packet_release_unsubmitted_command_slot(renderer);
    return false_v;
  }
  batch->upload_buffer = (id<MTLBuffer>)upload_buffer;
  batch->capacity = capacity;
  [renderer->command_buffer
      beginCommandBufferWithAllocator:renderer->command_allocator];
  [renderer->command_buffer
      useResidencySet:(id<MTLResidencySet>)
                          vkr_metal_memory_device_residency_set(
                              renderer->memory)];
  batch->encoder = [renderer->command_buffer computeCommandEncoder];
  if (!batch->encoder) {
    [renderer->command_buffer endCommandBuffer];
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_UPLOAD, batch->slice);
    vkr_metal_packet_release_unsubmitted_command_slot(renderer);
    *batch = (VkrMetalPacketTextureUploadBatch){.enabled = true_v};
    return false_v;
  }
  batch->active = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_texture_upload_batch_flush(
    VkrMetalPacketRenderer *renderer, bool8_t finish)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketTextureUploadBatch *batch = &renderer->texture_upload_batch;
  if (!batch->active) {
    batch->enabled = finish ? false_v : batch->enabled;
    return true_v;
  }
  [batch->encoder endEncoding];
  [renderer->command_buffer endCommandBuffer];
  if (batch->texture_count == 0u) {
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_UPLOAD, batch->slice);
    vkr_metal_packet_release_unsubmitted_command_slot(renderer);
    *batch = (VkrMetalPacketTextureUploadBatch){.enabled =
                                                    finish ? false_v : true_v};
    return true_v;
  }
  const uint64_t submit_value = ++renderer->submit_value;
  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  slot->submit_value = submit_value;
  id<MTL4CommandBuffer> submissions[] = {renderer->command_buffer};
  [renderer->queue commit:submissions count:1];
  vkr_metal_packet_advance_revision(&renderer->radiance_revision);
  [renderer->queue signalEvent:renderer->completion value:submit_value];
  const bool8_t submitted =
      vkr_metal_memory_device_submit_ring(
          renderer->memory, VKR_METAL_RING_KIND_UPLOAD, batch->slice,
          submit_value) == VKR_METAL_MEMORY_STATUS_OK;
  if (!submitted) {
    const bool8_t completed = vkr_metal_packet_wait(renderer, submit_value);
    if (completed) {
      vkr_metal_memory_device_cancel_ring(
          renderer->memory, VKR_METAL_RING_KIND_UPLOAD, batch->slice);
      (void)vkr_metal_packet_complete_command_slot(renderer, slot, false_v);
    }
    vkr_metal_packet_detach_active_command_slot(renderer);
    *batch = (VkrMetalPacketTextureUploadBatch){.enabled =
                                                    finish ? false_v : true_v};
    return false_v;
  }
  vkr_metal_packet_detach_active_command_slot(renderer);
  *batch =
      (VkrMetalPacketTextureUploadBatch){.enabled = finish ? false_v : true_v};
  return !finish || vkr_metal_packet_wait(renderer, submit_value);
}

bool8_t vkr_metal_packet_renderer_begin_texture_upload_batch(
    VkrMetalPacketRenderer *renderer) {
  if (!renderer || renderer->frame_prepared ||
      renderer->texture_upload_batch.enabled)
    return false_v;
  renderer->pending_texture_upload_bytes = 0u;
  renderer->texture_upload_batch.enabled = true_v;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_end_texture_upload_batch(
    VkrMetalPacketRenderer *renderer) {
  if (!renderer || !renderer->texture_upload_batch.enabled)
    return false_v;
  if (!@available(macOS 26.0, *))
    return false_v;
  return vkr_metal_packet_texture_upload_batch_flush(renderer, true_v);
}

vkr_internal bool8_t vkr_metal_packet_begin_upload(
    VkrMetalPacketRenderer *renderer, uint64_t size,
    VkrMetalRingSlice *out_slice, VkrMetalAddressPair *out_upload,
    id<MTLBuffer> *out_buffer, id<MTL4ComputeCommandEncoder> *out_encoder)
    API_AVAILABLE(macos(26.0)) {
  if (renderer->texture_upload_batch.active &&
      !vkr_metal_packet_texture_upload_batch_flush(renderer, false_v))
    return false_v;
  if (!vkr_metal_packet_acquire_command_slot(renderer, true_v))
    return false_v;
  void *buffer = NULL;
  if (vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_UPLOAD, size,
          renderer->completion.signaledValue, out_slice, out_upload,
          &buffer) != VKR_METAL_MEMORY_STATUS_OK) {
    vkr_metal_packet_release_unsubmitted_command_slot(renderer);
    return false_v;
  }
  *out_buffer = (id<MTLBuffer>)buffer;
  [renderer->command_buffer
      beginCommandBufferWithAllocator:renderer->command_allocator];
  [renderer->command_buffer
      useResidencySet:(id<MTLResidencySet>)
                          vkr_metal_memory_device_residency_set(
                              renderer->memory)];
  *out_encoder = [renderer->command_buffer computeCommandEncoder];
  if (*out_encoder)
    return true_v;
  [renderer->command_buffer endCommandBuffer];
  vkr_metal_memory_device_cancel_ring(renderer->memory,
                                      VKR_METAL_RING_KIND_UPLOAD, *out_slice);
  vkr_metal_packet_release_unsubmitted_command_slot(renderer);
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_submit_upload(
    VkrMetalPacketRenderer *renderer, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalRingSlice slice) API_AVAILABLE(macos(26.0)) {
  [encoder endEncoding];
  [renderer->command_buffer endCommandBuffer];
  const uint64_t submit_value = ++renderer->submit_value;
  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  slot->submit_value = submit_value;
  id<MTL4CommandBuffer> submissions[] = {renderer->command_buffer};
  [renderer->queue commit:submissions count:1];
  vkr_metal_packet_advance_revision(&renderer->radiance_revision);
  [renderer->queue signalEvent:renderer->completion value:submit_value];
  if (vkr_metal_memory_device_submit_ring(
          renderer->memory, VKR_METAL_RING_KIND_UPLOAD, slice, submit_value) !=
      VKR_METAL_MEMORY_STATUS_OK) {
    const bool8_t completed = vkr_metal_packet_wait(renderer, submit_value);
    if (completed) {
      vkr_metal_memory_device_cancel_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_UPLOAD, slice);
      (void)vkr_metal_packet_complete_command_slot(renderer, slot, false_v);
    }
    vkr_metal_packet_detach_active_command_slot(renderer);
    return false_v;
  }
  vkr_metal_packet_detach_active_command_slot(renderer);
  return true_v;
}

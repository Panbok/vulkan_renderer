/* Bounded command-slot acquisition, completion, timing retention, and uploads.
 */
vkr_internal void
vkr_metal_packet_wait_timeout_exit(VkrMetalPacketRenderer *renderer,
                                   const char *queue, uint64_t value) {
  VKR_METAL_DIAGNOSTIC(renderer, "completion_timeout_exit",
                       "queue=%s wait=%llu timeout_ms=%u", queue,
                       (unsigned long long)value, VKR_METAL_PACKET_TIMEOUT_MS);
  VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  char message[512];
  string_format(message, sizeof(message),
                "[FATAL]: Metal GPU completion timed out after %u ms "
                "(%s wait=%llu completed=%llu submitted=%llu). "
                "Exiting without GPU teardown; unsaved edits are lost.\n",
                VKR_METAL_PACKET_TIMEOUT_MS, queue, (unsigned long long)value,
                (unsigned long long)renderer->completion.signaledValue,
                (unsigned long long)renderer->submit_value);
  vkr_platform_stderr_write(message);
  /* Completion is unproven. Neither another wait nor normal teardown may
     recycle resources still referenced by the GPU. Skip exit handlers too. */
  vkr_platform_exit_immediate(1);
}

/* Waits until resources used up to frame-queue value `value` are free: the
   frame queue reaches it, and so does every presenter frame that sampled
   textures tagged with it (ADR-111). */
vkr_internal bool8_t vkr_metal_packet_wait(VkrMetalPacketRenderer *renderer,
                                           uint64_t value) {
  VKR_METAL_DIAGNOSTIC(renderer, "wait_begin", "wait=%llu",
                       (unsigned long long)value);
  VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  /* A presenter tag runs one value ahead of the frame queue's submissions;
     only presenter frames hold it. */
  const uint64_t frame_value = Min(value, renderer->submit_value);
  const bool8_t completed =
      [renderer->completion waitUntilSignaledValue:frame_value
                                         timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS]
          ? true_v
          : false_v;
  VKR_METAL_DIAGNOSTIC(renderer, "wait_end", "wait=%llu success=%u",
                       (unsigned long long)value, completed);
  if (!completed)
    vkr_metal_packet_wait_timeout_exit(renderer, "frame", frame_value);
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  for (uint32_t i = 0u; presenter->ready && i < ArrayCount(presenter->slots);
       ++i) {
    const VkrMetalPacketPresentSlot *slot = &presenter->slots[i];
    if (slot->frame_mark >= value ||
        presenter->completion.signaledValue >= slot->present_value)
      continue;
    if (![presenter->completion
            waitUntilSignaledValue:slot->present_value
                         timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS])
      vkr_metal_packet_wait_timeout_exit(renderer, "present",
                                         slot->present_value);
  }
  VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  return completed;
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
  /* Timings are in execution order. Overlap between a graphics pass's early
     start stamp and earlier encoders is counted once, in the earlier pass. */
  uint64_t previous_end = 0u;
  for (uint32_t i = 0u; i < slot->pending_result.pass_timing_count; ++i) {
    VkrMetalPacketPassTiming *timing = &slot->pending_result.pass_timings[i];
    if (timing->unavailable_reason !=
        VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY)
      continue;
    const uint32_t begin_index = timing->pass_index * 2u;
    if (begin_index + 1u >= slot->timestamp_entry_count)
      continue;
    uint64_t begin = entries[begin_index].timestamp;
    const uint64_t end = entries[begin_index + 1u].timestamp;
    if (begin > 0u && end > begin) {
      if (timing->starts_after_previous && previous_end > begin)
        begin = Min(previous_end, end);
      previous_end = Max(previous_end, end);
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
    if (!record->has_error && vkr_is_finite_f64(record->gpu_start_time) &&
        vkr_is_finite_f64(record->gpu_end_time) &&
        record->gpu_start_time > 0.0 && duration > 0.0 &&
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

/* Marks `layer` of every instance of the one-mip retained image `name`
   as holding no content, so the next frame that reads it draws it again. */
vkr_internal void
vkr_metal_packet_invalidate_retained_layer(VkrMetalPacketRenderer *renderer,
                                           const char *name, uint32_t layer) {
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image = array_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !vkr_string8_equals_cstr(&image->name, name))
      continue;
    VkrMetalPacketImage *slot = &renderer->images[i];
    if (!slot->live || slot->desc.mip_levels != 1u ||
        layer >= MIN(slot->desc.layers, VKR_METAL_PACKET_MAX_TEXTURE_LAYERS))
      return;
    for (uint32_t instance = 0u; instance < slot->instance_count; ++instance)
      slot->instances[instance].retained_states[layer].content_valid = false_v;
    return;
  }
}

/* Grows the GPU-draw command pool from the commands a completed frame's
   views needed, the visible draws and those the pool cut, per ICB group:
   once a frame needs two thirds of the target, the target becomes twice
   that need, so a pool grows in few, large steps. A cut view drew a shadow
   layer later frames would reuse without the cut casters, so that layer
   draws again; a cut camera view loses those draws for its frame only. */
vkr_internal void
vkr_metal_packet_gpu_draw_pool_feedback(VkrMetalPacketRenderer *renderer,
                                        const VkrMetalPacketCommandSlot *slot) {
  const VkrGpuDrawCompactionState *states =
      (const VkrGpuDrawCompactionState *)slot->gpu_draw_diagnostics_readback;
  const uint32_t view_count =
      MIN(slot->gpu_draw_view_count, VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX);
  const uint32_t group_size = renderer->gpu_draw_icb_view_group_size;
  uint64_t needed = 0u;
  uint64_t group_needed = 0u;
  for (uint32_t view = 0u; view < view_count; ++view) {
    if (view % group_size == 0u) {
      needed = Max(needed, group_needed);
      group_needed = 0u;
    }
    group_needed +=
        (uint64_t)states[view].visible_count + states[view].overflow_count;
    if (states[view].overflow_count == 0u)
      continue;
    const uint32_t target = slot->gpu_draw_view_targets[view];
    const uint32_t layer = target & VKR_METAL_PACKET_VIEW_TARGET_LAYER_MASK;
    if (target & VKR_METAL_PACKET_VIEW_TARGET_LOCAL) {
      vkr_metal_packet_invalidate_retained_layer(renderer, "local_shadow_map",
                                                 layer);
    } else if (target & VKR_METAL_PACKET_VIEW_TARGET_CASCADE) {
      vkr_metal_packet_invalidate_retained_layer(
          renderer, "shadow_cascade_static", layer);
      vkr_metal_packet_invalidate_retained_layer(renderer, "shadow_map", layer);
    }
    log_warn("Metal GPU-draw pool cut %u draws of view %u (pool target %u)",
             states[view].overflow_count, view, renderer->gpu_draw_pool_target);
  }
  needed = Max(needed, group_needed);
  if (needed * VKR_METAL_PACKET_GPU_DRAW_POOL_HEADROOM_NUM <=
      (uint64_t)renderer->gpu_draw_pool_target *
          VKR_METAL_PACKET_GPU_DRAW_POOL_HEADROOM_DEN)
    return;
  const uint64_t step = VKR_METAL_PACKET_GPU_DRAW_POOL_STEP;
  const uint64_t grown = (needed * 2u + step - 1u) / step * step;
  log_info("Metal GPU-draw pool target %u -> %llu commands (a frame needed "
           "%llu)",
           renderer->gpu_draw_pool_target, (unsigned long long)grown,
           (unsigned long long)needed);
  renderer->gpu_draw_pool_target = (uint32_t)Min(grown, (uint64_t)UINT32_MAX);
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
        slot->shadow_cascade_count, slot->shadow_cascade_dynamic_render_mask,
        slot->gpu_draw_view_count);
    vkr_metal_packet_gpu_draw_pool_feedback(renderer, slot);
  }
  if (slot->custom_late_readback) {
    slot->pending_result.custom_late_count = *slot->custom_late_readback;
    vkr_atomic_uint64_fetch_add(&renderer->custom_late_draws,
                                slot->pending_result.custom_late_count,
                                VKR_MEMORY_ORDER_RELAXED);
  }
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
  const uint64_t completed = vkr_metal_packet_completed(renderer);
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

/* Commit feedback reports a submission's GPU interval after its completion
   signal. A slot about to be reused waits, bounded, for its record so the
   frame reusing it publishes that submission's result rather than its own
   unfinished one; without the wait a late handler leaves that frame without
   GPU pass timings. Only frames timing both passes and the submission wait:
   dynamic resolution alone tolerates a late sample. */
vkr_internal void vkr_metal_packet_wait_commit_feedback(
    const VkrMetalPacketCommitFeedbackRecord *record) {
  const float64_t deadline = vkr_platform_get_absolute_time() +
                             (float64_t)VKR_METAL_PACKET_TIMEOUT_MS / 1000.0;
  while (!vkr_atomic_bool_load(&record->feedback_ready,
                               VKR_MEMORY_ORDER_ACQUIRE) &&
         vkr_platform_get_absolute_time() < deadline) {
    dispatch_sync(vkr_metal_packet_feedback_queue(), ^{
                  });
  }
}

vkr_internal bool8_t vkr_metal_packet_complete_command_slot(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketCommandSlot *slot,
    bool8_t upload_path) {
  const uint64_t wait_value =
      MAX(slot->submit_value, slot->resource_reuse_submit_value);
  if (wait_value == 0)
    return true_v;
  VKR_METAL_DIAGNOSTIC(renderer, "slot_reuse", "slot=%u wait=%llu upload=%u",
                       (uint32_t)(slot - renderer->command_slots),
                       (unsigned long long)wait_value, upload_path);
  if (vkr_metal_packet_completed(renderer) < wait_value) {
    vkr_metal_packet_wait_counters_record(&renderer->wait_counters,
                                          upload_path);
    if (!vkr_metal_packet_wait(renderer, wait_value))
      return false_v;
  }
  vkr_metal_packet_collect_picking_results(
      renderer, vkr_metal_packet_completed(renderer));
  if (slot->commit_feedback && slot->pass_timing_requested &&
      slot->result_pending && !slot->result_collected)
    vkr_metal_packet_wait_commit_feedback(slot->commit_feedback);
  vkr_metal_packet_cache_slot_result(renderer, slot);
  vkr_capture_ring_collect(&renderer->capture_ring,
                           vkr_metal_packet_completed(renderer));
  [slot->allocator reset];
  VKR_METAL_DIAGNOSTIC(renderer, "slot_reset", "slot=%u",
                       (uint32_t)(slot - renderer->command_slots));
  slot->submit_value = 0;
  slot->resource_reuse_submit_value = 0;
  slot->timestamp_entry_count = 0u;
  slot->pass_timing_requested = false_v;
  slot->result_pending = false_v;
  slot->result_collected = false_v;
  slot->commit_feedback = NULL;
  slot->picking_readback = NULL;
  slot->gpu_draw_diagnostics_readback = NULL;
  slot->custom_late_readback = NULL;
  slot->exposure_readback = NULL;
  slot->shadow_cascade_count = 0u;
  slot->shadow_cascade_dynamic_render_mask = 0u;
  slot->gpu_draw_view_count = 0u;
  slot->gpu_draw_diagnostics_requested = false_v;
  slot->exposure_requested = false_v;
  slot->pending_result = (VkrMetalPacketResult){0};
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_acquire_command_slot(
    VkrMetalPacketRenderer *renderer, bool8_t upload_path) {
  if (renderer->active_command_slot)
    return false_v;
  if (upload_path) {
    /* An upload reuses the slot of the upload one ring's length earlier,
       whose ring slice the upload ring hands out next. */
    VkrMetalPacketCommandSlot *slot =
        &renderer->upload_slots[renderer->next_upload_slot];
    if (!vkr_metal_packet_complete_command_slot(renderer, slot, true_v))
      return false_v;
    renderer->next_upload_slot =
        (renderer->next_upload_slot + 1u) % renderer->upload_slot_count;
    renderer->active_command_slot = slot;
    renderer->command_allocator = slot->allocator;
    renderer->command_buffer = slot->buffer;
    VKR_METAL_DIAGNOSTIC(renderer, "slot_acquire", "upload_slot=%u upload=1",
                         (uint32_t)(slot - renderer->upload_slots));
    return true_v;
  }
  uint32_t slot_index = renderer->next_command_slot;
  if (&renderer->command_slots[slot_index] == renderer->reserved_frame_slot)
    slot_index = (slot_index + 1u) % renderer->command_slot_count;
  VkrMetalPacketCommandSlot *slot = &renderer->command_slots[slot_index];
  if (!vkr_metal_packet_complete_command_slot(renderer, slot, upload_path))
    return false_v;
  renderer->next_command_slot =
      (slot_index + 1u) % renderer->command_slot_count;
  renderer->active_command_slot = slot;
  renderer->command_allocator = slot->allocator;
  renderer->command_buffer = slot->buffer;
  VKR_METAL_DIAGNOSTIC(renderer, "slot_acquire", "slot=%u upload=%u",
                       slot_index, upload_path);
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
  /* A committed presenter frame's texture tags run one value ahead of the
     frame queue (ADR-111); they are submitted uses. */
  const uint64_t texture_floor =
      Max(submitted, renderer->presenter.texture_floor);
  for (uint32_t i = 0; i < renderer->max_textures; ++i) {
    if (renderer->textures[i].last_use_submit_value > texture_floor)
      renderer->textures[i].last_use_submit_value = texture_floor;
  }
  if (renderer->ibl_black_last_use_submit_value > submitted)
    renderer->ibl_black_last_use_submit_value = submitted;
  if (renderer->ltc_last_use_submit_value > submitted)
    renderer->ltc_last_use_submit_value = submitted;
  if (renderer->dfg_lut_last_use_submit_value > submitted)
    renderer->dfg_lut_last_use_submit_value = submitted;
  if (renderer->cloud_noise_last_use_submit_value > submitted)
    renderer->cloud_noise_last_use_submit_value = submitted;
  if (renderer->anisotropy_lut_last_use_submit_value > submitted)
    renderer->anisotropy_lut_last_use_submit_value = submitted;
}

vkr_internal void
vkr_metal_packet_finish_frame(VkrMetalPacketRenderer *renderer,
                              bool8_t graph_active, bool8_t submitted) {
  VKR_METAL_DIAGNOSTIC(renderer, "frame_finish", "graph=%u committed=%u",
                       graph_active, submitted);
  /* A reserved frame has no speculative draw uses. Independent publication
   * may already have submitted uploads, or still own an open upload batch.
   * A presentation-only frame owns no command slot and tags only textures it
   * presents with (ADR-111). */
  const bool8_t present_only = renderer->present_prepared;
  if (!submitted && !renderer->reserved_frame_slot && !present_only)
    vkr_metal_packet_discard_unsubmitted_uses(renderer);
  if (graph_active)
    vkr_rg_end_frame(renderer->graph);
  [renderer->drawable release];
  renderer->drawable = nil;
  renderer->frame_prepared = false_v;
  renderer->present_prepared = false_v;
  if (renderer->reserved_frame_slot) {
    [renderer->reserved_frame_slot->allocator reset];
    renderer->reserved_frame_slot = NULL;
  } else if (!submitted && !present_only && renderer->active_command_slot) {
    vkr_metal_packet_release_unsubmitted_command_slot(renderer);
  }
}

vkr_internal VkrMetalMemoryStatus vkr_metal_packet_ensure_ring_capacity(
    VkrMetalPacketRenderer *renderer, VkrMetalRingKind ring_kind,
    uint64_t requested_size) {
  if (requested_size <=
      vkr_metal_memory_device_ring_slot_capacity(renderer->memory, ring_kind))
    return VKR_METAL_MEMORY_STATUS_OK;
  if (renderer->texture_upload_batch.active)
    return VKR_METAL_MEMORY_STATUS_RING_BUSY;
  for (uint32_t i = 0u; i < renderer->command_slot_count; ++i) {
    if (&renderer->command_slots[i] == renderer->active_command_slot)
      continue;
    if (!vkr_metal_packet_complete_command_slot(
            renderer, &renderer->command_slots[i], true_v))
      return VKR_METAL_MEMORY_STATUS_RING_BUSY;
  }
  for (uint32_t i = 0u; i < renderer->upload_slot_count; ++i) {
    if (&renderer->upload_slots[i] == renderer->active_command_slot)
      continue;
    if (!vkr_metal_packet_complete_command_slot(
            renderer, &renderer->upload_slots[i], true_v))
      return VKR_METAL_MEMORY_STATUS_RING_BUSY;
  }
  vkr_metal_packet_collect_completed_results(renderer);
  vkr_capture_ring_collect(&renderer->capture_ring,
                           vkr_metal_packet_completed(renderer));
  return vkr_metal_memory_device_grow_ring(
      renderer->memory, ring_kind, requested_size,
      vkr_metal_packet_completed(renderer));
}

vkr_internal bool8_t vkr_metal_packet_texture_upload_batch_start(
    VkrMetalPacketRenderer *renderer, uint64_t capacity)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketTextureUploadBatch *batch = &renderer->texture_upload_batch;
  if (batch->active)
    return true_v;
  if (vkr_metal_packet_ensure_ring_capacity(
          renderer, VKR_METAL_RING_KIND_PUBLICATION, capacity) !=
      VKR_METAL_MEMORY_STATUS_OK)
    return false_v;
  if (!batch->enabled ||
      (renderer->frame_prepared && !renderer->reserved_frame_slot) ||
      !vkr_metal_packet_acquire_command_slot(renderer, true_v))
    return false_v;

  void *upload_buffer = NULL;
  if (vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, capacity,
          vkr_metal_packet_completed(renderer), &batch->slice, &batch->upload,
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
  /* The open batch spans publisher calls and their autorelease pools. */
  batch->encoder = [[renderer->command_buffer computeCommandEncoder] retain];
  if (!batch->encoder) {
    [renderer->command_buffer endCommandBuffer];
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, batch->slice);
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
  [batch->encoder release];
  batch->encoder = nil;
  [renderer->command_buffer endCommandBuffer];
  const bool8_t holds_textures = batch->texture_count != 0u;
  if (!holds_textures && batch->buffer_count == 0u) {
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, batch->slice);
    vkr_metal_packet_release_unsubmitted_command_slot(renderer);
    *batch = (VkrMetalPacketTextureUploadBatch){.enabled =
                                                    finish ? false_v : true_v};
    return true_v;
  }
  const uint64_t submit_value = ++renderer->submit_value;
  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  slot->submit_value = submit_value;
  id<MTL4CommandBuffer> submissions[] = {renderer->command_buffer};
  VKR_METAL_DIAGNOSTIC(renderer, "upload_batch_commit_begin",
                       "slot=%u textures=%u buffers=%u bytes=%llu",
                       (uint32_t)(slot - renderer->upload_slots),
                       batch->texture_count, batch->buffer_count,
                       (unsigned long long)batch->used);
  VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  [renderer->queue commit:submissions count:1];
  [renderer->queue signalEvent:renderer->completion value:submit_value];
  VKR_METAL_DIAGNOSTIC(renderer, "upload_batch_commit_end", "slot=%u",
                       (uint32_t)(slot - renderer->upload_slots));
  VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  const bool8_t submitted =
      vkr_metal_memory_device_submit_ring(
          renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, batch->slice,
          submit_value) == VKR_METAL_MEMORY_STATUS_OK;
  if (!submitted) {
    const bool8_t completed = vkr_metal_packet_wait(renderer, submit_value);
    if (completed) {
      vkr_metal_memory_device_cancel_ring(
          renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, batch->slice);
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
  /* A finished batch holding textures returns once they are on the GPU,
     which texture publication relies on; buffer copies, as single uploads
     do, return at once. */
  return !finish || !holds_textures ||
         vkr_metal_packet_wait(renderer, submit_value);
}

bool8_t vkr_metal_packet_renderer_begin_texture_upload_batch(
    VkrMetalPacketRenderer *renderer) {
  if (!renderer ||
      (renderer->frame_prepared && !renderer->reserved_frame_slot) ||
      renderer->texture_upload_batch.enabled)
    return false_v;
  renderer->texture_upload_batch.enabled = true_v;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_end_texture_upload_batch(
    VkrMetalPacketRenderer *renderer) {
  @autoreleasepool {
    if (!renderer || !renderer->texture_upload_batch.enabled)
      return false_v;
    if (!@available(macOS 26.0, *))
      return false_v;
    return vkr_metal_packet_texture_upload_batch_flush(renderer, true_v);
  }
}

vkr_internal bool8_t vkr_metal_packet_begin_upload(
    VkrMetalPacketRenderer *renderer, uint64_t size,
    VkrMetalRingSlice *out_slice, VkrMetalAddressPair *out_upload,
    id<MTLBuffer> *out_buffer, id<MTL4ComputeCommandEncoder> *out_encoder)
    API_AVAILABLE(macos(26.0)) {
  if (renderer->texture_upload_batch.active &&
      !vkr_metal_packet_texture_upload_batch_flush(renderer, false_v))
    return false_v;
  if (vkr_metal_packet_ensure_ring_capacity(renderer,
                                            VKR_METAL_RING_KIND_PUBLICATION,
                                            size) != VKR_METAL_MEMORY_STATUS_OK)
    return false_v;
  if (!vkr_metal_packet_acquire_command_slot(renderer, true_v))
    return false_v;
  void *buffer = NULL;
  if (vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, size,
          vkr_metal_packet_completed(renderer), out_slice, out_upload,
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
  vkr_metal_memory_device_cancel_ring(
      renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, *out_slice);
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
  VKR_METAL_DIAGNOSTIC(renderer, "upload_commit_begin", "slot=%u",
                       (uint32_t)(slot - renderer->upload_slots));
  VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  [renderer->queue commit:submissions count:1];
  [renderer->queue signalEvent:renderer->completion value:submit_value];
  VKR_METAL_DIAGNOSTIC(renderer, "upload_commit_end", "slot=%u",
                       (uint32_t)(slot - renderer->upload_slots));
  VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
  if (vkr_metal_memory_device_submit_ring(
          renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, slice,
          submit_value) != VKR_METAL_MEMORY_STATUS_OK) {
    const bool8_t completed = vkr_metal_packet_wait(renderer, submit_value);
    if (completed) {
      vkr_metal_memory_device_cancel_ring(
          renderer->memory, VKR_METAL_RING_KIND_PUBLICATION, slice);
      (void)vkr_metal_packet_complete_command_slot(renderer, slot, false_v);
    }
    vkr_metal_packet_detach_active_command_slot(renderer);
    return false_v;
  }
  vkr_metal_packet_detach_active_command_slot(renderer);
  return true_v;
}

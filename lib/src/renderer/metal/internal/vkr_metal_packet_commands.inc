/* Bounded command-slot acquisition, completion, timing retention, and uploads.
 */
static bool8_t vkr_metal_packet_wait(VkrMetalPacketRenderer *renderer,
                                     uint64_t value) {
  return
      [renderer->completion waitUntilSignaledValue:value
                                         timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS]
          ? true_v
          : false_v;
}

static void
vkr_metal_packet_cache_slot_timing(VkrMetalPacketRenderer *renderer,
                                   VkrMetalPacketCommandSlot *slot) {
  if (!slot->timing_requested || slot->timing_collected)
    return;
  vkr_metal_packet_collect_pass_timings(renderer, slot->timestamp_heap,
                                        slot->timestamp_entry_count,
                                        &slot->timing_result);
  renderer->completed_timing_results[renderer->next_completed_timing] =
      slot->timing_result;
  renderer->next_completed_timing =
      (renderer->next_completed_timing + 1u) % renderer->command_slot_count;
  slot->timing_collected = true_v;
}

static void
vkr_metal_packet_cache_ready_timings(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  const uint64_t completed = renderer->completion.signaledValue;
  for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
    VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
    if (slot->submit_value != 0 && slot->submit_value <= completed)
      vkr_metal_packet_cache_slot_timing(renderer, slot);
  }
}

static const VkrMetalPacketResult *
vkr_metal_packet_find_completed_timing(const VkrMetalPacketRenderer *renderer,
                                       uint64_t submit_value) {
  for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
    const VkrMetalPacketResult *result = &renderer->completed_timing_results[i];
    if (result->submit_value == submit_value)
      return result;
  }
  return NULL;
}

static bool8_t
vkr_metal_packet_complete_command_slot(VkrMetalPacketRenderer *renderer,
                                       VkrMetalPacketCommandSlot *slot,
                                       bool8_t upload_path) {
  if (!slot || slot->submit_value == 0)
    return true_v;
  if (renderer->completion.signaledValue < slot->submit_value) {
    vkr_metal_packet_wait_counters_record(&renderer->wait_counters,
                                          upload_path);
    if (!vkr_metal_packet_wait(renderer, slot->submit_value))
      return false_v;
  }
  vkr_metal_packet_cache_slot_timing(renderer, slot);
  vkr_capture_ring_collect(&renderer->capture_ring,
                           renderer->completion.signaledValue);
  [slot->allocator reset];
  slot->submit_value = 0;
  slot->timestamp_entry_count = 0;
  slot->timing_requested = false_v;
  slot->timing_collected = false_v;
  slot->timing_result = (VkrMetalPacketResult){0};
  return true_v;
}

static bool8_t
vkr_metal_packet_acquire_command_slot(VkrMetalPacketRenderer *renderer,
                                      bool8_t upload_path) {
  if (!renderer || renderer->active_command_slot ||
      renderer->command_slot_count == 0)
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
  renderer->timestamp_heap = slot->timestamp_heap;
  return true_v;
}

static void vkr_metal_packet_release_unsubmitted_command_slot(
    VkrMetalPacketRenderer *renderer) {
  if (!renderer || !renderer->active_command_slot)
    return;
  [renderer->active_command_slot->allocator reset];
  renderer->active_command_slot = NULL;
  renderer->command_allocator = nil;
  renderer->command_buffer = nil;
  renderer->timestamp_heap = nil;
}

static void
vkr_metal_packet_detach_active_command_slot(VkrMetalPacketRenderer *renderer) {
  renderer->active_command_slot = NULL;
  renderer->command_allocator = nil;
  renderer->command_buffer = nil;
  renderer->timestamp_heap = nil;
}

static void
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

static void vkr_metal_packet_finish_frame(VkrMetalPacketRenderer *renderer,
                                          bool8_t graph_active,
                                          bool8_t submitted) {
  if (!renderer)
    return;
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

static bool8_t vkr_metal_packet_begin_upload(
    VkrMetalPacketRenderer *renderer, uint64_t size,
    VkrMetalRingSlice *out_slice, VkrMetalAddressPair *out_upload,
    id<MTLBuffer> *out_buffer, id<MTL4ComputeCommandEncoder> *out_encoder)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer || size == 0 || !out_slice || !out_upload || !out_buffer ||
      !out_encoder || !vkr_metal_packet_acquire_command_slot(renderer, true_v))
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

static bool8_t vkr_metal_packet_submit_upload(
    VkrMetalPacketRenderer *renderer, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalRingSlice slice) API_AVAILABLE(macos(26.0)) {
  [encoder endEncoding];
  [renderer->command_buffer endCommandBuffer];
  const uint64_t submit_value = ++renderer->submit_value;
  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  slot->submit_value = submit_value;
  id<MTL4CommandBuffer> submissions[] = {renderer->command_buffer};
  [renderer->queue commit:submissions count:1];
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

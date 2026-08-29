/* Public result polling, drain/adapters, and renderer destruction. */
VkrCaptureStatus
vkr_metal_packet_renderer_capture_poll(VkrMetalPacketRenderer *renderer,
                                       VkrCaptureRequestId request_id,
                                       VkrCapturePollResult *out_result) {
  if (!renderer || !out_result || request_id == 0) {
    if (out_result)
      MemZero(out_result, sizeof(*out_result));
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  if (@available(macOS 26.0, *)) {
    vkr_metal_packet_collect_completed_results(renderer);
    return vkr_capture_ring_poll(&renderer->capture_ring, request_id,
                                 renderer->completion.signaledValue,
                                 out_result);
  }
  MemZero(out_result, sizeof(*out_result));
  return VKR_CAPTURE_STATUS_NOT_FOUND;
}

bool8_t vkr_metal_packet_renderer_pass_timings_poll(
    VkrMetalPacketRenderer *renderer, uint64_t submit_value,
    VkrMetalPacketPassTiming *out_timings, uint32_t capacity,
    uint32_t *out_count) {
  if (out_count)
    *out_count = 0;
  if (!renderer || !out_timings || !out_count || submit_value == 0 ||
      capacity == 0)
    return false_v;
  if (!@available(macOS 26.0, *))
    return false_v;
  vkr_metal_packet_collect_completed_results(renderer);
  const VkrMetalPacketResult *result =
      vkr_metal_packet_find_completed_timing(renderer, submit_value);
  if (!result || result->pass_timing_count == 0 ||
      result->pass_timing_count > capacity)
    return false_v;
  MemCopy(out_timings, result->pass_timings,
          (uint64_t)result->pass_timing_count * sizeof(*out_timings));
  *out_count = result->pass_timing_count;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_submit_result_poll_next(
    VkrMetalPacketRenderer *renderer, uint64_t after_submit_value,
    VkrMetalPacketResult *out_result) {
  if (!renderer || !out_result)
    return false_v;
  *out_result = (VkrMetalPacketResult){0};
  if (!@available(macOS 26.0, *))
    return false_v;
  vkr_metal_packet_collect_completed_results(renderer);
  const VkrMetalPacketResult *next = NULL;
  for (uint32_t i = 0; i < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY; ++i) {
    const VkrMetalPacketResult *candidate =
        &renderer->completed_timing_results[i];
    if (candidate->submit_value > after_submit_value &&
        (!next || candidate->submit_value < next->submit_value))
      next = candidate;
  }
  if (!next)
    return false_v;
  *out_result = *next;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_wait_idle(VkrMetalPacketRenderer *renderer) {
  if (!renderer || renderer->frame_prepared || renderer->active_command_slot)
    return false_v;
  if (@available(macOS 26.0, *)) {
    if (renderer->submit_value > renderer->completion.signaledValue &&
        !vkr_metal_packet_wait(renderer, renderer->submit_value))
      return false_v;
    for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
      if (!vkr_metal_packet_complete_command_slot(
              renderer, &renderer->command_slots[i], false_v))
        return false_v;
    }
    if (renderer->commit_feedback_records) {
      const CFTimeInterval deadline =
          CACurrentMediaTime() +
          (CFTimeInterval)VKR_METAL_PACKET_TIMEOUT_MS / 1000.0;
      bool8_t feedback_complete = false_v;
      do {
        dispatch_sync(vkr_metal_packet_feedback_queue(), ^{
                      });
        feedback_complete = true_v;
        for (uint32_t i = 0u; i < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
             ++i) {
          const VkrMetalPacketCommitFeedbackRecord *record =
              &renderer->commit_feedback_records[i];
          if (record->in_use &&
              !vkr_atomic_bool_load(&record->feedback_ready,
                                    VKR_MEMORY_ORDER_ACQUIRE)) {
            feedback_complete = false_v;
            break;
          }
        }
      } while (!feedback_complete && CACurrentMediaTime() < deadline);
      if (!feedback_complete)
        return false_v;
    }
    vkr_metal_packet_harvest_commit_feedback(renderer);
    vkr_capture_ring_collect(&renderer->capture_ring,
                             renderer->completion.signaledValue);
    vkr_metal_packet_collect_retired_image_views(
        renderer, renderer->completion.signaledValue);
    return vkr_metal_memory_device_collect(
               renderer->memory, renderer->completion.signaledValue, NULL) ==
               VKR_METAL_MEMORY_STATUS_OK &&
           vkr_metal_material_table_device_collect(
               renderer->materials, renderer->completion.signaledValue, NULL) ==
               VKR_METAL_MATERIAL_STATUS_OK;
  }
  return false_v;
}

bool8_t
vkr_metal_packet_renderer_cancel_frame(VkrMetalPacketRenderer *renderer) {
  if (@available(macOS 26.0, *)) {
    vkr_metal_packet_finish_frame(renderer, false_v, false_v);
    return true_v;
  }
  return false_v;
}

uint64_t
vkr_metal_packet_renderer_submit_value(const VkrMetalPacketRenderer *renderer) {
  return renderer ? renderer->submit_value : 0;
}

uint64_t vkr_metal_packet_renderer_completed_value(
    const VkrMetalPacketRenderer *renderer) {
  if (!renderer)
    return 0;
  if (@available(macOS 26.0, *))
    return renderer->completion.signaledValue;
  return 0;
}

bool8_t vkr_metal_packet_renderer_get_memory_metrics(
    const VkrMetalPacketRenderer *renderer,
    VkrMetalMemoryDeviceMetrics *out_metrics) {
  if (!renderer || !out_metrics)
    return false_v;
  vkr_metal_memory_device_get_metrics(renderer->memory, out_metrics);
  out_metrics->pending_texture_upload_bytes =
      renderer->pending_texture_upload_bytes;
  return true_v;
}

void vkr_metal_packet_renderer_geometry_megabuffer_metrics(
    const VkrMetalPacketRenderer *renderer,
    VkrGeometryMegabufferMetrics *out_metrics) {
  if (!out_metrics)
    return;
  *out_metrics = (VkrGeometryMegabufferMetrics){0};
  if (!renderer)
    return;
  const VkrMetalPacketGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  out_metrics->vertex_capacity_bytes = mega->vertices.length;
  out_metrics->index_capacity_bytes = mega->indices.length;
  out_metrics->vertex_live_bytes = mega->vertex_live_bytes;
  out_metrics->index_live_bytes = mega->index_live_bytes;
  out_metrics->decode_metadata_live_bytes = mega->decode_metadata_live_bytes;
  out_metrics->live_bytes = mega->vertex_live_bytes + mega->index_live_bytes +
                            mega->decode_metadata_live_bytes;
  out_metrics->fragmentation_bytes =
      mega->vertex_cursor + mega->index_cursor - out_metrics->live_bytes;
  out_metrics->high_water_bytes =
      mega->vertex_high_water + mega->index_high_water;
  out_metrics->vertex_high_water_bytes = mega->vertex_high_water;
  out_metrics->index_high_water_bytes = mega->index_high_water;
  out_metrics->vertex_uploaded_bytes_total = mega->vertex_uploaded_bytes_total;
  out_metrics->index_uploaded_bytes_total = mega->index_uploaded_bytes_total;
  out_metrics->decode_metadata_high_water_bytes =
      mega->decode_metadata_high_water;
  out_metrics->decode_metadata_uploaded_bytes_total =
      mega->decode_metadata_uploaded_bytes_total;
  out_metrics->rejected_publications = mega->rejected_publications;
  out_metrics->generation_replacements = mega->generation_replacements;
  out_metrics->generation = mega->generation;
}

bool8_t vkr_metal_packet_renderer_get_and_reset_upload_wait_count(
    VkrMetalPacketRenderer *renderer, uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count)
    return false_v;
  *out_wait_count =
      vkr_metal_packet_wait_counters_take_upload(&renderer->wait_counters);
  return true_v;
}

bool8_t vkr_metal_packet_renderer_get_and_reset_command_slot_wait_count(
    VkrMetalPacketRenderer *renderer, uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count)
    return false_v;
  *out_wait_count = vkr_metal_packet_wait_counters_take_command_slot(
      &renderer->wait_counters);
  return true_v;
}

uint32_t
vkr_metal_packet_renderer_frame_slot(const VkrMetalPacketRenderer *renderer) {
  if (!renderer || renderer->command_slot_count == 0u)
    return 0u;
  /* `next_command_slot` already points past the acquired slot, so an active
     frame is recording into the one before it. Reporting `next_command_slot`
     directly would name the slot the *following* frame will use. */
  if (renderer->active_command_slot) {
    return (renderer->next_command_slot + renderer->command_slot_count - 1u) %
           renderer->command_slot_count;
  }
  return renderer->next_command_slot;
}

uint32_t vkr_metal_packet_renderer_frame_slot_count(
    const VkrMetalPacketRenderer *renderer) {
  return renderer ? renderer->command_slot_count : 0u;
}

bool8_t
vkr_metal_packet_renderer_capture_release(VkrMetalPacketRenderer *renderer,
                                          VkrCaptureRequestId request_id) {
  return renderer && request_id != 0
             ? vkr_capture_ring_release(&renderer->capture_ring, request_id)
             : false_v;
}

bool8_t vkr_metal_packet_renderer_drain(
    VkrMetalPacketRenderer *renderer,
    VkrMetalMemoryDeviceMetrics *out_memory_metrics,
    VkrMetalMaterialTableMetrics *out_material_metrics) {
  if (!renderer)
    return false_v;
  if (!@available(macOS 26.0, *))
    return false_v;
  if (!vkr_metal_packet_renderer_wait_idle(renderer))
    return false_v;
  vkr_capture_ring_collect(&renderer->capture_ring,
                           renderer->completion.signaledValue);

  if (renderer->gtao_white_live) {
    if (vkr_metal_memory_device_retire(
            renderer->memory, renderer->gtao_white_visibility.handle,
            renderer->gtao_white_last_use_submit_value) !=
        VKR_METAL_MEMORY_STATUS_OK)
      return false_v;
    renderer->gtao_white_live = false_v;
    renderer->gtao_white_visibility = (VkrMetalTextureResource){0};
  }

  if (renderer->ibl_live) {
    if (vkr_metal_memory_device_retire(
            renderer->memory, renderer->ibl_prefilter.handle,
            renderer->ibl_last_use_submit_value) != VKR_METAL_MEMORY_STATUS_OK)
      return false_v;
    renderer->ibl_live = false_v;
    renderer->ibl_prefilter = (VkrMetalTextureResource){0};
  }

  for (uint32_t i = 0; i < renderer->max_materials; ++i) {
    if (renderer->packet_materials[i].live &&
        !vkr_metal_packet_renderer_destroy_material(
            renderer,
            (VkrMaterialHandle){.id = i + 1,
                                .generation =
                                    renderer->packet_materials[i].generation}))
      return false_v;
  }
  for (uint32_t i = 0; i < renderer->max_meshes; ++i) {
    if (renderer->meshes[i].live &&
        !vkr_metal_packet_renderer_destroy_mesh(
            renderer,
            (VkrMeshHandle){.id = i + 1,
                            .generation = renderer->meshes[i].generation}))
      return false_v;
  }
  if (renderer->geometry_megabuffer.live) {
    if (vkr_metal_memory_device_retire(
            renderer->memory, renderer->geometry_megabuffer.vertices.handle,
            renderer->submit_value) != VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_memory_device_retire(
            renderer->memory, renderer->geometry_megabuffer.indices.handle,
            renderer->submit_value) != VKR_METAL_MEMORY_STATUS_OK)
      return false_v;
    renderer->geometry_megabuffer = (VkrMetalPacketGeometryMegabuffer){0};
  }
  for (uint32_t i = 0; i < renderer->max_textures; ++i) {
    if (renderer->textures[i].live &&
        !vkr_metal_packet_renderer_destroy_texture(
            renderer,
            (VkrTextureHandle){.id = i + 1,
                               .generation = renderer->textures[i].generation}))
      return false_v;
  }
  for (uint32_t i = 0; i < renderer->max_images; ++i) {
    VkrMetalPacketImage *slot = &renderer->images[i];
    if (slot->live && !vkr_metal_packet_retire_image_instances(
                          renderer, slot->instances, slot->instance_count))
      return false_v;
    *slot = (VkrMetalPacketImage){0};
  }
  for (uint32_t i = 0; i < renderer->max_images; ++i) {
    VkrMetalPacketGraphBuffer *slot = &renderer->graph_buffers[i];
    VkrMetalPacketGraphBufferInstance *instances =
        &renderer->graph_buffer_instances[(uint64_t)i *
                                          renderer->history_instance_count];
    if (slot->live && !vkr_metal_packet_retire_graph_buffer_instances(
                          renderer, instances, slot->instance_count))
      return false_v;
    *slot = (VkrMetalPacketGraphBuffer){0};
  }
  if (vkr_metal_material_table_device_collect(renderer->materials,
                                              renderer->submit_value, NULL) !=
          VKR_METAL_MATERIAL_STATUS_OK ||
      vkr_metal_memory_device_collect(renderer->memory, renderer->submit_value,
                                      NULL) != VKR_METAL_MEMORY_STATUS_OK)
    return false_v;
  if (out_memory_metrics)
    vkr_metal_memory_device_get_metrics(renderer->memory, out_memory_metrics);
  if (out_material_metrics)
    vkr_metal_material_table_device_get_metrics(renderer->materials,
                                                out_material_metrics);
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_publish_candidate_result(
    VkrMetalPacketRenderer *renderer, bool8_t result) {
  if (!result)
    return false_v;
  if (renderer->candidate_publication_generation == UINT64_MAX) {
    renderer->candidate_publication_generation = 0u;
    log_fatal("Metal candidate publication generation exhausted");
  } else {
    renderer->candidate_publication_generation++;
  }
  return true_v;
}

vkr_internal uint64_t
vkr_metal_packet_candidate_publication_generation(void *state) {
  const VkrMetalPacketRenderer *renderer = state;
  return renderer ? renderer->candidate_publication_generation : 0u;
}

vkr_internal bool8_t vkr_metal_packet_publish_geometry_adapter(
    void *state, VkrGeometryHandle handle,
    const struct VkrGeometryConfig *geometry) {
  return vkr_metal_packet_publish_candidate_result(
      state,
      vkr_metal_packet_renderer_publish_geometry(state, handle, geometry));
}

vkr_internal bool8_t vkr_metal_packet_publish_loaded_mesh_adapter(
    void *state, VkrGeometryHandle handle,
    const struct VkrMeshLoaderResult *mesh) {
  return vkr_metal_packet_publish_candidate_result(
      state,
      vkr_metal_packet_renderer_publish_loaded_mesh(state, handle, mesh));
}

vkr_internal bool8_t vkr_metal_packet_unpublish_geometry_adapter(
    void *state, VkrGeometryHandle handle) {
  return vkr_metal_packet_publish_candidate_result(
      state, vkr_metal_packet_renderer_destroy_mesh(
                 state, (VkrMeshHandle){.id = handle.id,
                                        .generation = handle.generation}));
}

vkr_internal bool8_t
vkr_metal_packet_begin_texture_upload_batch_adapter(void *state) {
  return vkr_metal_packet_renderer_begin_texture_upload_batch(state);
}

vkr_internal bool8_t
vkr_metal_packet_end_texture_upload_batch_adapter(void *state) {
  return vkr_metal_packet_renderer_end_texture_upload_batch(state);
}

vkr_internal bool8_t vkr_metal_packet_publish_texture_adapter(
    void *state, VkrTextureHandle handle,
    const struct VkrTexturePreparedLoad *texture) {
  return vkr_metal_packet_renderer_publish_prepared_texture(state, handle,
                                                            texture);
}

vkr_internal bool8_t vkr_metal_packet_publish_writable_texture_adapter(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  return vkr_metal_packet_renderer_publish_writable_texture(state, handle,
                                                            description);
}

vkr_internal bool8_t vkr_metal_packet_update_texture_sampler_adapter(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  return vkr_metal_packet_renderer_update_texture_sampler(state, handle,
                                                          description);
}

vkr_internal uint32_t
vkr_metal_packet_ibl_sh_slot_adapter(void *state, VkrTextureHandle source) {
  const VkrMetalPacketTexture *texture =
      vkr_metal_packet_resolve_texture(state, source);
  return texture ? texture->ibl_sh_slot : VKR_SH_SLOT_BLACK;
}

vkr_internal bool8_t vkr_metal_packet_bake_ibl_cubemap_adapter(
    void *state, VkrTextureHandle source, VkrTextureHandle prefilter,
    float32_t sh_deringing) {
  return vkr_metal_packet_renderer_bake_ibl_cubemap(state, source, prefilter,
                                                    sh_deringing);
}

vkr_internal bool8_t vkr_metal_packet_bake_hdr_environment_adapter(
    void *state, VkrTextureHandle equirect, VkrTextureHandle source,
    VkrTextureHandle prefilter, float32_t sh_deringing) {
  return vkr_metal_packet_renderer_bake_hdr_environment(
      state, equirect, source, prefilter, sh_deringing);
}

vkr_internal bool8_t vkr_metal_packet_unpublish_texture_adapter(
    void *state, VkrTextureHandle handle) {
  return vkr_metal_packet_renderer_destroy_texture(state, handle);
}

vkr_internal bool8_t vkr_metal_packet_publish_material_adapter(
    void *state, VkrMaterialHandle handle, const struct VkrMaterial *material) {
  return vkr_metal_packet_publish_candidate_result(
      state,
      vkr_metal_packet_renderer_publish_material(state, handle, material));
}

vkr_internal bool8_t vkr_metal_packet_unpublish_material_adapter(
    void *state, VkrMaterialHandle handle) {
  return vkr_metal_packet_publish_candidate_result(
      state, vkr_metal_packet_renderer_destroy_material(state, handle));
}

vkr_internal bool8_t vkr_metal_packet_publications_idle_adapter(void *state) {
  return state != NULL;
}

vkr_internal bool8_t vkr_metal_packet_texture_upload_available_adapter(
    void *state, uint64_t upload_bytes) {
  VkrMetalPacketRenderer *renderer = state;
  VkrMetalMemoryDeviceMetrics metrics = {0};
  if (!renderer || upload_bytes == 0u ||
      !vkr_metal_packet_renderer_get_memory_metrics(renderer, &metrics) ||
      metrics.suballocations.heap_size == 0u ||
      metrics.suballocations.free_bytes > metrics.suballocations.heap_size) {
    return false_v;
  }
  const uint64_t placement_capacity = metrics.suballocations.heap_size;
  const uint64_t budget = vkr_metal_memory_effective_budget(
      placement_capacity, metrics.driver_recommended_working_set_size);
  const uint64_t effective_free = vkr_metal_memory_effective_free_bytes(
      placement_capacity, metrics.suballocations.free_bytes,
      metrics.driver_recommended_working_set_size);
  VkrMetalMemoryMetrics effective = metrics.suballocations;
  effective.heap_size = budget;
  effective.free_bytes = Min(effective_free, budget);
  effective.largest_free_range =
      Min(effective.largest_free_range, effective.free_bytes);
  const uint64_t reserve = budget / 10u;

  if (vkr_metal_memory_can_allocate_before_reserve(&effective, upload_bytes,
                                                   reserve)) {
    return true_v;
  }
  renderer->pending_texture_upload_bytes =
      Max(renderer->pending_texture_upload_bytes, upload_bytes);
  return false_v;
}

void vkr_metal_packet_renderer_get_asset_publisher(
    VkrMetalPacketRenderer *renderer, VkrAssetPublisher *out_publisher) {
  if (!out_publisher)
    return;
  *out_publisher = renderer
                       ? (VkrAssetPublisher){
                             .state = renderer,
                             .publications_idle =
                                 vkr_metal_packet_publications_idle_adapter,
                             .publication_generation =
                                 vkr_metal_packet_candidate_publication_generation,
                             .texture_upload_available =
                                 vkr_metal_packet_texture_upload_available_adapter,
                             .publish_geometry =
                                 vkr_metal_packet_publish_geometry_adapter,
                             .publish_loaded_mesh =
                                 vkr_metal_packet_publish_loaded_mesh_adapter,
                             .unpublish_geometry =
                                 vkr_metal_packet_unpublish_geometry_adapter,
                             .begin_texture_upload_batch =
                                 vkr_metal_packet_begin_texture_upload_batch_adapter,
                             .end_texture_upload_batch =
                                 vkr_metal_packet_end_texture_upload_batch_adapter,
                             .publish_texture =
                                 vkr_metal_packet_publish_texture_adapter,
                             .publish_writable_texture =
                                 vkr_metal_packet_publish_writable_texture_adapter,
                             .update_texture_sampler =
                                 vkr_metal_packet_update_texture_sampler_adapter,
                             .bake_ibl_cubemap =
                                 vkr_metal_packet_bake_ibl_cubemap_adapter,
                             .bake_hdr_environment =
                                 vkr_metal_packet_bake_hdr_environment_adapter,
                             .ibl_sh_slot =
                                 vkr_metal_packet_ibl_sh_slot_adapter,
                             .unpublish_texture =
                                 vkr_metal_packet_unpublish_texture_adapter,
                             .publish_material =
                                 vkr_metal_packet_publish_material_adapter,
                             .unpublish_material =
                                 vkr_metal_packet_unpublish_material_adapter,
                         }
                       : (VkrAssetPublisher){0};
}

void vkr_metal_packet_renderer_destroy(VkrMetalPacketRenderer *renderer) {
  if (!renderer)
    return;
  if (@available(macOS 26.0, *)) {
    if (renderer->memory && renderer->materials && renderer->images)
      (void)vkr_metal_packet_renderer_drain(renderer, NULL, NULL);
    if (renderer->render_passes) {
      for (uint32_t i = 0; i < renderer->max_passes; ++i)
        [renderer->render_passes[i] release];
    }
    if (renderer->pass_labels) {
      for (uint32_t i = 0; i < renderer->max_passes; ++i) {
        [renderer->pass_labels[i].value release];
        renderer->pass_labels[i].value = nil;
      }
    }
    if (renderer->command_slots) {
      for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
        VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
        for (uint32_t group = 0u;
             group < VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX; ++group) {
          if (group < slot->gpu_draw_icb_residency_count)
            [slot->gpu_draw_icb_residencies[group] endResidency];
          [slot->gpu_draw_icbs[group] release];
          [slot->gpu_draw_icb_residencies[group] release];
        }
        [slot->transmission_gpu_draw_icb release];
        [slot->timestamp_heap release];
        [slot->buffer release];
        [slot->allocator release];
      }
    }
    if (renderer->commit_feedback_records) {
      for (uint32_t i = 0u; i < VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY;
           ++i) {
        [renderer->commit_feedback_records[i].options release];
        if (renderer->commit_feedback_records[i].handler)
          Block_release(renderer->commit_feedback_records[i].handler);
        if (renderer->commit_feedback_records[i].ready_handler)
          Block_release(renderer->commit_feedback_records[i].ready_handler);
      }
    }
    if (renderer->samplers) {
      for (uint32_t i = 0; i < renderer->sampler_count; ++i)
        [renderer->samplers[i].state release];
    }
    [renderer->argument_table release];
    [renderer->gpu_draw_icb_argument_encoder release];
    [renderer->depth_read_state release];
    [renderer->depth_write_state release];
    [renderer->ui_text_pipeline release];
    [renderer->world_text_pipeline release];
    [renderer->picking_text_pipeline release];
    [renderer->tonemap_pipeline release];
    [renderer->ibl_equirect_pipeline release];
    [renderer->ibl_prefilter_pipeline release];
    [renderer->ibl_sh_pipeline release];
    /* The pool has renderer lifetime and is released only here, after normal
       device teardown has drained outstanding readers. */
    [renderer->sh_coefficients release];
    [renderer->gpu_draw_encode_pipeline release];
    [renderer->temporal_transform_pipeline release];
    [renderer->gbuffer_resolve_pipeline release];
    [renderer->gtao_denoise_pipeline release];
    [renderer->gtao_evaluate_pipeline release];
    [renderer->gtao_depth_mip_pipeline release];
    [renderer->gtao_depth_prefilter_pipeline release];
    [renderer->deferred_lighting_pipeline release];
    [renderer->temporal_resolve_pipeline release];
    [renderer->transmission_shade_pipeline release];
    [renderer->transmission_coverage_pipeline release];
    [renderer->transmission_compact_pipeline release];
    [renderer->transmission_compact_finalize_pipeline release];
    [renderer->picking_resolve_pipeline release];
    [renderer->hzb_build_pipeline release];
    [renderer->sdsm_reduce_pipeline release];
    [renderer->bloom_combine_pipeline release];
    [renderer->bloom_upsample_pipeline release];
    [renderer->bloom_downsample_box4_pipeline release];
    [renderer->bloom_downsample_tent13_pipeline release];
    [renderer->bloom_prefilter_pipeline release];
    [renderer->exposure_resolve_pipeline release];
    [renderer->exposure_histogram_pipeline release];
    [renderer->exposure_clear_pipeline release];
    [renderer->gpu_draw_prefix_pipeline release];
    [renderer->gpu_draw_classify_pipeline release];
    [renderer->vbuffer_pipeline release];
    [renderer->transmission_vbuffer_pipeline release];
    [renderer->gpu_shadow_pipeline release];
    [renderer->gpu_shadow_opaque_pipeline release];
    [renderer->vbuffer_opaque_pipeline release];
    [renderer->blend_pipeline release];
    [renderer->overlay_pipeline release];
    [renderer->picking_pipeline release];
    [renderer->compiler release];
    [renderer->compiler_options release];
    [renderer->pipeline_archive release];
    [renderer->pipeline_serializer release];
    [renderer->completion release];
    [renderer->queue release];
    [renderer->drawable release];
    [renderer->layer release];
    [renderer->device release];
  }
  vkr_rg_destroy(renderer->graph);
  arena_destroy(renderer->graph_frame_arena);
  vkr_rg_json_destroy(&renderer->json_graph);
  vkr_rg_executor_registry_destroy(&renderer->executors);
  vkr_metal_material_table_device_destroy(renderer->materials);
  vkr_metal_memory_device_destroy(renderer->memory);
  /* Every record array lives in one tagged reservation. Release its aggregate
     accounting immediately before the backing store, matching bulk-lifetime
     allocator ownership. */
  VkrAllocator *allocator = renderer->allocator;
  if (renderer->record_allocator.ctx) {
    vkr_allocator_release_global_accounting(&renderer->record_allocator);
    vkr_dmemory_allocator_destroy(&renderer->record_allocator);
  }
  if (renderer->capture_storage_allocator.ctx) {
    vkr_allocator_release_global_accounting(
        &renderer->capture_storage_allocator);
    vkr_dmemory_allocator_destroy(&renderer->capture_storage_allocator);
  }
  vkr_metal_packet_free_sized(allocator, renderer, sizeof(*renderer));
}

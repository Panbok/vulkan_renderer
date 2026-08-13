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
  vkr_metal_packet_cache_ready_timings(renderer);
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

bool8_t vkr_metal_packet_renderer_pass_timings_poll_latest(
    VkrMetalPacketRenderer *renderer, uint64_t after_submit_value,
    VkrMetalPacketResult *out_result) {
  if (!renderer || !out_result)
    return false_v;
  *out_result = (VkrMetalPacketResult){0};
  if (!@available(macOS 26.0, *))
    return false_v;
  vkr_metal_packet_cache_ready_timings(renderer);
  const VkrMetalPacketResult *latest = NULL;
  for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
    const VkrMetalPacketResult *candidate =
        &renderer->completed_timing_results[i];
    if (candidate->submit_value > after_submit_value &&
        (!latest || candidate->submit_value > latest->submit_value))
      latest = candidate;
  }
  if (!latest)
    return false_v;
  *out_result = *latest;
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
    vkr_capture_ring_collect(&renderer->capture_ring,
                             renderer->completion.signaledValue);
    return vkr_metal_memory_device_collect(
               renderer->memory, renderer->completion.signaledValue, NULL) ==
               VKR_METAL_MEMORY_STATUS_OK &&
           vkr_metal_material_table_device_collect(
               renderer->materials, renderer->completion.signaledValue, NULL) ==
               VKR_METAL_MATERIAL_STATUS_OK;
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
  out_metrics->live_bytes = mega->vertex_live_bytes + mega->index_live_bytes;
  out_metrics->fragmentation_bytes =
      mega->vertex_cursor + mega->index_cursor - out_metrics->live_bytes;
  out_metrics->high_water_bytes =
      mega->vertex_high_water + mega->index_high_water;
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

  if (renderer->ibl_live) {
    if (vkr_metal_memory_device_retire(renderer->memory,
                                       renderer->ibl_irradiance.handle,
                                       renderer->ibl_last_use_submit_value) !=
            VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_memory_device_retire(
            renderer->memory, renderer->ibl_prefilter.handle,
            renderer->ibl_last_use_submit_value) != VKR_METAL_MEMORY_STATUS_OK)
      return false_v;
    renderer->ibl_live = false_v;
    renderer->ibl_irradiance = (VkrMetalTextureResource){0};
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
    for (uint32_t instance = 0u; slot->live && instance < slot->instance_count;
         ++instance) {
      if (!vkr_metal_packet_retire_image_instance(renderer,
                                                  &slot->instances[instance]))
        return false_v;
    }
    *slot = (VkrMetalPacketImage){0};
  }
  for (uint32_t i = 0; i < renderer->max_images; ++i) {
    VkrMetalPacketGraphBuffer *slot = &renderer->graph_buffers[i];
    VkrMetalPacketGraphBufferInstance *instances =
        &renderer->graph_buffer_instances[(uint64_t)i *
                                          renderer->command_slot_count];
    for (uint32_t instance = 0u; slot->live && instance < slot->instance_count;
         ++instance) {
      if (instances[instance].live &&
          vkr_metal_memory_device_retire(
              renderer->memory, instances[instance].resource.handle,
              instances[instance].last_use_submit_value) !=
              VKR_METAL_MEMORY_STATUS_OK)
        return false_v;
      instances[instance] = (VkrMetalPacketGraphBufferInstance){0};
    }
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

vkr_internal bool8_t vkr_metal_packet_publish_geometry_adapter(
    void *state, VkrGeometryHandle handle,
    const struct VkrGeometryConfig *geometry) {
  return vkr_metal_packet_renderer_publish_geometry(state, handle, geometry);
}

vkr_internal bool8_t vkr_metal_packet_publish_loaded_mesh_adapter(
    void *state, VkrGeometryHandle handle,
    const struct VkrMeshLoaderResult *mesh) {
  return vkr_metal_packet_renderer_publish_loaded_mesh(state, handle, mesh);
}

vkr_internal bool8_t vkr_metal_packet_unpublish_geometry_adapter(
    void *state, VkrGeometryHandle handle) {
  return vkr_metal_packet_renderer_destroy_mesh(
      state, (VkrMeshHandle){.id = handle.id, .generation = handle.generation});
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

vkr_internal bool8_t vkr_metal_packet_bake_ibl_cubemap_adapter(
    void *state, VkrTextureHandle source, VkrTextureHandle irradiance,
    VkrTextureHandle prefilter) {
  return vkr_metal_packet_renderer_bake_ibl_cubemap(state, source, irradiance,
                                                    prefilter);
}

vkr_internal bool8_t vkr_metal_packet_bake_hdr_environment_adapter(
    void *state, VkrTextureHandle equirect, VkrTextureHandle source,
    VkrTextureHandle irradiance, VkrTextureHandle prefilter) {
  return vkr_metal_packet_renderer_bake_hdr_environment(state, equirect, source,
                                                        irradiance, prefilter);
}

vkr_internal bool8_t vkr_metal_packet_unpublish_texture_adapter(
    void *state, VkrTextureHandle handle) {
  return vkr_metal_packet_renderer_destroy_texture(state, handle);
}

vkr_internal bool8_t vkr_metal_packet_publish_material_adapter(
    void *state, VkrMaterialHandle handle, const struct VkrMaterial *material) {
  return vkr_metal_packet_renderer_publish_material(state, handle, material);
}

vkr_internal bool8_t vkr_metal_packet_unpublish_material_adapter(
    void *state, VkrMaterialHandle handle) {
  return vkr_metal_packet_renderer_destroy_material(state, handle);
}

vkr_internal bool8_t vkr_metal_packet_publications_idle_adapter(void *state) {
  return state != NULL;
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
                             .publish_geometry =
                                 vkr_metal_packet_publish_geometry_adapter,
                             .publish_loaded_mesh =
                                 vkr_metal_packet_publish_loaded_mesh_adapter,
                             .unpublish_geometry =
                                 vkr_metal_packet_unpublish_geometry_adapter,
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
    if (renderer->command_slots) {
      id<MTLResidencySet> residency =
          renderer->memory
              ? (id<MTLResidencySet>)vkr_metal_memory_device_residency_set(
                    renderer->memory)
              : nil;
      for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
        if (renderer->command_slots[i].gpu_draw_icb && residency)
          [residency removeAllocation:renderer->command_slots[i].gpu_draw_icb];
        [renderer->command_slots[i].gpu_draw_icb release];
        if (renderer->command_slots[i].transmission_gpu_draw_icb && residency)
          [residency removeAllocation:renderer->command_slots[i]
                                          .transmission_gpu_draw_icb];
        [renderer->command_slots[i].transmission_gpu_draw_icb release];
        [renderer->command_slots[i].buffer release];
        [renderer->command_slots[i].allocator release];
        [renderer->command_slots[i].timestamp_heap release];
      }
      [residency commit];
    }
    if (renderer->samplers) {
      for (uint32_t i = 0; i < renderer->sampler_count; ++i)
        [renderer->samplers[i].state release];
    }
    [renderer->argument_table release];
    [renderer->gpu_draw_icb_argument_encoder release];
    [renderer->depth_read_state release];
    [renderer->depth_write_state release];
    [renderer->picking_text_pipeline release];
    [renderer->ui_text_pipeline release];
    [renderer->world_text_pipeline release];
    [renderer->tonemap_pipeline release];
    [renderer->ibl_equirect_pipeline release];
    [renderer->ibl_prefilter_pipeline release];
    [renderer->ibl_irradiance_pipeline release];
    [renderer->gpu_draw_encode_pipeline release];
    [renderer->gbuffer_resolve_pipeline release];
    [renderer->deferred_lighting_pipeline release];
    [renderer->transmission_shade_pipeline release];
    [renderer->hzb_build_pipeline release];
    [renderer->gpu_draw_prefix_pipeline release];
    [renderer->gpu_draw_classify_pipeline release];
    [renderer->vbuffer_pipeline release];
    [renderer->picking_pipeline release];
    [renderer->opaque_pipeline release];
    [renderer->blend_pipeline release];
    [renderer->overlay_pipeline release];
    [renderer->skybox_pipeline release];
    [renderer->shadow_pipeline release];
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

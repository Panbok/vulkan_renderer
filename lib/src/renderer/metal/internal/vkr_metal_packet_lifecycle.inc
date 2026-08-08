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
    return vkr_metal_capture_ring_poll(&renderer->capture_ring, request_id,
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

bool8_t vkr_metal_packet_result_publish_pass_metrics(
    const VkrMetalPacketResult *result, VkrRendererMetrics *renderer_metrics,
    uint64_t cpu_frame_index) {
  if (!result || !renderer_metrics ||
      result->pass_timing_count > VKR_METAL_PACKET_MAX_PASS_TIMINGS)
    return false_v;
  VkrRendererMetricsPassSample samples[VKR_METAL_PACKET_MAX_PASS_TIMINGS] = {0};
  for (uint32_t i = 0; i < result->pass_timing_count; ++i) {
    const VkrMetalPacketPassTiming *source = &result->pass_timings[i];
    if (!isfinite(source->cpu_ms) || source->cpu_ms < 0.0 ||
        (source->valid && (!isfinite(source->gpu_ms) || source->gpu_ms < 0.0)))
      return false_v;
    VkrRendererMetricsPassSample *sample = &samples[i];
    const uint64_t length =
        Min(string_length(source->name), (uint64_t)VKR_METRIC_NAME_MAX);
    MemCopy(sample->name, source->name, length);
    sample->name[length] = '\0';
    sample->name_length = (uint8_t)length;
    sample->cpu_ms = source->cpu_ms;
    sample->gpu_ms = source->gpu_ms;
    sample->cpu_frame_index = cpu_frame_index;
    sample->gpu_source_frame_index = cpu_frame_index;
    sample->gpu_source_submit_serial = result->submit_value;
    sample->gpu_valid = source->valid;
  }
  return vkr_renderer_metrics_publish_pass_samples(
      renderer_metrics, samples, result->pass_timing_count, cpu_frame_index);
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
    vkr_metal_capture_ring_collect(&renderer->capture_ring,
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
  return renderer && request_id != 0 ? vkr_metal_capture_ring_release(
                                           &renderer->capture_ring, request_id)
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
  vkr_metal_capture_ring_collect(&renderer->capture_ring,
                                 renderer->completion.signaledValue);

  if (renderer->ibl_live) {
    if (vkr_metal_memory_device_retire(renderer->memory,
                                       renderer->ibl_irradiance.handle,
                                       renderer->ibl_last_use_submit_value) !=
            VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_memory_device_retire(renderer->memory,
                                       renderer->ibl_prefilter.handle,
                                       renderer->ibl_last_use_submit_value) !=
            VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_memory_device_retire(
            renderer->memory, renderer->ibl_brdf.handle,
            renderer->ibl_last_use_submit_value) != VKR_METAL_MEMORY_STATUS_OK)
      return false_v;
    renderer->ibl_live = false_v;
    renderer->ibl_irradiance = (VkrMetalTextureResource){0};
    renderer->ibl_prefilter = (VkrMetalTextureResource){0};
    renderer->ibl_brdf = (VkrMetalTextureResource){0};
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
  for (uint32_t i = 0; i < renderer->max_textures; ++i) {
    if (renderer->textures[i].live &&
        !vkr_metal_packet_renderer_destroy_texture(
            renderer,
            (VkrTextureHandle){.id = i + 1,
                               .generation = renderer->textures[i].generation}))
      return false_v;
  }
  for (uint32_t i = 0; i < renderer->max_images; ++i) {
    if (!renderer->images[i].live || !renderer->images[i].owned) {
      renderer->images[i].live = false_v;
      continue;
    }
    if (vkr_metal_memory_device_retire(
            renderer->memory, renderer->images[i].resource.handle,
            renderer->submit_value) != VKR_METAL_MEMORY_STATUS_OK)
      return false_v;
    renderer->images[i].live = false_v;
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

static bool8_t vkr_metal_packet_publish_geometry_adapter(
    void *state, VkrGeometryHandle handle,
    const struct VkrGeometryConfig *geometry) {
  return vkr_metal_packet_renderer_publish_geometry(state, handle, geometry);
}

static bool8_t vkr_metal_packet_publish_loaded_mesh_adapter(
    void *state, VkrGeometryHandle handle,
    const struct VkrMeshLoaderResult *mesh) {
  return vkr_metal_packet_renderer_publish_loaded_mesh(state, handle, mesh);
}

static bool8_t
vkr_metal_packet_unpublish_geometry_adapter(void *state,
                                            VkrGeometryHandle handle) {
  return vkr_metal_packet_renderer_destroy_mesh(
      state, (VkrMeshHandle){.id = handle.id, .generation = handle.generation});
}

static bool8_t vkr_metal_packet_publish_texture_adapter(
    void *state, VkrTextureHandle handle,
    const struct VkrTexturePreparedLoad *texture) {
  return vkr_metal_packet_renderer_publish_prepared_texture(state, handle,
                                                            texture);
}

static bool8_t vkr_metal_packet_publish_writable_texture_adapter(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  return vkr_metal_packet_renderer_publish_writable_texture(state, handle,
                                                            description);
}

static bool8_t vkr_metal_packet_update_texture_sampler_adapter(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  return vkr_metal_packet_renderer_update_texture_sampler(state, handle,
                                                          description);
}

static bool8_t
vkr_metal_packet_bake_ibl_cubemap_adapter(void *state, VkrTextureHandle source,
                                          VkrTextureHandle irradiance,
                                          VkrTextureHandle prefilter) {
  return vkr_metal_packet_renderer_bake_ibl_cubemap(state, source, irradiance,
                                                    prefilter);
}

static bool8_t vkr_metal_packet_bake_hdr_environment_adapter(
    void *state, VkrTextureHandle equirect, VkrTextureHandle source,
    VkrTextureHandle irradiance, VkrTextureHandle prefilter) {
  return vkr_metal_packet_renderer_bake_hdr_environment(state, equirect, source,
                                                        irradiance, prefilter);
}

static bool8_t
vkr_metal_packet_unpublish_texture_adapter(void *state,
                                           VkrTextureHandle handle) {
  return vkr_metal_packet_renderer_destroy_texture(state, handle);
}

static bool8_t
vkr_metal_packet_publish_material_adapter(void *state, VkrMaterialHandle handle,
                                          const struct VkrMaterial *material) {
  return vkr_metal_packet_renderer_publish_material(state, handle, material);
}

static bool8_t
vkr_metal_packet_unpublish_material_adapter(void *state,
                                            VkrMaterialHandle handle) {
  return vkr_metal_packet_renderer_destroy_material(state, handle);
}

void vkr_metal_packet_renderer_get_asset_publisher(
    VkrMetalPacketRenderer *renderer, VkrAssetPublisher *out_publisher) {
  if (!out_publisher)
    return;
  *out_publisher = renderer
                       ? (VkrAssetPublisher){
                             .state = renderer,
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
      for (uint32_t i = 0; i < renderer->command_slot_count; ++i) {
        [renderer->command_slots[i].buffer release];
        [renderer->command_slots[i].allocator release];
        [renderer->command_slots[i].timestamp_heap release];
      }
    }
    if (renderer->samplers) {
      for (uint32_t i = 0; i < renderer->sampler_count; ++i)
        [renderer->samplers[i].state release];
    }
    [renderer->argument_table release];
    [renderer->depth_read_state release];
    [renderer->depth_write_state release];
    [renderer->picking_text_pipeline release];
    [renderer->ui_text_pipeline release];
    [renderer->world_text_pipeline release];
    [renderer->tonemap_pipeline release];
    [renderer->ibl_equirect_pipeline release];
    [renderer->ibl_brdf_pipeline release];
    [renderer->ibl_prefilter_pipeline release];
    [renderer->ibl_irradiance_pipeline release];
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
  free(renderer->completed_timing_results);
  free(renderer->command_slots);
  free(renderer->render_passes);
  free(renderer->packet_materials);
  free(renderer->textures);
  free(renderer->samplers);
  free(renderer->text_uploads);
  free(renderer->submeshes);
  free(renderer->meshes);
  free(renderer->images);
  free(renderer->capture_storage);
  free(renderer);
}

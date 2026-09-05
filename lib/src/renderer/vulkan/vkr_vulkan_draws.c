#include "renderer/vulkan/vkr_vulkan_internal.h"


typedef struct VkrVulkanTableDrawUpload {
  VkrVulkanPacketDrawRoot root;
  VkrGpuGeometryRow geometry;
  VkrGpuVisibleDrawRow visible;
} VkrVulkanTableDrawUpload;

vkr_internal bool8_t vkr_vk_pack_gpu_candidate_range(
    VkrVulkanRenderer *renderer, VkrVulkanFrameSlot *slot,
    const VkrWorldDrawCandidate *source, uint32_t count,
    uint32_t destination_first, VkrVulkanCandidateCopyRange *out_range,
    uint32_t *out_packed_count, uint32_t *out_omitted_count);

vkr_internal uint64_t vkr_vk_hash_bytes(uint64_t hash, const void *bytes,
                                        uint64_t size) {
  const uint8_t *data = bytes;
  for (uint64_t i = 0u; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

vkr_internal uint64_t vkr_vk_candidate_epoch(const VkrWorldPassPayload *world) {
  if (!world)
    return 0u;
  uint64_t hash = 1469598103934665603ull;
  hash = vkr_vk_hash_bytes(hash, &world->static_generation,
                           sizeof(world->static_generation));
  hash = vkr_vk_hash_bytes(hash, &world->dynamic_generation,
                           sizeof(world->dynamic_generation));
  hash = vkr_vk_hash_bytes(hash, &world->publication_generation,
                           sizeof(world->publication_generation));
  hash = vkr_vk_hash_bytes(hash, &world->gpu_candidate_count,
                           sizeof(world->gpu_candidate_count));
  hash = vkr_vk_hash_bytes(hash, &world->static_candidate_count,
                           sizeof(world->static_candidate_count));
  hash = vkr_vk_hash_bytes(hash, &world->gpu_camera_opaque_candidate_count,
                           sizeof(world->gpu_camera_opaque_candidate_count));
  return hash;
}

uint64_t vkr_vk_align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

void *vkr_vk_frame_upload_allocate(VkrVulkanFrameSlot *slot, uint64_t size,
                                   uint64_t alignment, uint64_t *out_address,
                                   uint64_t *out_offset) {
  const uint64_t offset = vkr_vk_align_up(slot->frame_upload_cursor, alignment);
  if (offset > slot->frame_upload.size ||
      size > slot->frame_upload.size - offset) {
    /* Exhaustion fails the whole frame at the call site. Count it so the
       failure is distinguishable from a malformed draw; the caller has no
       other way to tell the two apart. */
    slot->frame_upload_exhaustions++;
    return NULL;
  }
  slot->frame_upload_cursor = offset + size;
  if (out_address)
    *out_address = slot->frame_upload.address + offset;
  if (out_offset)
    *out_offset = offset;
  return (uint8_t *)slot->frame_upload.allocation.mapped + offset;
}

vkr_internal bool8_t vkr_vk_upload_instances(
    VkrVulkanFrameSlot *slot, const VkrInstanceDataGPU *instances,
    uint32_t count, uint64_t *out_address) {
  *out_address = 0u;
  if (count == 0u)
    return true_v;
  if (count > VKR_INSTANCE_BUFFER_MAX_INSTANCES)
    return false_v;
  const uint64_t size = (uint64_t)count * sizeof(*instances);
  void *destination = vkr_vk_frame_upload_allocate(
      slot, size, _Alignof(VkrInstanceDataGPU), out_address, NULL);
  if (!destination)
    return false_v;
  MemCopy(destination, instances, size);
  return true_v;
}

vkr_internal bool8_t vkr_vk_upload_ui_draw_list(
    VkrVulkanFrameSlot *slot, const VkrPreparedUiDrawList *draw_list) {
  slot->ui_vertices = 0u;
  slot->ui_index_offset = 0u;
  slot->ui_index_size = 0u;
  if (draw_list->vertex_count == 0u)
    return true_v;
  const uint64_t vertex_bytes =
      (uint64_t)draw_list->vertex_count * sizeof(VkrUiVertex);
  const uint64_t index_bytes =
      (uint64_t)draw_list->index_count * sizeof(uint32_t);
  void *vertices = vkr_vk_frame_upload_allocate(
      slot, vertex_bytes, _Alignof(VkrUiVertex), &slot->ui_vertices, NULL);
  void *indices = vkr_vk_frame_upload_allocate(
      slot, index_bytes, _Alignof(uint32_t), NULL, &slot->ui_index_offset);
  if (!vertices || !indices)
    return false_v;
  MemCopy(vertices, draw_list->vertices, vertex_bytes);
  MemCopy(indices, draw_list->indices, index_bytes);
  slot->ui_index_size = index_bytes;
  return true_v;
}

vkr_internal bool8_t vkr_vk_resolve_sampled_pair(VkrVulkanRenderer *renderer,
                                                 VkrTextureHandle handle,
                                                 uint32_t *out_texture,
                                                 uint32_t *out_sampler) {
  VkrVulkanPublishedTexture *texture =
      vkr_vk_published_texture(renderer, handle, NULL);
  if (!texture || texture->initialization_pending ||
      texture->sampler_record_index >= renderer->config.sampler_capacity)
    return false_v;
  const VkrVulkanPublishedSampler *sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  if (!sampler->live)
    return false_v;
  *out_texture = texture->sampled_slot.index;
  *out_sampler = sampler->slot.index;
  texture->last_use_submit_value = renderer->submit_value + 1u;
  return true_v;
}

vkr_internal bool8_t vkr_vk_upload_packet_tables(
    VkrVulkanRenderer *renderer, VkrVulkanFrameSlot *slot,
    const VkrRenderPacket *packet) {
  slot->point_light_data = 0u;
  slot->point_light_masks = 0u;
  slot->shadow_cascades = 0u;
  slot->ibl_probes = 0u;
  slot->ibl_probe_count = 0u;
  slot->prefilter_texture = 0u;
  slot->prefilter_sampler = 0u;
  slot->sh_global_slot = VKR_SH_SLOT_BLACK;
  slot->ibl_ready = false_v;
  slot->sh_referenced_slot_count = 0u;

  const VkrFrameLighting *lighting = packet->lighting;
  if (lighting && lighting->point_light_count) {
    const uint64_t light_bytes =
        (uint64_t)lighting->point_light_count * sizeof(VkrGpuPointLightRow);
    VkrGpuPointLightRow *packed = vkr_vk_frame_upload_allocate(
        slot, light_bytes, _Alignof(VkrGpuPointLightRow),
        &slot->point_light_data, NULL);
    if (!packed)
      return false_v;
    for (uint32_t i = 0u; i < lighting->point_light_count; ++i)
      vkr_lighting_system_pack_point_light(&lighting->point_lights[i],
                                           &packed[i]);
    const uint64_t mask_bytes =
        (uint64_t)lighting->point_light_grid->cell_count *
        sizeof(VkrPointLightMask);
    if (mask_bytes) {
      void *masks = vkr_vk_frame_upload_allocate(
          slot, mask_bytes, _Alignof(VkrPointLightMask),
          &slot->point_light_masks, NULL);
      if (!masks)
        return false_v;
      MemCopy(masks, lighting->point_light_grid->masks, mask_bytes);
    }
  }

  if (packet->shadow && packet->shadow->cascade_count) {
    const uint64_t cascade_bytes = (uint64_t)packet->shadow->cascade_count *
                                   sizeof(VkrVulkanPacketShadowCascade);
    VkrVulkanPacketShadowCascade *cascades = vkr_vk_frame_upload_allocate(
        slot, cascade_bytes, _Alignof(VkrVulkanPacketShadowCascade),
        &slot->shadow_cascades, NULL);
    if (!cascades)
      return false_v;
    for (uint32_t i = 0u; i < packet->shadow->cascade_count; ++i) {
      const VkrShadowCascadePacketData *source = &packet->shadow->cascades[i];
      cascades[i] = (VkrVulkanPacketShadowCascade){
          .light_view_projection = source->light_view_projection,
          .split_near_far_texel_depth = source->split_near_far_texel_depth,
          .origin_inv_size_pad = source->origin_inv_size_pad,
      };
    }
  }

  if (lighting && lighting->ibl_enabled && lighting->ibl_source.id) {
    VkrVulkanPublishedTexture *source =
        vkr_vk_published_texture(renderer, lighting->ibl_source, NULL);
    if (source && vkr_vk_resolve_sampled_pair(renderer, source->ibl_prefilter,
                                              &slot->prefilter_texture,
                                              &slot->prefilter_sampler)) {
      slot->ibl_ready = true_v;
      slot->sh_global_slot = source->ibl_sh_slot;
      slot->sh_referenced_slots[slot->sh_referenced_slot_count++] =
          source->ibl_sh_slot;
    }
  }

  if (lighting && lighting->ibl_probe_count) {
    const uint64_t probe_bytes =
        (uint64_t)lighting->ibl_probe_count * sizeof(VkrVulkanPacketIblProbe);
    VkrVulkanPacketIblProbe *probes = vkr_vk_frame_upload_allocate(
        slot, probe_bytes, _Alignof(VkrVulkanPacketIblProbe), &slot->ibl_probes,
        NULL);
    if (!probes)
      return false_v;
    for (uint32_t i = 0u; i < lighting->ibl_probe_count; ++i) {
      const VkrFrameIblProbe *probe = &lighting->ibl_probes[i];
      VkrVulkanPacketIblProbe packed = {
          .sh_slot = probe->sh_slot,
          .center_blend = {probe->center.x, probe->center.y, probe->center.z,
                           probe->blend_distance},
          .extents_weight = {probe->extents.x, probe->extents.y,
                             probe->extents.z, probe->weight},
          .intensity_box = {probe->intensity, probe->diffuse_intensity,
                            probe->specular_intensity,
                            probe->box_projection_enabled ? 1.0f : 0.0f},
      };
      if (!vkr_vk_resolve_sampled_pair(renderer, probe->prefilter,
                                       &packed.prefilter_texture,
                                       &packed.prefilter_sampler))
        continue;
      slot->sh_referenced_slots[slot->sh_referenced_slot_count++] =
          probe->sh_slot;
      probes[slot->ibl_probe_count++] = packed;
    }
    if (!slot->ibl_probe_count)
      slot->ibl_probes = 0u;
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_candidate_graph_buffers(
    VkrVulkanRenderer *renderer, VkrVulkanGraphBufferInstance **out_candidates,
    VkrVulkanGraphBufferInstance **out_instances,
    uint64_t *out_resource_generation) {
  const VkrRgBufferHandle candidate_handle =
      renderer->gpu_candidate_buffer_handle;
  const VkrRgBufferHandle instance_handle =
      renderer->gpu_candidate_instance_buffer_handle;
  if (!vkr_rg_buffer_handle_valid(candidate_handle) ||
      !vkr_rg_buffer_handle_valid(instance_handle))
    return false_v;
  VkrVulkanGraphBuffer *candidate_graph =
      &renderer->graph_buffers[candidate_handle.id - 1u];
  VkrVulkanGraphBuffer *instance_graph =
      &renderer->graph_buffers[instance_handle.id - 1u];
  *out_candidates = vkr_vk_graph_buffer(renderer, candidate_handle);
  *out_instances = vkr_vk_graph_buffer(renderer, instance_handle);
  *out_resource_generation =
      ((uint64_t)candidate_graph->graph_generation << 32u) |
      instance_graph->graph_generation;
  return *out_candidates && *out_instances &&
         (*out_candidates)->buffer.size >=
             (uint64_t)VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                 sizeof(VkrGpuCandidateDrawRow) &&
         (*out_instances)->buffer.size >=
             (uint64_t)VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                 sizeof(VkrInstanceDataGPU) &&
         *out_resource_generation != 0u;
}

bool8_t vkr_vk_prepare_packet_uploads(VkrVulkanRenderer *renderer,
                                      VkrVulkanFrameSlot *slot,
                                      const VkrRenderPacket *packet) {
  slot->frame_upload_cursor = 0u;
  slot->frame_upload_exhaustions = 0u;
  slot->world_instances = 0u;
  slot->ui_vertices = 0u;
  slot->ui_index_offset = 0u;
  slot->ui_index_size = 0u;
  slot->gpu_candidate_instances = 0u;
  slot->transmission_gpu_candidate_instances = 0u;
  slot->gpu_geometry_rows = 0u;
  slot->gpu_candidate_copy_count = 0u;
  slot->transmission_gpu_candidate_upload_offset = 0u;
  slot->transmission_gpu_instance_upload_offset = 0u;
  slot->gpu_candidate_buffer = NULL;
  slot->gpu_candidate_instance_buffer = NULL;
  slot->candidate_residency_pending = false_v;
  slot->gpu_candidate_count = 0u;
  slot->transmission_gpu_candidate_count = 0u;
  slot->gpu_world_epoch = 0u;
  slot->hzb_history_valid = false_v;
  slot->indexed_draw_count = 0u;
  slot->blend_draw_count = 0u;
  slot->packet_build = (VkrPacketBuildMetrics){0};
  const bool8_t common_uploads =
      vkr_vk_prepare_direct_draws(renderer, slot, packet->world) &&
      vkr_vk_upload_packet_tables(renderer, slot, packet) &&
      (!packet->world || vkr_vk_upload_instances(slot, packet->world->instances,
                                                 packet->world->instance_count,
                                                 &slot->world_instances)) &&
      (!packet->ui || vkr_vk_upload_ui_draw_list(slot, &packet->ui->draw_list));
  if (!common_uploads)
    return false_v;

  const uint64_t geometry_bytes =
      (uint64_t)renderer->config.geometry_capacity * sizeof(VkrGpuGeometryRow);
  VkrGpuGeometryRow *geometry_rows = vkr_vk_frame_upload_allocate(
      slot, geometry_bytes, _Alignof(VkrGpuGeometryRow),
      &slot->gpu_geometry_rows, NULL);
  if (!geometry_rows)
    return false_v;
#if VKR_METRICS_ENABLED
  const float64_t geometry_table_start = vkr_platform_get_absolute_time();
#endif
  MemZero(geometry_rows, geometry_bytes);
  for (uint32_t i = 0u; i < renderer->config.geometry_capacity; ++i) {
    const VkrVulkanPublishedGeometry *geometry =
        &renderer->published_geometries[i];
    if (geometry->live && geometry->pending_initialization_count == 0u)
      geometry_rows[i] = geometry->gpu_row;
  }
#if VKR_METRICS_ENABLED
  slot->packet_build.geometry_table_build_ns =
      vkr_metrics_elapsed_ns(geometry_table_start);
#endif
  slot->packet_build.geometry_row_bytes = geometry_bytes;

#if VKR_METRICS_ENABLED
  const float64_t hash_start = vkr_platform_get_absolute_time();
#endif
  const VkrWorldPassPayload *world = packet->world;
  slot->gpu_world_epoch = vkr_vk_candidate_epoch(world);
#if VKR_METRICS_ENABLED
  slot->packet_build.candidate_hash_ns = vkr_metrics_elapsed_ns(hash_start);
  const float64_t pack_start = vkr_platform_get_absolute_time();
#endif

  uint32_t omitted_count = 0u;
  uint32_t static_rows_written = 0u;
  uint32_t dynamic_rows_written = 0u;
  bool8_t packed = true_v;
  if (world) {
    uint64_t resource_generation = 0u;
    packed = vkr_vk_candidate_graph_buffers(
        renderer, &slot->gpu_candidate_buffer,
        &slot->gpu_candidate_instance_buffer, &resource_generation);
    if (packed) {
      slot->gpu_candidate_instances =
          slot->gpu_candidate_instance_buffer->buffer.address;
      const uint32_t source_static_count = world->static_candidate_count;
      const uint32_t source_dynamic_count =
          world->gpu_candidate_count - source_static_count;
      const bool8_t repack_static = vkr_candidate_residency_needs_static_repack(
          &slot->candidate_residency, world->static_generation,
          world->publication_generation, resource_generation);
      uint32_t packed_static_count =
          repack_static ? 0u : slot->candidate_residency.packed_static_count;
      uint32_t omitted_static_count =
          repack_static ? 0u : slot->candidate_residency.omitted_static_count;
      if (repack_static) {
        VkrVulkanCandidateCopyRange *range =
            &slot->gpu_candidate_copies[slot->gpu_candidate_copy_count];
        packed = vkr_vk_pack_gpu_candidate_range(
            renderer, slot, world->gpu_candidates, source_static_count, 0u,
            range, &packed_static_count, &omitted_static_count);
        if (packed && range->count > 0u) {
          slot->gpu_candidate_copy_count++;
          static_rows_written += range->count;
        }
        slot->pending_candidate_residency = vkr_candidate_residency_stage(
            world->static_generation, world->publication_generation,
            resource_generation, packed_static_count, omitted_static_count);
        slot->candidate_residency_pending = packed;
      }
      if (packed && source_dynamic_count > 0u) {
        VkrVulkanCandidateCopyRange *range =
            &slot->gpu_candidate_copies[slot->gpu_candidate_copy_count];
        uint32_t packed_dynamic_count = 0u;
        uint32_t omitted_dynamic_count = 0u;
        packed = vkr_vk_pack_gpu_candidate_range(
            renderer, slot, world->gpu_candidates + source_static_count,
            source_dynamic_count, packed_static_count, range,
            &packed_dynamic_count, &omitted_dynamic_count);
        if (packed && range->count > 0u) {
          slot->gpu_candidate_copy_count++;
          dynamic_rows_written += range->count;
        }
        slot->gpu_candidate_count = packed_static_count + packed_dynamic_count;
        omitted_count = omitted_static_count + omitted_dynamic_count;
      } else {
        slot->gpu_candidate_count = packed_static_count;
        omitted_count = omitted_static_count;
      }
    }

    if (packed && world->transmission_gpu_candidate_count > 0u) {
      VkrVulkanCandidateCopyRange range = {0};
      uint32_t omitted_transmission_count = 0u;
      packed = vkr_vk_pack_gpu_candidate_range(
          renderer, slot, world->transmission_gpu_candidates,
          world->transmission_gpu_candidate_count, 0u, &range,
          &slot->transmission_gpu_candidate_count, &omitted_transmission_count);
      slot->transmission_gpu_candidate_upload_offset =
          range.candidate_source_offset;
      slot->transmission_gpu_instance_upload_offset =
          range.instance_source_offset;
      dynamic_rows_written += range.count;
      omitted_count += omitted_transmission_count;
    }
  }
#if VKR_METRICS_ENABLED
  slot->packet_build.candidate_pack_ns = vkr_metrics_elapsed_ns(pack_start);
#endif
  const uint64_t written_candidates =
      (uint64_t)static_rows_written + dynamic_rows_written;
  slot->packet_build.candidate_row_bytes =
      written_candidates * sizeof(VkrGpuCandidateDrawRow);
  slot->packet_build.instance_row_bytes =
      written_candidates * sizeof(VkrInstanceDataGPU);
  slot->packet_build.static_candidate_row_bytes =
      (uint64_t)static_rows_written * sizeof(VkrGpuCandidateDrawRow);
  slot->packet_build.dynamic_candidate_row_bytes =
      (uint64_t)dynamic_rows_written * sizeof(VkrGpuCandidateDrawRow);
  slot->packet_build.publication_omitted_count = omitted_count;
  slot->packet_build.valid = packed;
  return packed;
}

vkr_internal VkrVulkanPublishedGeometry *
vkr_vk_resolve_geometry(VkrVulkanRenderer *renderer, VkrGeometryHandle handle) {
  if (handle.id == 0u || handle.id > renderer->config.geometry_capacity)
    return NULL;
  VkrVulkanPublishedGeometry *geometry =
      &renderer->published_geometries[handle.id - 1u];
  return geometry->live && geometry->handle.generation == handle.generation &&
                 geometry->pending_initialization_count == 0u
             ? geometry
             : NULL;
}

vkr_internal VkrVulkanPublishedMaterial *
vkr_vk_resolve_material(VkrVulkanRenderer *renderer, VkrMaterialHandle handle) {
  if (handle.id == 0u || handle.id > renderer->config.material_record_capacity)
    return NULL;
  VkrVulkanPublishedMaterial *material =
      &renderer->published_materials[handle.id - 1u];
  return material->live && material->handle.generation == handle.generation
             ? material
             : NULL;
}

bool8_t vkr_vk_prepare_direct_draws(VkrVulkanRenderer *renderer,
                                    VkrVulkanFrameSlot *slot,
                                    const VkrWorldPassPayload *world) {
  slot->direct_draws = NULL;
  slot->direct_draw_count = 0u;
  if (!world || !world->transparent_draw_count)
    return true_v;
  const uint32_t count = world->transparent_draw_count;
  slot->direct_draws = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)count * sizeof(*slot->direct_draws),
      _Alignof(VkrVulkanPreparedDirectDraw), NULL, NULL);
  if (!slot->direct_draws)
    return false_v;
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrDrawItem *source = &world->transparent_draws[i];
    if (!source->geometry.id ||
        source->geometry.id > renderer->config.geometry_capacity)
      return false_v;
    VkrVulkanPublishedGeometry *geometry =
        &renderer->published_geometries[source->geometry.id - 1u];
    VkrVulkanPublishedMaterial *material =
        vkr_vk_resolve_material(renderer, source->material);
    if (!geometry->live ||
        geometry->handle.generation != source->geometry.generation ||
        !material || source->submesh_index >= geometry->submesh_count)
      return false_v;
    /* Pending publication is distinct from invalid generation/range. Validate
       the complete draw before omitting it so pending work cannot hide errors.
     */
    if (geometry->pending_initialization_count ||
        material->pending_texture_count)
      continue;
    slot->direct_draws[slot->direct_draw_count++] =
        (VkrVulkanPreparedDirectDraw){
            .geometry = geometry,
            .material = material,
            .range = &geometry->submeshes[source->submesh_index],
            .first_instance = source->first_instance,
            .instance_count = source->instance_count,
        };
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_pack_gpu_candidate_range(
    VkrVulkanRenderer *renderer, VkrVulkanFrameSlot *slot,
    const VkrWorldDrawCandidate *source, uint32_t count,
    uint32_t destination_first, VkrVulkanCandidateCopyRange *out_range,
    uint32_t *out_packed_count, uint32_t *out_omitted_count) {
  *out_range = (VkrVulkanCandidateCopyRange){0};
  *out_packed_count = 0u;
  *out_omitted_count = 0u;
  if (!count)
    return true_v;
  if (!source || destination_first > VKR_GPU_DRAW_CANDIDATE_CAPACITY ||
      count > VKR_GPU_DRAW_CANDIDATE_CAPACITY - destination_first)
    return false_v;
  uint64_t candidate_source_offset = 0u;
  uint64_t instance_source_offset = 0u;
  VkrGpuCandidateDrawRow *candidates = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)count * sizeof(*candidates),
      _Alignof(VkrGpuCandidateDrawRow), NULL, &candidate_source_offset);
  VkrInstanceDataGPU *instances = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)count * sizeof(*instances), _Alignof(VkrInstanceDataGPU),
      NULL, &instance_source_offset);
  if (!candidates || !instances)
    return false_v;
  uint32_t packed_count = 0u;
  uint32_t unpublished_geometry_count = 0u;
  uint32_t unpublished_material_count = 0u;
  uint32_t invalid_submesh_count = 0u;
  uint32_t last_temporal_index = UINT32_MAX;
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrWorldDrawCandidate *candidate = &source[i];
    VkrVulkanPublishedGeometry *geometry =
        vkr_vk_resolve_geometry(renderer, candidate->geometry);
    VkrVulkanPublishedMaterial *material =
        vkr_vk_resolve_material(renderer, candidate->material);
    if (!geometry) {
      unpublished_geometry_count++;
      continue;
    }
    if (!material) {
      unpublished_material_count++;
      continue;
    }
    if (candidate->submesh_index >= geometry->submesh_count) {
      invalid_submesh_count++;
      continue;
    }
    const VkrVulkanSubmeshRange *submesh =
        &geometry->submeshes[candidate->submesh_index];
    candidates[packed_count] = (VkrGpuCandidateDrawRow){
        .geometry_index = candidate->geometry.id - 1u,
        .material_index = material->slot.index,
        .instance_index = destination_first + packed_count,
        .first_index = geometry->gpu_row.first_index + submesh->first_index,
        .index_count = submesh->index_count,
        .vertex_offset = submesh->vertex_offset,
        .decode_index = submesh->decode_index,
        .state_flags =
            vkr_gpu_draw_state_flags(candidate->state_bucket, candidate->flags),
        .local_bounding_sphere = candidate->local_bounding_sphere,
    };
    instances[packed_count] = candidate->instance;
    instances[packed_count].temporal_flags =
        candidate->instance.temporal_index != last_temporal_index
            ? VKR_INSTANCE_TEMPORAL_OWNER
            : 0u;
    last_temporal_index = candidate->instance.temporal_index;
    packed_count++;
  }
  if (invalid_submesh_count) {
    log_error("Vulkan rejected %u/%u deferred candidates naming a "
              "submesh outside their geometry",
              invalid_submesh_count, count);
    return false_v;
  }
  if (packed_count != count && !renderer->deferred_candidate_drop_logged) {
    log_warn("Vulkan deferred publication boundary omitted %u/%u "
             "candidates (geometry=%u material=%u)",
             count - packed_count, count, unpublished_geometry_count,
             unpublished_material_count);
    renderer->deferred_candidate_drop_logged = true_v;
  }
  *out_range = (VkrVulkanCandidateCopyRange){
      .candidate_source_offset = candidate_source_offset,
      .instance_source_offset = instance_source_offset,
      .destination_first = destination_first,
      .count = packed_count,
  };
  *out_packed_count = packed_count;
  *out_omitted_count = count - packed_count;
  return true_v;
}

vkr_internal VkrVulkanPacketUtilityRoot *
vkr_vk_packet_utility_root(VkrVulkanFrameSlot *slot, uint64_t *out_address) {
  VkrVulkanPacketUtilityRoot *root = vkr_vk_frame_upload_allocate(
      slot, sizeof(*root), _Alignof(VkrVulkanPacketUtilityRoot), out_address,
      NULL);
  if (root)
    MemZero(root, sizeof(*root));
  return root;
}

VkrVulkanPacketFrameRoot *vkr_vk_packet_frame_root(VkrVulkanFrameSlot *slot,
                                                   uint64_t *out_address) {
  VkrVulkanPacketFrameRoot *root = vkr_vk_frame_upload_allocate(
      slot, sizeof(*root), _Alignof(VkrVulkanPacketFrameRoot), out_address,
      NULL);
  if (root)
    MemZero(root, sizeof(*root));
  return root;
}

void vkr_vk_fill_packet_frame_root(
    VkrVulkanRenderer *renderer, VkrVulkanPacketFrameRoot *root,
    const VkrVulkanFrameSlot *slot, const VkrPacketFrameConstants *frame,
    uint64_t instances, Mat4 view_projection, uint32_t shadow_texture,
    uint32_t transmission_texture, bool8_t lighting_pass) {
  root->instances = instances;
  root->view_projection = view_projection;
  root->materials = renderer->materials.address;
  root->sh_coefficients = renderer->sh_coefficients.address;
  root->sh_global_slot = slot->sh_global_slot;
  root->prefilter_texture = slot->prefilter_texture;
  root->prefilter_sampler = slot->prefilter_sampler;
  root->shadow_texture = shadow_texture;
  root->shadow_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  root->shadow_comparison_sampler = renderer->shadow_comparison_sampler_slot;
  root->transmission_texture = transmission_texture;
  root->transmission_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  root->flags = vkr_packet_derive_frame_flags(renderer->graph->packet,
                                              lighting_pass, slot->ibl_ready);
  root->point_light_data = slot->point_light_data;
  root->point_light_masks = slot->point_light_masks;
  root->shadow_cascades = slot->shadow_cascades;
  root->ibl_probes = slot->ibl_probes;
  root->ibl_probe_count = slot->ibl_probe_count;

  root->view_position = frame->view_position;
  root->prefilter_mip_count = frame->prefilter_mip_count;
  root->ibl_controls = frame->ibl_controls;
  root->directional_direction_enabled = frame->directional_direction_enabled;
  root->directional_color_intensity = frame->directional_color_intensity;
  root->ambient_color = frame->ambient_color;
  root->render_mode = frame->render_mode;
  root->shadow_debug_mode = frame->shadow_debug_mode;
  root->point_light_grid_origin_cell_size =
      frame->point_light_grid_origin_cell_size;
  for (uint32_t i = 0; i < 4u; ++i) {
    root->point_light_grid_dimensions_count[i] =
        frame->point_light_grid_dimensions_count[i];
  }
  root->point_light_global_mask = frame->point_light_global_mask;
  root->point_light_count = frame->point_light_count;
  root->view = frame->view;
  root->shadow_cascade_count = frame->shadow_cascade_count;
  root->shadow_pcf_sample_count = frame->shadow_receiver.pcf_sample_count;
  root->shadow_receiver_bias_texels =
      frame->shadow_receiver.receiver_bias_texels;
  root->shadow_slope_bias_texels = frame->shadow_receiver.slope_bias_texels;
  root->shadow_normal_offset_texels =
      frame->shadow_receiver.normal_offset_texels;
  root->shadow_pcf_radius_texels = frame->shadow_receiver.pcf_radius_texels;
  root->shadow_cascade_blend_fraction =
      frame->shadow_receiver.cascade_blend_fraction;
  root->shadow_fade_start = frame->shadow_receiver.fade_start;
  root->shadow_fade_end = frame->shadow_receiver.fade_end;
  root->shadow_pcf_uniform_early_out =
      frame->shadow_receiver.pcf_uniform_early_out;
}

bool8_t
vkr_vk_record_packet_draws(VkrVulkanRenderer *renderer, VkCommandBuffer command,
                           VkrVulkanPacketPipeline pipeline, uint64_t instances,
                           Mat4 view_projection, uint32_t target_width,
                           uint32_t target_height, uint32_t shadow_texture,
                           uint32_t transmission_texture) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const uint32_t draw_count = slot->direct_draw_count;
  if (!draw_count)
    return true_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const VkrPacketFrameConstants frame =
      vkr_packet_derive_frame_constants(packet, target_width, target_height);
  const bool8_t lighting_pass =
      pipeline == VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND;
  uint64_t frame_root_address = 0u;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_root_address);
  if (!frame_root)
    return false_v;
  vkr_vk_fill_packet_frame_root(renderer, frame_root, slot, &frame, instances,
                                view_projection, shadow_texture,
                                transmission_texture, lighting_pass);
  if (lighting_pass) {
    uint64_t temporal_address = 0u;
    VkrVulkanPacketTemporalDrawState *temporal = vkr_vk_frame_upload_allocate(
        slot, sizeof(*temporal), _Alignof(VkrVulkanPacketTemporalDrawState),
        &temporal_address, NULL);
    if (!temporal)
      return false_v;
    *temporal = (VkrVulkanPacketTemporalDrawState){
        .previous_transforms =
            (slot->temporal_history_valid ? slot->temporal_transform_input
                                          : slot->temporal_transform_output)
                ->buffer.address,
        .current_view_projection =
            packet->globals.temporal.current_view_projection,
        .previous_view_projection =
            slot->temporal_history_valid
                ? slot->temporal_color_input->history_view_projection
                : packet->globals.temporal.current_view_projection,
        .history_valid = slot->temporal_history_valid,
        .previous_frame_index =
            slot->temporal_history_valid
                ? slot->temporal_color_input->history_frame_index
                : packet->frame.frame_index,
    };
    frame_root->temporal_draw_state = temporal_address;
  }
  uint64_t draw_roots_address = 0u;
  VkrVulkanTableDrawUpload *table_draws = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)draw_count * sizeof(*table_draws),
      _Alignof(VkrVulkanTableDrawUpload), &draw_roots_address, NULL);
  if (!table_draws)
    return false_v;
  /* Each pass owns a distinct root span: picking and blend reference different
     frame constants while sharing the same prepared resource rows. */
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrVulkanPreparedDirectDraw *draw = &slot->direct_draws[i];
    VkrVulkanPublishedGeometry *geometry = draw->geometry;
    VkrVulkanPublishedMaterial *material = draw->material;
    const VkrVulkanSubmeshRange *range = draw->range;
    const uint64_t draw_root_address =
        draw_roots_address + (uint64_t)i * sizeof(*table_draws);
    VkrVulkanTableDrawUpload *table_draw = &table_draws[i];
    *table_draw = (VkrVulkanTableDrawUpload){
        .root =
            {
                .geometry_rows = draw_root_address +
                                 offsetof(VkrVulkanTableDrawUpload, geometry),
                .visible_rows = draw_root_address +
                                offsetof(VkrVulkanTableDrawUpload, visible),
                .vertices = renderer->geometry_megabuffer.vertices.address,
                .frame = frame_root_address,
            },
        .geometry = geometry->gpu_row,
        .visible =
            {
                .geometry_index = 0u,
                .material_index = material->slot.index,
                .instance_index = draw->first_instance,
                .first_index =
                    geometry->gpu_row.first_index + range->first_index,
                .index_count = range->index_count,
                .vertex_offset = range->vertex_offset,
                .decode_index = range->decode_index,
                .state_flags = vkr_gpu_draw_state_flags(0u, 0u),
            },
    };
    const uint64_t pending_submit = renderer->submit_value + 1u;
    geometry->last_use_submit_value = pending_submit;
    for (uint32_t texture = 0u;
         texture < ArrayCount(material->texture_record_indices); ++texture) {
      const uint32_t record_index = material->texture_record_indices[texture];
      if (record_index != UINT32_MAX)
        renderer->published_textures[record_index].last_use_submit_value =
            pending_submit;
    }
  }
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->packet_pipelines[pipeline]);
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrVulkanPreparedDirectDraw *draw = &slot->direct_draws[i];
    const VkrVulkanPublishedGeometry *geometry = draw->geometry;
    const VkrVulkanPublishedMaterial *material = draw->material;
    const VkrVulkanSubmeshRange *range = draw->range;
    const uint64_t draw_root_address =
        draw_roots_address + (uint64_t)i * sizeof(*table_draws);
    const VkrVulkanPushConstants push = {
        .root = draw_root_address,
        .material_index = material->slot.index,
        .flags = 0u,
    };
    const uint64_t geometry_index_offset =
        (uint64_t)geometry->gpu_row.first_index * sizeof(uint32_t);
    vkCmdBindIndexBuffer2(
        command, geometry->indices.handle, geometry_index_offset,
        geometry->indices.size - geometry_index_offset, geometry->index_type);
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdDrawIndexed(command, range->index_count, draw->instance_count,
                     range->first_index, range->vertex_offset, 0u);
  }
  slot->indexed_draw_count += draw_count;
  if (lighting_pass)
    slot->blend_draw_count += draw_count;
  return true_v;
}

bool8_t vkr_vk_record_text_draws(VkrVulkanRenderer *renderer,
                                 VkCommandBuffer command,
                                 VkrVulkanPacketPipeline pipeline,
                                 const VkrPreparedTextDraw *draws,
                                 uint32_t draw_count, Mat4 view_projection,
                                 uint32_t target_width, uint32_t target_height,
                                 bool8_t ui_domain) {
  if (draw_count == 0u)
    return true_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->packet_pipelines[pipeline]);
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    VkrVulkanPublishedTexture *atlas =
        vkr_vk_published_texture(renderer, draw->atlas, NULL);
    /* Retained text can become visible while its asynchronously loaded atlas

     * is still crossing the asset-publisher seam. Omit that draw until the

     * logical publication exists; a not-yet-ready atlas must not reject the

     * otherwise coherent frame. If initialization is queued, its copy and

     * transition are recorded before the graph in this same command buffer. */
    if (!atlas || atlas->initialization_pending ||
        atlas->sampler_record_index >= renderer->config.sampler_capacity)
      continue;
    VkrVulkanPublishedSampler *sampler =
        &renderer->published_samplers[atlas->sampler_record_index];
    if (!sampler->live)
      continue;

    const uint64_t vertex_bytes =
        (uint64_t)draw->vertex_count * sizeof(*draw->vertices);
    const uint64_t index_bytes =
        (uint64_t)draw->index_count * sizeof(*draw->indices);
    uint64_t vertex_address = 0u;
    void *vertices = vkr_vk_frame_upload_allocate(
        slot, vertex_bytes, _Alignof(VkrTextVertex), &vertex_address, NULL);
    uint64_t index_offset = 0u;
    void *indices = vkr_vk_frame_upload_allocate(
        slot, index_bytes, _Alignof(uint32_t), NULL, &index_offset);
    uint64_t root_address = 0u;
    VkrVulkanPacketUtilityRoot *root =
        vkr_vk_packet_utility_root(slot, &root_address);
    if (!vertices || !indices || !root)
      return false_v;
    MemCopy(vertices, draw->vertices, vertex_bytes);
    MemCopy(indices, draw->indices, index_bytes);
    root->vertices = vertex_address;
    root->view_projection = view_projection;
    root->view = draw->model;
    root->transmission_texture = atlas->sampled_slot.index;
    root->transmission_sampler = sampler->slot.index;
    root->material_alpha.x = draw->unit_range.x;
    root->material_alpha.y = draw->unit_range.y;
    root->material_flags = draw->font_mode;
    root->first_instance = draw->object_id;
    root->flags = ui_domain ? 1u : 0u;
    root->point_light_grid_origin_cell_size =
        (Vec4){(float32_t)target_width, (float32_t)target_height, 0.0f, 0.0f};
    const VkrVulkanPushConstants push = {.root = root_address};
    vkCmdBindIndexBuffer2(command, slot->frame_upload.handle, index_offset,
                          index_bytes, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdDrawIndexed(command, draw->index_count, 1u, 0u, 0, 0u);
    atlas->last_use_submit_value = renderer->submit_value + 1u;
    slot->indexed_draw_count++;
  }
  return true_v;
}

bool8_t vkr_vk_record_ui_draw_list(VkrVulkanRenderer *renderer,
                                   VkCommandBuffer command,
                                   const VkrPreparedUiDrawList *draw_list,
                                   uint32_t target_width,
                                   uint32_t target_height) {
  if (draw_list->batch_count == 0u)
    return true_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (slot->ui_vertices == 0u || slot->ui_index_size == 0u)
    return false_v;

  const VkViewport viewport = {
      .width = (float32_t)target_width,
      .height = (float32_t)target_height,
      .maxDepth = 1.0f,
  };
  vkCmdSetViewport(command, 0u, 1u, &viewport);
  vkCmdSetCullMode(command, VK_CULL_MODE_NONE);
  vkCmdBindIndexBuffer2(command, slot->frame_upload.handle,
                        slot->ui_index_offset, slot->ui_index_size,
                        VK_INDEX_TYPE_UINT32);

  VkrVulkanPacketPipeline bound_pipeline = VKR_VULKAN_PACKET_PIPELINE_COUNT;
  for (uint32_t i = 0u; i < draw_list->batch_count; ++i) {
    const VkrUiDrawBatch *batch = &draw_list->batches[i];
    const VkrVulkanPacketPipeline pipeline =
        batch->mode == VKR_UI_DRAW_MODE_ROUNDED_RECT
            ? VKR_VULKAN_PACKET_PIPELINE_UI_RECT
            : VKR_VULKAN_PACKET_PIPELINE_UI;
    if (pipeline != bound_pipeline) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        renderer->packet_pipelines[pipeline]);
      bound_pipeline = pipeline;
    }

    uint32_t texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
    uint32_t sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX;
    const bool8_t textured = batch->texture.id != 0u;
    if (textured &&
        !vkr_vk_resolve_sampled_pair(
            renderer,
            (VkrTextureHandle){batch->texture.id, batch->texture.generation},
            &texture, &sampler))
      continue;

    uint64_t root_address = 0u;
    VkrVulkanUiRoot *root = vkr_vk_frame_upload_allocate(
        slot, sizeof(*root), _Alignof(VkrVulkanUiRoot), &root_address, NULL);
    if (!root)
      return false_v;
    *root = (VkrVulkanUiRoot){
        .vertices = slot->ui_vertices,
        .texture = texture,
        .sampler = sampler,
        .target_unit_range = {(float32_t)target_width, (float32_t)target_height,
                              batch->sdf_unit_range.x, batch->sdf_unit_range.y},
        .rect_extent = batch->rect_extent_px,
        .mode = batch->mode,
        .flags = textured ? 1u : 0u,
        .corner_radii = batch->corner_radius_px,
    };
    const VkRect2D scissor = {
        .offset = {(int32_t)batch->scissor_rect_px.x,
                   (int32_t)batch->scissor_rect_px.y},
        .extent = {(uint32_t)batch->scissor_rect_px.width,
                   (uint32_t)batch->scissor_rect_px.height},
    };
    vkCmdSetScissor(command, 0u, 1u, &scissor);
    const VkrVulkanPushConstants push = {.root = root_address};
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdDrawIndexed(command, batch->index_count, 1u, batch->first_index, 0,
                     0u);
    slot->indexed_draw_count++;
  }
  return true_v;
}

bool8_t vkr_vk_record_packet_fullscreen(VkrVulkanRenderer *renderer,
                                        VkCommandBuffer command,
                                        VkrVulkanPacketPipeline pipeline,
                                        uint32_t texture_index,
                                        uint64_t exposure_state, uint32_t flags,
                                        uint32_t output_width,
                                        uint32_t output_height) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t root_address = 0u;
  VkrVulkanPacketUtilityRoot *root =
      vkr_vk_packet_utility_root(slot, &root_address);
  uint64_t manual_state_address = 0u;
  VkrExposureGpuState *manual_state = vkr_vk_frame_upload_allocate(
      slot, sizeof(*manual_state), _Alignof(VkrExposureGpuState),
      &manual_state_address, NULL);
  if (!root || !manual_state)
    return false_v;
  *manual_state = (VkrExposureGpuState){
      .exposure_multiplier = renderer->graph->packet->globals.exposure.manual,
  };
  root->materials = renderer->materials.address;
  root->transmission_texture = texture_index;
  root->transmission_sampler =
      renderer->config.fxaa_enabled ? renderer->transmission_sampler_slot : 0u;
  root->exposure_state = exposure_state ? exposure_state : manual_state_address;
  root->point_light_grid_origin_cell_size =
      (Vec4){(float32_t)output_width, (float32_t)output_height, 0.0f, 0.0f};
  const VkrVulkanPushConstants push = {
      .root = root_address,
      .material_index = 0u,
      .flags =
          flags |
          (renderer->config.fxaa_enabled ? VKR_VULKAN_FULLSCREEN_FXAA : 0u),
  };
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->packet_pipelines[pipeline]);
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  vkCmdDraw(command, 3u, 1u, 0u, 0u);
  return true_v;
}

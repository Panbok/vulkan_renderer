#include "renderer/vulkan/vkr_vulkan_internal.h"

vkr_internal bool8_t vkr_vk_pack_gpu_candidates(
    VkrVulkanRenderer *renderer, VkrVulkanFrameSlot *slot,
    const VkrWorldDrawCandidate *source, uint32_t count,
    uint64_t *out_candidate_offset, uint64_t *out_instances_address,
    uint32_t *out_packed_count);

vkr_internal uint64_t vkr_vk_hash_bytes(uint64_t hash, const void *bytes,
                                        uint64_t size) {
  const uint8_t *data = bytes;
  for (uint64_t i = 0u; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

vkr_internal uint64_t vkr_vk_candidate_epoch(
    const VkrWorldDrawCandidate *candidates, uint32_t count) {
  uint64_t hash = 1469598103934665603ull;
  uint32_t occluder_count = 0u;
#define VKR_HASH_CANDIDATE_FIELD(FIELD)                                        \
  hash = vkr_vk_hash_bytes(hash, &candidate->FIELD, sizeof(candidate->FIELD))
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrWorldDrawCandidate *candidate = &candidates[i];
    if ((candidate->flags & VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE) == 0u)
      continue;
    occluder_count++;
    VKR_HASH_CANDIDATE_FIELD(mesh.id);
    VKR_HASH_CANDIDATE_FIELD(mesh.generation);
    VKR_HASH_CANDIDATE_FIELD(geometry.id);
    VKR_HASH_CANDIDATE_FIELD(geometry.generation);
    VKR_HASH_CANDIDATE_FIELD(submesh_index);
    VKR_HASH_CANDIDATE_FIELD(material.id);
    VKR_HASH_CANDIDATE_FIELD(material.generation);
    VKR_HASH_CANDIDATE_FIELD(instance.model);
    VKR_HASH_CANDIDATE_FIELD(local_bounding_sphere);
    VKR_HASH_CANDIDATE_FIELD(state_bucket);
    VKR_HASH_CANDIDATE_FIELD(flags);
  }
#undef VKR_HASH_CANDIDATE_FIELD
  return vkr_vk_hash_bytes(hash, &occluder_count, sizeof(occluder_count));
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
  slot->irradiance_texture = 0u;
  slot->irradiance_sampler = 0u;
  slot->prefilter_texture = 0u;
  slot->prefilter_sampler = 0u;
  slot->ibl_ready = false_v;

  const VkrFrameLighting *lighting = packet->lighting;
  if (lighting && lighting->point_light_count) {
    const uint64_t light_bytes =
        (uint64_t)lighting->point_light_count * 4u * sizeof(Vec4);
    Vec4 *packed = vkr_vk_frame_upload_allocate(
        slot, light_bytes, _Alignof(Vec4), &slot->point_light_data, NULL);
    if (!packed)
      return false_v;
    for (uint32_t i = 0u; i < lighting->point_light_count; ++i) {
      const VkrPointLight *light = &lighting->point_lights[i];
      packed[i * 4u + 0u] =
          (Vec4){light->position.x, light->position.y, light->position.z,
                 light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                     ? cosf(light->inner_cone_angle)
                     : light->constant};
      packed[i * 4u + 1u] =
          (Vec4){light->color.x, light->color.y, light->color.z,
                 light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                     ? cosf(light->outer_cone_angle)
                     : light->linear};
      packed[i * 4u + 2u] = (Vec4){light->intensity, light->quadratic,
                                   light->range, (float32_t)light->kind};
      packed[i * 4u + 3u] = (Vec4){light->direction.x, light->direction.y,
                                   light->direction.z, 0.0f};
    }
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
      cascades[i] = (VkrVulkanPacketShadowCascade){
          .light_view_projection = packet->shadow->light_view_proj[i],
          .split_depth =
              (Vec4){packet->shadow->split_depths[i], 0.0f, 0.0f, 0.0f},
      };
    }
  }

  if (lighting && lighting->ibl_enabled && lighting->ibl_source.id) {
    VkrVulkanPublishedTexture *source =
        vkr_vk_published_texture(renderer, lighting->ibl_source, NULL);
    if (source &&
        vkr_vk_resolve_sampled_pair(renderer, source->ibl_irradiance,
                                    &slot->irradiance_texture,
                                    &slot->irradiance_sampler) &&
        vkr_vk_resolve_sampled_pair(renderer, source->ibl_prefilter,
                                    &slot->prefilter_texture,
                                    &slot->prefilter_sampler)) {
      slot->ibl_ready = true_v;
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
          .center_blend = {probe->center.x, probe->center.y, probe->center.z,
                           probe->blend_distance},
          .extents_weight = {probe->extents.x, probe->extents.y,
                             probe->extents.z, probe->weight},
          .intensity_box = {probe->intensity, probe->diffuse_intensity,
                            probe->specular_intensity,
                            probe->box_projection_enabled ? 1.0f : 0.0f},
      };
      if (!vkr_vk_resolve_sampled_pair(renderer, probe->irradiance,
                                       &packed.irradiance_texture,
                                       &packed.irradiance_sampler) ||
          !vkr_vk_resolve_sampled_pair(renderer, probe->prefilter,
                                       &packed.prefilter_texture,
                                       &packed.prefilter_sampler))
        continue;
      probes[slot->ibl_probe_count++] = packed;
    }
    if (!slot->ibl_probe_count)
      slot->ibl_probes = 0u;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_packet_uploads(VkrVulkanRenderer *renderer,
                                      VkrVulkanFrameSlot *slot,
                                      const VkrRenderPacket *packet) {
  slot->frame_upload_cursor = 0u;
  slot->frame_upload_exhaustions = 0u;
  slot->world_instances = 0u;
  slot->ui_instances = 0u;
  slot->editor_instances = 0u;
  slot->gpu_candidate_instances = 0u;
  slot->transmission_gpu_candidate_instances = 0u;
  slot->gpu_geometry_rows = 0u;
  slot->gpu_candidate_upload_offset = 0u;
  slot->transmission_gpu_candidate_upload_offset = 0u;
  slot->gpu_candidate_count = 0u;
  slot->transmission_gpu_candidate_count = 0u;
  slot->gpu_world_epoch = 0u;
  slot->hzb_history_valid = false_v;
  slot->indexed_draw_count = 0u;
  slot->blend_draw_count = 0u;
  slot->packet_build = (VkrPacketBuildMetrics){0};
  const bool8_t common_uploads =
      vkr_vk_upload_packet_tables(renderer, slot, packet) &&
      (!packet->world || vkr_vk_upload_instances(slot, packet->world->instances,
                                                 packet->world->instance_count,
                                                 &slot->world_instances)) &&
      (!packet->ui || vkr_vk_upload_instances(slot, packet->ui->instances,
                                              packet->ui->instance_count,
                                              &slot->ui_instances)) &&
      (!packet->editor ||
       vkr_vk_upload_instances(slot, packet->editor->instances,
                               packet->editor->instance_count,
                               &slot->editor_instances));
  if (!common_uploads)
    return common_uploads;

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
  slot->gpu_world_epoch =
      packet->world ? vkr_vk_candidate_epoch(packet->world->gpu_candidates,
                                             packet->world->gpu_candidate_count)
                    : 0u;
#if VKR_METRICS_ENABLED
  slot->packet_build.candidate_hash_ns = vkr_metrics_elapsed_ns(hash_start);

  const float64_t pack_start = vkr_platform_get_absolute_time();
#endif
  const bool8_t packed =
      vkr_vk_pack_gpu_candidates(
          renderer, slot, packet->world ? packet->world->gpu_candidates : NULL,
          packet->world ? packet->world->gpu_candidate_count : 0u,
          &slot->gpu_candidate_upload_offset, &slot->gpu_candidate_instances,
          &slot->gpu_candidate_count) &&
      vkr_vk_pack_gpu_candidates(
          renderer, slot,
          packet->world ? packet->world->transmission_gpu_candidates : NULL,
          packet->world ? packet->world->transmission_gpu_candidate_count : 0u,
          &slot->transmission_gpu_candidate_upload_offset,
          &slot->transmission_gpu_candidate_instances,
          &slot->transmission_gpu_candidate_count);
#if VKR_METRICS_ENABLED
  slot->packet_build.candidate_pack_ns = vkr_metrics_elapsed_ns(pack_start);
#endif
  /* The gauges report actual row writes. A publication-boundary omission must
     change work volume or a timing drop could be mistaken for a faster pack. */
  const uint64_t written_candidates = (uint64_t)slot->gpu_candidate_count +
                                      slot->transmission_gpu_candidate_count;
  slot->packet_build.candidate_row_bytes =
      written_candidates * sizeof(VkrGpuCandidateDrawRow);
  slot->packet_build.instance_row_bytes =
      written_candidates * sizeof(VkrInstanceDataGPU);
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

vkr_internal bool8_t vkr_vk_pack_gpu_candidates(
    VkrVulkanRenderer *renderer, VkrVulkanFrameSlot *slot,
    const VkrWorldDrawCandidate *source, uint32_t count,
    uint64_t *out_candidate_offset, uint64_t *out_instances_address,
    uint32_t *out_packed_count) {
  *out_packed_count = 0u;
  if (!count) {
    *out_candidate_offset = 0u;
    *out_instances_address = 0u;
    return true_v;
  }
  if (!source || count > VKR_GPU_DRAW_CANDIDATE_CAPACITY)
    return false_v;
  VkrGpuCandidateDrawRow *candidates = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)count * sizeof(*candidates),
      _Alignof(VkrGpuCandidateDrawRow), NULL, out_candidate_offset);
  VkrInstanceDataGPU *instances = vkr_vk_frame_upload_allocate(
      slot, (uint64_t)count * sizeof(*instances), _Alignof(VkrInstanceDataGPU),
      out_instances_address, NULL);
  if (!candidates || !instances)
    return false_v;
  const uint64_t pending_submit = renderer->submit_value + 1u;
  uint32_t packed_count = 0u;
  uint32_t unpublished_geometry_count = 0u;
  uint32_t unpublished_material_count = 0u;
  uint32_t invalid_submesh_count = 0u;
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
        .instance_index = packed_count,
        .first_index = geometry->gpu_row.first_index + submesh->first_index,
        .index_count = submesh->index_count,
        .vertex_offset = submesh->vertex_offset,
        .state_bucket = candidate->state_bucket,
        .flags = candidate->flags,
        .local_bounding_sphere = candidate->local_bounding_sphere,
    };
    instances[packed_count++] = candidate->instance;
    geometry->last_use_submit_value =
        Max(geometry->last_use_submit_value, pending_submit);
  }
  // A submesh index outside a resolved geometry is malformed candidate
  // structure and is rejected before recording, per the P21 retirement
  // contract. An unresolved geometry or material handle is the bounded
  // publication boundary instead: uploads land at roughly one staging chunk
  // per two frames, so a freshly loaded scene legitimately presents candidates
  // whose GPU rows have not been initialized yet. Those are omitted for the
  // frame and reappear once publication completes; treating them as an error
  // would fail every frame of scene load.
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
  *out_packed_count = packed_count;
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
    uint32_t transmission_texture, bool8_t lighting_pass,
    bool8_t transmission_pass) {
  root->instances = instances;
  root->view_projection = view_projection;
  root->materials = renderer->materials.address;
  root->irradiance_texture = slot->irradiance_texture;
  root->irradiance_sampler = slot->irradiance_sampler;
  root->prefilter_texture = slot->prefilter_texture;
  root->prefilter_sampler = slot->prefilter_sampler;
  root->shadow_texture = shadow_texture;
  root->shadow_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  root->transmission_texture = transmission_texture;
  root->transmission_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  root->flags =
      vkr_packet_derive_frame_flags(renderer->graph->packet, lighting_pass,
                                    slot->ibl_ready, transmission_pass);
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
  root->shadow_bias = frame->shadow_bias;
}

bool8_t vkr_vk_record_packet_draws(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    VkrVulkanPacketPipeline pipeline, const VkrDrawItem *draws,
    uint32_t draw_count, uint64_t instances, Mat4 view_projection,
    bool8_t alpha_cutout, uint32_t shadow_texture,
    uint32_t transmission_texture, bool8_t transmission_pass) {
  if (draw_count == 0u)
    return true_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const VkrRenderPacket *packet = renderer->graph->packet;
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet,
      packet->frame.viewport_width ? packet->frame.viewport_width
                                   : packet->frame.window_width,
      packet->frame.viewport_height ? packet->frame.viewport_height
                                    : packet->frame.window_height);
  const bool8_t lighting_pass =
      pipeline == VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND;
  uint64_t frame_root_address = 0u;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_root_address);
  if (!frame_root)
    return false_v;
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, instances, view_projection,
      shadow_texture, transmission_texture, lighting_pass, transmission_pass);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->packet_pipelines[pipeline]);
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    VkrVulkanPublishedGeometry *geometry =
        vkr_vk_resolve_geometry(renderer, draw->geometry);
    VkrVulkanPublishedMaterial *material =
        vkr_vk_resolve_material(renderer, draw->material);
    if (!geometry && draw->geometry.id > 0u &&
        draw->geometry.id <= renderer->config.geometry_capacity) {
      const VkrVulkanPublishedGeometry *pending_geometry =
          &renderer->published_geometries[draw->geometry.id - 1u];
      if (pending_geometry->live &&
          pending_geometry->handle.generation == draw->geometry.generation &&
          pending_geometry->pending_initialization_count)
        continue;
    }
    if (!geometry || !material ||
        draw->submesh_index >= geometry->submesh_count)
      return false_v;
    if (material->pending_texture_count)
      continue;
    const VkrVulkanSubmeshRange *range =
        &geometry->submeshes[draw->submesh_index];
    typedef struct VkrVulkanTableDrawUpload {
      VkrVulkanPacketDrawRoot root;
      VkrGpuGeometryRow geometry;
      VkrGpuVisibleDrawRow visible;
    } VkrVulkanTableDrawUpload;
    uint64_t draw_root_address = 0u;
    VkrVulkanTableDrawUpload *table_draw = vkr_vk_frame_upload_allocate(
        slot, sizeof(*table_draw), _Alignof(VkrVulkanTableDrawUpload),
        &draw_root_address, NULL);
    if (!table_draw)
      return false_v;
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
                .flags = alpha_cutout ? 1u : 0u,
            },
    };
    const VkrVulkanPushConstants push = {
        .root = draw_root_address,
        .material_index = material->slot.index,
        .flags = alpha_cutout ? 1u : 0u,
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
    const uint64_t pending_submit = renderer->submit_value + 1u;
    geometry->last_use_submit_value = pending_submit;
    for (uint32_t texture = 0u;
         texture < ArrayCount(material->texture_record_indices); ++texture) {
      const uint32_t record_index = material->texture_record_indices[texture];
      if (record_index != UINT32_MAX)
        renderer->published_textures[record_index].last_use_submit_value =
            pending_submit;
    }
    slot->indexed_draw_count++;
    if (pipeline == VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND)
      slot->blend_draw_count++;
  }
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
    root->material_alpha.x = draw->screen_px_range;
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

bool8_t vkr_vk_record_packet_fullscreen(VkrVulkanRenderer *renderer,
                                        VkCommandBuffer command,
                                        VkrVulkanPacketPipeline pipeline,
                                        uint32_t texture_index,
                                        uint32_t flags) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t root_address = 0u;
  VkrVulkanPacketUtilityRoot *root =
      vkr_vk_packet_utility_root(slot, &root_address);
  if (!root)
    return false_v;
  root->materials = renderer->materials.address;
  root->transmission_texture = texture_index;
  root->transmission_sampler = 0u;
  root->ibl_controls.x = renderer->graph->packet->globals.exposure;
  const VkrVulkanPushConstants push = {
      .root = root_address,
      .material_index = 0u,
      .flags = flags,
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

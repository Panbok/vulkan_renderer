/* Immutable mesh/material/texture publication, lookup, and retirement. */
vkr_internal bool8_t vkr_metal_packet_sampler_key_equal(
    const VkrMetalPacketSamplerKey *a, const VkrMetalPacketSamplerKey *b) {
  return a->u_repeat_mode == b->u_repeat_mode &&
         a->v_repeat_mode == b->v_repeat_mode &&
         a->w_repeat_mode == b->w_repeat_mode &&
         a->min_filter == b->min_filter && a->mag_filter == b->mag_filter &&
         a->mip_filter == b->mip_filter &&
         a->mip_level_count == b->mip_level_count &&
         a->anisotropy_enable == b->anisotropy_enable;
}

vkr_internal MTLSamplerAddressMode
vkr_metal_packet_sampler_address(VkrTextureRepeatMode mode) {
  switch (mode) {
  case VKR_TEXTURE_REPEAT_MODE_REPEAT:
    return MTLSamplerAddressModeRepeat;
  case VKR_TEXTURE_REPEAT_MODE_MIRRORED_REPEAT:
    return MTLSamplerAddressModeMirrorRepeat;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE:
    return MTLSamplerAddressModeClampToEdge;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER:
    return MTLSamplerAddressModeClampToBorderColor;
  default:
    return MTLSamplerAddressModeClampToZero;
  }
}

vkr_internal id<MTLSamplerState>
vkr_metal_packet_sampler(VkrMetalPacketRenderer *renderer,
                         const VkrTextureDescription *description,
                         uint32_t mip_level_count) API_AVAILABLE(macos(26.0)) {
  if (!renderer || !description || mip_level_count == 0 ||
      mip_level_count > VKR_METAL_PACKET_MAX_TEXTURE_MIPS ||
      (uint32_t)description->u_repeat_mode >= VKR_TEXTURE_REPEAT_MODE_COUNT ||
      (uint32_t)description->v_repeat_mode >= VKR_TEXTURE_REPEAT_MODE_COUNT ||
      (uint32_t)description->w_repeat_mode >= VKR_TEXTURE_REPEAT_MODE_COUNT ||
      (uint32_t)description->min_filter >= VKR_FILTER_COUNT ||
      (uint32_t)description->mag_filter >= VKR_FILTER_COUNT ||
      (uint32_t)description->mip_filter >= VKR_MIP_FILTER_COUNT)
    return nil;
  const uint32_t sampler_mip_count =
      description->mip_filter == VKR_MIP_FILTER_NONE ? 1u : mip_level_count;
  const VkrMetalPacketSamplerKey key = {
      .u_repeat_mode = description->u_repeat_mode,
      .v_repeat_mode = description->v_repeat_mode,
      .w_repeat_mode = description->w_repeat_mode,
      .min_filter = description->min_filter,
      .mag_filter = description->mag_filter,
      .mip_filter = description->mip_filter,
      .mip_level_count = sampler_mip_count,
      .anisotropy_enable = description->anisotropy_enable ? true_v : false_v,
  };
  for (uint32_t i = 0; i < renderer->sampler_count; ++i) {
    if (vkr_metal_packet_sampler_key_equal(&renderer->samplers[i].key, &key))
      return renderer->samplers[i].state;
  }
  if (renderer->sampler_count >= renderer->max_samplers) {
    log_error("Metal sampler cache exhausted: %u unique states (max=%u)",
              renderer->sampler_count, renderer->max_samplers);
    return nil;
  }

  MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
  descriptor.supportArgumentBuffers = YES;
  descriptor.sAddressMode =
      vkr_metal_packet_sampler_address(description->u_repeat_mode);
  descriptor.tAddressMode =
      vkr_metal_packet_sampler_address(description->v_repeat_mode);
  descriptor.rAddressMode =
      vkr_metal_packet_sampler_address(description->w_repeat_mode);
  descriptor.minFilter = description->min_filter == VKR_FILTER_LINEAR
                             ? MTLSamplerMinMagFilterLinear
                             : MTLSamplerMinMagFilterNearest;
  descriptor.magFilter = description->mag_filter == VKR_FILTER_LINEAR
                             ? MTLSamplerMinMagFilterLinear
                             : MTLSamplerMinMagFilterNearest;
  descriptor.mipFilter =
      description->mip_filter == VKR_MIP_FILTER_LINEAR
          ? MTLSamplerMipFilterLinear
          : (description->mip_filter == VKR_MIP_FILTER_NEAREST
                 ? MTLSamplerMipFilterNearest
                 : MTLSamplerMipFilterNotMipmapped);
  descriptor.lodMinClamp = 0.0f;
  descriptor.lodMaxClamp = description->mip_filter == VKR_MIP_FILTER_NONE
                               ? 0.0f
                               : (float32_t)(sampler_mip_count - 1u);
  descriptor.maxAnisotropy = description->anisotropy_enable ? 16u : 1u;
  descriptor.borderColor = MTLSamplerBorderColorOpaqueBlack;
  id<MTLSamplerState> state =
      [renderer->device newSamplerStateWithDescriptor:descriptor];
  [descriptor release];
  if (!state)
    return nil;
  renderer->samplers[renderer->sampler_count++] =
      (VkrMetalPacketSampler){.key = key, .state = state};
  return state;
}

vkr_internal id<MTLSamplerState>
vkr_metal_packet_default_material_sampler(VkrMetalPacketRenderer *renderer,
                                          uint32_t mip_level_count)
    API_AVAILABLE(macos(26.0)) {
  const VkrTextureDescription description = {
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter =
          mip_level_count > 1 ? VKR_MIP_FILTER_LINEAR : VKR_MIP_FILTER_NONE,
  };
  return vkr_metal_packet_sampler(renderer, &description, mip_level_count);
}

vkr_internal bool8_t vkr_metal_packet_megabuffer_capacity(
    uint64_t current, uint64_t required, uint64_t *out_capacity) {
  uint64_t capacity = current ? current : MB(8);
  while (capacity < required) {
    if (capacity > UINT64_MAX / 2u)
      return false_v;
    capacity *= 2u;
  }
  *out_capacity = capacity;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_ensure_geometry_megabuffer(
    VkrMetalPacketRenderer *renderer, uint64_t vertex_end, uint64_t index_end)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  if (mega->live && vertex_end <= mega->vertices.length &&
      index_end <= mega->indices.length)
    return true_v;

  uint64_t vertex_capacity = 0u;
  uint64_t index_capacity = 0u;
  if (!vkr_metal_packet_megabuffer_capacity(mega->live ? mega->vertices.length
                                                       : 0u,
                                            vertex_end, &vertex_capacity) ||
      !vkr_metal_packet_megabuffer_capacity(
          mega->live ? mega->indices.length : 0u, index_end, &index_capacity)) {
    return false_v;
  }

  VkrMetalBufferResource vertices = {0};
  VkrMetalBufferResource indices = {0};
  if (vkr_metal_memory_device_create_buffer(renderer->memory, vertex_capacity,
                                            &vertices) !=
          VKR_METAL_MEMORY_STATUS_OK ||
      vkr_metal_memory_device_create_buffer(renderer->memory, index_capacity,
                                            &indices) !=
          VKR_METAL_MEMORY_STATUS_OK) {
    if (vertices.metal_buffer)
      (void)vkr_metal_memory_device_retire(renderer->memory, vertices.handle,
                                           renderer->submit_value);
    if (indices.metal_buffer)
      (void)vkr_metal_memory_device_retire(renderer->memory, indices.handle,
                                           renderer->submit_value);
    (void)vkr_metal_memory_device_collect(
        renderer->memory, renderer->completion.signaledValue, NULL);
    return false_v;
  }

  if (mega->live) {
    VkrMetalRingSlice slice = {0};
    VkrMetalAddressPair upload = {0};
    id<MTLBuffer> upload_buffer = nil;
    id<MTL4ComputeCommandEncoder> encoder = nil;
    if (!vkr_metal_packet_begin_upload(renderer, 1u, &slice, &upload,
                                       &upload_buffer, &encoder)) {
      (void)vkr_metal_memory_device_retire(renderer->memory, vertices.handle,
                                           renderer->submit_value);
      (void)vkr_metal_memory_device_retire(renderer->memory, indices.handle,
                                           renderer->submit_value);
      return false_v;
    }
    if (mega->vertex_cursor)
      [encoder copyFromBuffer:(id<MTLBuffer>)mega->vertices.metal_buffer
                 sourceOffset:0u
                     toBuffer:(id<MTLBuffer>)vertices.metal_buffer
            destinationOffset:0u
                         size:mega->vertex_cursor];
    if (mega->index_cursor)
      [encoder copyFromBuffer:(id<MTLBuffer>)mega->indices.metal_buffer
                 sourceOffset:0u
                     toBuffer:(id<MTLBuffer>)indices.metal_buffer
            destinationOffset:0u
                         size:mega->index_cursor];
    if (!vkr_metal_packet_submit_upload(renderer, encoder, slice)) {
      (void)vkr_metal_memory_device_retire(renderer->memory, vertices.handle,
                                           renderer->submit_value);
      (void)vkr_metal_memory_device_retire(renderer->memory, indices.handle,
                                           renderer->submit_value);
      return false_v;
    }
    const uint64_t retire_value = renderer->submit_value;
    if (vkr_metal_memory_device_retire(renderer->memory, mega->vertices.handle,
                                       retire_value) !=
            VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_memory_device_retire(renderer->memory, mega->indices.handle,
                                       retire_value) !=
            VKR_METAL_MEMORY_STATUS_OK) {
      (void)vkr_metal_memory_device_retire(renderer->memory, vertices.handle,
                                           retire_value);
      (void)vkr_metal_memory_device_retire(renderer->memory, indices.handle,
                                           retire_value);
      return false_v;
    }
    mega->generation_replacements++;
  }

  mega->vertices = vertices;
  mega->indices = indices;
  mega->generation =
      mega->generation == UINT32_MAX ? 1u : mega->generation + 1u;
  if (mega->generation == 0u)
    mega->generation = 1u;
  mega->live = true_v;
  for (uint32_t i = 0u; i < renderer->max_meshes; ++i) {
    VkrMetalPacketMesh *mesh = &renderer->meshes[i];
    if (!mesh->live)
      continue;
    mesh->vertices = mega->vertices;
    mesh->indices = mega->indices;
    vkr_gpu_geometry_row_relocate(&mesh->gpu_row, mega->vertices.gpu_address,
                                  mega->indices.gpu_address, mega->generation);
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_renderer_create_mesh_internal(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketMeshCreateInfo *create_info,
    VkrMeshHandle requested_handle, VkrMeshHandle *out_handle) {
  if (!out_handle)
    return false_v;
  *out_handle = VKR_MESH_HANDLE_INVALID;
  if (!renderer || !create_info || renderer->frame_prepared ||
      (requested_handle.id > renderer->max_meshes) ||
      (requested_handle.id > 0 &&
       requested_handle.generation == VKR_INVALID_ID) ||
      !create_info->vertices || create_info->vertex_count == 0 ||
      !create_info->indices || create_info->index_count == 0 ||
      create_info->submesh_count > renderer->max_submeshes_per_mesh ||
      (create_info->submesh_count > 0 && !create_info->submeshes) ||
      (uint64_t)create_info->vertex_count >
          UINT64_MAX / sizeof(VkrPackedStaticVertex) ||
      create_info->decode.flags != VKR_GPU_GEOMETRY_DECODE_STATIC_V1 ||
      (uint64_t)create_info->index_count > UINT64_MAX / sizeof(uint32_t)) {
    log_error("Metal geometry publication rejected before upload "
              "(frame_prepared=%u handle=%u:%u vertices=%u indices=%u)",
              renderer ? renderer->frame_prepared : 0u, requested_handle.id,
              requested_handle.generation,
              create_info ? create_info->vertex_count : 0u,
              create_info ? create_info->index_count : 0u);
    return false_v;
  }
  for (uint32_t submesh_index = 0; submesh_index < create_info->submesh_count;
       ++submesh_index) {
    const VkrMetalPacketSubmeshCreateInfo *submesh =
        &create_info->submeshes[submesh_index];
    if (submesh->index_count == 0 ||
        submesh->first_index > create_info->index_count ||
        submesh->index_count > create_info->index_count - submesh->first_index)
      return false_v;
    for (uint32_t index = 0; index < submesh->index_count; ++index) {
      const int64_t vertex =
          (int64_t)create_info->indices[submesh->first_index + index] +
          submesh->vertex_offset;
      if (vertex < 0 || (uint64_t)vertex >= create_info->vertex_count)
        return false_v;
    }
  }
  if (!@available(macOS 26.0, *))
    return false_v;
  const uint32_t first_slot = requested_handle.id ? requested_handle.id - 1 : 0;
  const uint32_t end_slot =
      requested_handle.id ? requested_handle.id : renderer->max_meshes;
  for (uint32_t i = first_slot; i < end_slot; ++i) {
    VkrMetalPacketMesh *slot = &renderer->meshes[i];
    if (slot->live)
      continue;
    const uint32_t generation = slot->generation;
    const uint64_t vertex_bytes =
        (uint64_t)create_info->vertex_count * sizeof(VkrPackedStaticVertex);
    const uint64_t decode_bytes = sizeof(VkrGpuGeometryDecodeRecord);
    const uint64_t index_bytes =
        (uint64_t)create_info->index_count * sizeof(uint32_t);
    VkrMetalPacketGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
    const bool8_t vertex_cursor_alignable =
        mega->vertex_cursor <= UINT64_MAX - 31u;
    const uint64_t vertex_offset =
        vertex_cursor_alignable
            ? vkr_metal_packet_align_up(mega->vertex_cursor, 32u)
            : UINT64_MAX;
    const bool8_t vertex_data_fits = vertex_offset <= UINT64_MAX - vertex_bytes;
    const uint64_t vertex_data_end =
        vertex_data_fits ? vertex_offset + vertex_bytes : UINT64_MAX;
    const bool8_t decode_alignable = vertex_data_end <= UINT64_MAX - 15u;
    const uint64_t decode_offset =
        decode_alignable ? vkr_metal_packet_align_up(vertex_data_end, 16u)
                         : UINT64_MAX;
    const bool8_t decode_fits = decode_offset <= UINT64_MAX - decode_bytes;
    const uint64_t vertex_end =
        decode_fits ? decode_offset + decode_bytes : UINT64_MAX;
    const uint64_t index_offset =
        vkr_metal_packet_align_up(mega->index_cursor, sizeof(uint32_t));
    const bool8_t offsets_valid =
        vertex_cursor_alignable && vertex_data_fits && decode_alignable &&
        decode_fits && index_offset <= UINT64_MAX - index_bytes &&
        vertex_offset / sizeof(VkrPackedStaticVertex) <= UINT32_MAX &&
        index_offset / sizeof(uint32_t) <= UINT32_MAX;
    const bool8_t capacity_ready =
        offsets_valid && vkr_metal_packet_ensure_geometry_megabuffer(
                             renderer, vertex_end, index_offset + index_bytes);
    const bool8_t vertices_uploaded =
        capacity_ready &&
        vkr_metal_packet_upload_buffer_chunks_at(
            renderer, create_info->vertices, vertex_bytes,
            (id<MTLBuffer>)mega->vertices.metal_buffer, vertex_offset);
    const bool8_t decode_uploaded =
        vertices_uploaded &&
        vkr_metal_packet_upload_buffer_chunks_at(
            renderer, &create_info->decode, decode_bytes,
            (id<MTLBuffer>)mega->vertices.metal_buffer, decode_offset);
    const bool8_t indices_uploaded =
        decode_uploaded &&
        vkr_metal_packet_upload_buffer_chunks_at(
            renderer, create_info->indices, index_bytes,
            (id<MTLBuffer>)mega->indices.metal_buffer, index_offset);
    if (!indices_uploaded) {
      log_error("Metal geometry publication upload failed "
                "(offsets=%u capacity=%u vertices=%u indices=%u)",
                offsets_valid, capacity_ready, vertices_uploaded,
                indices_uploaded);
      mega->rejected_publications++;
      *slot = (VkrMetalPacketMesh){.generation = generation};
      return false_v;
    }
    slot->vertices = mega->vertices;
    slot->indices = mega->indices;
    if (requested_handle.id) {
      slot->generation = requested_handle.generation;
    } else {
      slot->generation =
          slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
      if (slot->generation == 0)
        slot->generation = 1;
    }
    slot->vertex_count = create_info->vertex_count;
    slot->index_count = create_info->index_count;
    slot->gpu_row = (VkrGpuGeometryRow){
        .vertex_address = mega->vertices.gpu_address,
        .index_address = mega->indices.gpu_address,
        .first_vertex =
            (uint32_t)(vertex_offset / sizeof(VkrPackedStaticVertex)),
        .first_index = (uint32_t)(index_offset / sizeof(uint32_t)),
        .vertex_stride = sizeof(VkrPackedStaticVertex),
        .vertex_layout = VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1,
        .publication_generation = mega->generation,
        .flags = 1u,
        .decode_address = mega->vertices.gpu_address + decode_offset,
    };
    VkrMetalPacketSubmeshCreateInfo *submeshes =
        renderer->submeshes + (uint64_t)i * renderer->max_submeshes_per_mesh;
    if (create_info->submesh_count > 0) {
      MemCopy(submeshes, create_info->submeshes,
              (uint64_t)create_info->submesh_count * sizeof(*submeshes));
      slot->submesh_count = create_info->submesh_count;
    } else {
      submeshes[0] = (VkrMetalPacketSubmeshCreateInfo){
          .index_count = create_info->index_count,
      };
      slot->submesh_count = 1;
    }
    slot->last_use_submit_value = renderer->submit_value;
    slot->live = true_v;
    mega->vertex_cursor = vertex_end;
    mega->index_cursor = index_offset + index_bytes;
    mega->vertex_live_bytes += vertex_bytes;
    mega->index_live_bytes += index_bytes;
    mega->decode_metadata_live_bytes += decode_bytes;
    mega->vertex_uploaded_bytes_total += vertex_bytes;
    mega->index_uploaded_bytes_total += index_bytes;
    mega->decode_metadata_uploaded_bytes_total += decode_bytes;
    mega->vertex_high_water = Max(mega->vertex_high_water, mega->vertex_cursor);
    mega->index_high_water = Max(mega->index_high_water, mega->index_cursor);
    mega->decode_metadata_high_water =
        Max(mega->decode_metadata_high_water, mega->decode_metadata_live_bytes);
    *out_handle = (VkrMeshHandle){.id = i + 1, .generation = slot->generation};
    return true_v;
  }
  return false_v;
}

bool8_t vkr_metal_packet_renderer_create_mesh(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketMeshCreateInfo *create_info,
    VkrMeshHandle *out_handle) {
  return vkr_metal_packet_renderer_create_mesh_internal(
      renderer, create_info, VKR_MESH_HANDLE_INVALID, out_handle);
}

bool8_t vkr_metal_packet_renderer_create_loaded_mesh(
    VkrMetalPacketRenderer *renderer,
    const struct VkrMeshLoaderResult *loader_result,
    VkrMeshHandle *out_handle) {
  if (!out_handle)
    return false_v;
  *out_handle = VKR_MESH_HANDLE_INVALID;
  if (!renderer || !loader_result || !loader_result->has_mesh_buffer ||
      loader_result->mesh_buffer.vertex_size != sizeof(VkrPackedStaticVertex) ||
      loader_result->mesh_buffer.vertex_layout !=
          VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1 ||
      loader_result->mesh_buffer.decode.flags !=
          VKR_GPU_GEOMETRY_DECODE_STATIC_V1 ||
      loader_result->mesh_buffer.index_size != sizeof(uint32_t) ||
      loader_result->submeshes.length == 0 ||
      loader_result->submeshes.length > renderer->max_submeshes_per_mesh ||
      loader_result->submeshes.length > VKR_METAL_PACKET_LOADER_SUBMESH_MAX)
    return false_v;
  VkrMetalPacketSubmeshCreateInfo
      submeshes[VKR_METAL_PACKET_LOADER_SUBMESH_MAX];
  for (uint64_t i = 0; i < loader_result->submeshes.length; ++i) {
    const VkrMeshLoaderSubmeshRange *range = &loader_result->submeshes.data[i];
    submeshes[i] = (VkrMetalPacketSubmeshCreateInfo){
        .first_index = range->first_index,
        .index_count = range->index_count,
        .vertex_offset = range->vertex_offset,
    };
  }
  return vkr_metal_packet_renderer_create_mesh(
      renderer,
      &(VkrMetalPacketMeshCreateInfo){
          .vertices = loader_result->mesh_buffer.vertices,
          .vertex_count = loader_result->mesh_buffer.vertex_count,
          .decode = loader_result->mesh_buffer.decode,
          .indices = loader_result->mesh_buffer.indices,
          .index_count = loader_result->mesh_buffer.index_count,
          .submeshes = submeshes,
          .submesh_count = (uint32_t)loader_result->submeshes.length,
      },
      out_handle);
}

bool8_t vkr_metal_packet_renderer_publish_loaded_mesh(
    VkrMetalPacketRenderer *renderer, VkrGeometryHandle handle,
    const struct VkrMeshLoaderResult *loader_result) {
  if (!renderer || !loader_result || !loader_result->has_mesh_buffer ||
      loader_result->mesh_buffer.vertex_size != sizeof(VkrPackedStaticVertex) ||
      loader_result->mesh_buffer.vertex_layout !=
          VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1 ||
      loader_result->mesh_buffer.decode.flags !=
          VKR_GPU_GEOMETRY_DECODE_STATIC_V1 ||
      loader_result->mesh_buffer.index_size != sizeof(uint32_t) ||
      handle.id == 0 || handle.id > renderer->max_meshes ||
      loader_result->submeshes.length == 0 ||
      loader_result->submeshes.length > renderer->max_submeshes_per_mesh ||
      loader_result->submeshes.length > VKR_METAL_PACKET_LOADER_SUBMESH_MAX)
    return false_v;
  VkrMetalPacketSubmeshCreateInfo
      submeshes[VKR_METAL_PACKET_LOADER_SUBMESH_MAX];
  for (uint64_t i = 0; i < loader_result->submeshes.length; ++i) {
    const VkrMeshLoaderSubmeshRange *range = &loader_result->submeshes.data[i];
    submeshes[i] = (VkrMetalPacketSubmeshCreateInfo){
        .first_index = range->first_index,
        .index_count = range->index_count,
        .vertex_offset = range->vertex_offset,
    };
  }
  VkrMetalPacketMesh *existing = &renderer->meshes[handle.id - 1];
  if (existing->live) {
    if (existing->generation != handle.generation ||
        existing->vertex_count != loader_result->mesh_buffer.vertex_count ||
        existing->index_count != loader_result->mesh_buffer.index_count)
      return false_v;
    for (uint32_t i = 0; i < loader_result->submeshes.length; ++i) {
      const VkrMetalPacketSubmeshCreateInfo *submesh = &submeshes[i];
      if (submesh->index_count == 0 ||
          submesh->first_index > existing->index_count ||
          submesh->index_count > existing->index_count - submesh->first_index)
        return false_v;
    }
    VkrMetalPacketSubmeshCreateInfo *destination =
        renderer->submeshes +
        (uint64_t)(handle.id - 1) * renderer->max_submeshes_per_mesh;
    MemZero(destination,
            (uint64_t)renderer->max_submeshes_per_mesh * sizeof(*destination));
    MemCopy(destination, submeshes,
            loader_result->submeshes.length * sizeof(*submeshes));
    existing->submesh_count = (uint32_t)loader_result->submeshes.length;
    return true_v;
  }
  VkrMeshHandle published = VKR_MESH_HANDLE_INVALID;
  const VkrMeshHandle requested = {.id = handle.id,
                                   .generation = handle.generation};
  return vkr_metal_packet_renderer_create_mesh_internal(
             renderer,
             &(VkrMetalPacketMeshCreateInfo){
                 .vertices = loader_result->mesh_buffer.vertices,
                 .vertex_count = loader_result->mesh_buffer.vertex_count,
                 .decode = loader_result->mesh_buffer.decode,
                 .indices = loader_result->mesh_buffer.indices,
                 .index_count = loader_result->mesh_buffer.index_count,
                 .submeshes = submeshes,
                 .submesh_count = (uint32_t)loader_result->submeshes.length,
             },
             requested, &published) &&
         published.id == handle.id && published.generation == handle.generation;
}

bool8_t vkr_metal_packet_renderer_publish_geometry(
    VkrMetalPacketRenderer *renderer, VkrGeometryHandle handle,
    const struct VkrGeometryConfig *geometry) {
  if (!renderer || !geometry || !geometry->vertices || !geometry->indices ||
      geometry->vertex_layout != VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1 ||
      geometry->vertex_size != sizeof(VkrPackedStaticVertex) ||
      geometry->decode.flags != VKR_GPU_GEOMETRY_DECODE_STATIC_V1 ||
      geometry->index_size != sizeof(uint32_t) || geometry->vertex_count == 0 ||
      geometry->index_count == 0)
    return false_v;
  VkrMeshHandle published = VKR_MESH_HANDLE_INVALID;
  const VkrMeshHandle requested = {.id = handle.id,
                                   .generation = handle.generation};
  return vkr_metal_packet_renderer_create_mesh_internal(
             renderer,
             &(VkrMetalPacketMeshCreateInfo){
                 .vertices = geometry->vertices,
                 .vertex_count = geometry->vertex_count,
                 .decode = geometry->decode,
                 .indices = geometry->indices,
                 .index_count = geometry->index_count,
             },
             requested, &published) &&
         published.id == requested.id &&
         published.generation == requested.generation;
}

bool8_t vkr_metal_packet_renderer_destroy_mesh(VkrMetalPacketRenderer *renderer,
                                               VkrMeshHandle handle) {
  if (!renderer || handle.id == 0 || handle.id > renderer->max_meshes)
    return false_v;
  VkrMetalPacketMesh *slot = &renderer->meshes[handle.id - 1];
  if (!slot->live || slot->generation != handle.generation)
    return false_v;
  VkrMetalPacketGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  const uint64_t vertex_bytes =
      (uint64_t)slot->vertex_count * sizeof(VkrPackedStaticVertex);
  const uint64_t index_bytes = (uint64_t)slot->index_count * sizeof(uint32_t);
  mega->vertex_live_bytes -= Min(mega->vertex_live_bytes, vertex_bytes);
  mega->index_live_bytes -= Min(mega->index_live_bytes, index_bytes);
  mega->decode_metadata_live_bytes -=
      Min(mega->decode_metadata_live_bytes,
          (uint64_t)sizeof(VkrGpuGeometryDecodeRecord));
  slot->live = false_v;
  slot->generation =
      slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
  slot->vertices = (VkrMetalBufferResource){0};
  slot->indices = (VkrMetalBufferResource){0};
  slot->gpu_row = (VkrGpuGeometryRow){0};
  MemZero(renderer->submeshes +
              (uint64_t)(handle.id - 1) * renderer->max_submeshes_per_mesh,
          (uint64_t)renderer->max_submeshes_per_mesh *
              sizeof(*renderer->submeshes));
  slot->submesh_count = 0;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_create_material(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketMaterialCreateInfo *create_info,
    VkrMaterialHandle *out_handle) {
  if (!out_handle)
    return false_v;
  *out_handle = VKR_MATERIAL_HANDLE_INVALID;
  if (!renderer || renderer->frame_prepared ||
      !vkr_metal_packet_material_info_valid(create_info))
    return false_v;
  if (!@available(macOS 26.0, *))
    return false_v;
  id<MTLSamplerState> material_sampler =
      vkr_metal_packet_default_material_sampler(renderer, 1);
  if (!material_sampler || material_sampler.gpuResourceID._impl == 0)
    return false_v;
  const uint64_t material_sampler_id = material_sampler.gpuResourceID._impl;
  for (uint32_t i = 0; i < renderer->max_materials; ++i) {
    VkrMetalPacketMaterial *slot = &renderer->packet_materials[i];
    if (slot->live)
      continue;
    const uint32_t generation = slot->generation;
    MemZero(slot->textures, sizeof(slot->textures));
    slot->row = (VkrMetalMaterialHandle){0};
    slot->row_index = 0;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.mipmapLevelCount = 1;
    descriptor.usage = MTLTextureUsageShaderRead;
    bool8_t textures_created = true_v;
    for (uint32_t texture_index = 0;
         textures_created && texture_index < ArrayCount(slot->textures);
         ++texture_index) {
      const VkrMetalPacketRgba8TextureCreateInfo *texture =
          &create_info->textures[texture_index];
      descriptor.width = texture->width;
      descriptor.height = texture->height;
      const uint64_t texture_size =
          (uint64_t)texture->width * texture->height * 4u;
      textures_created =
          vkr_metal_memory_device_create_texture(
              renderer->memory, descriptor, &slot->textures[texture_index]) ==
              VKR_METAL_MEMORY_STATUS_OK &&
          vkr_metal_packet_upload(
              renderer, texture->pixels, texture_size, nil, NULL, 0, nil,
              (id<MTLTexture>)slot->textures[texture_index].metal_texture,
              texture->width, texture->height);
    }
    [descriptor release];
    if (!textures_created) {
      for (uint32_t texture_index = 0;
           texture_index < ArrayCount(slot->textures); ++texture_index) {
        if (!slot->textures[texture_index].metal_texture)
          continue;
        (void)vkr_metal_memory_device_retire(
            renderer->memory, slot->textures[texture_index].handle,
            renderer->submit_value);
      }
      (void)vkr_metal_memory_device_collect(renderer->memory,
                                            renderer->submit_value, NULL);
      *slot = (VkrMetalPacketMaterial){.generation = generation};
      return false_v;
    }
    const VkrPacketMaterialConstants material_constants =
        vkr_packet_derive_material_constants(&create_info->pbr,
                                             create_info->alpha_cutoff,
                                             create_info->alpha_mode);
    VkrMetalMaterialPublishedRow row = {
        .material =
            {
                .base_color_texture_id = slot->textures[0].resource_id,
                .normal_texture_id = slot->textures[1].resource_id,
                .orm_texture_id = slot->textures[2].resource_id,
                .emissive_texture_id = slot->textures[3].resource_id,
                .base_color_sampler_id = material_sampler_id,
                .normal_sampler_id = material_sampler_id,
                .orm_sampler_id = material_sampler_id,
                .emissive_sampler_id = material_sampler_id,
                .material_id = create_info->material_id,
                .flags = create_info->texture_flags,
                .alpha_mode = material_constants.alpha_mode,
                .temporal_reactivity =
                    Clamp(create_info->pbr.temporal_reactivity, 0.0f, 1.0f),
                .material_emissive = material_constants.emissive,
                .material_dielectric_specular =
                    material_constants.dielectric_specular,
                .material_surface = material_constants.surface,
                .material_alpha = material_constants.alpha,
                .material_attenuation_color =
                    material_constants.attenuation_color,
            },
        .transmission =
            {
                .transmission_texture_id = slot->textures[4].resource_id,
                .thickness_texture_id = slot->textures[5].resource_id,
                .transmission_sampler_id = material_sampler_id,
                .thickness_sampler_id = material_sampler_id,
            },
    };
    MemCopy(row.material.tint, create_info->tint, sizeof(row.material.tint));
    const VkrMetalMaterialStatus publish_status =
        vkr_metal_material_table_device_publish(renderer->materials, &row,
                                                &slot->row);
    const VkrMetalMaterialStatus resolve_status =
        publish_status == VKR_METAL_MATERIAL_STATUS_OK
            ? vkr_metal_material_table_device_resolve(
                  renderer->materials, slot->row, &slot->row_index)
            : publish_status;
    if (publish_status != VKR_METAL_MATERIAL_STATUS_OK ||
        resolve_status != VKR_METAL_MATERIAL_STATUS_OK) {
      if (publish_status == VKR_METAL_MATERIAL_STATUS_OK)
        (void)vkr_metal_material_table_device_retire(
            renderer->materials, slot->row, renderer->submit_value);
      for (uint32_t texture_index = 0;
           texture_index < ArrayCount(slot->textures); ++texture_index) {
        (void)vkr_metal_memory_device_retire(
            renderer->memory, slot->textures[texture_index].handle,
            renderer->submit_value);
      }
      (void)vkr_metal_memory_device_collect(renderer->memory,
                                            renderer->submit_value, NULL);
      *slot = (VkrMetalPacketMaterial){.generation = generation};
      return false_v;
    }
    slot->generation =
        slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
    if (slot->generation == 0)
      slot->generation = 1;
    slot->last_use_submit_value = renderer->submit_value;
    slot->pbr = create_info->pbr;
    slot->alpha_mode = create_info->alpha_mode;
    slot->alpha_cutoff = create_info->alpha_cutoff;
    slot->owns_textures = true_v;
    slot->double_sided = false_v;
    slot->live = true_v;
    *out_handle =
        (VkrMaterialHandle){.id = i + 1, .generation = slot->generation};
    return true_v;
  }
  return false_v;
}

bool8_t
vkr_metal_packet_renderer_destroy_material(VkrMetalPacketRenderer *renderer,
                                           VkrMaterialHandle handle) {
  if (!renderer || handle.id == 0 || handle.id > renderer->max_materials)
    return false_v;
  VkrMetalPacketMaterial *slot = &renderer->packet_materials[handle.id - 1];
  if (!slot->live || slot->generation != handle.generation)
    return false_v;
  if (vkr_metal_material_table_device_retire(renderer->materials, slot->row,
                                             slot->last_use_submit_value) !=
      VKR_METAL_MATERIAL_STATUS_OK)
    return false_v;
  for (uint32_t texture_index = 0; texture_index < ArrayCount(slot->textures);
       ++texture_index) {
    if (slot->owns_textures) {
      if (vkr_metal_memory_device_retire(
              renderer->memory, slot->textures[texture_index].handle,
              slot->last_use_submit_value) != VKR_METAL_MEMORY_STATUS_OK)
        return false_v;
    } else {
      VkrMetalPacketTexture *texture = vkr_metal_packet_resolve_texture(
          renderer, slot->texture_handles[texture_index]);
      if (texture &&
          texture->last_use_submit_value < slot->last_use_submit_value)
        texture->last_use_submit_value = slot->last_use_submit_value;
    }
  }
  slot->live = false_v;
  slot->generation =
      slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
  MemZero(slot->textures, sizeof(slot->textures));
  MemZero(slot->texture_handles, sizeof(slot->texture_handles));
  slot->row = (VkrMetalMaterialHandle){0};
  slot->row_index = 0;
  slot->owns_textures = false_v;
  slot->double_sided = false_v;
  return true_v;
}

bool8_t
vkr_metal_packet_renderer_publish_material(VkrMetalPacketRenderer *renderer,
                                           VkrMaterialHandle handle,
                                           const struct VkrMaterial *material) {
  if (!renderer || !material || renderer->frame_prepared || handle.id == 0 ||
      handle.id > renderer->max_materials ||
      handle.generation == VKR_INVALID_ID || material->id != handle.id ||
      material->generation != handle.generation)
    return false_v;
  if (!@available(macOS 26.0, *))
    return false_v;
  VkrMetalPacketMaterial *slot = &renderer->packet_materials[handle.id - 1];
  if (slot->live)
    return false_v;
  VkrTextureSlot source_slots[6] = {
      VKR_TEXTURE_SLOT_DIFFUSE,
      material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled
          ? VKR_TEXTURE_SLOT_NORMAL
          : VKR_TEXTURE_SLOT_DIFFUSE,
      material->textures[VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS].enabled
          ? VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS
          : (material->textures[VKR_TEXTURE_SLOT_SPECULAR].enabled
                 ? VKR_TEXTURE_SLOT_SPECULAR
                 : VKR_TEXTURE_SLOT_DIFFUSE),
      material->textures[VKR_TEXTURE_SLOT_EMISSION].enabled
          ? VKR_TEXTURE_SLOT_EMISSION
          : VKR_TEXTURE_SLOT_DIFFUSE,
      material->textures[VKR_TEXTURE_SLOT_TRANSMISSION].enabled
          ? VKR_TEXTURE_SLOT_TRANSMISSION
          : VKR_TEXTURE_SLOT_DIFFUSE,
      material->textures[VKR_TEXTURE_SLOT_THICKNESS].enabled
          ? VKR_TEXTURE_SLOT_THICKNESS
          : VKR_TEXTURE_SLOT_DIFFUSE,
  };
  VkrMetalPacketTexture *textures[6] = {0};
  for (uint32_t i = 0; i < ArrayCount(source_slots); ++i) {
    const VkrMaterialTexture *source = &material->textures[source_slots[i]];
    if (source->handle.id == 0 ||
        !(textures[i] =
              vkr_metal_packet_resolve_texture(renderer, source->handle)) ||
        textures[i]->sampler_resource_id == 0)
      return false_v;
    slot->texture_handles[i] = source->handle;
  }
  uint32_t flags = 0;
  if (material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled)
    flags |= VKR_METAL_PACKET_MATERIAL_TEXTURE_NORMAL;
  if (material->textures[VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS].enabled)
    flags |= VKR_METAL_PACKET_MATERIAL_TEXTURE_ORM;
  if (material->textures[VKR_TEXTURE_SLOT_EMISSION].enabled)
    flags |= VKR_METAL_PACKET_MATERIAL_TEXTURE_EMISSIVE;
  if (material->textures[VKR_TEXTURE_SLOT_TRANSMISSION].enabled)
    flags |= VKR_METAL_PACKET_MATERIAL_TEXTURE_TRANSMISSION;
  if (material->textures[VKR_TEXTURE_SLOT_THICKNESS].enabled)
    flags |= VKR_METAL_PACKET_MATERIAL_TEXTURE_THICKNESS;
  const Vec4 tint = material->material_type == VKR_MATERIAL_TYPE_PBR
                        ? material->pbr.base_color
                        : material->phong.diffuse_color;
  VkrPbrProperties pbr = material->pbr;
  if (material->material_type != VKR_MATERIAL_TYPE_PBR) {
    pbr.base_color = material->phong.diffuse_color;
    pbr.emissive_factor = material->phong.emission_color;
  }
  const VkrPacketMaterialConstants material_constants =
      vkr_packet_derive_material_constants(&pbr, material->alpha_cutoff,
                                           material->alpha_mode);
  VkrMetalMaterialPublishedRow row = {
      .material =
          {
              .base_color_texture_id = textures[0]->resource.resource_id,
              .normal_texture_id = textures[1]->resource.resource_id,
              .orm_texture_id = textures[2]->resource.resource_id,
              .emissive_texture_id = textures[3]->resource.resource_id,
              .base_color_sampler_id = textures[0]->sampler_resource_id,
              .normal_sampler_id = textures[1]->sampler_resource_id,
              .orm_sampler_id = textures[2]->sampler_resource_id,
              .emissive_sampler_id = textures[3]->sampler_resource_id,
              .material_id = handle.id,
              .flags = flags,
              .alpha_mode = material_constants.alpha_mode,
              .temporal_reactivity = Clamp(pbr.temporal_reactivity, 0.0f, 1.0f),
              .material_emissive = material_constants.emissive,
              .material_dielectric_specular =
                  material_constants.dielectric_specular,
              .material_surface = material_constants.surface,
              .material_alpha = material_constants.alpha,
              .material_attenuation_color =
                  material_constants.attenuation_color,
          },
      .transmission =
          {
              .transmission_texture_id = textures[4]->resource.resource_id,
              .thickness_texture_id = textures[5]->resource.resource_id,
              .transmission_sampler_id = textures[4]->sampler_resource_id,
              .thickness_sampler_id = textures[5]->sampler_resource_id,
          },
  };
  const float32_t tint_values[4] = {tint.x, tint.y, tint.z, tint.w};
  MemCopy(row.material.tint, tint_values, sizeof(row.material.tint));
  if (vkr_metal_material_table_device_publish(renderer->materials, &row,
                                              &slot->row) !=
          VKR_METAL_MATERIAL_STATUS_OK ||
      vkr_metal_material_table_device_resolve(renderer->materials, slot->row,
                                              &slot->row_index) !=
          VKR_METAL_MATERIAL_STATUS_OK) {
    if (slot->row.generation != 0)
      (void)vkr_metal_material_table_device_retire(
          renderer->materials, slot->row, renderer->submit_value);
    *slot = (VkrMetalPacketMaterial){0};
    return false_v;
  }
  slot->generation = handle.generation;
  slot->last_use_submit_value = renderer->submit_value;
  slot->pbr = pbr;
  slot->alpha_mode = material->alpha_mode;
  slot->alpha_cutoff = material->alpha_cutoff;
  slot->owns_textures = false_v;
  slot->double_sided = material->double_sided;
  slot->live = true_v;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_create_rgba8_texture(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketRgba8TextureCreateInfo *create_info,
    VkrTextureHandle *out_handle) {
  if (!renderer || !create_info || !out_handle || renderer->frame_prepared ||
      !create_info->pixels || create_info->width == 0 ||
      create_info->height == 0 ||
      create_info->width > VKR_TEXTURE_MAX_DIMENSION ||
      create_info->height > VKR_TEXTURE_MAX_DIMENSION ||
      (uint64_t)create_info->width >
          UINT64_MAX / ((uint64_t)create_info->height * 4u))
    return false_v;
  *out_handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!@available(macOS 26.0, *))
    return false_v;
  id<MTLSamplerState> sampler =
      vkr_metal_packet_default_material_sampler(renderer, 1u);
  if (!sampler || sampler.gpuResourceID._impl == 0)
    return false_v;
  for (uint32_t i = 0; i < renderer->max_textures; ++i) {
    VkrMetalPacketTexture *slot = &renderer->textures[i];
    if (slot->live)
      continue;
    const uint32_t generation = slot->generation;
    slot->resource = (VkrMetalTextureResource){0};
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.width = create_info->width;
    descriptor.height = create_info->height;
    descriptor.mipmapLevelCount = 1;
    descriptor.usage = MTLTextureUsageShaderRead;
    const bool8_t created = vkr_metal_memory_device_create_texture(
                                renderer->memory, descriptor,
                                &slot->resource) == VKR_METAL_MEMORY_STATUS_OK;
    [descriptor release];
    const uint64_t byte_count =
        (uint64_t)create_info->width * create_info->height * 4u;
    if (!created || !vkr_metal_packet_upload(
                        renderer, create_info->pixels, byte_count, nil, NULL, 0,
                        nil, (id<MTLTexture>)slot->resource.metal_texture,
                        create_info->width, create_info->height)) {
      if (slot->resource.metal_texture) {
        (void)vkr_metal_memory_device_retire(
            renderer->memory, slot->resource.handle, renderer->submit_value);
        (void)vkr_metal_memory_device_collect(renderer->memory,
                                              renderer->submit_value, NULL);
      }
      *slot = (VkrMetalPacketTexture){.generation = generation};
      return false_v;
    }
    slot->generation =
        slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
    if (slot->generation == 0)
      slot->generation = 1;
    slot->sampler_resource_id = sampler.gpuResourceID._impl;
    slot->last_use_submit_value = renderer->submit_value;
    slot->live = true_v;
    *out_handle =
        (VkrTextureHandle){.id = i + 1, .generation = slot->generation};
    return true_v;
  }
  return false_v;
}

bool8_t vkr_metal_packet_renderer_publish_prepared_texture(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle handle,
    const struct VkrTexturePreparedLoad *prepared) {
  if (!renderer || !prepared || renderer->frame_prepared || handle.id == 0 ||
      handle.id > renderer->max_textures ||
      handle.generation == VKR_INVALID_ID || !prepared->upload_data ||
      prepared->upload_data_size == 0 || !prepared->upload_regions ||
      prepared->upload_region_count == 0 || prepared->description.width == 0 ||
      prepared->description.height == 0 ||
      prepared->description.width > VKR_TEXTURE_MAX_DIMENSION ||
      prepared->description.height > VKR_TEXTURE_MAX_DIMENSION ||
      prepared->description.type >= VKR_TEXTURE_TYPE_COUNT ||
      prepared->upload_mip_levels == 0 ||
      prepared->upload_mip_levels > VKR_METAL_PACKET_MAX_TEXTURE_MIPS ||
      prepared->upload_array_layers == 0 ||
      prepared->upload_array_layers > VKR_TEXTURE_MAX_ARRAY_LAYERS)
    return false_v;
  if (!@available(macOS 26.0, *))
    return false_v;
  VkrMetalPacketTexture *slot = &renderer->textures[handle.id - 1];
  if (slot->live)
    return false_v;
  const MTLPixelFormat pixel_format =
      vkr_metal_packet_pixel_format(prepared->description.format);
  if (pixel_format == MTLPixelFormatInvalid)
    return false_v;
  const uint32_t prior_generation = slot->generation;
  slot->resource = (VkrMetalTextureResource){0};
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  const bool8_t cubemap =
      prepared->description.type == VKR_TEXTURE_TYPE_CUBE_MAP;
  const bool8_t cubemap_array =
      prepared->description.type == VKR_TEXTURE_TYPE_CUBE_MAP_ARRAY;
  const bool8_t array = prepared->description.type == VKR_TEXTURE_TYPE_2D_ARRAY;
  const bool8_t cube_shape = cubemap || cubemap_array;
  const bool8_t valid_layers =
      (prepared->description.type == VKR_TEXTURE_TYPE_2D &&
       prepared->upload_array_layers == 1u) ||
      (array && prepared->upload_array_layers > 1u) ||
      (cubemap && prepared->upload_array_layers == 6u) ||
      (cubemap_array && prepared->upload_array_layers > 6u &&
       prepared->upload_array_layers % 6u == 0u);
  if (!valid_layers || (cube_shape && prepared->description.width !=
                                          prepared->description.height)) {
    [descriptor release];
    return false_v;
  }
  descriptor.textureType = cubemap         ? MTLTextureTypeCube
                           : cubemap_array ? MTLTextureTypeCubeArray
                           : array         ? MTLTextureType2DArray
                                           : MTLTextureType2D;
  descriptor.pixelFormat = pixel_format;
  descriptor.width = prepared->description.width;
  descriptor.height = prepared->description.height;
  descriptor.mipmapLevelCount = prepared->upload_mip_levels;
  if (array) {
    descriptor.arrayLength = prepared->upload_array_layers;
  } else if (cubemap_array) {
    descriptor.arrayLength = prepared->upload_array_layers / 6u;
  }
  descriptor.usage = MTLTextureUsageShaderRead;
  const bool8_t created = vkr_metal_memory_device_create_texture(
                              renderer->memory, descriptor, &slot->resource) ==
                          VKR_METAL_MEMORY_STATUS_OK;
  [descriptor release];
  const VkrMetalPacketTextureUpload payload = {
      .data = prepared->upload_data,
      .data_size = prepared->upload_data_size,
      .mip_levels = prepared->upload_mip_levels,
      .array_layers = prepared->upload_array_layers,
      .is_compressed = prepared->upload_is_compressed,
      .region_count = prepared->upload_region_count,
      .regions = prepared->upload_regions,
  };
  const bool8_t batched = created && renderer->texture_upload_batch.enabled;
  const bool8_t uploaded =
      created &&
      (batched ? vkr_metal_packet_upload_texture_batched(
                     renderer, &payload, prepared->description.format,
                     (id<MTLTexture>)slot->resource.metal_texture)
               : vkr_metal_packet_upload_texture(
                     renderer, &payload, prepared->description.format,
                     (id<MTLTexture>)slot->resource.metal_texture));
  id<MTLSamplerState> sampler =
      uploaded ? vkr_metal_packet_sampler(renderer, &prepared->description,
                                          prepared->upload_mip_levels)
               : nil;
  if (batched && uploaded && (!sampler || sampler.gpuResourceID._impl == 0) &&
      !vkr_metal_packet_texture_upload_batch_flush(renderer, false_v))
    return false_v;
  if (!uploaded || !sampler || sampler.gpuResourceID._impl == 0) {
    if (slot->resource.metal_texture) {
      (void)vkr_metal_memory_device_retire(
          renderer->memory, slot->resource.handle, renderer->submit_value);
      (void)vkr_metal_memory_device_collect(renderer->memory,
                                            renderer->submit_value, NULL);
    }
    *slot = (VkrMetalPacketTexture){.generation = prior_generation};
    return false_v;
  }
  slot->sampler_resource_id = sampler.gpuResourceID._impl;
  slot->generation = handle.generation;
  slot->last_use_submit_value =
      batched ? renderer->submit_value + 1u : renderer->submit_value;
  slot->live = true_v;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_publish_writable_texture(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  if (!renderer || !description || renderer->frame_prepared || handle.id == 0 ||
      handle.id > renderer->max_textures ||
      handle.generation == VKR_INVALID_ID || description->id != handle.id ||
      description->generation != handle.generation || description->width == 0 ||
      description->height == 0 ||
      description->width > VKR_TEXTURE_MAX_DIMENSION ||
      description->height > VKR_TEXTURE_MAX_DIMENSION ||
      description->sample_count != VKR_SAMPLE_COUNT_1 ||
      (description->type != VKR_TEXTURE_TYPE_2D &&
       description->type != VKR_TEXTURE_TYPE_CUBE_MAP) ||
      (description->type == VKR_TEXTURE_TYPE_CUBE_MAP &&
       description->width != description->height)) {
    return false_v;
  }
  if (!@available(macOS 26.0, *))
    return false_v;

  VkrMetalPacketTexture *slot = &renderer->textures[handle.id - 1];
  if (slot->live)
    return false_v;
  const uint32_t prior_generation = slot->generation;
  const MTLPixelFormat pixel_format =
      vkr_metal_packet_pixel_format(description->format);
  if (pixel_format == MTLPixelFormatInvalid)
    return false_v;

  const uint32_t mip_count =
      description->mip_filter == VKR_MIP_FILTER_NONE
          ? 1u
          : vkr_metal_packet_mip_count(
                Max(description->width, description->height));
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = description->type == VKR_TEXTURE_TYPE_CUBE_MAP
                               ? MTLTextureTypeCube
                               : MTLTextureType2D;
  descriptor.pixelFormat = pixel_format;
  descriptor.width = description->width;
  descriptor.height = description->height;
  descriptor.mipmapLevelCount = mip_count;
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  const bool8_t created = vkr_metal_memory_device_create_texture(
                              renderer->memory, descriptor, &slot->resource) ==
                          VKR_METAL_MEMORY_STATUS_OK;
  [descriptor release];
  id<MTLSamplerState> sampler =
      created ? vkr_metal_packet_sampler(renderer, description, mip_count)
              : nil;
  if (!created || !sampler || sampler.gpuResourceID._impl == 0) {
    if (slot->resource.metal_texture) {
      (void)vkr_metal_memory_device_retire(
          renderer->memory, slot->resource.handle, renderer->submit_value);
      (void)vkr_metal_memory_device_collect(renderer->memory,
                                            renderer->submit_value, NULL);
    }
    *slot = (VkrMetalPacketTexture){.generation = prior_generation};
    return false_v;
  }

  slot->sampler_resource_id = sampler.gpuResourceID._impl;
  slot->generation = handle.generation;
  slot->last_use_submit_value = renderer->submit_value;
  slot->live = true_v;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_update_texture_sampler(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  if (!renderer || !description || renderer->frame_prepared || handle.id == 0 ||
      description->id != handle.id ||
      description->generation != handle.generation) {
    return false_v;
  }
  if (!@available(macOS 26.0, *)) {
    return false_v;
  }

  VkrMetalPacketTexture *texture =
      vkr_metal_packet_resolve_texture(renderer, handle);
  if (!texture) {
    return false_v;
  }
  for (uint32_t material_index = 0; material_index < renderer->max_materials;
       ++material_index) {
    const VkrMetalPacketMaterial *material =
        &renderer->packet_materials[material_index];
    if (!material->live || material->owns_textures) {
      continue;
    }
    for (uint32_t texture_index = 0;
         texture_index < ArrayCount(material->texture_handles);
         ++texture_index) {
      const VkrTextureHandle referenced =
          material->texture_handles[texture_index];
      if (referenced.id == handle.id &&
          referenced.generation == handle.generation) {
        log_warn("Metal sampler update for texture %u:%u requires material "
                 "republication; update rejected",
                 handle.id, handle.generation);
        return false_v;
      }
    }
  }
  id<MTLTexture> metal_texture =
      (id<MTLTexture>)texture->resource.metal_texture;
  const uint32_t mip_level_count = (uint32_t)metal_texture.mipmapLevelCount;
  id<MTLSamplerState> sampler =
      vkr_metal_packet_sampler(renderer, description, mip_level_count);
  if (!sampler || sampler.gpuResourceID._impl == 0) {
    return false_v;
  }
  texture->sampler_resource_id = sampler.gpuResourceID._impl;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_validate_ibl_textures(
    const VkrMetalPacketTexture *source, const VkrMetalPacketTexture *prefilter)
    API_AVAILABLE(macos(26.0)) {
  if (!source || !prefilter || source == prefilter) {
    return false_v;
  }

  id<MTLTexture> source_texture =
      (id<MTLTexture>)source->resource.metal_texture;
  id<MTLTexture> prefilter_texture =
      (id<MTLTexture>)prefilter->resource.metal_texture;
  return source_texture && prefilter_texture &&
         source_texture.textureType == MTLTextureTypeCube &&
         prefilter_texture.textureType == MTLTextureTypeCube &&
         source_texture.width == source_texture.height &&
         prefilter_texture.width == prefilter_texture.height &&
         prefilter_texture.pixelFormat == MTLPixelFormatRGBA16Float &&
         (source_texture.usage & MTLTextureUsageShaderRead) != 0 &&
         (prefilter_texture.usage & MTLTextureUsageShaderWrite) != 0 &&
         source_texture.mipmapLevelCount > 0 &&
         source_texture.mipmapLevelCount <= VKR_METAL_PACKET_MAX_TEXTURE_MIPS &&
         prefilter_texture.mipmapLevelCount > 0 &&
         prefilter_texture.mipmapLevelCount <=
             VKR_METAL_PACKET_MAX_TEXTURE_MIPS;
}

bool8_t vkr_metal_packet_renderer_bake_ibl_cubemap(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle source_handle,
    VkrTextureHandle prefilter_handle, float32_t sh_deringing) {
  if (!renderer || renderer->frame_prepared) {
    return false_v;
  }
  if (!@available(macOS 26.0, *)) {
    return false_v;
  }
  VkrMetalPacketTexture *source =
      vkr_metal_packet_resolve_texture(renderer, source_handle);
  VkrMetalPacketTexture *prefilter =
      vkr_metal_packet_resolve_texture(renderer, prefilter_handle);
  if (!vkr_metal_packet_validate_ibl_textures(source, prefilter))
    return false_v;

  id<MTLTexture> source_texture =
      (id<MTLTexture>)source->resource.metal_texture;
  id<MTLTexture> prefilter_texture =
      (id<MTLTexture>)prefilter->resource.metal_texture;
  const uint32_t prefilter_mip_count =
      (uint32_t)prefilter_texture.mipmapLevelCount;
  const uint64_t root_count = 1u + prefilter_mip_count;
  const uint64_t root_bytes = root_count * VKR_METAL_PACKET_ROOT_ALIGNMENT;
  VkrMetalRingSlice slice = {0};
  VkrMetalAddressPair upload = {0};
  id<MTLBuffer> upload_buffer = nil;
  id<MTL4ComputeCommandEncoder> encoder = nil;
  if (!vkr_metal_packet_begin_upload(renderer, root_bytes, &slice, &upload,
                                     &upload_buffer, &encoder)) {
    return false_v;
  }

  for (uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
    const uint64_t root_offset =
        (uint64_t)mip * VKR_METAL_PACKET_ROOT_ALIGNMENT;
    VkrMetalPacketPrefilterRoot *prefilter_root =
        (VkrMetalPacketPrefilterRoot *)((uint8_t *)upload.cpu_address +
                                        root_offset);
    *prefilter_root = (VkrMetalPacketPrefilterRoot){
        .source_texture_id = source->resource.resource_id,
        .target_texture_id = prefilter->resource.resource_id,
        .roughness =
            prefilter_mip_count > 1u
                ? (float32_t)mip / (float32_t)(prefilter_mip_count - 1u)
                : 0.0f,
        .source_face_size = (float32_t)source_texture.width,
        .source_mip_count = (float32_t)source_texture.mipmapLevelCount,
        .target_mip = mip,
    };
    const uint32_t mip_size = Max(1u, (uint32_t)prefilter_texture.width >> mip);
    [renderer->argument_table setAddress:upload.gpu_address + root_offset
                                 atIndex:0];
    [encoder setComputePipelineState:renderer->ibl_prefilter_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(mip_size, mip_size, 6)
        threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  }

  /* Exhaustion and projection failure are cold-path errors: the logical source
     keeps its prior publication, or black. */
  uint32_t sh_slot = VKR_SH_SLOT_BLACK;
  uint32_t projection_mip = 0u;
  uint32_t projection_extent = 0u;
  if (vkr_ibl_sh_projection_mip((uint32_t)source_texture.width,
                                (uint32_t)source_texture.mipmapLevelCount,
                                &projection_mip, &projection_extent)) {
    if (vkr_ibl_sh_pool_reserve(&renderer->sh_pool, &sh_slot) ==
        VKR_SH_POOL_STATUS_OK) {
      const uint64_t sh_root_offset =
          (uint64_t)prefilter_mip_count * VKR_METAL_PACKET_ROOT_ALIGNMENT;
      VkrMetalPacketShProjectRoot *sh_root =
          (VkrMetalPacketShProjectRoot *)((uint8_t *)upload.cpu_address +
                                          sh_root_offset);
      *sh_root = (VkrMetalPacketShProjectRoot){
          .source_texture_id = source->resource.resource_id,
          .destination = renderer->sh_coefficients.gpuAddress +
                         (uint64_t)sh_slot * VKR_SH_SLOT_BYTES,
          .source_face_size = projection_extent,
          .source_mip = projection_mip,
          /* The CPU owns pow(sinc_pi(l/3), sh_deringing) so the kernel carries
             no pow() and both sides cannot drift. */
          .window_band_0 = vkr_ibl_sh_window_factor(0u, sh_deringing),
          .window_band_1 = vkr_ibl_sh_window_factor(1u, sh_deringing),
          .window_band_2 = vkr_ibl_sh_window_factor(2u, sh_deringing),
      };
      [renderer->argument_table setAddress:upload.gpu_address + sh_root_offset
                                   atIndex:0];
      [encoder setComputePipelineState:renderer->ibl_sh_pipeline];
      [encoder setArgumentTable:renderer->argument_table];
      /* One workgroup per destination slot; the kernel's fixed reduction is
         what makes the result deterministic, so this must stay a single
         group. */
      [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      (void)vkr_ibl_sh_pool_mark_recorded(&renderer->sh_pool, sh_slot);
    } else {
      log_error("Metal SH coefficient pool exhausted; source keeps its "
                "previous coefficients");
    }
  }

  if (!vkr_metal_packet_submit_upload(renderer, encoder, slice)) {
    /* Nothing was submitted, so the candidate returns to the pool immediately
       rather than through retirement. */
    if (sh_slot != VKR_SH_SLOT_BLACK)
      (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool, sh_slot);
    return false_v;
  }
  source->last_use_submit_value = renderer->submit_value;
  prefilter->last_use_submit_value = renderer->submit_value;
  /* Submission succeeded, so the candidate becomes this source's published
     coefficients and the previous generation retires against its own last
     reader. */
  if (sh_slot != VKR_SH_SLOT_BLACK) {
    vkr_ibl_sh_pool_reference(&renderer->sh_pool, sh_slot,
                              renderer->submit_value);
    if (vkr_ibl_sh_pool_publish(&renderer->sh_pool, sh_slot) ==
        VKR_SH_POOL_STATUS_OK) {
      if (source->ibl_sh_slot != VKR_SH_SLOT_BLACK)
        (void)vkr_ibl_sh_pool_retire(&renderer->sh_pool, source->ibl_sh_slot);
      source->ibl_sh_slot = sh_slot;
    } else {
      (void)vkr_ibl_sh_pool_retire(&renderer->sh_pool, sh_slot);
    }
  }
  return true_v;
}

bool8_t vkr_metal_packet_renderer_bake_hdr_environment(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle equirect_handle,
    VkrTextureHandle source_handle, VkrTextureHandle prefilter_handle,
    float32_t sh_deringing) {
  if (!renderer || renderer->frame_prepared) {
    return false_v;
  }
  if (!@available(macOS 26.0, *)) {
    return false_v;
  }
  VkrMetalPacketTexture *equirect =
      vkr_metal_packet_resolve_texture(renderer, equirect_handle);
  VkrMetalPacketTexture *source =
      vkr_metal_packet_resolve_texture(renderer, source_handle);
  VkrMetalPacketTexture *prefilter =
      vkr_metal_packet_resolve_texture(renderer, prefilter_handle);
  if (!equirect || equirect == source || equirect == prefilter ||
      !vkr_metal_packet_validate_ibl_textures(source, prefilter)) {
    return false_v;
  }
  id<MTLTexture> equirect_texture =
      (id<MTLTexture>)equirect->resource.metal_texture;
  id<MTLTexture> source_texture =
      (id<MTLTexture>)source->resource.metal_texture;
  if (equirect_texture.textureType != MTLTextureType2D ||
      equirect_texture.width != equirect_texture.height * 2u ||
      (equirect_texture.usage & MTLTextureUsageShaderRead) == 0 ||
      source_texture.pixelFormat != MTLPixelFormatRGBA16Float ||
      (source_texture.usage & MTLTextureUsageShaderWrite) == 0) {
    return false_v;
  }

  VkrMetalRingSlice slice = {0};
  VkrMetalAddressPair upload = {0};
  id<MTLBuffer> upload_buffer = nil;
  id<MTL4ComputeCommandEncoder> encoder = nil;
  if (!vkr_metal_packet_begin_upload(renderer, VKR_METAL_PACKET_ROOT_ALIGNMENT,
                                     &slice, &upload, &upload_buffer,
                                     &encoder)) {
    return false_v;
  }
  VkrMetalPacketEquirectRoot *root = upload.cpu_address;
  *root = (VkrMetalPacketEquirectRoot){
      .source_texture_id = equirect->resource.resource_id,
      .target_texture_id = source->resource.resource_id,
      .target_size = (uint32_t)source_texture.width,
  };
  [renderer->argument_table setAddress:upload.gpu_address atIndex:0];
  [encoder setComputePipelineState:renderer->ibl_equirect_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(source_texture.width,
                                       source_texture.height, 6)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  [encoder barrierAfterEncoderStages:MTLStageDispatch
                 beforeEncoderStages:MTLStageBlit
                   visibilityOptions:MTL4VisibilityOptionDevice];
  [encoder generateMipmapsForTexture:source_texture];
  if (!vkr_metal_packet_submit_upload(renderer, encoder, slice)) {
    return false_v;
  }
  equirect->last_use_submit_value = renderer->submit_value;
  source->last_use_submit_value = renderer->submit_value;
  return vkr_metal_packet_renderer_bake_ibl_cubemap(
      renderer, source_handle, prefilter_handle, sh_deringing);
}

bool8_t vkr_metal_packet_renderer_create_cubemap(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketCubemapCreateInfo *create_info,
    VkrTextureHandle *out_handle) {
  if (!renderer || !create_info || !out_handle || renderer->frame_prepared ||
      !create_info->rgba8_faces || create_info->extent == 0 ||
      create_info->extent > VKR_TEXTURE_MAX_DIMENSION)
    return false_v;
  *out_handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!@available(macOS 26.0, *))
    return false_v;
  id<MTLSamplerState> sampler =
      vkr_metal_packet_default_material_sampler(renderer, 1u);
  if (!sampler || sampler.gpuResourceID._impl == 0)
    return false_v;
  for (uint32_t i = 0; i < renderer->max_textures; ++i) {
    VkrMetalPacketTexture *slot = &renderer->textures[i];
    if (slot->live)
      continue;
    const uint32_t generation = slot->generation;
    slot->resource = (VkrMetalTextureResource){0};
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureTypeCube;
    descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.width = create_info->extent;
    descriptor.height = create_info->extent;
    descriptor.depth = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.arrayLength = 1;
    descriptor.usage = MTLTextureUsageShaderRead;
    const bool8_t texture_created =
        vkr_metal_memory_device_create_texture(renderer->memory, descriptor,
                                               &slot->resource) ==
        VKR_METAL_MEMORY_STATUS_OK;
    [descriptor release];
    if (!texture_created ||
        !vkr_metal_packet_upload_cubemap(
            renderer, create_info->rgba8_faces, create_info->extent,
            (id<MTLTexture>)slot->resource.metal_texture)) {
      if (slot->resource.metal_texture) {
        (void)vkr_metal_memory_device_retire(
            renderer->memory, slot->resource.handle, renderer->submit_value);
        (void)vkr_metal_memory_device_collect(renderer->memory,
                                              renderer->submit_value, NULL);
      }
      *slot = (VkrMetalPacketTexture){.generation = generation};
      return false_v;
    }
    slot->generation =
        slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
    if (slot->generation == 0)
      slot->generation = 1;
    slot->sampler_resource_id = sampler.gpuResourceID._impl;
    slot->last_use_submit_value = renderer->submit_value;
    slot->live = true_v;
    *out_handle =
        (VkrTextureHandle){.id = i + 1, .generation = slot->generation};
    return true_v;
  }
  return false_v;
}

bool8_t vkr_metal_packet_renderer_create_hdr_environment(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketHdrEnvironmentCreateInfo *create_info,
    VkrTextureHandle *out_handle) {
  if (!renderer || !create_info || !out_handle || renderer->frame_prepared ||
      !create_info->rgba16_equirect || create_info->width == 0 ||
      create_info->height == 0 || create_info->cube_extent == 0 ||
      create_info->width > VKR_TEXTURE_MAX_DIMENSION ||
      create_info->height > VKR_TEXTURE_MAX_DIMENSION ||
      create_info->cube_extent > VKR_TEXTURE_MAX_DIMENSION ||
      create_info->height > UINT32_MAX / 2u ||
      create_info->width != create_info->height * 2u)
    return false_v;
  *out_handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!@available(macOS 26.0, *))
    return false_v;
  const uint32_t cube_mip_count =
      vkr_metal_packet_mip_count(create_info->cube_extent);
  id<MTLSamplerState> sampler =
      vkr_metal_packet_default_material_sampler(renderer, cube_mip_count);
  if (!sampler || sampler.gpuResourceID._impl == 0)
    return false_v;
  for (uint32_t i = 0; i < renderer->max_textures; ++i) {
    VkrMetalPacketTexture *slot = &renderer->textures[i];
    if (slot->live)
      continue;
    const uint32_t generation = slot->generation;
    VkrMetalTextureResource source = {0};
    slot->resource = (VkrMetalTextureResource){0};
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.width = create_info->width;
    descriptor.height = create_info->height;
    descriptor.mipmapLevelCount = 1;
    descriptor.usage = MTLTextureUsageShaderRead;
    const bool8_t source_created = vkr_metal_memory_device_create_texture(
                                       renderer->memory, descriptor, &source) ==
                                   VKR_METAL_MEMORY_STATUS_OK;
    descriptor.textureType = MTLTextureTypeCube;
    descriptor.width = create_info->cube_extent;
    descriptor.height = create_info->cube_extent;
    descriptor.mipmapLevelCount = cube_mip_count;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    const bool8_t cube_created =
        source_created && vkr_metal_memory_device_create_texture(
                              renderer->memory, descriptor, &slot->resource) ==
                              VKR_METAL_MEMORY_STATUS_OK;
    [descriptor release];
    const bool8_t converted =
        cube_created && vkr_metal_packet_convert_equirect(
                            renderer, create_info->rgba16_equirect,
                            create_info->width, create_info->height,
                            create_info->cube_extent, &source, &slot->resource);
    if (source.metal_texture) {
      (void)vkr_metal_memory_device_retire(renderer->memory, source.handle,
                                           renderer->submit_value);
    }
    if (!converted) {
      if (slot->resource.metal_texture) {
        (void)vkr_metal_memory_device_retire(
            renderer->memory, slot->resource.handle, renderer->submit_value);
      }
      (void)vkr_metal_memory_device_collect(
          renderer->memory, renderer->completion.signaledValue, NULL);
      *slot = (VkrMetalPacketTexture){.generation = generation};
      return false_v;
    }
    (void)vkr_metal_memory_device_collect(
        renderer->memory, renderer->completion.signaledValue, NULL);
    slot->generation =
        slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
    if (slot->generation == 0)
      slot->generation = 1;
    slot->sampler_resource_id = sampler.gpuResourceID._impl;
    slot->last_use_submit_value = renderer->submit_value;
    slot->live = true_v;
    *out_handle =
        (VkrTextureHandle){.id = i + 1, .generation = slot->generation};
    return true_v;
  }
  return false_v;
}

bool8_t
vkr_metal_packet_renderer_destroy_texture(VkrMetalPacketRenderer *renderer,
                                          VkrTextureHandle handle) {
  if (!renderer || handle.id == 0 || handle.id > renderer->max_textures) {
    log_warn("Metal texture destroy rejected invalid handle %u:%u", handle.id,
             handle.generation);
    return false_v;
  }
  VkrMetalPacketTexture *slot = &renderer->textures[handle.id - 1];
  if (!slot->live || slot->generation != handle.generation) {
    log_warn("Metal texture destroy rejected stale handle %u:%u "
             "(live=%u current_generation=%u)",
             handle.id, handle.generation, slot->live, slot->generation);
    return false_v;
  }
  for (uint32_t material_index = 0; material_index < renderer->max_materials;
       ++material_index) {
    const VkrMetalPacketMaterial *material =
        &renderer->packet_materials[material_index];
    if (!material->live || material->owns_textures)
      continue;
    for (uint32_t texture_index = 0;
         texture_index < ArrayCount(material->texture_handles);
         ++texture_index) {
      const VkrTextureHandle referenced =
          material->texture_handles[texture_index];
      if (referenced.id == handle.id &&
          referenced.generation == handle.generation) {
        log_warn("Metal texture %u:%u is still referenced by material %u:%u; "
                 "ownership retained for retry",
                 handle.id, handle.generation, material_index + 1u,
                 material->generation);
        return false_v;
      }
    }
  }
  if (vkr_metal_memory_device_retire(renderer->memory, slot->resource.handle,
                                     slot->last_use_submit_value) !=
      VKR_METAL_MEMORY_STATUS_OK) {
    log_warn("Metal texture %u:%u retirement failed at submit %llu", handle.id,
             handle.generation,
             (unsigned long long)slot->last_use_submit_value);
    return false_v;
  }
  if (renderer->ibl_ready && renderer->ibl_source.id == handle.id &&
      renderer->ibl_source.generation == handle.generation) {
    renderer->ibl_ready = false_v;
    renderer->ibl_source = VKR_TEXTURE_HANDLE_INVALID;
  }
  if (slot->ibl_sh_slot != VKR_SH_SLOT_BLACK &&
      vkr_ibl_sh_pool_retire(&renderer->sh_pool, slot->ibl_sh_slot) !=
          VKR_SH_POOL_STATUS_OK) {
    log_fatal("Metal lost the SH publication owned by texture %u:%u", handle.id,
              handle.generation);
  }
  const uint32_t next_generation =
      slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
  *slot = (VkrMetalPacketTexture){.generation = next_generation};
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_ibl_source(const VkrRenderPacket *packet,
                                                 VkrTextureHandle *out_source) {
  *out_source = VKR_TEXTURE_HANDLE_INVALID;
  if (packet->lighting && packet->lighting->ibl_source.id != 0) {
    *out_source = packet->lighting->ibl_source;
  } else if (packet->skybox && packet->skybox->cubemap.id != 0) {
    *out_source = packet->skybox->cubemap;
  }
  return out_source->id != 0;
}

vkr_internal bool8_t vkr_metal_packet_ibl_needs_bake(
    const VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet) {
  VkrTextureHandle source = VKR_TEXTURE_HANDLE_INVALID;
  if (!vkr_metal_packet_ibl_source(packet, &source))
    return false_v;
  return !renderer->ibl_ready || renderer->ibl_source.id != source.id ||
                 renderer->ibl_source.generation != source.generation
             ? true_v
             : false_v;
}

vkr_internal VkrMetalPacketTexture *
vkr_metal_packet_resolve_texture(VkrMetalPacketRenderer *renderer,
                                 VkrTextureHandle handle);

vkr_internal void vkr_metal_packet_pack_gpu_draw_view(
    VkrMetalPacketGpuDrawView *out_view, const VkrFrustum *frustum,
    uint32_t required_candidate_flags, bool8_t hzb_enabled) {
  *out_view = (VkrMetalPacketGpuDrawView){
      .required_candidate_flags = required_candidate_flags,
      .hzb_enabled = hzb_enabled ? 1u : 0u,
  };
  for (uint32_t i = 0u; i < VKR_FRUSTUM_PLANE_COUNT; ++i) {
    const VkrPlane *plane = &frustum->planes[i];
    out_view->frustum_planes[i] =
        (Vec4){plane->normal.x, plane->normal.y, plane->normal.z, plane->d};
  }
}

vkr_internal bool8_t vkr_metal_packet_candidate_graph_buffers(
    VkrMetalPacketRenderer *renderer,
    VkrMetalPacketGraphBufferInstance **out_candidates,
    VkrMetalPacketGraphBufferInstance **out_instances,
    uint64_t *out_resource_generation) {
  const VkrRgBufferHandle candidate_handle =
      renderer->gpu_candidate_buffer_handle;
  const VkrRgBufferHandle instance_handle =
      renderer->gpu_candidate_instance_buffer_handle;
  if (!vkr_rg_buffer_handle_valid(candidate_handle) ||
      !vkr_rg_buffer_handle_valid(instance_handle))
    return false_v;
  VkrMetalPacketGraphBuffer *candidate_graph =
      &renderer->graph_buffers[candidate_handle.id - 1u];
  VkrMetalPacketGraphBuffer *instance_graph =
      &renderer->graph_buffers[instance_handle.id - 1u];
  *out_candidates = vkr_metal_packet_graph_buffer(renderer, candidate_handle);
  *out_instances = vkr_metal_packet_graph_buffer(renderer, instance_handle);
  *out_resource_generation =
      ((uint64_t)candidate_graph->graph_generation << 32u) |
      instance_graph->graph_generation;
  return *out_candidates && *out_instances &&
         (*out_candidates)->resource.length >=
             (uint64_t)VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                 sizeof(VkrGpuCandidateDrawRow) &&
         (*out_instances)->resource.length >=
             (uint64_t)VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                 sizeof(VkrInstanceDataGPU) &&
         *out_resource_generation != 0u;
}

vkr_internal bool8_t vkr_metal_packet_pack_gpu_draw_stream(
    VkrMetalPacketRenderer *renderer, const VkrWorldDrawCandidate *sources,
    uint32_t count, uint32_t destination_first, VkrInstanceDataGPU *instances,
    VkrGpuCandidateDrawRow *candidates, VkrGpuGeometryRow *geometry_rows,
    uint32_t *out_packed_count, uint32_t *out_omitted_count) {
  *out_packed_count = 0u;
  *out_omitted_count = 0u;
  if (count == 0u)
    return true_v;
  if (!sources || destination_first > VKR_GPU_DRAW_CANDIDATE_CAPACITY ||
      count > VKR_GPU_DRAW_CANDIDATE_CAPACITY - destination_first)
    return false_v;
  const uint64_t pending_submit = renderer->submit_value + 1u;
  uint32_t packed_count = 0u;
  uint32_t unresolved_count = 0u;
  uint32_t last_temporal_index = UINT32_MAX;
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrWorldDrawCandidate *source = &sources[i];
    const VkrMeshHandle geometry = {
        .id = source->geometry.id,
        .generation = source->geometry.generation,
    };
    VkrMetalPacketMesh *mesh =
        vkr_metal_packet_resolve_mesh(renderer, geometry);
    VkrMetalPacketMaterial *material =
        vkr_metal_packet_resolve_material(renderer, source->material);
    if (!mesh || !material) {
      unresolved_count++;
      continue;
    }
    if (source->submesh_index >= mesh->submesh_count)
      return false_v;
    const VkrMetalPacketSubmeshCreateInfo *submesh =
        &renderer->submeshes[(uint64_t)(geometry.id - 1u) *
                                 renderer->max_submeshes_per_mesh +
                             source->submesh_index];
    instances[packed_count] = source->instance;
    instances[packed_count].temporal_flags =
        source->instance.temporal_index != last_temporal_index
            ? VKR_INSTANCE_TEMPORAL_OWNER
            : 0u;
    last_temporal_index = source->instance.temporal_index;
    geometry_rows[geometry.id - 1u] = mesh->gpu_row;
    candidates[packed_count] = (VkrGpuCandidateDrawRow){
        .geometry_index = geometry.id - 1u,
        .material_index = material->row_index,
        .instance_index = destination_first + packed_count,
        .first_index = mesh->gpu_row.first_index + submesh->first_index,
        .index_count = submesh->index_count,
        .vertex_offset = submesh->vertex_offset,
        .state_bucket = source->state_bucket,
        .flags = source->flags,
        .local_bounding_sphere = source->local_bounding_sphere,
    };
    packed_count++;
    mesh->last_use_submit_value = pending_submit;
    material->last_use_submit_value = pending_submit;
  }
  if (unresolved_count && !renderer->deferred_candidate_drop_logged) {
    log_warn("Metal deferred publication boundary omitted %u/%u candidates",
             unresolved_count, count);
    renderer->deferred_candidate_drop_logged = true_v;
  }
  *out_packed_count = packed_count;
  *out_omitted_count = unresolved_count;
  return true_v;
}

vkr_internal void
vkr_metal_packet_mark_gpu_draw_stream_use(VkrMetalPacketRenderer *renderer,
                                          const VkrWorldDrawCandidate *sources,
                                          uint32_t count) {
  const uint64_t pending_submit = renderer->submit_value + 1u;
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrWorldDrawCandidate *source = &sources[i];
    VkrMetalPacketMesh *mesh = vkr_metal_packet_resolve_mesh(
        renderer, (VkrMeshHandle){.id = source->geometry.id,
                                  .generation = source->geometry.generation});
    VkrMetalPacketMaterial *material =
        vkr_metal_packet_resolve_material(renderer, source->material);
    if (mesh)
      mesh->last_use_submit_value =
          MAX(mesh->last_use_submit_value, pending_submit);
    if (material)
      material->last_use_submit_value =
          MAX(material->last_use_submit_value, pending_submit);
  }
}

vkr_internal bool8_t vkr_metal_packet_prepare_upload(
    VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet,
    VkrMetalPacketFrameUpload *upload) {
  MemZero(upload, sizeof(*upload));
  const uint32_t world_count =
      packet->world ? packet->world->instance_count : 0;
  const uint32_t shadow_count = 0u;
  const uint32_t picking_count = 0u;
  const uint32_t editor_count =
      packet->editor ? packet->editor->instance_count : 0;
  const uint32_t ui_count = packet->ui ? packet->ui->instance_count : 0;
  const VkrWorldPassPayload *world = packet->world;
  const uint32_t source_gpu_draw_count =
      world ? world->gpu_candidate_count : 0u;
  const uint32_t source_static_count =
      world ? world->static_candidate_count : 0u;
  const uint32_t source_dynamic_count =
      source_gpu_draw_count - source_static_count;
  const uint32_t source_transmission_gpu_draw_count =
      world ? world->transmission_gpu_candidate_count : 0u;
  VkrMetalPacketGraphBufferInstance *gpu_draw_candidates_buffer = NULL;
  VkrMetalPacketGraphBufferInstance *gpu_draw_instances_buffer = NULL;
  uint64_t resource_generation = 0u;
  VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
  slot->candidate_residency_pending = false_v;
  const bool8_t graph_buffers_ready =
      !world || vkr_metal_packet_candidate_graph_buffers(
                    renderer, &gpu_draw_candidates_buffer,
                    &gpu_draw_instances_buffer, &resource_generation);
  if (!graph_buffers_ready)
    return false_v;
  VkrMetalPacketGraphBufferInstance *transmission_gpu_draw_instances_buffer =
      NULL;
  if (source_transmission_gpu_draw_count > 0u) {
    const VkrRgBufferHandle handle =
        renderer->transmission_gpu_candidate_instance_buffer_handle;
    transmission_gpu_draw_instances_buffer =
        vkr_metal_packet_graph_buffer(renderer, handle);
    if (!transmission_gpu_draw_instances_buffer)
      return false_v;
  }
  const bool8_t repack_static =
      world && vkr_candidate_residency_needs_static_repack(
                   &slot->candidate_residency, world->static_generation,
                   world->publication_generation, resource_generation);
  const uint32_t gpu_draw_stage_count =
      source_dynamic_count + (repack_static ? source_static_count : 0u);
  renderer->gpu_draw_count = 0u;
  renderer->transmission_gpu_draw_count = 0u;
  const uint32_t world_text_count =
      packet->world ? packet->world->text_draw_count : 0;
  const uint32_t ui_text_count = packet->ui ? packet->ui->text_draw_count : 0;
  if ((uint64_t)world_text_count + ui_text_count > renderer->max_draws)
    return false_v;
  const uint64_t retained_instance_count = (uint64_t)world_count +
                                           shadow_count + picking_count +
                                           editor_count + ui_count;
  if (retained_instance_count > renderer->max_instances ||
      source_gpu_draw_count > renderer->max_instances ||
      source_transmission_gpu_draw_count > renderer->max_instances)
    return false_v;
  uint64_t root_count = 1;
  if (vkr_metal_packet_ibl_needs_bake(renderer, packet))
    root_count += 1 + VKR_IBL_PREFILTER_MIP_COUNT;
  if (packet->world)
    root_count += packet->world->transparent_draw_count + world_text_count;
  {
    const uint32_t gpu_draw_view_count =
        1u + renderer->prepared_frame.shadow_cascade_count;
    const uint32_t gpu_draw_icb_group_count =
        (gpu_draw_view_count + renderer->gpu_draw_icb_view_group_size - 1u) /
        renderer->gpu_draw_icb_view_group_size;
    root_count += 11u + renderer->prepared_frame.hzb_reduce_pass_count +
                  gpu_draw_icb_group_count;
  }
  root_count += renderer->prepared_frame.sdsm_enabled ? 1u : 0u;
  if (source_transmission_gpu_draw_count > 0u)
    root_count +=
        17u +
        (renderer->prepared_frame.transmission_compact_enabled ? 4u : 0u) +
        (packet->debug && packet->debug->enable_timing &&
                 packet->debug->capture_pass_timestamps &&
                 !renderer->prepared_frame.transmission_compact_enabled
             ? 4u
             : 0u);
  root_count += renderer->prepared_frame.shadow_cascade_count;
  if (packet->picking && packet->picking->pending)
    root_count += 1u;
  if (packet->editor)
    root_count += packet->editor->draw_count;
  if (packet->ui)
    root_count += packet->ui->draw_count + ui_text_count;
  /*
   * Pass encoders allocate at most one ordinary root each. Keep that authored
   * graph contribution explicit instead of relying on the fixed estimates
   * above to be updated whenever a pass gains a root. Specialized multi-root
   * work (IBL, GPU draw setup, transmission, text) is counted separately.
   */
  if (renderer->graph->execution_order.length > UINT64_MAX - root_count)
    return false_v;
  root_count += renderer->graph->execution_order.length;
  if (root_count > renderer->max_draws)
    return false_v;
  uint64_t frame_root_count = 0;
  if (packet->world && packet->world->transparent_draw_count > 0) {
    // Blend encoding retains both its frame root and temporal draw state.
    frame_root_count += 2u;
  }
  frame_root_count += packet->editor && packet->editor->draw_count > 0;
  frame_root_count += packet->ui && packet->ui->draw_count > 0;
  if (frame_root_count > UINT32_MAX - root_count)
    return false_v;
  root_count += frame_root_count;

  const uint64_t world_bytes =
      (uint64_t)world_count * sizeof(VkrInstanceDataGPU);
  const uint64_t shadow_bytes =
      (uint64_t)shadow_count * sizeof(VkrInstanceDataGPU);
  const uint64_t picking_bytes =
      (uint64_t)picking_count * sizeof(VkrInstanceDataGPU);
  const uint64_t editor_bytes =
      (uint64_t)editor_count * sizeof(VkrInstanceDataGPU);
  const uint64_t ui_bytes = (uint64_t)ui_count * sizeof(VkrInstanceDataGPU);
  const uint64_t retained_instance_bytes =
      world_bytes + shadow_bytes + picking_bytes + editor_bytes + ui_bytes;
  const uint64_t gpu_draw_instances_offset = vkr_metal_packet_align_up(
      retained_instance_bytes, AlignOf(VkrInstanceDataGPU));
  const uint64_t gpu_draw_instances_bytes =
      (uint64_t)gpu_draw_stage_count * sizeof(VkrInstanceDataGPU);
  const uint64_t transmission_gpu_draw_instances_offset =
      vkr_metal_packet_align_up(gpu_draw_instances_offset +
                                    gpu_draw_instances_bytes,
                                AlignOf(VkrInstanceDataGPU));
  const uint64_t transmission_gpu_draw_instances_bytes =
      (uint64_t)source_transmission_gpu_draw_count * sizeof(VkrInstanceDataGPU);
  const uint64_t instance_bytes = transmission_gpu_draw_instances_offset +
                                  transmission_gpu_draw_instances_bytes;
  const uint32_t point_light_count =
      packet->lighting ? packet->lighting->point_light_count : 0;
  const uint32_t point_light_cell_count =
      point_light_count > 0 ? packet->lighting->point_light_grid->cell_count
                            : 0;
  const uint64_t point_light_offset =
      vkr_metal_packet_align_up(instance_bytes, AlignOf(Vec4));
  const uint64_t point_light_bytes =
      (uint64_t)point_light_count * 4u * sizeof(Vec4);
  const uint64_t point_light_masks_offset = vkr_metal_packet_align_up(
      point_light_offset + point_light_bytes, AlignOf(VkrPointLightMask));
  const uint64_t point_light_masks_bytes =
      (uint64_t)point_light_cell_count * sizeof(VkrPointLightMask);
  const uint32_t shadow_cascade_count =
      packet->shadow ? packet->shadow->cascade_count : 0;
  const uint64_t shadow_cascades_offset = vkr_metal_packet_align_up(
      point_light_masks_offset + point_light_masks_bytes,
      AlignOf(VkrMetalPacketShadowCascade));
  const uint64_t shadow_cascades_bytes =
      (uint64_t)shadow_cascade_count * sizeof(VkrMetalPacketShadowCascade);
  const uint32_t ibl_probe_count =
      packet->lighting ? packet->lighting->ibl_probe_count : 0;
  const uint64_t ibl_probes_offset =
      vkr_metal_packet_align_up(shadow_cascades_offset + shadow_cascades_bytes,
                                AlignOf(VkrMetalPacketIblProbe));
  const uint64_t ibl_probes_bytes =
      (uint64_t)ibl_probe_count * sizeof(VkrMetalPacketIblProbe);
  const uint32_t gpu_draw_view_count =
      1u + renderer->prepared_frame.shadow_cascade_count;
  const uint32_t gpu_draw_icb_group_count =
      gpu_draw_view_count > 0u ? (gpu_draw_view_count +
                                  renderer->gpu_draw_icb_view_group_size - 1u) /
                                     renderer->gpu_draw_icb_view_group_size
                               : 0u;
  const uint64_t gpu_draw_views_offset = vkr_metal_packet_align_up(
      ibl_probes_offset + ibl_probes_bytes, AlignOf(VkrMetalPacketGpuDrawView));
  const uint64_t gpu_draw_views_bytes =
      (uint64_t)gpu_draw_view_count * sizeof(VkrMetalPacketGpuDrawView);
  const uint64_t transmission_gpu_draw_view_offset =
      vkr_metal_packet_align_up(gpu_draw_views_offset + gpu_draw_views_bytes,
                                AlignOf(VkrMetalPacketGpuDrawView));
  const uint64_t transmission_gpu_draw_view_bytes =
      source_transmission_gpu_draw_count > 0u
          ? sizeof(VkrMetalPacketGpuDrawView)
          : 0u;
  const uint64_t gpu_draw_candidates_offset = vkr_metal_packet_align_up(
      transmission_gpu_draw_view_offset + transmission_gpu_draw_view_bytes,
      AlignOf(VkrGpuCandidateDrawRow));
  const uint64_t gpu_draw_candidate_bytes =
      (uint64_t)gpu_draw_stage_count * sizeof(VkrGpuCandidateDrawRow);
  const uint64_t transmission_gpu_draw_candidates_offset =
      vkr_metal_packet_align_up(gpu_draw_candidates_offset +
                                    gpu_draw_candidate_bytes,
                                AlignOf(VkrGpuCandidateDrawRow));
  const uint64_t transmission_gpu_draw_candidate_bytes =
      (uint64_t)source_transmission_gpu_draw_count *
      sizeof(VkrGpuCandidateDrawRow);
  const uint64_t gpu_draw_geometry_rows_offset =
      vkr_metal_packet_align_up(transmission_gpu_draw_candidates_offset +
                                    transmission_gpu_draw_candidate_bytes,
                                AlignOf(VkrGpuGeometryRow));
  const uint64_t gpu_draw_geometry_rows_bytes =
      source_gpu_draw_count > 0u || source_transmission_gpu_draw_count > 0u
          ? (uint64_t)renderer->max_meshes * sizeof(VkrGpuGeometryRow)
          : 0u;
  const uint64_t gpu_draw_state_zero_offset = vkr_metal_packet_align_up(
      gpu_draw_geometry_rows_offset + gpu_draw_geometry_rows_bytes,
      AlignOf(VkrGpuDrawCompactionState));
  const uint64_t gpu_draw_state_zero_bytes =
      (uint64_t)gpu_draw_view_count * sizeof(VkrGpuDrawCompactionState);
  const uint64_t gpu_draw_state_zero_end =
      gpu_draw_state_zero_offset + gpu_draw_state_zero_bytes;
  const uint64_t sdsm_state_reset_offset =
      renderer->prepared_frame.sdsm_enabled
          ? vkr_metal_packet_align_up(gpu_draw_state_zero_end,
                                      AlignOf(VkrMetalPacketSdsmState))
          : gpu_draw_state_zero_end;
  const uint64_t sdsm_state_reset_bytes = renderer->prepared_frame.sdsm_enabled
                                              ? sizeof(VkrMetalPacketSdsmState)
                                              : 0u;
  const uint64_t gpu_draw_icb_argument_alignment =
      source_gpu_draw_count > 0u
          ? renderer->gpu_draw_icb_argument_encoder.alignment
          : 1u;
  const uint64_t gpu_draw_icb_argument_offset = vkr_metal_packet_align_up(
      sdsm_state_reset_offset + sdsm_state_reset_bytes,
      gpu_draw_icb_argument_alignment);
  const uint64_t gpu_draw_icb_argument_stride = vkr_metal_packet_align_up(
      renderer->gpu_draw_icb_argument_encoder.encodedLength,
      gpu_draw_icb_argument_alignment);
  const uint64_t gpu_draw_icb_argument_bytes =
      source_gpu_draw_count > 0u
          ? gpu_draw_icb_argument_stride * gpu_draw_icb_group_count
          : 0u;
  const uint64_t transmission_gpu_draw_state_zero_offset =
      vkr_metal_packet_align_up(gpu_draw_icb_argument_offset +
                                    gpu_draw_icb_argument_bytes,
                                AlignOf(VkrMetalPacketTransmissionDiagnostics));
  const uint64_t transmission_gpu_draw_state_zero_bytes =
      source_transmission_gpu_draw_count > 0u
          ? sizeof(VkrMetalPacketTransmissionDiagnostics)
          : 0u;
  const uint64_t transmission_gpu_draw_icb_argument_alignment =
      source_transmission_gpu_draw_count > 0u
          ? renderer->gpu_draw_icb_argument_encoder.alignment
          : 1u;
  const uint64_t transmission_gpu_draw_icb_argument_offset =
      vkr_metal_packet_align_up(transmission_gpu_draw_state_zero_offset +
                                    transmission_gpu_draw_state_zero_bytes,
                                transmission_gpu_draw_icb_argument_alignment);
  const uint64_t transmission_gpu_draw_icb_argument_bytes =
      source_transmission_gpu_draw_count > 0u
          ? renderer->gpu_draw_icb_argument_encoder.encodedLength
          : 0u;
  uint64_t text_geometry_end = transmission_gpu_draw_icb_argument_offset +
                               transmission_gpu_draw_icb_argument_bytes;
  const VkrPreparedTextDraw *text_domains[] = {
      packet->world ? packet->world->text_draws : NULL,
      packet->ui ? packet->ui->text_draws : NULL,
  };
  const uint32_t text_domain_counts[] = {world_text_count, ui_text_count};
  for (uint32_t domain = 0; domain < ArrayCount(text_domains); ++domain) {
    for (uint32_t i = 0; i < text_domain_counts[domain]; ++i) {
      const VkrPreparedTextDraw *draw = &text_domains[domain][i];
      const uint64_t vertex_bytes =
          (uint64_t)draw->vertex_count * sizeof(VkrTextVertex);
      const uint64_t index_bytes =
          (uint64_t)draw->index_count * sizeof(uint32_t);
      text_geometry_end =
          vkr_metal_packet_align_up(text_geometry_end, AlignOf(VkrTextVertex));
      if (text_geometry_end > UINT64_MAX - vertex_bytes)
        return false_v;
      text_geometry_end += vertex_bytes;
      text_geometry_end =
          vkr_metal_packet_align_up(text_geometry_end, AlignOf(uint32_t));
      if (text_geometry_end > UINT64_MAX - index_bytes)
        return false_v;
      text_geometry_end += index_bytes;
    }
  }
  const uint64_t root_offset = vkr_metal_packet_align_up(
      text_geometry_end, VKR_METAL_PACKET_ROOT_ALIGNMENT);
  if (root_count >
      (UINT64_MAX - root_offset) / VKR_METAL_PACKET_DRAW_ROOT_STRIDE)
    return false_v;
  const uint64_t required =
      root_offset + root_count * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  void *buffer = NULL;
  if (vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_UPLOAD, required,
          renderer->completion.signaledValue, &upload->slice,
          &upload->addresses, &buffer) != VKR_METAL_MEMORY_STATUS_OK)
    return false_v;
  upload->buffer = (id<MTLBuffer>)buffer;
  upload->acquired = true_v;
  uint8_t *cpu = upload->addresses.cpu_address;
  uint64_t gpu = upload->addresses.gpu_address;
  MemZero(cpu + gpu_draw_state_zero_offset, gpu_draw_state_zero_bytes);
  upload->gpu_draw_state_zero_source_offset =
      upload->slice.offset + gpu_draw_state_zero_offset;
  if (sdsm_state_reset_bytes > 0u) {
    VkrMetalPacketSdsmState *sdsm_reset =
        (VkrMetalPacketSdsmState *)(cpu + sdsm_state_reset_offset);
    *sdsm_reset = (VkrMetalPacketSdsmState){
        .min_device_z_bits = UINT32_MAX,
    };
    upload->sdsm_state_reset_source_offset =
        upload->slice.offset + sdsm_state_reset_offset;
  }
  if (transmission_gpu_draw_state_zero_bytes > 0u) {
    MemZero(cpu + transmission_gpu_draw_state_zero_offset,
            transmission_gpu_draw_state_zero_bytes);
    upload->transmission_gpu_draw_state_zero_source_offset =
        upload->slice.offset + transmission_gpu_draw_state_zero_offset;
  }
  if (world_bytes > 0) {
    MemCopy(cpu, packet->world->instances, world_bytes);
    upload->world_instances_gpu = gpu;
  }
  if (picking_bytes > 0) {
  }
  if (editor_bytes > 0) {
    const uint64_t offset = world_bytes + shadow_bytes + picking_bytes;
    MemCopy(cpu + offset, packet->editor->instances, editor_bytes);
    upload->editor_instances_gpu = gpu + offset;
  }
  if (ui_bytes > 0) {
    const uint64_t offset =
        world_bytes + shadow_bytes + picking_bytes + editor_bytes;
    MemCopy(cpu + offset, packet->ui->instances, ui_bytes);
    upload->ui_instances_gpu = gpu + offset;
  }
  if (source_gpu_draw_count > 0u || source_transmission_gpu_draw_count > 0u) {
    VkrInstanceDataGPU *gpu_instances =
        (VkrInstanceDataGPU *)(cpu + gpu_draw_instances_offset);
    VkrGpuCandidateDrawRow *gpu_candidates =
        (VkrGpuCandidateDrawRow *)(cpu + gpu_draw_candidates_offset);
    VkrInstanceDataGPU *transmission_gpu_instances =
        (VkrInstanceDataGPU *)(cpu + transmission_gpu_draw_instances_offset);
    VkrGpuCandidateDrawRow *transmission_gpu_candidates =
        (VkrGpuCandidateDrawRow *)(cpu +
                                   transmission_gpu_draw_candidates_offset);
    VkrGpuGeometryRow *geometry_rows =
        (VkrGpuGeometryRow *)(cpu + gpu_draw_geometry_rows_offset);
#if VKR_METRICS_ENABLED
    const float64_t geometry_table_start = vkr_platform_get_absolute_time();
#endif
    MemZero(geometry_rows, gpu_draw_geometry_rows_bytes);
    for (uint32_t i = 0u; i < renderer->max_meshes; ++i) {
      const VkrMetalPacketMesh *mesh = &renderer->meshes[i];
      if (mesh->live)
        geometry_rows[i] = mesh->gpu_row;
    }
    renderer->packet_build.geometry_row_bytes = gpu_draw_geometry_rows_bytes;

#if VKR_METRICS_ENABLED
    const float64_t pack_start = vkr_platform_get_absolute_time();
#endif
    bool8_t packed = true_v;
    uint32_t static_rows_written = 0u;
    uint32_t dynamic_rows_written = 0u;
    uint32_t omitted_count = 0u;
    uint32_t packed_static_count =
        repack_static ? 0u : slot->candidate_residency.packed_static_count;
    uint32_t omitted_static_count =
        repack_static ? 0u : slot->candidate_residency.omitted_static_count;
    uint32_t stage_first = 0u;
    if (!repack_static)
      vkr_metal_packet_mark_gpu_draw_stream_use(renderer, world->gpu_candidates,
                                                source_static_count);
    if (repack_static) {
      packed = vkr_metal_packet_pack_gpu_draw_stream(
          renderer, world->gpu_candidates, source_static_count, 0u,
          gpu_instances, gpu_candidates, geometry_rows, &packed_static_count,
          &omitted_static_count);
      if (packed && packed_static_count > 0u) {
        upload->gpu_draw_candidate_copies
            [upload->gpu_draw_candidate_copy_count++] =
            (VkrMetalPacketCandidateCopyRange){
                .candidate_source_offset =
                    upload->slice.offset + gpu_draw_candidates_offset,
                .instance_source_offset =
                    upload->slice.offset + gpu_draw_instances_offset,
                .count = packed_static_count,
            };
        static_rows_written = packed_static_count;
      }
      slot->pending_candidate_residency = vkr_candidate_residency_stage(
          world->static_generation, world->publication_generation,
          resource_generation, packed_static_count, omitted_static_count);
      slot->candidate_residency_pending = packed;
      stage_first = source_static_count;
    }
    if (packed && source_dynamic_count > 0u) {
      uint32_t packed_dynamic_count = 0u;
      uint32_t omitted_dynamic_count = 0u;
      packed = vkr_metal_packet_pack_gpu_draw_stream(
          renderer, world->gpu_candidates + source_static_count,
          source_dynamic_count, packed_static_count,
          gpu_instances + stage_first, gpu_candidates + stage_first,
          geometry_rows, &packed_dynamic_count, &omitted_dynamic_count);
      if (packed && packed_dynamic_count > 0u) {
        upload->gpu_draw_candidate_copies
            [upload->gpu_draw_candidate_copy_count++] =
            (VkrMetalPacketCandidateCopyRange){
                .candidate_source_offset =
                    upload->slice.offset + gpu_draw_candidates_offset +
                    (uint64_t)stage_first * sizeof(VkrGpuCandidateDrawRow),
                .instance_source_offset =
                    upload->slice.offset + gpu_draw_instances_offset +
                    (uint64_t)stage_first * sizeof(VkrInstanceDataGPU),
                .destination_first = packed_static_count,
                .count = packed_dynamic_count,
            };
        dynamic_rows_written += packed_dynamic_count;
      }
      renderer->gpu_draw_count = packed_static_count + packed_dynamic_count;
      omitted_count = omitted_static_count + omitted_dynamic_count;
    } else {
      renderer->gpu_draw_count = packed_static_count;
      omitted_count = omitted_static_count;
    }
    if (packed && source_transmission_gpu_draw_count > 0u) {
      uint32_t omitted_transmission_count = 0u;
      packed = vkr_metal_packet_pack_gpu_draw_stream(
          renderer, world->transmission_gpu_candidates,
          source_transmission_gpu_draw_count, 0u, transmission_gpu_instances,
          transmission_gpu_candidates, geometry_rows,
          &renderer->transmission_gpu_draw_count, &omitted_transmission_count);
      dynamic_rows_written += renderer->transmission_gpu_draw_count;
      omitted_count += omitted_transmission_count;
    }
#if VKR_METRICS_ENABLED
    renderer->packet_build.candidate_pack_ns =
        vkr_metrics_elapsed_ns(pack_start);
    renderer->packet_build.geometry_table_build_ns =
        vkr_metrics_elapsed_ns(geometry_table_start);
#endif
    const uint64_t written_candidates =
        (uint64_t)static_rows_written + dynamic_rows_written;
    renderer->packet_build.candidate_row_bytes =
        written_candidates * sizeof(VkrGpuCandidateDrawRow);
    renderer->packet_build.instance_row_bytes =
        written_candidates * sizeof(VkrInstanceDataGPU);
    renderer->packet_build.static_candidate_row_bytes =
        (uint64_t)static_rows_written * sizeof(VkrGpuCandidateDrawRow);
    renderer->packet_build.dynamic_candidate_row_bytes =
        (uint64_t)dynamic_rows_written * sizeof(VkrGpuCandidateDrawRow);
    renderer->packet_build.publication_omitted_count = omitted_count;
    renderer->packet_build.valid = packed;
    if (!packed)
      return false_v;
    if (renderer->gpu_draw_count > 0u) {
      for (uint32_t group = 0u; group < gpu_draw_icb_group_count; ++group) {
        [renderer->gpu_draw_icb_argument_encoder
            setArgumentBuffer:upload->buffer
                       offset:upload->slice.offset +
                              gpu_draw_icb_argument_offset +
                              (uint64_t)group * gpu_draw_icb_argument_stride];
        [renderer->gpu_draw_icb_argument_encoder
            setIndirectCommandBuffer:slot->gpu_draw_icbs[group]
                             atIndex:0u];
      }
    }
    if (renderer->transmission_gpu_draw_count > 0u) {
      [renderer->gpu_draw_icb_argument_encoder
          setArgumentBuffer:upload->buffer
                     offset:upload->slice.offset +
                            transmission_gpu_draw_icb_argument_offset];
      [renderer->gpu_draw_icb_argument_encoder
          setIndirectCommandBuffer:slot->transmission_gpu_draw_icb
                           atIndex:0u];
    }
    upload->gpu_draw_instances_gpu =
        gpu_draw_instances_buffer
            ? gpu_draw_instances_buffer->resource.gpu_address
            : 0u;
    upload->transmission_gpu_draw_instances_gpu =
        transmission_gpu_draw_instances_buffer
            ? transmission_gpu_draw_instances_buffer->resource.gpu_address
            : 0u;
    upload->gpu_draw_geometry_rows_gpu = gpu + gpu_draw_geometry_rows_offset;
    upload->gpu_draw_icb_argument_gpu = gpu + gpu_draw_icb_argument_offset;
    upload->gpu_draw_icb_argument_stride = gpu_draw_icb_argument_stride;
    upload->transmission_gpu_draw_icb_argument_gpu =
        gpu + transmission_gpu_draw_icb_argument_offset;
    upload->transmission_gpu_draw_candidate_source_offset =
        upload->slice.offset + transmission_gpu_draw_candidates_offset;
    upload->transmission_gpu_draw_instance_source_offset =
        upload->slice.offset + transmission_gpu_draw_instances_offset;
    upload->transmission_gpu_draw_candidate_bytes =
        (uint64_t)renderer->transmission_gpu_draw_count *
        sizeof(VkrGpuCandidateDrawRow);
  }
  if (point_light_count > 0) {
    Vec4 *packed = (Vec4 *)(cpu + point_light_offset);
    for (uint32_t i = 0; i < point_light_count; ++i) {
      const VkrPointLight *light = &packet->lighting->point_lights[i];
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
    upload->point_light_data_gpu = gpu + point_light_offset;
    if (point_light_masks_bytes > 0) {
      MemCopy(cpu + point_light_masks_offset,
              packet->lighting->point_light_grid->masks,
              point_light_masks_bytes);
      upload->point_light_masks_gpu = gpu + point_light_masks_offset;
    }
  }
  if (shadow_cascade_count > 0) {
    VkrMetalPacketShadowCascade *cascades =
        (VkrMetalPacketShadowCascade *)(cpu + shadow_cascades_offset);
    for (uint32_t i = 0; i < shadow_cascade_count; ++i) {
      const VkrShadowCascadePacketData *source = &packet->shadow->cascades[i];
      cascades[i] = (VkrMetalPacketShadowCascade){
          .light_view_projection = source->light_view_projection,
          .split_near_far_texel_depth = source->split_near_far_texel_depth,
          .origin_inv_size_pad = source->origin_inv_size_pad,
      };
    }
    id<MTLTexture> shadow_texture =
        vkr_metal_packet_named_texture(renderer, "shadow_map");
    if (!shadow_texture)
      return false_v;
    upload->shadow_cascades_gpu = gpu + shadow_cascades_offset;
    upload->shadow_texture_id = shadow_texture.gpuResourceID._impl;
  }
  /* Resolved from the packet's environment source, so a probe that aliases the
     scene environment shares this publication instead of projecting a
     duplicate. */
  VkrTextureHandle sh_global_source = VKR_TEXTURE_HANDLE_INVALID;
  if (vkr_metal_packet_ibl_source(packet, &sh_global_source)) {
    const VkrMetalPacketTexture *sh_global =
        vkr_metal_packet_resolve_texture(renderer, sh_global_source);
    renderer->ibl_sh_slot =
        sh_global ? sh_global->ibl_sh_slot : VKR_SH_SLOT_BLACK;
  }
  if (ibl_probe_count > 0) {
    VkrMetalPacketIblProbe *packed =
        (VkrMetalPacketIblProbe *)(cpu + ibl_probes_offset);
    for (uint32_t i = 0; i < ibl_probe_count; ++i) {
      const VkrFrameIblProbe *probe = &packet->lighting->ibl_probes[i];
      VkrMetalPacketTexture *prefilter =
          vkr_metal_packet_resolve_texture(renderer, probe->prefilter);
      if (!prefilter)
        return false_v;
      packed[i] = (VkrMetalPacketIblProbe){
          .sh_slot = probe->sh_slot,
          .prefilter_texture_id = prefilter->resource.resource_id,
          .center_blend = {probe->center.x, probe->center.y, probe->center.z,
                           probe->blend_distance},
          .extents_weight = {probe->extents.x, probe->extents.y,
                             probe->extents.z, probe->weight},
          .intensity_box = {probe->intensity, probe->diffuse_intensity,
                            probe->specular_intensity,
                            probe->box_projection_enabled ? 1.0f : 0.0f},
      };
      const uint64_t pending_submit = renderer->submit_value + 1u;
      prefilter->last_use_submit_value = pending_submit;
    }
    upload->ibl_probes_gpu = gpu + ibl_probes_offset;
  }
  if (gpu_draw_view_count > 0u) {
    VkrMetalPacketGpuDrawView *views =
        (VkrMetalPacketGpuDrawView *)(cpu + gpu_draw_views_offset);
    const VkrFrustum camera = vkr_frustum_from_view_projection(
        packet->globals.view, packet->globals.projection);
    vkr_metal_packet_pack_gpu_draw_view(
        &views[0], &camera, VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE,
        renderer->prepared_frame.hzb_history_valid);
    for (uint32_t cascade = 0u;
         cascade < renderer->prepared_frame.shadow_cascade_count; ++cascade) {
      const VkrFrustum shadow = vkr_frustum_from_matrix(
          packet->shadow->cascades[cascade].light_view_projection);
      vkr_metal_packet_pack_gpu_draw_view(
          &views[cascade + 1u], &shadow, VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER,
          false_v);
    }
    upload->gpu_draw_views_gpu = gpu + gpu_draw_views_offset;
  }
  if (transmission_gpu_draw_view_bytes > 0u) {
    VkrMetalPacketGpuDrawView *view =
        (VkrMetalPacketGpuDrawView *)(cpu + transmission_gpu_draw_view_offset);
    const VkrFrustum camera = vkr_frustum_from_view_projection(
        packet->globals.view, packet->globals.projection);
    vkr_metal_packet_pack_gpu_draw_view(
        view, &camera, 0u, renderer->prepared_frame.hzb_history_valid);
    upload->transmission_gpu_draw_view_gpu =
        gpu + transmission_gpu_draw_view_offset;
  }
  uint64_t text_offset = transmission_gpu_draw_icb_argument_offset +
                         transmission_gpu_draw_icb_argument_bytes;
  uint32_t text_upload_index = 0;
  for (uint32_t domain = 0; domain < ArrayCount(text_domains); ++domain) {
    for (uint32_t i = 0; i < text_domain_counts[domain]; ++i) {
      const VkrPreparedTextDraw *draw = &text_domains[domain][i];
      VkrMetalPacketTextUpload *text_upload =
          &renderer->text_uploads[text_upload_index++];
      const uint64_t vertex_bytes =
          (uint64_t)draw->vertex_count * sizeof(VkrTextVertex);
      const uint64_t index_bytes =
          (uint64_t)draw->index_count * sizeof(uint32_t);
      text_offset =
          vkr_metal_packet_align_up(text_offset, AlignOf(VkrTextVertex));
      MemCopy(cpu + text_offset, draw->vertices, vertex_bytes);
      text_upload->vertices_gpu = gpu + text_offset;
      text_offset += vertex_bytes;
      text_offset = vkr_metal_packet_align_up(text_offset, AlignOf(uint32_t));
      MemCopy(cpu + text_offset, draw->indices, index_bytes);
      text_upload->indices_gpu = gpu + text_offset;
      text_upload->indices_length = index_bytes;
      text_offset += index_bytes;
    }
  }
  if (packet->world && packet->world->transmission_gpu_candidate_count > 0) {
    id<MTLTexture> transmission_texture = vkr_metal_packet_named_texture(
        renderer, packet->frame.editor_enabled ? "scene_pre_transmission"
                                               : "hdr_pre_transmission");
    if (!transmission_texture)
      return false_v;
    upload->transmission_texture_id = transmission_texture.gpuResourceID._impl;
  }
  upload->root_cpu = cpu + root_offset;
  upload->root_gpu = gpu + root_offset;
  upload->text_uploads = renderer->text_uploads;
  upload->world_text_count = world_text_count;
  upload->ui_text_count = ui_text_count;
  upload->root_capacity = (uint32_t)root_count;
  return true_v;
}

/** Rolls provisional candidate uses back when no queue submission follows. */
vkr_internal void vkr_metal_packet_rollback_gpu_draw_resource_preparation(
    VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet) {
  if (!packet->world)
    return;
  const uint64_t pending_submit = renderer->submit_value + 1u;
  const VkrWorldDrawCandidate *streams[] = {
      packet->world->gpu_candidates,
      packet->world->transmission_gpu_candidates,
  };
  const uint32_t counts[] = {
      packet->world->gpu_candidate_count,
      packet->world->transmission_gpu_candidate_count,
  };
  for (uint32_t stream = 0u; stream < ArrayCount(streams); ++stream) {
    for (uint32_t i = 0u; i < counts[stream]; ++i) {
      const VkrWorldDrawCandidate *candidate = &streams[stream][i];
      const VkrMeshHandle geometry = {
          .id = candidate->geometry.id,
          .generation = candidate->geometry.generation,
      };
      VkrMetalPacketMesh *mesh =
          vkr_metal_packet_resolve_mesh(renderer, geometry);
      VkrMetalPacketMaterial *material =
          vkr_metal_packet_resolve_material(renderer, candidate->material);
      if (mesh && mesh->last_use_submit_value == pending_submit)
        mesh->last_use_submit_value = renderer->submit_value;
      if (material && material->last_use_submit_value == pending_submit)
        material->last_use_submit_value = renderer->submit_value;
    }
  }
}

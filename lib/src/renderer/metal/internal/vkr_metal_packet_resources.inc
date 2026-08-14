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
    mesh->gpu_row.vertex_address = mega->vertices.gpu_address;
    mesh->gpu_row.index_address = mega->indices.gpu_address;
    mesh->gpu_row.publication_generation = mega->generation;
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
      (uint64_t)create_info->vertex_count > UINT64_MAX / sizeof(VkrVertex3d) ||
      (uint64_t)create_info->index_count > UINT64_MAX / sizeof(uint32_t))
    return false_v;
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
        (uint64_t)create_info->vertex_count * sizeof(VkrVertex3d);
    const uint64_t index_bytes =
        (uint64_t)create_info->index_count * sizeof(uint32_t);
    VkrMetalPacketGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
    const uint64_t vertex_offset =
        vkr_metal_packet_align_up(mega->vertex_cursor, 16u);
    const uint64_t index_offset =
        vkr_metal_packet_align_up(mega->index_cursor, sizeof(uint32_t));
    if (vertex_offset > UINT64_MAX - vertex_bytes ||
        index_offset > UINT64_MAX - index_bytes ||
        vertex_offset / sizeof(VkrVertex3d) > UINT32_MAX ||
        index_offset / sizeof(uint32_t) > UINT32_MAX ||
        !vkr_metal_packet_ensure_geometry_megabuffer(
            renderer, vertex_offset + vertex_bytes,
            index_offset + index_bytes) ||
        !vkr_metal_packet_upload_buffer_chunks_at(
            renderer, create_info->vertices, vertex_bytes,
            (id<MTLBuffer>)mega->vertices.metal_buffer, vertex_offset) ||
        !vkr_metal_packet_upload_buffer_chunks_at(
            renderer, create_info->indices, index_bytes,
            (id<MTLBuffer>)mega->indices.metal_buffer, index_offset)) {
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
        .first_vertex = (uint32_t)(vertex_offset / sizeof(VkrVertex3d)),
        .first_index = (uint32_t)(index_offset / sizeof(uint32_t)),
        .vertex_stride = sizeof(VkrVertex3d),
        .vertex_layout = VKR_GPU_VERTEX_LAYOUT_3D,
        .publication_generation = mega->generation,
        .flags = 1u,
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
    mega->vertex_cursor = vertex_offset + vertex_bytes;
    mega->index_cursor = index_offset + index_bytes;
    mega->vertex_live_bytes += vertex_bytes;
    mega->index_live_bytes += index_bytes;
    mega->vertex_high_water = Max(mega->vertex_high_water, mega->vertex_cursor);
    mega->index_high_water = Max(mega->index_high_water, mega->index_cursor);
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
      loader_result->mesh_buffer.vertex_size != sizeof(VkrVertex3d) ||
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
      loader_result->mesh_buffer.vertex_size != sizeof(VkrVertex3d) ||
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
  if (!geometry || !geometry->vertices || !geometry->indices ||
      geometry->vertex_count == 0 || geometry->index_count == 0 ||
      (geometry->vertex_size != sizeof(VkrVertex3d) &&
       geometry->vertex_size != sizeof(VkrVertex2d)) ||
      (geometry->index_size != sizeof(uint16_t) &&
       geometry->index_size != sizeof(uint32_t)))
    return false_v;
  /* Scratch widening buffers for 2D vertices and 16-bit indices. Owned by the
     renderer allocator rather than libc so a publication burst is visible in
     the renderer memory tag; both are released on every exit path below. */
  VkrVertex3d *converted_vertices = NULL;
  uint32_t *converted_indices = NULL;
  const uint64_t converted_vertices_bytes =
      (uint64_t)geometry->vertex_count * sizeof(*converted_vertices);
  const uint64_t converted_indices_bytes =
      (uint64_t)geometry->index_count * sizeof(*converted_indices);
  const VkrVertex3d *vertices = geometry->vertices;
  const uint32_t *indices = geometry->indices;
  if (geometry->vertex_size == sizeof(VkrVertex2d)) {
    converted_vertices = vkr_metal_packet_alloc_zeroed(
        renderer->allocator, converted_vertices_bytes);
    if (!converted_vertices)
      return false_v;
    const VkrVertex2d *source = geometry->vertices;
    for (uint32_t i = 0; i < geometry->vertex_count; ++i) {
      converted_vertices[i] = (VkrVertex3d){
          .position = {source[i].position.x, source[i].position.y, 0.0f},
          .normal = {0.0f, 0.0f, 1.0f},
          .texcoord = source[i].texcoord,
          .colour = {1.0f, 1.0f, 1.0f, 1.0f},
          .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
      };
    }
    vertices = converted_vertices;
  }
  if (geometry->index_size == sizeof(uint16_t)) {
    converted_indices = vkr_metal_packet_alloc_zeroed(renderer->allocator,
                                                      converted_indices_bytes);
    if (!converted_indices) {
      vkr_metal_packet_free_sized(renderer->allocator, converted_vertices,
                                  converted_vertices_bytes);
      return false_v;
    }
    const uint16_t *source = geometry->indices;
    for (uint32_t i = 0; i < geometry->index_count; ++i)
      converted_indices[i] = source[i];
    indices = converted_indices;
  }
  VkrMeshHandle published = VKR_MESH_HANDLE_INVALID;
  const VkrMeshHandle requested = {.id = handle.id,
                                   .generation = handle.generation};
  const bool8_t passed = vkr_metal_packet_renderer_create_mesh_internal(
                             renderer,
                             &(VkrMetalPacketMeshCreateInfo){
                                 .vertices = vertices,
                                 .vertex_count = geometry->vertex_count,
                                 .indices = indices,
                                 .index_count = geometry->index_count,
                             },
                             requested, &published) &&
                         published.id == requested.id &&
                         published.generation == requested.generation;
  vkr_metal_packet_free_sized(renderer->allocator, converted_indices,
                              converted_indices_bytes);
  vkr_metal_packet_free_sized(renderer->allocator, converted_vertices,
                              converted_vertices_bytes);
  return passed;
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
      (uint64_t)slot->vertex_count * sizeof(VkrVertex3d);
  const uint64_t index_bytes = (uint64_t)slot->index_count * sizeof(uint32_t);
  mega->vertex_live_bytes -= Min(mega->vertex_live_bytes, vertex_bytes);
  mega->index_live_bytes -= Min(mega->index_live_bytes, index_bytes);
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
    VkrMetalMaterialGpuRow row = {
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
        .material_emissive = material_constants.emissive,
        .material_dielectric_specular = material_constants.dielectric_specular,
        .material_surface = material_constants.surface,
        .material_alpha = material_constants.alpha,
        .material_attenuation_color = material_constants.attenuation_color,
    };
    MemCopy(row.tint, create_info->tint, sizeof(row.tint));
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
  VkrTextureSlot source_slots[4] = {
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
  };
  VkrMetalPacketTexture *textures[4] = {0};
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
  VkrMetalMaterialGpuRow row = {
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
      .material_emissive = material_constants.emissive,
      .material_dielectric_specular = material_constants.dielectric_specular,
      .material_surface = material_constants.surface,
      .material_alpha = material_constants.alpha,
      .material_attenuation_color = material_constants.attenuation_color,
  };
  const float32_t tint_values[4] = {tint.x, tint.y, tint.z, tint.w};
  MemCopy(row.tint, tint_values, sizeof(row.tint));
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
      prepared->upload_data_size == 0 || prepared->description.width == 0 ||
      prepared->description.height == 0 ||
      prepared->description.width > VKR_TEXTURE_MAX_DIMENSION ||
      prepared->description.height > VKR_TEXTURE_MAX_DIMENSION ||
      prepared->description.width != prepared->description.height &&
          prepared->description.type == VKR_TEXTURE_TYPE_CUBE_MAP)
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
  if (!cubemap &&
      (prepared->description.type != VKR_TEXTURE_TYPE_2D ||
       !prepared->upload_regions || prepared->upload_region_count == 0 ||
       prepared->upload_mip_levels == 0 ||
       prepared->upload_mip_levels > VKR_METAL_PACKET_MAX_TEXTURE_MIPS ||
       prepared->upload_array_layers != 1)) {
    [descriptor release];
    return false_v;
  }
  const uint64_t cube_bytes = (uint64_t)prepared->description.width *
                              prepared->description.height * 4u * 6u;
  if (cubemap &&
      (prepared->description.channels != 4 ||
       prepared->upload_mip_levels != 1 || prepared->upload_array_layers != 6 ||
       prepared->upload_data_size != cube_bytes)) {
    [descriptor release];
    return false_v;
  }
  descriptor.textureType = cubemap ? MTLTextureTypeCube : MTLTextureType2D;
  descriptor.pixelFormat = pixel_format;
  descriptor.width = prepared->description.width;
  descriptor.height = prepared->description.height;
  descriptor.mipmapLevelCount = cubemap ? 1 : prepared->upload_mip_levels;
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
  const bool8_t uploaded =
      created &&
      (cubemap
           ? vkr_metal_packet_upload_cubemap(
                 renderer, prepared->upload_data, prepared->description.width,
                 (id<MTLTexture>)slot->resource.metal_texture)
           : vkr_metal_packet_upload_texture(
                 renderer, &payload, prepared->description.format,
                 (id<MTLTexture>)slot->resource.metal_texture));
  id<MTLSamplerState> sampler =
      uploaded
          ? vkr_metal_packet_sampler(renderer, &prepared->description,
                                     cubemap ? 1u : prepared->upload_mip_levels)
          : nil;
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
  slot->last_use_submit_value = renderer->submit_value;
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
    const VkrMetalPacketTexture *source,
    const VkrMetalPacketTexture *irradiance,
    const VkrMetalPacketTexture *prefilter) API_AVAILABLE(macos(26.0)) {
  if (!source || !irradiance || !prefilter || source == irradiance ||
      source == prefilter || irradiance == prefilter) {
    return false_v;
  }

  id<MTLTexture> source_texture =
      (id<MTLTexture>)source->resource.metal_texture;
  id<MTLTexture> irradiance_texture =
      (id<MTLTexture>)irradiance->resource.metal_texture;
  id<MTLTexture> prefilter_texture =
      (id<MTLTexture>)prefilter->resource.metal_texture;
  return source_texture && irradiance_texture && prefilter_texture &&
         source_texture.textureType == MTLTextureTypeCube &&
         irradiance_texture.textureType == MTLTextureTypeCube &&
         prefilter_texture.textureType == MTLTextureTypeCube &&
         source_texture.width == source_texture.height &&
         irradiance_texture.width == irradiance_texture.height &&
         prefilter_texture.width == prefilter_texture.height &&
         irradiance_texture.pixelFormat == MTLPixelFormatRGBA16Float &&
         prefilter_texture.pixelFormat == MTLPixelFormatRGBA16Float &&
         (source_texture.usage & MTLTextureUsageShaderRead) != 0 &&
         (irradiance_texture.usage & MTLTextureUsageShaderWrite) != 0 &&
         (prefilter_texture.usage & MTLTextureUsageShaderWrite) != 0 &&
         source_texture.mipmapLevelCount > 0 &&
         source_texture.mipmapLevelCount <= VKR_METAL_PACKET_MAX_TEXTURE_MIPS &&
         irradiance_texture.mipmapLevelCount == 1 &&
         prefilter_texture.mipmapLevelCount > 0 &&
         prefilter_texture.mipmapLevelCount <=
             VKR_METAL_PACKET_MAX_TEXTURE_MIPS;
}

bool8_t vkr_metal_packet_renderer_bake_ibl_cubemap(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle source_handle,
    VkrTextureHandle irradiance_handle, VkrTextureHandle prefilter_handle) {
  if (!renderer || renderer->frame_prepared) {
    return false_v;
  }
  if (!@available(macOS 26.0, *)) {
    return false_v;
  }
  VkrMetalPacketTexture *source =
      vkr_metal_packet_resolve_texture(renderer, source_handle);
  VkrMetalPacketTexture *irradiance =
      vkr_metal_packet_resolve_texture(renderer, irradiance_handle);
  VkrMetalPacketTexture *prefilter =
      vkr_metal_packet_resolve_texture(renderer, prefilter_handle);
  if (!vkr_metal_packet_validate_ibl_textures(source, irradiance, prefilter))
    return false_v;

  id<MTLTexture> source_texture =
      (id<MTLTexture>)source->resource.metal_texture;
  id<MTLTexture> irradiance_texture =
      (id<MTLTexture>)irradiance->resource.metal_texture;
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

  VkrMetalPacketIrradianceRoot *irradiance_root = upload.cpu_address;
  *irradiance_root = (VkrMetalPacketIrradianceRoot){
      .source_texture_id = source->resource.resource_id,
      .target_texture_id = irradiance->resource.resource_id,
      .sample_count = 128u,
      .target_size = (uint32_t)irradiance_texture.width,
  };
  [renderer->argument_table setAddress:upload.gpu_address atIndex:0];
  [encoder setComputePipelineState:renderer->ibl_irradiance_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(irradiance_texture.width,
                                       irradiance_texture.height, 6)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];

  for (uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
    const uint64_t root_offset =
        (uint64_t)(mip + 1u) * VKR_METAL_PACKET_ROOT_ALIGNMENT;
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

  if (!vkr_metal_packet_submit_upload(renderer, encoder, slice))
    return false_v;
  source->last_use_submit_value = renderer->submit_value;
  irradiance->last_use_submit_value = renderer->submit_value;
  prefilter->last_use_submit_value = renderer->submit_value;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_bake_hdr_environment(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle equirect_handle,
    VkrTextureHandle source_handle, VkrTextureHandle irradiance_handle,
    VkrTextureHandle prefilter_handle) {
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
  VkrMetalPacketTexture *irradiance =
      vkr_metal_packet_resolve_texture(renderer, irradiance_handle);
  VkrMetalPacketTexture *prefilter =
      vkr_metal_packet_resolve_texture(renderer, prefilter_handle);
  if (!equirect || equirect == source || equirect == irradiance ||
      equirect == prefilter ||
      !vkr_metal_packet_validate_ibl_textures(source, irradiance, prefilter)) {
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
      renderer, source_handle, irradiance_handle, prefilter_handle);
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

vkr_internal bool8_t vkr_metal_packet_pack_gpu_draw_stream(
    VkrMetalPacketRenderer *renderer, const VkrWorldDrawCandidate *sources,
    uint32_t count, VkrInstanceDataGPU *instances,
    VkrGpuCandidateDrawRow *candidates, VkrGpuGeometryRow *geometry_rows) {
  if (count == 0u)
    return true_v;
  const uint64_t pending_submit = renderer->submit_value + 1u;
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
    if (!mesh || !material || source->submesh_index >= mesh->submesh_count)
      return false_v;
    const VkrMetalPacketSubmeshCreateInfo *submesh =
        &renderer->submeshes[(uint64_t)(geometry.id - 1u) *
                                 renderer->max_submeshes_per_mesh +
                             source->submesh_index];
    instances[i] = source->instance;
    geometry_rows[geometry.id - 1u] = mesh->gpu_row;
    candidates[i] = (VkrGpuCandidateDrawRow){
        .geometry_index = geometry.id - 1u,
        .material_index = material->row_index,
        .instance_index = i,
        .first_index = mesh->gpu_row.first_index + submesh->first_index,
        .index_count = submesh->index_count,
        .vertex_offset = submesh->vertex_offset,
        .state_bucket = source->state_bucket,
        .flags = source->flags,
        .local_bounding_sphere = source->local_bounding_sphere,
    };
    mesh->last_use_submit_value = pending_submit;
    material->last_use_submit_value = pending_submit;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_upload(
    VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet,
    VkrMetalPacketFrameUpload *upload) {
  MemZero(upload, sizeof(*upload));
  const uint32_t world_count =
      packet->world ? packet->world->instance_count : 0;
  const uint32_t shadow_count =
      packet->shadow && !renderer->prepared_frame.deferred_enabled
          ? packet->shadow->instance_count
          : 0;
  const bool8_t deferred_picking = renderer->prepared_frame.deferred_enabled &&
                                   packet->picking && packet->picking->pending;
  const uint32_t picking_count = packet->picking && !deferred_picking
                                     ? packet->picking->instance_count
                                     : 0;
  const uint32_t editor_count =
      packet->editor ? packet->editor->instance_count : 0;
  const uint32_t ui_count = packet->ui ? packet->ui->instance_count : 0;
  const uint32_t gpu_draw_count =
      renderer->prepared_frame.deferred_enabled && packet->world
          ? packet->world->gpu_candidate_count
          : 0u;
  const uint32_t transmission_gpu_draw_count =
      renderer->prepared_frame.deferred_enabled && packet->world
          ? packet->world->transmission_gpu_candidate_count
          : 0u;
  const uint32_t world_text_count =
      packet->world ? packet->world->text_draw_count : 0;
  const uint32_t ui_text_count = packet->ui ? packet->ui->text_draw_count : 0;
  if ((uint64_t)world_text_count + ui_text_count > renderer->max_draws)
    return false_v;
  const uint64_t retained_instance_count = (uint64_t)world_count +
                                           shadow_count + picking_count +
                                           editor_count + ui_count;
  if (retained_instance_count > renderer->max_instances ||
      gpu_draw_count > renderer->max_instances ||
      transmission_gpu_draw_count > renderer->max_instances)
    return false_v;
  uint64_t root_count = 1;
  if (packet->skybox)
    root_count += 1;
  if (vkr_metal_packet_ibl_needs_bake(renderer, packet))
    root_count += 2 + VKR_IBL_PREFILTER_MIP_COUNT;
  if (packet->world)
    root_count += (renderer->prepared_frame.deferred_enabled
                       ? 0u
                       : (uint64_t)packet->world->opaque_draw_count) +
                  packet->world->transmission_draw_count +
                  packet->world->transparent_draw_count + world_text_count;
  if (renderer->prepared_frame.deferred_enabled) {
    const uint32_t gpu_draw_view_count =
        1u + renderer->prepared_frame.shadow_cascade_count;
    const uint32_t gpu_draw_icb_group_count =
        (gpu_draw_view_count + renderer->gpu_draw_icb_view_group_size - 1u) /
        renderer->gpu_draw_icb_view_group_size;
    root_count += 9u + renderer->prepared_frame.hzb_reduce_pass_count +
                  gpu_draw_icb_group_count - 1u;
  }
  if (transmission_gpu_draw_count > 0u)
    root_count +=
        17u +
        (renderer->prepared_frame.transmission_compact_enabled ? 8u : 0u) +
        (packet->debug && packet->debug->enable_timing &&
                 packet->debug->capture_pass_timestamps &&
                 !renderer->prepared_frame.transmission_compact_enabled
             ? 4u
             : 0u);
  if (packet->shadow && !renderer->prepared_frame.deferred_enabled) {
    root_count += ((uint64_t)packet->shadow->opaque_draw_count +
                   (uint64_t)packet->shadow->alpha_draw_count) *
                  packet->shadow->cascade_count;
  }
  if (renderer->prepared_frame.deferred_enabled)
    root_count += renderer->prepared_frame.shadow_cascade_count;
  if (packet->picking && packet->picking->pending) {
    root_count +=
        deferred_picking
            ? 1u +
                  (packet->world ? packet->world->transparent_draw_count : 0u) +
                  world_text_count + ui_text_count
            : packet->picking->draw_count + world_text_count + ui_text_count;
  }
  if (packet->editor)
    root_count += packet->editor->draw_count;
  if (packet->ui)
    root_count += packet->ui->draw_count + ui_text_count;
  if (root_count > renderer->max_draws)
    return false_v;
  uint64_t frame_root_count = 0;
  if (packet->world) {
    frame_root_count += !renderer->prepared_frame.deferred_enabled &&
                        packet->world->opaque_draw_count > 0;
    frame_root_count += packet->world->transmission_draw_count > 0;
    frame_root_count += packet->world->transparent_draw_count > 0;
  }
  if (packet->shadow && !renderer->prepared_frame.deferred_enabled) {
    frame_root_count += (uint64_t)(packet->shadow->opaque_draw_count > 0) *
                        packet->shadow->cascade_count;
    frame_root_count += (uint64_t)(packet->shadow->alpha_draw_count > 0) *
                        packet->shadow->cascade_count;
  }
  frame_root_count +=
      packet->picking && packet->picking->pending &&
      (deferred_picking
           ? packet->world && packet->world->transparent_draw_count > 0u
           : packet->picking->draw_count > 0u);
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
      (uint64_t)gpu_draw_count * sizeof(VkrInstanceDataGPU);
  const uint64_t transmission_gpu_draw_instances_offset =
      vkr_metal_packet_align_up(gpu_draw_instances_offset +
                                    gpu_draw_instances_bytes,
                                AlignOf(VkrInstanceDataGPU));
  const uint64_t transmission_gpu_draw_instances_bytes =
      (uint64_t)transmission_gpu_draw_count * sizeof(VkrInstanceDataGPU);
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
      renderer->prepared_frame.deferred_enabled
          ? 1u + renderer->prepared_frame.shadow_cascade_count
          : 0u;
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
      transmission_gpu_draw_count > 0u ? sizeof(VkrMetalPacketGpuDrawView) : 0u;
  const uint64_t gpu_draw_candidates_offset = vkr_metal_packet_align_up(
      transmission_gpu_draw_view_offset + transmission_gpu_draw_view_bytes,
      AlignOf(VkrGpuCandidateDrawRow));
  const uint64_t gpu_draw_candidate_bytes =
      (uint64_t)gpu_draw_count * sizeof(VkrGpuCandidateDrawRow);
  const uint64_t transmission_gpu_draw_candidates_offset =
      vkr_metal_packet_align_up(gpu_draw_candidates_offset +
                                    gpu_draw_candidate_bytes,
                                AlignOf(VkrGpuCandidateDrawRow));
  const uint64_t transmission_gpu_draw_candidate_bytes =
      (uint64_t)transmission_gpu_draw_count * sizeof(VkrGpuCandidateDrawRow);
  const uint64_t gpu_draw_geometry_rows_offset =
      vkr_metal_packet_align_up(transmission_gpu_draw_candidates_offset +
                                    transmission_gpu_draw_candidate_bytes,
                                AlignOf(VkrGpuGeometryRow));
  const uint64_t gpu_draw_geometry_rows_bytes =
      gpu_draw_count > 0u || transmission_gpu_draw_count > 0u
          ? (uint64_t)renderer->max_meshes * sizeof(VkrGpuGeometryRow)
          : 0u;
  const uint64_t gpu_draw_state_zero_offset = vkr_metal_packet_align_up(
      gpu_draw_geometry_rows_offset + gpu_draw_geometry_rows_bytes,
      AlignOf(VkrGpuDrawCompactionState));
  const uint64_t gpu_draw_state_zero_bytes =
      renderer->prepared_frame.deferred_enabled
          ? (uint64_t)gpu_draw_view_count * sizeof(VkrGpuDrawCompactionState)
          : 0u;
  const uint64_t gpu_draw_icb_argument_alignment =
      gpu_draw_count > 0u ? renderer->gpu_draw_icb_argument_encoder.alignment
                          : 1u;
  const uint64_t gpu_draw_icb_argument_offset = vkr_metal_packet_align_up(
      gpu_draw_state_zero_offset + gpu_draw_state_zero_bytes,
      gpu_draw_icb_argument_alignment);
  const uint64_t gpu_draw_icb_argument_stride = vkr_metal_packet_align_up(
      renderer->gpu_draw_icb_argument_encoder.encodedLength,
      gpu_draw_icb_argument_alignment);
  const uint64_t gpu_draw_icb_argument_bytes =
      gpu_draw_count > 0u
          ? gpu_draw_icb_argument_stride * gpu_draw_icb_group_count
          : 0u;
  const uint64_t transmission_gpu_draw_state_zero_offset =
      vkr_metal_packet_align_up(gpu_draw_icb_argument_offset +
                                    gpu_draw_icb_argument_bytes,
                                AlignOf(VkrMetalPacketTransmissionDiagnostics));
  const uint64_t transmission_gpu_draw_state_zero_bytes =
      transmission_gpu_draw_count > 0u
          ? sizeof(VkrMetalPacketTransmissionDiagnostics)
          : 0u;
  const uint64_t transmission_gpu_draw_icb_argument_alignment =
      transmission_gpu_draw_count > 0u
          ? renderer->gpu_draw_icb_argument_encoder.alignment
          : 1u;
  const uint64_t transmission_gpu_draw_icb_argument_offset =
      vkr_metal_packet_align_up(transmission_gpu_draw_state_zero_offset +
                                    transmission_gpu_draw_state_zero_bytes,
                                transmission_gpu_draw_icb_argument_alignment);
  const uint64_t transmission_gpu_draw_icb_argument_bytes =
      transmission_gpu_draw_count > 0u
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
  if (world_bytes > 0) {
    MemCopy(cpu, packet->world->instances, world_bytes);
    upload->world_instances_gpu = gpu;
  }
  if (shadow_bytes > 0) {
    MemCopy(cpu + world_bytes, packet->shadow->instances, shadow_bytes);
    upload->shadow_instances_gpu = gpu + world_bytes;
  }
  if (picking_bytes > 0) {
    MemCopy(cpu + world_bytes + shadow_bytes, packet->picking->instances,
            picking_bytes);
    upload->picking_instances_gpu = gpu + world_bytes + shadow_bytes;
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
  if (gpu_draw_instances_bytes > 0u ||
      transmission_gpu_draw_instances_bytes > 0u) {
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
    MemZero(geometry_rows, gpu_draw_geometry_rows_bytes);
    if (!vkr_metal_packet_pack_gpu_draw_stream(
            renderer, packet->world->gpu_candidates, gpu_draw_count,
            gpu_instances, gpu_candidates, geometry_rows) ||
        !vkr_metal_packet_pack_gpu_draw_stream(
            renderer, packet->world->transmission_gpu_candidates,
            transmission_gpu_draw_count, transmission_gpu_instances,
            transmission_gpu_candidates, geometry_rows))
      return false_v;
    MemZero(cpu + gpu_draw_state_zero_offset, gpu_draw_state_zero_bytes);
    MemZero(cpu + transmission_gpu_draw_state_zero_offset,
            transmission_gpu_draw_state_zero_bytes);
    VkrMetalPacketCommandSlot *slot = renderer->active_command_slot;
    if (gpu_draw_count > 0u) {
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
    if (transmission_gpu_draw_count > 0u) {
      [renderer->gpu_draw_icb_argument_encoder
          setArgumentBuffer:upload->buffer
                     offset:upload->slice.offset +
                            transmission_gpu_draw_icb_argument_offset];
      [renderer->gpu_draw_icb_argument_encoder
          setIndirectCommandBuffer:slot->transmission_gpu_draw_icb
                           atIndex:0u];
    }
    upload->gpu_draw_instances_gpu = gpu + gpu_draw_instances_offset;
    upload->transmission_gpu_draw_instances_gpu =
        gpu + transmission_gpu_draw_instances_offset;
    upload->gpu_draw_geometry_rows_gpu = gpu + gpu_draw_geometry_rows_offset;
    upload->gpu_draw_icb_argument_gpu = gpu + gpu_draw_icb_argument_offset;
    upload->gpu_draw_icb_argument_stride = gpu_draw_icb_argument_stride;
    upload->gpu_draw_candidate_source_offset =
        upload->slice.offset + gpu_draw_candidates_offset;
    upload->gpu_draw_state_zero_source_offset =
        upload->slice.offset + gpu_draw_state_zero_offset;
    upload->gpu_draw_candidate_bytes = gpu_draw_candidate_bytes;
    upload->transmission_gpu_draw_icb_argument_gpu =
        gpu + transmission_gpu_draw_icb_argument_offset;
    upload->transmission_gpu_draw_candidate_source_offset =
        upload->slice.offset + transmission_gpu_draw_candidates_offset;
    upload->transmission_gpu_draw_state_zero_source_offset =
        upload->slice.offset + transmission_gpu_draw_state_zero_offset;
    upload->transmission_gpu_draw_candidate_bytes =
        transmission_gpu_draw_candidate_bytes;
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
      cascades[i] = (VkrMetalPacketShadowCascade){
          .light_view_projection = packet->shadow->light_view_proj[i],
          .split_depth = {packet->shadow->split_depths[i], 0.0f, 0.0f, 0.0f},
      };
    }
    id<MTLTexture> shadow_texture =
        vkr_metal_packet_named_texture(renderer, "shadow_map");
    if (!shadow_texture)
      return false_v;
    upload->shadow_cascades_gpu = gpu + shadow_cascades_offset;
    upload->shadow_texture_id = shadow_texture.gpuResourceID._impl;
  }
  if (ibl_probe_count > 0) {
    VkrMetalPacketIblProbe *packed =
        (VkrMetalPacketIblProbe *)(cpu + ibl_probes_offset);
    for (uint32_t i = 0; i < ibl_probe_count; ++i) {
      const VkrFrameIblProbe *probe = &packet->lighting->ibl_probes[i];
      VkrMetalPacketTexture *irradiance =
          vkr_metal_packet_resolve_texture(renderer, probe->irradiance);
      VkrMetalPacketTexture *prefilter =
          vkr_metal_packet_resolve_texture(renderer, probe->prefilter);
      if (!irradiance || !prefilter)
        return false_v;
      packed[i] = (VkrMetalPacketIblProbe){
          .irradiance_texture_id = irradiance->resource.resource_id,
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
      irradiance->last_use_submit_value = pending_submit;
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
      const VkrFrustum shadow =
          vkr_frustum_from_matrix(packet->shadow->light_view_proj[cascade]);
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
  if (packet->world && packet->world->transmission_draw_count > 0) {
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
  if (!renderer->prepared_frame.deferred_enabled)
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

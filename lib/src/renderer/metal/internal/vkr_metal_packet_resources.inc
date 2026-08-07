/* Immutable mesh/material/texture publication, lookup, and retirement. */
static bool8_t vkr_metal_packet_renderer_create_mesh_internal(
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
    slot->vertices = (VkrMetalBufferResource){0};
    slot->indices = (VkrMetalBufferResource){0};
    const uint64_t vertex_bytes =
        (uint64_t)create_info->vertex_count * sizeof(VkrVertex3d);
    const uint64_t index_bytes =
        (uint64_t)create_info->index_count * sizeof(uint32_t);
    if (vkr_metal_memory_device_create_buffer(renderer->memory, vertex_bytes,
                                              &slot->vertices) !=
            VKR_METAL_MEMORY_STATUS_OK ||
        vkr_metal_memory_device_create_buffer(renderer->memory, index_bytes,
                                              &slot->indices) !=
            VKR_METAL_MEMORY_STATUS_OK ||
        !vkr_metal_packet_upload_buffer_chunks(
            renderer, create_info->vertices, vertex_bytes,
            (id<MTLBuffer>)slot->vertices.metal_buffer) ||
        !vkr_metal_packet_upload_buffer_chunks(
            renderer, create_info->indices, index_bytes,
            (id<MTLBuffer>)slot->indices.metal_buffer)) {
      if (slot->vertices.metal_buffer)
        (void)vkr_metal_memory_device_retire(
            renderer->memory, slot->vertices.handle, renderer->submit_value);
      if (slot->indices.metal_buffer)
        (void)vkr_metal_memory_device_retire(
            renderer->memory, slot->indices.handle, renderer->submit_value);
      (void)vkr_metal_memory_device_collect(renderer->memory,
                                            renderer->submit_value, NULL);
      *slot = (VkrMetalPacketMesh){.generation = generation};
      return false_v;
    }
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
  VkrVertex3d *converted_vertices = NULL;
  uint32_t *converted_indices = NULL;
  const VkrVertex3d *vertices = geometry->vertices;
  const uint32_t *indices = geometry->indices;
  if (geometry->vertex_size == sizeof(VkrVertex2d)) {
    converted_vertices = calloc(geometry->vertex_count, sizeof(*vertices));
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
    converted_indices = calloc(geometry->index_count, sizeof(*indices));
    if (!converted_indices) {
      free(converted_vertices);
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
  free(converted_indices);
  free(converted_vertices);
  return passed;
}

bool8_t vkr_metal_packet_renderer_destroy_mesh(VkrMetalPacketRenderer *renderer,
                                               VkrMeshHandle handle) {
  if (!renderer || handle.id == 0 || handle.id > renderer->max_meshes)
    return false_v;
  VkrMetalPacketMesh *slot = &renderer->meshes[handle.id - 1];
  if (!slot->live || slot->generation != handle.generation)
    return false_v;
  if (vkr_metal_memory_device_retire(renderer->memory, slot->vertices.handle,
                                     slot->last_use_submit_value) !=
          VKR_METAL_MEMORY_STATUS_OK ||
      vkr_metal_memory_device_retire(renderer->memory, slot->indices.handle,
                                     slot->last_use_submit_value) !=
          VKR_METAL_MEMORY_STATUS_OK)
    return false_v;
  slot->live = false_v;
  slot->generation =
      slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
  slot->vertices = (VkrMetalBufferResource){0};
  slot->indices = (VkrMetalBufferResource){0};
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
    VkrMetalMaterialGpuRow row = {
        .base_color_texture_id = slot->textures[0].resource_id,
        .normal_texture_id = slot->textures[1].resource_id,
        .orm_texture_id = slot->textures[2].resource_id,
        .emissive_texture_id = slot->textures[3].resource_id,
        .material_id = create_info->material_id,
        .flags = create_info->texture_flags,
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
  VkrTextureHandle referenced_textures[ArrayCount(slot->texture_handles)] = {0};
  for (uint32_t texture_index = 0; texture_index < ArrayCount(slot->textures);
       ++texture_index) {
    if (slot->owns_textures) {
      if (vkr_metal_memory_device_retire(
              renderer->memory, slot->textures[texture_index].handle,
              slot->last_use_submit_value) != VKR_METAL_MEMORY_STATUS_OK)
        return false_v;
    } else {
      referenced_textures[texture_index] = slot->texture_handles[texture_index];
      VkrMetalPacketTexture *texture = vkr_metal_packet_resolve_retired_texture(
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
  for (uint32_t texture_index = 0;
       texture_index < ArrayCount(referenced_textures); ++texture_index) {
    const VkrTextureHandle texture_handle = referenced_textures[texture_index];
    VkrMetalPacketTexture *texture =
        vkr_metal_packet_resolve_retired_texture(renderer, texture_handle);
    if (texture && texture->pending_destroy &&
        !vkr_metal_packet_renderer_destroy_texture(renderer, texture_handle))
      return false_v;
  }
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
              vkr_metal_packet_resolve_texture(renderer, source->handle)))
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
  VkrMetalMaterialGpuRow row = {
      .base_color_texture_id = textures[0]->resource.resource_id,
      .normal_texture_id = textures[1]->resource.resource_id,
      .orm_texture_id = textures[2]->resource.resource_id,
      .emissive_texture_id = textures[3]->resource.resource_id,
      .material_id = handle.id,
      .flags = flags,
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
  slot->pbr = material->pbr;
  if (material->material_type != VKR_MATERIAL_TYPE_PBR) {
    slot->pbr.base_color = material->phong.diffuse_color;
    slot->pbr.emissive_factor = material->phong.emission_color;
  }
  slot->alpha_mode = material->alpha_mode;
  slot->alpha_cutoff = material->alpha_cutoff;
  slot->owns_textures = false_v;
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
      (uint64_t)create_info->width >
          UINT64_MAX / ((uint64_t)create_info->height * 4u))
    return false_v;
  *out_handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!@available(macOS 26.0, *))
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
       prepared->upload_array_layers != 1)) {
    [descriptor release];
    return false_v;
  }
  const uint64_t cube_bytes = (uint64_t)prepared->description.width *
                              prepared->description.height * 4u * 6u;
  if (cubemap && (prepared->description.channels != 4 ||
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
  const VkrTextureUploadPayload payload = {
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
  if (!uploaded) {
    if (slot->resource.metal_texture) {
      (void)vkr_metal_memory_device_retire(
          renderer->memory, slot->resource.handle, renderer->submit_value);
      (void)vkr_metal_memory_device_collect(renderer->memory,
                                            renderer->submit_value, NULL);
    }
    *slot = (VkrMetalPacketTexture){.generation = prior_generation};
    return false_v;
  }
  slot->generation = handle.generation;
  slot->last_use_submit_value = renderer->submit_value;
  slot->live = true_v;
  return true_v;
}

bool8_t vkr_metal_packet_renderer_create_cubemap(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketCubemapCreateInfo *create_info,
    VkrTextureHandle *out_handle) {
  if (!renderer || !create_info || !out_handle || renderer->frame_prepared ||
      !create_info->rgba8_faces || create_info->extent == 0)
    return false_v;
  *out_handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!@available(macOS 26.0, *))
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
      create_info->height > UINT32_MAX / 2u ||
      create_info->width != create_info->height * 2u)
    return false_v;
  *out_handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!@available(macOS 26.0, *))
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
    descriptor.mipmapLevelCount =
        vkr_metal_packet_mip_count(create_info->cube_extent);
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
        /* Logical owners may release a texture before all materials that
           reference its immutable row are retired. Keep the placement alive
           and complete destruction when the final material disappears. */
        slot->pending_destroy = true_v;
        return true_v;
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
  slot->live = false_v;
  slot->generation =
      slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
  slot->resource = (VkrMetalTextureResource){0};
  slot->pending_destroy = false_v;
  return true_v;
}

static bool8_t
vkr_metal_packet_ibl_needs_bake(const VkrMetalPacketRenderer *renderer,
                                const VkrRenderPacket *packet) {
  if (!renderer || !packet || !packet->skybox ||
      packet->skybox->cubemap.id == 0)
    return false_v;
  return !renderer->ibl_ready ||
                 renderer->ibl_source.id != packet->skybox->cubemap.id ||
                 renderer->ibl_source.generation !=
                     packet->skybox->cubemap.generation
             ? true_v
             : false_v;
}

static VkrMetalPacketTexture *
vkr_metal_packet_resolve_texture(VkrMetalPacketRenderer *renderer,
                                 VkrTextureHandle handle);

static bool8_t
vkr_metal_packet_prepare_upload(VkrMetalPacketRenderer *renderer,
                                const VkrRenderPacket *packet,
                                VkrMetalPacketFrameUpload *upload) {
  MemZero(upload, sizeof(*upload));
  const uint32_t world_count =
      packet->world ? packet->world->instance_count : 0;
  const uint32_t shadow_count =
      packet->shadow ? packet->shadow->instance_count : 0;
  const uint32_t picking_count =
      packet->picking ? packet->picking->instance_count : 0;
  const uint32_t editor_count =
      packet->editor ? packet->editor->instance_count : 0;
  const uint32_t ui_count = packet->ui ? packet->ui->instance_count : 0;
  const uint32_t world_text_count =
      packet->world ? packet->world->text_draw_count : 0;
  const uint32_t ui_text_count = packet->ui ? packet->ui->text_draw_count : 0;
  if ((world_count > 0 && !packet->world->instances) ||
      (shadow_count > 0 && !packet->shadow->instances) ||
      (picking_count > 0 && !packet->picking->instances) ||
      (editor_count > 0 && !packet->editor->instances) ||
      (ui_count > 0 && !packet->ui->instances) ||
      (packet->shadow &&
       packet->shadow->cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX))
    return false_v;
  if ((uint64_t)world_text_count + ui_text_count > renderer->max_draws)
    return false_v;
  const uint64_t instance_count = (uint64_t)world_count + shadow_count +
                                  picking_count + editor_count + ui_count;
  if (instance_count > renderer->max_instances)
    return false_v;
  uint64_t root_count = 1;
  if (packet->skybox) {
    root_count += 1;
    if (vkr_metal_packet_ibl_needs_bake(renderer, packet))
      root_count += 2 + VKR_IBL_PREFILTER_MIP_COUNT;
  }
  if (packet->world)
    root_count += (uint64_t)packet->world->opaque_draw_count +
                  packet->world->transmission_draw_count +
                  packet->world->transparent_draw_count + world_text_count;
  if (packet->shadow) {
    root_count += ((uint64_t)packet->shadow->opaque_draw_count +
                   (uint64_t)packet->shadow->alpha_draw_count) *
                  packet->shadow->cascade_count;
  }
  if (packet->picking && packet->picking->pending)
    root_count +=
        packet->picking->draw_count + world_text_count + ui_text_count;
  if (packet->editor)
    root_count += packet->editor->draw_count;
  if (packet->ui)
    root_count += packet->ui->draw_count + ui_text_count;
  if (root_count > renderer->max_draws)
    return false_v;

  const uint64_t world_bytes =
      (uint64_t)world_count * sizeof(VkrInstanceDataGPU);
  const uint64_t shadow_bytes =
      (uint64_t)shadow_count * sizeof(VkrInstanceDataGPU);
  const uint64_t picking_bytes =
      (uint64_t)picking_count * sizeof(VkrInstanceDataGPU);
  const uint64_t editor_bytes =
      (uint64_t)editor_count * sizeof(VkrInstanceDataGPU);
  const uint64_t ui_bytes = (uint64_t)ui_count * sizeof(VkrInstanceDataGPU);
  const uint64_t instance_bytes =
      world_bytes + shadow_bytes + picking_bytes + editor_bytes + ui_bytes;
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
  uint64_t text_geometry_end = ibl_probes_offset + ibl_probes_bytes;
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
    if (!packet->world->instances)
      return false_v;
    MemCopy(cpu, packet->world->instances, world_bytes);
    upload->world_instances_gpu = gpu;
  }
  if (shadow_bytes > 0) {
    if (!packet->shadow->instances)
      return false_v;
    MemCopy(cpu + world_bytes, packet->shadow->instances, shadow_bytes);
    upload->shadow_instances_gpu = gpu + world_bytes;
  }
  if (picking_bytes > 0) {
    if (!packet->picking->instances)
      return false_v;
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
  uint64_t text_offset = ibl_probes_offset + ibl_probes_bytes;
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

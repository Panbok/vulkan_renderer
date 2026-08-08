/* Per-frame upload preparation, pass encoding, submission, and failure
 * rollback. */
static void *vkr_metal_packet_next_root(VkrMetalPacketFrameUpload *upload,
                                        uint64_t *out_gpu_address) {
  if (!upload || upload->root_cursor >= upload->root_capacity)
    return NULL;
  const uint64_t offset =
      (uint64_t)upload->root_cursor++ * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  *out_gpu_address = upload->root_gpu + offset;
  void *root = upload->root_cpu + offset;
  MemZero(root, VKR_METAL_PACKET_DRAW_ROOT_STRIDE);
  return root;
}

static VkrMetalPacketMesh *
vkr_metal_packet_resolve_mesh(VkrMetalPacketRenderer *renderer,
                              VkrMeshHandle handle) {
  if (!renderer || handle.id == 0 || handle.id > renderer->max_meshes)
    return NULL;
  VkrMetalPacketMesh *mesh = &renderer->meshes[handle.id - 1];
  return mesh->live && mesh->generation == handle.generation ? mesh : NULL;
}

static VkrMetalPacketMaterial *
vkr_metal_packet_resolve_material(VkrMetalPacketRenderer *renderer,
                                  VkrMaterialHandle handle) {
  if (!renderer || handle.id == 0 || handle.id > renderer->max_materials)
    return NULL;
  VkrMetalPacketMaterial *material = &renderer->packet_materials[handle.id - 1];
  return material->live && material->generation == handle.generation ? material
                                                                     : NULL;
}

static VkrMetalPacketTexture *
vkr_metal_packet_resolve_texture(VkrMetalPacketRenderer *renderer,
                                 VkrTextureHandle handle) {
  if (!renderer || handle.id == 0 || handle.id > renderer->max_textures)
    return NULL;
  VkrMetalPacketTexture *texture = &renderer->textures[handle.id - 1];
  return texture->live && texture->generation == handle.generation ? texture
                                                                   : NULL;
}

static bool8_t vkr_metal_packet_encode_draws(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const VkrDrawItem *draws, uint32_t draw_count, uint64_t instances_gpu,
    uint32_t instance_count, uint32_t target_width, uint32_t target_height,
    Mat4 view_projection, const VkrRenderPacket *packet, bool8_t ibl_enabled,
    bool8_t transmission_pass, VkrMetalPacketFrameUpload *upload,
    uint32_t *domain_draw_count, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  if (draw_count == 0)
    return true_v;
  if (!draws || !instances_gpu) {
    log_error(
        "Metal draw encoding failed: draws=%p instances_gpu=%llu count=%u",
        (const void *)draws, (unsigned long long)instances_gpu, draw_count);
    return false_v;
  }
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  MTLCullMode active_cull_mode = MTLCullModeNone;
  [encoder setCullMode:active_cull_mode];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setDepthStencilState:depth_state];
  [encoder setRenderPipelineState:pipeline];
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->instance_count == 0 || draw->first_instance > instance_count ||
        draw->instance_count > instance_count - draw->first_instance) {
      log_error("Metal draw %u instance range invalid: first=%u count=%u "
                "available=%u",
                i, draw->first_instance, draw->instance_count, instance_count);
      return false_v;
    }
    const VkrMeshHandle geometry = {.id = draw->geometry.id,
                                    .generation = draw->geometry.generation};
    VkrMetalPacketMesh *mesh =
        vkr_metal_packet_resolve_mesh(renderer, geometry);
    VkrMetalPacketMaterial *material =
        vkr_metal_packet_resolve_material(renderer, draw->material);
    if (!mesh || !material || draw->submesh_index >= mesh->submesh_count) {
      log_error("Metal draw %u resource invalid: geometry=%u:%u mesh=%u "
                "material=%u:%u row=%u submesh=%u/%u",
                i, draw->geometry.id, draw->geometry.generation, mesh != NULL,
                draw->material.id, draw->material.generation, material != NULL,
                draw->submesh_index, mesh ? mesh->submesh_count : 0);
      return false_v;
    }
    const MTLCullMode material_cull_mode =
        material->double_sided ? MTLCullModeNone : MTLCullModeBack;
    if (material_cull_mode != active_cull_mode) {
      [encoder setCullMode:material_cull_mode];
      active_cull_mode = material_cull_mode;
    }
    const VkrMetalPacketSubmeshCreateInfo *submesh =
        &renderer->submeshes[(uint64_t)(geometry.id - 1) *
                                 renderer->max_submeshes_per_mesh +
                             draw->submesh_index];
    const uint64_t index_offset =
        (uint64_t)submesh->first_index * sizeof(uint32_t);
    uint64_t root_gpu = 0;
    VkrMetalPacketDrawRoot *root =
        vkr_metal_packet_next_root(upload, &root_gpu);
    if (!root) {
      log_error("Metal draw %u root upload exhausted: cursor=%u capacity=%u", i,
                upload->root_cursor, upload->root_capacity);
      return false_v;
    }
    *root = (VkrMetalPacketDrawRoot){
        .vertices = mesh->vertices.gpu_address,
        .instances = instances_gpu,
        .view_projection = view_projection,
        .materials =
            vkr_metal_material_table_device_gpu_address(renderer->materials),
        .irradiance_texture_id = renderer->ibl_irradiance.resource_id,
        .prefilter_texture_id = renderer->ibl_prefilter.resource_id,
        .brdf_texture_id = renderer->ibl_brdf.resource_id,
        .view_position = (Vec4){packet->globals.view_position.x,
                                packet->globals.view_position.y,
                                packet->globals.view_position.z, 1.0f},
        .material_index = material->row_index,
        .first_instance = draw->first_instance,
        .prefilter_mip_count = VKR_IBL_PREFILTER_MIP_COUNT,
        .flags = ibl_enabled ? 1u : 0u,
        .material_emissive = (Vec4){material->pbr.emissive_factor.x,
                                    material->pbr.emissive_factor.y,
                                    material->pbr.emissive_factor.z, 0.0f},
        .material_dielectric_specular =
            (Vec4){material->pbr.dielectric_specular.x,
                   material->pbr.dielectric_specular.y,
                   material->pbr.dielectric_specular.z,
                   transmission_pass ? 1.0f : 0.0f},
        .material_surface =
            (Vec4){material->pbr.metallic, material->pbr.roughness,
                   material->pbr.normal_scale,
                   material->pbr.occlusion_strength},
        .material_alpha =
            (Vec4){material->alpha_cutoff, material->pbr.transmission_factor,
                   material->pbr.ior, material->pbr.thickness_factor},
        .ibl_controls =
            packet->lighting
                ? (Vec4){packet->lighting->ibl_intensity,
                         packet->lighting->ibl_diffuse_intensity,
                         packet->lighting->ibl_specular_intensity,
                         1.0f / (float32_t)target_width}
                : (Vec4){1.0f, 1.0f, 1.0f, 1.0f / (float32_t)target_width},
        .directional_direction_enabled =
            packet->lighting
                ? (Vec4){packet->lighting->directional_direction.x,
                         packet->lighting->directional_direction.y,
                         packet->lighting->directional_direction.z,
                         packet->lighting->directional_enabled ? 1.0f : 0.0f}
                : vec4_zero(),
        .directional_color_intensity =
            packet->lighting ? (Vec4){packet->lighting->directional_color.x,
                                      packet->lighting->directional_color.y,
                                      packet->lighting->directional_color.z,
                                      packet->lighting->directional_intensity}
                             : vec4_zero(),
        .ambient_color = {packet->globals.ambient_color.x,
                          packet->globals.ambient_color.y,
                          packet->globals.ambient_color.z,
                          1.0f / (float32_t)target_height},
        .render_mode = packet->globals.render_mode,
        .alpha_mode = material->alpha_mode,
        .material_flags =
            packet->lighting && packet->lighting->ibl_enabled ? 1u : 0u,
        .reserved = 0u,
        .point_light_data = upload->point_light_data_gpu,
        .point_light_masks = upload->point_light_masks_gpu,
        .point_light_grid_origin_cell_size =
            packet->lighting && packet->lighting->point_light_count > 0
                ? (Vec4){packet->lighting->point_light_grid->origin.x,
                         packet->lighting->point_light_grid->origin.y,
                         packet->lighting->point_light_grid->origin.z,
                         packet->lighting->point_light_grid->cell_size}
                : vec4_zero(),
        .point_light_grid_dimensions_count =
            {packet->lighting && packet->lighting->point_light_count > 0
                 ? packet->lighting->point_light_grid->dimensions[0]
                 : 0,
             packet->lighting && packet->lighting->point_light_count > 0
                 ? packet->lighting->point_light_grid->dimensions[1]
                 : 0,
             packet->lighting && packet->lighting->point_light_count > 0
                 ? packet->lighting->point_light_grid->dimensions[2]
                 : 0,
             packet->lighting && packet->lighting->point_light_count > 0
                 ? packet->lighting->point_light_grid->cell_count
                 : 0},
        .point_light_global_mask =
            packet->lighting && packet->lighting->point_light_count > 0
                ? packet->lighting->point_light_grid->global_mask
                : (VkrPointLightMask){0},
        .point_light_count =
            packet->lighting ? packet->lighting->point_light_count : 0,
        .shadow_texture_id = upload->shadow_texture_id,
        .shadow_cascades = upload->shadow_cascades_gpu,
        .view = packet->globals.view,
        .shadow_cascade_count =
            packet->shadow ? packet->shadow->cascade_count : 0,
        .shadow_bias = 0.0001f,
        .transmission_texture_id = upload->transmission_texture_id,
        .material_attenuation_color = {material->pbr.attenuation_color.x,
                                       material->pbr.attenuation_color.y,
                                       material->pbr.attenuation_color.z,
                                       material->pbr.attenuation_distance},
        .ibl_probes = upload->ibl_probes_gpu,
        .ibl_probe_count =
            packet->lighting ? packet->lighting->ibl_probe_count : 0,
    };
    [renderer->argument_table setAddress:root_gpu atIndex:0];
    [renderer->argument_table setAddress:root_gpu atIndex:1];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:submesh->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:mesh->indices.gpu_address + index_offset
                 indexBufferLength:mesh->indices.length - index_offset
                     instanceCount:draw->instance_count
                        baseVertex:submesh->vertex_offset
                      baseInstance:0];
    const uint64_t pending_submit = renderer->submit_value + 1;
    mesh->last_use_submit_value = pending_submit;
    material->last_use_submit_value = pending_submit;
    if (ibl_enabled)
      renderer->ibl_last_use_submit_value = pending_submit;
    (*domain_draw_count)++;
    result->indexed_draw_count++;
  }
  return true_v;
}

static bool8_t vkr_metal_packet_encode_text_draws(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const VkrPreparedTextDraw *draws, uint32_t draw_count,
    uint32_t text_upload_start, uint32_t target_width, uint32_t target_height,
    Mat4 view_projection, bool8_t ui_domain, VkrMetalPacketFrameUpload *upload,
    uint32_t *domain_draw_count, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  if (draw_count == 0)
    return true_v;
  if (!draws || !upload->text_uploads || !domain_draw_count)
    return false_v;
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setDepthStencilState:depth_state];
  [encoder setRenderPipelineState:pipeline];
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    const VkrMetalPacketTextUpload *text_upload =
        &upload->text_uploads[text_upload_start + i];
    VkrMetalPacketTexture *atlas =
        vkr_metal_packet_resolve_texture(renderer, draw->atlas);
    if (!atlas || text_upload->vertices_gpu == 0 ||
        text_upload->indices_gpu == 0 || text_upload->indices_length == 0)
      return false_v;
    uint64_t root_gpu = 0;
    VkrMetalPacketTextRoot *root =
        vkr_metal_packet_next_root(upload, &root_gpu);
    if (!root)
      return false_v;
    *root = (VkrMetalPacketTextRoot){
        .vertices = text_upload->vertices_gpu,
        .atlas_texture_id = atlas->resource.resource_id,
        .model = draw->model,
        .view_projection = view_projection,
        .controls = {draw->screen_px_range, (float32_t)draw->font_mode,
                     (float32_t)target_width, (float32_t)target_height},
        .object_id = draw->object_id,
        .flags = ui_domain ? 1u : 0u,
    };
    [renderer->argument_table setAddress:root_gpu atIndex:1];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:draw->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:text_upload->indices_gpu
                 indexBufferLength:text_upload->indices_length
                     instanceCount:1
                        baseVertex:0
                      baseInstance:0];
    atlas->last_use_submit_value = renderer->submit_value + 1u;
    (*domain_draw_count)++;
    result->indexed_draw_count++;
  }
  return true_v;
}

static bool8_t vkr_metal_packet_encode_tonemap(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4RenderCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  if (pass->desc.image_reads.length == 0)
    return false_v;
  const VkrRgImageUse *read =
      vector_get_VkrRgImageUse(&pass->desc.image_reads, 0);
  id<MTLTexture> source = vkr_metal_packet_texture(renderer, read->image);
  if (!source)
    return false_v;
  uint64_t root_gpu = 0;
  VkrMetalPacketTonemapRoot *root =
      vkr_metal_packet_next_root(upload, &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketTonemapRoot){
      .source_texture_id = source.gpuResourceID._impl,
      .exposure =
          packet->globals.exposure > 0.0f ? packet->globals.exposure : 1.0f,
      .reserved = {renderer->srgb_output ? 1u : 0u, 0u, 0u},
  };
  [renderer->argument_table setAddress:root_gpu atIndex:1];
  [encoder setViewport:(MTLViewport){0.0, 0.0, source.width, source.height, 0.0,
                                     1.0}];
  [encoder setRenderPipelineState:renderer->tonemap_pipeline];
  [encoder setArgumentTable:renderer->argument_table
                   atStages:MTLRenderStageFragment];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  return true_v;
}

static bool8_t vkr_metal_packet_encode_skybox(
    VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet,
    id<MTL4RenderCommandEncoder> encoder, uint32_t target_width,
    uint32_t target_height, VkrMetalPacketFrameUpload *upload,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  if (!packet->skybox)
    return true_v;
  VkrMetalPacketTexture *cubemap =
      vkr_metal_packet_resolve_texture(renderer, packet->skybox->cubemap);
  if (!cubemap)
    return false_v;
  uint64_t root_gpu = 0;
  VkrMetalPacketSkyboxRoot *root =
      vkr_metal_packet_next_root(upload, &root_gpu);
  if (!root)
    return false_v;
  Mat4 view = packet->globals.view;
  view.elements[12] = 0.0f;
  view.elements[13] = 0.0f;
  view.elements[14] = 0.0f;
  *root = (VkrMetalPacketSkyboxRoot){
      .inverse_view_projection = mat4_inverse(vkr_metal_packet_clip_matrix(
          renderer->convert_vulkan_clip_y,
          mat4_mul(packet->globals.projection, view))),
      .cubemap_texture_id = cubemap->resource.resource_id,
      .target_width = (float32_t)target_width,
      .target_height = (float32_t)target_height,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:1];
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setDepthStencilState:renderer->depth_read_state];
  [encoder setRenderPipelineState:renderer->skybox_pipeline];
  [encoder setArgumentTable:renderer->argument_table
                   atStages:MTLRenderStageFragment];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  cubemap->last_use_submit_value = renderer->submit_value + 1;
  result->skybox_draw_count++;
  return true_v;
}

static bool8_t vkr_metal_packet_encode_ibl(
    VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet,
    id<MTL4ComputeCommandEncoder> encoder, VkrMetalPacketFrameUpload *upload,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  VkrTextureHandle source_handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!vkr_metal_packet_ibl_source(packet, &source_handle))
    return true_v;
  VkrMetalPacketTexture *source =
      vkr_metal_packet_resolve_texture(renderer, source_handle);
  if (!source || !renderer->ibl_live)
    return false_v;
  if (!vkr_metal_packet_ibl_needs_bake(renderer, packet))
    return true_v;
  id<MTLTexture> source_texture =
      (id<MTLTexture>)source->resource.metal_texture;

  uint64_t root_gpu = 0;
  VkrMetalPacketIrradianceRoot *irradiance_root =
      vkr_metal_packet_next_root(upload, &root_gpu);
  if (!irradiance_root)
    return false_v;
  *irradiance_root = (VkrMetalPacketIrradianceRoot){
      .source_texture_id = source->resource.resource_id,
      .target_texture_id = renderer->ibl_irradiance.resource_id,
      .sample_count = 128,
      .target_size = VKR_IBL_IRRADIANCE_SIZE,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0];
  [encoder setComputePipelineState:renderer->ibl_irradiance_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(VKR_IBL_IRRADIANCE_SIZE,
                                       VKR_IBL_IRRADIANCE_SIZE, 6)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  result->ibl_dispatch_count++;

  for (uint32_t mip = 0; mip < VKR_IBL_PREFILTER_MIP_COUNT; ++mip) {
    VkrMetalPacketPrefilterRoot *prefilter_root =
        vkr_metal_packet_next_root(upload, &root_gpu);
    if (!prefilter_root)
      return false_v;
    const uint32_t mip_size = Max(1u, VKR_IBL_PREFILTER_SIZE >> mip);
    *prefilter_root = (VkrMetalPacketPrefilterRoot){
        .source_texture_id = source->resource.resource_id,
        .target_texture_id = renderer->ibl_prefilter.resource_id,
        .roughness =
            (float32_t)mip / (float32_t)(VKR_IBL_PREFILTER_MIP_COUNT - 1u),
        .source_face_size = (float32_t)source_texture.width,
        .source_mip_count = (float32_t)source_texture.mipmapLevelCount,
        .target_mip = mip,
    };
    [renderer->argument_table setAddress:root_gpu atIndex:0];
    [encoder setComputePipelineState:renderer->ibl_prefilter_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(mip_size, mip_size, 6)
        threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    result->ibl_dispatch_count++;
  }

  VkrMetalPacketBrdfRoot *brdf_root =
      vkr_metal_packet_next_root(upload, &root_gpu);
  if (!brdf_root)
    return false_v;
  *brdf_root = (VkrMetalPacketBrdfRoot){
      .target_texture_id = renderer->ibl_brdf.resource_id,
      .sample_count = 512,
      .target_size = VKR_IBL_BRDF_SIZE,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0];
  [encoder setComputePipelineState:renderer->ibl_brdf_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(VKR_IBL_BRDF_SIZE, VKR_IBL_BRDF_SIZE, 1)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  result->ibl_dispatch_count++;

  const uint64_t pending_submit = renderer->submit_value + 1;
  source->last_use_submit_value = pending_submit;
  renderer->ibl_last_use_submit_value = pending_submit;
  return true_v;
}

static bool8_t vkr_metal_packet_encode_pass_body(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4RenderCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> target = nil;
  if (pass->desc.color_attachments.length > 0) {
    const VkrRgAttachment *attachment =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, 0);
    target = vkr_metal_packet_texture(renderer, attachment->image);
  } else if (pass->desc.has_depth_attachment) {
    target =
        vkr_metal_packet_texture(renderer, pass->desc.depth_attachment.image);
  }
  if (!target)
    return false_v;
  if (vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.skybox"))
    return vkr_metal_packet_encode_skybox(
        renderer, packet, encoder, (uint32_t)target.width,
        (uint32_t)target.height, upload, result);
  if (vkr_string8_equals_cstr(&pass->desc.execute_name,
                              "pass.shadow.cascade")) {
    if (!packet->shadow)
      return true_v;
    const uint32_t cascade = (uint32_t)(uintptr_t)pass->desc.user_data;
    if (cascade >= packet->shadow->cascade_count)
      return true_v;
    if (!vkr_metal_packet_encode_draws(
            renderer, encoder, renderer->shadow_pipeline,
            renderer->depth_write_state, packet->shadow->opaque_draws,
            packet->shadow->opaque_draw_count, upload->shadow_instances_gpu,
            packet->shadow->instance_count, (uint32_t)target.width,
            (uint32_t)target.height,
            vkr_metal_packet_clip_matrix(
                renderer->convert_vulkan_clip_y,
                packet->shadow->light_view_proj[cascade]),
            packet, false_v, false_v, upload, &result->shadow_draw_count,
            result))
      return false_v;
    return vkr_metal_packet_encode_draws(
        renderer, encoder, renderer->shadow_pipeline,
        renderer->depth_write_state, packet->shadow->alpha_draws,
        packet->shadow->alpha_draw_count, upload->shadow_instances_gpu,
        packet->shadow->instance_count, (uint32_t)target.width,
        (uint32_t)target.height,
        vkr_metal_packet_clip_matrix(renderer->convert_vulkan_clip_y,
                                     packet->shadow->light_view_proj[cascade]),
        packet, false_v, false_v, upload, &result->shadow_draw_count, result);
  }
  if (vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.world.opaque")) {
    if (!packet->world)
      return true_v;
    return vkr_metal_packet_encode_draws(
        renderer, encoder, renderer->opaque_pipeline,
        renderer->depth_write_state, packet->world->opaque_draws,
        packet->world->opaque_draw_count, upload->world_instances_gpu,
        packet->world->instance_count, (uint32_t)target.width,
        (uint32_t)target.height,
        vkr_metal_packet_clip_matrix(
            renderer->convert_vulkan_clip_y,
            mat4_mul(packet->globals.projection, packet->globals.view)),
        packet, true_v, false_v, upload, &result->opaque_draw_count, result);
  }
  if (vkr_string8_equals_cstr(&pass->desc.execute_name,
                              "pass.world.transmission")) {
    if (!packet->world)
      return true_v;
    return vkr_metal_packet_encode_draws(
        renderer, encoder, renderer->opaque_pipeline,
        renderer->depth_read_state, packet->world->transmission_draws,
        packet->world->transmission_draw_count, upload->world_instances_gpu,
        packet->world->instance_count, (uint32_t)target.width,
        (uint32_t)target.height,
        vkr_metal_packet_clip_matrix(
            renderer->convert_vulkan_clip_y,
            mat4_mul(packet->globals.projection, packet->globals.view)),
        packet, true_v, true_v, upload, &result->transmission_draw_count,
        result);
  }
  if (vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.world.blend")) {
    if (!packet->world)
      return true_v;
    const Mat4 view_projection = vkr_metal_packet_clip_matrix(
        renderer->convert_vulkan_clip_y,
        mat4_mul(packet->globals.projection, packet->globals.view));
    if (!vkr_metal_packet_encode_draws(
            renderer, encoder, renderer->blend_pipeline,
            renderer->depth_read_state, packet->world->transparent_draws,
            packet->world->transparent_draw_count, upload->world_instances_gpu,
            packet->world->instance_count, (uint32_t)target.width,
            (uint32_t)target.height, view_projection, packet, true_v, false_v,
            upload, &result->blend_draw_count, result))
      return false_v;
    return vkr_metal_packet_encode_text_draws(
        renderer, encoder, renderer->world_text_pipeline,
        renderer->depth_read_state, packet->world->text_draws,
        packet->world->text_draw_count, 0, (uint32_t)target.width,
        (uint32_t)target.height, view_projection, false_v, upload,
        &result->text_draw_count, result);
  }
  if (vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.picking")) {
    if (!packet->picking || !packet->picking->pending)
      return true_v;
    const Mat4 view_projection = vkr_metal_packet_clip_matrix(
        renderer->convert_vulkan_clip_y,
        mat4_mul(packet->globals.projection, packet->globals.view));
    if (!vkr_metal_packet_encode_draws(
            renderer, encoder, renderer->picking_pipeline,
            renderer->depth_write_state, packet->picking->draws,
            packet->picking->draw_count, upload->picking_instances_gpu,
            packet->picking->instance_count, (uint32_t)target.width,
            (uint32_t)target.height, view_projection, packet, false_v, false_v,
            upload, &result->picking_draw_count, result))
      return false_v;
    if (packet->world &&
        !vkr_metal_packet_encode_text_draws(
            renderer, encoder, renderer->picking_text_pipeline,
            renderer->depth_write_state, packet->world->text_draws,
            packet->world->text_draw_count, 0, (uint32_t)target.width,
            (uint32_t)target.height, view_projection, false_v, upload,
            &result->picking_text_draw_count, result))
      return false_v;
    return !packet->ui ||
           vkr_metal_packet_encode_text_draws(
               renderer, encoder, renderer->picking_text_pipeline,
               renderer->depth_write_state, packet->ui->text_draws,
               packet->ui->text_draw_count, upload->world_text_count,
               (uint32_t)target.width, (uint32_t)target.height, mat4_identity(),
               true_v, upload, &result->picking_text_draw_count, result);
  }
  if (vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.tonemap"))
    return vkr_metal_packet_encode_tonemap(renderer, pass, packet, encoder,
                                           upload);
  if (vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.editor")) {
    if (!vkr_metal_packet_encode_tonemap(renderer, pass, packet, encoder,
                                         upload))
      return false_v;
    if (!packet->editor)
      return true_v;
    return vkr_metal_packet_encode_draws(
        renderer, encoder, renderer->overlay_pipeline, nil,
        packet->editor->draws, packet->editor->draw_count,
        upload->editor_instances_gpu, packet->editor->instance_count,
        (uint32_t)target.width, (uint32_t)target.height, mat4_identity(),
        packet, false_v, false_v, upload, &result->editor_draw_count, result);
  }
  if (vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.ui")) {
    if (!packet->ui)
      return true_v;
    if (!vkr_metal_packet_encode_draws(
            renderer, encoder, renderer->overlay_pipeline, nil,
            packet->ui->draws, packet->ui->draw_count, upload->ui_instances_gpu,
            packet->ui->instance_count, (uint32_t)target.width,
            (uint32_t)target.height, mat4_identity(), packet, false_v, false_v,
            upload, &result->ui_draw_count, result))
      return false_v;
    return vkr_metal_packet_encode_text_draws(
        renderer, encoder, renderer->ui_text_pipeline, nil,
        packet->ui->text_draws, packet->ui->text_draw_count,
        upload->world_text_count, (uint32_t)target.width,
        (uint32_t)target.height, mat4_identity(), true_v, upload,
        &result->text_draw_count, result);
  }
  return true_v;
}

bool8_t vkr_metal_packet_renderer_prepare_frame(
    VkrMetalPacketRenderer *renderer,
    const VkrRenderGraphFrameInfo *frame_info) {
  if (!renderer || !frame_info || frame_info->target_width == 0 ||
      frame_info->target_height == 0 || frame_info->viewport_width == 0 ||
      frame_info->viewport_height == 0 || renderer->frame_prepared)
    return false_v;
  [renderer->drawable release];
  renderer->drawable = nil;
  renderer->prepared_frame = *frame_info;
  if (renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW) {
    if (vkr_metal_packet_pixel_format(frame_info->target_color_format) !=
        renderer->layer.pixelFormat)
      return false_v;
  }
  if (!vkr_metal_packet_acquire_command_slot(renderer, false_v))
    return false_v;
  if (renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW) {
    renderer->layer.drawableSize =
        CGSizeMake(frame_info->target_width, frame_info->target_height);
    renderer->drawable = [[renderer->layer nextDrawable] retain];
    if (!renderer->drawable ||
        renderer->drawable.texture.width != frame_info->target_width ||
        renderer->drawable.texture.height != frame_info->target_height) {
      [renderer->drawable release];
      renderer->drawable = nil;
      vkr_metal_packet_release_unsubmitted_command_slot(renderer);
      return false_v;
    }
  }
  renderer->frame_prepared = true_v;
  return true_v;
}

bool8_t
vkr_metal_packet_renderer_submit_packet(VkrMetalPacketRenderer *renderer,
                                        const VkrRenderPacket *packet,
                                        VkrMetalPacketResult *out_result) {
  if (!renderer || !packet || !out_result || !renderer->frame_prepared)
    return false_v;
  if (packet->packet_version != VKR_RENDER_PACKET_VERSION ||
      !vkr_metal_packet_lighting_valid(packet->lighting) ||
      (packet->world &&
       !vkr_metal_packet_text_draws_valid(packet->world->text_draws,
                                          packet->world->text_draw_count)) ||
      (packet->ui &&
       !vkr_metal_packet_text_draws_valid(packet->ui->text_draws,
                                          packet->ui->text_draw_count))) {
    vkr_metal_packet_finish_frame(renderer, false_v, false_v);
    return false_v;
  }
  if (!@available(macOS 26.0, *)) {
    vkr_metal_packet_finish_frame(renderer, false_v, false_v);
    return false_v;
  }

  *out_result = (VkrMetalPacketResult){0};
  /* prepare_frame acquires the present target before the packet exists. Patch
     packet-derived graph conditions here, immediately before graph build. */
  renderer->prepared_frame.picking_pending =
      packet->picking && packet->picking->pending;
  if (packet->shadow) {
    renderer->prepared_frame.shadow_cascade_count =
        MIN(packet->shadow->cascade_count, VKR_SHADOW_CASCADE_COUNT_MAX);
  }
  const uint64_t completed_submit_value = renderer->completion.signaledValue;
  vkr_metal_capture_ring_collect(&renderer->capture_ring,
                                 completed_submit_value);
  uint32_t collected = 0;
  if (vkr_metal_memory_device_collect(renderer->memory, completed_submit_value,
                                      &collected) !=
          VKR_METAL_MEMORY_STATUS_OK ||
      vkr_metal_material_table_device_collect(
          renderer->materials, completed_submit_value, &collected) !=
          VKR_METAL_MATERIAL_STATUS_OK) {
    log_error("Metal packet stage failed: retirement collection");
    vkr_metal_packet_finish_frame(renderer, false_v, false_v);
    return false_v;
  }

  vkr_rg_begin_frame(renderer->graph, &renderer->prepared_frame);
  vkr_rg_set_packet(renderer->graph, packet);
  if (!vkr_rg_build_from_json(renderer->graph, &renderer->json_graph,
                              &renderer->prepared_frame,
                              &renderer->executors) ||
      !vkr_rg_compile_schedule(renderer->graph) ||
      renderer->graph->passes.length > renderer->max_passes ||
      !vkr_metal_packet_realize_images(renderer)) {
    log_error("Metal packet stage failed: graph build/compile/realize");
    vkr_metal_packet_finish_frame(renderer, true_v, false_v);
    return false_v;
  }

  const bool8_t picking = renderer->prepared_frame.picking_pending;
  VkrTextureHandle ibl_source = VKR_TEXTURE_HANDLE_INVALID;
  const bool8_t ibl = vkr_metal_packet_ibl_source(packet, &ibl_source);
  const bool8_t ibl_bake = vkr_metal_packet_ibl_needs_bake(renderer, packet);
  const bool8_t timing_requested = packet->debug &&
                                   packet->debug->enable_timing &&
                                   packet->debug->capture_pass_timestamps;
  if (timing_requested) {
    [renderer->timestamp_heap
        invalidateCounterRange:NSMakeRange(0, renderer->graph->passes.length *
                                                  2u)];
  }
  const uint64_t ibl_capture_offset = picking ? 16 : 8;
  const uint64_t ibl_prefilter_capture_offset = ibl_capture_offset + 8;
  const uint64_t ibl_brdf_capture_offset =
      ibl_prefilter_capture_offset + VKR_IBL_PREFILTER_MIP_COUNT * 8u;
  const uint64_t probe_size =
      ibl ? ibl_brdf_capture_offset + 8 : ibl_capture_offset;
  VkrMetalPacketCapturePlan capture_plans[VKR_CAPTURE_MAX_ITEMS] = {0};
  VkrCaptureBackendItemPlan capture_ring_plans[VKR_CAPTURE_MAX_ITEMS] = {0};
  uint32_t capture_count = 0;
  uint64_t capture_size = probe_size;
  if (!vkr_metal_packet_capture_plan(renderer, packet, probe_size,
                                     capture_plans, &capture_count,
                                     &capture_size)) {
    log_error("Metal packet stage failed: capture plan");
    vkr_metal_packet_finish_frame(renderer, true_v, false_v);
    return false_v;
  }
  const VkrCaptureBatchRequest *capture_request =
      packet->debug ? packet->debug->capture : NULL;
  uint64_t capture_base = 0;
  if (capture_count > 0) {
    capture_base = capture_plans[0].buffer_offset;
    for (uint32_t i = 0; i < capture_count; ++i) {
      capture_ring_plans[i] = (VkrCaptureBackendItemPlan){
          .result = capture_plans[i].result,
          .buffer_offset = capture_plans[i].buffer_offset - capture_base,
      };
    }
    const VkrRendererError capture_error = vkr_metal_capture_ring_reserve(
        &renderer->capture_ring, capture_request, capture_ring_plans,
        packet->frame.frame_index);
    if (capture_error != VKR_RENDERER_ERROR_NONE) {
      out_result->capture = (VkrCapturePollResult){
          .status = VKR_CAPTURE_STATUS_FAILED, .error = capture_error};
      vkr_metal_packet_finish_frame(renderer, true_v, false_v);
      return false_v;
    }
  }
  VkrMetalPacketFrameUpload frame_upload = {0};
  if (!vkr_metal_packet_prepare_upload(renderer, packet, &frame_upload)) {
    log_error(
        "Metal packet stage failed: frame upload (world=%u shadow=%u ui=%u)",
        packet->world ? packet->world->instance_count : 0,
        packet->shadow ? packet->shadow->instance_count : 0,
        packet->ui ? packet->ui->instance_count : 0);
    if (frame_upload.acquired)
      vkr_metal_memory_device_cancel_ring(
          renderer->memory, VKR_METAL_RING_KIND_UPLOAD, frame_upload.slice);
    vkr_metal_packet_capture_fail_reserved(
        renderer, capture_request, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    vkr_metal_packet_finish_frame(renderer, true_v, false_v);
    return false_v;
  }
  VkrMetalRingSlice readback_slice = {0};
  VkrMetalAddressPair readback = {0};
  void *readback_object = NULL;
  if (vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_READBACK, capture_size,
          renderer->completion.signaledValue, &readback_slice, &readback,
          &readback_object) != VKR_METAL_MEMORY_STATUS_OK) {
    log_error("Metal packet stage failed: readback ring (%llu bytes)",
              (unsigned long long)capture_size);
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_UPLOAD, frame_upload.slice);
    vkr_metal_packet_capture_fail_reserved(renderer, capture_request,
                                           VKR_RENDERER_ERROR_CAPTURE_BUSY);
    vkr_metal_packet_finish_frame(renderer, true_v, false_v);
    return false_v;
  }
  MemZero(readback.cpu_address, capture_size);

  [renderer->command_buffer
      beginCommandBufferWithAllocator:renderer->command_allocator];
  id<MTLResidencySet> residency_sets[] = {
      (id<MTLResidencySet>)vkr_metal_memory_device_residency_set(
          renderer->memory),
      (id<MTLResidencySet>)vkr_metal_material_table_device_residency_set(
          renderer->materials),
  };
  [renderer->command_buffer useResidencySets:residency_sets count:2];

  const VkrGpuDependency capture_dependency = {
      .src_stages = VKR_GPU_STAGE_COLOR_OUTPUT,
      .dst_stages = VKR_GPU_STAGE_TRANSFER,
      .visibility = VKR_GPU_VISIBILITY_DEVICE,
  };
  const VkrGpuDependency shadow_capture_dependency = {
      .src_stages = VKR_GPU_STAGE_LATE_DEPTH,
      .dst_stages = VKR_GPU_STAGE_TRANSFER,
      .visibility = VKR_GPU_VISIBILITY_DEVICE,
  };
  const VkrGpuDependency ibl_capture_dependency = {
      .src_stages = VKR_GPU_STAGE_COMPUTE_SHADER,
      .dst_stages = VKR_GPU_STAGE_TRANSFER,
      .visibility = VKR_GPU_VISIBILITY_DEVICE,
  };
  const VkrGpuDependency batch_capture_dependency = {
      .src_stages = VKR_GPU_STAGE_ALL_GRAPHICS | VKR_GPU_STAGE_COMPUTE_SHADER |
                    VKR_GPU_STAGE_TRANSFER,
      .dst_stages = VKR_GPU_STAGE_TRANSFER,
      .visibility = VKR_GPU_VISIBILITY_DEVICE,
  };
  bool8_t passed = true_v;
  const VkrRgPass *failed_pass = NULL;
  for (uint64_t order = 0;
       passed && order < renderer->graph->execution_order.length; ++order) {
    const uint32_t *pass_index =
        vector_get_uint32_t(&renderer->graph->execution_order, order);
    VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, *pass_index);
    if (!pass || pass->culled)
      continue;
    const CFTimeInterval pass_cpu_begin = CACurrentMediaTime();
    const bool8_t last_present_writer =
        vkr_metal_packet_is_last_present_writer(renderer, order);
    if (timing_requested) {
      [renderer->command_buffer
          writeTimestampIntoHeap:renderer->timestamp_heap
                         atIndex:(NSUInteger)*pass_index * 2u];
    }
    if (pass->desc.type == VKR_RG_PASS_TYPE_GRAPHICS) {
      MTL4RenderPassDescriptor *descriptor =
          renderer->render_passes[*pass_index];
      passed = vkr_metal_packet_configure_render_pass(renderer, pass, packet,
                                                      descriptor);
      id<MTL4RenderCommandEncoder> encoder =
          passed ? [renderer->command_buffer
                       renderCommandEncoderWithDescriptor:descriptor]
                 : nil;
      passed =
          passed && encoder &&
          vkr_metal_packet_encode_consumers(encoder, pass,
                                            &out_result->dependency_count) &&
          vkr_metal_packet_encode_pass_body(renderer, pass, packet, encoder,
                                            &frame_upload, out_result) &&
          vkr_metal_packet_encode_producers(renderer, encoder, *pass_index,
                                            &out_result->dependency_count);
      if (passed && last_present_writer) {
        passed = vkr_metal_packet_encode_dependency(
            encoder, &capture_dependency, true_v);
        out_result->dependency_count++;
      }
      if (passed &&
          vkr_string8_equals_cstr(&pass->desc.execute_name,
                                  "pass.shadow.cascade") &&
          (uint32_t)(uintptr_t)pass->desc.user_data + 1 ==
              renderer->prepared_frame.shadow_cascade_count) {
        passed = vkr_metal_packet_encode_dependency(
            encoder, &shadow_capture_dependency, true_v);
        out_result->dependency_count++;
      }
      if (passed && capture_count > 0) {
        passed = vkr_metal_packet_encode_dependency(
            encoder, &batch_capture_dependency, true_v);
        out_result->dependency_count++;
      }
      [encoder endEncoding];
      out_result->graphics_pass_count++;
    } else {
      id<MTL4ComputeCommandEncoder> encoder =
          [renderer->command_buffer computeCommandEncoder];
      passed = encoder && vkr_metal_packet_encode_consumers(
                              encoder, pass, &out_result->dependency_count);
      if (passed && pass->desc.type == VKR_RG_PASS_TYPE_TRANSFER)
        passed = vkr_metal_packet_encode_transfer(renderer, pass, encoder);
      if (passed &&
          vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.ibl_bake"))
        passed = vkr_metal_packet_encode_ibl(renderer, packet, encoder,
                                             &frame_upload, out_result);
      if (passed && picking)
        passed = vkr_metal_packet_encode_picking_readback(
            renderer, pass, packet, encoder, (id<MTLBuffer>)readback_object,
            readback_slice.offset + 8);
      if (passed)
        passed = vkr_metal_packet_encode_producers(
            renderer, encoder, *pass_index, &out_result->dependency_count);
      if (passed && ibl_bake &&
          vkr_string8_equals_cstr(&pass->desc.execute_name, "pass.ibl_bake")) {
        passed = vkr_metal_packet_encode_dependency(
            encoder, &ibl_capture_dependency, true_v);
        out_result->dependency_count++;
      }
      if (passed && capture_count > 0) {
        passed = vkr_metal_packet_encode_dependency(
            encoder, &batch_capture_dependency, true_v);
        out_result->dependency_count++;
      }
      [encoder endEncoding];
      if (pass->desc.type == VKR_RG_PASS_TYPE_TRANSFER)
        out_result->transfer_pass_count++;
      else
        out_result->compute_pass_count++;
    }
    if (!passed)
      failed_pass = pass;
    if (timing_requested) {
      [renderer->command_buffer
          writeTimestampIntoHeap:renderer->timestamp_heap
                         atIndex:(NSUInteger)*pass_index * 2u + 1u];
    }
    if (out_result->pass_timing_count < VKR_METAL_PACKET_MAX_PASS_TIMINGS) {
      VkrMetalPacketPassTiming *timing =
          &out_result->pass_timings[out_result->pass_timing_count++];
      const uint64_t name_length =
          Min(pass->desc.name.length,
              (uint64_t)VKR_METAL_PACKET_TIMING_NAME_CAPACITY - 1u);
      if (name_length > 0)
        MemCopy(timing->name, pass->desc.name.str, name_length);
      timing->name[name_length] = '\0';
      timing->pass_index = *pass_index;
      timing->cpu_ms = (CACurrentMediaTime() - pass_cpu_begin) * 1000.0;
      timing->valid = false_v;
    }
    out_result->executed_pass_count++;
  }

  id<MTLTexture> present =
      vkr_metal_packet_texture(renderer, renderer->graph->present_image);
  id<MTL4ComputeCommandEncoder> capture_encoder =
      passed ? [renderer->command_buffer computeCommandEncoder] : nil;
  if (passed) {
    passed = capture_encoder && present &&
             vkr_metal_packet_encode_dependency(capture_encoder,
                                                &capture_dependency, false_v) &&
             vkr_metal_packet_encode_dependency(
                 capture_encoder, &shadow_capture_dependency, false_v);
    out_result->dependency_count += 2;
    if (passed && ibl_bake) {
      passed = vkr_metal_packet_encode_dependency(
          capture_encoder, &ibl_capture_dependency, false_v);
      out_result->dependency_count++;
    }
    if (passed && capture_count > 0) {
      passed = vkr_metal_packet_encode_dependency(
          capture_encoder, &batch_capture_dependency, false_v);
      out_result->dependency_count++;
    }
  }
  if (passed) {
    [capture_encoder copyFromTexture:present
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(1, 1, 1)
                            toBuffer:(id<MTLBuffer>)readback_object
                   destinationOffset:readback_slice.offset
              destinationBytesPerRow:4
            destinationBytesPerImage:4
                             options:MTLBlitOptionNone];
    id<MTLTexture> shadow =
        vkr_metal_packet_named_texture(renderer, "shadow_map");
    if (!shadow) {
      passed = false_v;
    } else {
      [capture_encoder copyFromTexture:shadow
                           sourceSlice:0
                           sourceLevel:0
                          sourceOrigin:MTLOriginMake(0, 0, 0)
                            sourceSize:MTLSizeMake(1, 1, 1)
                              toBuffer:(id<MTLBuffer>)readback_object
                     destinationOffset:readback_slice.offset + 4
                destinationBytesPerRow:sizeof(float32_t)
              destinationBytesPerImage:sizeof(float32_t)
                               options:MTLBlitOptionNone];
    }
    if (passed && ibl) {
      [capture_encoder
                   copyFromTexture:(id<MTLTexture>)
                                       renderer->ibl_irradiance.metal_texture
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(1, 1, 1)
                          toBuffer:(id<MTLBuffer>)readback_object
                 destinationOffset:readback_slice.offset + ibl_capture_offset
            destinationBytesPerRow:8
          destinationBytesPerImage:8
                           options:MTLBlitOptionNone];
      for (uint32_t mip = 0; mip < VKR_IBL_PREFILTER_MIP_COUNT; ++mip) {
        [capture_encoder
                     copyFromTexture:(id<MTLTexture>)
                                         renderer->ibl_prefilter.metal_texture
                         sourceSlice:0
                         sourceLevel:mip
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(1, 1, 1)
                            toBuffer:(id<MTLBuffer>)readback_object
                   destinationOffset:readback_slice.offset +
                                     ibl_prefilter_capture_offset + mip * 8u
              destinationBytesPerRow:8
            destinationBytesPerImage:8
                             options:MTLBlitOptionNone];
      }
      [capture_encoder copyFromTexture:(id<MTLTexture>)
                                           renderer->ibl_brdf.metal_texture
                           sourceSlice:0
                           sourceLevel:0
                          sourceOrigin:MTLOriginMake(VKR_IBL_BRDF_SIZE / 2u,
                                                     VKR_IBL_BRDF_SIZE / 2u, 0)
                            sourceSize:MTLSizeMake(1, 1, 1)
                              toBuffer:(id<MTLBuffer>)readback_object
                     destinationOffset:readback_slice.offset +
                                       ibl_brdf_capture_offset
                destinationBytesPerRow:8
              destinationBytesPerImage:8
                               options:MTLBlitOptionNone];
    }
    for (uint32_t i = 0; passed && i < capture_count; ++i) {
      const VkrMetalPacketCapturePlan *plan = &capture_plans[i];
      [capture_encoder
                   copyFromTexture:plan->texture
                       sourceSlice:plan->source_slice
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(plan->result.width,
                                               plan->result.height, 1)
                          toBuffer:(id<MTLBuffer>)readback_object
                 destinationOffset:readback_slice.offset + plan->buffer_offset
            destinationBytesPerRow:plan->result.row_pitch
          destinationBytesPerImage:plan->result.data_size
                           options:MTLBlitOptionNone];
    }
  }
  [capture_encoder endEncoding];
  [renderer->command_buffer endCommandBuffer];
  if (!passed) {
    if (failed_pass) {
      log_error("Metal packet stage failed: encode pass '%.*s' executor '%.*s'",
                (int)failed_pass->desc.name.length, failed_pass->desc.name.str,
                (int)failed_pass->desc.execute_name.length,
                failed_pass->desc.execute_name.str);
    } else {
      log_error("Metal packet stage failed: final capture encoding");
    }
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_READBACK, readback_slice);
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_UPLOAD, frame_upload.slice);
    vkr_metal_packet_capture_fail_reserved(
        renderer, capture_request, VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED);
    vkr_metal_packet_finish_frame(renderer, true_v, false_v);
    return false_v;
  }

  const uint64_t submit_value = ++renderer->submit_value;
  VkrMetalPacketCommandSlot *command_slot = renderer->active_command_slot;
  command_slot->submit_value = submit_value;
  command_slot->timestamp_entry_count =
      (uint32_t)renderer->graph->passes.length * 2u;
  command_slot->timing_requested = timing_requested;
  id<MTL4CommandBuffer> submissions[] = {renderer->command_buffer};
  if (renderer->drawable)
    [renderer->queue waitForDrawable:renderer->drawable];
  [renderer->queue commit:submissions count:1];
  [renderer->queue signalEvent:renderer->completion value:submit_value];
  if (renderer->drawable) {
    [renderer->queue signalDrawable:renderer->drawable];
    [renderer->drawable present];
  }
  const bool8_t capture_submitted =
      capture_count == 0 ||
      vkr_metal_capture_ring_submit(
          &renderer->capture_ring, capture_request->request_id, submit_value,
          (const uint8_t *)readback.cpu_address + capture_base);
  const VkrMetalMemoryStatus readback_submitted =
      vkr_metal_memory_device_submit_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_READBACK,
                                          readback_slice, submit_value);
  const VkrMetalMemoryStatus upload_submitted =
      vkr_metal_memory_device_submit_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_UPLOAD,
                                          frame_upload.slice, submit_value);
  if (!capture_submitted || readback_submitted != VKR_METAL_MEMORY_STATUS_OK ||
      upload_submitted != VKR_METAL_MEMORY_STATUS_OK) {
    log_error("Metal packet stage failed: submit rings capture=%u readback=%s "
              "upload=%s",
              capture_submitted,
              vkr_metal_memory_status_string(readback_submitted),
              vkr_metal_memory_status_string(upload_submitted));
    if (vkr_metal_packet_wait(renderer, submit_value)) {
      (void)vkr_metal_packet_complete_command_slot(renderer, command_slot,
                                                   false_v);
      if (readback_submitted != VKR_METAL_MEMORY_STATUS_OK)
        vkr_metal_memory_device_cancel_ring(
            renderer->memory, VKR_METAL_RING_KIND_READBACK, readback_slice);
      if (upload_submitted != VKR_METAL_MEMORY_STATUS_OK)
        vkr_metal_memory_device_cancel_ring(
            renderer->memory, VKR_METAL_RING_KIND_UPLOAD, frame_upload.slice);
    }
    vkr_metal_packet_detach_active_command_slot(renderer);
    vkr_metal_packet_capture_fail_reserved(
        renderer, capture_request, VKR_RENDERER_ERROR_SUBMISSION_FAILED);
    vkr_metal_packet_finish_frame(renderer, true_v, true_v);
    return false_v;
  }
  if (renderer->synchronous_validation_readback) {
    if (!vkr_metal_packet_wait(renderer, submit_value)) {
      vkr_metal_packet_detach_active_command_slot(renderer);
      vkr_metal_packet_finish_frame(renderer, true_v, true_v);
      return false_v;
    }
    vkr_metal_capture_ring_collect(&renderer->capture_ring,
                                   renderer->completion.signaledValue);
    if (timing_requested)
      vkr_metal_packet_collect_pass_timings(
          renderer, command_slot->timestamp_heap,
          command_slot->timestamp_entry_count, out_result);

    MemCopy(out_result->color, readback.cpu_address, 4);
    MemCopy(&out_result->shadow_depth,
            (uint8_t *)readback.cpu_address + sizeof(uint32_t),
            sizeof(float32_t));
    out_result->has_shadow_depth = true_v;
    if (picking) {
      MemCopy(&out_result->picking_id,
              (uint8_t *)readback.cpu_address + sizeof(uint32_t) * 2,
              sizeof(uint32_t));
      out_result->has_picking_id = true_v;
    }
    if (ibl) {
      vkr_metal_packet_decode_half4((uint8_t *)readback.cpu_address +
                                        ibl_capture_offset,
                                    out_result->ibl_color);
      for (uint32_t mip = 0; mip < VKR_IBL_PREFILTER_MIP_COUNT; ++mip) {
        vkr_metal_packet_decode_half4((uint8_t *)readback.cpu_address +
                                          ibl_prefilter_capture_offset +
                                          mip * 8u,
                                      out_result->ibl_prefilter[mip]);
      }
      vkr_metal_packet_decode_half4((uint8_t *)readback.cpu_address +
                                        ibl_brdf_capture_offset,
                                    out_result->ibl_brdf);
      out_result->ibl_irradiance_size = VKR_IBL_IRRADIANCE_SIZE;
      out_result->ibl_prefilter_size = VKR_IBL_PREFILTER_SIZE;
      out_result->ibl_prefilter_mip_count = VKR_IBL_PREFILTER_MIP_COUNT;
      out_result->ibl_brdf_size = VKR_IBL_BRDF_SIZE;
      out_result->has_ibl_color = true_v;
      out_result->has_ibl_convolution = true_v;
    }
  }
  if (ibl_bake) {
    renderer->ibl_source = ibl_source;
    renderer->ibl_ready = true_v;
  }
  if (capture_count > 0) {
    (void)vkr_metal_capture_ring_poll(
        &renderer->capture_ring, capture_request->request_id,
        renderer->completion.signaledValue, &out_result->capture);
  }
  out_result->submit_value = submit_value;
  out_result->source_frame_index = packet->frame.frame_index;
  out_result->resize_count = renderer->resize_count;
  out_result->pipeline_count = 14;
  out_result->pipeline_archive_warm = renderer->pipeline_archive_warm;
  out_result->pipeline_archive_written = renderer->pipeline_archive_written;
  vkr_metal_memory_device_get_metrics(renderer->memory, &out_result->memory);
  vkr_metal_material_table_device_get_metrics(renderer->materials,
                                              &out_result->materials);
  if (renderer->synchronous_validation_readback) {
    command_slot->timing_collected = timing_requested;
    (void)vkr_metal_packet_complete_command_slot(renderer, command_slot,
                                                 false_v);
  } else if (timing_requested) {
    command_slot->timing_result = *out_result;
  }
  vkr_metal_packet_detach_active_command_slot(renderer);
  vkr_metal_packet_finish_frame(renderer, true_v, true_v);
  return true_v;
}

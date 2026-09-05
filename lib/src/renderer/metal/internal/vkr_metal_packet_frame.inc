/* Per-frame upload preparation, pass encoding, submission, and failure
 * rollback. */
vkr_internal void *vkr_metal_packet_next_root(VkrMetalPacketFrameUpload *upload,
                                              uint64_t size,
                                              uint64_t *out_gpu_address) {
  if (size > VKR_METAL_PACKET_DRAW_ROOT_STRIDE ||
      upload->root_cursor >= upload->root_capacity)
    return NULL;
  const uint64_t offset =
      (uint64_t)upload->root_cursor++ * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  *out_gpu_address = upload->root_gpu + offset;
  void *root = upload->root_cpu + offset;
  MemZero(root, size);
  return root;
}

vkr_internal VkrMetalPacketMesh *
vkr_metal_packet_resolve_mesh(VkrMetalPacketRenderer *renderer,
                              VkrMeshHandle handle) {
  if (handle.id == 0 || handle.id > renderer->max_meshes)
    return NULL;
  VkrMetalPacketMesh *mesh = &renderer->meshes[handle.id - 1];
  return mesh->live && mesh->generation == handle.generation ? mesh : NULL;
}

vkr_internal VkrMetalPacketMaterial *
vkr_metal_packet_resolve_material(VkrMetalPacketRenderer *renderer,
                                  VkrMaterialHandle handle) {
  if (handle.id == 0 || handle.id > renderer->max_materials)
    return NULL;
  VkrMetalPacketMaterial *material = &renderer->packet_materials[handle.id - 1];
  return material->live && material->generation == handle.generation ? material
                                                                     : NULL;
}

vkr_internal VkrMetalPacketTexture *
vkr_metal_packet_resolve_texture(VkrMetalPacketRenderer *renderer,
                                 VkrTextureHandle handle) {
  if (handle.id == 0 || handle.id > renderer->max_textures)
    return NULL;
  VkrMetalPacketTexture *texture = &renderer->textures[handle.id - 1];
  return texture->live && texture->generation == handle.generation ? texture
                                                                   : NULL;
}

vkr_internal void
vkr_metal_packet_abandon_ibl_sh_candidate(VkrMetalPacketRenderer *renderer,
                                          VkrMetalPacketFrameUpload *upload) {
  if (upload->ibl_sh_candidate_slot == VKR_SH_SLOT_BLACK)
    return;
  (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool,
                                upload->ibl_sh_candidate_slot);
  renderer->ibl_sh_slot = upload->ibl_sh_previous_slot;
  upload->ibl_sh_candidate_slot = VKR_SH_SLOT_BLACK;
}

vkr_internal bool8_t vkr_metal_packet_create_frame_root(
    VkrMetalPacketRenderer *renderer, const VkrRenderPacket *packet,
    VkrMetalPacketFrameUpload *upload, uint64_t instances_gpu,
    uint32_t target_width, uint32_t target_height, Mat4 view_projection,
    bool8_t lighting_pass, bool8_t transmission_pass, uint64_t *out_root_gpu,
    uint32_t *out_frame_flags) {
  VkrMetalPacketFrameRoot *frame_root =
      vkr_metal_packet_next_root(upload, sizeof(*frame_root), out_root_gpu);
  if (!frame_root)
    return false_v;
  const VkrPacketFrameConstants frame_constants =
      vkr_packet_derive_frame_constants(packet, target_width, target_height);
  *out_frame_flags =
      vkr_packet_derive_frame_flags(packet, lighting_pass, renderer->ibl_ready);
  *frame_root = (VkrMetalPacketFrameRoot){
      .instances = instances_gpu,
      .view_projection = vkr_metal_packet_slang_draw_matrix(view_projection),
      .materials =
          vkr_metal_material_table_device_gpu_address(renderer->materials),
      .sh_coefficients_address = renderer->sh_coefficients.gpuAddress,
      .prefilter_texture_id = renderer->ibl_prefilter.resource_id,
      .sh_global_slot = renderer->ibl_sh_slot,
      .view_position = frame_constants.view_position,
      .prefilter_mip_count = frame_constants.prefilter_mip_count,
      .flags = *out_frame_flags,
      .ibl_controls = frame_constants.ibl_controls,
      .directional_direction_enabled =
          frame_constants.directional_direction_enabled,
      .directional_color_intensity =
          frame_constants.directional_color_intensity,
      .ambient_color = frame_constants.ambient_color,
      .render_mode = frame_constants.render_mode,
      .shadow_debug_mode = frame_constants.shadow_debug_mode,
      .point_light_data = upload->point_light_data_gpu,
      .point_light_masks = upload->point_light_masks_gpu,
      .point_light_grid_origin_cell_size =
          frame_constants.point_light_grid_origin_cell_size,
      .point_light_grid_dimensions_count =
          {frame_constants.point_light_grid_dimensions_count[0],
           frame_constants.point_light_grid_dimensions_count[1],
           frame_constants.point_light_grid_dimensions_count[2],
           frame_constants.point_light_grid_dimensions_count[3]},
      .point_light_global_mask = frame_constants.point_light_global_mask,
      .point_light_count = frame_constants.point_light_count,
      .shadow_texture_id = upload->shadow_texture_id,
      .shadow_cascades = upload->shadow_cascades_gpu,
      .view = frame_constants.view,
      .shadow_cascade_count = frame_constants.shadow_cascade_count,
      .shadow_pcf_sample_count =
          frame_constants.shadow_receiver.pcf_sample_count,
      .shadow_receiver_bias_texels =
          frame_constants.shadow_receiver.receiver_bias_texels,
      .shadow_slope_bias_texels =
          frame_constants.shadow_receiver.slope_bias_texels,
      .shadow_normal_offset_texels =
          frame_constants.shadow_receiver.normal_offset_texels,
      .shadow_pcf_radius_texels =
          frame_constants.shadow_receiver.pcf_radius_texels,
      .shadow_cascade_blend_fraction =
          frame_constants.shadow_receiver.cascade_blend_fraction,
      .shadow_fade_start = frame_constants.shadow_receiver.fade_start,
      .shadow_fade_end = frame_constants.shadow_receiver.fade_end,
      .shadow_pcf_uniform_early_out =
          frame_constants.shadow_receiver.pcf_uniform_early_out,
      .transmission_texture_id = upload->transmission_texture_id,
      .ibl_probes = upload->ibl_probes_gpu,
      .ibl_probe_count =
          packet->lighting ? packet->lighting->ibl_probe_count : 0,
  };
  if (lighting_pass && !transmission_pass &&
      instances_gpu == upload->world_instances_gpu) {
    const uint32_t current_temporal_instance =
        (uint32_t)(renderer->active_command_slot - renderer->command_slots);
    const bool8_t history_valid =
        renderer->selected_temporal_history_instance != UINT32_MAX;
    VkrMetalPacketGraphBufferInstance *previous_transform =
        vkr_metal_packet_graph_buffer_instance(
            renderer, renderer->temporal_transform_history_handle,
            history_valid ? renderer->selected_temporal_history_instance
                          : current_temporal_instance);
    if (!previous_transform)
      return false_v;
    uint64_t temporal_gpu = 0u;
    VkrMetalPacketTemporalDrawState *temporal =
        vkr_metal_packet_next_root(upload, sizeof(*temporal), &temporal_gpu);
    if (!temporal)
      return false_v;
    *temporal = (VkrMetalPacketTemporalDrawState){
        .previous_transforms = previous_transform->resource.gpu_address,
        .current_view_projection = vkr_metal_packet_clip_matrix(
            renderer->convert_vulkan_clip_y,
            packet->globals.temporal.current_view_projection),
        .previous_view_projection =
            history_valid
                ? previous_transform->history_view_projection
                : vkr_metal_packet_clip_matrix(
                      renderer->convert_vulkan_clip_y,
                      packet->globals.temporal.current_view_projection),
        .history_valid = history_valid,
        .previous_frame_index = history_valid
                                    ? previous_transform->history_frame_index
                                    : packet->frame.frame_index,
    };
    frame_root->temporal_draw_state = temporal_gpu;
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_draws(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const VkrMetalPacketPreparedDraw *draws, uint32_t draw_count,
    uint64_t instances_gpu, uint32_t target_width, uint32_t target_height,
    Mat4 view_projection, const VkrRenderPacket *packet, bool8_t lighting_pass,
    bool8_t transmission_pass, VkrMetalPacketFrameUpload *upload,
    uint32_t *domain_draw_count, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  if (draw_count == 0)
    return true_v;
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  MTLCullMode active_cull_mode = MTLCullModeNone;
  [encoder setCullMode:active_cull_mode];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setDepthStencilState:depth_state];
  [encoder setRenderPipelineState:pipeline];
  uint64_t frame_root_gpu = 0;
  uint32_t frame_flags = 0u;
  if (!vkr_metal_packet_create_frame_root(
          renderer, packet, upload, instances_gpu, target_width, target_height,
          view_projection, lighting_pass, transmission_pass, &frame_root_gpu,
          &frame_flags)) {
    log_error("Metal frame root upload exhausted: cursor=%u capacity=%u",
              upload->root_cursor, upload->root_capacity);
    return false_v;
  }
  if (draw_count > upload->root_capacity - upload->root_cursor) {
    log_error("Metal direct draw root span exhausted: draws=%u cursor=%u "
              "capacity=%u",
              draw_count, upload->root_cursor, upload->root_capacity);
    return false_v;
  }
  /* Each pass owns a disjoint root span: a later picking/debug invocation
     cannot replace frame pointers already referenced by encoded blend draws. */
  const uint64_t root_offset =
      (uint64_t)upload->root_cursor * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  uint8_t *root_cpu = upload->root_cpu + root_offset;
  uint64_t root_gpu = upload->root_gpu + root_offset;
  upload->root_cursor += draw_count;
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrMetalPacketPreparedDraw *draw = &draws[i];
    if (draw->cull_mode != active_cull_mode) {
      [encoder setCullMode:draw->cull_mode];
      active_cull_mode = draw->cull_mode;
    }
    VkrMetalPacketTableDrawUpload *table_draw =
        (VkrMetalPacketTableDrawUpload *)root_cpu;
    *table_draw = (VkrMetalPacketTableDrawUpload){
        .root =
            {
                .geometry_rows =
                    root_gpu +
                    offsetof(VkrMetalPacketTableDrawUpload, geometry),
                .visible_rows =
                    root_gpu + offsetof(VkrMetalPacketTableDrawUpload, visible),
                .vertices = renderer->geometry_megabuffer.vertices.gpu_address,
                .frame = frame_root_gpu,
            },
        .geometry = draw->geometry,
        .visible = draw->visible,
    };
    [renderer->argument_table setAddress:root_gpu atIndex:0];
    [renderer->argument_table setAddress:root_gpu atIndex:1];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:draw->visible.index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:draw->index_address
                 indexBufferLength:draw->index_length
                     instanceCount:draw->instance_count
                        baseVertex:draw->visible.vertex_offset
                      baseInstance:0];
    root_cpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
    root_gpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  }
  if ((frame_flags & VKR_PACKET_FRAME_FLAG_IBL) != 0u)
    renderer->ibl_last_use_submit_value = renderer->submit_value + 1u;
  *domain_draw_count += draw_count;
  result->indexed_draw_count += draw_count;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_text_draws(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const VkrPreparedTextDraw *draws, uint32_t draw_count,
    uint32_t text_upload_start, uint32_t target_width, uint32_t target_height,
    Mat4 view_projection, bool8_t ui_domain, VkrMetalPacketFrameUpload *upload,
    uint32_t *domain_draw_count, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  if (draw_count == 0)
    return true_v;
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
        vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
    if (!root)
      return false_v;
    *root = (VkrMetalPacketTextRoot){
        .vertices = text_upload->vertices_gpu,
        .atlas_texture_id = atlas->resource.resource_id,
        .model = draw->model,
        .view_projection = view_projection,
        .controls = {draw->unit_range.x, draw->unit_range.y,
                     (float32_t)target_width, (float32_t)target_height},
        .object_id = draw->object_id,
        .flags = (ui_domain ? 1u : 0u) | (draw->font_mode != 0u ? 2u : 0u),
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

vkr_internal bool8_t vkr_metal_packet_encode_ui_draw_list(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const VkrPreparedUiDrawList *draw_list, uint32_t target_width,
    uint32_t target_height, VkrMetalPacketFrameUpload *upload,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  if (draw_list->batch_count == 0u)
    return true_v;
  if (upload->ui_vertices_gpu == 0u || upload->ui_indices_gpu == 0u ||
      upload->ui_indices_length == 0u)
    return false_v;

  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setDepthStencilState:nil];
  id<MTLRenderPipelineState> bound_pipeline = nil;
  for (uint32_t i = 0u; i < draw_list->batch_count; ++i) {
    const VkrUiDrawBatch *batch = &draw_list->batches[i];
    id<MTLRenderPipelineState> pipeline =
        batch->mode == VKR_UI_DRAW_MODE_ROUNDED_RECT
            ? renderer->ui_rect_pipeline
            : renderer->ui_pipeline;
    if (pipeline != bound_pipeline) {
      [encoder setRenderPipelineState:pipeline];
      bound_pipeline = pipeline;
    }

    VkrMetalPacketTexture *texture = NULL;
    if (batch->texture.id != 0u) {
      texture = vkr_metal_packet_resolve_texture(
          renderer,
          (VkrTextureHandle){.id = batch->texture.id,
                             .generation = batch->texture.generation});
      if (!texture)
        continue;
    }

    uint64_t root_gpu = 0u;
    VkrMetalPacketUiRoot *root =
        vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
    if (!root)
      return false_v;
    *root = (VkrMetalPacketUiRoot){
        .vertices = upload->ui_vertices_gpu,
        .texture_id = texture ? texture->resource.resource_id : 0u,
        .target_unit_range = {(float32_t)target_width, (float32_t)target_height,
                              batch->sdf_unit_range.x, batch->sdf_unit_range.y},
        .rect_extent = batch->rect_extent_px,
        .mode = batch->mode,
        .flags = texture ? 1u : 0u,
        .corner_radii = batch->corner_radius_px,
    };
    [encoder setScissorRect:(MTLScissorRect){
                                (NSUInteger)batch->scissor_rect_px.x,
                                (NSUInteger)batch->scissor_rect_px.y,
                                (NSUInteger)batch->scissor_rect_px.width,
                                (NSUInteger)batch->scissor_rect_px.height}];
    [renderer->argument_table setAddress:root_gpu atIndex:1u];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    const uint64_t index_offset =
        (uint64_t)batch->first_index * sizeof(uint32_t);
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:batch->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:upload->ui_indices_gpu + index_offset
                 indexBufferLength:upload->ui_indices_length - index_offset
                     instanceCount:1u
                        baseVertex:0
                      baseInstance:0u];
    if (texture)
      texture->last_use_submit_value = renderer->submit_value + 1u;
    result->ui_draw_count++;
    result->indexed_draw_count++;
  }
  return true_v;
}

/** Resolves the texture behind one authored image use, honouring its mip. */
vkr_internal id<MTLTexture>
vkr_metal_packet_subresource_texture(VkrMetalPacketRenderer *renderer,
                                     const VkrRgImageUse *use)
    API_AVAILABLE(macos(26.0)) {
  if (!use)
    return nil;
  return use->has_slice ? vkr_metal_packet_mip_texture(renderer, use->image,
                                                       use->slice.mip_level)
                        : vkr_metal_packet_texture(renderer, use->image);
}

vkr_internal bool8_t vkr_metal_packet_encode_tonemap(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4RenderCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload, uint32_t target_width,
    uint32_t target_height, const VkrEditorPassPayload *editor)
    API_AVAILABLE(macos(26.0)) {
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 2u, 0u);
  if (!read)
    read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  id<MTLTexture> source = vkr_metal_packet_subresource_texture(renderer, read);
  if (!source)
    return false_v;
  uint64_t root_gpu = 0;
  VkrMetalPacketTonemapRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  uint64_t manual_state_gpu = 0u;
  VkrExposureGpuState *manual_state = vkr_metal_packet_next_root(
      upload, sizeof(*manual_state), &manual_state_gpu);
  if (!root || !manual_state)
    return false_v;
  *manual_state = (VkrExposureGpuState){
      .exposure_multiplier = packet->globals.exposure.manual,
  };
  const uint32_t output_x = editor ? (uint32_t)editor->image_rect_px.x : 0u;
  const uint32_t output_y = editor ? (uint32_t)editor->image_rect_px.y : 0u;
  const uint32_t output_width =
      editor ? (uint32_t)editor->image_rect_px.z : target_width;
  const uint32_t output_height =
      editor ? (uint32_t)editor->image_rect_px.w : target_height;
  *root = (VkrMetalPacketTonemapRoot){
      .source_texture_id = source.gpuResourceID._impl,
      .reserved = {renderer->tonemap_enabled ? 1u : 0u,
                   renderer->fxaa_enabled && !renderer->metalfx_enabled ? 1u
                                                                        : 0u},
      .exposure_state = manual_state_gpu,
      .output_extent = {output_width, output_height},
  };
  const VkrRgBufferUse *exposure_use =
      vkr_rg_pass_find_buffer_use(&pass->desc, 1u, 0u);
  VkrMetalPacketGraphBufferInstance *exposure_state =
      exposure_use
          ? vkr_metal_packet_graph_buffer(renderer, exposure_use->buffer)
          : NULL;
  if (exposure_state)
    root->exposure_state = exposure_state->resource.gpu_address;
  [renderer->argument_table setAddress:root_gpu atIndex:1];
  [encoder setViewport:(MTLViewport){output_x, output_y, output_width,
                                     output_height, 0.0, 1.0}];
  [encoder setScissorRect:(MTLScissorRect){output_x, output_y, output_width,
                                           output_height}];
  [encoder setRenderPipelineState:renderer->tonemap_pipeline];
  [encoder setArgumentTable:renderer->argument_table
                   atStages:MTLRenderStageFragment];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_ibl(
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
  for (uint32_t mip = 0; mip < VKR_IBL_PREFILTER_MIP_COUNT; ++mip) {
    VkrMetalPacketPrefilterRoot *prefilter_root =
        vkr_metal_packet_next_root(upload, sizeof(*prefilter_root), &root_gpu);
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

  /* Asset-published scene sources already own coefficients. The packet path
     projects only a skybox-only source that reaches submission without that
     cold publication step. */
  if (source->ibl_sh_slot == VKR_SH_SLOT_BLACK) {
    uint32_t projection_mip = 0u;
    uint32_t projection_extent = 0u;
    if (!vkr_ibl_sh_projection_mip((uint32_t)source_texture.width,
                                   (uint32_t)source_texture.mipmapLevelCount,
                                   &projection_mip, &projection_extent)) {
      log_error("Metal packet IBL source cannot supply an SH projection mip; "
                "the previous coefficients remain published");
    } else {
      uint32_t candidate_slot = VKR_SH_SLOT_BLACK;
      if (vkr_ibl_sh_pool_reserve(&renderer->sh_pool, &candidate_slot) !=
          VKR_SH_POOL_STATUS_OK) {
        log_error("Metal SH coefficient pool exhausted; the previous "
                  "coefficients remain published");
      } else {
        VkrMetalPacketShProjectRoot *sh_root =
            vkr_metal_packet_next_root(upload, sizeof(*sh_root), &root_gpu);
        if (!sh_root) {
          (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool, candidate_slot);
          return false_v;
        }
        *sh_root = (VkrMetalPacketShProjectRoot){
            .source_texture_id = source->resource.resource_id,
            .destination = renderer->sh_coefficients.gpuAddress +
                           (uint64_t)candidate_slot * VKR_SH_SLOT_BYTES,
            .source_face_size = projection_extent,
            .source_mip = projection_mip,
            /* Packet-only IBL sources carry no scene authoring metadata, so
               the contract default is the identity window. */
            .window_band_0 = 1.0f,
            .window_band_1 = 1.0f,
            .window_band_2 = 1.0f,
        };
        [renderer->argument_table setAddress:root_gpu atIndex:0];
        [encoder setComputePipelineState:renderer->ibl_sh_pipeline];
        [encoder setArgumentTable:renderer->argument_table];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder barrierAfterEncoderStages:MTLStageDispatch
                       beforeEncoderStages:(MTLStages)(MTLStageDispatch |
                                                       MTLStageFragment)
                         visibilityOptions:MTL4VisibilityOptionDevice];
        if (vkr_ibl_sh_pool_mark_recorded(&renderer->sh_pool, candidate_slot) !=
            VKR_SH_POOL_STATUS_OK) {
          (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool, candidate_slot);
          return false_v;
        }
        upload->ibl_sh_previous_slot = renderer->ibl_sh_slot;
        upload->ibl_sh_candidate_slot = candidate_slot;
        renderer->ibl_sh_slot = candidate_slot;
      }
    }
  }

  const uint64_t pending_submit = renderer->submit_value + 1;
  source->last_use_submit_value = pending_submit;
  renderer->ibl_last_use_submit_value = pending_submit;
  return true_v;
}

vkr_internal VkrMetalPacketGraphBufferInstance *
vkr_metal_packet_pass_buffer(VkrMetalPacketRenderer *renderer,
                             const VkrRgPass *pass, uint32_t binding) {
  const VkrRgBufferUse *use =
      pass ? vkr_rg_pass_find_buffer_use(&pass->desc, binding, 0u) : NULL;
  return use ? vkr_metal_packet_graph_buffer(renderer, use->buffer) : NULL;
}

vkr_internal uint64_t vkr_metal_packet_hash_bytes(uint64_t hash,
                                                  const void *bytes,
                                                  uint64_t byte_count) {
  const uint8_t *data = bytes;
  for (uint64_t i = 0u; i < byte_count; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

vkr_internal uint64_t
vkr_metal_packet_world_occluder_epoch(const VkrWorldPassPayload *world) {
  if (!world)
    return 0u;
  uint64_t hash = 1469598103934665603ull;
  hash = vkr_metal_packet_hash_bytes(hash, &world->static_generation,
                                     sizeof(world->static_generation));
  hash = vkr_metal_packet_hash_bytes(hash, &world->dynamic_generation,
                                     sizeof(world->dynamic_generation));
  hash = vkr_metal_packet_hash_bytes(hash, &world->publication_generation,
                                     sizeof(world->publication_generation));
  hash = vkr_metal_packet_hash_bytes(hash, &world->gpu_candidate_count,
                                     sizeof(world->gpu_candidate_count));
  hash = vkr_metal_packet_hash_bytes(hash, &world->static_candidate_count,
                                     sizeof(world->static_candidate_count));
  hash = vkr_metal_packet_hash_bytes(
      hash, &world->gpu_camera_opaque_candidate_count,
      sizeof(world->gpu_camera_opaque_candidate_count));
  return hash;
}

vkr_internal id<MTLTexture>
vkr_metal_packet_pass_texture(VkrMetalPacketRenderer *renderer,
                              const VkrRgPass *pass, uint32_t binding) {
  const VkrRgImageUse *use =
      pass ? vkr_rg_pass_find_image_use(&pass->desc, binding, 0u) : NULL;
  if (!use)
    return nil;
  VkrMetalPacketImageInstance *instance =
      vkr_metal_packet_image_instance(renderer, use->image);
  if (!instance)
    return nil;
  if (use->has_slice && use->slice.mip_level == 0u &&
      use->slice.mip_count == 1u && use->slice.layer_count == 1u &&
      use->slice.base_layer < VKR_METAL_PACKET_MAX_TEXTURE_LAYERS &&
      instance->views.layer_views[use->slice.base_layer])
    return (id<MTLTexture>)instance->views.layer_views[use->slice.base_layer];
  return (id<MTLTexture>)instance->resource.metal_texture;
}

vkr_internal VkrMetalPacketGpuDrawRoot *
vkr_metal_packet_next_gpu_draw_root(VkrMetalPacketFrameUpload *upload,
                                    uint64_t *out_gpu_address) {
  return vkr_metal_packet_next_root(upload, sizeof(VkrMetalPacketGpuDrawRoot),
                                    out_gpu_address);
}

vkr_internal bool8_t vkr_metal_packet_encode_gpu_draw_compute(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  if (!renderer || !pass || !packet || !encoder || !upload)
    return false_v;
  VkrMetalPacketGraphExecutorKind kind;
  if (!vkr_metal_packet_graph_executor_kind(pass, &kind))
    return false_v;
  const bool8_t classify =
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY ||
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY;
  const bool8_t prefix =
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_PREFIX ||
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX;
  const bool8_t encode =
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_ENCODE ||
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE;
  if (!classify && !prefix && !encode)
    return false_v;
  const bool8_t transmission =
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY ||
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX ||
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE;
  const uint32_t candidate_count = transmission
                                       ? renderer->transmission_gpu_draw_count
                                       : renderer->gpu_draw_count;
  const uint64_t instances_gpu =
      transmission ? upload->transmission_gpu_draw_instances_gpu
                   : upload->gpu_draw_instances_gpu;
  const uint32_t view_count =
      transmission ? 1u : 1u + renderer->prepared_frame.shadow_cascade_count;
  if (candidate_count == 0u)
    return true_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketGpuDrawRoot *root =
      vkr_metal_packet_next_gpu_draw_root(upload, &root_gpu);
  if (!root)
    return false_v;
  root->candidate_count = candidate_count;
  root->visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY;
  root->view_count = view_count;
  root->icb_view_group_size =
      transmission ? 1u : renderer->gpu_draw_icb_view_group_size;
  root->views = transmission ? upload->transmission_gpu_draw_view_gpu
                             : upload->gpu_draw_views_gpu;
  if (!root->views)
    return false_v;

  if (classify) {
    VkrMetalPacketGraphBufferInstance *candidates =
        vkr_metal_packet_pass_buffer(renderer, pass, 0u);
    VkrMetalPacketGraphBufferInstance *classifications =
        vkr_metal_packet_pass_buffer(renderer, pass, 1u);
    VkrMetalPacketGraphBufferInstance *state =
        vkr_metal_packet_pass_buffer(renderer, pass, 2u);
    if (!candidates || !classifications || !state)
      return false_v;
    root->candidates = candidates->resource.gpu_address;
    root->instances = instances_gpu;
    root->classifications = classifications->resource.gpu_address;
    root->compaction_state = state->resource.gpu_address;
    id<MTLTexture> hzb = renderer->prepared_frame.hzb_history_valid
                             ? vkr_metal_packet_selected_hzb_texture(renderer)
                             : nil;
    root->hzb_texture_id = hzb ? hzb.gpuResourceID._impl : 0u;
    root->history_view_projection = renderer->current_hzb_view_projection;
    root->hzb_extent[0] = renderer->prepared_frame.viewport_width;
    root->hzb_extent[1] = renderer->prepared_frame.viewport_height;
    root->hzb_mip_count = renderer->prepared_frame.hzb_reduce_pass_count + 1u;
    root->hzb_enabled = hzb ? 1u : 0u;
    // Keep the comparison conservative across projected-sphere and depth-buffer
    // quantization; a smaller tolerance self-rejected static fixture geometry.
    root->hzb_depth_epsilon = 1e-3f;
    [renderer->argument_table setAddress:root_gpu atIndex:0u];
    [encoder setComputePipelineState:renderer->gpu_draw_classify_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(candidate_count, view_count, 1u)
        threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
    return true_v;
  }

  if (prefix) {
    VkrMetalPacketGraphBufferInstance *state =
        vkr_metal_packet_pass_buffer(renderer, pass, 0u);
    if (!state)
      return false_v;
    root->compaction_state = state->resource.gpu_address;
    [renderer->argument_table setAddress:root_gpu atIndex:0u];
    [encoder setComputePipelineState:renderer->gpu_draw_prefix_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(view_count, 1u, 1u)
        threadsPerThreadgroup:MTLSizeMake(view_count, 1u, 1u)];
    return true_v;
  }

  if (!encode)
    return false_v;
  VkrMetalPacketGraphBufferInstance *candidates =
      vkr_metal_packet_pass_buffer(renderer, pass, 0u);
  VkrMetalPacketGraphBufferInstance *classifications =
      vkr_metal_packet_pass_buffer(renderer, pass, 1u);
  VkrMetalPacketGraphBufferInstance *visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 3u);
  if (!candidates || !classifications || !visible || !state ||
      !(transmission ? renderer->active_command_slot->transmission_gpu_draw_icb
                     : renderer->active_command_slot->gpu_draw_icbs[0]))
    return false_v;

  uint64_t draw_roots_gpu = 0u;
  VkrMetalPacketDrawRoot *draw_roots = vkr_metal_packet_next_root(
      upload, (uint64_t)view_count * sizeof(*draw_roots), &draw_roots_gpu);
  if (!draw_roots)
    return false_v;
  uint32_t frame_flags = 0u;
  for (uint32_t view = 0u; view < view_count; ++view) {
    const bool8_t camera_view = view == 0u;
    const Mat4 view_projection =
        camera_view
            ? vkr_metal_packet_clip_matrix(
                  renderer->convert_vulkan_clip_y,
                  mat4_mul(packet->globals.temporal.jittered_projection,
                           packet->globals.view))
            : vkr_metal_packet_clip_matrix(
                  renderer->convert_vulkan_clip_y,
                  packet->shadow->cascades[view - 1u].light_view_projection);
    uint64_t frame_root_gpu = 0u;
    uint32_t view_frame_flags = 0u;
    if (!vkr_metal_packet_create_frame_root(
            renderer, packet, upload, instances_gpu,
            camera_view ? renderer->prepared_frame.viewport_width
                        : renderer->prepared_frame.shadow_map_size,
            camera_view ? renderer->prepared_frame.viewport_height
                        : renderer->prepared_frame.shadow_map_size,
            view_projection, camera_view, false_v, &frame_root_gpu,
            &view_frame_flags))
      return false_v;
    frame_flags |= view_frame_flags;
    draw_roots[view] = (VkrMetalPacketDrawRoot){
        .geometry_rows = upload->gpu_draw_geometry_rows_gpu,
        .visible_rows = visible->resource.gpu_address +
                        (uint64_t)view * VKR_GPU_DRAW_CANDIDATE_CAPACITY *
                            sizeof(VkrGpuVisibleDrawRow),
        .vertices = renderer->geometry_megabuffer.vertices.gpu_address,
        .frame = frame_root_gpu,
        .flags = 1u,
    };
  }
  root->candidates = candidates->resource.gpu_address;
  root->geometry_rows = upload->gpu_draw_geometry_rows_gpu;
  root->instances = instances_gpu;
  root->classifications = classifications->resource.gpu_address;
  root->compaction_state = state->resource.gpu_address;
  root->visible_rows = visible->resource.gpu_address;
  root->draw_roots = draw_roots_gpu;
  if (transmission)
    upload->transmission_gpu_draw_root_gpu = draw_roots_gpu;
  [encoder
      setComputePipelineState:transmission
                                  ? renderer->gpu_draw_encode_inherited_pipeline
                                  : renderer->gpu_draw_encode_pipeline];
  const uint32_t encode_group_size = root->icb_view_group_size;
  const uint32_t encode_group_count =
      (view_count + encode_group_size - 1u) / encode_group_size;
  for (uint32_t group = 0u; group < encode_group_count; ++group) {
    const uint32_t first_view = group * encode_group_size;
    const uint32_t group_view_count =
        MIN(encode_group_size, view_count - first_view);
    uint64_t view_root_gpu = root_gpu;
    VkrMetalPacketGpuDrawRoot *view_root = root;
    if (group > 0u) {
      view_root = vkr_metal_packet_next_gpu_draw_root(upload, &view_root_gpu);
      if (!view_root)
        return false_v;
      *view_root = *root;
    }
    view_root->encode_view_index = first_view;
    id<MTLIndirectCommandBuffer> icb =
        transmission ? renderer->active_command_slot->transmission_gpu_draw_icb
                     : renderer->active_command_slot->gpu_draw_icbs[group];
    if (!icb)
      return false_v;
    for (uint32_t group_view = 0u; group_view < group_view_count; ++group_view)
      [encoder
          resetCommandsInBuffer:icb
                      withRange:NSMakeRange((uint64_t)group_view *
                                                VKR_GPU_DRAW_CANDIDATE_CAPACITY,
                                            candidate_count)];
    const uint64_t icb_argument_gpu =
        transmission
            ? upload->transmission_gpu_draw_icb_argument_gpu
            : upload->gpu_draw_icb_argument_gpu +
                  (uint64_t)group * upload->gpu_draw_icb_argument_stride;
    [renderer->argument_table setAddress:view_root_gpu atIndex:0u];
    [renderer->argument_table setAddress:icb_argument_gpu atIndex:1u];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(candidate_count, group_view_count, 1u)
        threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
  }
  if ((frame_flags & VKR_PACKET_FRAME_FLAG_IBL) != 0u)
    renderer->ibl_last_use_submit_value = renderer->submit_value + 1u;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_temporal_transform(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketGraphBufferInstance *instances =
      vkr_metal_packet_pass_buffer(renderer, pass, 0u);
  VkrMetalPacketGraphBufferInstance *transforms =
      vkr_metal_packet_pass_buffer(renderer, pass, 1u);
  if (!instances || !transforms || !renderer->temporal_transform_pipeline)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketTemporalTransformRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketTemporalTransformRoot){
      .instances = instances->resource.gpu_address,
      .transforms = transforms->resource.gpu_address,
      .instance_count = renderer->gpu_draw_count,
      .transform_capacity = VKR_TEMPORAL_TRANSFORM_CAPACITY,
      .frame_index = packet->frame.frame_index,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->temporal_transform_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  if (root->instance_count)
    [encoder dispatchThreads:MTLSizeMake(root->instance_count, 1u, 1u)
        threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_gbuffer_resolve(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  if (!renderer->gbuffer_resolve_pipeline)
    return false_v;
  id<MTLTexture> vbuffer = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> albedo = vkr_metal_packet_pass_texture(renderer, pass, 2u);
  id<MTLTexture> specular = vkr_metal_packet_pass_texture(renderer, pass, 3u);
  id<MTLTexture> normal = vkr_metal_packet_pass_texture(renderer, pass, 4u);
  id<MTLTexture> emissive = vkr_metal_packet_pass_texture(renderer, pass, 5u);
  id<MTLTexture> debug = vkr_metal_packet_pass_texture(renderer, pass, 6u);
  id<MTLTexture> hdr_seed = vkr_metal_packet_pass_texture(renderer, pass, 8u);
  id<MTLTexture> motion = vkr_metal_packet_pass_texture(renderer, pass, 11u);
  id<MTLTexture> validity = vkr_metal_packet_pass_texture(renderer, pass, 12u);
  VkrMetalPacketGraphBufferInstance *visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 1u);
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 7u);
  VkrMetalPacketGraphBufferInstance *previous_transform =
      renderer->selected_temporal_history_instance != UINT32_MAX
          ? vkr_metal_packet_graph_buffer_instance(
                renderer, renderer->temporal_transform_history_handle,
                renderer->selected_temporal_history_instance)
          : NULL;
  if (!vbuffer || !albedo || !specular || !normal || !emissive || !debug ||
      !hdr_seed || !motion || !validity || !visible || !state ||
      !vkr_metal_packet_pass_buffer(renderer, pass, 10u) ||
      vbuffer.width == 0u || vbuffer.height == 0u ||
      vbuffer.width > UINT32_MAX || vbuffer.height > UINT32_MAX)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketGBufferResolveRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketGBufferResolveRoot){
      .visible_rows = visible->resource.gpu_address,
      .geometry_rows = upload->gpu_draw_geometry_rows_gpu,
      .instances = upload->gpu_draw_instances_gpu,
      .materials =
          vkr_metal_material_table_device_gpu_address(renderer->materials),
      .compaction_state = state->resource.gpu_address,
      .vbuffer_texture_id = vbuffer.gpuResourceID._impl,
      .albedo_texture_id = albedo.gpuResourceID._impl,
      .specular_texture_id = specular.gpuResourceID._impl,
      .normal_texture_id = normal.gpuResourceID._impl,
      .emissive_texture_id = emissive.gpuResourceID._impl,
      .debug_texture_id = debug.gpuResourceID._impl,
      .hdr_seed_texture_id = hdr_seed.gpuResourceID._impl,
      .view_projection = vkr_metal_packet_clip_matrix(
          renderer->convert_vulkan_clip_y,
          mat4_mul(packet->globals.temporal.jittered_projection,
                   packet->globals.view)),
      .current_view_projection = vkr_metal_packet_clip_matrix(
          renderer->convert_vulkan_clip_y,
          packet->globals.temporal.current_view_projection),
      .previous_view_projection =
          previous_transform
              ? previous_transform->history_view_projection
              : vkr_metal_packet_clip_matrix(
                    renderer->convert_vulkan_clip_y,
                    packet->globals.temporal.current_view_projection),
      .previous_transforms =
          previous_transform ? previous_transform->resource.gpu_address : 0u,
      .motion_texture_id = motion.gpuResourceID._impl,
      .validity_texture_id = validity.gpuResourceID._impl,
      .extent = {(uint32_t)vbuffer.width, (uint32_t)vbuffer.height},
      .visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .geometry_count = renderer->max_meshes,
      .material_count = renderer->max_materials,
      .instance_count = renderer->gpu_draw_count,
      .render_mode = packet->globals.render_mode,
      .history_valid = previous_transform != NULL,
      .previous_frame_index = previous_transform
                                  ? previous_transform->history_frame_index
                                  : packet->frame.frame_index,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->gbuffer_resolve_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(vbuffer.width, vbuffer.height, 1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_gtao(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    id<MTL4ComputeCommandEncoder> encoder, VkrMetalPacketFrameUpload *upload,
    VkrMetalPacketGraphExecutorKind kind) API_AVAILABLE(macos(26.0)) {
  if (!renderer->prepared_frame.gtao_enabled)
    return false_v;

  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_GTAO_DEPTH_PREFILTER ||
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_GTAO_DEPTH_MIP) {
    const VkrRgImageUse *source_use =
        vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
    const VkrRgImageUse *destination_use =
        vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
    id<MTLTexture> source =
        vkr_metal_packet_subresource_texture(renderer, source_use);
    id<MTLTexture> destination =
        vkr_metal_packet_subresource_texture(renderer, destination_use);
    id<MTLComputePipelineState> pipeline =
        kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_GTAO_DEPTH_PREFILTER
            ? renderer->gtao_depth_prefilter_pipeline
            : renderer->gtao_depth_mip_pipeline;
    if (!source || !destination || !pipeline || source.width == 0u ||
        source.height == 0u || destination.width == 0u ||
        destination.height == 0u || source.width > UINT32_MAX ||
        source.height > UINT32_MAX || destination.width > UINT32_MAX ||
        destination.height > UINT32_MAX)
      return false_v;

    uint64_t root_gpu = 0u;
    VkrMetalPacketGtaoDepthRoot *root =
        vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
    if (!root)
      return false_v;
    *root = (VkrMetalPacketGtaoDepthRoot){
        .params = renderer->gtao_params,
        .source_texture_id = source.gpuResourceID._impl,
        .destination_texture_id = destination.gpuResourceID._impl,
        .source_extent = {(uint32_t)source.width, (uint32_t)source.height},
        .destination_extent = {(uint32_t)destination.width,
                               (uint32_t)destination.height},
    };
    [renderer->argument_table setAddress:root_gpu atIndex:0u];
    [encoder setComputePipelineState:pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(destination.width, destination.height,
                                         1u)
        threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
    return true_v;
  }

  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_GTAO_EVALUATE) {
    id<MTLTexture> vbuffer = vkr_metal_packet_pass_texture(renderer, pass, 0u);
    id<MTLTexture> view_depth =
        vkr_metal_packet_pass_texture(renderer, pass, 1u);
    id<MTLTexture> normal = vkr_metal_packet_pass_texture(renderer, pass, 2u);
    id<MTLTexture> destination =
        vkr_metal_packet_pass_texture(renderer, pass, 3u);
    id<MTLTexture> edges = vkr_metal_packet_pass_texture(renderer, pass, 4u);
    if (!vbuffer || !view_depth || !normal || !destination || !edges ||
        !renderer->gtao_evaluate_pipeline || view_depth.width == 0u ||
        view_depth.height == 0u || destination.width == 0u ||
        destination.height == 0u || view_depth.width > UINT32_MAX ||
        view_depth.height > UINT32_MAX || destination.width > UINT32_MAX ||
        destination.height > UINT32_MAX)
      return false_v;

    uint64_t root_gpu = 0u;
    VkrMetalPacketGtaoEvaluateRoot *root =
        vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
    if (!root)
      return false_v;
    *root = (VkrMetalPacketGtaoEvaluateRoot){
        .params = renderer->gtao_params,
        .vbuffer_texture_id = vbuffer.gpuResourceID._impl,
        .view_depth_texture_id = view_depth.gpuResourceID._impl,
        .normal_texture_id = normal.gpuResourceID._impl,
        .destination_texture_id = destination.gpuResourceID._impl,
        .edges_texture_id = edges.gpuResourceID._impl,
        .source_extent = {(uint32_t)view_depth.width,
                          (uint32_t)view_depth.height},
        .destination_extent = {(uint32_t)destination.width,
                               (uint32_t)destination.height},
    };
    [renderer->argument_table setAddress:root_gpu atIndex:0u];
    [encoder setComputePipelineState:renderer->gtao_evaluate_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(destination.width, destination.height,
                                         1u)
        threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
    return true_v;
  }

  if (kind != VKR_METAL_PACKET_GRAPH_EXECUTOR_GTAO_DENOISE)
    return false_v;
  id<MTLTexture> source = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> edges = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  id<MTLTexture> destination =
      vkr_metal_packet_pass_texture(renderer, pass, 2u);
  if (!source || !edges || !destination || !renderer->gtao_denoise_pipeline ||
      source.width == 0u || source.height == 0u || destination.width == 0u ||
      destination.height == 0u || source.width > UINT32_MAX ||
      source.height > UINT32_MAX || destination.width > UINT32_MAX ||
      destination.height > UINT32_MAX)
    return false_v;

  uint64_t root_gpu = 0u;
  VkrMetalPacketGtaoDenoiseRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketGtaoDenoiseRoot){
      .params = renderer->gtao_params,
      .source_texture_id = source.gpuResourceID._impl,
      .edges_texture_id = edges.gpuResourceID._impl,
      .destination_texture_id = destination.gpuResourceID._impl,
      .source_extent = {(uint32_t)source.width, (uint32_t)source.height},
      .destination_extent = {(uint32_t)destination.width,
                             (uint32_t)destination.height},
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->gtao_denoise_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(destination.width, destination.height,
                                       1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_deferred_lighting(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  if (!renderer->deferred_lighting_pipeline)
    return false_v;
  id<MTLTexture> vbuffer = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  id<MTLTexture> albedo = vkr_metal_packet_pass_texture(renderer, pass, 2u);
  id<MTLTexture> specular = vkr_metal_packet_pass_texture(renderer, pass, 3u);
  id<MTLTexture> normal = vkr_metal_packet_pass_texture(renderer, pass, 4u);
  id<MTLTexture> hdr = vkr_metal_packet_pass_texture(renderer, pass, 6u);
  id<MTLTexture> gtao_visibility =
      renderer->prepared_frame.gtao_enabled
          ? vkr_metal_packet_pass_texture(renderer, pass, 7u)
          : (id<MTLTexture>)renderer->gtao_white_visibility.metal_texture;
  if (!vbuffer || !depth || !albedo || !specular || !normal || !hdr ||
      !gtao_visibility || !renderer->gtao_white_live || vbuffer.width == 0u ||
      vbuffer.height == 0u || vbuffer.width > UINT32_MAX ||
      vbuffer.height > UINT32_MAX)
    return false_v;
  VkrMetalPacketTexture *sky =
      packet->skybox
          ? vkr_metal_packet_resolve_texture(renderer, packet->skybox->cubemap)
          : NULL;
  if (packet->skybox && !sky)
    return false_v;

  uint64_t frame_root_gpu = 0u;
  uint32_t frame_flags = 0u;
  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->globals.temporal.jittered_projection,
               packet->globals.view));
  if (!vkr_metal_packet_create_frame_root(
          renderer, packet, upload, upload->gpu_draw_instances_gpu,
          (uint32_t)vbuffer.width, (uint32_t)vbuffer.height, view_projection,
          true_v, false_v, &frame_root_gpu, &frame_flags))
    return false_v;

  uint64_t root_gpu = 0u;
  VkrMetalPacketDeferredLightingRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketDeferredLightingRoot){
      .frame = frame_root_gpu,
      .vbuffer_texture_id = vbuffer.gpuResourceID._impl,
      .depth_texture_id = depth.gpuResourceID._impl,
      .albedo_texture_id = albedo.gpuResourceID._impl,
      .specular_texture_id = specular.gpuResourceID._impl,
      .normal_texture_id = normal.gpuResourceID._impl,
      .hdr_texture_id = hdr.gpuResourceID._impl,
      .sky_texture_id = sky ? sky->resource.resource_id : 0u,
      .gtao_visibility_texture_id = gtao_visibility.gpuResourceID._impl,
      .inverse_view_projection = mat4_inverse(view_projection),
      .extent = {(uint32_t)vbuffer.width, (uint32_t)vbuffer.height},
      .sky_enabled = sky ? 1u : 0u,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->deferred_lighting_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(vbuffer.width, vbuffer.height, 1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  const uint64_t pending_submit = renderer->submit_value + 1u;
  if ((frame_flags & VKR_PACKET_FRAME_FLAG_IBL) != 0u)
    renderer->ibl_last_use_submit_value = pending_submit;
  if (sky)
    sky->last_use_submit_value = pending_submit;
  if (!renderer->prepared_frame.gtao_enabled)
    renderer->gtao_white_last_use_submit_value = pending_submit;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_temporal_resolve(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> scene = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> pre_transmission =
      vkr_metal_packet_pass_texture(renderer, pass, 1u);
  id<MTLTexture> motion = vkr_metal_packet_pass_texture(renderer, pass, 2u);
  id<MTLTexture> validity = vkr_metal_packet_pass_texture(renderer, pass, 3u);
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 4u);
  id<MTLTexture> vbuffer = vkr_metal_packet_pass_texture(renderer, pass, 5u);
  id<MTLTexture> output_color =
      vkr_metal_packet_pass_texture(renderer, pass, 8u);
  id<MTLTexture> output_depth =
      vkr_metal_packet_pass_texture(renderer, pass, 9u);
  id<MTLTexture> output_identity =
      vkr_metal_packet_pass_texture(renderer, pass, 10u);
  id<MTLTexture> output_primitive =
      vkr_metal_packet_pass_texture(renderer, pass, 11u);
  VkrMetalPacketGraphBufferInstance *visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 6u);
  VkrMetalPacketGraphBufferInstance *instances =
      vkr_metal_packet_pass_buffer(renderer, pass, 7u);
  id<MTLTexture> transmission_vbuffer =
      vkr_metal_packet_pass_texture(renderer, pass, 12u);
  id<MTLTexture> transmission_depth =
      vkr_metal_packet_pass_texture(renderer, pass, 15u);
  VkrMetalPacketGraphBufferInstance *transmission_visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 13u);
  VkrMetalPacketGraphBufferInstance *transmission_instances =
      vkr_metal_packet_pass_buffer(renderer, pass, 14u);
  const bool8_t transmission_enabled =
      transmission_vbuffer && transmission_depth && transmission_visible &&
      transmission_instances;
  if (!scene || !pre_transmission || !motion || !validity || !depth ||
      !vbuffer || !output_color || !output_depth || !output_identity ||
      !output_primitive || !visible || !instances ||
      !renderer->temporal_resolve_pipeline)
    return false_v;

  const uint32_t selected = renderer->selected_temporal_history_instance;
  VkrMetalPacketImage *color_slot =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_color");
  VkrMetalPacketImage *depth_slot =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_depth");
  VkrMetalPacketImage *identity_slot =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_identity");
  VkrMetalPacketImage *primitive_slot =
      vkr_metal_packet_named_image_slot(renderer, "temporal_history_primitive");
  const bool8_t history_valid = selected != UINT32_MAX && color_slot &&
                                depth_slot && identity_slot && primitive_slot &&
                                selected < color_slot->instance_count &&
                                selected < depth_slot->instance_count &&
                                selected < identity_slot->instance_count &&
                                selected < primitive_slot->instance_count;
  const Mat4 current_history_view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      packet->globals.temporal.current_view_projection);
  id<MTLTexture> history_color =
      history_valid ? (id<MTLTexture>)color_slot->instances[selected]
                          .resource.metal_texture
                    : scene;
  id<MTLTexture> history_depth =
      history_valid ? (id<MTLTexture>)depth_slot->instances[selected]
                          .resource.metal_texture
                    : output_depth;
  id<MTLTexture> history_identity =
      history_valid ? (id<MTLTexture>)identity_slot->instances[selected]
                          .resource.metal_texture
                    : output_identity;
  id<MTLTexture> history_primitive =
      history_valid ? (id<MTLTexture>)primitive_slot->instances[selected]
                          .resource.metal_texture
                    : output_primitive;
  uint64_t root_gpu = 0u;
  VkrMetalPacketTemporalResolveRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketTemporalResolveRoot){
      .visible_rows = visible->resource.gpu_address,
      .instances = instances->resource.gpu_address,
      .scene_texture_id = scene.gpuResourceID._impl,
      .pre_transmission_texture_id = pre_transmission.gpuResourceID._impl,
      .motion_texture_id = motion.gpuResourceID._impl,
      .validity_texture_id = validity.gpuResourceID._impl,
      .depth_texture_id = depth.gpuResourceID._impl,
      .vbuffer_texture_id = vbuffer.gpuResourceID._impl,
      .history_color_texture_id = history_color.gpuResourceID._impl,
      .history_depth_texture_id = history_depth.gpuResourceID._impl,
      .history_identity_texture_id = history_identity.gpuResourceID._impl,
      .history_primitive_texture_id = history_primitive.gpuResourceID._impl,
      .output_color_texture_id = output_color.gpuResourceID._impl,
      .output_depth_texture_id = output_depth.gpuResourceID._impl,
      .output_identity_texture_id = output_identity.gpuResourceID._impl,
      .output_primitive_texture_id = output_primitive.gpuResourceID._impl,
      .extent = {(uint32_t)scene.width, (uint32_t)scene.height},
      .history_valid = history_valid,
      .render_mode = packet->globals.render_mode,
      .camera_stationary =
          history_valid &&
          MemCompare(&color_slot->instances[selected].history_view_projection,
                     &current_history_view_projection, sizeof(Mat4)) == 0,
      .transmission_visible_rows =
          transmission_enabled ? transmission_visible->resource.gpu_address
                               : 0u,
      .transmission_instances =
          transmission_enabled ? transmission_instances->resource.gpu_address
                               : 0u,
      .transmission_vbuffer_texture_id =
          transmission_enabled ? transmission_vbuffer.gpuResourceID._impl : 0u,
      .transmission_depth_texture_id =
          transmission_enabled ? transmission_depth.gpuResourceID._impl : 0u,
      .transmission_enabled = transmission_enabled,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->temporal_resolve_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(scene.width, scene.height, 1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_metalfx_temporal(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> color = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  id<MTLTexture> motion = vkr_metal_packet_pass_texture(renderer, pass, 2u);
  id<MTLTexture> output = vkr_metal_packet_pass_texture(renderer, pass, 3u);
  id<MTL4FXTemporalScaler> scaler = renderer->metalfx_temporal_scaler;
  const uint32_t content_width = renderer->prepared_frame.viewport_width;
  const uint32_t content_height = renderer->prepared_frame.viewport_height;

  scaler.colorTexture = color;
  scaler.depthTexture = depth;
  scaler.motionTexture = motion;
  scaler.outputTexture = output;
  scaler.exposureTexture = nil;
  scaler.reactiveMaskTexture = nil;
  scaler.inputContentWidth = content_width;
  scaler.inputContentHeight = content_height;
  scaler.preExposure = 1.0f;
  scaler.jitterOffsetX = packet->globals.temporal.jitter_pixels.x;
  scaler.jitterOffsetY = packet->globals.temporal.jitter_pixels.y;
  scaler.motionVectorScaleX = (float32_t)content_width;
  scaler.motionVectorScaleY = (float32_t)content_height;
  scaler.depthReversed = NO;
  scaler.reset = packet->globals.temporal.reset_reasons != 0u ||
                 !packet->globals.temporal.history_valid ||
                 renderer->selected_temporal_history_instance == UINT32_MAX;
  [scaler encodeToCommandBuffer:renderer->command_buffer];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_transmission_shade(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  if (!renderer || !pass || !packet || !packet->world || !encoder || !upload ||
      !renderer->transmission_shade_pipeline ||
      !renderer->transmission_shade_partitioned_pipeline ||
      !renderer->transmission_shade_production_pipeline ||
      !renderer->transmission_shade_production_temporal_pipeline ||
      !renderer->transmission_shade_partitioned_production_pipeline ||
      !renderer->transmission_shade_partitioned_production_temporal_pipeline)
    return false_v;
  id<MTLTexture> vbuffer = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  VkrMetalPacketGraphBufferInstance *visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 3u);
  VkrMetalPacketGraphBufferInstance *pixel_list =
      vkr_metal_packet_pass_buffer(renderer, pass, 6u);
  VkrMetalPacketGraphBufferInstance *indirect_arguments =
      vkr_metal_packet_pass_buffer(renderer, pass, 7u);
  id<MTLTexture> source = vkr_metal_packet_pass_texture(renderer, pass, 4u);
  id<MTLTexture> opaque_pyramid =
      vkr_metal_packet_pass_texture(renderer, pass, 13u);
  id<MTLTexture> destination =
      vkr_metal_packet_pass_texture(renderer, pass, 5u);
  if (!vbuffer || !depth || !visible || !state || !source || !opaque_pyramid ||
      !destination || vbuffer.width == 0u || vbuffer.height == 0u ||
      vbuffer.width > UINT32_MAX || vbuffer.height > UINT32_MAX)
    return false_v;
  const bool8_t compact = pixel_list && indirect_arguments;
  const uint64_t pixel_capacity =
      compact ? pixel_list->resource.length / (2u * sizeof(uint32_t)) : 0u;
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const uint32_t compact_layer = vbuffer_use && vbuffer_use->has_slice
                                     ? vbuffer_use->slice.base_layer
                                     : UINT32_MAX;
  if (compact && (pass->desc.dispatch.kind != VKR_RG_DISPATCH_INDIRECT ||
                  pixel_list->resource.length % (2u * sizeof(uint32_t)) != 0u ||
                  pixel_capacity != (uint64_t)vbuffer.width * vbuffer.height ||
                  pixel_capacity > UINT32_MAX ||
                  compact_layer >= VKR_METAL_PACKET_TRANSMISSION_LAYER_COUNT))
    return false_v;
  const bool8_t temporal_outputs = compact_layer == 0u;
  const bool8_t diagnostic =
      packet->globals.render_mode != 0u ||
      (packet->debug && packet->debug->shadow_debug_mode != 0u);
  id<MTLTexture> motion =
      temporal_outputs ? vkr_metal_packet_pass_texture(renderer, pass, 11u)
                       : nil;
  id<MTLTexture> validity =
      temporal_outputs ? vkr_metal_packet_pass_texture(renderer, pass, 12u)
                       : nil;
  VkrMetalPacketGraphBufferInstance *temporal_transforms =
      temporal_outputs ? vkr_metal_packet_pass_buffer(renderer, pass, 10u)
                       : NULL;
  if (temporal_outputs && (!motion || !validity || !temporal_transforms))
    return false_v;
  const uint32_t current_temporal_instance =
      (uint32_t)(renderer->active_command_slot - renderer->command_slots);
  const uint32_t previous_temporal_instance =
      renderer->selected_temporal_history_instance != UINT32_MAX
          ? renderer->selected_temporal_history_instance
          : current_temporal_instance;
  VkrMetalPacketGraphBufferInstance *previous_transform =
      vkr_metal_packet_graph_buffer_instance(
          renderer, renderer->temporal_transform_history_handle,
          previous_temporal_instance);
  if (!previous_transform)
    return false_v;

  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->globals.temporal.jittered_projection,
               packet->globals.view));
  uint64_t frame_root_gpu = 0u;
  uint32_t frame_flags = 0u;
  if (!vkr_metal_packet_create_frame_root(
          renderer, packet, upload, upload->transmission_gpu_draw_instances_gpu,
          (uint32_t)vbuffer.width, (uint32_t)vbuffer.height, view_projection,
          true_v, true_v, &frame_root_gpu, &frame_flags))
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketTransmissionShadeRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketTransmissionShadeRoot){
      .frame = frame_root_gpu,
      .visible_rows = visible->resource.gpu_address,
      .geometry_rows = upload->gpu_draw_geometry_rows_gpu,
      .instances = upload->transmission_gpu_draw_instances_gpu,
      .materials =
          vkr_metal_material_table_device_gpu_address(renderer->materials),
      .transmission_materials =
          vkr_metal_material_table_device_transmission_gpu_address(
              renderer->materials),
      .compaction_state = state->resource.gpu_address,
      .pixel_list = compact ? pixel_list->resource.gpu_address : 0u,
      .compact_counts = compact ? indirect_arguments->resource.gpu_address +
                                      6u * sizeof(uint32_t)
                                : 0u,
      .vbuffer_texture_id = vbuffer.gpuResourceID._impl,
      .depth_texture_id = depth.gpuResourceID._impl,
      .source_texture_id = source.gpuResourceID._impl,
      .destination_texture_id = destination.gpuResourceID._impl,
      .view_projection = view_projection,
      .inverse_view_projection = mat4_inverse(view_projection),
      .extent = {(uint32_t)vbuffer.width, (uint32_t)vbuffer.height},
      .visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .geometry_count = renderer->max_meshes,
      .material_count = renderer->max_materials,
      .instance_count = renderer->transmission_gpu_draw_count,
      .pixel_capacity = (uint32_t)pixel_capacity,
      .compact_layer = compact_layer,
      .compact_enabled = compact ? 1u : 0u,
      .opaque_mip_count =
          renderer->prepared_frame.transmission_rough_mip_pass_count + 1u,
      .opaque_texture_id = opaque_pyramid.gpuResourceID._impl,
      .motion_texture_id = temporal_outputs ? motion.gpuResourceID._impl : 0u,
      .validity_texture_id =
          temporal_outputs ? validity.gpuResourceID._impl : 0u,
      .previous_transforms = previous_transform->resource.gpu_address,
      .current_view_projection = vkr_metal_packet_clip_matrix(
          renderer->convert_vulkan_clip_y,
          packet->globals.temporal.current_view_projection),
      .previous_view_projection =
          renderer->selected_temporal_history_instance != UINT32_MAX
              ? previous_transform->history_view_projection
              : vkr_metal_packet_clip_matrix(
                    renderer->convert_vulkan_clip_y,
                    packet->globals.temporal.current_view_projection),
      .history_valid =
          renderer->selected_temporal_history_instance != UINT32_MAX,
      .previous_frame_index =
          renderer->selected_temporal_history_instance != UINT32_MAX
              ? previous_transform->history_frame_index
              : packet->frame.frame_index,
      .temporal_outputs_enabled = temporal_outputs,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setArgumentTable:renderer->argument_table];
  if (compact) {
    const VkrRgBufferUse *indirect_use = vkr_rg_pass_find_buffer_use(
        &pass->desc, pass->desc.dispatch.indirect_binding,
        pass->desc.dispatch.indirect_array_index);
    VkrMetalPacketGraphBufferInstance *indirect_source =
        indirect_use
            ? vkr_metal_packet_graph_buffer(renderer, indirect_use->buffer)
            : NULL;
    if (!indirect_source ||
        pass->desc.dispatch.indirect_offset >
            indirect_source->resource.length ||
        indirect_source->resource.length - pass->desc.dispatch.indirect_offset <
            8u * sizeof(uint32_t))
      return false_v;
    id<MTLComputePipelineState> pipeline =
        diagnostic
            ? renderer->transmission_shade_partitioned_pipeline
            : (temporal_outputs
                   ? renderer
                         ->transmission_shade_partitioned_production_temporal_pipeline
                   : renderer
                         ->transmission_shade_partitioned_production_pipeline);
    [encoder setComputePipelineState:pipeline];
    [encoder dispatchThreadgroupsWithIndirectBuffer:indirect_source->resource
                                                        .gpu_address +
                                                    pass->desc.dispatch
                                                        .indirect_offset
                              threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
  } else {
    id<MTLComputePipelineState> pipeline =
        diagnostic
            ? renderer->transmission_shade_pipeline
            : (temporal_outputs
                   ? renderer->transmission_shade_production_temporal_pipeline
                   : renderer->transmission_shade_production_pipeline);
    [encoder setComputePipelineState:pipeline];
    [encoder dispatchThreads:MTLSizeMake(vbuffer.width, vbuffer.height, 1u)
        threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  }
  if ((frame_flags & VKR_PACKET_FRAME_FLAG_IBL) != 0u)
    renderer->ibl_last_use_submit_value = renderer->submit_value + 1u;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_transmission_compact(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    id<MTL4ComputeCommandEncoder> encoder, VkrMetalPacketFrameUpload *upload)
    API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> visibility = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  VkrMetalPacketGraphBufferInstance *pixel_list =
      vkr_metal_packet_pass_buffer(renderer, pass, 1u);
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  VkrMetalPacketGraphBufferInstance *indirect_arguments =
      vkr_metal_packet_pass_buffer(renderer, pass, 3u);
  VkrMetalPacketGraphBufferInstance *visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 6u);
  id<MTLTexture> source = vkr_metal_packet_pass_texture(renderer, pass, 4u);
  id<MTLTexture> destination =
      vkr_metal_packet_pass_texture(renderer, pass, 5u);
  const VkrRgImageUse *use = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const uint32_t layer =
      use && use->has_slice ? use->slice.base_layer : UINT32_MAX;
  const uint64_t capacity =
      pixel_list ? pixel_list->resource.length / (2u * sizeof(uint32_t)) : 0u;
  if (!visibility || !pixel_list || !state || !indirect_arguments || !visible ||
      !source || !destination ||
      !renderer->transmission_compact_clear_pipeline ||
      !renderer->transmission_compact_pipeline ||
      !renderer->transmission_compact_finalize_pipeline ||
      visibility.width == 0u || visibility.height == 0u ||
      visibility.width > UINT16_MAX || visibility.height > UINT16_MAX ||
      pixel_list->resource.length % (2u * sizeof(uint32_t)) != 0u ||
      capacity != (uint64_t)visibility.width * visibility.height ||
      capacity > UINT32_MAX ||
      layer >= VKR_METAL_PACKET_TRANSMISSION_LAYER_COUNT ||
      source.width != visibility.width || source.height != visibility.height ||
      destination.width != visibility.width ||
      destination.height != visibility.height ||
      indirect_arguments->resource.length < 8u * sizeof(uint32_t))
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketTransmissionCompactRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketTransmissionCompactRoot){
      .vbuffer_texture_id = visibility.gpuResourceID._impl,
      .pixel_list = pixel_list->resource.gpu_address,
      .covered_pixels =
          state->resource.gpu_address +
          offsetof(VkrMetalPacketTransmissionDiagnostics, covered_pixels),
      .overflow_counts =
          state->resource.gpu_address +
          offsetof(VkrMetalPacketTransmissionDiagnostics, compact_overflow),
      .indirect_arguments = indirect_arguments->resource.gpu_address,
      .visible_rows = visible->resource.gpu_address,
      .materials =
          vkr_metal_material_table_device_gpu_address(renderer->materials),
      .source_texture_id = source.gpuResourceID._impl,
      .destination_texture_id = destination.gpuResourceID._impl,
      .extent = {(uint32_t)visibility.width, (uint32_t)visibility.height},
      .layer = layer,
      .capacity = (uint32_t)capacity,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder
      setComputePipelineState:renderer->transmission_compact_clear_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(1u, 1u, 1u)
      threadsPerThreadgroup:MTLSizeMake(1u, 1u, 1u)];
  [encoder barrierAfterEncoderStages:MTLStageDispatch
                 beforeEncoderStages:MTLStageDispatch
                   visibilityOptions:MTL4VisibilityOptionDevice];
  [encoder setComputePipelineState:renderer->transmission_compact_pipeline];
  [encoder dispatchThreads:MTLSizeMake(visibility.width, visibility.height, 1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  [encoder barrierAfterEncoderStages:MTLStageDispatch
                 beforeEncoderStages:MTLStageDispatch
                   visibilityOptions:MTL4VisibilityOptionDevice];
  [encoder
      setComputePipelineState:renderer->transmission_compact_finalize_pipeline];
  [encoder dispatchThreads:MTLSizeMake(1u, 1u, 1u)
      threadsPerThreadgroup:MTLSizeMake(1u, 1u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_transmission_coverage(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    id<MTL4ComputeCommandEncoder> encoder, VkrMetalPacketFrameUpload *upload)
    API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> visibility = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 1u);
  const VkrRgImageUse *use = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const uint32_t layer =
      use && use->has_slice ? use->slice.base_layer : UINT32_MAX;
  if (!visibility || !state || !renderer->transmission_coverage_pipeline ||
      visibility.width == 0u || visibility.height == 0u ||
      layer >= VKR_METAL_PACKET_TRANSMISSION_DIAGNOSTIC_LAYER_COUNT)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketTransmissionCoverageRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketTransmissionCoverageRoot){
      .vbuffer_texture_id = visibility.gpuResourceID._impl,
      .covered_pixels =
          state->resource.gpu_address +
          offsetof(VkrMetalPacketTransmissionDiagnostics, covered_pixels),
      .extent = {(uint32_t)visibility.width, (uint32_t)visibility.height},
      .layer = layer,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->transmission_coverage_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(visibility.width, visibility.height, 1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_picking_resolve(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  if (!renderer || !pass || !packet || !packet->picking || !encoder ||
      !upload || !renderer->picking_resolve_pipeline)
    return false_v;
  id<MTLTexture> opaque_vbuffer =
      vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> opaque_depth =
      vkr_metal_packet_pass_texture(renderer, pass, 1u);
  id<MTLTexture> destination =
      vkr_metal_packet_pass_texture(renderer, pass, 4u);
  VkrMetalPacketGraphBufferInstance *opaque_visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  VkrMetalPacketGraphBufferInstance *opaque_state =
      vkr_metal_packet_pass_buffer(renderer, pass, 3u);
  id<MTLTexture> transmission_vbuffer =
      vkr_metal_packet_pass_texture(renderer, pass, 5u);
  id<MTLTexture> transmission_depth =
      vkr_metal_packet_pass_texture(renderer, pass, 6u);
  VkrMetalPacketGraphBufferInstance *transmission_visible =
      vkr_metal_packet_pass_buffer(renderer, pass, 7u);
  VkrMetalPacketGraphBufferInstance *transmission_state =
      vkr_metal_packet_pass_buffer(renderer, pass, 8u);
  const bool8_t transmission = transmission_vbuffer != nil;
  if (!opaque_vbuffer || !opaque_depth || !destination || !opaque_visible ||
      !opaque_state ||
      (transmission &&
       (!transmission_depth || !transmission_visible || !transmission_state)) ||
      destination.width == 0u || destination.height == 0u ||
      destination.width > UINT32_MAX || destination.height > UINT32_MAX)
    return false_v;

  uint64_t root_gpu = 0u;
  VkrMetalPacketPickingResolveRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketPickingResolveRoot){
      .opaque_visible_rows = opaque_visible->resource.gpu_address,
      .opaque_instances = upload->gpu_draw_instances_gpu,
      .opaque_compaction_state = opaque_state->resource.gpu_address,
      .transmission_visible_rows =
          transmission ? transmission_visible->resource.gpu_address : 0u,
      .transmission_instances =
          transmission ? upload->transmission_gpu_draw_instances_gpu : 0u,
      .transmission_compaction_state =
          transmission ? transmission_state->resource.gpu_address : 0u,
      .opaque_vbuffer_texture_id = opaque_vbuffer.gpuResourceID._impl,
      .opaque_depth_texture_id = opaque_depth.gpuResourceID._impl,
      .transmission_vbuffer_texture_id =
          transmission ? transmission_vbuffer.gpuResourceID._impl : 0u,
      .transmission_depth_texture_id =
          transmission ? transmission_depth.gpuResourceID._impl : 0u,
      .destination_texture_id = destination.gpuResourceID._impl,
      .pixel = {MIN(packet->picking->x, (uint32_t)destination.width - 1u),
                MIN(packet->picking->y, (uint32_t)destination.height - 1u)},
      .extent = {(uint32_t)destination.width, (uint32_t)destination.height},
      .opaque_visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .opaque_instance_count = renderer->gpu_draw_count,
      .transmission_visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .transmission_instance_count = renderer->transmission_gpu_draw_count,
      .transmission_enabled = transmission ? 1u : 0u,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->picking_resolve_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(1u, 1u, 1u)
      threadsPerThreadgroup:MTLSizeMake(1u, 1u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_hzb_build(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    id<MTL4ComputeCommandEncoder> encoder, VkrMetalPacketFrameUpload *upload)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer || !pass || !encoder || !upload ||
      !renderer->hzb_build_pipeline)
    return false_v;
  const VkrRgImageUse *source_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  if (!source_use || !destination_use)
    return false_v;
  const uint32_t source_mip =
      source_use->has_slice ? source_use->slice.mip_level : 0u;
  const uint32_t destination_mip =
      destination_use->has_slice ? destination_use->slice.mip_level : 0u;
  id<MTLTexture> source =
      source_use->has_slice
          ? vkr_metal_packet_mip_texture(renderer, source_use->image,
                                         source_mip)
          : vkr_metal_packet_texture(renderer, source_use->image);
  id<MTLTexture> destination =
      destination_use->has_slice
          ? vkr_metal_packet_mip_texture(renderer, destination_use->image,
                                         destination_mip)
          : vkr_metal_packet_texture(renderer, destination_use->image);
  if (!source || !destination || source.width == 0u || source.height == 0u ||
      destination.width == 0u || destination.height == 0u ||
      source.width > UINT32_MAX || source.height > UINT32_MAX ||
      destination.width > UINT32_MAX || destination.height > UINT32_MAX)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketHzbBuildRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketHzbBuildRoot){
      .source_texture_id = source.gpuResourceID._impl,
      .destination_texture_id = destination.gpuResourceID._impl,
      .source_extent = {(uint32_t)source.width, (uint32_t)source.height},
      .destination_extent = {(uint32_t)destination.width,
                             (uint32_t)destination.height},
      .source_is_depth = source_use->image.id != destination_use->image.id,
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->hzb_build_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(destination.width, destination.height,
                                       1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_sdsm_reduce(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    id<MTL4ComputeCommandEncoder> encoder, VkrMetalPacketFrameUpload *upload)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer || !pass || !encoder || !upload ||
      !renderer->sdsm_reduce_pipeline)
    return false_v;
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> vbuffer = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  if (!depth || !vbuffer || !state || depth.width == 0u || depth.height == 0u ||
      depth.width > UINT32_MAX || depth.height > UINT32_MAX ||
      depth.width > UINT32_MAX / depth.height)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketSdsmRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketSdsmRoot){
      .depth_texture_id = depth.gpuResourceID._impl,
      .vbuffer_texture_id = vbuffer.gpuResourceID._impl,
      .reduce_state = state->resource.gpu_address,
      .extent = {(uint32_t)depth.width, (uint32_t)depth.height},
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->sdsm_reduce_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(depth.width, depth.height, 1u)
      threadsPerThreadgroup:MTLSizeMake(16u, 16u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_exposure_histogram(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> source = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  VkrMetalPacketGraphBufferInstance *histogram =
      vkr_metal_packet_pass_buffer(renderer, pass, 1u);
  if (!source || !histogram || !renderer->exposure_clear_pipeline ||
      !renderer->exposure_histogram_pipeline || source.width == 0u ||
      source.height == 0u || source.width > UINT32_MAX ||
      source.height > UINT32_MAX)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketExposureRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketExposureRoot){
      .histogram = histogram->resource.gpu_address,
      .source_texture_id = source.gpuResourceID._impl,
      .extent = {(uint32_t)source.width, (uint32_t)source.height},
      .reset_reasons = packet->globals.exposure.reset_reasons,
      .metering = vkr_exposure_gpu_metering(&renderer->exposure_metering,
                                            &packet->globals.exposure),
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->exposure_clear_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u, 1u)
      threadsPerThreadgroup:MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u,
                                        1u)];
  [encoder barrierAfterEncoderStages:MTLStageDispatch
                 beforeEncoderStages:MTLStageDispatch
                   visibilityOptions:MTL4VisibilityOptionDevice];
  [encoder setComputePipelineState:renderer->exposure_histogram_pipeline];
  // Every group needs all 256 lanes to initialize and merge its histogram bins.
  [encoder dispatchThreadgroups:MTLSizeMake((source.width + 15u) / 16u,
                                           (source.height + 15u) / 16u, 1u)
          threadsPerThreadgroup:MTLSizeMake(16u, 16u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_exposure_resolve(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketGraphBufferInstance *histogram =
      vkr_metal_packet_pass_buffer(renderer, pass, 0u);
  VkrMetalPacketGraphBufferInstance *output =
      vkr_metal_packet_pass_buffer(renderer, pass, 1u);
  VkrMetalPacketGraphBufferInstance *previous =
      vkr_metal_packet_graph_buffer_instance(
          renderer, renderer->exposure_state_handle,
          renderer->selected_exposure_history_instance);
  if (!histogram || !output || !renderer->exposure_resolve_pipeline)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketExposureRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketExposureRoot){
      .histogram = histogram->resource.gpu_address,
      .state = output->resource.gpu_address,
      .previous_state = previous ? previous->resource.gpu_address
                                 : output->resource.gpu_address,
      .reset_reasons = packet->globals.exposure.reset_reasons,
      .metering = vkr_exposure_gpu_metering(&renderer->exposure_metering,
                                            &packet->globals.exposure),
  };
  if (!previous)
    root->metering.history_valid = 0u;
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:renderer->exposure_resolve_pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u, 1u)
      threadsPerThreadgroup:MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u,
                                        1u)];
  return true_v;
}

/**
 * @brief Encodes one bloom chain step.
 *
 * Every level derives its own extents from the authored subresource rather than
 * from a pass index, exactly as the HZB chain does. The chain length is then a
 * graph decision alone, and an encoder cannot disagree with the graph about
 * which mip it is writing.
 *
 * The upsample step additionally chooses its coarse source here: at the deepest
 * level the accumulation chain above it has never been written, so binding 3
 * (the downsample chain at the same depth) is used instead of binding 2.
 * Selecting on the CPU is what keeps the kernel free of a bootstrap branch and
 * keeps every sampled texel defined.
 */
vkr_internal bool8_t vkr_metal_packet_encode_bloom(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4ComputeCommandEncoder> encoder,
    VkrMetalPacketFrameUpload *upload, VkrMetalPacketGraphExecutorKind kind)
    API_AVAILABLE(macos(26.0)) {
  const VkrRgImageUse *source_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  id<MTLTexture> source =
      vkr_metal_packet_subresource_texture(renderer, source_use);
  id<MTLTexture> destination =
      vkr_metal_packet_subresource_texture(renderer, destination_use);
  if (!source || !destination || destination.width == 0u ||
      destination.height == 0u || source.width == 0u || source.height == 0u ||
      source.width > UINT32_MAX || source.height > UINT32_MAX ||
      destination.width > UINT32_MAX || destination.height > UINT32_MAX)
    return false_v;

  id<MTLTexture> coarse = source;
  const bool8_t upsample =
      kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_BLOOM_UPSAMPLE;
  const bool8_t combine = kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_BLOOM_COMBINE;
  if (upsample || combine) {
    uint32_t binding = 2u;
    if (upsample) {
      const uint32_t destination_mip =
          destination_use->has_slice ? destination_use->slice.mip_level : 0u;
      const bool8_t deepest =
          destination_mip + 2u >= renderer->prepared_frame.bloom_mip_count;
      binding = deepest ? 3u : 2u;
    }
    coarse = vkr_metal_packet_subresource_texture(
        renderer, vkr_rg_pass_find_image_use(&pass->desc, binding, 0u));
    if (!coarse || coarse.width == 0u || coarse.height == 0u ||
        coarse.width > UINT32_MAX || coarse.height > UINT32_MAX)
      return false_v;
  }

  id<MTLComputePipelineState> pipeline = renderer->bloom_prefilter_pipeline;
  if (combine) {
    pipeline = renderer->bloom_combine_pipeline;
  } else if (upsample) {
    pipeline = renderer->bloom_upsample_pipeline;
  } else if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_BLOOM_DOWNSAMPLE) {
    /* Both filters are resident; the cold configuration selects which one this
       build measures. Neither is a fallback for the other. */
    pipeline = renderer->bloom_config.filter == VKR_BLOOM_FILTER_BOX_4
                   ? renderer->bloom_downsample_box4_pipeline
                   : renderer->bloom_downsample_tent13_pipeline;
  } else if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_DOWNSAMPLE) {
    pipeline = renderer->bloom_downsample_box4_pipeline;
  }
  if (!pipeline)
    return false_v;

  uint64_t root_gpu = 0u;
  VkrMetalPacketBloomRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  /* The tap offsets are relative to the filtered source: the reduction input
     for the prefilter and downsample, the coarse level for the upsample. */
  id<MTLTexture> filtered = upsample ? coarse : source;
  *root = (VkrMetalPacketBloomRoot){
      .source_texture_id = source.gpuResourceID._impl,
      .coarse_texture_id = coarse.gpuResourceID._impl,
      .destination_texture_id = destination.gpuResourceID._impl,
      .filter_extent = {(uint32_t)filtered.width, (uint32_t)filtered.height},
      .destination_extent = {(uint32_t)destination.width,
                             (uint32_t)destination.height},
      .params =
          vkr_bloom_gpu_params(&renderer->bloom_config, &packet->globals.bloom),
  };
  [renderer->argument_table setAddress:root_gpu atIndex:0u];
  [encoder setComputePipelineState:pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:MTLSizeMake(destination.width, destination.height,
                                       1u)
      threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_gpu_draws(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline,
    id<MTLRenderPipelineState> opaque_pipeline,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  const bool8_t transmission = vkr_metal_packet_graph_pass_is(
      pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION);
  const uint32_t candidate_count = transmission
                                       ? renderer->transmission_gpu_draw_count
                                       : renderer->gpu_draw_count;
  if (candidate_count == 0u)
    return true_v;
  id<MTLIndirectCommandBuffer> icb =
      transmission ? renderer->active_command_slot->transmission_gpu_draw_icb
                   : renderer->active_command_slot->gpu_draw_icbs[0];
  if (!state || !icb || !pipeline)
    return false_v;
  if (transmission) {
    const VkrRgImageUse *previous_depth_use =
        vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u);
    id<MTLTexture> previous_depth =
        previous_depth_use ? vkr_metal_packet_pass_texture(renderer, pass, 3u)
                           : nil;
    uint64_t peel_root_gpu = 0u;
    VkrMetalPacketTransmissionPeelRoot *peel =
        vkr_metal_packet_next_root(upload, sizeof(*peel), &peel_root_gpu);
    if (!peel || !upload->transmission_gpu_draw_root_gpu ||
        (previous_depth_use && !previous_depth))
      return false_v;
    *peel = (VkrMetalPacketTransmissionPeelRoot){
        .previous_depth_texture_id =
            previous_depth ? previous_depth.gpuResourceID._impl : 0u,
        .depth_epsilon = 1e-6f,
        .previous_depth_enabled = previous_depth ? 1u : 0u,
    };
    [renderer->argument_table setAddress:peel_root_gpu atIndex:2u];
    [renderer->argument_table setAddress:upload->transmission_gpu_draw_root_gpu
                                 atIndex:0u];
    [renderer->argument_table setAddress:upload->transmission_gpu_draw_root_gpu
                                 atIndex:1u];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
  }
  [encoder setViewport:(MTLViewport){
                           0.0, 0.0, renderer->prepared_frame.viewport_width,
                           renderer->prepared_frame.viewport_height, 0.0, 1.0}];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setDepthStencilState:renderer->depth_write_state];
  id<MTLRenderPipelineState> bound_pipeline = nil;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    // Opaque buckets take the discard-free variant when one exists; the
    // transmission pass passes nil because its peel logic needs the full stage.
    id<MTLRenderPipelineState> wanted_pipeline =
        (opaque_pipeline &&
         (bucket == VKR_WORLD_DRAW_STATE_OPAQUE_BACK ||
          bucket == VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED))
            ? opaque_pipeline
            : pipeline;
    if (wanted_pipeline != bound_pipeline) {
      [encoder setRenderPipelineState:wanted_pipeline];
      bound_pipeline = wanted_pipeline;
    }
    [encoder
        setCullMode:(bucket & 1u) != 0u ? MTLCullModeNone : MTLCullModeBack];
    [encoder executeCommandsInBuffer:icb
                      indirectBuffer:state->resource.gpu_address +
                                     (uint64_t)bucket * 2u * sizeof(uint32_t)];
  }
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_gpu_shadow(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrRenderPacket *packet, id<MTL4RenderCommandEncoder> encoder,
    uint32_t cascade, uint32_t target_width, uint32_t target_height,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  if (!packet->world || !packet->shadow || !state ||
      !renderer->gpu_shadow_pipeline ||
      cascade >= renderer->prepared_frame.shadow_cascade_count)
    return false_v;
  const uint32_t view = cascade + 1u;
  const uint32_t group = view / renderer->gpu_draw_icb_view_group_size;
  id<MTLIndirectCommandBuffer> icb =
      renderer->active_command_slot->gpu_draw_icbs[group];
  if (!icb)
    return false_v;
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setDepthStencilState:renderer->depth_write_state];
  /* Raster depth bias from the shared packet contract. Each backend lowers the
     same constant/slope/clamp controls through its native depth-bias command;
     receiver bias remains a separate shader input. */
  const VkrShadowConfigOverride bias = packet->shadow->config_override
                                           ? *packet->shadow->config_override
                                           : (VkrShadowConfigOverride){0};
  [encoder setDepthBias:bias.depth_bias_constant
             slopeScale:bias.depth_bias_slope
                  clamp:bias.depth_bias_clamp];
  id<MTLRenderPipelineState> bound_shadow_pipeline = nil;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    // Opaque cascade geometry runs the depth-only pipeline; only the cutout
    // buckets need the alpha-testing fragment stage.
    id<MTLRenderPipelineState> wanted_shadow_pipeline =
        (bucket == VKR_WORLD_DRAW_STATE_OPAQUE_BACK ||
         bucket == VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED)
            ? renderer->gpu_shadow_opaque_pipeline
            : renderer->gpu_shadow_pipeline;
    if (wanted_shadow_pipeline != bound_shadow_pipeline) {
      [encoder setRenderPipelineState:wanted_shadow_pipeline];
      bound_shadow_pipeline = wanted_shadow_pipeline;
    }
    [encoder
        setCullMode:(bucket & 1u) != 0u ? MTLCullModeNone : MTLCullModeBack];
    [encoder executeCommandsInBuffer:icb
                      indirectBuffer:state->resource.gpu_address +
                                     ((uint64_t)view *
                                      sizeof(VkrGpuDrawCompactionState)) +
                                     (uint64_t)bucket * 2u * sizeof(uint32_t)];
  }
  (void)result;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_encode_pass_body(
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
  VkrMetalPacketGraphExecutorKind kind;
  if (!vkr_metal_packet_graph_executor_kind(pass, &kind))
    return false_v;
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_SHADOW) {
    if (!packet->shadow)
      return true_v;
    const uint32_t cascade = pass->desc.repeat_index;
    if (cascade >= packet->shadow->cascade_count)
      return true_v;
    if (!packet->world || renderer->gpu_draw_count == 0u)
      return true_v;
    return vkr_metal_packet_encode_gpu_shadow(renderer, pass, packet, encoder,
                                              cascade, (uint32_t)target.width,
                                              (uint32_t)target.height, result);
  }
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_OPAQUE)
    return vkr_metal_packet_encode_gpu_draws(
        renderer, pass, packet, encoder, renderer->vbuffer_pipeline,
        renderer->vbuffer_opaque_pipeline, upload);
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION)
    return vkr_metal_packet_encode_gpu_draws(
        renderer, pass, packet, encoder,
        renderer->transmission_vbuffer_pipeline, nil, upload);
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_WORLD_BLEND) {
    if (!packet->world)
      return true_v;
    const Mat4 view_projection = vkr_metal_packet_clip_matrix(
        renderer->convert_vulkan_clip_y,
        mat4_mul(packet->globals.temporal.jittered_projection,
                 packet->globals.view));
    if (!vkr_metal_packet_encode_draws(
            renderer, encoder, renderer->blend_pipeline,
            renderer->depth_read_state, upload->direct_draws,
            upload->direct_draw_count, upload->world_instances_gpu,
            (uint32_t)target.width, (uint32_t)target.height, view_projection,
            packet, true_v, false_v, upload, &result->blend_draw_count, result))
      return false_v;
    return vkr_metal_packet_encode_text_draws(
        renderer, encoder, renderer->world_text_pipeline,
        renderer->depth_read_state, packet->world->text_draws,
        packet->world->text_draw_count, 0, (uint32_t)target.width,
        (uint32_t)target.height, view_projection, false_v, upload,
        &result->text_draw_count, result);
  }
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING) {
    if (!packet->picking || !packet->picking->pending)
      return true_v;
    uint32_t picking_draw_count = 0u;
    uint32_t picking_text_draw_count = 0u;
    const Mat4 view_projection = vkr_metal_packet_clip_matrix(
        renderer->convert_vulkan_clip_y,
        mat4_mul(packet->globals.projection, packet->globals.view));
    if (packet->world &&
        !vkr_metal_packet_encode_draws(
            renderer, encoder, renderer->picking_pipeline,
            renderer->depth_write_state, upload->direct_draws,
            upload->direct_draw_count, upload->world_instances_gpu,
            (uint32_t)target.width, (uint32_t)target.height, view_projection,
            packet, false_v, false_v, upload, &picking_draw_count, result))
      return false_v;
    if (packet->world &&
        !vkr_metal_packet_encode_text_draws(
            renderer, encoder, renderer->picking_text_pipeline,
            renderer->depth_write_state, packet->world->text_draws,
            packet->world->text_draw_count, 0u, (uint32_t)target.width,
            (uint32_t)target.height, view_projection, false_v, upload,
            &picking_text_draw_count, result))
      return false_v;
    return true_v;
  }
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_TONEMAP)
    return vkr_metal_packet_encode_tonemap(renderer, pass, packet, encoder,
                                           upload, (uint32_t)target.width,
                                           (uint32_t)target.height, NULL);
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_EDITOR)
    return vkr_metal_packet_encode_tonemap(
        renderer, pass, packet, encoder, upload, (uint32_t)target.width,
        (uint32_t)target.height, packet->editor);
  if (kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_UI) {
    if (!packet->ui)
      return true_v;
    return vkr_metal_packet_encode_ui_draw_list(
        renderer, encoder, &packet->ui->draw_list, (uint32_t)target.width,
        (uint32_t)target.height, upload, result);
  }
  return true_v;
}

bool8_t vkr_metal_packet_renderer_prepare_frame(
    VkrMetalPacketRenderer *renderer,
    const VkrRenderGraphFrameInfo *frame_info) {
  if (frame_info->target_width == 0 || frame_info->target_height == 0 ||
      frame_info->scene_output_width == 0 ||
      frame_info->scene_output_height == 0 || frame_info->viewport_width == 0 ||
      frame_info->viewport_height == 0) {
    log_error(
        "Metal frame preparation rejected a zero target, Scene output, or "
        "viewport");
    return false_v;
  }
  [renderer->drawable release];
  renderer->drawable = nil;
  renderer->prepared_frame = *frame_info;
  if (renderer->target_kind == VKR_METAL_PACKET_TARGET_WINDOW) {
    if (vkr_metal_packet_pixel_format(frame_info->target_color_format) !=
        renderer->layer.pixelFormat) {
      log_error("Metal frame preparation target format mismatch");
      return false_v;
    }
  }
  if (!vkr_metal_packet_acquire_command_slot(renderer, false_v)) {
    log_error("Metal frame preparation could not acquire a command slot "
              "(next=%u completed=%llu submitted=%llu)",
              renderer->next_command_slot,
              (unsigned long long)renderer->completion.signaledValue,
              (unsigned long long)renderer->submit_value);
    return false_v;
  }
  renderer->prepared_frame.image_index =
      (uint32_t)(renderer->active_command_slot - renderer->command_slots);
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
      log_error("Metal frame preparation could not acquire the requested "
                "drawable extent");
      return false_v;
    }
  }
  renderer->frame_prepared = true_v;
  return true_v;
}

uint32_t vkr_metal_packet_renderer_frame_image_index(
    const VkrMetalPacketRenderer *renderer) {
  return renderer->prepared_frame.image_index;
}

/* Rebuild a cached label only when that graph-pass index changes name. */
vkr_internal NSString *
vkr_metal_packet_pass_label(VkrMetalPacketRenderer *renderer, uint32_t index,
                            const VkrRgPass *pass) {
  if (!renderer->pass_labels || index >= renderer->max_passes)
    return nil;
  VkrMetalPacketPassLabel *cached = &renderer->pass_labels[index];
  const uint32_t length =
      (uint32_t)Min(pass->desc.name.length,
                    (uint64_t)VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY - 1u);
  const bool8_t matches =
      cached->value && cached->length == length &&
      (length == 0u ||
       MemCompare(cached->name, pass->desc.name.str, length) == 0);
  if (!matches) {
    [cached->value release];
    cached->value = nil;
    cached->length = length;
    if (length > 0u) {
      MemCopy(cached->name, pass->desc.name.str, length);
      cached->name[length] = '\0';
      cached->value = [[NSString alloc] initWithBytes:cached->name
                                               length:length
                                             encoding:NSUTF8StringEncoding];
    }
  }
  return cached->value;
}

bool8_t
vkr_metal_packet_renderer_submit_packet(VkrMetalPacketRenderer *renderer,
                                        const VkrRenderPacket *packet,
                                        VkrMetalPacketResult *out_result) {
  if (!@available(macOS 26.0, *)) {
    vkr_metal_packet_finish_frame(renderer, false_v, false_v);
    return false_v;
  }

  /* prepare_frame acquires the present target before the packet exists. Patch
     packet-derived graph conditions and viewport here, immediately before
     graph build. */
  renderer->prepared_frame.picking_pending =
      packet->picking && packet->picking->pending;
  renderer->prepared_frame.editor_enabled = packet->frame.editor_enabled;
  renderer->prepared_frame.viewport_width =
      packet->frame.viewport_width ? packet->frame.viewport_width
                                   : renderer->prepared_frame.target_width;
  renderer->prepared_frame.viewport_height =
      packet->frame.viewport_height ? packet->frame.viewport_height
                                    : renderer->prepared_frame.target_height;
  *out_result = (VkrMetalPacketResult){0};
  out_result->source_render_scale = renderer->prepared_frame.render_scale;
  out_result->source_render_width = renderer->prepared_frame.viewport_width;
  out_result->source_render_height = renderer->prepared_frame.viewport_height;
  renderer->prepared_frame.exposure_automatic =
      packet->globals.exposure.mode == VKR_EXPOSURE_MODE_AUTOMATIC;
  /* A requested chain that the viewport is too small to hold is not bloom, so
     the graph sees the same "no bloom" state it would for a frame that never
     asked, rather than a one-mip chain nothing can upsample. */
  renderer->prepared_frame.bloom_mip_count =
      packet->globals.bloom.enabled
          ? vkr_bloom_mip_count(&renderer->bloom_config,
                                renderer->prepared_frame.viewport_width,
                                renderer->prepared_frame.viewport_height)
          : 0u;
  renderer->prepared_frame.bloom_enabled =
      renderer->prepared_frame.bloom_mip_count > 0u;
  renderer->prepared_frame.gtao_depth_mip_count =
      packet->globals.gtao.enabled
          ? vkr_gtao_depth_mip_count(&renderer->gtao_config,
                                     renderer->prepared_frame.viewport_width,
                                     renderer->prepared_frame.viewport_height)
          : 0u;
  renderer->prepared_frame.gtao_enabled =
      renderer->prepared_frame.gtao_depth_mip_count > 0u;
  renderer->gtao_params =
      renderer->prepared_frame.gtao_enabled
          ? vkr_gtao_gpu_params(&renderer->gtao_config, &packet->globals.gtao,
                                packet->globals.view,
                                packet->globals.temporal.jittered_projection,
                                renderer->prepared_frame.viewport_width,
                                renderer->prepared_frame.viewport_height,
                                packet->frame.frame_index,
                                packet->globals.temporal.enabled)
          : (VkrGtaoGpuParams){0};
  renderer->prepared_frame.transmission_pending =
      packet->world && packet->world->transmission_gpu_candidate_count > 0u;
  renderer->prepared_frame.transmission_depth_diagnostic_enabled =
      renderer->prepared_frame.transmission_pending && packet->debug &&
      (packet->debug->transmission_depth_diagnostic_enabled ||
       vkr_renderer_capture_request_contains(
           packet->debug->capture, "transmission_visibility_ids_layer_4"));
  renderer->prepared_frame.transmission_compact_enabled =
      renderer->transmission_compact_enabled &&
      renderer->prepared_frame.transmission_pending;
  renderer->prepared_frame.timing_enabled =
      packet->debug && packet->debug->enable_timing &&
      packet->debug->capture_pass_timestamps;
  renderer->prepared_frame.hzb_history_valid = false_v;
  renderer->selected_hzb_history_instance = UINT32_MAX;
  renderer->selected_temporal_history_instance = UINT32_MAX;
  renderer->selected_exposure_history_instance = UINT32_MAX;
  renderer->current_hzb_view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->globals.projection, packet->globals.view));
  /* Seeded valid so a frame with no candidates publishes an honest zero rather
     than reading as an absent producer; the pack path overwrites it on
     failure. */
  renderer->packet_build = (VkrPacketBuildMetrics){.valid = true_v};
#if VKR_METRICS_ENABLED
  const float64_t candidate_hash_start = vkr_platform_get_absolute_time();
#endif
  renderer->current_hzb_world_epoch =
      vkr_metal_packet_world_occluder_epoch(packet->world);
#if VKR_METRICS_ENABLED
  renderer->packet_build.candidate_hash_ns =
      vkr_metrics_elapsed_ns(candidate_hash_start);
#endif
  uint32_t hzb_mip_count = 1u;
  uint32_t hzb_max_extent = MAX(renderer->prepared_frame.viewport_width,
                                renderer->prepared_frame.viewport_height);
  while (hzb_max_extent > 1u) {
    hzb_max_extent >>= 1u;
    hzb_mip_count++;
  }
  renderer->prepared_frame.hzb_reduce_pass_count = hzb_mip_count - 1u;
  renderer->prepared_frame.transmission_rough_mip_pass_count =
      Min(hzb_mip_count, 6u) - 1u;
  renderer->prepared_frame.shadow_cascade_count =
      packet->shadow
          ? MIN(packet->shadow->cascade_count, VKR_SHADOW_CASCADE_COUNT_MAX)
          : 0u;
  renderer->prepared_frame.sdsm_enabled =
      packet->shadow && packet->shadow->sdsm_enabled;
  renderer->prepared_frame.shadow_cascade_render_mask =
      packet->shadow ? packet->shadow->cascade_render_mask : 0u;
  const uint64_t completed_submit_value = renderer->completion.signaledValue;
  vkr_capture_ring_collect(&renderer->capture_ring, completed_submit_value);
  vkr_metal_packet_collect_retired_image_views(renderer,
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

  if (!vkr_rg_begin_frame(renderer->graph, &renderer->prepared_frame)) {
    log_error("Metal packet stage failed: graph frame allocation");
    vkr_metal_packet_finish_frame(renderer, false_v, false_v);
    return false_v;
  }
  vkr_rg_set_packet(renderer->graph, packet);
  /* Installed before compilation, because seeding a retained subresource reads
     through this provider. */
  vkr_metal_packet_install_retained_provider(renderer);
  if (!vkr_rg_build_from_json(renderer->graph, &renderer->json_graph,
                              &renderer->prepared_frame)) {
    log_error("Metal packet stage failed: graph build");
    goto graph_failure;
  }
  if (!vkr_rg_compile_schedule(renderer->graph)) {
    log_error("Metal packet stage failed: graph compile");
    goto graph_failure;
  }
  if (renderer->graph->passes.length > renderer->max_passes) {
    log_error("Metal packet stage failed: %llu passes exceed capacity %u",
              (unsigned long long)renderer->graph->passes.length,
              renderer->max_passes);
    goto graph_failure;
  }
  if (!vkr_metal_packet_realize_images(renderer)) {
    log_error("Metal packet stage failed: image realization");
    goto graph_failure;
  }
  if (!vkr_metal_packet_realize_buffers(renderer)) {
    log_error("Metal packet stage failed: buffer realization");
    goto graph_failure;
  }
  if (!vkr_metal_packet_select_history_output(renderer)) {
    log_error("Metal packet stage failed: history output selection");
    goto graph_failure;
  }
  if (!vkr_metal_packet_select_hzb_history(renderer)) {
    log_error("Metal packet stage failed: HZB history selection");
    goto graph_failure;
  }
  if (!vkr_metal_packet_select_temporal_history(renderer)) {
    log_error("Metal packet stage failed: temporal history selection");
    goto graph_failure;
  }
  if (!vkr_metal_packet_select_exposure_history(renderer)) {
    log_error("Metal packet stage failed: exposure history selection");
    goto graph_failure;
  }

  const bool8_t picking = renderer->prepared_frame.picking_pending;
  VkrTextureHandle ibl_source = VKR_TEXTURE_HANDLE_INVALID;
  const bool8_t ibl = vkr_metal_packet_ibl_source(packet, &ibl_source);
  const bool8_t ibl_bake = vkr_metal_packet_ibl_needs_bake(renderer, packet);
  const bool8_t pass_timing_requested = packet->debug &&
                                        packet->debug->enable_timing &&
                                        packet->debug->capture_pass_timestamps;
  const bool8_t submission_timing_requested =
      renderer->metalfx_dynamic_resolution_enabled ||
      (packet->debug && packet->debug->enable_timing &&
       packet->debug->capture_submission_timing);
  const bool8_t deferred_diagnostics = packet->world != NULL;
  const bool8_t synchronous_deferred_validation =
      renderer->synchronous_validation_readback && deferred_diagnostics;
  const bool8_t transmission_coverage =
      renderer->prepared_frame.transmission_pending &&
      (pass_timing_requested ||
       renderer->prepared_frame.transmission_compact_enabled ||
       renderer->prepared_frame.transmission_depth_diagnostic_enabled);
  const VkrMetalPacketReadbackLayout readback_layout =
      vkr_metal_packet_readback_layout(
          picking, ibl, deferred_diagnostics,
          renderer->prepared_frame.transmission_pending,
          renderer->prepared_frame.sdsm_enabled,
          renderer->prepared_frame.exposure_automatic,
          renderer->prepared_frame.shadow_cascade_count);
  VkrMetalPacketCapturePlan capture_plans[VKR_CAPTURE_MAX_ITEMS] = {0};
  VkrCaptureBackendItemPlan capture_ring_plans[VKR_CAPTURE_MAX_ITEMS] = {0};
  uint32_t capture_count = 0;
  uint64_t capture_size = readback_layout.prefix_size;
  if (!vkr_metal_packet_capture_plan(renderer, packet,
                                     readback_layout.prefix_size, capture_plans,
                                     &capture_count, &capture_size)) {
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
    const VkrRendererError capture_error =
        vkr_capture_ring_reserve(&renderer->capture_ring, capture_request,
                                 capture_ring_plans, packet->frame.frame_index);
    if (capture_error != VKR_RENDERER_ERROR_NONE) {
      out_result->capture = (VkrCapturePollResult){
          .status = VKR_CAPTURE_STATUS_FAILED, .error = capture_error};
      vkr_metal_packet_finish_frame(renderer, true_v, false_v);
      return false_v;
    }
  }
  VkrMetalPacketFrameUpload frame_upload = {0};
  if (!vkr_metal_packet_prepare_upload(renderer, packet, &frame_upload)) {
    log_error("Metal packet stage failed: frame upload (world=%u ui=%u)",
              packet->world ? packet->world->instance_count : 0,
              packet->ui ? packet->ui->draw_list.vertex_count : 0);
    vkr_metal_packet_rollback_gpu_draw_resource_preparation(renderer, packet);
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
  const VkrMetalMemoryStatus readback_status =
      vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_READBACK, capture_size,
          renderer->completion.signaledValue, &readback_slice, &readback,
          &readback_object);
  if (readback_status != VKR_METAL_MEMORY_STATUS_OK) {
    const VkrRendererError readback_error =
        readback_status == VKR_METAL_MEMORY_STATUS_RING_BUSY
            ? VKR_RENDERER_ERROR_CAPTURE_BUSY
            : VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    log_error("Metal packet stage failed: readback ring %s (%llu bytes)",
              vkr_metal_memory_status_string(readback_status),
              (unsigned long long)capture_size);
    vkr_metal_packet_rollback_gpu_draw_resource_preparation(renderer, packet);
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_UPLOAD, frame_upload.slice);
    vkr_metal_packet_capture_fail_reserved(renderer, capture_request,
                                           readback_error);
    vkr_metal_packet_finish_frame(renderer, true_v, false_v);
    return false_v;
  }
  MemZero(readback.cpu_address, capture_size);

  if (pass_timing_requested) {
    [renderer->active_command_slot->timestamp_heap
        invalidateCounterRange:NSMakeRange(
                                   0u,
                                   (NSUInteger)renderer->graph->passes.length *
                                       2u)];
  }

  [renderer->command_buffer
      beginCommandBufferWithAllocator:renderer->command_allocator];
  id<MTLResidencySet>
      residency_sets[2u + VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX] = {
          (id<MTLResidencySet>)vkr_metal_memory_device_residency_set(
              renderer->memory),
          (id<MTLResidencySet>)vkr_metal_material_table_device_residency_set(
              renderer->materials),
      };
  NSUInteger residency_set_count = 2u;
  for (uint32_t group = 0u;
       group < renderer->active_command_slot->gpu_draw_icb_residency_count;
       ++group)
    residency_sets[residency_set_count++] =
        renderer->active_command_slot->gpu_draw_icb_residencies[group];
  [renderer->command_buffer useResidencySets:residency_sets
                                       count:residency_set_count];
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
      .dst_stages = VKR_GPU_STAGE_COMPUTE_SHADER | VKR_GPU_STAGE_TRANSFER,
      .visibility = VKR_GPU_VISIBILITY_DEVICE,
  };
  bool8_t passed = true_v;
  const VkrRgPass *failed_pass = NULL;
  VkrMetalPacketGraphBufferInstance *deferred_state = NULL;
  VkrMetalPacketGraphBufferInstance *transmission_deferred_state = NULL;
  VkrMetalPacketGraphBufferInstance *sdsm_reduce_state = NULL;
  VkrMetalPacketGraphBufferInstance *exposure_histogram = NULL;
  VkrMetalPacketGraphBufferInstance *exposure_state = NULL;
  for (uint64_t order = 0;
       passed && order < renderer->graph->execution_order.length; ++order) {
    const uint32_t *pass_index =
        vector_get_uint32_t(&renderer->graph->execution_order, order);
    VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, *pass_index);
    if (!pass || pass->culled)
      continue;
    const bool8_t metalfx_pass = vkr_metal_packet_graph_pass_is(
        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_METALFX_TEMPORAL);
    const bool8_t encoder_timing_pass =
        pass_timing_requested && pass->desc.type != VKR_RG_PASS_TYPE_TRANSFER &&
        !metalfx_pass;
    const CFTimeInterval pass_cpu_begin = CACurrentMediaTime();
    const bool8_t last_present_writer =
        vkr_metal_packet_is_last_present_writer(renderer, order);
    if (pass->desc.type == VKR_RG_PASS_TYPE_GRAPHICS) {
      MTL4RenderPassDescriptor *descriptor =
          renderer->render_passes[*pass_index];
      passed = vkr_metal_packet_configure_render_pass(renderer, pass, packet,
                                                      descriptor);
      id<MTL4RenderCommandEncoder> encoder =
          passed ? [renderer->command_buffer
                       renderCommandEncoderWithDescriptor:descriptor]
                 : nil;
      encoder.label = vkr_metal_packet_pass_label(renderer, *pass_index, pass);
      if (encoder_timing_pass && encoder) {
        [encoder writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                    afterStage:MTLRenderStageFragment
                                      intoHeap:renderer->active_command_slot
                                                   ->timestamp_heap
                                       atIndex:(NSUInteger)*pass_index * 2u];
      }
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
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_SHADOW) &&
          pass->desc.repeat_index + 1 ==
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
      if (encoder_timing_pass && passed) {
        [encoder
            writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                               afterStage:MTLRenderStageFragment
                                 intoHeap:renderer->active_command_slot
                                              ->timestamp_heap
                                  atIndex:(NSUInteger)*pass_index * 2u + 1u];
      }
      [encoder endEncoding];
      out_result->graphics_pass_count++;
    } else if (metalfx_pass) {
      /* MetalFX owns its internal Metal 4 encoders. The staging producer and
         each downstream consumer retain the graph-authored queue barriers on
         their own encoders, so no empty compatibility encoder is inserted. */
      passed = vkr_metal_packet_encode_metalfx_temporal(renderer, pass, packet);
      out_result->compute_pass_count++;
    } else {
      id<MTL4ComputeCommandEncoder> encoder =
          [renderer->command_buffer computeCommandEncoder];
      encoder.label = vkr_metal_packet_pass_label(renderer, *pass_index, pass);
      if (encoder_timing_pass && encoder) {
        [encoder writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                      intoHeap:renderer->active_command_slot
                                                   ->timestamp_heap
                                       atIndex:(NSUInteger)*pass_index * 2u];
      }
      passed = encoder && vkr_metal_packet_encode_consumers(
                              encoder, pass, &out_result->dependency_count);
      if (passed && pass->desc.type == VKR_RG_PASS_TYPE_TRANSFER)
        passed = vkr_metal_packet_encode_transfer(renderer, pass, encoder,
                                                  &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_IBL_BAKE))
        passed = vkr_metal_packet_encode_ibl(renderer, packet, encoder,
                                             &frame_upload, out_result);
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TEMPORAL_TRANSFORM))
        passed = vkr_metal_packet_encode_temporal_transform(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_GBUFFER_RESOLVE))
        passed = vkr_metal_packet_encode_gbuffer_resolve(
            renderer, pass, packet, encoder, &frame_upload);
      for (uint32_t gtao_kind =
               VKR_METAL_PACKET_GRAPH_EXECUTOR_GTAO_DEPTH_PREFILTER;
           passed && gtao_kind <= VKR_METAL_PACKET_GRAPH_EXECUTOR_GTAO_DENOISE;
           ++gtao_kind) {
        if (!vkr_metal_packet_graph_pass_is(
                pass, (VkrMetalPacketGraphExecutorKind)gtao_kind))
          continue;
        passed = vkr_metal_packet_encode_gtao(
            renderer, pass, encoder, &frame_upload,
            (VkrMetalPacketGraphExecutorKind)gtao_kind);
      }
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_DEFERRED_LIGHTING))
        passed = vkr_metal_packet_encode_deferred_lighting(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TEMPORAL_RESOLVE))
        passed = vkr_metal_packet_encode_temporal_resolve(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_EXPOSURE_HISTOGRAM))
        passed = vkr_metal_packet_encode_exposure_histogram(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_EXPOSURE_HISTOGRAM))
        exposure_histogram = vkr_metal_packet_pass_buffer(renderer, pass, 1u);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_EXPOSURE_RESOLVE))
        passed = vkr_metal_packet_encode_exposure_resolve(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_EXPOSURE_RESOLVE))
        exposure_state = vkr_metal_packet_pass_buffer(renderer, pass, 1u);
      /* All four bloom pass kinds share one encoder; only the pipeline and the
         coarse-source selection differ, so the kind is carried in rather than
         split into three near-identical call sites. */
      for (uint32_t bloom_kind =
               VKR_METAL_PACKET_GRAPH_EXECUTOR_BLOOM_PREFILTER;
           passed &&
           bloom_kind <= VKR_METAL_PACKET_GRAPH_EXECUTOR_BLOOM_COMBINE;
           ++bloom_kind) {
        if (!vkr_metal_packet_graph_pass_is(
                pass, (VkrMetalPacketGraphExecutorKind)bloom_kind))
          continue;
        passed = vkr_metal_packet_encode_bloom(
            renderer, pass, packet, encoder, &frame_upload,
            (VkrMetalPacketGraphExecutorKind)bloom_kind);
      }
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_DOWNSAMPLE))
        passed = vkr_metal_packet_encode_bloom(
            renderer, pass, packet, encoder, &frame_upload,
            VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_DOWNSAMPLE);
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_SHADE))
        passed = vkr_metal_packet_encode_transmission_shade(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_COVERAGE))
        passed = vkr_metal_packet_encode_transmission_coverage(
            renderer, pass, encoder, &frame_upload);
      if (passed &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_COMPACT))
        passed = vkr_metal_packet_encode_transmission_compact(
            renderer, pass, encoder, &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING_RESOLVE))
        passed = vkr_metal_packet_encode_picking_resolve(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_HZB_BUILD))
        passed = vkr_metal_packet_encode_hzb_build(renderer, pass, encoder,
                                                   &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_SDSM_REDUCE))
        passed = vkr_metal_packet_encode_sdsm_reduce(renderer, pass, encoder,
                                                     &frame_upload);
      if (passed && vkr_metal_packet_graph_pass_is(
                        pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_SDSM_REDUCE))
        sdsm_reduce_state = vkr_metal_packet_pass_buffer(renderer, pass, 2u);
      if (passed && deferred_diagnostics &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_GBUFFER_RESOLVE))
        deferred_state = vkr_metal_packet_pass_buffer(renderer, pass, 7u);
      if (passed && deferred_diagnostics &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_SHADE))
        transmission_deferred_state =
            vkr_metal_packet_pass_buffer(renderer, pass, 3u);
      if (passed && transmission_coverage &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_COVERAGE))
        transmission_deferred_state =
            vkr_metal_packet_pass_buffer(renderer, pass, 1u);
      if (passed &&
          (vkr_metal_packet_graph_pass_is(
               pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY) ||
           vkr_metal_packet_graph_pass_is(
               pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_PREFIX) ||
           vkr_metal_packet_graph_pass_is(
               pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_ENCODE) ||
           vkr_metal_packet_graph_pass_is(
               pass,
               VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY) ||
           vkr_metal_packet_graph_pass_is(
               pass,
               VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX) ||
           vkr_metal_packet_graph_pass_is(
               pass,
               VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE)))
        passed = vkr_metal_packet_encode_gpu_draw_compute(
            renderer, pass, packet, encoder, &frame_upload);
      if (passed && picking)
        passed = vkr_metal_packet_encode_picking_readback(
            renderer, pass, packet, encoder, (id<MTLBuffer>)readback_object,
            readback_slice.offset + 8);
      if (passed)
        passed = vkr_metal_packet_encode_producers(
            renderer, encoder, *pass_index, &out_result->dependency_count);
      if (passed && ibl_bake &&
          vkr_metal_packet_graph_pass_is(
              pass, VKR_METAL_PACKET_GRAPH_EXECUTOR_IBL_BAKE)) {
        passed = vkr_metal_packet_encode_dependency(
            encoder, &ibl_capture_dependency, true_v);
        out_result->dependency_count++;
      }
      if (passed && (capture_count > 0 || deferred_diagnostics ||
                     transmission_coverage || readback_layout.sdsm_size > 0u ||
                     readback_layout.exposure_size > 0u)) {
        passed = vkr_metal_packet_encode_dependency(
            encoder, &batch_capture_dependency, true_v);
        out_result->dependency_count++;
      }
      if (encoder_timing_pass && passed) {
        [encoder
            writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                 intoHeap:renderer->active_command_slot
                                              ->timestamp_heap
                                  atIndex:(NSUInteger)*pass_index * 2u + 1u];
      }
      [encoder endEncoding];
      if (pass->desc.type == VKR_RG_PASS_TYPE_TRANSFER)
        out_result->transfer_pass_count++;
      else
        out_result->compute_pass_count++;
    }
    if (!passed)
      failed_pass = pass;
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
      timing->unavailable_reason =
          encoder_timing_pass
              ? VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY
              : (pass_timing_requested
                     ? VKR_RENDERER_IMPL_GPU_TIMING_REASON_UNSUPPORTED_TIMESTAMP_SCOPE
                     : VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED);
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
    if (passed && (capture_count > 0 || deferred_diagnostics ||
                   transmission_coverage || readback_layout.sdsm_size > 0u ||
                   readback_layout.exposure_size > 0u)) {
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
                                     readback_layout.ibl_prefilter + mip * 8u
              destinationBytesPerRow:8
            destinationBytesPerImage:8
                             options:MTLBlitOptionNone];
      }
    }
    if (passed && (deferred_diagnostics || transmission_coverage)) {
      if (!deferred_state) {
        passed = deferred_diagnostics ? false_v : passed;
      } else {
        if (deferred_diagnostics) {
          [capture_encoder
                 copyFromBuffer:(id<MTLBuffer>)
                                    deferred_state->resource.metal_buffer
                   sourceOffset:0u
                       toBuffer:(id<MTLBuffer>)readback_object
              destinationOffset:readback_slice.offset +
                                readback_layout.deferred_diagnostics
                           size:readback_layout.deferred_diagnostics_size];
        }
      }
      if (passed && renderer->prepared_frame.transmission_pending) {
        if (!transmission_deferred_state) {
          passed = false_v;
        } else {
          [capture_encoder
                 copyFromBuffer:(id<MTLBuffer>)transmission_deferred_state
                                    ->resource.metal_buffer
                   sourceOffset:0u
                       toBuffer:(id<MTLBuffer>)readback_object
              destinationOffset:readback_slice.offset +
                                readback_layout.transmission_diagnostics
                           size:sizeof(VkrMetalPacketTransmissionDiagnostics)];
        }
      }
    }
    if (passed && readback_layout.sdsm_size > 0u) {
      if (!sdsm_reduce_state) {
        passed = false_v;
      } else {
        [capture_encoder
               copyFromBuffer:(id<MTLBuffer>)
                                  sdsm_reduce_state->resource.metal_buffer
                 sourceOffset:0u
                     toBuffer:(id<MTLBuffer>)readback_object
            destinationOffset:readback_slice.offset + readback_layout.sdsm
                         size:readback_layout.sdsm_size];
      }
    }
    if (passed && readback_layout.exposure_size > 0u) {
      if (!exposure_histogram || !exposure_state) {
        passed = false_v;
      } else {
        [capture_encoder
               copyFromBuffer:(id<MTLBuffer>)
                                  exposure_state->resource.metal_buffer
                 sourceOffset:0u
                     toBuffer:(id<MTLBuffer>)readback_object
            destinationOffset:readback_slice.offset + readback_layout.exposure
                         size:sizeof(VkrExposureGpuState)];
        [capture_encoder
               copyFromBuffer:(id<MTLBuffer>)
                                  exposure_histogram->resource.metal_buffer
                 sourceOffset:0u
                     toBuffer:(id<MTLBuffer>)readback_object
            destinationOffset:readback_slice.offset + readback_layout.exposure +
                              sizeof(VkrExposureGpuState)
                         size:sizeof(VkrExposureGpuHistogram)];
      }
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
      log_error("Metal packet stage failed: encode pass '%.*s' executor %u",
                (int)failed_pass->desc.name.length, failed_pass->desc.name.str,
                failed_pass->desc.executor_id);
    } else {
      log_error("Metal packet stage failed: final capture encoding");
    }
    vkr_metal_packet_rollback_gpu_draw_resource_preparation(renderer, packet);
    vkr_metal_packet_abandon_ibl_sh_candidate(renderer, &frame_upload);
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_READBACK, readback_slice);
    vkr_metal_memory_device_cancel_ring(
        renderer->memory, VKR_METAL_RING_KIND_UPLOAD, frame_upload.slice);
    vkr_metal_packet_capture_fail_reserved(
        renderer, capture_request, VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED);
    vkr_metal_packet_finish_frame(renderer, true_v, false_v);
    return false_v;
  }

  const uint64_t metalfx_temporal_wait_value =
      renderer->metalfx_enabled &&
              renderer->selected_temporal_history_instance != UINT32_MAX
          ? vkr_metal_packet_graph_buffer_instance(
                renderer, renderer->temporal_transform_history_handle,
                renderer->selected_temporal_history_instance)
                ->history_producer_submit_value
          : 0u;
  const uint64_t submit_value = ++renderer->submit_value;
  vkr_ibl_sh_pool_reference(&renderer->sh_pool,
                            frame_upload.ibl_sh_candidate_slot, submit_value);
  if (packet->lighting && packet->lighting->ibl_enabled) {
    vkr_ibl_sh_pool_reference(&renderer->sh_pool, renderer->ibl_sh_slot,
                              submit_value);
    for (uint32_t i = 0u; i < packet->lighting->ibl_probe_count; ++i) {
      vkr_ibl_sh_pool_reference(&renderer->sh_pool,
                                packet->lighting->ibl_probes[i].sh_slot,
                                submit_value);
    }
  }
  vkr_metal_packet_mark_graph_images_submitted(renderer, submit_value);
  vkr_metal_packet_mark_hzb_history_submitted(renderer, submit_value);
  vkr_metal_packet_mark_temporal_submitted(renderer, submit_value);
  vkr_metal_packet_mark_exposure_submitted(renderer, submit_value);
  vkr_metal_packet_mark_graph_buffers_submitted(renderer, submit_value);
  VkrMetalPacketCommandSlot *command_slot = renderer->active_command_slot;
  command_slot->submit_value = submit_value;
  command_slot->timestamp_entry_count =
      pass_timing_requested ? (uint32_t)renderer->graph->passes.length * 2u
                            : 0u;
  command_slot->pass_timing_requested = pass_timing_requested;
  VkrMetalPacketCommitFeedbackRecord *commit_feedback =
      submission_timing_requested
          ? vkr_metal_packet_acquire_commit_feedback(renderer)
          : NULL;
  command_slot->commit_feedback = commit_feedback;
  out_result->gpu_submission_unavailable_reason =
      submission_timing_requested
          ? (commit_feedback
                 ? VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY
                 : VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_UNAVAILABLE)
          : VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED;
  command_slot->gpu_draw_diagnostics_requested = deferred_diagnostics;
  command_slot->sdsm_requested = readback_layout.sdsm_size > 0u;
  command_slot->exposure_requested = readback_layout.exposure_size > 0u;
  command_slot->shadow_cascade_count =
      renderer->prepared_frame.shadow_cascade_count;
  if (deferred_diagnostics) {
    command_slot->gpu_draw_diagnostics_readback =
        (const uint8_t *)readback.cpu_address +
        readback_layout.deferred_diagnostics;
  }
  command_slot->sdsm_readback =
      readback_layout.sdsm_size > 0u
          ? (const uint8_t *)readback.cpu_address + readback_layout.sdsm
          : NULL;
  command_slot->exposure_readback =
      readback_layout.exposure_size > 0u
          ? (const uint8_t *)readback.cpu_address + readback_layout.exposure
          : NULL;
  command_slot->transmission_coverage_requested = transmission_coverage;
  if (renderer->prepared_frame.transmission_pending) {
    command_slot->transmission_coverage_readback =
        (const uint8_t *)readback.cpu_address +
        readback_layout.transmission_diagnostics;
  }
  if (transmission_coverage) {
    command_slot->transmission_coverage_extent[0] =
        renderer->prepared_frame.viewport_width;
    command_slot->transmission_coverage_extent[1] =
        renderer->prepared_frame.viewport_height;
  }
  id<MTL4CommandBuffer> submissions[] = {renderer->command_buffer};
  if (metalfx_temporal_wait_value > renderer->completion.signaledValue)
    [renderer->queue waitForEvent:renderer->completion
                            value:metalfx_temporal_wait_value];
  if (renderer->drawable)
    [renderer->queue waitForDrawable:renderer->drawable];
  out_result->submit_value = submit_value;
  out_result->source_frame_index = packet->frame.frame_index;
  if (readback_layout.sdsm_size > 0u) {
    const float32_t a = packet->globals.projection.elements[10];
    const float32_t b = packet->globals.projection.elements[14];
    out_result->shadow_depth_range = (VkrShadowDepthRangeSample){
        .projection_convention = 0u,
        .source_depth_linearize = {a, b, 0.0f, 0.0f},
        .source_near = a != 0.0f ? b / a : 0.0f,
        .source_far = (1.0f + a) != 0.0f ? b / (1.0f + a) : 0.0f,
        .source_frame_index = packet->frame.frame_index,
        .source_projection_generation =
            vkr_shadow_projection_generation(&packet->globals.projection),
        .source_scene_generation = packet->frame.scene_generation,
        .submit_value = submit_value,
    };
  }
  command_slot->pending_result = *out_result;
  command_slot->result_pending = true_v;
  if (commit_feedback)
    [renderer->queue commit:submissions
                      count:1
                    options:commit_feedback->options];
  else
    [renderer->queue commit:submissions count:1];
  [renderer->queue signalEvent:renderer->completion value:submit_value];
  if (command_slot->candidate_residency_pending) {
    command_slot->candidate_residency =
        command_slot->pending_candidate_residency;
    command_slot->candidate_residency_pending = false_v;
  }
  /* Past the commit, so retained contents are now proven to have been written.
     Every earlier failure path returns without reaching here, which is the
     ADR-029 rollback: a cancelled frame commits nothing and the previous
     contents stay authoritative. */
  vkr_rg_commit_retained_state(renderer->graph);
  if (frame_upload.ibl_sh_candidate_slot != VKR_SH_SLOT_BLACK) {
    VkrMetalPacketTexture *sh_source =
        vkr_metal_packet_resolve_texture(renderer, ibl_source);
    if (sh_source &&
        vkr_ibl_sh_pool_publish(&renderer->sh_pool,
                                frame_upload.ibl_sh_candidate_slot) ==
            VKR_SH_POOL_STATUS_OK) {
      if (sh_source->ibl_sh_slot != VKR_SH_SLOT_BLACK)
        (void)vkr_ibl_sh_pool_retire(&renderer->sh_pool,
                                     sh_source->ibl_sh_slot);
      sh_source->ibl_sh_slot = frame_upload.ibl_sh_candidate_slot;
    } else {
      (void)vkr_ibl_sh_pool_retire(&renderer->sh_pool,
                                   frame_upload.ibl_sh_candidate_slot);
      renderer->ibl_sh_slot = frame_upload.ibl_sh_previous_slot;
    }
  }
  if (renderer->drawable) {
    [renderer->queue signalDrawable:renderer->drawable];
    [renderer->drawable present];
  }
  const bool8_t capture_submitted =
      capture_count == 0 ||
      vkr_capture_ring_submit(
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
    if (readback_layout.exposure_size > 0u) {
      VkrExposureGpuState state = {0};
      MemCopy(&state,
              (const uint8_t *)readback.cpu_address + readback_layout.exposure,
              sizeof(state));
      (void)vkr_capture_ring_set_display_exposure(
          &renderer->capture_ring, submit_value, state.exposure_multiplier);
    }
    vkr_capture_ring_collect(&renderer->capture_ring,
                             renderer->completion.signaledValue);
    if (transmission_coverage) {
      VkrMetalPacketTransmissionDiagnostics transmission_diagnostics = {0};
      MemCopy(&transmission_diagnostics,
              (uint8_t *)readback.cpu_address +
                  readback_layout.transmission_diagnostics,
              sizeof(transmission_diagnostics));
      MemCopy(out_result->transmission_covered_pixels,
              transmission_diagnostics.covered_pixels,
              sizeof(out_result->transmission_covered_pixels));
      out_result->transmission_coverage_extent[0] =
          renderer->prepared_frame.viewport_width;
      out_result->transmission_coverage_extent[1] =
          renderer->prepared_frame.viewport_height;
      for (uint32_t layer = 0u;
           layer < VKR_METAL_PACKET_TRANSMISSION_LAYER_COUNT; ++layer) {
        out_result->transmission_compact_overflow_count +=
            transmission_diagnostics.compact_overflow[layer];
      }
      out_result->has_transmission_coverage = true_v;
    }

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
      for (uint32_t mip = 0; mip < VKR_IBL_PREFILTER_MIP_COUNT; ++mip) {
        vkr_metal_packet_decode_half4((uint8_t *)readback.cpu_address +
                                          readback_layout.ibl_prefilter +
                                          mip * 8u,
                                      out_result->ibl_prefilter[mip]);
      }
      out_result->ibl_prefilter_size = VKR_IBL_PREFILTER_SIZE;
      out_result->ibl_prefilter_mip_count = VKR_IBL_PREFILTER_MIP_COUNT;
      out_result->has_ibl_convolution = true_v;
    }
    if (synchronous_deferred_validation) {
      out_result->gpu_candidate_count = renderer->gpu_draw_count;
      out_result->transmission_gpu_candidate_count =
          renderer->transmission_gpu_draw_count;
      vkr_metal_packet_decode_gpu_draw_diagnostics(
          out_result,
          (const uint8_t *)readback.cpu_address +
              readback_layout.deferred_diagnostics,
          renderer->prepared_frame.shadow_cascade_count,
          renderer->prepared_frame.transmission_pending
              ? (const uint8_t *)readback.cpu_address +
                    readback_layout.transmission_diagnostics
              : NULL);
      out_result->hzb_history_valid =
          renderer->prepared_frame.hzb_history_valid;
      log_info(
          "Metal deferred validation: candidates=%u visible=%u buckets="
          "%u/%u/%u/%u overflow=%u resolve_invalid=%u hzb_rejected=%u",
          out_result->gpu_candidate_count, out_result->gpu_visible_count,
          out_result->gpu_bucket_counts[0], out_result->gpu_bucket_counts[1],
          out_result->gpu_bucket_counts[2], out_result->gpu_bucket_counts[3],
          out_result->gpu_overflow_count, out_result->gpu_resolve_invalid_count,
          out_result->gpu_occlusion_culled_count);
    }
  }
  if (ibl_bake) {
    renderer->ibl_source = ibl_source;
    renderer->ibl_ready = true_v;
  }
  if (capture_count > 0) {
    (void)vkr_capture_ring_poll(
        &renderer->capture_ring, capture_request->request_id,
        renderer->completion.signaledValue, &out_result->capture);
  }
  out_result->packet_build = renderer->packet_build;
  out_result->hzb_history_valid = renderer->prepared_frame.hzb_history_valid;
  out_result->resize_count = renderer->resize_count;
  out_result->pipeline_count = 34u;
  out_result->pipeline_archive_warm = renderer->pipeline_archive_warm;
  out_result->pipeline_archive_written = renderer->pipeline_archive_written;
  if (deferred_diagnostics) {
    out_result->gpu_candidate_count = renderer->gpu_draw_count;
    out_result->transmission_gpu_candidate_count =
        renderer->transmission_gpu_draw_count;
  }
  vkr_metal_memory_device_get_metrics(renderer->memory, &out_result->memory);
  vkr_metal_material_table_device_get_metrics(renderer->materials,
                                              &out_result->materials);
  command_slot->pending_result = *out_result;
  if (renderer->synchronous_validation_readback) {
    (void)vkr_metal_packet_complete_command_slot(renderer, command_slot,
                                                 false_v);
  }
  vkr_metal_packet_detach_active_command_slot(renderer);
  vkr_metal_packet_finish_frame(renderer, true_v, true_v);
  return true_v;

graph_failure:
  vkr_metal_packet_finish_frame(renderer, true_v, false_v);
  return false_v;
}

#include "vkr_render_graph_frame.h"

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

/* A cold record wider than one draw-root cell: it takes `size` bytes of
 * contiguous cells, so per-draw cells stay fixed at 512 B. */
vkr_internal void *
vkr_metal_packet_next_root_span(VkrMetalPacketFrameUpload *upload,
                                uint64_t size, uint64_t *out_gpu_address) {
  const uint64_t cell_count = (size + VKR_METAL_PACKET_DRAW_ROOT_STRIDE - 1u) /
                              VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  if (upload->root_cursor > upload->root_capacity ||
      cell_count > upload->root_capacity - upload->root_cursor)
    return NULL;
  const uint64_t offset =
      (uint64_t)upload->root_cursor * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  upload->root_cursor += (uint32_t)cell_count;
  *out_gpu_address = upload->root_gpu + offset;
  void *root = upload->root_cpu + offset;
  MemZero(root, size);
  return root;
}

/* Frame roots are shared cold records spanning their ABI-sized cells. */
vkr_internal VkrMetalPacketFrameRoot *
vkr_metal_packet_next_frame_root(VkrMetalPacketFrameUpload *upload,
                                 uint64_t *out_gpu_address) {
  return (VkrMetalPacketFrameRoot *)vkr_metal_packet_next_root_span(
      upload, sizeof(VkrMetalPacketFrameRoot), out_gpu_address);
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

/* Ratio that brings a history written by `producer_submit_value` to this
   frame's pre-exposure. Both scales are powers of two, so it is exact. A
   producer outside the record window is treated as this frame's scale. */
vkr_internal float32_t vkr_metal_packet_history_pre_exposure_scale(
    const VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    uint64_t producer_submit_value) {
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_PRE_EXPOSURE_HISTORY; ++i) {
    const VkrMetalPacketPreExposureRecord *record =
        &renderer->pre_exposure_history[i];
    if (record->submit_value == producer_submit_value &&
        producer_submit_value != 0u)
      return ldexpf(1.0f, packet->exposure.pre_exposure_stops - record->stops);
  }
  return 1.0f;
}

/* Cancellation proves the command buffer carrying the recorded bakes was not
   submitted. The bakes stay queued for the next frame; their SH candidates
   return to the pool so a retry does not leak a slot. */
vkr_internal void vkr_metal_packet_abandon_ibl_bake_recordings(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketFrameUpload *upload) {
  if (!upload->ibl_bake_recorded)
    return;
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    VkrMetalPacketPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    if (!job->recorded)
      continue;
    if (job->sh_slot != VKR_SH_SLOT_BLACK)
      (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool, job->sh_slot);
    job->sh_slot = VKR_SH_SLOT_BLACK;
    job->recorded = false_v;
  }
  upload->ibl_bake_recorded = false_v;
}

vkr_internal bool8_t vkr_metal_packet_create_frame_root(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketFrameUpload *upload, uint64_t instances_gpu,
    uint32_t target_width, uint32_t target_height, Mat4 view_projection,
    bool8_t lighting_pass, uint64_t *out_root_gpu, uint32_t *out_frame_flags) {
  VkrMetalPacketFrameRoot *frame_root =
      vkr_metal_packet_next_frame_root(upload, out_root_gpu);
  if (!frame_root)
    return false_v;
  const VkrPacketFrameConstants frame_constants =
      vkr_packet_derive_frame_constants(packet, target_width, target_height);
  /* Global lighting samples the prefilter queued with its source. A queued
     bake records in this frame's IBL.Bake pass, whose producer barrier orders
     it before every later encoder. */
  VkrTextureHandle global_ibl_source = VKR_TEXTURE_HANDLE_INVALID;
  VkrMetalPacketTexture *global_prefilter = NULL;
  if (vkr_metal_packet_ibl_source(packet, &global_ibl_source)) {
    const VkrMetalPacketTexture *source =
        vkr_metal_packet_resolve_texture(renderer, global_ibl_source);
    global_prefilter =
        source
            ? vkr_metal_packet_resolve_texture(renderer, source->ibl_prefilter)
            : NULL;
  }
  const bool8_t has_global_ibl = global_prefilter != NULL;
  if (has_global_ibl)
    global_prefilter->last_use_submit_value = renderer->submit_value + 1u;
  const bool8_t ibl_resources_ready =
      has_global_ibl ||
      (packet->input.lighting && packet->input.lighting->ibl_enabled &&
       packet->input.lighting->ibl_probe_count > 0u);
  *out_frame_flags =
      vkr_packet_derive_frame_flags(packet, lighting_pass, ibl_resources_ready);
  *frame_root = (VkrMetalPacketFrameRoot){
      .instances = instances_gpu,
      .view_projection = vkr_metal_packet_slang_draw_matrix(view_projection),
      .materials =
          vkr_metal_material_table_device_gpu_address(renderer->materials),
      .sh_coefficients_address = renderer->sh_coefficients.gpuAddress,
      .prefilter_texture_id = has_global_ibl
                                  ? global_prefilter->resource.resource_id
                                  : renderer->ibl_black_prefilter.resource_id,
      /* The cloud-lit sky light replaces the source's slot for frame roots
         prepared after its pass (ADR-074). */
      .sh_global_slot = !has_global_ibl ? VKR_SH_SLOT_BLACK
                        : upload->cloud_sh_slot != VKR_SH_SLOT_BLACK
                            ? upload->cloud_sh_slot
                            : renderer->ibl_sh_slot,
      .pre_exposure = frame_constants.pre_exposure,
      .view_position = frame_constants.view_position,
      .prefilter_mip_count = frame_constants.prefilter_mip_count,
      .flags = *out_frame_flags,
      /* The shared constants pre-expose the IBL gain; the global source's
         own bake scale is removed here. */
      .ibl_controls =
          {frame_constants.ibl_controls.x *
               (has_global_ibl ? ldexpf(1.0f, -global_prefilter->radiance_stops)
                               : 1.0f),
           frame_constants.ibl_controls.y, frame_constants.ibl_controls.z,
           frame_constants.ibl_controls.w},
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
      .custom_time = packet->clock_seconds,
      .shadow_texture_id = upload->shadow_texture_id,
      .shadow_cascades = upload->shadow_cascades_gpu,
      .local_shadow_texture_id = upload->local_shadow_texture_id,
      .local_shadow_views = upload->local_shadow_views_gpu,
      .transmission_texture_id = upload->transmission_texture_id,
      .dfg_texture_id = renderer->dfg_lut.resource_id,
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
      .ibl_probes = upload->ibl_probes_gpu,
      .ibl_probe_count =
          packet->input.lighting ? packet->input.lighting->ibl_probe_count : 0,
  };
  if (!has_global_ibl && (*out_frame_flags & VKR_PACKET_FRAME_FLAG_IBL) != 0u) {
    renderer->ibl_black_last_use_submit_value = renderer->submit_value + 1u;
  }
  frame_root->diffuse_volume_texture_id = upload->diffuse_volume_texture_id;
  frame_root->diffuse_volume_params = upload->diffuse_volume_params_gpu;
  frame_root->ltc = upload->ltc_gpu;
  frame_root->sheen = upload->sheen_gpu;
  frame_root->fog = upload->fog_gpu;
  frame_root->sky = upload->sky_gpu;
  frame_root->lightmap = upload->lightmap_gpu;
  frame_root->decals = upload->decals_gpu;
  frame_root->terrain_materials =
      vkr_metal_material_table_device_terrain_gpu_address(renderer->materials);
  frame_root->custom_materials =
      vkr_metal_material_table_device_custom_gpu_address(renderer->materials);
  frame_root->transmission_materials =
      vkr_metal_material_table_device_transmission_gpu_address(
          renderer->materials);
  uint64_t anisotropy_gpu = 0u;
  VkrMetalPacketAnisotropy *anisotropy =
      vkr_metal_packet_next_root(upload, sizeof(*anisotropy), &anisotropy_gpu);
  if (!anisotropy)
    return false_v;
  *anisotropy = (VkrMetalPacketAnisotropy){
      .table0 = renderer->anisotropy_luts[0].resource_id,
      .table1 = renderer->anisotropy_luts[1].resource_id,
      .table2 = renderer->anisotropy_luts[2].resource_id,
  };
  frame_root->anisotropy = anisotropy_gpu;
  renderer->ltc_last_use_submit_value = renderer->submit_value + 1u;
  renderer->sheen_lut_last_use_submit_value = renderer->submit_value + 1u;
  renderer->dfg_lut_last_use_submit_value = renderer->submit_value + 1u;
  renderer->anisotropy_lut_last_use_submit_value = renderer->submit_value + 1u;
  return true_v;
}

vkr_internal void vkr_metal_packet_record_prepared_draws(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const VkrMetalPacketPreparedDraw *draws, uint32_t draw_count,
    uint32_t target_width, uint32_t target_height, uint64_t root_gpu)
    API_AVAILABLE(macos(26.0)) {
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  MTLCullMode active_cull_mode = MTLCullModeNone;
  [encoder setCullMode:active_cull_mode];
  [encoder setDepthStencilState:depth_state];
  [encoder setRenderPipelineState:pipeline];
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrMetalPacketPreparedDraw *draw = &draws[i];
    [encoder setFrontFacingWinding:draw->winding];
    if (draw->cull_mode != active_cull_mode) {
      [encoder setCullMode:draw->cull_mode];
      active_cull_mode = draw->cull_mode;
    }
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
    root_gpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  }
}

vkr_internal bool8_t vkr_metal_packet_prepare_draws(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketDirectPass *prepared,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const VkrMetalPacketPreparedDraw *draws, uint32_t draw_count,
    uint64_t instances_gpu, uint32_t target_width, uint32_t target_height,
    Mat4 view_projection, const VkrPreparedFrame *packet, bool8_t lighting_pass,
    VkrMetalPacketFrameUpload *upload, uint32_t *domain_draw_count,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  if (draw_count == 0)
    return true_v;
  uint64_t frame_root_gpu = 0;
  uint32_t frame_flags = 0u;
  if (!vkr_metal_packet_create_frame_root(
          renderer, packet, upload, instances_gpu, target_width, target_height,
          view_projection, lighting_pass, &frame_root_gpu, &frame_flags)) {
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
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrMetalPacketPreparedDraw *draw = &draws[i];
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
    root_cpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
    root_gpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  }
  *prepared = (VkrMetalPacketDirectPass){
      .pipeline = pipeline,
      .depth = depth_state,
      .draws = draws,
      .count = draw_count,
      .width = target_width,
      .height = target_height,
      .root_gpu = upload->root_gpu + root_offset,
  };
  *domain_draw_count += draw_count;
  result->indexed_draw_count += draw_count;
  return true_v;
}

/* The native root remains the prefix; command arguments use its reserved
 * fixed-stride upload slot and are consumed only by the CPU recorder. */
typedef struct VkrMetalPacketPreparedTextDraw {
  VkrMetalPacketTextRoot root;
  uint64_t index_address;
  uint64_t index_length;
  uint32_t index_count;
} VkrMetalPacketPreparedTextDraw;

typedef struct VkrMetalPacketPreparedUiDraw {
  VkrMetalPacketUiRoot root;
  MTLScissorRect scissor;
  id<MTLRenderPipelineState> pipeline;
  uint64_t index_address;
  uint64_t index_length;
  uint32_t index_count;
} VkrMetalPacketPreparedUiDraw;

_Static_assert(sizeof(VkrMetalPacketPreparedTextDraw) <=
                   VKR_METAL_PACKET_DRAW_ROOT_STRIDE,
               "Prepared text draw must fit its upload root slot");
_Static_assert(sizeof(VkrMetalPacketPreparedUiDraw) <=
                   VKR_METAL_PACKET_DRAW_ROOT_STRIDE,
               "Prepared UI draw must fit its upload root slot");

vkr_internal void vkr_metal_packet_record_prepared_text_draws(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const uint8_t *root_cpu, uint64_t root_gpu, uint32_t draw_count,
    uint32_t target_width, uint32_t target_height) API_AVAILABLE(macos(26.0)) {
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setDepthStencilState:depth_state];
  [encoder setRenderPipelineState:pipeline];
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrMetalPacketPreparedTextDraw *draw =
        (const VkrMetalPacketPreparedTextDraw *)root_cpu;
    [renderer->argument_table setAddress:root_gpu atIndex:1];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:draw->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:draw->index_address
                 indexBufferLength:draw->index_length
                     instanceCount:1
                        baseVertex:0
                      baseInstance:0];
    root_cpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
    root_gpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  }
}

vkr_internal bool8_t vkr_metal_packet_prepare_text_draws(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketTextPass *prepared_pass,
    id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth_state,
    const VkrPreparedTextDraw *draws, uint32_t draw_count,
    uint32_t text_upload_start, uint32_t target_width, uint32_t target_height,
    Mat4 view_projection, bool8_t ui_domain, VkrMetalPacketFrameUpload *upload,
    uint32_t *domain_draw_count, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  if (draw_count == 0u)
    return true_v;
  if (text_upload_start > upload->world_text_count ||
      draw_count > upload->world_text_count - text_upload_start ||
      draw_count > upload->root_capacity - upload->root_cursor)
    return false_v;
  const uint64_t root_offset =
      (uint64_t)upload->root_cursor * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  uint8_t *root_cpu = upload->root_cpu + root_offset;
  const uint64_t root_gpu = upload->root_gpu + root_offset;
  upload->root_cursor += draw_count;
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    const VkrMetalPacketTextUpload *text_upload =
        &upload->text_uploads[text_upload_start + i];
    VkrMetalPacketTexture *atlas =
        vkr_metal_packet_resolve_texture(renderer, draw->atlas);
    if (!atlas || text_upload->vertices_gpu == 0u ||
        text_upload->indices_gpu == 0u || text_upload->indices_length == 0u)
      return false_v;
    VkrMetalPacketPreparedTextDraw *prepared =
        (VkrMetalPacketPreparedTextDraw
             *)(root_cpu + (uint64_t)i * VKR_METAL_PACKET_DRAW_ROOT_STRIDE);
    *prepared = (VkrMetalPacketPreparedTextDraw){
        .root =
            {
                .vertices = text_upload->vertices_gpu,
                .atlas_texture_id = atlas->resource.resource_id,
                .model = draw->model,
                .view_projection = view_projection,
                .controls = {draw->unit_range.x, draw->unit_range.y,
                             (float32_t)target_width, (float32_t)target_height},
                .object_id = draw->object_id,
                .flags =
                    (ui_domain ? 1u : 0u) | (draw->font_mode != 0u ? 2u : 0u),
            },
        .index_address = text_upload->indices_gpu,
        .index_length = text_upload->indices_length,
        .index_count = draw->index_count,
    };
    atlas->last_use_submit_value = renderer->submit_value + 1u;
  }
  *prepared_pass = (VkrMetalPacketTextPass){
      .pipeline = pipeline,
      .depth = depth_state,
      .count = draw_count,
      .width = target_width,
      .height = target_height,
      .root_cpu = root_cpu,
      .root_gpu = root_gpu,
  };
  *domain_draw_count += draw_count;
  result->indexed_draw_count += draw_count;
  return true_v;
}

vkr_internal void vkr_metal_packet_record_prepared_ui_draws(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const uint8_t *root_cpu, uint64_t root_gpu, uint32_t draw_count,
    uint32_t target_width, uint32_t target_height) API_AVAILABLE(macos(26.0)) {
  [encoder setViewport:(MTLViewport){0.0, 0.0, target_width, target_height, 0.0,
                                     1.0}];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setDepthStencilState:nil];
  id<MTLRenderPipelineState> bound_pipeline = nil;
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrMetalPacketPreparedUiDraw *draw =
        (const VkrMetalPacketPreparedUiDraw *)root_cpu;
    if (draw->pipeline != bound_pipeline) {
      [encoder setRenderPipelineState:draw->pipeline];
      bound_pipeline = draw->pipeline;
    }
    [encoder setScissorRect:draw->scissor];
    [renderer->argument_table setAddress:root_gpu atIndex:1u];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:draw->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:draw->index_address
                 indexBufferLength:draw->index_length
                     instanceCount:1u
                        baseVertex:0
                      baseInstance:0u];
    root_cpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
    root_gpu += VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  }
}

vkr_internal bool8_t vkr_metal_packet_prepare_ui_draw_list(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketUiPass *prepared_pass,
    const VkrPreparedUiDrawList *draw_list, uint32_t target_width,
    uint32_t target_height, VkrMetalPacketFrameUpload *upload,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  if (draw_list->batch_count == 0u)
    return true_v;
  if (upload->ui_vertices_gpu == 0u || upload->ui_indices_gpu == 0u ||
      upload->ui_indices_length == 0u ||
      draw_list->batch_count > upload->root_capacity - upload->root_cursor)
    return false_v;
  const uint64_t root_offset =
      (uint64_t)upload->root_cursor * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  uint8_t *root_cpu = upload->root_cpu + root_offset;
  const uint64_t root_gpu = upload->root_gpu + root_offset;
  VkrMetalPacketTexture preview_texture = {0};
  if (renderer->graph->packet->input.animation_preview) {
    VkrRgImageHandle handle = vkr_rg_find_image(
        renderer->graph, string8_lit("animation_preview_color"));
    VkrMetalPacketImageInstance *image =
        vkr_metal_packet_image_instance(renderer, handle);
    if (image) {
      preview_texture.resource = image->resource;
    }
  }
  uint32_t prepared_count = 0u;
  for (uint32_t i = 0u; i < draw_list->batch_count; ++i) {
    const VkrUiDrawBatch *batch = &draw_list->batches[i];
    VkrMetalPacketTexture *texture = NULL;
    if (batch->texture.id == UINT32_MAX) {
      if (!preview_texture.resource.resource_id) {
        continue;
      }
      texture = &preview_texture;
    } else if (batch->texture.id != 0u) {
      texture = vkr_metal_packet_resolve_texture(
          renderer,
          (VkrTextureHandle){.id = batch->texture.id,
                             .generation = batch->texture.generation});
      if (!texture)
        continue;
    }
    VkrMetalPacketPreparedUiDraw *prepared =
        (VkrMetalPacketPreparedUiDraw *)(root_cpu +
                                         (uint64_t)prepared_count *
                                             VKR_METAL_PACKET_DRAW_ROOT_STRIDE);
    const uint64_t index_offset =
        (uint64_t)batch->first_index * sizeof(uint32_t);
    *prepared = (VkrMetalPacketPreparedUiDraw){
        .root =
            {
                .vertices = upload->ui_vertices_gpu,
                .texture_id = texture ? texture->resource.resource_id : 0u,
                .target_unit_range = {(float32_t)target_width,
                                      (float32_t)target_height,
                                      batch->sdf_unit_range.x,
                                      batch->sdf_unit_range.y},
                .flags = texture ? 1u : 0u,
                .display_output = upload->display_output_gpu,
            },
        .scissor = {(NSUInteger)batch->scissor_rect_px.x,
                    (NSUInteger)batch->scissor_rect_px.y,
                    (NSUInteger)batch->scissor_rect_px.width,
                    (NSUInteger)batch->scissor_rect_px.height},
        .pipeline = renderer->ui_pipeline,
        .index_address = upload->ui_indices_gpu + index_offset,
        .index_length = upload->ui_indices_length - index_offset,
        .index_count = batch->index_count,
    };
    if (texture)
      texture->last_use_submit_value = renderer->submit_value + 1u;
    prepared_count++;
  }
  upload->root_cursor += prepared_count;
  *prepared_pass = (VkrMetalPacketUiPass){
      .count = prepared_count,
      .width = target_width,
      .height = target_height,
      .root_cpu = root_cpu,
      .root_gpu = root_gpu,
  };
  result->ui_draw_count += prepared_count;
  result->indexed_draw_count += prepared_count;
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

vkr_internal bool8_t vkr_metal_packet_prepare_tonemap(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload, uint32_t target_width,
    uint32_t target_height, const Vec4 *editor_rect)
    API_AVAILABLE(macos(26.0)) {
  const bool8_t prepare_display_linear =
      prepared->kind == VKR_RG_EXECUTOR_TONEMAP_PREPARE;
  /* The display-linear cache at binding 5 replaces the scene at binding 0
     when the frame filters it. */
  const VkrRgImageUse *cached = vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u);
  const VkrRgImageUse *read =
      cached ? cached : vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  id<MTLTexture> source = vkr_metal_packet_subresource_texture(renderer, read);
  if (!source)
    return false_v;
  /* The tonemap binds the bloom chain's first level to add to the scene it
     samples (ADR-087); a display-linear source already holds it. */
  const VkrRgImageUse *bloom_use =
      cached ? NULL : vkr_rg_pass_find_image_use(&pass->desc, 6u, 0u);
  id<MTLTexture> bloom =
      vkr_metal_packet_subresource_texture(renderer, bloom_use);
  if (bloom_use && !bloom)
    return false_v;
  uint64_t root_gpu = 0;
  VkrMetalPacketTonemapRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  uint64_t manual_state_gpu = 0u;
  VkrExposureGpuState *manual_state = vkr_metal_packet_next_root(
      upload, sizeof(*manual_state), &manual_state_gpu);
  uint64_t color_grading_gpu = 0u;
  VkrColorGradingGpu *color_grading = vkr_metal_packet_next_root(
      upload, sizeof(*color_grading), &color_grading_gpu);
  if (!root || !manual_state || !color_grading)
    return false_v;
  *color_grading = !editor_rect && packet->input.globals.render_mode ==
                                       VKR_RENDER_MODE_DEFAULT
                       ? packet->color_grading
                       : (VkrColorGradingGpu){0};
  *manual_state = (VkrExposureGpuState){
      .exposure_multiplier = editor_rect ? 1.0f : packet->exposure.manual,
  };
  const uint32_t output_x = editor_rect ? (uint32_t)editor_rect->x : 0u;
  const uint32_t output_y = editor_rect ? (uint32_t)editor_rect->y : 0u;
  const uint32_t output_width =
      editor_rect ? (uint32_t)editor_rect->z : target_width;
  const uint32_t output_height =
      editor_rect ? (uint32_t)editor_rect->w : target_height;
  *root = (VkrMetalPacketTonemapRoot){
      .source_texture_id = source.gpuResourceID._impl,
      .inverse_pre_exposure = 1.0f / packet->exposure.pre_exposure,
      .flags =
          (!editor_rect && renderer->tonemap_enabled ? 1u : 0u) |
          (packet->input.globals.display_transform ==
                   VKR_DISPLAY_TRANSFORM_ACES_FITTED
               ? 4u
               : 0u) |
          (prepare_display_linear
               ? VKR_METAL_PACKET_TONEMAP_FLAG_PREPARE_DISPLAY_LINEAR
               : 0u) |
          (cached ? VKR_METAL_PACKET_TONEMAP_FLAG_SOURCE_DISPLAY_LINEAR : 0u) |
          (editor_rect ? VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED
                       : 0u) |
          (editor_rect && packet->input.editor->scene_backdrop_blur
               ? VKR_METAL_PACKET_TONEMAP_FLAG_SCENE_BLUR
               : 0u) |
          (bloom ? VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM : 0u),
      .image_sharpness =
          !editor_rect && !prepare_display_linear &&
                  packet->input.globals.render_mode == VKR_RENDER_MODE_DEFAULT
              ? packet->input.globals.image_sharpness
              : 0.0f,
      .exposure_state = manual_state_gpu,
      .output_extent = {output_width, output_height},
      .color_grading = color_grading_gpu,
      .display_output = upload->display_output_gpu,
      .bloom_intensity =
          bloom ? vkr_bloom_gpu_params(&renderer->bloom_config, &packet->bloom,
                                       renderer->prepared_frame.bloom_mip_count)
                      .intensity
                : 0.0f,
      .bloom_texture_id = bloom ? bloom.gpuResourceID._impl : 0u,
  };
  const VkrRgBufferUse *exposure_use =
      vkr_rg_pass_find_buffer_use(&pass->desc, 1u, 0u);
  VkrMetalPacketGraphBufferInstance *exposure_state =
      exposure_use
          ? vkr_metal_packet_graph_buffer(renderer, exposure_use->buffer)
          : NULL;
  if (exposure_state)
    root->exposure_state = exposure_state->resource.gpu_address;
  prepared->tonemap.root = root_gpu;
  prepared->tonemap.viewport =
      (MTLViewport){output_x, output_y, output_width, output_height, 0.0, 1.0};
  prepared->tonemap.scissor =
      (MTLScissorRect){output_x, output_y, output_width, output_height};
  prepared->active = true_v;
  return true_v;
}

/* The outline reads the R8 selection mask at binding 0 and covers the whole
 * Scene image; the mask shares that extent. */
vkr_internal bool8_t vkr_metal_packet_prepare_selection_outline(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload, uint32_t target_width,
    uint32_t target_height) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> mask = vkr_metal_packet_subresource_texture(
      renderer, vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u));
  if (!mask)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketSelectionOutlineRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketSelectionOutlineRoot){
      .mask = mask.gpuResourceID._impl,
      .color = packet->input.editor->selection_color,
      .radius_px = packet->input.editor->selection_width_px,
      .display_output = upload->display_output_gpu,
  };
  prepared->tonemap.root = root_gpu;
  prepared->tonemap.viewport =
      (MTLViewport){0.0, 0.0, target_width, target_height, 0.0, 1.0};
  prepared->tonemap.scissor =
      (MTLScissorRect){0u, 0u, target_width, target_height};
  prepared->active = true_v;
  return true_v;
}

/* The grid reads the opaque depth at binding 0 and covers the whole Scene
 * image. Rays use the unjittered camera, like the editor handles. */
vkr_internal bool8_t vkr_metal_packet_prepare_editor_grid(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload, uint32_t target_width,
    uint32_t target_height) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> depth = vkr_metal_packet_subresource_texture(
      renderer, vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u));
  if (!depth)
    return false_v;
  const VkrEditorGridPayload *grid = &packet->input.editor->grid;
  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->input.globals.projection, packet->input.globals.view));
  /* The shader's plane passes through its origin: shift the unprojected
     world along the plane's normal by the grid height instead of changing
     the shader contract. */
  const Vec3 shift = vec3_scale(
      grid->plane == VKR_EDITOR_GRID_PLANE_ZY   ? vec3_new(1.0f, 0.0f, 0.0f)
      : grid->plane == VKR_EDITOR_GRID_PLANE_XY ? vec3_new(0.0f, 0.0f, 1.0f)
                                                : vec3_new(0.0f, 1.0f, 0.0f),
      grid->height);
  const Vec3 eye = vec3_sub(packet->input.globals.view_position, shift);
  uint64_t root_gpu = 0u;
  VkrMetalPacketEditorGridRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketEditorGridRoot){
      .inverse_view_projection =
          mat4_mul(mat4_translate(vec3_scale(shift, -1.0f)),
                   mat4_inverse(view_projection)),
      .camera_position = {eye.x, eye.y, eye.z,
                          grid->fade_end > 0.0f ? 1.0f : 0.0f},
      .params = {grid->cell_size, grid->fade_start, grid->fade_end,
                 grid->through_geometry ? 1.0f : 0.0f},
      .extent = {target_width, target_height},
      .plane = (uint32_t)grid->plane,
      .depth = depth.gpuResourceID._impl,
      .display_output = upload->display_output_gpu,
  };
  prepared->tonemap.root = root_gpu;
  prepared->tonemap.viewport =
      (MTLViewport){0.0, 0.0, target_width, target_height, 0.0, 1.0};
  prepared->tonemap.scissor =
      (MTLScissorRect){0u, 0u, target_width, target_height};
  prepared->active = true_v;
  return true_v;
}

/* Records every queued bake into this frame's IBL.Bake pass. Roots come from
   the frame upload, so a cancelled frame keeps the queue for its retry. */
vkr_internal bool8_t vkr_metal_packet_prepare_ibl(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketPreparedPass *prepared, VkrMetalPacketFrameUpload *upload,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  const uint64_t pending_submit = renderer->submit_value + 1u;
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    VkrMetalPacketPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    VkrMetalPacketTexture *source =
        vkr_metal_packet_resolve_texture(renderer, job->source);
    VkrMetalPacketTexture *prefilter =
        vkr_metal_packet_resolve_texture(renderer, job->prefilter);
    VkrMetalPacketTexture *transmittance =
        job->is_atmosphere
            ? vkr_metal_packet_resolve_texture(renderer, job->transmittance)
            : NULL;
    VkrMetalPacketTexture *multiple_scattering =
        job->is_atmosphere ? vkr_metal_packet_resolve_texture(
                                 renderer, job->multiple_scattering)
                           : NULL;
    /* Destruction drops every bake that names a texture, so each handle of a
       queued bake resolves. */
    if (!source || !prefilter ||
        (job->is_atmosphere && (!transmittance || !multiple_scattering)))
      return false_v;
    id<MTLTexture> source_texture =
        (id<MTLTexture>)source->resource.metal_texture;
    id<MTLTexture> prefilter_texture =
        (id<MTLTexture>)prefilter->resource.metal_texture;
    uint64_t root_gpu = 0u;

    if (job->is_atmosphere) {
      /* The bake stores radiance pre-exposed by this frame's scale, so a
         night sky fits RGBA16F; the source and prefilter carry the
         exponent for whichever frames later sample them. */
      VkrAtmosphereGpuParams bake_params = job->params;
      bake_params.solar.x *= packet->exposure.pre_exposure;
      bake_params.solar.y *= packet->exposure.pre_exposure;
      bake_params.solar.z *= packet->exposure.pre_exposure;
      bake_params.lunar.x *= packet->exposure.pre_exposure;
      bake_params.lunar.y *= packet->exposure.pre_exposure;
      bake_params.lunar.z *= packet->exposure.pre_exposure;
      source->radiance_stops = packet->exposure.pre_exposure_stops;
      /* Transmittance, then multiple scattering, then the source cube; the
         source dispatch reads only the face size. */
      const uint32_t extents[VKR_METAL_PACKET_ATMOSPHERE_DISPATCH_COUNT][2] = {
          {VKR_ATMOSPHERE_TRANSMITTANCE_WIDTH,
           VKR_ATMOSPHERE_TRANSMITTANCE_HEIGHT},
          {VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE,
           VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE},
          {VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE,
           VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE},
      };
      for (uint32_t dispatch = 0u;
           dispatch < VKR_METAL_PACKET_ATMOSPHERE_DISPATCH_COUNT; ++dispatch) {
        VkrMetalPacketAtmosphereRoot *root =
            vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
        if (!root)
          return false_v;
        *root = (VkrMetalPacketAtmosphereRoot){
            .params = bake_params,
            .transmittance_sample_texture_id =
                transmittance->resource.resource_id,
            .transmittance_storage_texture_id =
                transmittance->resource.resource_id,
            .multiple_scattering_sample_texture_id =
                multiple_scattering->resource.resource_id,
            .multiple_scattering_storage_texture_id =
                multiple_scattering->resource.resource_id,
            .source_storage_texture_id = source->resource.resource_id,
            .extent = {extents[dispatch][0], extents[dispatch][1]},
            .face_size = (uint32_t)source_texture.width,
        };
        job->atmosphere_roots[dispatch] = root_gpu;
      }
    }

    prefilter->radiance_stops = source->radiance_stops;
    job->prefilter_size = (uint32_t)prefilter_texture.width;
    job->prefilter_mip_count = (uint32_t)prefilter_texture.mipmapLevelCount;
    for (uint32_t mip = 0u; mip < job->prefilter_mip_count; ++mip) {
      VkrMetalPacketPrefilterRoot *root =
          vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
      if (!root)
        return false_v;
      *root = (VkrMetalPacketPrefilterRoot){
          .source_texture_id = source->resource.resource_id,
          .target_texture_id = prefilter->resource.resource_id,
          .roughness =
              job->prefilter_mip_count > 1u
                  ? (float32_t)mip / (float32_t)(job->prefilter_mip_count - 1u)
                  : 0.0f,
          .source_face_size = (float32_t)source_texture.width,
          .source_mip_count = (float32_t)source_texture.mipmapLevelCount,
          .target_mip = mip,
      };
      job->prefilter_roots[mip] = root_gpu;
      result->ibl_dispatch_count++;
    }

    /* Exhaustion and projection failure are cold-path errors: the source keeps
       its prior publication, or black. */
    job->sh_root = 0u;
    job->sh_slot = VKR_SH_SLOT_BLACK;
    uint32_t projection_mip = 0u;
    uint32_t projection_extent = 0u;
    if (!vkr_ibl_sh_projection_mip((uint32_t)source_texture.width,
                                   (uint32_t)source_texture.mipmapLevelCount,
                                   &projection_mip, &projection_extent)) {
      log_error("Metal IBL source %u:%u cannot supply an SH projection mip; "
                "its previous coefficients remain published",
                job->source.id, job->source.generation);
    } else {
      uint32_t candidate_slot = VKR_SH_SLOT_BLACK;
      if (vkr_ibl_sh_pool_reserve(&renderer->sh_pool, &candidate_slot) !=
          VKR_SH_POOL_STATUS_OK) {
        log_error("Metal SH coefficient pool exhausted; texture %u:%u keeps "
                  "its previous coefficients",
                  job->source.id, job->source.generation);
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
            /* The CPU owns pow(sinc_pi(l/3), sh_deringing) so the kernel
               carries no pow() and both sides cannot drift. */
            .window_band_0 = vkr_ibl_sh_window_factor(0u, job->sh_deringing),
            .window_band_1 = vkr_ibl_sh_window_factor(1u, job->sh_deringing),
            .window_band_2 = vkr_ibl_sh_window_factor(2u, job->sh_deringing),
        };
        if (vkr_ibl_sh_pool_mark_recorded(&renderer->sh_pool, candidate_slot) !=
            VKR_SH_POOL_STATUS_OK) {
          (void)vkr_ibl_sh_pool_abandon(&renderer->sh_pool, candidate_slot);
          return false_v;
        }
        job->sh_root = root_gpu;
        job->sh_slot = candidate_slot;
      }
    }

    source->last_use_submit_value = pending_submit;
    prefilter->last_use_submit_value = pending_submit;
    if (job->is_atmosphere) {
      transmittance->last_use_submit_value = pending_submit;
      multiple_scattering->last_use_submit_value = pending_submit;
    }
    job->recorded = true_v;
    upload->ibl_bake_recorded = true_v;
  }
  prepared->active = upload->ibl_bake_recorded;
  return true_v;
}

/* The frame carrying the recorded bakes has committed. Each SH candidate
   becomes its source's publication, and each atmosphere source completes with
   this submission. Bakes queued after preparation stay queued. */
vkr_internal void
vkr_metal_packet_publish_ibl_bakes(VkrMetalPacketRenderer *renderer,
                                   uint64_t submit_value) {
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    VkrMetalPacketPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    if (!job->recorded) {
      renderer->pending_ibl_bakes[kept++] = *job;
      continue;
    }
    VkrMetalPacketTexture *source =
        vkr_metal_packet_resolve_texture(renderer, job->source);
    if (job->sh_slot != VKR_SH_SLOT_BLACK) {
      if (source && vkr_ibl_sh_pool_publish(&renderer->sh_pool, job->sh_slot) ==
                        VKR_SH_POOL_STATUS_OK) {
        if (source->ibl_sh_slot != VKR_SH_SLOT_BLACK)
          (void)vkr_ibl_sh_pool_retire(&renderer->sh_pool, source->ibl_sh_slot);
        source->ibl_sh_slot = job->sh_slot;
        source->ibl_sh_deringing = job->sh_deringing;
      } else {
        (void)vkr_ibl_sh_pool_retire(&renderer->sh_pool, job->sh_slot);
      }
    }
    if (source && job->is_atmosphere) {
      source->atmosphere_completion_submit_value = submit_value;
      source->atmosphere_ibl_failed =
          job->sh_slot == VKR_SH_SLOT_BLACK ? true_v : false_v;
    }
  }
  renderer->pending_ibl_bake_count = kept;
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

/* This frame's Custom compaction table (ADR-096), made the first time a GPU
   draw root asks for it, with the ready mask the frame records with. Zero
   while no material names a Custom graph. The upload plan reserves its
   cells; a publication since the plan finds none, and that frame draws
   every Custom material's fallback. */
vkr_internal void
vkr_metal_packet_custom_compaction(VkrMetalPacketRenderer *renderer,
                                   VkrMetalPacketFrameUpload *upload,
                                   uint64_t *out_gpu_address) {
  *out_gpu_address = 0u;
  if (renderer->custom_graph_count == 0u)
    return;
  if (!renderer->custom_compaction) {
    uint64_t gpu_address = 0u;
    VkrMetalCustomCompaction *compaction = vkr_metal_packet_next_root_span(
        upload, sizeof(VkrMetalCustomCompaction), &gpu_address);
    if (!compaction)
      return;
    renderer->custom_ready_mask = vkr_metal_packet_custom_state_mask(
        renderer, VKR_METAL_CUSTOM_GRAPH_READY);
    compaction->ready_mask = renderer->custom_ready_mask;
    compaction->pending_mask = vkr_metal_packet_custom_state_mask(
        renderer, VKR_METAL_CUSTOM_GRAPH_PENDING);
    compaction->materials =
        vkr_metal_material_table_device_gpu_address(renderer->materials);
    renderer->custom_compaction = compaction;
    renderer->custom_compaction_gpu = gpu_address;
  }
  *out_gpu_address = renderer->custom_compaction_gpu;
}

vkr_internal bool8_t vkr_metal_packet_prepare_gpu_draw_compute(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  if (!renderer || !pass || !packet || !prepared || !upload)
    return false_v;
  VkrRgExecutorKind kind;
  if (!vkr_render_graph_executor_kind(pass, &kind))
    return false_v;
  const bool8_t classify = kind == VKR_RG_EXECUTOR_GPU_DRAW_CLASSIFY;
  const bool8_t prefix = kind == VKR_RG_EXECUTOR_GPU_DRAW_PREFIX;
  const bool8_t encode = kind == VKR_RG_EXECUTOR_GPU_DRAW_ENCODE;
  if (!classify && !prefix && !encode)
    return false_v;
  const uint32_t candidate_count = renderer->gpu_draw_count;
  const uint64_t instances_gpu = upload->gpu_draw_instances_gpu;
  /* The camera, the directional cascades and the drawn local faces; tiled
     frames draw no local transmission layers (ADR-087). */
  const uint32_t view_count =
      1u + renderer->prepared_frame.shadow_cascade_count +
      renderer->prepared_frame.local_shadow_render_count;
  if (candidate_count == 0u)
    return true_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketGpuDrawRoot *root =
      vkr_metal_packet_next_gpu_draw_root(upload, &root_gpu);
  if (!root)
    return false_v;
  root->candidate_count = candidate_count;
  root->visible_capacity = renderer->prepared_frame.gpu_draw_visible_capacity;
  root->view_count = view_count;
  root->icb_view_group_size = renderer->gpu_draw_icb_view_group_size;
  root->views = upload->gpu_draw_views_gpu;
  root->lod_views = upload->gpu_draw_lod_views_gpu;
  if (!root->views || !root->lod_views)
    return false_v;
  vkr_metal_packet_custom_compaction(renderer, upload,
                                     &root->custom_compaction);

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
    /* Decode records lead classification to each range's LOD row. */
    root->geometry_rows = upload->gpu_draw_geometry_rows_gpu;
    root->classifications = classifications->resource.gpu_address;
    root->compaction_state = state->resource.gpu_address;
    const VkrMetalPacketImageInstance *history =
        renderer->prepared_frame.hzb_history_valid
            ? vkr_metal_packet_selected_hzb_instance(renderer)
            : NULL;
    id<MTLTexture> hzb =
        history ? (id<MTLTexture>)history->resource.metal_texture : nil;
    root->hzb_texture_id = hzb ? hzb.gpuResourceID._impl : 0u;
    root->history_view_projection =
        history ? history->history_raster_view_projection
                : renderer->current_hzb_raster_view_projection;
    root->hzb_extent[0] = renderer->prepared_frame.viewport_width;
    root->hzb_extent[1] = renderer->prepared_frame.viewport_height;
    root->hzb_mip_count = renderer->prepared_frame.hzb_reduce_pass_count + 1u;
    root->hzb_enabled = hzb ? 1u : 0u;
    // Keep the comparison conservative across projected-sphere and depth-buffer
    // quantization; a smaller tolerance self-rejected static fixture geometry.
    root->hzb_depth_epsilon = 1e-3f;
    prepared->dispatch = (VkrMetalPacketDispatch){
        .pipeline = renderer->gpu_draw_classify_pipeline,
        .root = root_gpu,
        .threads = MTLSizeMake(candidate_count, view_count, 1u),
        .group_size = MTLSizeMake(64u, 1u, 1u),
    };
    prepared->active = true_v;
    return true_v;
  }

  if (prefix) {
    VkrMetalPacketGraphBufferInstance *state =
        vkr_metal_packet_pass_buffer(renderer, pass, 0u);
    if (!state)
      return false_v;
    root->compaction_state = state->resource.gpu_address;
    prepared->dispatch = (VkrMetalPacketDispatch){
        .pipeline = renderer->gpu_draw_prefix_pipeline,
        .root = root_gpu,
        .threads = MTLSizeMake(view_count, 1u, 1u),
        .group_size = MTLSizeMake(view_count, 1u, 1u),
    };
    prepared->active = true_v;
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
      !renderer->active_command_slot->gpu_draw_icbs[0])
    return false_v;

  const uint32_t draw_root_slots =
      (view_count * sizeof(VkrMetalPacketDrawRoot) +
       VKR_METAL_PACKET_DRAW_ROOT_STRIDE - 1u) /
      VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  if (draw_root_slots > upload->root_capacity - upload->root_cursor)
    return false_v;
  // Draw kernels index a contiguous array, which may span several root slots.
  const uint64_t draw_roots_offset =
      (uint64_t)upload->root_cursor * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  const uint64_t draw_roots_gpu = upload->root_gpu + draw_roots_offset;
  VkrMetalPacketDrawRoot *draw_roots =
      (VkrMetalPacketDrawRoot *)(upload->root_cpu + draw_roots_offset);
  upload->root_cursor += draw_root_slots;
  for (uint32_t view = 0u; view < view_count; ++view) {
    const bool8_t camera_view = view == 0u;
    const uint32_t first_local_view =
        1u + renderer->prepared_frame.shadow_cascade_count;
    const bool8_t local_view = view >= first_local_view;
    /* Local culling views follow render slots; each draws one view. */
    const uint32_t local_face =
        local_view
            ? packet->input.local_shadow->render_views[view - first_local_view]
            : 0u;
    const uint32_t shadow_map_size =
        local_view ? (uint32_t)(packet->input.local_shadow->views[local_face]
                                    .atlas_rect.z *
                                (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE)
                   : renderer->prepared_frame.shadow_map_size;
    const Mat4 view_projection =
        camera_view
            ? vkr_metal_packet_clip_matrix(
                  renderer->convert_vulkan_clip_y,
                  mat4_mul(packet->temporal.jittered_projection,
                           packet->input.globals.view))
            : vkr_metal_packet_clip_matrix(
                  renderer->convert_vulkan_clip_y,
                  local_view ? packet->input.local_shadow->views[local_face]
                                   .light_view_projection
                             : packet->input.shadow->cascades[view - 1u]
                                   .light_view_projection);
    uint64_t frame_root_gpu = 0u;
    uint32_t view_frame_flags = 0u;
    if (!vkr_metal_packet_create_frame_root(
            renderer, packet, upload, instances_gpu,
            camera_view ? renderer->prepared_frame.viewport_width
                        : shadow_map_size,
            camera_view ? renderer->prepared_frame.viewport_height
                        : shadow_map_size,
            view_projection, camera_view, &frame_root_gpu, &view_frame_flags))
      return false_v;
    draw_roots[view] = (VkrMetalPacketDrawRoot){
        .geometry_rows = upload->gpu_draw_geometry_rows_gpu,
        .visible_rows = visible->resource.gpu_address +
                        (uint64_t)view * root->visible_capacity *
                            sizeof(VkrGpuVisibleDrawRow),
        .vertices = renderer->geometry_megabuffer.vertices.gpu_address,
        .frame = frame_root_gpu,
        .flags = VKR_METAL_PACKET_DRAW_ROOT_GPU_DRAW |
                 (camera_view ? VKR_METAL_PACKET_DRAW_ROOT_PROBES : 0u),
    };
  }
  root->candidates = candidates->resource.gpu_address;
  root->geometry_rows = upload->gpu_draw_geometry_rows_gpu;
  root->instances = instances_gpu;
  root->classifications = classifications->resource.gpu_address;
  root->compaction_state = state->resource.gpu_address;
  root->visible_rows = visible->resource.gpu_address;
  root->draw_roots = draw_roots_gpu;
  const uint32_t encode_group_size = root->icb_view_group_size;
  const uint32_t encode_group_count =
      (view_count + encode_group_size - 1u) / encode_group_size;
  if (encode_group_count > VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX)
    return false_v;
  prepared->gpu_encode.group_count = encode_group_count;
  prepared->gpu_encode.candidate_count = candidate_count;
  prepared->gpu_encode.idle_view_mask = 0u;
  for (uint32_t cascade = 0u;
       cascade < renderer->prepared_frame.shadow_cascade_count; ++cascade) {
    if ((renderer->prepared_frame.shadow_cascade_render_mask &
         (UINT32_C(1) << cascade)) == 0u)
      prepared->gpu_encode.idle_view_mask |= UINT64_C(1) << (cascade + 1u);
  }
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
        renderer->active_command_slot->gpu_draw_icbs[group];
    if (!icb)
      return false_v;
    const uint64_t icb_argument_gpu =
        upload->gpu_draw_icb_argument_gpu +
        (uint64_t)group * upload->gpu_draw_icb_argument_stride;
    prepared->gpu_encode.groups[group] = (VkrMetalPacketGpuEncodeGroup){
        .commands = icb,
        .root = view_root_gpu,
        .arguments = icb_argument_gpu,
        .first_view = first_view,
        .view_count = group_view_count,
    };
  }
  prepared->active = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_skinning(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  const VkrWorldPassPayload *world = renderer->graph->packet->input.world;
  prepared->skinning.count = world ? world->skinning_count : 0u;
  for (uint32_t i = 0u; i < prepared->skinning.count; ++i) {
    uint64_t address = 0u;
    VkrMetalPacketSkinningRoot *root =
        vkr_metal_packet_next_root(upload, sizeof(*root), &address);
    if (!root) {
      return false_v;
    }
    *root = upload->skinning_roots[i];
    prepared->skinning.roots[i] = address;
    prepared->skinning.vertices[i] = root->vertex_count;
  }
  prepared->active = prepared->skinning.count != 0u;
  return true_v;
}

/* Builds the frame's sky-view lookup or aerial-perspective volume from the
   published generation's lookup textures in the frame-slot sky record. */
vkr_internal bool8_t vkr_metal_packet_prepare_sky(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    VkrMetalPacketPreparedPass *prepared, VkrMetalPacketFrameUpload *upload,
    VkrRgExecutorKind kind) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> output = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  const bool8_t sky_view = kind == VKR_RG_EXECUTOR_SKY_VIEW_LUT;
  id<MTLComputePipelineState> pipeline =
      sky_view ? renderer->sky_view_pipeline
               : renderer->aerial_perspective_pipeline;
  const MTLSize extent =
      sky_view
          ? MTLSizeMake(VKR_ATMOSPHERE_SKY_VIEW_WIDTH *
                            VKR_ATMOSPHERE_SKY_VIEW_TABLES,
                        VKR_ATMOSPHERE_SKY_VIEW_HEIGHT, 1u)
          : MTLSizeMake(VKR_ATMOSPHERE_AERIAL_SIZE, VKR_ATMOSPHERE_AERIAL_SIZE,
                        VKR_ATMOSPHERE_AERIAL_SIZE);
  if (!output || !pipeline || upload->sky_gpu == 0u ||
      output.textureType != (sky_view ? MTLTextureType2D : MTLTextureType3D) ||
      output.width != extent.width || output.height != extent.height ||
      output.depth != extent.depth)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketSkyBuildRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketSkyBuildRoot){
      .sky = upload->sky_gpu,
      .output_texture_id = output.gpuResourceID._impl,
  };
  /* Aerial perspective marches one froxel column per thread. */
  prepared->dispatch = (VkrMetalPacketDispatch){
      .pipeline = pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(extent.width, extent.height, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  prepared->active = true_v;
  return true_v;
}

/* Sun-projected cloud transmittance over the map around the camera. */
vkr_internal bool8_t vkr_metal_packet_prepare_cloud_shadow(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    VkrMetalPacketPreparedPass *prepared, VkrMetalPacketFrameUpload *upload)
    API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> output = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  if (!output || !renderer->cloud_shadow_pipeline || upload->sky_gpu == 0u ||
      output.pixelFormat != MTLPixelFormatR16Float ||
      output.width != VKR_CLOUD_SHADOW_SIZE ||
      output.height != VKR_CLOUD_SHADOW_SIZE)
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketSkyBuildRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketSkyBuildRoot){
      .sky = upload->sky_gpu,
      .output_texture_id = output.gpuResourceID._impl,
  };
  prepared->dispatch = (VkrMetalPacketDispatch){
      .pipeline = renderer->cloud_shadow_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(VKR_CLOUD_SHADOW_SIZE, VKR_CLOUD_SHADOW_SIZE, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  prepared->active = true_v;
  return true_v;
}

/* Half-resolution cloud trace into this frame's history output. A selected
   history carries the view-projection that reprojects it. */
vkr_internal bool8_t vkr_metal_packet_prepare_cloud_trace(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> output = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 1u);
  VkrMetalPacketImage *history_slot =
      vkr_metal_packet_named_image_slot(renderer, "cloud_history");
  const uint32_t selected = renderer->selected_cloud_history_instance;
  const bool8_t history_valid = selected != UINT32_MAX && history_slot &&
                                selected < history_slot->instance_count;
  id<MTLTexture> history =
      history_valid ? (id<MTLTexture>)history_slot->instances[selected]
                          .resource.metal_texture
                    : output;
  if (!output || !depth || !history || !renderer->cloud_trace_pipeline ||
      upload->sky_gpu == 0u || output.pixelFormat != MTLPixelFormatRGBA16Float)
    return false_v;

  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->temporal.jittered_projection,
               packet->input.globals.view));
  uint64_t frame_gpu = 0u;
  uint32_t frame_flags = 0u;
  if (!vkr_metal_packet_create_frame_root(
          renderer, packet, upload, upload->gpu_draw_instances_gpu,
          renderer->prepared_frame.viewport_width,
          renderer->prepared_frame.viewport_height, view_projection, true_v,
          &frame_gpu, &frame_flags))
    return false_v;
  uint64_t root_gpu = 0u;
  VkrMetalPacketCloudTraceRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  *root = (VkrMetalPacketCloudTraceRoot){
      .history_pre_exposure_scale =
          history_valid ? vkr_metal_packet_history_pre_exposure_scale(
                              renderer, packet,
                              history_slot->instances[selected]
                                  .history_producer_submit_value)
                        : 1.0f,
      .previous_view_projection =
          history_valid ? renderer->cloud_history[selected].view_projection
                        : packet->sky.view_projection,
      .sky = upload->sky_gpu,
      .frame = frame_gpu,
      .depth_texture_id = depth.gpuResourceID._impl,
      .history_texture_id = history.gpuResourceID._impl,
      .output_texture_id = output.gpuResourceID._impl,
      .extent = {(uint32_t)output.width, (uint32_t)output.height},
      .depth_extent = {renderer->prepared_frame.viewport_width,
                       renderer->prepared_frame.viewport_height},
      .frame_index = (uint32_t)packet->input.frame.frame_index,
      .history_valid = history_valid ? 1u : 0u,
  };
  prepared->dispatch = (VkrMetalPacketDispatch){
      .pipeline = renderer->cloud_trace_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(output.width, output.height, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  prepared->active = true_v;
  return true_v;
}

/* The cloud-lit sky light (ADR-074): every texel of the clear sky source's
   16-texel mip is seen through this frame's cloud layer from the camera,
   projected into this command slot's SH slot with the source's own deringing,
   and the cloud's in-scatter and transmittance halve down to one texel for
   global reflections.
   Frame roots prepared afterwards read that slot as the global sky light; the
   layer itself keeps the clear slot through the sky record. A frame without a
   published source, or a slot beyond the cloud slots, dispatches nothing and
   keeps the clear slot. */
vkr_internal bool8_t vkr_metal_packet_prepare_cloud_sky_light(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketPreparedPass *prepared, VkrMetalPacketFrameUpload *upload)
    API_AVAILABLE(macos(26.0)) {
  prepared->active = false_v;
  VkrTextureHandle source_handle = VKR_TEXTURE_HANDLE_INVALID;
  VkrMetalPacketTexture *source =
      vkr_metal_packet_ibl_source(packet, &source_handle)
          ? vkr_metal_packet_resolve_texture(renderer, source_handle)
          : NULL;
  id<MTLTexture> native =
      source ? (id<MTLTexture>)source->resource.metal_texture : nil;
  const uint32_t command_slot =
      (uint32_t)(renderer->active_command_slot - renderer->command_slots);
  if (!native || native.textureType != MTLTextureTypeCube ||
      source->ibl_sh_slot == VKR_SH_SLOT_BLACK || upload->sky_gpu == 0u ||
      !upload->sky_cpu || upload->cloud_sky_light_radiance_gpu == 0u ||
      command_slot >= VKR_SH_CLOUD_SLOT_COUNT ||
      !renderer->cloud_sky_light_pipeline ||
      !renderer->cloud_sky_light_sh_pipeline ||
      !renderer->cloud_sky_light_mips_pipeline)
    return true_v;
  /* The first mip no wider than the composite's face. */
  uint32_t mip = 0u;
  uint32_t extent = (uint32_t)native.width;
  while (extent > VKR_CLOUD_SKY_LIGHT_FACE_SIZE &&
         mip + 1u < (uint32_t)native.mipmapLevelCount) {
    extent = Max(1u, extent >> 1u);
    ++mip;
  }
  if (extent > VKR_CLOUD_SKY_LIGHT_FACE_SIZE || extent == 0u)
    return true_v;

  /* The march reads the clear slot through the sky record; this root is
     created before the frame's global slot moves. */
  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->temporal.jittered_projection,
               packet->input.globals.view));
  uint64_t frame_gpu = 0u;
  uint32_t frame_flags = 0u;
  if (!vkr_metal_packet_create_frame_root(
          renderer, packet, upload, upload->gpu_draw_instances_gpu,
          renderer->prepared_frame.viewport_width,
          renderer->prepared_frame.viewport_height, view_projection, true_v,
          &frame_gpu, &frame_flags))
    return false_v;
  const VkrPacketFrameConstants constants = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint64_t root_gpu = 0u;
  VkrMetalPacketCloudSkyLightRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu);
  if (!root)
    return false_v;
  const uint32_t cloud_slot = VKR_SH_CLOUD_SLOT_FIRST + command_slot;
  /* Cloud radiance is pre-exposed; the source holds radiance scaled by
     2^radiance_stops. */
  *root = (VkrMetalPacketCloudSkyLightRoot){
      .sky = upload->sky_gpu,
      .frame = frame_gpu,
      .radiance = upload->cloud_sky_light_radiance_gpu,
      .destination = renderer->sh_coefficients.gpuAddress +
                     (uint64_t)cloud_slot * VKR_SH_SLOT_BYTES,
      .source_texture_id = source->resource.resource_id,
      .source_mip = mip,
      .face_size = extent,
      .cloud_to_source_scale =
          ldexpf(1.0f,
                 vkr_metal_packet_global_radiance_stops(renderer, packet)) /
          Max(constants.pre_exposure, 1e-20f),
      .window_band_0 = vkr_ibl_sh_window_factor(0u, source->ibl_sh_deringing),
      .window_band_1 = vkr_ibl_sh_window_factor(1u, source->ibl_sh_deringing),
      .window_band_2 = vkr_ibl_sh_window_factor(2u, source->ibl_sh_deringing),
  };
  source->last_use_submit_value = renderer->submit_value + 1u;
  prepared->cloud_sky_light.root = root_gpu;
  prepared->cloud_sky_light.extent = extent;
  prepared->active = true_v;
  upload->cloud_sh_slot = cloud_slot;
  /* Global reflections read the chain through the sky record. */
  upload->sky_cpu->cloud_sky_light_face_size = extent;
  upload->sky_cpu->cloud_sky_light = upload->cloud_sky_light_radiance_gpu;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_hzb_build(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    VkrMetalPacketPreparedPass *prepared, VkrMetalPacketFrameUpload *upload)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer || !pass || !prepared || !upload ||
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
  prepared->dispatch = (VkrMetalPacketDispatch){
      .pipeline = renderer->hzb_build_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(destination.width, destination.height, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  prepared->active = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_exposure_histogram(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
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
      .reset_reasons = packet->exposure.reset_reasons,
      .metering = vkr_exposure_gpu_metering(&renderer->exposure_metering,
                                            &packet->exposure),
  };
  prepared->histogram.root = root_gpu;
  prepared->histogram.groups =
      MTLSizeMake((source.width + 15u) / 16u, (source.height + 15u) / 16u, 1u);
  prepared->active = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_exposure_resolve(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
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
      .reset_reasons = packet->exposure.reset_reasons,
      .metering = vkr_exposure_gpu_metering(&renderer->exposure_metering,
                                            &packet->exposure),
  };
  if (previous)
    root->metering.delta_seconds = vkr_exposure_history_delta(
        renderer->exposure_seconds + root->metering.delta_seconds,
        previous->history_exposure_seconds);
  else
    root->metering.history_valid = 0u;
  prepared->dispatch = (VkrMetalPacketDispatch){
      .pipeline = renderer->exposure_resolve_pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u, 1u),
      .group_size = MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u, 1u),
  };
  prepared->active = true_v;
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
 *
 * The tiled pipeline's refraction source (ADR-087) is reduced by the same
 * four-tap box, without the firefly clamp; its base level, read from the
 * scene, takes the 2x2 box.
 */
vkr_internal bool8_t vkr_metal_packet_prepare_bloom(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload, VkrRgExecutorKind kind)
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
  const bool8_t upsample = kind == VKR_RG_EXECUTOR_BLOOM_UPSAMPLE;
  if (upsample) {
    const uint32_t destination_mip =
        destination_use->has_slice ? destination_use->slice.mip_level : 0u;
    const bool8_t deepest =
        destination_mip + 2u >= renderer->prepared_frame.bloom_mip_count;
    coarse = vkr_metal_packet_subresource_texture(
        renderer,
        vkr_rg_pass_find_image_use(&pass->desc, deepest ? 3u : 2u, 0u));
    if (!coarse || coarse.width == 0u || coarse.height == 0u ||
        coarse.width > UINT32_MAX || coarse.height > UINT32_MAX)
      return false_v;
  }

  id<MTLComputePipelineState> pipeline = renderer->bloom_prefilter_pipeline;
  if (upsample) {
    pipeline = renderer->bloom_upsample_pipeline;
  } else if (kind == VKR_RG_EXECUTOR_BLOOM_DOWNSAMPLE) {
    /* Both filters are resident; the cold configuration selects which one this
       build measures. Neither is a fallback for the other. */
    pipeline = renderer->bloom_config.filter == VKR_BLOOM_FILTER_BOX_4
                   ? renderer->bloom_downsample_box4_pipeline
                   : renderer->bloom_downsample_tent13_pipeline;
  } else if (kind == VKR_RG_EXECUTOR_TRANSMISSION_DOWNSAMPLE) {
    const bool8_t base =
        !destination_use->has_slice || destination_use->slice.mip_level == 0u;
    pipeline = base ? renderer->bloom_downsample_box2_pipeline
                    : renderer->bloom_downsample_box4_pipeline;
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
      .params = vkr_bloom_gpu_params(&renderer->bloom_config, &packet->bloom,
                                     renderer->prepared_frame.bloom_mip_count),
  };
  /* The light seen through glass keeps its highlights; the reduction still
     drops non-finite and negative texels. */
  if (kind == VKR_RG_EXECUTOR_TRANSMISSION_DOWNSAMPLE)
    root->params.firefly_clamp = 65504.0f;
  prepared->dispatch = (VkrMetalPacketDispatch){
      .pipeline = pipeline,
      .root = root_gpu,
      .threads = MTLSizeMake(destination.width, destination.height, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  prepared->active = true_v;
  return true_v;
}

/* The camera's culled draws in the first indirect command buffer. */
vkr_internal bool8_t vkr_metal_packet_prepare_gpu_draws(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    VkrMetalPacketPreparedPass *prepared, id<MTLRenderPipelineState> pipeline,
    id<MTLRenderPipelineState> opaque_pipeline) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  if (renderer->gpu_draw_count == 0u)
    return true_v;
  id<MTLIndirectCommandBuffer> icb =
      renderer->active_command_slot->gpu_draw_icbs[0];
  if (!state || !icb || !pipeline)
    return false_v;
  prepared->indirect.commands = icb;
  prepared->indirect.pipeline = pipeline;
  prepared->indirect.opaque_pipeline = opaque_pipeline;
  prepared->indirect.arguments = state->resource.gpu_address;
  if (renderer->custom_compaction) {
    prepared->indirect.custom_arguments =
        renderer->custom_compaction_gpu +
        offsetof(VkrMetalCustomCompaction, execution_ranges);
    prepared->indirect.custom_ready_mask = renderer->custom_ready_mask;
  }
  prepared->indirect.viewport =
      (MTLViewport){0.0,
                    0.0,
                    renderer->prepared_frame.viewport_width,
                    renderer->prepared_frame.viewport_height,
                    0.0,
                    1.0};
  prepared->active = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_gpu_shadow(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    uint32_t view, const VkrShadowConfigOverride *depth_bias,
    uint32_t target_width, uint32_t target_height, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketGraphBufferInstance *state =
      vkr_metal_packet_pass_buffer(renderer, pass, 2u);
  if (!packet->input.world || !state || !renderer->gpu_shadow_pipeline)
    return false_v;
  const uint32_t group = view / renderer->gpu_draw_icb_view_group_size;
  id<MTLIndirectCommandBuffer> icb =
      renderer->active_command_slot->gpu_draw_icbs[group];
  if (!icb)
    return false_v;
  prepared->indirect.commands = icb;
  prepared->indirect.pipeline = renderer->gpu_shadow_pipeline;
  prepared->indirect.opaque_pipeline = renderer->gpu_shadow_opaque_pipeline;
  prepared->indirect.arguments =
      state->resource.gpu_address +
      (uint64_t)view * sizeof(VkrGpuDrawCompactionState);
  prepared->indirect.viewport =
      (MTLViewport){0.0, 0.0, target_width, target_height, 0.0, 1.0};
  prepared->indirect.depth_bias =
      depth_bias ? *depth_bias : (VkrShadowConfigOverride){0};
  prepared->active = true_v;
  (void)result;
  return true_v;
}

/* Keeps memoryless multisampled colour and depth of the tiled pipeline's
   opaque pass at the target's extent. They have no memory to make resident,
   and residency sets reject them. Frames still in flight may render into
   replaced targets, so a resize retires them until the GPU completes the
   last submission; a dynamic-resolution step therefore does not stall.
   Only a second resize before that completion waits for it. */
vkr_internal bool8_t vkr_metal_packet_tiled_ensure_targets(
    VkrMetalPacketRenderer *renderer, uint32_t width, uint32_t height)
    API_AVAILABLE(macos(26.0)) {
  if (renderer->tiled_retired_color &&
      renderer->completion.signaledValue >=
          renderer->tiled_retired_submit_value) {
    [renderer->tiled_retired_color release];
    [renderer->tiled_retired_depth release];
    renderer->tiled_retired_color = nil;
    renderer->tiled_retired_depth = nil;
  }
  if (renderer->tiled_msaa_color && renderer->tiled_width == width &&
      renderer->tiled_height == height)
    return true_v;
  if (renderer->tiled_msaa_color) {
    if (renderer->tiled_retired_color) {
      if (!vkr_metal_packet_wait(renderer,
                                 renderer->tiled_retired_submit_value))
        return false_v;
      [renderer->tiled_retired_color release];
      [renderer->tiled_retired_depth release];
    }
    renderer->tiled_retired_color = renderer->tiled_msaa_color;
    renderer->tiled_retired_depth = renderer->tiled_msaa_depth;
    renderer->tiled_retired_submit_value = renderer->submit_value;
  }
  renderer->tiled_msaa_color = nil;
  renderer->tiled_msaa_depth = nil;

  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = MTLTextureType2DMultisample;
  descriptor.width = width;
  descriptor.height = height;
  descriptor.sampleCount = VKR_METAL_TILED_SAMPLE_COUNT;
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeMemoryless;
  descriptor.pixelFormat = MTLPixelFormatRGBA16Float;
  renderer->tiled_msaa_color =
      [renderer->device newTextureWithDescriptor:descriptor];
  descriptor.pixelFormat = MTLPixelFormatDepth32Float;
  renderer->tiled_msaa_depth =
      [renderer->device newTextureWithDescriptor:descriptor];
  [descriptor release];
  if (!renderer->tiled_msaa_color || !renderer->tiled_msaa_depth) {
    log_error("Metal tiled multisampled targets unavailable");
    return false_v;
  }
  renderer->tiled_width = width;
  renderer->tiled_height = height;
  return true_v;
}

/* The sky draws' root: the frame, the sky-view lookup at `sky_view_binding`
   and the sky mode. */
vkr_internal bool8_t vkr_metal_packet_prepare_tiled_sky_root(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketFrameUpload *upload,
    uint32_t width, uint32_t height, uint32_t sky_view_binding,
    id<MTLTexture> depth, uint64_t *out_root_gpu) API_AVAILABLE(macos(26.0)) {
  const VkrSkyPassPayload *sky = packet->input.sky;
  const VkrSkyMode sky_mode = !sky ? VKR_SKY_MODE_NONE
                              : sky->atmosphere.enabled
                                  ? VKR_SKY_MODE_ATMOSPHERE
                                  : VKR_SKY_MODE_CONSTANT;
  id<MTLTexture> sky_view =
      sky_mode == VKR_SKY_MODE_ATMOSPHERE
          ? vkr_metal_packet_pass_texture(renderer, pass, sky_view_binding)
          : nil;
  if (sky_mode == VKR_SKY_MODE_ATMOSPHERE && !sky_view)
    return false_v;
  uint64_t frame_root_gpu = 0u;
  uint32_t frame_flags = 0u;
  const Mat4 view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->temporal.jittered_projection,
               packet->input.globals.view));
  if (!vkr_metal_packet_create_frame_root(
          renderer, packet, upload, upload->gpu_draw_instances_gpu, width,
          height, view_projection, true_v, &frame_root_gpu, &frame_flags))
    return false_v;
  VkrMetalTiledSkyRoot *root =
      vkr_metal_packet_next_root(upload, sizeof(*root), out_root_gpu);
  if (!root)
    return false_v;
  const float32_t pre_exposure = packet->exposure.pre_exposure;
  *root = (VkrMetalTiledSkyRoot){
      .frame = frame_root_gpu,
      .sky_view_texture_id = sky_view ? sky_view.gpuResourceID._impl : 0u,
      .inverse_view_projection = mat4_inverse(view_projection),
      .extent = {width, height},
      .sky_mode = sky_mode,
      .clouds_enabled = renderer->prepared_frame.clouds_enabled ? 1u : 0u,
      .sky_radiance =
          sky_mode == VKR_SKY_MODE_CONSTANT
              ? (Vec4){sky->constant_radiance.x * pre_exposure,
                       sky->constant_radiance.y * pre_exposure,
                       sky->constant_radiance.z * pre_exposure, 0.0f}
              : (Vec4){0},
      .depth_texture_id = depth ? depth.gpuResourceID._impl : 0u,
  };
  return true_v;
}

/* The tiled pipeline's shading variant for the frame's dynamic lights
   (ADR-087), or the inspection variant for the editor's unlit, detail
   lighting, lighting only and wireframe modes. Without a local shadow view
   no light carries a shadow, so the variant without shadow filtering shades
   the same result. */
vkr_internal VkrMetalTiledLighting
vkr_metal_packet_tiled_lighting(const VkrPreparedFrame *packet) {
  const uint32_t mode = packet->input.globals.render_mode;
  if (mode == VKR_RENDER_MODE_UNLIT ||
      mode == VKR_RENDER_MODE_DETAIL_LIGHTING ||
      mode == VKR_RENDER_MODE_LIGHTING_ONLY ||
      mode == VKR_RENDER_MODE_WIREFRAME) {
    return VKR_METAL_TILED_LIGHTING_INSPECT;
  }
  const VkrFrameLighting *lighting = packet->input.lighting;
  if (!lighting) {
    return VKR_METAL_TILED_LIGHTING_NONE;
  }
  if (lighting->rectangle_light_count > 0u) {
    return VKR_METAL_TILED_LIGHTING_ALL;
  }
  if (lighting->point_light_count == 0u) {
    return VKR_METAL_TILED_LIGHTING_NONE;
  }
  if (packet->input.local_shadow &&
      packet->input.local_shadow->view_count > 0u) {
    return VKR_METAL_TILED_LIGHTING_SHADOWED;
  }
  return VKR_METAL_TILED_LIGHTING_PUNCTUAL;
}

/* Whether the camera sees a reflection probe's influence volume, so the
   frame takes the tiled shading variants with probes (ADR-087). Conservative:
   a probe counts when the sphere around its box and blend band meets the
   view frustum. */
vkr_internal uint32_t
vkr_metal_packet_tiled_probe_variant(const VkrPreparedFrame *packet) {
  const VkrFrameLighting *lighting = packet->input.lighting;
  if (!lighting || lighting->ibl_probe_count == 0u || !lighting->ibl_probes)
    return 0u;
  const VkrFrustum frustum = vkr_frustum_from_view_projection(
      packet->input.globals.view, packet->input.globals.projection);
  for (uint32_t i = 0u; i < lighting->ibl_probe_count; ++i) {
    const VkrFrameIblProbe *probe = &lighting->ibl_probes[i];
    if (probe->weight <= 0.0f)
      continue;
    const float32_t radius =
        vec3_length(probe->extents) + Max(probe->blend_distance, 0.0f);
    if (vkr_frustum_test_sphere(&frustum, probe->center, radius))
      return 1u;
  }
  return 0u;
}

/* Whether the camera sees a decal's box, so the frame takes the tiled
   shading variants with decals (ADR-092). Conservative: a decal counts when
   the sphere around its bounds meets the view frustum. */
vkr_internal uint32_t
vkr_metal_packet_tiled_decal_variant(const VkrPreparedFrame *packet) {
  const VkrWorldPassPayload *world = packet->input.world;
  if (!world || world->decal_count == 0u) {
    return 0u;
  }
  const VkrFrustum frustum = vkr_frustum_from_view_projection(
      packet->input.globals.view, packet->input.globals.projection);
  for (uint32_t i = 0u; i < world->decal_count; ++i) {
    const VkrDecal *decal = &world->decals[i];
    const Vec3 centre =
        vec3_scale(vec3_add(decal->bounds_min, decal->bounds_max), 0.5f);
    const float32_t radius =
        0.5f * vec3_length(vec3_sub(decal->bounds_max, decal->bounds_min));
    if (vkr_frustum_test_sphere(&frustum, centre, radius)) {
      return 1u;
    }
  }
  return 0u;
}

/* Prepares the tiled pipeline's opaque pass (ADR-087): the camera's culled
   opaque draws, the clear sky's root, and multisampled targets in place of
   the pass's graph colour and depth, which they resolve into. Depth resolves
   to the nearest sample, so a pixel any surface covers is not sky. */
vkr_internal bool8_t vkr_metal_packet_prepare_tiled_opaque(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload, id<MTLTexture> target)
    API_AVAILABLE(macos(26.0)) {
  const VkrMetalTiledLighting lighting =
      vkr_metal_packet_tiled_lighting(packet);
  const uint32_t probes = vkr_metal_packet_tiled_probe_variant(packet);
  const uint32_t decals = vkr_metal_packet_tiled_decal_variant(packet);
  if (!vkr_metal_packet_prepare_gpu_draws(
          renderer, pass, prepared,
          renderer->tiled_coverage_pipelines[decals][probes][lighting],
          renderer->tiled_forward_pipelines[decals][probes][lighting])) {
    return false_v;
  }
  VkrMetalPacketIndirectPass draws = prepared->indirect;
  const uint32_t width = (uint32_t)target.width;
  const uint32_t height = (uint32_t)target.height;
  draws.viewport = (MTLViewport){0.0, 0.0, width, height, 0.0, 1.0};
  uint64_t sky_root = 0u;
  if (!vkr_metal_packet_tiled_ensure_targets(renderer, width, height) ||
      !vkr_metal_packet_prepare_tiled_sky_root(
          renderer, pass, packet, upload, width, height, 15u, nil, &sky_root))
    return false_v;
  MTL4RenderPassDescriptor *descriptor =
      renderer->render_passes[prepared->index];
  MTLRenderPassColorAttachmentDescriptor *color =
      descriptor.colorAttachments[0];
  color.resolveTexture = color.texture;
  color.texture = renderer->tiled_msaa_color;
  color.storeAction = MTLStoreActionMultisampleResolve;
  MTLRenderPassDepthAttachmentDescriptor *depth = descriptor.depthAttachment;
  depth.resolveTexture = depth.texture;
  depth.texture = renderer->tiled_msaa_depth;
  depth.storeAction = MTLStoreActionMultisampleResolve;
  depth.depthResolveFilter = MTLMultisampleDepthResolveFilterMin;
  descriptor.tileWidth = VKR_METAL_TILED_TILE_WIDTH;
  descriptor.tileHeight = VKR_METAL_TILED_TILE_HEIGHT;
  prepared->tiled.draws = draws;
  prepared->tiled.sky_root = sky_root;
  prepared->tiled.decals = decals;
  prepared->tiled.probes = probes;
  prepared->tiled.lighting = lighting;
  prepared->active = true_v;
  return true_v;
}

/* Prepares the atmosphere draw over the tiled pipeline's resolved image: it
   reads the resolved depth at binding 0 and the sky-view lookup at 15. */
vkr_internal bool8_t vkr_metal_packet_prepare_tiled_atmosphere(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload, id<MTLTexture> target)
    API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> depth = vkr_metal_packet_pass_texture(renderer, pass, 0u);
  if (!depth)
    return false_v;
  const uint32_t width = (uint32_t)target.width;
  const uint32_t height = (uint32_t)target.height;
  if (!vkr_metal_packet_prepare_tiled_sky_root(
          renderer, pass, packet, upload, width, height, 15u, depth,
          &prepared->tiled_atmosphere.root))
    return false_v;
  prepared->tiled_atmosphere.viewport =
      (MTLViewport){0.0, 0.0, width, height, 0.0, 1.0};
  prepared->active = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_pass_body(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload, VkrMetalPacketResult *result)
    API_AVAILABLE(macos(26.0)) {
  id<MTLTexture> target = nil;
  if (pass->desc.color_attachments.length > 0) {
    const VkrRgAttachment *attachment =
        array_get_VkrRgAttachment(&pass->desc.color_attachments, 0);
    target = vkr_metal_packet_texture(renderer, attachment->image);
  } else if (pass->desc.has_depth_attachment) {
    target =
        vkr_metal_packet_texture(renderer, pass->desc.depth_attachment.image);
  }
  if (!target)
    return false_v;
  VkrRgExecutorKind kind;
  if (!vkr_render_graph_executor_kind(pass, &kind))
    return false_v;
  if (kind == VKR_RG_EXECUTOR_SHADOW) {
    if (!packet->input.shadow)
      return true_v;
    const uint32_t cascade = pass->desc.repeat_index;
    if (cascade >= packet->input.shadow->cascade_count)
      return true_v;
    if (!packet->input.world || renderer->gpu_draw_count == 0u)
      return true_v;
    return vkr_metal_packet_prepare_gpu_shadow(
        renderer, pass, packet, prepared, cascade + 1u,
        packet->input.shadow->config_override, (uint32_t)target.width,
        (uint32_t)target.height, result);
  }
  if (kind == VKR_RG_EXECUTOR_LOCAL_SHADOW) {
    /* The face draws into its own atlas square. It clears only that square,
       even without casters, unless its layer was cleared whole this
       submission. Render slot i draws with culling view i. */
    const uint32_t slot = pass->desc.repeat_index;
    if (!packet->input.local_shadow ||
        slot >= packet->input.local_shadow->render_count)
      return false_v;
    /* A face keeping its opaque content draws nothing. */
    if ((packet->input.local_shadow->retained_opaque_mask &
         (UINT64_C(1) << slot)) != 0u)
      return true_v;
    const uint32_t face = packet->input.local_shadow->render_views[slot];
    if (packet->input.world && renderer->gpu_draw_count > 0u &&
        !vkr_metal_packet_prepare_gpu_shadow(
            renderer, pass, packet, prepared,
            1u + renderer->prepared_frame.shadow_cascade_count + slot, NULL,
            (uint32_t)target.width, (uint32_t)target.height, result))
      return false_v;
    const Vec4 rect = packet->input.local_shadow->views[face].atlas_rect;
    const float32_t atlas = (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE;
    const uint32_t x = (uint32_t)(rect.x * atlas);
    const uint32_t y = (uint32_t)(rect.y * atlas);
    const uint32_t size = (uint32_t)(rect.z * atlas);
    prepared->indirect.viewport = (MTLViewport){x, y, size, size, 0.0, 1.0};
    prepared->indirect.clear_rect = (MTLScissorRect){x, y, size, size};
    prepared->indirect.clear_depth =
        !vkr_local_shadow_render_layer_cleared(packet->input.local_shadow,
                                               slot);
    prepared->active = true_v;
    return true_v;
  }
  if (kind == VKR_RG_EXECUTOR_TILED_OPAQUE)
    return vkr_metal_packet_prepare_tiled_opaque(renderer, pass, packet,
                                                 prepared, upload, target);
  if (kind == VKR_RG_EXECUTOR_TILED_ATMOSPHERE)
    return vkr_metal_packet_prepare_tiled_atmosphere(renderer, pass, packet,
                                                     prepared, upload, target);
  if (kind == VKR_RG_EXECUTOR_TILED_PICKING)
    return vkr_metal_packet_prepare_gpu_draws(
        renderer, pass, prepared, renderer->tiled_picking_pipeline, nil);
  /* The tiled blend pass draws the sorted blended list, glass included,
     with its own single-target pipelines (ADR-087). */
  if (kind == VKR_RG_EXECUTOR_TILED_BLEND) {
    prepared->active = true_v;
    if (!packet->input.world)
      return true_v;
    const Mat4 view_projection = vkr_metal_packet_clip_matrix(
        renderer->convert_vulkan_clip_y,
        mat4_mul(packet->temporal.jittered_projection,
                 packet->input.globals.view));
    const VkrMetalTiledLighting lighting =
        vkr_metal_packet_tiled_lighting(packet);
    if (!vkr_metal_packet_prepare_draws(
            renderer, &prepared->world.direct,
            renderer
                ->tiled_blend_pipelines[vkr_metal_packet_tiled_probe_variant(
                    packet)][lighting],
            renderer->depth_read_state, upload->direct_draws,
            upload->direct_draw_count, upload->world_instances_gpu,
            (uint32_t)target.width, (uint32_t)target.height, view_projection,
            packet, true_v, upload, &result->blend_draw_count, result))
      return false_v;
    return vkr_metal_packet_prepare_text_draws(
        renderer, &prepared->world.text, renderer->tiled_text_pipeline,
        renderer->depth_read_state, packet->input.world->text_draws,
        packet->input.world->text_draw_count, 0, (uint32_t)target.width,
        (uint32_t)target.height, view_projection, false_v, upload,
        &result->text_draw_count, result);
  }
  if (kind == VKR_RG_EXECUTOR_PICKING) {
    prepared->active = true_v;
    if (!packet->input.picking || !packet->input.picking->pending)
      return true_v;
    uint32_t picking_draw_count = 0u;
    uint32_t picking_text_draw_count = 0u;
    const Mat4 view_projection = vkr_metal_packet_clip_matrix(
        renderer->convert_vulkan_clip_y,
        mat4_mul(packet->input.globals.projection, packet->input.globals.view));
    if (packet->input.world &&
        !vkr_metal_packet_prepare_draws(
            renderer, &prepared->world.direct, renderer->picking_pipeline,
            renderer->depth_write_state, upload->direct_draws,
            upload->direct_draw_count, upload->world_instances_gpu,
            (uint32_t)target.width, (uint32_t)target.height, view_projection,
            packet, false_v, upload, &picking_draw_count, result))
      return false_v;
    if (packet->input.world &&
        !vkr_metal_packet_prepare_text_draws(
            renderer, &prepared->world.text, renderer->picking_text_pipeline,
            renderer->depth_write_state, packet->input.world->text_draws,
            packet->input.world->text_draw_count, 0u, (uint32_t)target.width,
            (uint32_t)target.height, view_projection, false_v, upload,
            &picking_text_draw_count, result))
      return false_v;
    return true_v;
  }
  if (kind == VKR_RG_EXECUTOR_ANIMATION_PREVIEW) {
    return vkr_metal_packet_prepare_animation_preview(renderer, prepared,
                                                      packet, upload, result);
  }
  if (kind == VKR_RG_EXECUTOR_EDITOR_OVERLAY ||
      kind == VKR_RG_EXECUTOR_EDITOR_OVERLAY_PICKING) {
    prepared->active = true_v;
    prepared->world.direct = (VkrMetalPacketDirectPass){
        .pipeline = kind == VKR_RG_EXECUTOR_EDITOR_OVERLAY
                        ? renderer->editor_overlay_pipeline
                        : renderer->editor_overlay_picking_pipeline,
        .depth = nil,
        .draws = upload->overlay_draws,
        .count = upload->overlay_draw_count,
        .width = (uint32_t)target.width,
        .height = (uint32_t)target.height,
        .root_gpu = upload->overlay_root_gpu,
    };
    result->indexed_draw_count += upload->overlay_draw_count;
    return true_v;
  }
  if (kind == VKR_RG_EXECUTOR_EDITOR_SELECTION_MASK) {
    prepared->active = true_v;
    prepared->world.direct = (VkrMetalPacketDirectPass){
        .pipeline = renderer->editor_selection_mask_pipeline,
        .depth = nil,
        .draws = upload->selection_draws,
        .count = upload->selection_draw_count,
        .width = (uint32_t)target.width,
        .height = (uint32_t)target.height,
        .root_gpu = upload->selection_root_gpu,
    };
    result->indexed_draw_count += upload->selection_draw_count;
    return true_v;
  }
  if (kind == VKR_RG_EXECUTOR_EDITOR_SELECTION_OUTLINE)
    return vkr_metal_packet_prepare_selection_outline(
        renderer, pass, packet, prepared, upload, (uint32_t)target.width,
        (uint32_t)target.height);
  if (kind == VKR_RG_EXECUTOR_EDITOR_GRID)
    return vkr_metal_packet_prepare_editor_grid(
        renderer, pass, packet, prepared, upload, (uint32_t)target.width,
        (uint32_t)target.height);
  if (kind == VKR_RG_EXECUTOR_TONEMAP ||
      kind == VKR_RG_EXECUTOR_TONEMAP_PREPARE) {
    return vkr_metal_packet_prepare_tonemap(renderer, pass, packet, prepared,
                                            upload, (uint32_t)target.width,
                                            (uint32_t)target.height, NULL);
  }
  if (kind == VKR_RG_EXECUTOR_EDITOR)
    return vkr_metal_packet_prepare_tonemap(
        renderer, pass, packet, prepared, upload, (uint32_t)target.width,
        (uint32_t)target.height, &packet->editor_image_rect_px);
  if (kind == VKR_RG_EXECUTOR_CLEAR) {
    prepared->active = true_v;
    return true_v;
  }
  if (kind == VKR_RG_EXECUTOR_UI) {
    prepared->active = true_v;
    if (!packet->input.ui)
      return true_v;
    return vkr_metal_packet_prepare_ui_draw_list(
        renderer, &prepared->ui, &packet->input.ui->draw_list,
        (uint32_t)target.width, (uint32_t)target.height, upload, result);
  }
  /* The graph's executor check admits no other graphics executor. */
  return false_v;
}

vkr_internal bool8_t vkr_metal_packet_prepare_pass(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketPreparedPass *prepared, VkrMetalPacketFrameUpload *upload,
    id<MTLBuffer> readback, uint64_t readback_offset,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  const VkrRgPass *pass = prepared->source;
  if (!vkr_metal_packet_prepare_dependencies(renderer, prepared))
    return false_v;
  if (pass->desc.type == VKR_RG_PASS_TYPE_GRAPHICS) {
    if (!vkr_metal_packet_configure_render_pass(
            renderer, pass, packet, renderer->render_passes[prepared->index]))
      return false_v;
    return vkr_metal_packet_prepare_pass_body(renderer, pass, packet, prepared,
                                              upload, result);
  }
  if (pass->desc.type == VKR_RG_PASS_TYPE_TRANSFER)
    return vkr_metal_packet_prepare_transfer(renderer, pass, prepared, upload);
  switch (prepared->kind) {
  case VKR_RG_EXECUTOR_IBL_BAKE:
    return vkr_metal_packet_prepare_ibl(renderer, packet, prepared, upload,
                                        result);
  case VKR_RG_EXECUTOR_SKINNING:
    return vkr_metal_packet_prepare_skinning(renderer, prepared, upload);
  case VKR_RG_EXECUTOR_SKY_VIEW_LUT:
  case VKR_RG_EXECUTOR_AERIAL_PERSPECTIVE:
    return vkr_metal_packet_prepare_sky(renderer, pass, prepared, upload,
                                        prepared->kind);
  case VKR_RG_EXECUTOR_CLOUD_SHADOW:
    return vkr_metal_packet_prepare_cloud_shadow(renderer, pass, prepared,
                                                 upload);
  case VKR_RG_EXECUTOR_CLOUD_TRACE:
    return vkr_metal_packet_prepare_cloud_trace(renderer, pass, packet,
                                                prepared, upload);
  case VKR_RG_EXECUTOR_CLOUD_SKY_LIGHT:
    return vkr_metal_packet_prepare_cloud_sky_light(renderer, packet, prepared,
                                                    upload);
  case VKR_RG_EXECUTOR_EXPOSURE_HISTOGRAM:
    return vkr_metal_packet_prepare_exposure_histogram(renderer, pass, packet,
                                                       prepared, upload);
  case VKR_RG_EXECUTOR_EXPOSURE_RESOLVE:
    return vkr_metal_packet_prepare_exposure_resolve(renderer, pass, packet,
                                                     prepared, upload);
  case VKR_RG_EXECUTOR_HZB_BUILD:
    return vkr_metal_packet_prepare_hzb_build(renderer, pass, prepared, upload);
  case VKR_RG_EXECUTOR_BLOOM_PREFILTER:
  case VKR_RG_EXECUTOR_BLOOM_DOWNSAMPLE:
  case VKR_RG_EXECUTOR_BLOOM_UPSAMPLE:
  case VKR_RG_EXECUTOR_TRANSMISSION_DOWNSAMPLE:
    return vkr_metal_packet_prepare_bloom(renderer, pass, packet, prepared,
                                          upload, prepared->kind);
  case VKR_RG_EXECUTOR_GPU_DRAW_CLASSIFY:
  case VKR_RG_EXECUTOR_GPU_DRAW_PREFIX:
  case VKR_RG_EXECUTOR_GPU_DRAW_ENCODE:
    return vkr_metal_packet_prepare_gpu_draw_compute(renderer, pass, packet,
                                                     prepared, upload);
  case VKR_RG_EXECUTOR_PICKING_READBACK:
    return vkr_metal_packet_prepare_picking_readback(
        renderer, pass, packet, prepared, readback, readback_offset);
  default:
    return false_v;
  }
}

#include "metal/internal/vkr_metal_packet_record.inc"

bool8_t vkr_metal_packet_renderer_prepare_frame(
    VkrMetalPacketRenderer *renderer,
    const VkrRenderGraphFrameInfo *frame_info) {
  @autoreleasepool {
    if (renderer->frame_prepared || renderer->reserved_frame_slot)
      return false_v;
    /* A requested reload of the Custom library runs before the frame
       records anything (ADR-096). */
    if (vkr_atomic_bool_exchange(&renderer->custom_reload_requested, false_v,
                                 VKR_MEMORY_ORDER_ACQ_REL) &&
        !vkr_metal_packet_custom_reload(renderer))
      vkr_atomic_bool_store(&renderer->custom_reload_requested, true_v,
                            VKR_MEMORY_ORDER_RELEASE);
    VKR_METAL_DIAGNOSTIC(
        renderer, "frame_prepare",
        "target=%ux%u scene=%ux%u render=%ux%u next_slot=%u",
        frame_info->target_width, frame_info->target_height,
        frame_info->scene_output_width, frame_info->scene_output_height,
        frame_info->viewport_width, frame_info->viewport_height,
        renderer->next_command_slot);
    if (frame_info->target_width == 0 || frame_info->target_height == 0 ||
        frame_info->scene_output_width == 0 ||
        frame_info->scene_output_height == 0 ||
        frame_info->viewport_width == 0 || frame_info->viewport_height == 0) {
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
      /* A render thread acquires frames too. Layer properties change only
         with the size, inside an explicit transaction, because a thread
         without a run loop never commits an implicit one. */
      const CGSize drawable_size =
          CGSizeMake(frame_info->target_width, frame_info->target_height);
      if (!CGSizeEqualToSize(renderer->layer.drawableSize, drawable_size)) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        renderer->layer.drawableSize = drawable_size;
        [CATransaction commit];
      }
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
    renderer->reserved_frame_slot = renderer->active_command_slot;
    vkr_metal_packet_detach_active_command_slot(renderer);
    renderer->frame_prepared = true_v;
    return true_v;
  }
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

/*
 * One submission's frame-wide choices, readback reservation and prepared
 * passes. The stages below fill it in order; the submit driver owns every
 * failure cleanup.
 */
typedef struct VkrMetalPacketFrameSubmission {
  /* Frame-wide choices. */
  bool8_t picking;
  bool8_t pass_timing_requested;
  bool8_t submission_timing_requested;
  bool8_t deferred_diagnostics;
  bool8_t synchronous_deferred_validation;
  VkrMetalPacketReadbackLayout readback_layout;
  /* Capture copies and the readback slice that receives them. */
  const VkrCaptureBatchRequest *capture_request;
  VkrMetalPacketCapturePlan capture_plans[VKR_CAPTURE_MAX_ITEMS];
  uint32_t capture_count;
  uint64_t capture_size;
  uint64_t capture_base;
  VkrMetalRingSlice readback_slice;
  VkrMetalAddressPair readback;
  void *readback_object;
  /* Upload slice, capture barriers and prepared passes. */
  VkrMetalPacketFrameUpload frame_upload;
  VkrMetalDependency capture_barrier;
  VkrMetalDependency shadow_capture_barrier;
  VkrMetalDependency ibl_bake_barrier;
  VkrMetalDependency batch_capture_barrier;
  VkrMetalPacketPreparedPass *prepared_passes;
  uint64_t prepared_count;
  const VkrRgPass *failed_pass;
  VkrMetalPacketGraphBufferInstance *deferred_state;
  VkrMetalPacketGraphBufferInstance *exposure_histogram;
  VkrMetalPacketGraphBufferInstance *exposure_state;
  id<MTLTexture> present;
  id<MTLTexture> shadow;
} VkrMetalPacketFrameSubmission;

/* Finishes pending asset uploads and makes the reserved slot this frame's
 * command slot. */
vkr_internal bool8_t
vkr_metal_packet_activate_frame_slot(VkrMetalPacketRenderer *renderer) {
  if (renderer->texture_upload_batch.enabled &&
      !vkr_metal_packet_texture_upload_batch_flush(renderer, true_v)) {
    log_error("Metal frame preparation failed while finishing asset uploads");
    return false_v;
  }
  if (renderer->active_command_slot) {
    log_error(
        "Metal frame preparation found an unfinished upload command slot");
    return false_v;
  }
  renderer->active_command_slot = renderer->reserved_frame_slot;
  /* Uploads advanced their own acquisitions past the reserved frame. Resume
   * frame rotation after this slot so the next frame can overlap its submit.
   */
  renderer->next_command_slot =
      ((uint32_t)(renderer->reserved_frame_slot - renderer->command_slots) +
       1u) %
      renderer->command_slot_count;
  renderer->reserved_frame_slot = NULL;
  renderer->command_allocator = renderer->active_command_slot->allocator;
  renderer->command_buffer = renderer->active_command_slot->buffer;
  return true_v;
}

/* Lowers the packet into this frame's prepared state and resets the history
 * selections, HZB matrices and packet-build metrics later stages fill. */
vkr_internal void
vkr_metal_packet_prepare_frame_state(VkrMetalPacketRenderer *renderer,
                                     const VkrPreparedFrame *packet,
                                     VkrMetalPacketResult *out_result) {
  /* The tiled graph has no GTAO pass, so its parameters are discarded. */
  const VkrGtaoConfig gtao_config = vkr_gtao_config_default();
  VkrGtaoGpuParams gtao_params = {0};
  vkr_render_graph_prepare_frame(packet, &renderer->bloom_config, &gtao_config,
                                 &renderer->prepared_frame, &gtao_params);
  renderer->prepared_frame.hzb_build_enabled &= renderer->hzb_enabled;
  VKR_METAL_DIAGNOSTIC(renderer, "frame_packet",
                       "frame=%llu scene_rendering=%u retained=%u "
                       "retained_extent=%ux%u",
                       (unsigned long long)packet->input.frame.frame_index,
                       packet->scene_rendering, packet->editor_image_available,
                       packet->editor_image_width, packet->editor_image_height);
  out_result->source_render_scale = renderer->prepared_frame.render_scale;
  out_result->source_render_width = renderer->prepared_frame.viewport_width;
  out_result->source_render_height = renderer->prepared_frame.viewport_height;
  renderer->prepared_frame.hzb_history_valid = false_v;
  renderer->selected_hzb_history_instance = UINT32_MAX;
  renderer->selected_cloud_history_instance = UINT32_MAX;
  renderer->selected_exposure_history_instance = UINT32_MAX;
  renderer->current_hzb_view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->input.globals.projection, packet->input.globals.view));
  renderer->current_hzb_raster_view_projection = vkr_metal_packet_clip_matrix(
      renderer->convert_vulkan_clip_y,
      mat4_mul(packet->temporal.jittered_projection,
               packet->input.globals.view));
  /* Seeded valid so a frame with no candidates publishes an honest zero
     rather than reading as an absent producer; the pack path overwrites it on
     failure. */
  renderer->packet_build = (VkrPacketBuildMetrics){.valid = true_v};
#if VKR_METRICS_ENABLED
  const float64_t candidate_hash_start = vkr_platform_get_absolute_time();
#endif
  renderer->current_hzb_world_epoch =
      vkr_metal_packet_world_occluder_epoch(packet->input.world);
#if VKR_METRICS_ENABLED
  renderer->packet_build.candidate_hash_ns =
      vkr_metrics_elapsed_ns(candidate_hash_start);
#endif
}

/* Builds, compiles and realizes the frame graph. Allocation failures leave
 * their error in `out_result`; the caller finishes the frame. */
vkr_internal bool8_t vkr_metal_packet_realize_frame_graph(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketResult *out_result) {
  vkr_rg_set_packet(renderer->graph, packet);
  /* Installed before compilation, because seeding a retained subresource
     reads through this provider. */
  vkr_metal_packet_install_retained_provider(renderer);
#if VKR_METRICS_ENABLED
  float64_t graph_stage_start = vkr_platform_get_absolute_time();
#endif
  if (!vkr_rg_build_from_json(renderer->graph, &renderer->json_graph,
                              &renderer->prepared_frame)) {
    log_error("Metal packet stage failed: graph build");
    return false_v;
  }
#if VKR_METRICS_ENABLED
  renderer->packet_build.graph_build_ns =
      vkr_metrics_elapsed_ns(graph_stage_start);
  graph_stage_start = vkr_platform_get_absolute_time();
#endif
  if (!vkr_rg_compile_schedule(renderer->graph)) {
    log_error("Metal packet stage failed: graph compile");
    return false_v;
  }
#if VKR_METRICS_ENABLED
  renderer->packet_build.graph_compile_ns =
      vkr_metrics_elapsed_ns(graph_stage_start);
  renderer->packet_build.graph_timing_valid = true_v;
#endif
  if (renderer->graph->passes.length > renderer->max_passes) {
    log_error("Metal packet stage failed: %llu passes exceed capacity %u",
              (unsigned long long)renderer->graph->passes.length,
              renderer->max_passes);
    return false_v;
  }
  /* Normal DRS and UI-only frames retain the ordinary replacement policy.
   * The cold completion wait is reserved for allocation-OOM recovery. */
  if (packet->scene_rendering && renderer->graph_memory_recovery_pending &&
      !vkr_metal_packet_reclaim_graph_targets(renderer)) {
    log_error("Metal packet stage failed: graph memory recovery");
    return false_v;
  }
  VkrMetalPacketImageAllocationFailure image_failure = {0};
  bool8_t images_realized = vkr_metal_packet_realize_images(
      renderer, &out_result->error, &image_failure);
  if (!images_realized && packet->scene_rendering &&
      out_result->error == VKR_RENDERER_ERROR_OUT_OF_MEMORY) {
    /* One cold retry avoids old/new target overlap before Application
     * considers reducing Scene output. No commands use this graph yet. */
    if (!vkr_metal_packet_reclaim_graph_targets(renderer)) {
      out_result->error = VKR_RENDERER_ERROR_DEVICE_ERROR;
      log_error("Metal packet stage failed: image allocation reclamation "
                "after '%.*s' allocation failed (%s)",
                (int)image_failure.image->name.length,
                image_failure.image->name.str,
                vkr_metal_memory_status_string(image_failure.status));
      return false_v;
    }
    const VkrMetalPacketImageAllocationFailure first_failure = image_failure;
    images_realized = vkr_metal_packet_realize_images(
        renderer, &out_result->error, &image_failure);
    if (images_realized)
      log_warn("Metal Scene image allocation recovered at unchanged "
               "resolution %ux%u after GPU completion and reclamation "
               "(image='%.*s', reason=%s)",
               renderer->prepared_frame.viewport_width,
               renderer->prepared_frame.viewport_height,
               (int)first_failure.image->name.length,
               first_failure.image->name.str,
               vkr_metal_memory_status_string(first_failure.status));
  }
  if (!images_realized) {
    renderer->graph_memory_recovery_pending |=
        out_result->error == VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    if (image_failure.image) {
      const VkrRgImage *image = image_failure.image;
      log_error("Metal graph image '%.*s' allocation failed: %s, format=%u "
                "extent=%ux%u layers=%u mips=%u usage=0x%x",
                (int)image->name.length, image->name.str,
                vkr_metal_memory_status_string(image_failure.status),
                image->desc.format, image->desc.width, image->desc.height,
                image->desc.layers, image->desc.mip_levels,
                image->desc.usage.set);
    } else {
      log_error("Metal packet stage failed: image realization");
    }
    return false_v;
  }
  if (!vkr_metal_packet_realize_buffers(renderer, &out_result->error)) {
    renderer->graph_memory_recovery_pending |=
        out_result->error == VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    log_error("Metal packet stage failed: buffer realization");
    return false_v;
  }
  return true_v;
}

/* Selects the history instance each history consumer reads this frame. */
vkr_internal bool8_t vkr_metal_packet_select_frame_histories(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet) {
  if ((packet->scene_rendering ||
       (packet->input.world && packet->input.world->skinning_count)) &&
      !vkr_metal_packet_select_history_output(renderer)) {
    log_error("Metal packet stage failed: history output selection");
    return false_v;
  }
  if (packet->scene_rendering &&
      !vkr_metal_packet_select_hzb_history(renderer)) {
    log_error("Metal packet stage failed: HZB history selection");
    return false_v;
  }
  if (packet->scene_rendering &&
      !vkr_metal_packet_select_cloud_history(renderer)) {
    log_error("Metal packet stage failed: cloud history selection");
    return false_v;
  }
  if (packet->scene_rendering &&
      !vkr_metal_packet_select_exposure_history(renderer)) {
    log_error("Metal packet stage failed: exposure history selection");
    return false_v;
  }
  return true_v;
}

/* Records the frame's optional work and the fixed readback prefix layout. */
vkr_internal void vkr_metal_packet_plan_frame_submission(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketFrameSubmission *submission) {
  submission->picking = renderer->prepared_frame.picking_pending;
  submission->pass_timing_requested =
      packet->input.debug && packet->input.debug->enable_timing &&
      packet->input.debug->capture_pass_timestamps;
  submission->submission_timing_requested =
      renderer->dynamic_resolution_enabled ||
      (packet->input.debug && packet->input.debug->enable_timing &&
       packet->input.debug->capture_submission_timing);
  submission->deferred_diagnostics = packet->input.world != NULL;
  submission->synchronous_deferred_validation =
      renderer->synchronous_validation_readback &&
      submission->deferred_diagnostics;
  submission->readback_layout = vkr_metal_packet_readback_layout(
      submission->deferred_diagnostics,
      renderer->prepared_frame.exposure_automatic,
      1u + renderer->prepared_frame.shadow_cascade_count +
          renderer->prepared_frame.local_shadow_render_count);
}

/* Plans capture copies after the readback prefix and reserves their capture
 * ring entry. */
vkr_internal bool8_t vkr_metal_packet_reserve_frame_capture(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketFrameSubmission *submission,
    VkrMetalPacketResult *out_result) {
  VkrCaptureBackendItemPlan capture_ring_plans[VKR_CAPTURE_MAX_ITEMS] = {0};
  submission->capture_count = 0;
  submission->capture_size = submission->readback_layout.prefix_size;
  if (!vkr_metal_packet_capture_plan(
          renderer, packet, submission->readback_layout.prefix_size,
          submission->capture_plans, &submission->capture_count,
          &submission->capture_size)) {
    log_error("Metal packet stage failed: capture plan");
    return false_v;
  }
  submission->capture_request =
      packet->input.debug ? packet->input.debug->capture : NULL;
  submission->capture_base = 0;
  if (submission->capture_count > 0) {
    submission->capture_base = submission->capture_plans[0].buffer_offset;
    for (uint32_t i = 0; i < submission->capture_count; ++i) {
      capture_ring_plans[i] = (VkrCaptureBackendItemPlan){
          .result = submission->capture_plans[i].result,
          .buffer_offset = submission->capture_plans[i].buffer_offset -
                           submission->capture_base,
      };
    }
    const VkrRendererError capture_error = vkr_capture_ring_reserve(
        &renderer->capture_ring, submission->capture_request,
        capture_ring_plans, packet->input.frame.frame_index);
    if (capture_error != VKR_RENDERER_ERROR_NONE) {
      out_result->capture = (VkrCapturePollResult){
          .status = VKR_CAPTURE_STATUS_FAILED, .error = capture_error};
      return false_v;
    }
  }
  return true_v;
}

/* Acquires and clears the readback slice for the prefix and capture copies.
 * Returns the error the caller reports and fails the capture with. */
vkr_internal VkrRendererError vkr_metal_packet_acquire_frame_readback(
    VkrMetalPacketRenderer *renderer,
    VkrMetalPacketFrameSubmission *submission) {
  /* Ring acquisition reclaims completed slices. Use the same completion
     observation when preserving pixels so it cannot reclaim a newer slice. */
  uint64_t readback_completed = renderer->completion.signaledValue;
  vkr_metal_packet_collect_picking_results(renderer, readback_completed);
  const VkrMetalMemoryStatus readback_capacity_status =
      vkr_metal_packet_ensure_ring_capacity(
          renderer, VKR_METAL_RING_KIND_READBACK, submission->capture_size);
  if (readback_capacity_status != VKR_METAL_MEMORY_STATUS_OK) {
    return readback_capacity_status == VKR_METAL_MEMORY_STATUS_RING_BUSY
               ? VKR_RENDERER_ERROR_CAPTURE_BUSY
               : vkr_metal_packet_allocation_error(readback_capacity_status);
  }
  readback_completed = renderer->completion.signaledValue;
  const VkrMetalMemoryStatus readback_status =
      vkr_metal_memory_device_acquire_ring(
          renderer->memory, VKR_METAL_RING_KIND_READBACK,
          submission->capture_size, readback_completed,
          &submission->readback_slice, &submission->readback,
          &submission->readback_object);
  if (readback_status != VKR_METAL_MEMORY_STATUS_OK) {
    log_error("Metal packet stage failed: readback ring %s (%llu bytes)",
              vkr_metal_memory_status_string(readback_status),
              (unsigned long long)submission->capture_size);
    return readback_status == VKR_METAL_MEMORY_STATUS_RING_BUSY
               ? VKR_RENDERER_ERROR_CAPTURE_BUSY
               : vkr_metal_packet_allocation_error(readback_status);
  }
  MemZero(submission->readback.cpu_address, submission->capture_size);
  return VKR_RENDERER_ERROR_NONE;
}

/* Lowers the capture barriers, prepares every scheduled pass and resolves the
 * resources the readback copies read. Logs the failure it returns. */
vkr_internal bool8_t vkr_metal_packet_prepare_frame_passes(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketFrameSubmission *submission, VkrMetalPacketResult *out_result)
    API_AVAILABLE(macos(26.0)) {
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
  /* IBL.Bake writes no graph resource; every later encoder may sample its
     prefilters and SH coefficients. */
  const VkrGpuDependency ibl_bake_dependency = {
      .src_stages = VKR_GPU_STAGE_COMPUTE_SHADER | VKR_GPU_STAGE_TRANSFER,
      .dst_stages = VKR_GPU_STAGE_ALL_GRAPHICS | VKR_GPU_STAGE_COMPUTE_SHADER |
                    VKR_GPU_STAGE_TRANSFER,
      .visibility = VKR_GPU_VISIBILITY_DEVICE,
  };
  const VkrGpuDependency batch_capture_dependency = {
      .src_stages = VKR_GPU_STAGE_ALL_GRAPHICS | VKR_GPU_STAGE_COMPUTE_SHADER |
                    VKR_GPU_STAGE_TRANSFER,
      .dst_stages = VKR_GPU_STAGE_COMPUTE_SHADER | VKR_GPU_STAGE_TRANSFER,
      .visibility = VKR_GPU_VISIBILITY_DEVICE,
  };
  bool8_t passed =
      vkr_metal_dependency_lower(&capture_dependency,
                                 &submission->capture_barrier) &&
      vkr_metal_dependency_lower(&shadow_capture_dependency,
                                 &submission->shadow_capture_barrier) &&
      vkr_metal_dependency_lower(&ibl_bake_dependency,
                                 &submission->ibl_bake_barrier) &&
      vkr_metal_dependency_lower(&batch_capture_dependency,
                                 &submission->batch_capture_barrier);
  submission->prepared_count = renderer->graph->execution_order.length;
  /* This diagnostic pass inventory is separate from command emission. It
     reports planned work, not proof that any pass reached the GPU. */
  if (renderer->diagnostics.enabled) {
    for (uint64_t order = 0u; order < submission->prepared_count; ++order) {
      const uint32_t index = renderer->graph->execution_order.data[order];
      const VkrRgPass *pass = &renderer->graph->passes.data[index];
      VKR_METAL_DIAGNOSTIC(renderer, "frame_pass",
                           "frame=%llu order=%llu index=%u culled=%u name=%.*s",
                           (unsigned long long)packet->input.frame.frame_index,
                           (unsigned long long)order, index, pass->culled,
                           (int)pass->desc.name.length, pass->desc.name.str);
    }
  }
  submission->prepared_passes = vkr_allocator_alloc(
      &renderer->graph_frame_allocator,
      submission->prepared_count * sizeof(*submission->prepared_passes),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  passed = passed && (submission->prepared_count == 0u ||
                      submission->prepared_passes != NULL);
  for (uint64_t order = 0u; passed && order < submission->prepared_count;
       ++order) {
    const CFTimeInterval preparation_cpu_begin = CACurrentMediaTime();
    const uint32_t index = renderer->graph->execution_order.data[order];
    const VkrRgPass *pass = &renderer->graph->passes.data[index];
    VkrMetalPacketPreparedPass *prepared = &submission->prepared_passes[order];
    *prepared = (VkrMetalPacketPreparedPass){
        .source = pass,
        .index = index,
        .last_present_writer =
            vkr_metal_packet_is_last_present_writer(renderer, order),
        .label = vkr_metal_packet_pass_label(renderer, index, pass),
    };
    if (pass->culled)
      continue;
    if (!vkr_render_graph_executor_kind(pass, &prepared->kind) ||
        !vkr_metal_packet_prepare_pass(
            renderer, packet, prepared, &submission->frame_upload,
            (id<MTLBuffer>)submission->readback_object,
            submission->readback_slice.offset +
                submission->readback_layout.picking,
            out_result)) {
      submission->failed_pass = pass;
      passed = false_v;
      break;
    }
    switch (prepared->kind) {
    /* The opaque pass reads the culling state, without G-buffer resolve
       counters (ADR-087). */
    case VKR_RG_EXECUTOR_TILED_OPAQUE:
      if (submission->deferred_diagnostics)
        submission->deferred_state =
            vkr_metal_packet_pass_buffer(renderer, pass, 2u);
      break;
    case VKR_RG_EXECUTOR_EXPOSURE_HISTOGRAM:
      submission->exposure_histogram =
          vkr_metal_packet_pass_buffer(renderer, pass, 1u);
      break;
    case VKR_RG_EXECUTOR_EXPOSURE_RESOLVE:
      submission->exposure_state =
          vkr_metal_packet_pass_buffer(renderer, pass, 1u);
      break;
    default:
      break;
    }
    prepared->preparation_cpu_ms =
        (CACurrentMediaTime() - preparation_cpu_begin) * 1000.0;
  }
  submission->present =
      vkr_metal_packet_texture(renderer, renderer->graph->present_image);
  submission->shadow =
      packet->scene_rendering
          ? vkr_metal_packet_named_texture(renderer, "shadow_map")
          : nil;
  passed = passed && submission->present &&
           (!packet->scene_rendering || submission->shadow) &&
           (!submission->deferred_diagnostics || submission->deferred_state) &&
           (!submission->readback_layout.exposure_size ||
            (submission->exposure_histogram && submission->exposure_state));
  if (!passed) {
    if (submission->failed_pass) {
      log_error("Metal frame preparation failed at pass '%.*s' "
                "(root cells %u/%u)",
                (int)submission->failed_pass->desc.name.length,
                submission->failed_pass->desc.name.str,
                submission->frame_upload.root_cursor,
                submission->frame_upload.root_capacity);
    } else {
      log_error("Metal frame preparation failed for capture resources or "
                "native dependencies");
    }
  }
  return passed;
}

/* Appends one pass's timing row. GPU duration arrives with the submission's
   timestamp heap when the pass wrote its interval. */
vkr_internal void vkr_metal_packet_record_pass_timing(
    const VkrMetalPacketFrameSubmission *submission,
    const VkrMetalPacketPreparedPass *prepared, bool8_t encoder_timing_pass,
    CFTimeInterval pass_cpu_begin, VkrMetalPacketResult *out_result)
    API_AVAILABLE(macos(26.0)) {
  const VkrRgPass *pass = prepared->source;
  if (out_result->pass_timing_count < VKR_METAL_PACKET_MAX_PASS_TIMINGS) {
    VkrMetalPacketPassTiming *timing =
        &out_result->pass_timings[out_result->pass_timing_count++];
    const uint64_t name_length =
        Min(pass->desc.name.length,
            (uint64_t)VKR_METAL_PACKET_TIMING_NAME_CAPACITY - 1u);
    if (name_length > 0)
      MemCopy(timing->name, pass->desc.name.str, name_length);
    timing->name[name_length] = '\0';
    timing->pass_index = prepared->index;
    timing->starts_after_previous =
        pass->desc.type == VKR_RG_PASS_TYPE_GRAPHICS;
    timing->cpu_ms = prepared->preparation_cpu_ms +
                     (CACurrentMediaTime() - pass_cpu_begin) * 1000.0;
    timing->valid = false_v;
    timing->unavailable_reason =
        encoder_timing_pass
            ? VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY
            : (submission->pass_timing_requested
                   ? VKR_RENDERER_IMPL_GPU_TIMING_REASON_UNSUPPORTED_TIMESTAMP_SCOPE
                   : VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED);
  }
  out_result->executed_pass_count++;
}

/* Compute and transfer passes share one encoder per run; graphics passes own
   theirs. */
vkr_internal bool8_t
vkr_metal_packet_pass_batchable(const VkrMetalPacketPreparedPass *prepared) {
  return prepared->source->desc.type != VKR_RG_PASS_TYPE_GRAPHICS;
}

vkr_internal void
vkr_metal_packet_merge_dependency(VkrMetalDependency *merged,
                                  const VkrMetalDependency *dependency) {
  merged->src_stages |= dependency->src_stages;
  merged->dst_stages |= dependency->dst_stages;
  merged->visibility_options |= dependency->visibility_options;
}

/* Encodes a graphics pass and records its timing row. */
vkr_internal bool8_t vkr_metal_packet_encode_graph_pass(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketFrameSubmission *submission,
    const VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketResult *out_result) API_AVAILABLE(macos(26.0)) {
  const VkrRgPass *pass = prepared->source;
  const bool8_t encoder_timing_pass = submission->pass_timing_requested;
  const CFTimeInterval pass_cpu_begin = CACurrentMediaTime();
  id<MTL4RenderCommandEncoder> encoder = [renderer->command_buffer
      renderCommandEncoderWithDescriptor:renderer
                                             ->render_passes[prepared->index]];
  if (!encoder) {
    submission->failed_pass = pass;
    return false_v;
  }
  encoder.label = prepared->label;
  /* Apple GPUs run a pass's vertex and tiling work before its fragment work.
     The start stamp follows the earliest stage so the interval covers both;
     one written after the fragment stage would begin once tiling had ended. */
  if (encoder_timing_pass)
    [encoder writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                afterStage:MTLRenderStageVertex
                                  intoHeap:renderer->active_command_slot
                                               ->timestamp_heap
                                   atIndex:(NSUInteger)prepared->index * 2u];
  vkr_metal_packet_record_consumers(encoder, prepared);
  if (prepared->active)
    vkr_metal_packet_record_graphics_pass(renderer, encoder, prepared);
  vkr_metal_packet_record_producers(encoder, prepared);
  out_result->dependency_count +=
      prepared->consumer_count + prepared->producer_count;
  if (prepared->last_present_writer) {
    vkr_metal_dependency_encode_producer(encoder, &submission->capture_barrier);
    out_result->dependency_count++;
  }
  if (prepared->kind == VKR_RG_EXECUTOR_SHADOW &&
      pass->desc.repeat_index + 1u ==
          renderer->prepared_frame.shadow_cascade_count) {
    vkr_metal_dependency_encode_producer(encoder,
                                         &submission->shadow_capture_barrier);
    out_result->dependency_count++;
  }
  if (submission->capture_count > 0u) {
    vkr_metal_dependency_encode_producer(encoder,
                                         &submission->batch_capture_barrier);
    out_result->dependency_count++;
  }
  if (encoder_timing_pass)
    [encoder
        writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                           afterStage:MTLRenderStageFragment
                             intoHeap:renderer->active_command_slot
                                          ->timestamp_heap
                              atIndex:(NSUInteger)prepared->index * 2u + 1u];
  [encoder endEncoding];
  out_result->graphics_pass_count++;
  vkr_metal_packet_record_pass_timing(submission, prepared, encoder_timing_pass,
                                      pass_cpu_begin, out_result);
  return true_v;
}

/* Every encoder boundary drains the GPU (about 25-50 us on the M1), so a run
   of consecutive compute and transfer passes shares one encoder. The graph's
   dependencies keep their meaning: the run first waits for every producer its
   passes need from earlier encoders, each later pass takes an intra-encoder
   barrier covering producers earlier in the run, and the run ends with the
   union of its passes' producer barriers for later encoders. Passes without a
   dependency between them may overlap. */
vkr_internal bool8_t vkr_metal_packet_encode_compute_run(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketFrameSubmission *submission,
    uint64_t first, uint64_t end, VkrMetalPacketResult *out_result)
    API_AVAILABLE(macos(26.0)) {
  const MTLStages encoder_stages = MTLStageDispatch | MTLStageBlit;
  VkrMetalDependency wait = {0};
  VkrMetalDependency signal = {0};
  bool8_t ibl_baked = false_v;
  for (uint64_t order = first; order < end; ++order) {
    const VkrMetalPacketPreparedPass *prepared =
        &submission->prepared_passes[order];
    if (prepared->source->culled)
      continue;
    for (uint32_t i = 0u; i < prepared->consumer_count; ++i)
      vkr_metal_packet_merge_dependency(&wait, &prepared->consumers[i]);
    for (uint32_t i = 0u; i < prepared->producer_count; ++i)
      vkr_metal_packet_merge_dependency(&signal, &prepared->producers[i]);
    /* The cloud-lit sky light also writes SH coefficients that later
       encoders read outside the graph's resources. */
    ibl_baked =
        ibl_baked || (prepared->active &&
                      (prepared->kind == VKR_RG_EXECUTOR_IBL_BAKE ||
                       prepared->kind == VKR_RG_EXECUTOR_CLOUD_SKY_LIGHT));
  }

  id<MTL4ComputeCommandEncoder> encoder =
      [renderer->command_buffer computeCommandEncoder];
  if (!encoder) {
    submission->failed_pass = submission->prepared_passes[first].source;
    return false_v;
  }
  encoder.label = submission->prepared_passes[first].label;
  if (wait.src_stages != 0u)
    vkr_metal_dependency_encode_consumer(encoder, &wait);

  bool8_t first_pass = true_v;
  for (uint64_t order = first; order < end; ++order) {
    const VkrMetalPacketPreparedPass *prepared =
        &submission->prepared_passes[order];
    const VkrRgPass *pass = prepared->source;
    if (pass->culled)
      continue;
    const bool8_t transfer = pass->desc.type == VKR_RG_PASS_TYPE_TRANSFER;
    const bool8_t encoder_timing_pass =
        submission->pass_timing_requested && !transfer;
    const CFTimeInterval pass_cpu_begin = CACurrentMediaTime();
    if (!first_pass && prepared->consumer_count > 0u) {
      VkrMetalDependency ordering = {0};
      for (uint32_t i = 0u; i < prepared->consumer_count; ++i)
        vkr_metal_packet_merge_dependency(&ordering, &prepared->consumers[i]);
      /* Producers in earlier encoders were awaited at the run's start; only
         this encoder's stages order the passes inside it. */
      ordering.src_stages &= encoder_stages;
      ordering.dst_stages &= encoder_stages;
      if (ordering.src_stages != 0u && ordering.dst_stages != 0u)
        vkr_metal_dependency_encode_intra(encoder, &ordering);
    }
    first_pass = false_v;
    [encoder pushDebugGroup:prepared->label];
    if (encoder_timing_pass)
      [encoder writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                    intoHeap:renderer->active_command_slot
                                                 ->timestamp_heap
                                     atIndex:(NSUInteger)prepared->index * 2u];
    if (prepared->active) {
      if (transfer)
        vkr_metal_packet_record_transfer(encoder, &prepared->transfer);
      else
        vkr_metal_packet_record_compute_pass(renderer, encoder, prepared);
    }
    if (encoder_timing_pass)
      [encoder
          writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                               intoHeap:renderer->active_command_slot
                                            ->timestamp_heap
                                atIndex:(NSUInteger)prepared->index * 2u + 1u];
    [encoder popDebugGroup];
    out_result->dependency_count +=
        prepared->consumer_count + prepared->producer_count;
    if (transfer)
      out_result->transfer_pass_count++;
    else
      out_result->compute_pass_count++;
    vkr_metal_packet_record_pass_timing(
        submission, prepared, encoder_timing_pass, pass_cpu_begin, out_result);
  }

  if (signal.src_stages != 0u)
    vkr_metal_dependency_encode_producer(encoder, &signal);
  if (ibl_baked) {
    vkr_metal_dependency_encode_producer(encoder,
                                         &submission->ibl_bake_barrier);
    out_result->dependency_count++;
  }
  if (submission->capture_count > 0u || submission->deferred_diagnostics ||
      submission->readback_layout.exposure_size > 0u) {
    vkr_metal_dependency_encode_producer(encoder,
                                         &submission->batch_capture_barrier);
    out_result->dependency_count++;
  }
  [encoder endEncoding];
  return true_v;
}

/* Copies the present pixel, diagnostics and capture targets into the readback
 * slice after every producer barrier. */
vkr_internal bool8_t vkr_metal_packet_encode_frame_readback(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    const VkrMetalPacketFrameSubmission *submission,
    VkrMetalPacketResult *out_result) API_AVAILABLE(macos(26.0)) {
  id<MTL4ComputeCommandEncoder> capture_encoder =
      [renderer->command_buffer computeCommandEncoder];
  if (capture_encoder == nil)
    return false_v;
  vkr_metal_dependency_encode_consumer(capture_encoder,
                                       &submission->capture_barrier);
  out_result->dependency_count++;
  if (packet->scene_rendering) {
    vkr_metal_dependency_encode_consumer(capture_encoder,
                                         &submission->shadow_capture_barrier);
    out_result->dependency_count++;
  }
  if (submission->capture_count > 0u || submission->deferred_diagnostics ||
      submission->readback_layout.exposure_size > 0u) {
    vkr_metal_dependency_encode_consumer(capture_encoder,
                                         &submission->batch_capture_barrier);
    out_result->dependency_count++;
  }
  [capture_encoder copyFromTexture:submission->present
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(1, 1, 1)
                          toBuffer:(id<MTLBuffer>)submission->readback_object
                 destinationOffset:submission->readback_slice.offset
            destinationBytesPerRow:8
          destinationBytesPerImage:8
                           options:MTLBlitOptionNone];
  if (packet->scene_rendering) {
    [capture_encoder copyFromTexture:submission->shadow
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(1, 1, 1)
                            toBuffer:(id<MTLBuffer>)submission->readback_object
                   destinationOffset:submission->readback_slice.offset +
                                     submission->readback_layout.shadow_depth
              destinationBytesPerRow:sizeof(float32_t)
            destinationBytesPerImage:sizeof(float32_t)
                             options:MTLBlitOptionNone];
  }
  if (submission->deferred_diagnostics) {
    [capture_encoder
           copyFromBuffer:(id<MTLBuffer>)
                              submission->deferred_state->resource.metal_buffer
             sourceOffset:0u
                 toBuffer:(id<MTLBuffer>)submission->readback_object
        destinationOffset:submission->readback_slice.offset +
                          submission->readback_layout.deferred_diagnostics
                     size:submission->readback_layout
                              .deferred_diagnostics_size];
  }
  /* The classifier counted late Custom draws in the upload's table. */
  if (submission->deferred_diagnostics && renderer->custom_compaction) {
    id<MTLBuffer> upload_buffer = submission->frame_upload.buffer;
    [capture_encoder
           copyFromBuffer:upload_buffer
             sourceOffset:renderer->custom_compaction_gpu +
                          offsetof(VkrMetalCustomCompaction, late_count) -
                          upload_buffer.gpuAddress
                 toBuffer:(id<MTLBuffer>)submission->readback_object
        destinationOffset:submission->readback_slice.offset +
                          submission->readback_layout.custom_late
                     size:sizeof(uint32_t)];
  }
  if (submission->readback_layout.exposure_size > 0u) {
    [capture_encoder
           copyFromBuffer:(id<MTLBuffer>)
                              submission->exposure_state->resource.metal_buffer
             sourceOffset:0u
                 toBuffer:(id<MTLBuffer>)submission->readback_object
        destinationOffset:submission->readback_slice.offset +
                          submission->readback_layout.exposure
                     size:sizeof(VkrExposureGpuState)];
    [capture_encoder
           copyFromBuffer:(id<MTLBuffer>)submission->exposure_histogram
                              ->resource.metal_buffer
             sourceOffset:0u
                 toBuffer:(id<MTLBuffer>)submission->readback_object
        destinationOffset:submission->readback_slice.offset +
                          submission->readback_layout.exposure +
                          sizeof(VkrExposureGpuState)
                     size:sizeof(VkrExposureGpuHistogram)];
  }
  for (uint32_t i = 0; i < submission->capture_count; ++i) {
    const VkrMetalPacketCapturePlan *plan = &submission->capture_plans[i];
    [capture_encoder copyFromTexture:plan->texture
                         sourceSlice:plan->source_slice
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(plan->result.width,
                                                 plan->result.height, 1)
                            toBuffer:(id<MTLBuffer>)submission->readback_object
                   destinationOffset:submission->readback_slice.offset +
                                     plan->buffer_offset
              destinationBytesPerRow:plan->result.row_pitch
            destinationBytesPerImage:plan->result.data_size
                             options:MTLBlitOptionNone];
  }
  [capture_encoder endEncoding];
  return true_v;
}

/* Records every prepared pass and the readback copies into the frame's
 * command buffer. Logs the failure it returns. */
vkr_internal bool8_t vkr_metal_packet_encode_frame_commands(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketFrameSubmission *submission, VkrMetalPacketResult *out_result)
    API_AVAILABLE(macos(26.0)) {
  if (submission->pass_timing_requested) {
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
  bool8_t passed = true_v;
  for (uint64_t order = 0u; passed && order < submission->prepared_count;) {
    const VkrMetalPacketPreparedPass *prepared =
        &submission->prepared_passes[order];
    if (prepared->source->culled) {
      ++order;
      continue;
    }
    if (!vkr_metal_packet_pass_batchable(prepared)) {
      passed = vkr_metal_packet_encode_graph_pass(renderer, submission,
                                                  prepared, out_result);
      ++order;
      continue;
    }
    uint64_t end = order + 1u;
    while (end < submission->prepared_count &&
           (submission->prepared_passes[end].source->culled ||
            vkr_metal_packet_pass_batchable(&submission->prepared_passes[end])))
      ++end;
    passed = vkr_metal_packet_encode_compute_run(renderer, submission, order,
                                                 end, out_result);
    order = end;
  }
  passed = passed && vkr_metal_packet_encode_frame_readback(
                         renderer, packet, submission, out_result);
  [renderer->command_buffer endCommandBuffer];
  if (!passed) {
    if (submission->failed_pass) {
      log_error("Metal packet stage failed: encode pass '%.*s' executor %u",
                (int)submission->failed_pass->desc.name.length,
                submission->failed_pass->desc.name.str,
                submission->failed_pass->desc.executor_id);
    } else {
      log_error("Metal packet stage failed: final capture encoding");
    }
  }
  return passed;
}

/* Records the submit value on every resource and history this frame uses. */
vkr_internal void vkr_metal_packet_mark_frame_submitted(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    const VkrMetalPacketFrameSubmission *submission, uint64_t submit_value) {
  if (submission->frame_upload.ibl_bake_recorded) {
    for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
      const VkrMetalPacketPendingIblBake *job = &renderer->pending_ibl_bakes[i];
      if (job->recorded)
        vkr_ibl_sh_pool_reference(&renderer->sh_pool, job->sh_slot,
                                  submit_value);
    }
  }
  if (packet->input.lighting && packet->input.lighting->ibl_enabled) {
    vkr_ibl_sh_pool_reference(&renderer->sh_pool, renderer->ibl_sh_slot,
                              submit_value);
    for (uint32_t i = 0u; i < packet->input.lighting->ibl_probe_count; ++i) {
      vkr_ibl_sh_pool_reference(&renderer->sh_pool,
                                packet->input.lighting->ibl_probes[i].sh_slot,
                                submit_value);
    }
  }
  vkr_metal_packet_mark_graph_images_submitted(renderer, submit_value);
  if (packet->scene_rendering) {
    vkr_metal_packet_mark_hzb_history_submitted(renderer, submit_value);
    vkr_metal_packet_mark_cloud_history_submitted(renderer, submit_value);
    vkr_metal_packet_mark_exposure_submitted(renderer, submit_value);
  }
  vkr_metal_packet_mark_graph_buffers_submitted(renderer, submit_value);
}

/* Points the command slot at the readback fields its completion decodes. */
vkr_internal void vkr_metal_packet_record_slot_readbacks(
    const VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketFrameSubmission *submission,
    VkrMetalPacketCommandSlot *command_slot) {
  command_slot->gpu_draw_diagnostics_requested =
      submission->deferred_diagnostics;
  command_slot->exposure_requested =
      submission->readback_layout.exposure_size > 0u;
  command_slot->shadow_cascade_count =
      renderer->prepared_frame.shadow_cascade_count;
  command_slot->gpu_draw_view_count =
      1u + renderer->prepared_frame.shadow_cascade_count +
      renderer->prepared_frame.local_shadow_render_count;
  if (submission->deferred_diagnostics) {
    command_slot->gpu_draw_diagnostics_readback =
        (const uint8_t *)submission->readback.cpu_address +
        submission->readback_layout.deferred_diagnostics;
  }
  command_slot->custom_late_readback =
      submission->deferred_diagnostics && renderer->custom_compaction
          ? (const uint32_t *)((const uint8_t *)
                                   submission->readback.cpu_address +
                               submission->readback_layout.custom_late)
          : NULL;
  command_slot->exposure_readback =
      submission->readback_layout.exposure_size > 0u
          ? (const uint8_t *)submission->readback.cpu_address +
                submission->readback_layout.exposure
          : NULL;
}

/* Decodes the readback fields a synchronous validation frame waited for. */
vkr_internal void vkr_metal_packet_read_synchronous_results(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    const VkrMetalPacketFrameSubmission *submission,
    const VkrMetalPacketCommandSlot *command_slot, uint64_t submit_value,
    VkrMetalPacketResult *out_result) {
  if (submission->readback_layout.exposure_size > 0u) {
    VkrExposureGpuState state = {0};
    MemCopy(&state,
            (const uint8_t *)submission->readback.cpu_address +
                submission->readback_layout.exposure,
            sizeof(state));
    (void)vkr_capture_ring_set_display_exposure(
        &renderer->capture_ring, submit_value, state.exposure_multiplier);
  }
  vkr_capture_ring_collect(&renderer->capture_ring,
                           renderer->completion.signaledValue);

  MemCopy(out_result->color, submission->readback.cpu_address, 4);
  if (packet->scene_rendering) {
    MemCopy(&out_result->shadow_depth,
            (uint8_t *)submission->readback.cpu_address +
                submission->readback_layout.shadow_depth,
            sizeof(float32_t));
    out_result->has_shadow_depth = true_v;
  }
  if (submission->picking) {
    MemCopy(&out_result->picking_id,
            (uint8_t *)submission->readback.cpu_address +
                submission->readback_layout.picking,
            sizeof(uint32_t));
    out_result->has_picking_id = true_v;
  }
  if (submission->synchronous_deferred_validation) {
    out_result->gpu_candidate_count = renderer->gpu_draw_count;
    vkr_metal_packet_decode_gpu_draw_diagnostics(
        out_result,
        (const uint8_t *)submission->readback.cpu_address +
            submission->readback_layout.deferred_diagnostics,
        renderer->prepared_frame.shadow_cascade_count,
        command_slot->gpu_draw_view_count);
    out_result->hzb_history_valid = renderer->prepared_frame.hzb_history_valid;
    log_info("Metal deferred validation: candidates=%u visible=%u buckets="
             "%u/%u/%u/%u/%u/%u/%u/%u overflow=%u resolve_invalid=%u "
             "hzb_rejected=%u",
             out_result->gpu_candidate_count, out_result->gpu_visible_count,
             out_result->gpu_bucket_counts[0], out_result->gpu_bucket_counts[1],
             out_result->gpu_bucket_counts[2], out_result->gpu_bucket_counts[3],
             out_result->gpu_bucket_counts[4], out_result->gpu_bucket_counts[5],
             out_result->gpu_bucket_counts[6], out_result->gpu_bucket_counts[7],
             out_result->gpu_overflow_count,
             out_result->gpu_resolve_invalid_count,
             out_result->gpu_occlusion_culled_count);
  }
}

/* Fills the result fields reported once the frame is submitted. */
vkr_internal void vkr_metal_packet_fill_frame_result(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketFrameSubmission *submission,
    VkrMetalPacketResult *out_result) {
  if (submission->capture_count > 0) {
    (void)vkr_capture_ring_poll(
        &renderer->capture_ring, submission->capture_request->request_id,
        renderer->completion.signaledValue, &out_result->capture);
  }
  out_result->packet_build = renderer->packet_build;
  out_result->hzb_history_valid = renderer->prepared_frame.hzb_history_valid;
  out_result->resize_count = renderer->resize_count;
  out_result->pipeline_archive_warm = renderer->pipeline_archive_warm;
  out_result->pipeline_archive_written = renderer->pipeline_archive_written;
  if (submission->deferred_diagnostics)
    out_result->gpu_candidate_count = renderer->gpu_draw_count;
  vkr_metal_memory_device_get_metrics(renderer->memory, &out_result->memory);
  vkr_metal_material_table_device_get_metrics(renderer->materials,
                                              &out_result->materials);
}

bool8_t
vkr_metal_packet_renderer_submit_packet(VkrMetalPacketRenderer *renderer,
                                        const VkrPreparedFrame *packet,
                                        VkrMetalPacketResult *out_result) {
  @autoreleasepool {
    *out_result = (VkrMetalPacketResult){0};
    if (!@available(macOS 26.0, *)) {
      vkr_metal_packet_finish_frame(renderer, false_v, false_v);
      return false_v;
    }

    if (!renderer->frame_prepared || !renderer->reserved_frame_slot)
      return false_v;
    if (!vkr_metal_packet_activate_frame_slot(renderer)) {
      vkr_metal_packet_finish_frame(renderer, false_v, false_v);
      return false_v;
    }

    vkr_metal_packet_prepare_frame_state(renderer, packet, out_result);
    const uint64_t completed_submit_value = renderer->completion.signaledValue;
    vkr_capture_ring_collect(&renderer->capture_ring, completed_submit_value);
    uint32_t collected = 0;
    if (vkr_metal_packet_collect_memory(renderer, completed_submit_value,
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
    if (!vkr_metal_packet_realize_frame_graph(renderer, packet, out_result) ||
        !vkr_metal_packet_select_frame_histories(renderer, packet))
      goto graph_failure;

    VkrMetalPacketFrameSubmission submission = {0};
    vkr_metal_packet_plan_frame_submission(renderer, packet, &submission);
    if (!vkr_metal_packet_reserve_frame_capture(renderer, packet, &submission,
                                                out_result)) {
      vkr_metal_packet_finish_frame(renderer, true_v, false_v);
      return false_v;
    }

    VkrRendererError upload_error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_metal_packet_prepare_upload(
            renderer, packet, &submission.frame_upload, &upload_error)) {
      if (upload_error == VKR_RENDERER_ERROR_NONE)
        upload_error = VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
      out_result->error = upload_error;
      log_error("Metal packet stage failed: frame upload (world=%u ui=%u)",
                packet->input.world ? packet->input.world->instance_count : 0,
                packet->input.ui ? packet->input.ui->draw_list.vertex_count
                                 : 0);
      vkr_metal_packet_rollback_gpu_draw_resource_preparation(renderer, packet);
      if (submission.frame_upload.acquired)
        vkr_metal_memory_device_cancel_ring(renderer->memory,
                                            VKR_METAL_RING_KIND_UPLOAD,
                                            submission.frame_upload.slice);
      vkr_metal_packet_capture_fail_reserved(
          renderer, submission.capture_request, upload_error);
      vkr_metal_packet_finish_frame(renderer, true_v, false_v);
      return false_v;
    }
    const VkrRendererError readback_error =
        vkr_metal_packet_acquire_frame_readback(renderer, &submission);
    if (readback_error != VKR_RENDERER_ERROR_NONE) {
      out_result->error = readback_error;
      vkr_metal_packet_rollback_gpu_draw_resource_preparation(renderer, packet);
      vkr_metal_memory_device_cancel_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_UPLOAD,
                                          submission.frame_upload.slice);
      vkr_metal_packet_capture_fail_reserved(
          renderer, submission.capture_request, readback_error);
      vkr_metal_packet_finish_frame(renderer, true_v, false_v);
      return false_v;
    }

    if (!vkr_metal_packet_prepare_frame_passes(renderer, packet, &submission,
                                               out_result)) {
      vkr_metal_packet_rollback_gpu_draw_resource_preparation(renderer, packet);
      vkr_metal_packet_abandon_ibl_bake_recordings(renderer,
                                                   &submission.frame_upload);
      vkr_metal_memory_device_cancel_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_READBACK,
                                          submission.readback_slice);
      vkr_metal_memory_device_cancel_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_UPLOAD,
                                          submission.frame_upload.slice);
      vkr_metal_packet_capture_fail_reserved(
          renderer, submission.capture_request,
          VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
      vkr_metal_packet_finish_frame(renderer, true_v, false_v);
      return false_v;
    }
    if (!vkr_metal_packet_encode_frame_commands(renderer, packet, &submission,
                                                out_result)) {
      vkr_metal_packet_rollback_gpu_draw_resource_preparation(renderer, packet);
      vkr_metal_packet_abandon_ibl_bake_recordings(renderer,
                                                   &submission.frame_upload);
      vkr_metal_memory_device_cancel_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_READBACK,
                                          submission.readback_slice);
      vkr_metal_memory_device_cancel_ring(renderer->memory,
                                          VKR_METAL_RING_KIND_UPLOAD,
                                          submission.frame_upload.slice);
      vkr_metal_packet_capture_fail_reserved(
          renderer, submission.capture_request,
          VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED);
      vkr_metal_packet_finish_frame(renderer, true_v, false_v);
      return false_v;
    }

    const uint64_t submit_value = ++renderer->submit_value;
    renderer->pre_exposure_history[renderer->pre_exposure_history_next] =
        (VkrMetalPacketPreExposureRecord){
            .submit_value = submit_value,
            .stops = packet->exposure.pre_exposure_stops,
        };
    renderer->pre_exposure_history_next =
        (renderer->pre_exposure_history_next + 1u) %
        VKR_METAL_PACKET_PRE_EXPOSURE_HISTORY;
    vkr_metal_packet_mark_frame_submitted(renderer, packet, &submission,
                                          submit_value);
    VkrMetalPacketCommandSlot *command_slot = renderer->active_command_slot;
    command_slot->submit_value = submit_value;
    command_slot->timestamp_entry_count =
        submission.pass_timing_requested
            ? (uint32_t)renderer->graph->passes.length * 2u
            : 0u;
    command_slot->pass_timing_requested = submission.pass_timing_requested;
    VkrMetalPacketCommitFeedbackRecord *commit_feedback =
        submission.submission_timing_requested
            ? vkr_metal_packet_acquire_commit_feedback(renderer)
            : NULL;
    command_slot->commit_feedback = commit_feedback;
    out_result->gpu_submission_unavailable_reason =
        submission.submission_timing_requested
            ? (commit_feedback
                   ? VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY
                   : VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_UNAVAILABLE)
            : VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED;
    vkr_metal_packet_record_slot_readbacks(renderer, &submission, command_slot);
    id<MTL4CommandBuffer> submissions[] = {renderer->command_buffer};
    VKR_METAL_DIAGNOSTIC(renderer, "frame_queue_begin",
                         "frame=%llu slot=%u drawable=%u",
                         (unsigned long long)packet->input.frame.frame_index,
                         (uint32_t)(command_slot - renderer->command_slots),
                         renderer->drawable != nil);
    VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
    if (renderer->drawable)
      [renderer->queue waitForDrawable:renderer->drawable];
    out_result->submit_value = submit_value;
    out_result->source_frame_index = packet->input.frame.frame_index;
    command_slot->pending_result = *out_result;
    command_slot->result_pending = true_v;
    VKR_METAL_DIAGNOSTIC(renderer, "frame_commit_begin",
                         "frame=%llu slot=%u passes=%u",
                         (unsigned long long)packet->input.frame.frame_index,
                         (uint32_t)(command_slot - renderer->command_slots),
                         out_result->executed_pass_count);
    VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
    if (commit_feedback)
      [renderer->queue commit:submissions
                        count:1
                      options:commit_feedback->options];
    else
      [renderer->queue commit:submissions count:1];
    [renderer->queue signalEvent:renderer->completion value:submit_value];
    if (submission.picking) {
      command_slot->picking_readback =
          (const uint8_t *)submission.readback.cpu_address +
          submission.readback_layout.picking;
      renderer->picking_submit_value = submit_value;
      renderer->picking_result = (VkrPixelReadbackResult){
          .status = VKR_READBACK_STATUS_PENDING,
          .request_id = packet->input.picking->request_id,
          .x = packet->input.picking->x,
          .y = packet->input.picking->y,
      };
    }
    VKR_METAL_DIAGNOSTIC(renderer, "frame_commit_end", "frame=%llu slot=%u",
                         (unsigned long long)packet->input.frame.frame_index,
                         (uint32_t)(command_slot - renderer->command_slots));
    VKR_METAL_DIAGNOSTIC_FLUSH(renderer);
    if (command_slot->candidate_residency_pending) {
      command_slot->candidate_residency =
          command_slot->pending_candidate_residency;
      command_slot->candidate_residency_pending = false_v;
    }
    /* Past the commit, so retained contents are now proven to have been
       written. Every earlier failure path returns without reaching here, which
       is the ADR-029 rollback: a cancelled frame commits nothing and the
       previous contents stay authoritative. */
    vkr_rg_commit_retained_state(renderer->graph);
    if (submission.frame_upload.ibl_bake_recorded)
      vkr_metal_packet_publish_ibl_bakes(renderer, submit_value);
    if (renderer->drawable) {
      [renderer->queue signalDrawable:renderer->drawable];
      [renderer->drawable present];
      VKR_METAL_DIAGNOSTIC(renderer, "drawable_present", "frame=%llu",
                           (unsigned long long)packet->input.frame.frame_index);
    }
    const bool8_t capture_submitted =
        submission.capture_count == 0 ||
        vkr_capture_ring_submit(
            &renderer->capture_ring, submission.capture_request->request_id,
            submit_value,
            (const uint8_t *)submission.readback.cpu_address +
                submission.capture_base);
    const VkrMetalMemoryStatus readback_submitted =
        vkr_metal_memory_device_submit_ring(
            renderer->memory, VKR_METAL_RING_KIND_READBACK,
            submission.readback_slice, submit_value);
    const VkrMetalMemoryStatus upload_submitted =
        vkr_metal_memory_device_submit_ring(
            renderer->memory, VKR_METAL_RING_KIND_UPLOAD,
            submission.frame_upload.slice, submit_value);
    if (!capture_submitted ||
        readback_submitted != VKR_METAL_MEMORY_STATUS_OK ||
        upload_submitted != VKR_METAL_MEMORY_STATUS_OK) {
      log_error(
          "Metal packet stage failed: submit rings capture=%u readback=%s "
          "upload=%s",
          capture_submitted, vkr_metal_memory_status_string(readback_submitted),
          vkr_metal_memory_status_string(upload_submitted));
      if (vkr_metal_packet_wait(renderer, submit_value)) {
        (void)vkr_metal_packet_complete_command_slot(renderer, command_slot,
                                                     false_v);
        if (readback_submitted != VKR_METAL_MEMORY_STATUS_OK)
          vkr_metal_memory_device_cancel_ring(renderer->memory,
                                              VKR_METAL_RING_KIND_READBACK,
                                              submission.readback_slice);
        if (upload_submitted != VKR_METAL_MEMORY_STATUS_OK)
          vkr_metal_memory_device_cancel_ring(renderer->memory,
                                              VKR_METAL_RING_KIND_UPLOAD,
                                              submission.frame_upload.slice);
      }
      vkr_metal_packet_detach_active_command_slot(renderer);
      vkr_metal_packet_capture_fail_reserved(
          renderer, submission.capture_request,
          VKR_RENDERER_ERROR_SUBMISSION_FAILED);
      vkr_metal_packet_finish_frame(renderer, true_v, true_v);
      return false_v;
    }
    if (renderer->synchronous_validation_readback) {
      if (!vkr_metal_packet_wait(renderer, submit_value)) {
        vkr_metal_packet_detach_active_command_slot(renderer);
        vkr_metal_packet_finish_frame(renderer, true_v, true_v);
        return false_v;
      }
      vkr_metal_packet_read_synchronous_results(renderer, packet, &submission,
                                                command_slot, submit_value,
                                                out_result);
    }
    vkr_metal_packet_fill_frame_result(renderer, &submission, out_result);
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
}

/* Native command recording consumes prepared pass resources and root spans. */
vkr_internal void vkr_metal_packet_record_dispatch(
    VkrMetalPacketRenderer *renderer, id<MTL4ComputeCommandEncoder> encoder,
    const VkrMetalPacketDispatch *dispatch) API_AVAILABLE(macos(26.0)) {
  [renderer->argument_table setAddress:dispatch->root atIndex:0u];
  [encoder setComputePipelineState:dispatch->pipeline];
  [encoder setArgumentTable:renderer->argument_table];
  [encoder dispatchThreads:dispatch->threads
      threadsPerThreadgroup:dispatch->group_size];
}

/* Records the bakes IBL.Bake prepared. An atmosphere bake writes its lookups,
   source and source mips before its prefilter and SH projection read them. A
   final barrier covers later passes in this encoder; the compute run's
   producer barrier covers later encoders. */
vkr_internal void
vkr_metal_packet_record_ibl_bakes(VkrMetalPacketRenderer *renderer,
                                  id<MTL4ComputeCommandEncoder> encoder)
    API_AVAILABLE(macos(26.0)) {
  /* A source can come from an earlier texture-upload submission, and Metal 4
     does not order separate command buffers, so wait before reading it. */
  [encoder barrierAfterQueueStages:MTLStageDispatch | MTLStageBlit
                      beforeStages:MTLStageDispatch | MTLStageBlit
                 visibilityOptions:MTL4VisibilityOptionDevice];
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    const VkrMetalPacketPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    if (!job->recorded)
      continue;
    if (job->is_atmosphere) {
      const VkrMetalPacketTexture *source =
          vkr_metal_packet_resolve_texture(renderer, job->source);
      id<MTLTexture> source_texture =
          (id<MTLTexture>)source->resource.metal_texture;
      [renderer->argument_table setAddress:job->atmosphere_roots[0] atIndex:0u];
      [encoder
          setComputePipelineState:renderer->atmosphere_transmittance_pipeline];
      [encoder setArgumentTable:renderer->argument_table];
      [encoder dispatchThreads:MTLSizeMake(VKR_ATMOSPHERE_TRANSMITTANCE_WIDTH,
                                           VKR_ATMOSPHERE_TRANSMITTANCE_HEIGHT,
                                           1u)
          threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
      [encoder barrierAfterEncoderStages:MTLStageDispatch
                     beforeEncoderStages:MTLStageDispatch
                       visibilityOptions:MTL4VisibilityOptionDevice];

      [renderer->argument_table setAddress:job->atmosphere_roots[1] atIndex:0u];
      [encoder setComputePipelineState:renderer->
                                       atmosphere_multiple_scattering_pipeline];
      [encoder setArgumentTable:renderer->argument_table];
      [encoder
           dispatchThreadgroups:MTLSizeMake(
                                    VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE,
                                    VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE, 1u)
          threadsPerThreadgroup:
              MTLSizeMake(1u, 1u,
                          VKR_METAL_PACKET_ATMOSPHERE_MULTI_DIRECTIONS)];
      [encoder barrierAfterEncoderStages:MTLStageDispatch
                     beforeEncoderStages:MTLStageDispatch
                       visibilityOptions:MTL4VisibilityOptionDevice];

      [renderer->argument_table setAddress:job->atmosphere_roots[2] atIndex:0u];
      [encoder setComputePipelineState:renderer->atmosphere_source_pipeline];
      [encoder setArgumentTable:renderer->argument_table];
      [encoder dispatchThreads:MTLSizeMake(source_texture.width,
                                           source_texture.height, 6u)
          threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
      [encoder barrierAfterEncoderStages:MTLStageDispatch
                     beforeEncoderStages:MTLStageBlit
                       visibilityOptions:MTL4VisibilityOptionDevice];
      if (source_texture.mipmapLevelCount > 1u)
        [encoder generateMipmapsForTexture:source_texture];
      [encoder barrierAfterEncoderStages:MTLStageBlit
                     beforeEncoderStages:MTLStageDispatch
                       visibilityOptions:MTL4VisibilityOptionDevice];
    }

    for (uint32_t mip = 0u; mip < job->prefilter_mip_count; ++mip) {
      const uint32_t extent = Max(1u, job->prefilter_size >> mip);
      const VkrMetalPacketDispatch dispatch = {
          .pipeline = renderer->ibl_prefilter_pipeline,
          .root = job->prefilter_roots[mip],
          .threads = MTLSizeMake(extent, extent, 6u),
          .group_size = MTLSizeMake(8u, 8u, 1u),
      };
      vkr_metal_packet_record_dispatch(renderer, encoder, &dispatch);
    }
    if (job->sh_root) {
      [renderer->argument_table setAddress:job->sh_root atIndex:0u];
      [encoder setComputePipelineState:renderer->ibl_sh_pipeline];
      [encoder setArgumentTable:renderer->argument_table];
      /* One workgroup per destination slot; the kernel's fixed reduction is
         what makes the result deterministic, so this must stay a single
         group. */
      [encoder dispatchThreadgroups:MTLSizeMake(1u, 1u, 1u)
              threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
    }
  }
  [encoder
      barrierAfterEncoderStages:MTLStageDispatch
            beforeEncoderStages:(MTLStages)(MTLStageDispatch | MTLStageBlit)
              visibilityOptions:MTL4VisibilityOptionDevice];
}

vkr_internal void vkr_metal_packet_record_compute_pass(
    VkrMetalPacketRenderer *renderer, id<MTL4ComputeCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  switch (pass->kind) {
  case VKR_RG_EXECUTOR_SKINNING:
    for (uint32_t i = 0u; i < pass->skinning.count; ++i) {
      const VkrMetalPacketDispatch dispatch = {
          .pipeline = renderer->skinning_pipeline,
          .root = pass->skinning.roots[i],
          .threads = MTLSizeMake(pass->skinning.vertices[i], 1u, 1u),
          .group_size = MTLSizeMake(64u, 1u, 1u),
      };
      vkr_metal_packet_record_dispatch(renderer, encoder, &dispatch);
    }
    break;
  case VKR_RG_EXECUTOR_GPU_DRAW_ENCODE: {
    for (uint32_t i = 0u; i < pass->gpu_encode.group_count; ++i) {
      const VkrMetalPacketGpuEncodeGroup *group = &pass->gpu_encode.groups[i];
      for (uint32_t view = 0u; view < group->view_count; ++view) {
        /* A retained cascade's pass does not run, so its stale commands are
           never executed; the frame that redraws it resets them first. */
        const uint32_t global_view = group->first_view + view;
        if (global_view < 64u &&
            (pass->gpu_encode.idle_view_mask & (UINT64_C(1) << global_view)))
          continue;
        [encoder resetCommandsInBuffer:group->commands
                             withRange:NSMakeRange(
                                           (uint64_t)view *
                                               pass->gpu_encode.candidate_count,
                                           pass->gpu_encode.candidate_count)];
      }
    }
    /* Metal 4 resets execute at the blit stage, concurrently with dispatches
       unless this intra-pass dependency orders command generation after them.
     */
    const VkrMetalDependency reset_to_encode = {
        .src_stages = MTLStageBlit,
        .dst_stages = MTLStageDispatch,
        .visibility_options = MTL4VisibilityOptionDevice,
    };
    vkr_metal_dependency_encode_intra(encoder, &reset_to_encode);
    [encoder setComputePipelineState:renderer->gpu_draw_encode_pipeline];
    for (uint32_t i = 0u; i < pass->gpu_encode.group_count; ++i) {
      const VkrMetalPacketGpuEncodeGroup *group = &pass->gpu_encode.groups[i];
      [renderer->argument_table setAddress:group->root atIndex:0u];
      [renderer->argument_table setAddress:group->arguments atIndex:1u];
      [encoder setArgumentTable:renderer->argument_table];
      [encoder dispatchThreads:MTLSizeMake(pass->gpu_encode.candidate_count,
                                           group->view_count, 1u)
          threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
    }
    break;
  }
  case VKR_RG_EXECUTOR_IBL_BAKE:
    vkr_metal_packet_record_ibl_bakes(renderer, encoder);
    break;
  case VKR_RG_EXECUTOR_EXPOSURE_HISTOGRAM:
    [renderer->argument_table setAddress:pass->histogram.root atIndex:0u];
    [encoder setComputePipelineState:renderer->exposure_clear_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u,
                                         1u)
        threadsPerThreadgroup:MTLSizeMake(VKR_EXPOSURE_HISTOGRAM_BIN_COUNT, 1u,
                                          1u)];
    [encoder barrierAfterEncoderStages:MTLStageDispatch
                   beforeEncoderStages:MTLStageDispatch
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder setComputePipelineState:renderer->exposure_histogram_pipeline];
    [encoder dispatchThreadgroups:pass->histogram.groups
            threadsPerThreadgroup:MTLSizeMake(16u, 16u, 1u)];
    break;
  case VKR_RG_EXECUTOR_CLOUD_SKY_LIGHT: {
    /* The march, then the projection and the chain that read its first
       level; later passes in this encoder read the SH slot and the chain,
       which no graph resource orders. */
    const uint32_t extent = pass->cloud_sky_light.extent;
    [renderer->argument_table setAddress:pass->cloud_sky_light.root atIndex:0u];
    [encoder setComputePipelineState:renderer->cloud_sky_light_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(extent, extent, 6u)
        threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
    [encoder barrierAfterEncoderStages:MTLStageDispatch
                   beforeEncoderStages:MTLStageDispatch
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder setComputePipelineState:renderer->cloud_sky_light_sh_pipeline];
    [encoder dispatchThreadgroups:MTLSizeMake(1u, 1u, 1u)
            threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
    [encoder barrierAfterEncoderStages:MTLStageDispatch
                   beforeEncoderStages:MTLStageDispatch
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder setComputePipelineState:renderer->cloud_sky_light_mips_pipeline];
    [encoder dispatchThreadgroups:MTLSizeMake(1u, 1u, 6u)
            threadsPerThreadgroup:MTLSizeMake(VKR_CLOUD_SKY_LIGHT_FACE_SIZE,
                                              VKR_CLOUD_SKY_LIGHT_FACE_SIZE,
                                              1u)];
    [encoder barrierAfterEncoderStages:MTLStageDispatch
                   beforeEncoderStages:MTLStageDispatch
                     visibilityOptions:MTL4VisibilityOptionDevice];
    break;
  }
  case VKR_RG_EXECUTOR_PICKING_READBACK:
    [encoder copyFromTexture:pass->picking_readback.source
                     sourceSlice:0u
                     sourceLevel:0u
                    sourceOrigin:pass->picking_readback.origin
                      sourceSize:MTLSizeMake(1u, 1u, 1u)
                        toBuffer:pass->picking_readback.destination
               destinationOffset:pass->picking_readback.destination_offset
          destinationBytesPerRow:sizeof(uint32_t)
        destinationBytesPerImage:sizeof(uint32_t)
                         options:MTLBlitOptionNone];
    /* The depth follows the object id in the readback layout. */
    if (pass->picking_readback.depth_source)
      [encoder copyFromTexture:pass->picking_readback.depth_source
                       sourceSlice:0u
                       sourceLevel:0u
                      sourceOrigin:pass->picking_readback.origin
                        sourceSize:MTLSizeMake(1u, 1u, 1u)
                          toBuffer:pass->picking_readback.destination
                 destinationOffset:pass->picking_readback.destination_offset +
                                   sizeof(uint32_t)
            destinationBytesPerRow:sizeof(float32_t)
          destinationBytesPerImage:sizeof(float32_t)
                           options:MTLBlitOptionNone];
    break;
  default:
    vkr_metal_packet_record_dispatch(renderer, encoder, &pass->dispatch);
    break;
  }
}

/* Executes Custom graph slot `slot`'s range of camera bucket `bucket`. */
vkr_internal void
vkr_metal_packet_record_custom_bucket(id<MTL4RenderCommandEncoder> encoder,
                                      const VkrMetalPacketIndirectPass *draw,
                                      uint32_t slot, uint32_t bucket)
    API_AVAILABLE(macos(26.0)) {
  [encoder setCullMode:(bucket & 1u) ? MTLCullModeNone : MTLCullModeBack];
  [encoder setFrontFacingWinding:(bucket & VKR_GPU_DRAW_STATE_MIRRORED_BIT)
                                     ? MTLWindingClockwise
                                     : MTLWindingCounterClockwise];
  [encoder executeCommandsInBuffer:draw->commands
                    indirectBuffer:draw->custom_arguments +
                                   ((uint64_t)slot *
                                        VKR_WORLD_DRAW_STATE_BUCKET_COUNT +
                                    bucket) *
                                       2u * sizeof(uint32_t)];
}

vkr_internal void vkr_metal_packet_record_indirect_pass(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  const VkrMetalPacketIndirectPass *draw = &pass->indirect;
  [encoder setViewport:draw->viewport];
  if (draw->clear_depth) {
    [encoder setScissorRect:draw->clear_rect];
    [encoder setDepthStencilState:renderer->depth_clear_state];
    [encoder setRenderPipelineState:renderer->depth_clear_pipeline];
    [encoder setCullMode:MTLCullModeNone];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0u
                vertexCount:3u];
  }
  [encoder setDepthStencilState:renderer->depth_write_state];
  if (!draw->commands)
    return;
  if (pass->kind == VKR_RG_EXECUTOR_SHADOW)
    [encoder setDepthBias:draw->depth_bias.depth_bias_constant
               slopeScale:draw->depth_bias.depth_bias_slope
                    clamp:draw->depth_bias.depth_bias_clamp];
  id<MTLRenderPipelineState> bound_pipeline = nil;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    id<MTLRenderPipelineState> pipeline =
        draw->opaque_pipeline && (bucket & 2u) == 0u ? draw->opaque_pipeline
                                                     : draw->pipeline;
    if (pipeline != bound_pipeline) {
      [encoder setRenderPipelineState:pipeline];
      bound_pipeline = pipeline;
    }
    [encoder setCullMode:(bucket & 1u) ? MTLCullModeNone : MTLCullModeBack];
    [encoder setFrontFacingWinding:(bucket & VKR_GPU_DRAW_STATE_MIRRORED_BIT)
                                       ? MTLWindingClockwise
                                       : MTLWindingCounterClockwise];
    [encoder executeCommandsInBuffer:draw->commands
                      indirectBuffer:draw->arguments +
                                     (uint64_t)bucket * 2u * sizeof(uint32_t)];
  }
  /* A camera pass draws the Custom graphs' buckets with its own pipeline:
     it writes no shading. */
  for (uint32_t slot = 1u;
       draw->custom_arguments && slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    if ((draw->custom_ready_mask & (1u << (slot - 1u))) == 0u)
      continue;
    for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
         ++bucket) {
      id<MTLRenderPipelineState> pipeline =
          draw->opaque_pipeline && (bucket & 2u) == 0u ? draw->opaque_pipeline
                                                       : draw->pipeline;
      if (pipeline != bound_pipeline) {
        [encoder setRenderPipelineState:pipeline];
        bound_pipeline = pipeline;
      }
      vkr_metal_packet_record_custom_bucket(encoder, draw, slot, bucket);
    }
  }
}

/* The tiled pipeline's opaque pass (ADR-087): shading of the opaque
   buckets, which cannot discard, so hidden-surface removal shades each
   sample's nearest surface once, then of the alpha-tested buckets, which lay
   the depth of the samples they cover, then the clear sky behind them, then
   the tone-mapped resolve of their samples. */
vkr_internal void vkr_metal_packet_record_tiled_opaque(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  const VkrMetalPacketIndirectPass *draw = &pass->tiled.draws;
  [encoder setViewport:draw->viewport];
  if (draw->commands) {
    /* Opaque buckets shade first, without discard, then the alpha-tested
       ones write the samples their alpha covers. */
    [encoder setDepthStencilState:renderer->tiled_shade_state];
    for (uint32_t cutout = 0u; cutout < 2u; ++cutout) {
      [encoder setRenderPipelineState:cutout ? draw->pipeline
                                             : draw->opaque_pipeline];
      for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
           ++bucket) {
        if (((bucket & 2u) != 0u) != (cutout != 0u)) {
          continue;
        }
        [encoder setCullMode:(bucket & 1u) ? MTLCullModeNone : MTLCullModeBack];
        [encoder
            setFrontFacingWinding:(bucket & VKR_GPU_DRAW_STATE_MIRRORED_BIT)
                                      ? MTLWindingClockwise
                                      : MTLWindingCounterClockwise];
        [encoder
            executeCommandsInBuffer:draw->commands
                     indirectBuffer:draw->arguments +
                                    (uint64_t)bucket * 2u * sizeof(uint32_t)];
      }
      /* Each ready Custom graph shades its buckets with its own pipelines
         of the frame's forward variant, in the same order. */
      for (uint32_t slot = 1u;
           draw->custom_arguments && slot < VKR_METAL_CUSTOM_GRAPH_SLOTS;
           ++slot) {
        if ((draw->custom_ready_mask & (1u << (slot - 1u))) == 0u)
          continue;
        const VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
        const uint32_t decals = pass->tiled.decals;
        const uint32_t probes = pass->tiled.probes;
        const uint32_t lighting = pass->tiled.lighting;
        [encoder setRenderPipelineState:cutout ? graph->coverage[decals][probes]
                                                                [lighting]
                                               : graph->forward[decals][probes]
                                                               [lighting]];
        for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
             ++bucket) {
          if (((bucket & 2u) != 0u) != (cutout != 0u))
            continue;
          vkr_metal_packet_record_custom_bucket(encoder, draw, slot, bucket);
        }
      }
    }
  }
  [renderer->argument_table setAddress:pass->tiled.sky_root atIndex:1u];
  [encoder setArgumentTable:renderer->argument_table
                   atStages:MTLRenderStageVertex | MTLRenderStageFragment];
  [encoder setDepthStencilState:renderer->tiled_sky_state];
  [encoder setRenderPipelineState:renderer->tiled_sky_pipeline];
  [encoder setCullMode:MTLCullModeNone];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0u
              vertexCount:3u];
  [encoder setRenderPipelineState:renderer->tiled_resolve_pipeline];
  [encoder dispatchThreadsPerTile:MTLSizeMake(VKR_METAL_TILED_TILE_WIDTH,
                                              VKR_METAL_TILED_TILE_HEIGHT, 1u)];
}

/* The cloud layer, aerial perspective and fog over the tiled pipeline's
   resolved image. */
vkr_internal void vkr_metal_packet_record_tiled_atmosphere(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  [encoder setViewport:pass->tiled_atmosphere.viewport];
  [renderer->argument_table setAddress:pass->tiled_atmosphere.root atIndex:1u];
  [encoder setArgumentTable:renderer->argument_table
                   atStages:MTLRenderStageVertex | MTLRenderStageFragment];
  [encoder setRenderPipelineState:renderer->tiled_atmosphere_pipeline];
  [encoder setCullMode:MTLCullModeNone];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0u
              vertexCount:3u];
}

vkr_internal void vkr_metal_packet_record_graphics_pass(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  switch (pass->kind) {
  case VKR_RG_EXECUTOR_TILED_OPAQUE:
    vkr_metal_packet_record_tiled_opaque(renderer, encoder, pass);
    break;
  case VKR_RG_EXECUTOR_TILED_ATMOSPHERE:
    vkr_metal_packet_record_tiled_atmosphere(renderer, encoder, pass);
    break;
  case VKR_RG_EXECUTOR_SHADOW:
  case VKR_RG_EXECUTOR_LOCAL_SHADOW:
  case VKR_RG_EXECUTOR_TILED_PICKING:
    vkr_metal_packet_record_indirect_pass(renderer, encoder, pass);
    break;
  case VKR_RG_EXECUTOR_TILED_BLEND:
  case VKR_RG_EXECUTOR_PICKING: {
    const VkrMetalPacketDirectPass *direct = &pass->world.direct;
    if (direct->count)
      vkr_metal_packet_record_prepared_draws(
          renderer, encoder, direct->pipeline, direct->depth, direct->draws,
          direct->count, direct->width, direct->height, direct->root_gpu);
    const VkrMetalPacketTextPass *text = &pass->world.text;
    if (text->count)
      vkr_metal_packet_record_prepared_text_draws(
          renderer, encoder, text->pipeline, text->depth, text->root_cpu,
          text->root_gpu, text->count, text->width, text->height);
    break;
  }
  case VKR_RG_EXECUTOR_ANIMATION_PREVIEW:
  case VKR_RG_EXECUTOR_EDITOR_OVERLAY:
  case VKR_RG_EXECUTOR_EDITOR_OVERLAY_PICKING:
  case VKR_RG_EXECUTOR_EDITOR_SELECTION_MASK: {
    const VkrMetalPacketDirectPass *direct = &pass->world.direct;
    if (direct->count)
      vkr_metal_packet_record_prepared_draws(
          renderer, encoder, direct->pipeline, direct->depth, direct->draws,
          direct->count, direct->width, direct->height, direct->root_gpu);
    break;
  }
  case VKR_RG_EXECUTOR_UI:
    if (pass->ui.count)
      vkr_metal_packet_record_prepared_ui_draws(
          renderer, encoder, pass->ui.root_cpu, pass->ui.root_gpu,
          pass->ui.count, pass->ui.width, pass->ui.height);
    break;
  case VKR_RG_EXECUTOR_EDITOR_SELECTION_OUTLINE:
  case VKR_RG_EXECUTOR_EDITOR_GRID:
    [renderer->argument_table setAddress:pass->tonemap.root atIndex:1u];
    [encoder setViewport:pass->tonemap.viewport];
    [encoder setScissorRect:pass->tonemap.scissor];
    [encoder setRenderPipelineState:pass->kind == VKR_RG_EXECUTOR_EDITOR_GRID
                                        ? renderer->editor_grid_pipeline
                                        : renderer->
                                          editor_selection_outline_pipeline];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageFragment];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0u
                vertexCount:3u];
    break;
  case VKR_RG_EXECUTOR_TONEMAP:
  case VKR_RG_EXECUTOR_TONEMAP_PREPARE:
  case VKR_RG_EXECUTOR_EDITOR:
    [renderer->argument_table setAddress:pass->tonemap.root atIndex:1u];
    [encoder setViewport:pass->tonemap.viewport];
    [encoder setScissorRect:pass->tonemap.scissor];
    [encoder
        setRenderPipelineState:pass->kind == VKR_RG_EXECUTOR_TONEMAP_PREPARE
                                   ? renderer->display_linear_pipeline
                                   : renderer->tonemap_pipeline];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageFragment];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0u
                vertexCount:3u];
    break;
  default:
    break;
  }
}

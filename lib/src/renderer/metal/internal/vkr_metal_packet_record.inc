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

vkr_internal void vkr_metal_packet_record_compute_pass(
    VkrMetalPacketRenderer *renderer, id<MTL4ComputeCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  switch (pass->kind) {
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_GPU_DRAW_ENCODE:
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE:
    [encoder setComputePipelineState:pass->gpu_encode.pipeline];
    for (uint32_t i = 0u; i < pass->gpu_encode.group_count; ++i) {
      const VkrMetalPacketGpuEncodeGroup *group = &pass->gpu_encode.groups[i];
      for (uint32_t view = 0u; view < group->view_count; ++view)
        [encoder resetCommandsInBuffer:group->commands
                             withRange:NSMakeRange(
                                           (uint64_t)view *
                                               VKR_GPU_DRAW_CANDIDATE_CAPACITY,
                                           pass->gpu_encode.candidate_count)];
      [renderer->argument_table setAddress:group->root atIndex:0u];
      [renderer->argument_table setAddress:group->arguments atIndex:1u];
      [encoder setArgumentTable:renderer->argument_table];
      [encoder dispatchThreads:MTLSizeMake(pass->gpu_encode.candidate_count,
                                           group->view_count, 1u)
          threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
    }
    break;
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_IBL_BAKE:
    for (uint32_t mip = 0u; mip < VKR_IBL_PREFILTER_MIP_COUNT; ++mip) {
      const uint32_t extent = Max(1u, VKR_IBL_PREFILTER_SIZE >> mip);
      const VkrMetalPacketDispatch dispatch = {
          .pipeline = renderer->ibl_prefilter_pipeline,
          .root = pass->ibl.prefilter_roots[mip],
          .threads = MTLSizeMake(extent, extent, 6u),
          .group_size = MTLSizeMake(8u, 8u, 1u),
      };
      vkr_metal_packet_record_dispatch(renderer, encoder, &dispatch);
    }
    if (pass->ibl.sh_root) {
      [renderer->argument_table setAddress:pass->ibl.sh_root atIndex:0u];
      [encoder setComputePipelineState:renderer->ibl_sh_pipeline];
      [encoder setArgumentTable:renderer->argument_table];
      [encoder dispatchThreadgroups:MTLSizeMake(1u, 1u, 1u)
              threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
      [encoder barrierAfterEncoderStages:MTLStageDispatch
                     beforeEncoderStages:(MTLStages)(MTLStageDispatch |
                                                     MTLStageFragment)
                       visibilityOptions:MTL4VisibilityOptionDevice];
    }
    break;
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_SHADE:
    [renderer->argument_table setAddress:pass->transmission.root atIndex:0u];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder setComputePipelineState:pass->transmission.pipeline];
    if (pass->transmission.indirect)
      [encoder
          dispatchThreadgroupsWithIndirectBuffer:pass->transmission.arguments
                           threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
    else
      [encoder dispatchThreads:pass->transmission.threads
          threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
    break;
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_TRANSMISSION_COMPACT:
    [renderer->argument_table setAddress:pass->compact.root atIndex:0u];
    [encoder
        setComputePipelineState:renderer->transmission_compact_clear_pipeline];
    [encoder setArgumentTable:renderer->argument_table];
    [encoder dispatchThreads:MTLSizeMake(1u, 1u, 1u)
        threadsPerThreadgroup:MTLSizeMake(1u, 1u, 1u)];
    [encoder barrierAfterEncoderStages:MTLStageDispatch
                   beforeEncoderStages:MTLStageDispatch
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder setComputePipelineState:renderer->transmission_compact_pipeline];
    [encoder dispatchThreads:pass->compact.extent
        threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];
    [encoder barrierAfterEncoderStages:MTLStageDispatch
                   beforeEncoderStages:MTLStageDispatch
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder setComputePipelineState:renderer->
                                     transmission_compact_finalize_pipeline];
    [encoder dispatchThreads:MTLSizeMake(1u, 1u, 1u)
        threadsPerThreadgroup:MTLSizeMake(1u, 1u, 1u)];
    break;
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_EXPOSURE_HISTOGRAM:
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
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING_READBACK:
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
    break;
  default:
    vkr_metal_packet_record_dispatch(renderer, encoder, &pass->dispatch);
    break;
  }
}

vkr_internal void vkr_metal_packet_record_indirect_pass(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  const VkrMetalPacketIndirectPass *draw = &pass->indirect;
  if (pass->kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION) {
    [renderer->argument_table setAddress:draw->peel_root atIndex:2u];
    [renderer->argument_table setAddress:draw->draw_root atIndex:0u];
    [renderer->argument_table setAddress:draw->draw_root atIndex:1u];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageVertex | MTLRenderStageFragment];
  }
  [encoder setViewport:draw->viewport];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setDepthStencilState:renderer->depth_write_state];
  if (pass->kind == VKR_METAL_PACKET_GRAPH_EXECUTOR_SHADOW)
    [encoder setDepthBias:draw->depth_bias.depth_bias_constant
               slopeScale:draw->depth_bias.depth_bias_slope
                    clamp:draw->depth_bias.depth_bias_clamp];
  id<MTLRenderPipelineState> bound_pipeline = nil;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    id<MTLRenderPipelineState> pipeline =
        draw->opaque_pipeline &&
                (bucket == VKR_WORLD_DRAW_STATE_OPAQUE_BACK ||
                 bucket == VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED)
            ? draw->opaque_pipeline
            : draw->pipeline;
    if (pipeline != bound_pipeline) {
      [encoder setRenderPipelineState:pipeline];
      bound_pipeline = pipeline;
    }
    [encoder setCullMode:(bucket & 1u) ? MTLCullModeNone : MTLCullModeBack];
    [encoder executeCommandsInBuffer:draw->commands
                      indirectBuffer:draw->arguments +
                                     (uint64_t)bucket * 2u * sizeof(uint32_t)];
  }
}

vkr_internal void vkr_metal_packet_record_graphics_pass(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    const VkrMetalPacketPreparedPass *pass) API_AVAILABLE(macos(26.0)) {
  switch (pass->kind) {
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_SHADOW:
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_OPAQUE:
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION:
    vkr_metal_packet_record_indirect_pass(renderer, encoder, pass);
    break;
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_WORLD_BLEND:
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_PICKING: {
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
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_UI:
    if (pass->ui.count)
      vkr_metal_packet_record_prepared_ui_draws(
          renderer, encoder, pass->ui.root_cpu, pass->ui.root_gpu,
          pass->ui.count, pass->ui.width, pass->ui.height);
    break;
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_TONEMAP:
  case VKR_METAL_PACKET_GRAPH_EXECUTOR_EDITOR:
    [renderer->argument_table setAddress:pass->tonemap.root atIndex:1u];
    [encoder setViewport:pass->tonemap.viewport];
    [encoder setScissorRect:pass->tonemap.scissor];
    [encoder setRenderPipelineState:renderer->tonemap_pipeline];
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

vkr_internal void
vkr_metal_packet_record_metalfx(VkrMetalPacketRenderer *renderer,
                                const VkrMetalPacketPreparedPass *pass)
    API_AVAILABLE(macos(26.0)) {
  id<MTL4FXTemporalScaler> scaler = renderer->metalfx_temporal_scaler;
  scaler.colorTexture = pass->metalfx.color;
  scaler.depthTexture = pass->metalfx.depth;
  scaler.motionTexture = pass->metalfx.motion;
  scaler.outputTexture = pass->metalfx.output;
  scaler.exposureTexture = nil;
  scaler.reactiveMaskTexture = nil;
  scaler.inputContentWidth = pass->metalfx.width;
  scaler.inputContentHeight = pass->metalfx.height;
  scaler.preExposure = 1.0f;
  scaler.jitterOffsetX = pass->metalfx.jitter.x;
  scaler.jitterOffsetY = pass->metalfx.jitter.y;
  scaler.motionVectorScaleX = (float32_t)pass->metalfx.width;
  scaler.motionVectorScaleY = (float32_t)pass->metalfx.height;
  scaler.depthReversed = NO;
  scaler.reset = pass->metalfx.reset;
  [scaler encodeToCommandBuffer:renderer->command_buffer];
}

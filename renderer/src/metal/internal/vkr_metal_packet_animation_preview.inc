/* Preview draws reuse acquired frame upload storage and the graph's independent
 * color/depth attachments. No preview camera or pose reaches the main scene. */
vkr_internal bool8_t vkr_metal_packet_prepare_animation_preview(
    VkrMetalPacketRenderer *renderer, VkrMetalPacketPreparedPass *prepared,
    const VkrPreparedFrame *packet, VkrMetalPacketFrameUpload *upload,
    VkrMetalPacketResult *result) API_AVAILABLE(macos(26.0)) {
  const VkrAnimationPreviewInput *preview = packet->input.animation_preview;
  if (!preview || !preview->draw_count) {
    return true_v;
  }
  if (preview->draw_count > upload->root_capacity - upload->root_cursor) {
    return false_v;
  }
  VkrMetalPacketPreparedDraw *draws = vkr_allocator_alloc(
      &renderer->graph_frame_allocator,
      (uint64_t)preview->draw_count * sizeof(*draws),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!draws) {
    return false_v;
  }
  const uint64_t root_offset =
      (uint64_t)upload->root_cursor * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  const uint64_t root_gpu = upload->root_gpu + root_offset;
  uint8_t *root_cpu = upload->root_cpu + root_offset;
  upload->root_cursor += preview->draw_count;
  for (uint32_t i = 0u; i < preview->draw_count; ++i) {
    const VkrAnimationPreviewDraw *source = &preview->draws[i];
    VkrMetalPacketMesh *mesh =
        vkr_metal_packet_resolve_mesh(renderer,
            (VkrMeshHandle){source->geometry.id, source->geometry.generation});
    if (!mesh || source->submesh_index >= mesh->submesh_count) {
      return false_v;
    }
    const VkrMetalPacketSubmeshCreateInfo *range =
        &mesh->submeshes[source->submesh_index];
    const uint64_t index_offset =
        ((uint64_t)mesh->gpu_row.first_index + range->first_index) *
        sizeof(uint32_t);
    draws[i] = (VkrMetalPacketPreparedDraw){
        .visible = {.index_count = range->index_count,
                    .vertex_offset = range->vertex_offset},
        .index_address = mesh->indices.gpu_address + index_offset,
        .index_length = mesh->indices.length - index_offset,
        .instance_count = 1u,
        .cull_mode = MTLCullModeNone,
        .winding = MTLWindingCounterClockwise,
    };
    VkrMetalPacketAnimationPreviewRoot *root =
        (VkrMetalPacketAnimationPreviewRoot *)(root_cpu +
            (uint64_t)i * VKR_METAL_PACKET_DRAW_ROOT_STRIDE);
    *root = (VkrMetalPacketAnimationPreviewRoot){
        .model = source->model,
        .view_projection = vkr_metal_packet_clip_matrix(
            renderer->convert_vulkan_clip_y, preview->view_projection),
        .vertices = mesh->gpu_row.vertex_address,
        .decode = mesh->gpu_row.decode_address,
        .deformation_address = source->skinning_index
            ? renderer->skinning_addresses[source->skinning_index - 1u] : 0u,
        .tint = {0.72f, 0.76f, 0.82f, 1.0f},
        .first_vertex = mesh->gpu_row.first_vertex,
        .decode_index = range->decode_index,
    };
    mesh->last_use_submit_value = renderer->submit_value + 1u;
  }
  prepared->active = true_v;
  prepared->world.direct = (VkrMetalPacketDirectPass){
      .pipeline = renderer->animation_preview_pipeline,
      .depth = renderer->depth_write_state,
      .draws = draws,
      .count = preview->draw_count,
      .width = 512u,
      .height = 512u,
      .root_gpu = root_gpu,
  };
  result->indexed_draw_count += preview->draw_count;
  return true_v;
}

#include "math/vkr_frustum.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_internal.h"

enum {
  VKR_BINDLESS_VK_DEFERRED_BUCKET_COUNT = VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
  VKR_BINDLESS_VK_DEFERRED_COMMAND_PARTITION_CAPACITY =
      VKR_GPU_DRAW_CANDIDATE_CAPACITY / VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
  VKR_BINDLESS_VK_INDIRECT_COMMAND_SIZE = sizeof(VkDrawIndexedIndirectCommand),
};

_Static_assert(VKR_WORLD_DRAW_STATE_BUCKET_COUNT == 4u,
               "Vulkan deferred shaders require four draw-state buckets");
_Static_assert(VKR_FRUSTUM_PLANE_COUNT == 6u,
               "Vulkan deferred shaders require six frustum planes");
_Static_assert(VKR_BINDLESS_VK_TEXTURE_MIP_MAX == 16u,
               "Vulkan deferred shaders require sixteen HZB mip slots");

vkr_internal VkrBindlessVkGraphBufferInstance *
vkr_bindless_vk_deferred_buffer(VkrBindlessVulkanRenderer *renderer,
                                const VkrRgPass *pass, uint32_t binding) {
  const VkrRgBufferUse *use =
      vkr_rg_pass_find_buffer_use(&pass->desc, binding, 0u);
  return use ? vkr_bindless_vk_graph_buffer(renderer, use->buffer) : NULL;
}

vkr_internal VkrBindlessVkGraphImageInstance *
vkr_bindless_vk_deferred_image(VkrBindlessVulkanRenderer *renderer,
                               VkrRgImageHandle handle) {
  return vkr_bindless_vk_graph_image(renderer, handle,
                                     renderer->prepared_frame.image_index);
}

vkr_internal bool8_t vkr_bindless_vk_deferred_storage_index(
    VkrBindlessVulkanRenderer *renderer, const VkrRgPass *pass,
    uint32_t binding, uint32_t *out_index) {
  const VkrRgImageUse *use =
      vkr_rg_pass_find_image_use(&pass->desc, binding, 0u);
  VkrBindlessVkGraphImageInstance *image =
      use ? vkr_bindless_vk_deferred_image(renderer, use->image) : NULL;
  if (!image || !out_index)
    return false_v;
  if (use->has_slice && use->slice.mip_count <= 1u) {
    const uint32_t mip = use->slice.mip_level;
    if (mip >= image->image.mip_levels || !image->has_storage_mip_slot[mip])
      return false_v;
    *out_index = image->storage_mip_slots[mip].index;
    return true_v;
  }
  if (!image->has_storage_slot)
    return false_v;
  *out_index = image->storage_slot.index;
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_deferred_sampled_index(
    VkrBindlessVulkanRenderer *renderer, const VkrRgPass *pass,
    uint32_t binding, uint32_t *out_index) {
  const VkrRgImageUse *use =
      vkr_rg_pass_find_image_use(&pass->desc, binding, 0u);
  VkrBindlessVkGraphImageInstance *image =
      use ? vkr_bindless_vk_deferred_image(renderer, use->image) : NULL;
  if (!image || !out_index)
    return false_v;
  if (use->has_slice && use->slice.mip_count <= 1u) {
    const uint32_t mip = use->slice.mip_level;
    if (mip >= image->image.mip_levels || !image->has_sampled_mip_slot[mip])
      return false_v;
    *out_index = image->sampled_mip_slots[mip].index;
    return true_v;
  }
  if (!image->has_sampled_slot)
    return false_v;
  *out_index = image->sampled_slot.index;
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_deferred_push_root(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const void *root, uint64_t size, uint64_t alignment,
    uint64_t *out_address) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  void *upload = vkr_bindless_vk_frame_upload_allocate(slot, size, alignment,
                                                       out_address, NULL);
  if (!upload)
    return false_v;
  MemCopy(upload, root, size);
  const VkrBindlessVkPushConstants push = {.root = *out_address};
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  return true_v;
}

bool8_t vkr_bindless_vk_record_deferred_upload(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, bool8_t transmission) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrBindlessVkGraphBufferInstance *candidates =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 0u);
  VkrBindlessVkGraphBufferInstance *state =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 1u);
  const uint32_t count = transmission ? slot->transmission_gpu_candidate_count
                                      : slot->gpu_candidate_count;
  const uint64_t source_offset =
      transmission ? slot->transmission_gpu_candidate_upload_offset
                   : slot->gpu_candidate_upload_offset;
  if (!candidates || !state)
    return false_v;
  if (transmission)
    slot->transmission_gpu_compaction_state = state;
  else
    slot->gpu_compaction_state = state;
  if (count) {
    const VkBufferCopy copy = {
        .srcOffset = source_offset,
        .size = (uint64_t)count * sizeof(VkrGpuCandidateDrawRow),
    };
    vkCmdCopyBuffer(command, slot->frame_upload.handle,
                    candidates->buffer.handle, 1u, &copy);
  }
  vkCmdFillBuffer(command, state->buffer.handle, 0u, VK_WHOLE_SIZE, 0u);
  return true_v;
}

bool8_t
vkr_bindless_vk_record_deferred_readback(VkrBindlessVulkanRenderer *renderer,
                                         VkCommandBuffer command) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrBindlessVkGraphBufferInstance *opaque = slot->gpu_compaction_state;
  VkrBindlessVkGraphBufferInstance *transmission =
      renderer->prepared_frame.transmission_pending
          ? slot->transmission_gpu_compaction_state
          : NULL;
  if (!opaque ||
      (renderer->prepared_frame.transmission_pending && !transmission))
    return false_v;
  VkBufferMemoryBarrier2 barriers[2] = {0};
  uint32_t barrier_count = 0u;
  VkrBindlessVkGraphBufferInstance *sources[] = {opaque, transmission};
  for (uint32_t i = 0u; i < ArrayCount(sources); ++i) {
    if (!sources[i])
      continue;
    barriers[barrier_count++] = (VkBufferMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = sources[i]->buffer.handle,
        .size = VK_WHOLE_SIZE,
    };
  }
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = barrier_count,
      .pBufferMemoryBarriers = barriers,
  };
  vkCmdPipelineBarrier2(command, &dependency);
  vkCmdFillBuffer(command, slot->readback.handle,
                  VKR_BINDLESS_VK_READBACK_DRAW_STATE_OFFSET,
                  VKR_BINDLESS_VK_READBACK_SIZE -
                      VKR_BINDLESS_VK_READBACK_DRAW_STATE_OFFSET,
                  0u);
  const VkBufferMemoryBarrier2 clear_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->readback.handle,
      .offset = VKR_BINDLESS_VK_READBACK_DRAW_STATE_OFFSET,
      .size = VKR_BINDLESS_VK_READBACK_SIZE -
              VKR_BINDLESS_VK_READBACK_DRAW_STATE_OFFSET,
  };
  const VkDependencyInfo clear_dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &clear_barrier,
  };
  vkCmdPipelineBarrier2(command, &clear_dependency);
  const VkBufferCopy opaque_copy = {
      .dstOffset = VKR_BINDLESS_VK_READBACK_DRAW_STATE_OFFSET,
      .size = (1u + renderer->prepared_frame.shadow_cascade_count) *
              sizeof(VkrGpuDrawCompactionState),
  };
  vkCmdCopyBuffer(command, opaque->buffer.handle, slot->readback.handle, 1u,
                  &opaque_copy);
  if (transmission) {
    const VkBufferCopy transmission_copy = {
        .dstOffset = VKR_BINDLESS_VK_READBACK_TRANSMISSION_STATE_OFFSET,
        .size = sizeof(VkrGpuTransmissionDiagnostics),
    };
    vkCmdCopyBuffer(command, transmission->buffer.handle, slot->readback.handle,
                    1u, &transmission_copy);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_deferred_cull_root(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, VkrBindlessVkDeferredPipeline pipeline,
    bool8_t transmission, VkrBindlessVkCullRoot *out_root,
    uint64_t *out_root_address) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrBindlessVkGraphBufferInstance *candidates = NULL;
  VkrBindlessVkGraphBufferInstance *classifications = NULL;
  VkrBindlessVkGraphBufferInstance *visible = NULL;
  VkrBindlessVkGraphBufferInstance *states = NULL;
  VkrBindlessVkGraphBufferInstance *commands = NULL;
  switch (pipeline) {
  case VKR_BINDLESS_VK_DEFERRED_PIPELINE_CLASSIFY:
    candidates = vkr_bindless_vk_deferred_buffer(renderer, pass, 0u);
    classifications = vkr_bindless_vk_deferred_buffer(renderer, pass, 1u);
    states = vkr_bindless_vk_deferred_buffer(renderer, pass, 2u);
    break;
  case VKR_BINDLESS_VK_DEFERRED_PIPELINE_PREFIX:
    states = vkr_bindless_vk_deferred_buffer(renderer, pass, 0u);
    break;
  case VKR_BINDLESS_VK_DEFERRED_PIPELINE_ENCODE:
    candidates = vkr_bindless_vk_deferred_buffer(renderer, pass, 0u);
    classifications = vkr_bindless_vk_deferred_buffer(renderer, pass, 1u);
    visible = vkr_bindless_vk_deferred_buffer(renderer, pass, 2u);
    states = vkr_bindless_vk_deferred_buffer(renderer, pass, 3u);
    commands = vkr_bindless_vk_deferred_buffer(renderer, pass, 4u);
    break;
  default:
    return false_v;
  }
  if (!states ||
      (pipeline == VKR_BINDLESS_VK_DEFERRED_PIPELINE_CLASSIFY &&
       (!candidates || !classifications)) ||
      (pipeline == VKR_BINDLESS_VK_DEFERRED_PIPELINE_ENCODE &&
       (!candidates || !classifications || !visible || !commands)))
    return false_v;
  const uint32_t view_count =
      transmission ? 1u : 1u + renderer->prepared_frame.shadow_cascade_count;
  uint64_t views_address = 0u;
  uint64_t planes_address = 0u;
  Mat4 *views = NULL;
  if (pipeline == VKR_BINDLESS_VK_DEFERRED_PIPELINE_CLASSIFY) {
    const VkrRenderPacket *packet = renderer->graph->packet;
    views = vkr_bindless_vk_frame_upload_allocate(
        slot, (uint64_t)view_count * sizeof(*views), _Alignof(Mat4),
        &views_address, NULL);
    Vec4 *planes = vkr_bindless_vk_frame_upload_allocate(
        slot, (uint64_t)view_count * VKR_FRUSTUM_PLANE_COUNT * sizeof(*planes),
        _Alignof(Vec4), &planes_address, NULL);
    if (!views || !planes)
      return false_v;
    views[0] = mat4_mul(packet->globals.projection, packet->globals.view);
    const VkrFrustum camera = vkr_frustum_from_view_projection(
        packet->globals.view, packet->globals.projection);
    for (uint32_t plane = 0u; plane < VKR_FRUSTUM_PLANE_COUNT; ++plane) {
      const VkrPlane *source = &camera.planes[plane];
      planes[plane] = (Vec4){source->normal.x, source->normal.y,
                             source->normal.z, source->d};
    }
    for (uint32_t i = 1u; i < view_count; ++i) {
      views[i] = packet->shadow->light_view_proj[i - 1u];
      const VkrFrustum shadow = vkr_frustum_from_matrix(views[i]);
      for (uint32_t plane = 0u; plane < VKR_FRUSTUM_PLANE_COUNT; ++plane) {
        const VkrPlane *source = &shadow.planes[plane];
        planes[i * VKR_FRUSTUM_PLANE_COUNT + plane] = (Vec4){
            source->normal.x, source->normal.y, source->normal.z, source->d};
      }
    }
  }
  *out_root = (VkrBindlessVkCullRoot){
      .candidates = candidates ? candidates->buffer.address : 0u,
      .classifications = classifications ? classifications->buffer.address : 0u,
      .visible = visible ? visible->buffer.address : 0u,
      .states = states->buffer.address,
      .commands = commands ? commands->buffer.address : 0u,
      .instances = transmission ? slot->transmission_gpu_candidate_instances
                                : slot->gpu_candidate_instances,
      .view_projections = views_address,
      .frustum_planes = planes_address,
      .candidate_count = transmission ? slot->transmission_gpu_candidate_count
                                      : slot->gpu_candidate_count,
      .view_count = view_count,
      .candidate_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .command_partition_capacity =
          VKR_BINDLESS_VK_DEFERRED_COMMAND_PARTITION_CAPACITY,
      .hzb_depth_epsilon = 1e-3f,
      .camera_required_flags =
          transmission ? 0u : VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE,
      .shadow_required_flags = VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER,
  };
  for (uint32_t mip = 0u; mip < VKR_BINDLESS_VK_TEXTURE_MIP_MAX; ++mip)
    out_root->hzb_textures[mip] = UINT32_MAX;
  if (pipeline == VKR_BINDLESS_VK_DEFERRED_PIPELINE_CLASSIFY && !transmission &&
      renderer->config.hzb_enabled) {
    const VkrRgImageUse *hzb_use =
        vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u);
    if (hzb_use && vkr_rg_image_handle_valid(hzb_use->image)) {
      VkrBindlessVkGraphImage *hzb =
          &renderer->graph_images[hzb_use->image.id - 1u];
      const Mat4 current_view_projection = views[0];
      VkrBindlessVkGraphImageInstance *selected = NULL;
      for (uint32_t i = 0u; i < hzb->instance_count; ++i) {
        VkrBindlessVkGraphImageInstance *candidate = &hzb->instances[i];
        if (i == renderer->active_frame_slot || !candidate->history_valid ||
            candidate->history_producer_submit_value >
                renderer->completed_value ||
            candidate->history_world_epoch != slot->gpu_world_epoch ||
            candidate->history_width !=
                renderer->prepared_frame.viewport_width ||
            candidate->history_height !=
                renderer->prepared_frame.viewport_height ||
            MemCompare(&candidate->history_view_projection,
                       &current_view_projection, sizeof(Mat4)) != 0)
          continue;
        if (!selected || candidate->history_producer_submit_value >
                             selected->history_producer_submit_value)
          selected = candidate;
      }
      if (selected) {
        for (uint32_t mip = 0u; mip < selected->image.mip_levels; ++mip) {
          if (!selected->has_storage_mip_slot[mip]) {
            selected = NULL;
            break;
          }
        }
      }
      if (selected) {
        const VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = selected->image.handle,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = selected->image.mip_levels,
                    .layerCount = 1u,
                },
        };
        const VkDependencyInfo dependency = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1u,
            .pImageMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(command, &dependency);
        out_root->hzb_mip_count = selected->image.mip_levels;
        for (uint32_t mip = 0u; mip < selected->image.mip_levels; ++mip)
          out_root->hzb_textures[mip] = selected->storage_mip_slots[mip].index;
        out_root->hzb_extent[0] = selected->image.width;
        out_root->hzb_extent[1] = selected->image.height;
        out_root->hzb_enabled = 1u;
        slot->hzb_history_valid = true_v;
        slot->hzb_history_input = selected;
      }
    }
  }
  return vkr_bindless_vk_deferred_push_root(
      renderer, command, out_root, sizeof(*out_root),
      _Alignof(VkrBindlessVkCullRoot), out_root_address);
}

void vkr_bindless_vk_mark_hzb_submitted(VkrBindlessVulkanRenderer *renderer,
                                        uint64_t submit_value) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (slot->hzb_history_input)
    slot->hzb_history_input->last_use_submit_value = submit_value;
  VkrBindlessVkGraphImageInstance *instance = slot->hzb_history_output;
  if (!instance)
    return;
  const VkrRenderPacket *packet = renderer->graph->packet;
  instance->history_producer_submit_value = submit_value;
  instance->history_world_epoch = slot->gpu_world_epoch;
  instance->history_view_projection =
      mat4_mul(packet->globals.projection, packet->globals.view);
  instance->history_width = renderer->prepared_frame.viewport_width;
  instance->history_height = renderer->prepared_frame.viewport_height;
  instance->history_valid = true_v;
}

bool8_t vkr_bindless_vk_record_deferred_cull(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, VkrBindlessVkDeferredPipeline pipeline,
    bool8_t transmission) {
  VkrBindlessVkCullRoot root = {0};
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_cull_root(renderer, command, pass, pipeline,
                                          transmission, &root, &root_address))
    return false_v;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->deferred_pipelines[pipeline]);
  if (pipeline == VKR_BINDLESS_VK_DEFERRED_PIPELINE_PREFIX) {
    vkCmdDispatch(command, root.view_count, 1u, 1u);
  } else {
    const uint64_t work = (uint64_t)root.candidate_count * root.view_count;
    if (work)
      vkCmdDispatch(command, (uint32_t)((work + 63u) / 64u), 1u, 1u);
  }
  return true_v;
}

bool8_t vkr_bindless_vk_record_deferred_raster(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, bool8_t shadow, bool8_t transmission) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrBindlessVkGraphBufferInstance *visible =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 1u);
  VkrBindlessVkGraphBufferInstance *states =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 2u);
  VkrBindlessVkGraphBufferInstance *commands =
      vkr_bindless_vk_deferred_buffer(renderer, pass, transmission ? 4u : 3u);
  if (!visible || !states || !commands)
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const uint32_t view_index =
      shadow ? 1u + pass->desc.depth_attachment.desc.slice.base_layer : 0u;
  const Mat4 view_projection =
      shadow ? packet->shadow->light_view_proj[view_index - 1u]
             : mat4_mul(packet->globals.projection, packet->globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint64_t frame_address = 0u;
  VkrBindlessVkPacketFrameRoot *frame_root =
      vkr_bindless_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  vkr_bindless_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame,
      transmission ? slot->transmission_gpu_candidate_instances
                   : slot->gpu_candidate_instances,
      view_projection, VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX,
      VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX, false_v, transmission);
  VkrBindlessVkRasterRoot root = {
      .geometry_rows = slot->gpu_geometry_rows,
      .visible_rows = visible->buffer.address,
      .states = states->buffer.address,
      .frame = frame_address,
      .view_index = view_index,
      .visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .previous_depth_texture = UINT32_MAX,
  };
  if (transmission) {
    const VkrRgImageUse *previous_depth =
        vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u);
    if (previous_depth) {
      if (!vkr_bindless_vk_deferred_sampled_index(renderer, pass, 3u,
                                                  &root.previous_depth_texture))
        return false_v;
      root.previous_depth_layer =
          previous_depth->has_slice ? previous_depth->slice.base_layer : 0u;
    }
  }
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_push_root(
          renderer, command, &root, sizeof(root),
          _Alignof(VkrBindlessVkRasterRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_GRAPHICS,
      renderer->packet_pipelines
          [shadow ? VKR_BINDLESS_VK_PACKET_PIPELINE_VISIBILITY_SHADOW
                  : VKR_BINDLESS_VK_PACKET_PIPELINE_VISIBILITY]);
  vkCmdBindIndexBuffer(command, renderer->geometry_megabuffer.indices.handle,
                       0u, VK_INDEX_TYPE_UINT32);
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    const VkrBindlessVkPushConstants push = {
        .root = root_address,
        .material_index = bucket,
    };
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdSetCullMode(command, bucket == VKR_WORLD_DRAW_STATE_OPAQUE_BACK ||
                                      bucket == VKR_WORLD_DRAW_STATE_CUTOUT_BACK
                                  ? VK_CULL_MODE_BACK_BIT
                                  : VK_CULL_MODE_NONE);
    const VkDeviceSize argument_offset =
        ((VkDeviceSize)view_index * VKR_WORLD_DRAW_STATE_BUCKET_COUNT +
         bucket) *
        VKR_BINDLESS_VK_DEFERRED_COMMAND_PARTITION_CAPACITY *
        sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize count_offset =
        (VkDeviceSize)view_index * sizeof(VkrGpuDrawCompactionState) +
        offsetof(VkrGpuDrawCompactionState, bucket_counts) +
        bucket * sizeof(uint32_t);
    vkCmdDrawIndexedIndirectCount(
        command, commands->buffer.handle, argument_offset,
        states->buffer.handle, count_offset,
        VKR_BINDLESS_VK_DEFERRED_COMMAND_PARTITION_CAPACITY,
        sizeof(VkDrawIndexedIndirectCommand));
  }
  return true_v;
}

bool8_t
vkr_bindless_vk_record_deferred_gbuffer(VkrBindlessVulkanRenderer *renderer,
                                        VkCommandBuffer command,
                                        const VkrRgPass *pass) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrBindlessVkGraphBufferInstance *visible =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 1u);
  VkrBindlessVkGraphBufferInstance *state =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 7u);
  uint32_t indices[7] = {0};
  if (!visible || !state)
    return false_v;
  const uint32_t bindings[] = {0u, 2u, 3u, 4u, 5u, 6u, 8u};
  for (uint32_t i = 0u; i < ArrayCount(bindings); ++i)
    if (!vkr_bindless_vk_deferred_storage_index(renderer, pass, bindings[i],
                                                &indices[i]))
      return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const VkrBindlessVkResolveRoot root = {
      .geometry_rows = slot->gpu_geometry_rows,
      .visible_rows = visible->buffer.address,
      .vertices = renderer->geometry_megabuffer.vertices.address,
      .instances = slot->gpu_candidate_instances,
      .materials = renderer->materials.address,
      .indices = renderer->geometry_megabuffer.indices.address,
      .compaction_state = state->buffer.address,
      .view_projection =
          mat4_mul(packet->globals.projection, packet->globals.view),
      .vbuffer_texture = indices[0],
      .albedo_texture = indices[1],
      .specular_texture = indices[2],
      .normal_texture = indices[3],
      .emissive_texture = indices[4],
      .debug_texture = indices[5],
      .scene_texture = indices[6],
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .geometry_count = renderer->config.geometry_capacity,
      .material_count = renderer->config.material_slot_capacity,
      .instance_count = slot->gpu_candidate_count,
      .render_mode = packet->globals.render_mode,
  };
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_push_root(
          renderer, command, &root, sizeof(root),
          _Alignof(VkrBindlessVkResolveRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_BINDLESS_VK_DEFERRED_PIPELINE_GBUFFER]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

bool8_t
vkr_bindless_vk_record_deferred_lighting(VkrBindlessVulkanRenderer *renderer,
                                         VkCommandBuffer command,
                                         const VkrRgPass *pass) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint32_t vbuffer = 0u, depth = 0u, albedo = 0u, specular = 0u, normal = 0u,
           scene = 0u;
  if (!vkr_bindless_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_bindless_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 2u, &albedo) ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 3u, &specular) ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 4u, &normal) ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 6u, &scene))
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const Mat4 view_projection =
      mat4_mul(packet->globals.projection, packet->globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint32_t shadow_texture = VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.shadow_cascade_count > 0u &&
      !vkr_bindless_vk_deferred_sampled_index(renderer, pass, 5u,
                                              &shadow_texture))
    return false_v;
  /* A cubemap still publishing its initial upload is not sampled this frame;
     the fallback colour matches the authored forward skybox clear. */
  uint32_t sky_texture = VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX;
  uint32_t sky_sampler = VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX;
  uint32_t sky_enabled = 0u;
  VkrBindlessVkPublishedTexture *sky =
      packet->skybox ? vkr_bindless_vk_published_texture(
                           renderer, packet->skybox->cubemap, NULL)
                     : NULL;
  if (packet->skybox && !sky)
    return false_v;
  if (sky && !sky->initialization_pending && sky->image.array_layers == 6u &&
      sky->sampler_record_index < renderer->config.sampler_capacity &&
      renderer->published_samplers[sky->sampler_record_index].live) {
    sky_texture = sky->sampled_slot.index;
    sky_sampler =
        renderer->published_samplers[sky->sampler_record_index].slot.index;
    sky_enabled = 1u;
    sky->last_use_submit_value = renderer->submit_value + 1u;
  }
  uint64_t frame_address = 0u;
  VkrBindlessVkPacketFrameRoot *frame_root =
      vkr_bindless_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  vkr_bindless_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, slot->gpu_candidate_instances,
      view_projection, shadow_texture, VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX,
      true_v, false_v);
  const VkrBindlessVkLightingRoot root = {
      .frame = frame_address,
      .inverse_view_projection = mat4_inverse(view_projection),
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .albedo_texture = albedo,
      .specular_texture = specular,
      .normal_texture = normal,
      .scene_texture = scene,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .sky_texture = sky_texture,
      .sky_sampler = sky_sampler,
      .sky_enabled = sky_enabled,
  };
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_push_root(
          renderer, command, &root, sizeof(root),
          _Alignof(VkrBindlessVkLightingRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_BINDLESS_VK_DEFERRED_PIPELINE_LIGHTING]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

bool8_t vkr_bindless_vk_record_deferred_hzb(VkrBindlessVulkanRenderer *renderer,
                                            VkCommandBuffer command,
                                            const VkrRgPass *pass) {
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *write = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  VkrBindlessVkGraphImageInstance *source =
      read ? vkr_bindless_vk_deferred_image(renderer, read->image) : NULL;
  VkrBindlessVkGraphImageInstance *destination =
      write ? vkr_bindless_vk_deferred_image(renderer, write->image) : NULL;
  uint32_t source_index = 0u, destination_index = 0u;
  const bool8_t source_is_depth =
      read && (read->access & VKR_RG_IMAGE_ACCESS_SAMPLED);
  if (!source || !destination ||
      !(source_is_depth ? vkr_bindless_vk_deferred_sampled_index(
                              renderer, pass, 0u, &source_index)
                        : vkr_bindless_vk_deferred_storage_index(
                              renderer, pass, 0u, &source_index)) ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 1u,
                                              &destination_index))
    return false_v;
  renderer->frame_slots[renderer->active_frame_slot].hzb_history_output =
      destination;
  const uint32_t source_mip = read->has_slice ? read->slice.mip_level : 0u;
  const uint32_t destination_mip =
      write->has_slice ? write->slice.mip_level : 0u;
  const VkrBindlessVkHzbRoot root = {
      .source_texture = source_index,
      .destination_texture = destination_index,
      .source_extent = {Max(1u, source->image.width >> source_mip),
                        Max(1u, source->image.height >> source_mip)},
      .destination_extent = {Max(1u,
                                 destination->image.width >> destination_mip),
                             Max(1u,
                                 destination->image.height >> destination_mip)},
      .source_is_depth = source_is_depth ? 2u : 0u,
  };
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_push_root(
          renderer, command, &root, sizeof(root),
          _Alignof(VkrBindlessVkHzbRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_BINDLESS_VK_DEFERRED_PIPELINE_HZB]);
  vkCmdDispatch(command, (root.destination_extent[0] + 7u) / 8u,
                (root.destination_extent[1] + 7u) / 8u, 1u);
  return true_v;
}

bool8_t
vkr_bindless_vk_record_deferred_picking(VkrBindlessVulkanRenderer *renderer,
                                        VkCommandBuffer command,
                                        const VkrRgPass *pass) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrBindlessVkGraphBufferInstance *opaque =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 2u);
  uint32_t opaque_vbuffer = 0u, transmission_vbuffer = 0u, output = 0u;
  if (!opaque ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 0u,
                                              &opaque_vbuffer) ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 4u, &output))
    return false_v;
  const bool8_t use_transmission =
      vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u) != NULL;
  VkrBindlessVkGraphBufferInstance *transmission =
      use_transmission ? vkr_bindless_vk_deferred_buffer(renderer, pass, 7u)
                       : NULL;
  if (use_transmission &&
      (!transmission || !vkr_bindless_vk_deferred_storage_index(
                            renderer, pass, 5u, &transmission_vbuffer)))
    return false_v;
  const VkrRgImageUse *transmission_use =
      vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u);
  const VkrBindlessVkPickingRoot root = {
      .opaque_visible = opaque->buffer.address,
      .transmission_visible = transmission ? transmission->buffer.address : 0u,
      .opaque_instances = slot->gpu_candidate_instances,
      .transmission_instances = slot->transmission_gpu_candidate_instances,
      .opaque_vbuffer = opaque_vbuffer,
      .transmission_vbuffer = transmission_vbuffer,
      .output_texture = output,
      .pixel = {slot->picking_x, slot->picking_y},
      .transmission_layer = transmission_use && transmission_use->has_slice
                                ? transmission_use->slice.base_layer
                                : 0u,
      .use_transmission = use_transmission,
  };
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_push_root(
          renderer, command, &root, sizeof(root),
          _Alignof(VkrBindlessVkPickingRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_BINDLESS_VK_DEFERRED_PIPELINE_PICKING]);
  vkCmdDispatch(command, 1u, 1u, 1u);
  return true_v;
}

bool8_t vkr_bindless_vk_record_deferred_transmission(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrBindlessVkGraphBufferInstance *visible =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 2u);
  VkrBindlessVkGraphBufferInstance *state =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 3u);
  uint32_t vbuffer = 0u, depth = 0u, feedback = 0u, output = 0u;
  if (!visible || !state ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_bindless_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_bindless_vk_deferred_sampled_index(renderer, pass, 4u, &feedback) ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 5u, &output))
    return false_v;
  uint32_t shadow_texture = VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.shadow_cascade_count > 0u &&
      !vkr_bindless_vk_deferred_sampled_index(renderer, pass, 6u,
                                              &shadow_texture))
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const Mat4 view_projection =
      mat4_mul(packet->globals.projection, packet->globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint64_t frame_address = 0u;
  VkrBindlessVkPacketFrameRoot *frame_root =
      vkr_bindless_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  vkr_bindless_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame,
      slot->transmission_gpu_candidate_instances, view_projection,
      shadow_texture, VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX, true_v, true_v);
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrBindlessVkTransmissionRoot root = {
      .visible_rows = visible->buffer.address,
      .materials = renderer->materials.address,
      .geometry_rows = slot->gpu_geometry_rows,
      .instances = slot->transmission_gpu_candidate_instances,
      .vertices = renderer->geometry_megabuffer.vertices.address,
      .indices = renderer->geometry_megabuffer.indices.address,
      .compaction_state = state->buffer.address,
      .frame = frame_address,
      .view_projection = view_projection,
      .inverse_view_projection = mat4_inverse(view_projection),
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .feedback_texture = feedback,
      .feedback_sampler = VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX,
      .output_texture = output,
      .layer = vbuffer_use && vbuffer_use->has_slice
                   ? vbuffer_use->slice.base_layer
                   : 0u,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .geometry_count = renderer->config.geometry_capacity,
      .material_count = renderer->config.material_slot_capacity,
      .instance_count = slot->transmission_gpu_candidate_count,
  };
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_push_root(
          renderer, command, &root, sizeof(root),
          _Alignof(VkrBindlessVkTransmissionRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer
          ->deferred_pipelines[VKR_BINDLESS_VK_DEFERRED_PIPELINE_TRANSMISSION]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

bool8_t vkr_bindless_vk_record_deferred_transmission_coverage(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass) {
  VkrBindlessVkGraphBufferInstance *state =
      vkr_bindless_vk_deferred_buffer(renderer, pass, 1u);
  uint32_t vbuffer = 0u;
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  if (!state || !vbuffer_use ||
      !vkr_bindless_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer))
    return false_v;
  const VkrBindlessVkTransmissionCoverageRoot root = {
      .covered_pixels = state->buffer.address +
                        offsetof(VkrGpuTransmissionDiagnostics, covered_pixels),
      .vbuffer_texture = vbuffer,
      .layer = vbuffer_use->has_slice ? vbuffer_use->slice.base_layer : 0u,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
  };
  uint64_t root_address = 0u;
  if (!vkr_bindless_vk_deferred_push_root(
          renderer, command, &root, sizeof(root),
          _Alignof(VkrBindlessVkTransmissionCoverageRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines
          [VKR_BINDLESS_VK_DEFERRED_PIPELINE_TRANSMISSION_COVERAGE]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

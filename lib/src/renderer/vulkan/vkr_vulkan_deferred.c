#include "math/vkr_frustum.h"
#include "renderer/vulkan/vkr_vulkan_internal.h"

enum {
  VKR_VULKAN_DEFERRED_BUCKET_COUNT = VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
  VKR_VULKAN_DEFERRED_COMMAND_PARTITION_CAPACITY =
      VKR_GPU_DRAW_CANDIDATE_CAPACITY / VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
  VKR_VULKAN_INDIRECT_COMMAND_SIZE = sizeof(VkDrawIndexedIndirectCommand),
};

_Static_assert(VKR_WORLD_DRAW_STATE_BUCKET_COUNT == 4u,
               "Vulkan deferred shaders require four draw-state buckets");
_Static_assert(VKR_FRUSTUM_PLANE_COUNT == 6u,
               "Vulkan deferred shaders require six frustum planes");
_Static_assert(VKR_VULKAN_TEXTURE_MIP_MAX == 16u,
               "Vulkan deferred shaders require sixteen HZB mip slots");

vkr_internal VkrVulkanGraphBufferInstance *
vkr_vk_deferred_buffer(VkrVulkanRenderer *renderer, const VkrRgPass *pass,
                       uint32_t binding) {
  const VkrRgBufferUse *use =
      vkr_rg_pass_find_buffer_use(&pass->desc, binding, 0u);
  return use ? vkr_vk_graph_buffer(renderer, use->buffer) : NULL;
}

vkr_internal VkrVulkanGraphImageInstance *
vkr_vk_deferred_image(VkrVulkanRenderer *renderer, VkrRgImageHandle handle) {
  return vkr_vk_graph_image(renderer, handle,
                            renderer->prepared_frame.image_index);
}

vkr_internal VkrVulkanGraphImage *
vkr_vk_temporal_graph_image(VkrVulkanRenderer *renderer, const char *name) {
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (image && image->declared_this_frame &&
        vkr_string8_equals_cstr(&image->name, name))
      return &renderer->graph_images[i];
  }
  return NULL;
}

vkr_internal bool8_t vkr_vk_deferred_storage_index(VkrVulkanRenderer *renderer,
                                                   const VkrRgPass *pass,
                                                   uint32_t binding,
                                                   uint32_t *out_index) {
  const VkrRgImageUse *use =
      vkr_rg_pass_find_image_use(&pass->desc, binding, 0u);
  VkrVulkanGraphImageInstance *image =
      use ? vkr_vk_deferred_image(renderer, use->image) : NULL;
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

vkr_internal bool8_t vkr_vk_deferred_sampled_index(VkrVulkanRenderer *renderer,
                                                   const VkrRgPass *pass,
                                                   uint32_t binding,
                                                   uint32_t *out_index) {
  const VkrRgImageUse *use =
      vkr_rg_pass_find_image_use(&pass->desc, binding, 0u);
  VkrVulkanGraphImageInstance *image =
      use ? vkr_vk_deferred_image(renderer, use->image) : NULL;
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

vkr_internal bool8_t vkr_vk_deferred_push_root(VkrVulkanRenderer *renderer,
                                               VkCommandBuffer command,
                                               const void *root, uint64_t size,
                                               uint64_t alignment,
                                               uint64_t *out_address) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  void *upload =
      vkr_vk_frame_upload_allocate(slot, size, alignment, out_address, NULL);
  if (!upload)
    return false_v;
  MemCopy(upload, root, size);
  const VkrVulkanPushConstants push = {.root = *out_address};
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  return true_v;
}

bool8_t vkr_vk_record_deferred_upload(VkrVulkanRenderer *renderer,
                                      VkCommandBuffer command,
                                      const VkrRgPass *pass,
                                      bool8_t transmission) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *candidates =
      vkr_vk_deferred_buffer(renderer, pass, 0u);
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  VkrVulkanGraphBufferInstance *instances =
      vkr_vk_deferred_buffer(renderer, pass, transmission ? 2u : 3u);
  VkrVulkanGraphBufferInstance *sdsm =
      !transmission && slot->sdsm_requested
          ? vkr_vk_deferred_buffer(renderer, pass, 2u)
          : NULL;
  if (!candidates || !instances || !state ||
      (!transmission && slot->sdsm_requested && !sdsm))
    return false_v;
  if (transmission) {
    slot->transmission_gpu_compaction_state = state;
    slot->transmission_gpu_candidate_instances = instances->buffer.address;
    const uint32_t count = slot->transmission_gpu_candidate_count;
    if (count) {
      const VkBufferCopy candidate_copy = {
          .srcOffset = slot->transmission_gpu_candidate_upload_offset,
          .size = (uint64_t)count * sizeof(VkrGpuCandidateDrawRow),
      };
      const VkBufferCopy instance_copy = {
          .srcOffset = slot->transmission_gpu_instance_upload_offset,
          .size = (uint64_t)count * sizeof(VkrInstanceDataGPU),
      };
      vkCmdCopyBuffer(command, slot->frame_upload.handle,
                      candidates->buffer.handle, 1u, &candidate_copy);
      vkCmdCopyBuffer(command, slot->frame_upload.handle,
                      instances->buffer.handle, 1u, &instance_copy);
    }
  } else {
    if (slot->gpu_candidate_buffer != candidates ||
        slot->gpu_candidate_instance_buffer != instances)
      return false_v;
    slot->gpu_compaction_state = state;
    slot->sdsm_reduce_state = sdsm;
    slot->gpu_candidate_instances = instances->buffer.address;
    for (uint32_t i = 0u; i < slot->gpu_candidate_copy_count; ++i) {
      const VkrVulkanCandidateCopyRange *range = &slot->gpu_candidate_copies[i];
      const VkBufferCopy candidate_copy = {
          .srcOffset = range->candidate_source_offset,
          .dstOffset = (uint64_t)range->destination_first *
                       sizeof(VkrGpuCandidateDrawRow),
          .size = (uint64_t)range->count * sizeof(VkrGpuCandidateDrawRow),
      };
      const VkBufferCopy instance_copy = {
          .srcOffset = range->instance_source_offset,
          .dstOffset =
              (uint64_t)range->destination_first * sizeof(VkrInstanceDataGPU),
          .size = (uint64_t)range->count * sizeof(VkrInstanceDataGPU),
      };
      vkCmdCopyBuffer(command, slot->frame_upload.handle,
                      candidates->buffer.handle, 1u, &candidate_copy);
      vkCmdCopyBuffer(command, slot->frame_upload.handle,
                      instances->buffer.handle, 1u, &instance_copy);
    }
  }
  vkCmdFillBuffer(command, state->buffer.handle, 0u, VK_WHOLE_SIZE, 0u);
  if (sdsm) {
    vkCmdFillBuffer(command, sdsm->buffer.handle, 0u, sizeof(uint32_t),
                    UINT32_MAX);
    vkCmdFillBuffer(command, sdsm->buffer.handle, sizeof(uint32_t),
                    VKR_VULKAN_SDSM_STATE_SIZE - sizeof(uint32_t), 0u);
  }
  return true_v;
}

bool8_t vkr_vk_record_deferred_readback(VkrVulkanRenderer *renderer,
                                        VkCommandBuffer command) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *opaque = slot->gpu_compaction_state;
  VkrVulkanGraphBufferInstance *transmission =
      renderer->prepared_frame.transmission_pending
          ? slot->transmission_gpu_compaction_state
          : NULL;
  if (!opaque ||
      (renderer->prepared_frame.transmission_pending && !transmission))
    return false_v;
  VkBufferMemoryBarrier2 barriers[5] = {0};
  uint32_t barrier_count = 0u;
  VkrVulkanGraphBufferInstance *sources[] = {
      opaque,
      transmission,
      slot->sdsm_requested ? slot->sdsm_reduce_state : NULL,
      slot->exposure_requested ? slot->exposure_histogram : NULL,
      slot->exposure_requested ? slot->exposure_state_output : NULL,
  };
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
  vkCmdFillBuffer(
      command, slot->readback.handle, VKR_VULKAN_READBACK_DRAW_STATE_OFFSET,
      VKR_VULKAN_READBACK_SIZE - VKR_VULKAN_READBACK_DRAW_STATE_OFFSET, 0u);
  const VkBufferMemoryBarrier2 clear_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->readback.handle,
      .offset = VKR_VULKAN_READBACK_DRAW_STATE_OFFSET,
      .size = VKR_VULKAN_READBACK_SIZE - VKR_VULKAN_READBACK_DRAW_STATE_OFFSET,
  };
  const VkDependencyInfo clear_dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &clear_barrier,
  };
  vkCmdPipelineBarrier2(command, &clear_dependency);
  const VkBufferCopy opaque_copy = {
      .dstOffset = VKR_VULKAN_READBACK_DRAW_STATE_OFFSET,
      .size = (1u + renderer->prepared_frame.shadow_cascade_count) *
              sizeof(VkrGpuDrawCompactionState),
  };
  vkCmdCopyBuffer(command, opaque->buffer.handle, slot->readback.handle, 1u,
                  &opaque_copy);
  if (transmission) {
    const VkBufferCopy transmission_copy = {
        .dstOffset = VKR_VULKAN_READBACK_TRANSMISSION_STATE_OFFSET,
        .size = sizeof(VkrGpuTransmissionDiagnostics),
    };
    vkCmdCopyBuffer(command, transmission->buffer.handle, slot->readback.handle,
                    1u, &transmission_copy);
  }
  if (slot->sdsm_requested) {
    if (!slot->sdsm_reduce_state)
      return false_v;
    const VkBufferCopy sdsm_copy = {
        .dstOffset = VKR_VULKAN_READBACK_SDSM_STATE_OFFSET,
        .size = VKR_VULKAN_SDSM_STATE_SIZE,
    };
    vkCmdCopyBuffer(command, slot->sdsm_reduce_state->buffer.handle,
                    slot->readback.handle, 1u, &sdsm_copy);
  }
  if (slot->exposure_requested) {
    if (!slot->exposure_histogram || !slot->exposure_state_output)
      return false_v;
    const VkBufferCopy exposure_state_copy = {
        .dstOffset = VKR_VULKAN_READBACK_EXPOSURE_STATE_OFFSET,
        .size = sizeof(VkrExposureGpuState),
    };
    vkCmdCopyBuffer(command, slot->exposure_state_output->buffer.handle,
                    slot->readback.handle, 1u, &exposure_state_copy);
    const VkBufferCopy exposure_histogram_copy = {
        .dstOffset = VKR_VULKAN_READBACK_EXPOSURE_HISTOGRAM_OFFSET,
        .size = sizeof(VkrExposureGpuHistogram),
    };
    vkCmdCopyBuffer(command, slot->exposure_histogram->buffer.handle,
                    slot->readback.handle, 1u, &exposure_histogram_copy);
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_deferred_cull_root(
    VkrVulkanRenderer *renderer, VkCommandBuffer command, const VkrRgPass *pass,
    VkrVulkanDeferredPipeline pipeline, bool8_t transmission,
    VkrVulkanCullRoot *out_root, uint64_t *out_root_address) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *candidates = NULL;
  VkrVulkanGraphBufferInstance *classifications = NULL;
  VkrVulkanGraphBufferInstance *visible = NULL;
  VkrVulkanGraphBufferInstance *states = NULL;
  VkrVulkanGraphBufferInstance *commands = NULL;
  switch (pipeline) {
  case VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY:
    candidates = vkr_vk_deferred_buffer(renderer, pass, 0u);
    classifications = vkr_vk_deferred_buffer(renderer, pass, 1u);
    states = vkr_vk_deferred_buffer(renderer, pass, 2u);
    break;
  case VKR_VULKAN_DEFERRED_PIPELINE_PREFIX:
    states = vkr_vk_deferred_buffer(renderer, pass, 0u);
    break;
  case VKR_VULKAN_DEFERRED_PIPELINE_ENCODE:
    candidates = vkr_vk_deferred_buffer(renderer, pass, 0u);
    classifications = vkr_vk_deferred_buffer(renderer, pass, 1u);
    visible = vkr_vk_deferred_buffer(renderer, pass, 2u);
    states = vkr_vk_deferred_buffer(renderer, pass, 3u);
    commands = vkr_vk_deferred_buffer(renderer, pass, 4u);
    break;
  default:
    return false_v;
  }
  if (!states ||
      (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY &&
       (!candidates || !classifications)) ||
      (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_ENCODE &&
       (!candidates || !classifications || !visible || !commands)))
    return false_v;
  const uint32_t view_count =
      transmission ? 1u : 1u + renderer->prepared_frame.shadow_cascade_count;
  uint64_t views_address = 0u;
  uint64_t planes_address = 0u;
  Mat4 *views = NULL;
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY) {
    const VkrRenderPacket *packet = renderer->graph->packet;
    views = vkr_vk_frame_upload_allocate(slot,
                                         (uint64_t)view_count * sizeof(*views),
                                         _Alignof(Mat4), &views_address, NULL);
    Vec4 *planes = vkr_vk_frame_upload_allocate(
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
      views[i] = packet->shadow->cascades[i - 1u].light_view_projection;
      const VkrFrustum shadow = vkr_frustum_from_matrix(views[i]);
      for (uint32_t plane = 0u; plane < VKR_FRUSTUM_PLANE_COUNT; ++plane) {
        const VkrPlane *source = &shadow.planes[plane];
        planes[i * VKR_FRUSTUM_PLANE_COUNT + plane] = (Vec4){
            source->normal.x, source->normal.y, source->normal.z, source->d};
      }
    }
  }
  *out_root = (VkrVulkanCullRoot){
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
          VKR_VULKAN_DEFERRED_COMMAND_PARTITION_CAPACITY,
      .hzb_depth_epsilon = 1e-3f,
      .camera_required_flags =
          transmission ? 0u : VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE,
      .shadow_required_flags = VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER,
  };
  for (uint32_t mip = 0u; mip < VKR_VULKAN_TEXTURE_MIP_MAX; ++mip)
    out_root->hzb_textures[mip] = UINT32_MAX;
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY && !transmission &&
      renderer->config.hzb_enabled) {
    const VkrRgImageUse *hzb_use =
        vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u);
    if (hzb_use && vkr_rg_image_handle_valid(hzb_use->image)) {
      VkrVulkanGraphImage *hzb =
          &renderer->graph_images[hzb_use->image.id - 1u];
      const Mat4 current_view_projection = views[0];
      VkrVulkanGraphImageInstance *selected = NULL;
      for (uint32_t i = 0u; i < hzb->instance_count; ++i) {
        VkrVulkanGraphImageInstance *candidate = &hzb->instances[i];
        if (i == renderer->history_output_index || !candidate->history_valid ||
            candidate->history_producer_submit_value >
                renderer->completed_value ||
            candidate->history_world_epoch != slot->gpu_world_epoch ||
            candidate->history_width !=
                renderer->prepared_frame.viewport_width ||
            candidate->history_height !=
                renderer->prepared_frame.viewport_height ||
            MemCompare(&candidate->history_view_projection,
                       &current_view_projection,
                       sizeof(current_view_projection)) != 0)
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
  return vkr_vk_deferred_push_root(
      renderer, command, out_root, sizeof(*out_root),
      _Alignof(VkrVulkanCullRoot), out_root_address);
}

void vkr_vk_mark_hzb_submitted(VkrVulkanRenderer *renderer,
                               uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (slot->hzb_history_input)
    slot->hzb_history_input->last_use_submit_value = submit_value;
  VkrVulkanGraphImageInstance *instance = slot->hzb_history_output;
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

bool8_t vkr_vk_record_temporal_transform(VkrVulkanRenderer *renderer,
                                         VkCommandBuffer command,
                                         const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *instances =
      vkr_vk_deferred_buffer(renderer, pass, 0u);
  VkrVulkanGraphBufferInstance *output =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  VkrVulkanGraphImage *colors =
      vkr_vk_temporal_graph_image(renderer, "temporal_history_color");
  VkrVulkanGraphImage *depths =
      vkr_vk_temporal_graph_image(renderer, "temporal_history_depth");
  VkrVulkanGraphImage *identities =
      vkr_vk_temporal_graph_image(renderer, "temporal_history_identity");
  VkrVulkanGraphImage *primitives =
      vkr_vk_temporal_graph_image(renderer, "temporal_history_primitive");
  if (!instances || !output || !colors || !depths || !identities ||
      !primitives ||
      !vkr_rg_buffer_handle_valid(
          renderer->temporal_transform_history_handle) ||
      colors->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
      depths->instance_count != colors->instance_count ||
      identities->instance_count != colors->instance_count ||
      primitives->instance_count != colors->instance_count)
    return false_v;

  const uint32_t current = renderer->history_output_index;
  slot->temporal_transform_output = output;
  slot->temporal_color_output = &colors->instances[current];
  slot->temporal_depth_output = &depths->instances[current];
  slot->temporal_identity_output = &identities->instances[current];
  slot->temporal_primitive_output = &primitives->instances[current];

  const VkrRenderPacket *packet = renderer->graph->packet;
  VkrVulkanGraphImageInstance *selected = NULL;
  uint32_t selected_index = UINT32_MAX;
  if (packet->globals.temporal.history_valid) {
    for (uint32_t i = 0u; i < colors->instance_count; ++i) {
      VkrVulkanGraphImageInstance *candidate = &colors->instances[i];
      if (i == current || !candidate->history_valid ||
          candidate->history_producer_submit_value >
              renderer->completed_value ||
          candidate->history_scene_generation !=
              packet->frame.scene_generation ||
          candidate->history_width != renderer->prepared_frame.viewport_width ||
          candidate->history_height != renderer->prepared_frame.viewport_height)
        continue;
      if (!selected || candidate->history_producer_submit_value >
                           selected->history_producer_submit_value) {
        selected = candidate;
        selected_index = i;
      }
    }
  }
  if (selected) {
    VkrVulkanGraphBuffer *transforms =
        &renderer
             ->graph_buffers[renderer->temporal_transform_history_handle.id -
                             1u];
    if (selected_index >= transforms->instance_count)
      return false_v;
    VkrVulkanGraphBufferInstance *previous_transform =
        &transforms->instances[selected_index];
    VkrVulkanGraphImageInstance *previous_depth =
        &depths->instances[selected_index];
    VkrVulkanGraphImageInstance *previous_identity =
        &identities->instances[selected_index];
    VkrVulkanGraphImageInstance *previous_primitive =
        &primitives->instances[selected_index];
    if (!previous_transform->history_valid || !selected->has_sampled_slot ||
        !previous_depth->has_storage_slot ||
        !previous_identity->has_storage_slot ||
        !previous_primitive->has_storage_slot) {
      selected = NULL;
    } else {
      const VkBufferMemoryBarrier2 buffer_barrier = {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .buffer = previous_transform->buffer.handle,
          .size = VK_WHOLE_SIZE,
      };
      VkImageMemoryBarrier2 image_barriers[4] = {0};
      VkrVulkanGraphImageInstance *history_images[] = {
          selected, previous_depth, previous_identity, previous_primitive};
      for (uint32_t i = 0u; i < ArrayCount(history_images); ++i) {
        image_barriers[i] = (VkImageMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = i == 0u ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                    : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = i == 0u ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                     : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = i == 0u ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                     : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .oldLayout = i == 0u ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = i == 0u ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = history_images[i]->image.handle,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1u,
                    .layerCount = 1u,
                },
        };
      }
      const VkDependencyInfo dependency = {
          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
          .imageMemoryBarrierCount = ArrayCount(image_barriers),
          .pImageMemoryBarriers = image_barriers,
          .bufferMemoryBarrierCount = 1u,
          .pBufferMemoryBarriers = &buffer_barrier,
      };
      vkCmdPipelineBarrier2(command, &dependency);
      slot->temporal_transform_input = previous_transform;
      slot->temporal_color_input = selected;
      slot->temporal_depth_input = previous_depth;
      slot->temporal_identity_input = previous_identity;
      slot->temporal_primitive_input = previous_primitive;
      slot->temporal_history_valid = true_v;
    }
  }

  const VkrVulkanTemporalTransformRoot root = {
      .instances = instances->buffer.address,
      .transforms = output->buffer.address,
      .instance_count = slot->gpu_candidate_count,
      .transform_capacity = VKR_TEMPORAL_TRANSFORM_CAPACITY,
      .frame_index = packet->frame.frame_index,
  };
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanTemporalTransformRoot),
                                 &root_address))
    return false_v;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->deferred_pipelines
                        [VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_TRANSFORM]);
  if (root.instance_count)
    vkCmdDispatch(command, (root.instance_count + 63u) / 64u, 1u, 1u);
  return true_v;
}

void vkr_vk_mark_temporal_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImageInstance *inputs[] = {
      slot->temporal_color_input, slot->temporal_depth_input,
      slot->temporal_identity_input, slot->temporal_primitive_input};
  for (uint32_t i = 0u; i < ArrayCount(inputs); ++i)
    if (inputs[i])
      inputs[i]->last_use_submit_value = submit_value;
  if (slot->temporal_transform_input)
    slot->temporal_transform_input->last_use_submit_value = submit_value;

  const VkrRenderPacket *packet = renderer->graph->packet;
  VkrVulkanGraphImageInstance *outputs[] = {
      slot->temporal_color_output, slot->temporal_depth_output,
      slot->temporal_identity_output, slot->temporal_primitive_output};
  for (uint32_t i = 0u; i < ArrayCount(outputs); ++i) {
    VkrVulkanGraphImageInstance *instance = outputs[i];
    if (!instance)
      continue;
    instance->history_producer_submit_value = submit_value;
    instance->history_frame_index = packet->frame.frame_index;
    instance->history_scene_generation = packet->frame.scene_generation;
    instance->history_view_projection =
        packet->globals.temporal.current_view_projection;
    instance->history_width = renderer->prepared_frame.viewport_width;
    instance->history_height = renderer->prepared_frame.viewport_height;
    instance->history_valid = true_v;
  }
  if (slot->temporal_transform_output) {
    slot->temporal_transform_output->history_producer_submit_value =
        submit_value;
    slot->temporal_transform_output->history_frame_index =
        packet->frame.frame_index;
    slot->temporal_transform_output->history_scene_generation =
        packet->frame.scene_generation;
    slot->temporal_transform_output->history_valid = true_v;
  }
}

bool8_t vkr_vk_record_deferred_cull(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass,
                                    VkrVulkanDeferredPipeline pipeline,
                                    bool8_t transmission) {
  VkrVulkanCullRoot root = {0};
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_cull_root(renderer, command, pass, pipeline,
                                 transmission, &root, &root_address))
    return false_v;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->deferred_pipelines[pipeline]);
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_PREFIX) {
    vkCmdDispatch(command, root.view_count, 1u, 1u);
  } else {
    const uint64_t work = (uint64_t)root.candidate_count * root.view_count;
    if (work)
      vkCmdDispatch(command, (uint32_t)((work + 63u) / 64u), 1u, 1u);
  }
  return true_v;
}

bool8_t vkr_vk_record_deferred_raster(VkrVulkanRenderer *renderer,
                                      VkCommandBuffer command,
                                      const VkrRgPass *pass, bool8_t shadow,
                                      bool8_t transmission) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  VkrVulkanGraphBufferInstance *states =
      vkr_vk_deferred_buffer(renderer, pass, 2u);
  VkrVulkanGraphBufferInstance *commands =
      vkr_vk_deferred_buffer(renderer, pass, transmission ? 4u : 3u);
  if (!visible || !states || !commands)
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const uint32_t view_index =
      shadow ? 1u + pass->desc.depth_attachment.desc.slice.base_layer : 0u;
  const Mat4 view_projection =
      shadow ? packet->shadow->cascades[view_index - 1u].light_view_projection
             : mat4_mul(packet->globals.temporal.jittered_projection,
                        packet->globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint64_t frame_address = 0u;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame,
      transmission ? slot->transmission_gpu_candidate_instances
                   : slot->gpu_candidate_instances,
      view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, false_v, transmission);
  VkrVulkanRasterRoot root = {
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
      if (!vkr_vk_deferred_sampled_index(renderer, pass, 3u,
                                         &root.previous_depth_texture))
        return false_v;
      root.previous_depth_layer =
          previous_depth->has_slice ? previous_depth->slice.base_layer : 0u;
    }
  }
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanRasterRoot), &root_address))
    return false_v;
  // Opaque camera buckets can use the discard-free visibility fragment. A
  // transmission peel cannot because every bucket may discard against the
  // preceding layer's depth.
  const bool8_t early_z_opaque =
      !shadow && root.previous_depth_texture == UINT32_MAX;
  VkrVulkanPacketPipeline bound_pipeline = VKR_VULKAN_PACKET_PIPELINE_COUNT;
  vkCmdBindIndexBuffer(command, renderer->geometry_megabuffer.indices.handle,
                       0u, VK_INDEX_TYPE_UINT32);
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    const bool8_t opaque_bucket =
        bucket == VKR_WORLD_DRAW_STATE_OPAQUE_BACK ||
        bucket == VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED;
    const VkrVulkanPacketPipeline wanted_pipeline =
        shadow ? (opaque_bucket
                      ? VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW_OPAQUE
                      : VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW)
        : (early_z_opaque && opaque_bucket)
            ? VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE
            : VKR_VULKAN_PACKET_PIPELINE_VISIBILITY;
    if (wanted_pipeline != bound_pipeline) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        renderer->packet_pipelines[wanted_pipeline]);
      bound_pipeline = wanted_pipeline;
    }
    const VkrVulkanPushConstants push = {
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
        VKR_VULKAN_DEFERRED_COMMAND_PARTITION_CAPACITY *
        sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize count_offset =
        (VkDeviceSize)view_index * sizeof(VkrGpuDrawCompactionState) +
        offsetof(VkrGpuDrawCompactionState, bucket_counts) +
        bucket * sizeof(uint32_t);
    vkCmdDrawIndexedIndirectCount(
        command, commands->buffer.handle, argument_offset,
        states->buffer.handle, count_offset,
        VKR_VULKAN_DEFERRED_COMMAND_PARTITION_CAPACITY,
        sizeof(VkDrawIndexedIndirectCommand));
  }
  return true_v;
}

bool8_t vkr_vk_record_deferred_gbuffer(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 7u);
  uint32_t indices[9] = {0};
  if (!visible || !state || !vkr_vk_deferred_buffer(renderer, pass, 10u))
    return false_v;
  const uint32_t bindings[] = {0u, 2u, 3u, 4u, 5u, 6u, 8u, 11u, 12u};
  for (uint32_t i = 0u; i < ArrayCount(bindings); ++i)
    if (!vkr_vk_deferred_storage_index(renderer, pass, bindings[i],
                                       &indices[i]))
      return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const VkrVulkanResolveRoot root = {
      .geometry_rows = slot->gpu_geometry_rows,
      .visible_rows = visible->buffer.address,
      .vertices = renderer->geometry_megabuffer.vertices.address,
      .instances = slot->gpu_candidate_instances,
      .materials = renderer->materials.address,
      .indices = renderer->geometry_megabuffer.indices.address,
      .compaction_state = state->buffer.address,
      .previous_transforms =
          slot->temporal_history_valid
              ? slot->temporal_transform_input->buffer.address
              : 0u,
      .view_projection = mat4_mul(packet->globals.temporal.jittered_projection,
                                  packet->globals.view),
      .current_view_projection =
          packet->globals.temporal.current_view_projection,
      .previous_view_projection =
          slot->temporal_history_valid
              ? slot->temporal_color_input->history_view_projection
              : packet->globals.temporal.current_view_projection,
      .vbuffer_texture = indices[0],
      .albedo_texture = indices[1],
      .specular_texture = indices[2],
      .normal_texture = indices[3],
      .emissive_texture = indices[4],
      .debug_texture = indices[5],
      .scene_texture = indices[6],
      .motion_texture = indices[7],
      .validity_texture = indices[8],
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .geometry_count = renderer->config.geometry_capacity,
      .material_count = renderer->config.material_slot_capacity,
      .instance_count = slot->gpu_candidate_count,
      .render_mode = packet->globals.render_mode,
      .history_valid = slot->temporal_history_valid,
      .previous_frame_index =
          slot->temporal_history_valid
              ? slot->temporal_color_input->history_frame_index
              : packet->frame.frame_index,
  };
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanResolveRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

bool8_t vkr_vk_record_deferred_lighting(VkrVulkanRenderer *renderer,
                                        VkCommandBuffer command,
                                        const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint32_t vbuffer = 0u, depth = 0u, albedo = 0u, specular = 0u, normal = 0u,
           scene = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u, &albedo) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 3u, &specular) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &normal) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 6u, &scene))
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(
      packet->globals.temporal.jittered_projection, packet->globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.shadow_cascade_count > 0u &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 5u, &shadow_texture))
    return false_v;
  uint32_t gtao_visibility = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 7u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 7u, &gtao_visibility))
    return false_v;
  /* A cubemap still publishing its initial upload is not sampled this frame;
     the fallback colour matches the authored forward skybox clear. */
  uint32_t sky_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t sky_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t sky_enabled = 0u;
  VkrVulkanPublishedTexture *sky =
      packet->skybox
          ? vkr_vk_published_texture(renderer, packet->skybox->cubemap, NULL)
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
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  vkr_vk_fill_packet_frame_root(renderer, frame_root, slot, &frame,
                                slot->gpu_candidate_instances, view_projection,
                                shadow_texture, VKR_VULKAN_SENTINEL_SLOT_INDEX,
                                true_v, false_v);
  const VkrVulkanLightingRoot root = {
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
      .gtao_visibility_texture = gtao_visibility,
  };
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanLightingRoot),
                                 &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_LIGHTING]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

bool8_t vkr_vk_record_temporal_resolve(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 6u);
  VkrVulkanGraphBufferInstance *instances =
      vkr_vk_deferred_buffer(renderer, pass, 7u);
  uint32_t sampled[5] = {0};
  uint32_t storage[5] = {0};
  const uint32_t sampled_bindings[] = {0u, 1u, 2u, 3u, 4u};
  const uint32_t storage_bindings[] = {5u, 8u, 9u, 10u, 11u};
  if (!visible || !instances)
    return false_v;
  for (uint32_t i = 0u; i < ArrayCount(sampled_bindings); ++i)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, sampled_bindings[i],
                                       &sampled[i]))
      return false_v;
  for (uint32_t i = 0u; i < ArrayCount(storage_bindings); ++i)
    if (!vkr_vk_deferred_storage_index(renderer, pass, storage_bindings[i],
                                       &storage[i]))
      return false_v;
  const bool8_t transmission_enabled =
      vkr_rg_pass_find_image_use(&pass->desc, 12u, 0u) != NULL;
  VkrVulkanGraphBufferInstance *transmission_visible =
      transmission_enabled ? vkr_vk_deferred_buffer(renderer, pass, 13u) : NULL;
  VkrVulkanGraphBufferInstance *transmission_instances =
      transmission_enabled ? vkr_vk_deferred_buffer(renderer, pass, 14u) : NULL;
  uint32_t transmission_vbuffer = 0u;
  uint32_t transmission_depth = 0u;
  if (transmission_enabled &&
      (!transmission_visible || !transmission_instances ||
       !vkr_vk_deferred_storage_index(renderer, pass, 12u,
                                      &transmission_vbuffer) ||
       !vkr_vk_deferred_sampled_index(renderer, pass, 15u,
                                      &transmission_depth)))
    return false_v;

  const bool8_t history_valid =
      slot->temporal_history_valid && slot->temporal_color_input &&
      slot->temporal_depth_input && slot->temporal_identity_input &&
      slot->temporal_primitive_input;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const VkrVulkanTemporalResolveRoot root = {
      .visible_rows = visible->buffer.address,
      .instances = instances->buffer.address,
      .scene_texture = sampled[0],
      .pre_transmission_texture = sampled[1],
      .motion_texture = sampled[2],
      .validity_texture = sampled[3],
      .depth_texture = sampled[4],
      .vbuffer_texture = storage[0],
      .history_color_texture =
          history_valid ? slot->temporal_color_input->sampled_slot.index
                        : sampled[0],
      .history_depth_texture =
          history_valid ? slot->temporal_depth_input->storage_slot.index
                        : storage[2],
      .history_identity_texture =
          history_valid ? slot->temporal_identity_input->storage_slot.index
                        : storage[3],
      .history_primitive_texture =
          history_valid ? slot->temporal_primitive_input->storage_slot.index
                        : storage[4],
      .output_color_texture = storage[1],
      .output_depth_texture = storage[2],
      .output_identity_texture = storage[3],
      .output_primitive_texture = storage[4],
      .history_sampler = renderer->transmission_sampler_slot,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .history_valid = history_valid,
      .render_mode = packet->globals.render_mode,
      .camera_stationary =
          history_valid &&
          MemCompare(&slot->temporal_color_input->history_view_projection,
                     &packet->globals.temporal.current_view_projection,
                     sizeof(Mat4)) == 0,
      .transmission_visible_rows =
          transmission_visible ? transmission_visible->buffer.address : 0u,
      .transmission_instances =
          transmission_instances ? transmission_instances->buffer.address : 0u,
      .transmission_vbuffer_texture = transmission_vbuffer,
      .transmission_depth_texture = transmission_depth,
      .transmission_enabled = transmission_enabled,
  };
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanTemporalResolveRoot),
                                 &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_RESOLVE]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

bool8_t vkr_vk_record_deferred_hzb(VkrVulkanRenderer *renderer,
                                   VkCommandBuffer command,
                                   const VkrRgPass *pass) {
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *write = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  VkrVulkanGraphImageInstance *source =
      read ? vkr_vk_deferred_image(renderer, read->image) : NULL;
  VkrVulkanGraphImageInstance *destination =
      write ? vkr_vk_deferred_image(renderer, write->image) : NULL;
  uint32_t source_index = 0u, destination_index = 0u;
  const bool8_t source_is_depth =
      read && (read->access & VKR_RG_IMAGE_ACCESS_SAMPLED);
  if (!source || !destination ||
      !(source_is_depth
            ? vkr_vk_deferred_sampled_index(renderer, pass, 0u, &source_index)
            : vkr_vk_deferred_storage_index(renderer, pass, 0u,
                                            &source_index)) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 1u, &destination_index))
    return false_v;
  renderer->frame_slots[renderer->active_frame_slot].hzb_history_output =
      destination;
  const uint32_t source_mip = read->has_slice ? read->slice.mip_level : 0u;
  const uint32_t destination_mip =
      write->has_slice ? write->slice.mip_level : 0u;
  const VkrVulkanHzbRoot root = {
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
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanHzbRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_HZB]);
  vkCmdDispatch(command, (root.destination_extent[0] + 7u) / 8u,
                (root.destination_extent[1] + 7u) / 8u, 1u);
  return true_v;
}
bool8_t vkr_vk_record_deferred_sdsm(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass) {
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 2u);
  uint32_t depth = 0u, vbuffer = 0u;
  if (!state || !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &vbuffer))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->sdsm_reduce_state = state;
  const VkrVulkanSdsmRoot root = {
      .reduce_state = state->buffer.address,
      .depth_texture = depth,
      .vbuffer_texture = vbuffer,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
  };
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanSdsmRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SDSM]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

/**
 * @brief Builds the metering root shared by both exposure passes.
 *
 * The two passes agree on one root so the metering constants the histogram
 * binned with are the same ones the resolve reduces with. Deriving them twice
 * would let a mid-frame configuration change split a frame's decision.
 */
vkr_internal bool8_t vkr_vk_exposure_root(VkrVulkanRenderer *renderer,
                                          const VkrRgPass *pass,
                                          uint32_t histogram_binding,
                                          VkrVulkanExposureRoot *out_root) {
  VkrVulkanGraphBufferInstance *histogram =
      vkr_vk_deferred_buffer(renderer, pass, histogram_binding);
  if (!histogram)
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  *out_root = (VkrVulkanExposureRoot){
      .histogram = histogram->buffer.address,
      .reset_reasons = packet->globals.exposure.reset_reasons,
      .metering = vkr_exposure_gpu_metering(&renderer->exposure_metering,
                                            &packet->globals.exposure),
  };
  return true_v;
}

bool8_t vkr_vk_record_exposure_histogram(VkrVulkanRenderer *renderer,
                                         VkCommandBuffer command,
                                         const VkrRgPass *pass) {
  const VkrRgImageUse *source_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  VkrVulkanGraphImageInstance *source =
      source_use ? vkr_vk_deferred_image(renderer, source_use->image) : NULL;
  VkrVulkanExposureRoot root = {0};
  uint32_t source_index = 0u;
  if (!source || !vkr_vk_exposure_root(renderer, pass, 1u, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &source_index))
    return false_v;
  renderer->frame_slots[renderer->active_frame_slot].exposure_histogram =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  root.source_texture = source_index;
  root.extent[0] = source->image.width;
  root.extent[1] = source->image.height;

  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanExposureRoot),
                                 &root_address))
    return false_v;
  /* The bounded histogram is cleared and filled inside one pass, so the first
     use of a frame slot does not depend on device memory arriving zeroed. */
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_CLEAR]);
  vkCmdDispatch(command, 1u, 1u, 1u);
  const VkMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1u,
      .pMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->deferred_pipelines
                        [VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_HISTOGRAM]);
  vkCmdDispatch(command, (root.extent[0] + 15u) / 16u,
                (root.extent[1] + 15u) / 16u, 1u);
  return true_v;
}

bool8_t vkr_vk_record_exposure_resolve(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass) {
  const VkrRgBufferUse *state_use =
      vkr_rg_pass_find_buffer_use(&pass->desc, 1u, 0u);
  VkrVulkanGraphBufferInstance *output =
      state_use ? vkr_vk_graph_buffer(renderer, state_use->buffer) : NULL;
  VkrVulkanExposureRoot root = {0};
  if (!output || !vkr_vk_exposure_root(renderer, pass, 0u, &root))
    return false_v;

  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->exposure_state_output = output;

  /* Newest completed record, chosen the same way temporal reconstruction picks
     its history: a record the GPU has not finished writing is not history. */
  VkrVulkanGraphBuffer *states =
      &renderer->graph_buffers[state_use->buffer.id - 1u];
  slot->exposure_state_history = states;
  VkrVulkanGraphBufferInstance *previous = NULL;
  if (renderer->graph->packet->globals.exposure.history_valid) {
    for (uint32_t i = 0u; i < states->instance_count; ++i) {
      VkrVulkanGraphBufferInstance *candidate = &states->instances[i];
      if (candidate == output || !candidate->history_valid ||
          candidate->history_producer_submit_value >
              renderer->completed_value ||
          candidate->history_scene_generation !=
              renderer->graph->packet->frame.scene_generation)
        continue;
      if (!previous || candidate->history_producer_submit_value >
                           previous->history_producer_submit_value)
        previous = candidate;
    }
  }
  if (previous) {
    slot->exposure_state_input = previous;
    const VkBufferMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = previous->buffer.handle,
        .size = VK_WHOLE_SIZE,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1u,
        .pBufferMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  } else {
    /* No completed record. The kernel still reads this address and discards the
       value through `history_valid`, so it points at the output instance rather
       than at nothing. */
    root.metering.history_valid = 0u;
  }
  root.state = output->buffer.address;
  root.previous_state =
      previous ? previous->buffer.address : output->buffer.address;

  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanExposureRoot),
                                 &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_RESOLVE]);
  vkCmdDispatch(command, 1u, 1u, 1u);
  return true_v;
}

void vkr_vk_mark_exposure_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (slot->exposure_state_input)
    slot->exposure_state_input->last_use_submit_value = submit_value;
  if (!slot->exposure_state_output)
    return;
  const VkrRenderPacket *packet = renderer->graph->packet;
  if (packet->globals.exposure.reset_reasons && slot->exposure_state_history) {
    for (uint32_t i = 0u; i < slot->exposure_state_history->instance_count; ++i)
      slot->exposure_state_history->instances[i].history_valid = false_v;
  }
  slot->exposure_state_output->history_producer_submit_value = submit_value;
  slot->exposure_state_output->history_frame_index = packet->frame.frame_index;
  slot->exposure_state_output->history_scene_generation =
      packet->frame.scene_generation;
  slot->exposure_state_output->history_valid = true_v;
}

/**
 * @brief Builds the root shared by all four bloom pass kinds.
 *
 * Every level derives its own extents from the authored subresource rather than
 * from a pass index, exactly as the HZB chain does. The chain length is then a
 * graph decision alone, and an executor cannot disagree with the graph about
 * which mip it is writing.
 */
vkr_internal bool8_t vkr_vk_bloom_root(VkrVulkanRenderer *renderer,
                                       const VkrRgPass *pass,
                                       VkrVulkanBloomRoot *out_root) {
  const VkrRgImageUse *source_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  VkrVulkanGraphImageInstance *source =
      source_use ? vkr_vk_deferred_image(renderer, source_use->image) : NULL;
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  uint32_t source_index = 0u, destination_index = 0u;
  if (!source || !destination ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &source_index) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 1u, &destination_index))
    return false_v;

  const uint32_t source_mip =
      source_use->has_slice ? source_use->slice.mip_level : 0u;
  const uint32_t destination_mip =
      destination_use->has_slice ? destination_use->slice.mip_level : 0u;
  const VkrRenderPacket *packet = renderer->graph->packet;
  *out_root = (VkrVulkanBloomRoot){
      .source_texture = source_index,
      /* Overwritten by the upsample pass. Every other pass leaves it pointing
         at its own source so the descriptor is always a live sampled view. */
      .coarse_texture = source_index,
      .destination_texture = destination_index,
      .source_sampler = renderer->transmission_sampler_slot,
      .filter_extent = {Max(1u, source->image.width >> source_mip),
                        Max(1u, source->image.height >> source_mip)},
      .destination_extent = {Max(1u,
                                 destination->image.width >> destination_mip),
                             Max(1u,
                                 destination->image.height >> destination_mip)},
      .params =
          vkr_bloom_gpu_params(&renderer->bloom_config, &packet->globals.bloom),
  };
  return true_v;
}

vkr_internal bool8_t vkr_vk_dispatch_bloom(VkrVulkanRenderer *renderer,
                                           VkCommandBuffer command,
                                           const VkrVulkanBloomRoot *root,
                                           VkrVulkanDeferredPipeline pipeline) {
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, root, sizeof(*root),
                                 _Alignof(VkrVulkanBloomRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->deferred_pipelines[pipeline]);
  vkCmdDispatch(command, (root->destination_extent[0] + 7u) / 8u,
                (root->destination_extent[1] + 7u) / 8u, 1u);
  return true_v;
}

bool8_t vkr_vk_record_bloom_prefilter(VkrVulkanRenderer *renderer,
                                      VkCommandBuffer command,
                                      const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root))
    return false_v;
  return vkr_vk_dispatch_bloom(renderer, command, &root,
                               VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_PREFILTER);
}

bool8_t vkr_vk_record_bloom_downsample(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root))
    return false_v;
  /* Both filters are resident; the cold configuration selects which one this
     build measures. Neither is a fallback for the other. */
  return vkr_vk_dispatch_bloom(
      renderer, command, &root,
      renderer->bloom_config.filter == VKR_BLOOM_FILTER_BOX_4
          ? VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_DOWNSAMPLE_BOX4
          : VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_DOWNSAMPLE_TENT13);
}

bool8_t vkr_vk_record_bloom_upsample(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command,
                                     const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root))
    return false_v;

  /* Binding 2 is the accumulation level above this one; binding 3 is the
     downsample level at the same depth. They are the same extent and the same
     content at the deepest step, because nothing has accumulated into the
     accumulation chain yet. Choosing between them here rather than in the
     kernel is what keeps every sampled texel defined without a bootstrap pass
     or a shader branch. */
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  const VkrRgImageUse *coarse_use =
      vkr_rg_pass_find_image_use(&pass->desc, 2u, 0u);
  VkrVulkanGraphImageInstance *coarse =
      coarse_use ? vkr_vk_deferred_image(renderer, coarse_use->image) : NULL;
  const uint32_t destination_mip = destination_use && destination_use->has_slice
                                       ? destination_use->slice.mip_level
                                       : 0u;
  const bool8_t deepest =
      destination_mip + 2u >= renderer->prepared_frame.bloom_mip_count;
  if (!coarse || !vkr_vk_deferred_sampled_index(
                     renderer, pass, deepest ? 3u : 2u, &root.coarse_texture))
    return false_v;

  const uint32_t coarse_mip =
      coarse_use->has_slice ? coarse_use->slice.mip_level : 0u;
  root.filter_extent[0] = Max(1u, coarse->image.width >> coarse_mip);
  root.filter_extent[1] = Max(1u, coarse->image.height >> coarse_mip);
  return vkr_vk_dispatch_bloom(renderer, command, &root,
                               VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_UPSAMPLE);
}

bool8_t vkr_vk_record_bloom_combine(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &root.coarse_texture))
    return false_v;
  return vkr_vk_dispatch_bloom(renderer, command, &root,
                               VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_COMBINE);
}

vkr_internal bool8_t vkr_vk_gtao_root(VkrVulkanRenderer *renderer,
                                      const VkrRgPass *pass,
                                      uint32_t source_binding,
                                      uint32_t destination_binding,
                                      VkrVulkanGtaoRoot *out_root) {
  const VkrRgImageUse *source_use =
      vkr_rg_pass_find_image_use(&pass->desc, source_binding, 0u);
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, destination_binding, 0u);
  VkrVulkanGraphImageInstance *source =
      source_use ? vkr_vk_deferred_image(renderer, source_use->image) : NULL;
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  uint32_t source_texture = 0u, destination_texture = 0u;
  if (!source || !destination ||
      !vkr_vk_deferred_sampled_index(renderer, pass, source_binding,
                                     &source_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, destination_binding,
                                     &destination_texture))
    return false_v;

  const uint32_t source_mip =
      source_use->has_slice ? source_use->slice.mip_level : 0u;
  const uint32_t destination_mip =
      destination_use->has_slice ? destination_use->slice.mip_level : 0u;
  *out_root = (VkrVulkanGtaoRoot){
      .params = renderer->gtao_params,
      .source_texture = source_texture,
      .vbuffer_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .normal_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .destination_texture = destination_texture,
      .edges_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .point_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .source_extent = {Max(1u, source->image.width >> source_mip),
                        Max(1u, source->image.height >> source_mip)},
      .destination_extent = {Max(1u,
                                 destination->image.width >> destination_mip),
                             Max(1u,
                                 destination->image.height >> destination_mip)},
  };
  return true_v;
}

vkr_internal bool8_t vkr_vk_dispatch_gtao(VkrVulkanRenderer *renderer,
                                          VkCommandBuffer command,
                                          const VkrVulkanGtaoRoot *root,
                                          VkrVulkanDeferredPipeline pipeline) {
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, root, sizeof(*root),
                                 _Alignof(VkrVulkanGtaoRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->deferred_pipelines[pipeline]);
  vkCmdDispatch(command, (root->destination_extent[0] + 7u) / 8u,
                (root->destination_extent[1] + 7u) / 8u, 1u);
  return true_v;
}

bool8_t vkr_vk_record_gtao_depth_prefilter(VkrVulkanRenderer *renderer,
                                           VkCommandBuffer command,
                                           const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 0u, 1u, &root))
    return false_v;
  return vkr_vk_dispatch_gtao(
      renderer, command, &root,
      VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DEPTH_PREFILTER);
}

bool8_t vkr_vk_record_gtao_depth_mip(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command,
                                     const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 0u, 1u, &root))
    return false_v;
  return vkr_vk_dispatch_gtao(renderer, command, &root,
                              VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DEPTH_MIP);
}

bool8_t vkr_vk_record_gtao_evaluate(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 1u, 3u, &root) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u,
                                     &root.vbuffer_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u,
                                     &root.normal_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &root.edges_texture))
    return false_v;
  return vkr_vk_dispatch_gtao(renderer, command, &root,
                              VKR_VULKAN_DEFERRED_PIPELINE_GTAO_EVALUATE);
}

bool8_t vkr_vk_record_gtao_denoise(VkrVulkanRenderer *renderer,
                                   VkCommandBuffer command,
                                   const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 0u, 2u, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &root.edges_texture))
    return false_v;
  return vkr_vk_dispatch_gtao(renderer, command, &root,
                              VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DENOISE);
}

bool8_t vkr_vk_record_deferred_picking(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *opaque =
      vkr_vk_deferred_buffer(renderer, pass, 2u);
  uint32_t opaque_vbuffer = 0u, transmission_vbuffer = 0u, output = 0u;
  if (!opaque ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &opaque_vbuffer) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &output))
    return false_v;
  const bool8_t use_transmission =
      vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u) != NULL;
  VkrVulkanGraphBufferInstance *transmission =
      use_transmission ? vkr_vk_deferred_buffer(renderer, pass, 7u) : NULL;
  if (use_transmission &&
      (!transmission || !vkr_vk_deferred_storage_index(renderer, pass, 5u,
                                                       &transmission_vbuffer)))
    return false_v;
  const VkrRgImageUse *transmission_use =
      vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u);
  const VkrVulkanPickingRoot root = {
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
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanPickingRoot), &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_PICKING]);
  vkCmdDispatch(command, 1u, 1u, 1u);
  return true_v;
}

bool8_t vkr_vk_record_deferred_transmission(VkrVulkanRenderer *renderer,
                                            VkCommandBuffer command,
                                            const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 2u);
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 3u);
  uint32_t vbuffer = 0u, depth = 0u, feedback = 0u, output = 0u;
  if (!visible || !state ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 4u, &feedback) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 5u, &output))
    return false_v;
  uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.shadow_cascade_count > 0u &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 6u, &shadow_texture))
    return false_v;
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const uint32_t layer = vbuffer_use && vbuffer_use->has_slice
                             ? vbuffer_use->slice.base_layer
                             : 0u;
  uint32_t motion = 0u;
  uint32_t validity = 0u;
  if (layer == 0u &&
      (!vkr_vk_deferred_buffer(renderer, pass, 10u) ||
       !vkr_vk_deferred_storage_index(renderer, pass, 11u, &motion) ||
       !vkr_vk_deferred_storage_index(renderer, pass, 12u, &validity)))
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(
      packet->globals.temporal.jittered_projection, packet->globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint64_t frame_address = 0u;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  vkr_vk_fill_packet_frame_root(renderer, frame_root, slot, &frame,
                                slot->transmission_gpu_candidate_instances,
                                view_projection, shadow_texture,
                                VKR_VULKAN_SENTINEL_SLOT_INDEX, true_v, true_v);
  const VkrVulkanTransmissionRoot root = {
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
      .previous_transforms =
          (slot->temporal_history_valid ? slot->temporal_transform_input
                                        : slot->temporal_transform_output)
              ->buffer.address,
      .current_view_projection =
          packet->globals.temporal.current_view_projection,
      .previous_view_projection =
          slot->temporal_history_valid
              ? slot->temporal_color_input->history_view_projection
              : packet->globals.temporal.current_view_projection,
      .motion_texture = motion,
      .validity_texture = validity,
      .history_valid = slot->temporal_history_valid,
      .previous_frame_index =
          slot->temporal_history_valid
              ? slot->temporal_color_input->history_frame_index
              : packet->frame.frame_index,
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .feedback_texture = feedback,
      .feedback_sampler = renderer->transmission_sampler_slot,
      .output_texture = output,
      .layer = layer,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .visible_capacity = VKR_GPU_DRAW_CANDIDATE_CAPACITY,
      .geometry_count = renderer->config.geometry_capacity,
      .material_count = renderer->config.material_slot_capacity,
      .instance_count = slot->transmission_gpu_candidate_count,
  };
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanTransmissionRoot),
                                 &root_address))
    return false_v;
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

bool8_t
vkr_vk_record_deferred_transmission_coverage(VkrVulkanRenderer *renderer,
                                             VkCommandBuffer command,
                                             const VkrRgPass *pass) {
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  uint32_t vbuffer = 0u;
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  if (!state || !vbuffer_use ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer))
    return false_v;
  const VkrVulkanTransmissionCoverageRoot root = {
      .covered_pixels = state->buffer.address +
                        offsetof(VkrGpuTransmissionDiagnostics, covered_pixels),
      .vbuffer_texture = vbuffer,
      .layer = vbuffer_use->has_slice ? vbuffer_use->slice.base_layer : 0u,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
  };
  uint64_t root_address = 0u;
  if (!vkr_vk_deferred_push_root(renderer, command, &root, sizeof(root),
                                 _Alignof(VkrVulkanTransmissionCoverageRoot),
                                 &root_address))
    return false_v;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->deferred_pipelines
                        [VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COVERAGE]);
  vkCmdDispatch(command, (root.extent[0] + 7u) / 8u, (root.extent[1] + 7u) / 8u,
                1u);
  return true_v;
}

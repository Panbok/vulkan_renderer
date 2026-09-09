#include "math/vkr_frustum.h"
#include "vulkan/vkr_vulkan_internal.h"

enum {
  VKR_VULKAN_DEFERRED_BUCKET_COUNT = VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
  VKR_VULKAN_INDIRECT_COMMAND_SIZE = sizeof(VkDrawIndexedIndirectCommand),
  VKR_VULKAN_DEFERRED_PREFIX_GROUP_SIZE = 5,
};

_Static_assert(VKR_WORLD_DRAW_STATE_BUCKET_COUNT == 8u,
               "Vulkan deferred shaders require eight draw-state buckets");
_Static_assert(VKR_FRUSTUM_PLANE_COUNT == 6u,
               "Vulkan deferred shaders require six frustum planes");
_Static_assert(VKR_VULKAN_DEFERRED_VIEW_COUNT_MAX <=
                   5u * VKR_VULKAN_DEFERRED_PREFIX_GROUP_SIZE,
               "Vulkan cull prefix group coverage is incomplete");
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
  return true_v;
}

vkr_internal bool8_t
vkr_vk_temporal_scene_equal(const VkrVulkanTemporalSceneState *current,
                            const VkrVulkanTemporalSceneState *previous) {
  return current->signature.eligible && previous->signature.eligible &&
         current->signature.hash[0] == previous->signature.hash[0] &&
         current->signature.hash[1] == previous->signature.hash[1] &&
         current->radiance_revision == previous->radiance_revision &&
         current->publication_generation == previous->publication_generation &&
         current->graph_revision == previous->graph_revision;
}

bool8_t vkr_vk_prepare_fsr31_inputs(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  const VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanFsr31PrepareRoot root = {
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .transmission_enabled = renderer->prepared_frame.transmission_pending,
      .scene_stationary =
          slot->temporal_history_valid &&
          vkr_vk_temporal_scene_equal(&slot->temporal_scene,
                                      &renderer->fsr31_history.scene),
  };
  if (!vkr_vk_deferred_sampled_index(renderer, pass, 0u, &root.scene_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u,
                                     &root.pre_transmission_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u,
                                     &root.validity_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 3u,
                                     &root.opaque_depth_texture) ||
      (root.transmission_enabled &&
       !vkr_vk_deferred_sampled_index(renderer, pass, 4u,
                                      &root.transmission_depth_texture)) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 5u,
                                     &root.output_depth_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 6u,
                                     &root.reactive_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 7u,
                                     &root.composition_texture) ||
      !vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanFsr31PrepareRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_FSR31_PREPARE];
  prepared->groups[0][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_fsr31_stabilize(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImage *colors =
      vkr_vk_temporal_graph_image(renderer, "fsr31_output_color");
  if (!colors || colors->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT)
    return false_v;
  VkrVulkanGraphImageInstance *output =
      &colors->instances[renderer->history_output_index];
  slot->temporal_color_output = output;
  VkrVulkanFsr31StabilizeRoot root = {
      .output_extent = {output->image.width, output->image.height},
      .render_extent = {renderer->prepared_frame.viewport_width,
                        renderer->prepared_frame.viewport_height},
      .jitter_pixels = renderer->graph->packet->temporal.jitter_pixels,
  };
  if (slot->temporal_history_valid &&
      vkr_vk_temporal_scene_equal(&slot->temporal_scene,
                                  &renderer->fsr31_history.scene)) {
    VkrVulkanGraphImageInstance *previous =
        &colors->instances[renderer->fsr31_history.history_index];
    if (previous->history_valid && previous->has_sampled_slot &&
        previous->history_producer_submit_value ==
            renderer->fsr31_history.submit_value &&
        previous->image.width == output->image.width &&
        previous->image.height == output->image.height) {
      root.scene_stationary = true_v;
      root.history_texture = previous->sampled_slot.index;
      slot->temporal_color_input = previous;
      // The preceding submission owns this history. Order its writes before
      // this read; the HISTORY pool separately proves completion before reuse.
      prepared->image_barriers[0] = (VkImageMemoryBarrier2){
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = previous->image.handle,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1u,
                               .layerCount = 1u},
      };
      prepared->image_barrier_count = 1u;
    }
  }
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u,
                                     &root.output_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u,
                                     &root.reactive_texture) ||
      !vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanFsr31StabilizeRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_FSR31_STABILIZE];
  prepared->groups[0][0] = (root.output_extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.output_extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_upload(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedUpload *prepared,
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
  prepared->source = slot->candidate_upload.handle;
  prepared->candidates = candidates->buffer.handle;
  prepared->instances = instances->buffer.handle;
  prepared->state = state->buffer.handle;
  prepared->sdsm = sdsm ? sdsm->buffer.handle : VK_NULL_HANDLE;
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
          .size = (uint64_t)count * sizeof(VkrPreparedInstanceGPU),
      };
      prepared->candidate_copies[prepared->copy_count] = candidate_copy;
      prepared->instance_copies[prepared->copy_count] = instance_copy;
      prepared->copy_count++;
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
          .dstOffset = (uint64_t)range->destination_first *
                       sizeof(VkrPreparedInstanceGPU),
          .size = (uint64_t)range->count * sizeof(VkrPreparedInstanceGPU),
      };
      prepared->candidate_copies[prepared->copy_count] = candidate_copy;
      prepared->instance_copies[prepared->copy_count] = instance_copy;
      prepared->copy_count++;
    }
  }
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_readback(VkrVulkanRenderer *renderer) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanPreparedReadback *prepared = &slot->deferred_readback;
  prepared->count = 0u;
  slot->shadow_cascade_count = 0u;
  slot->local_shadow_view_count = 0u;
  // UI-only frames have no Scene producers. Clear prior slot readbacks before
  // returning so reused slots cannot copy stale Scene statistics.
  if (!renderer->graph->packet->scene_rendering)
    return true_v;
  VkrVulkanGraphBufferInstance *opaque = slot->gpu_compaction_state;
  VkrVulkanGraphBufferInstance *transmission =
      renderer->prepared_frame.transmission_pending
          ? slot->transmission_gpu_compaction_state
          : NULL;
  if (!opaque ||
      (renderer->prepared_frame.transmission_pending && !transmission) ||
      (slot->sdsm_requested && !slot->sdsm_reduce_state) ||
      (slot->exposure_requested &&
       (!slot->exposure_histogram || !slot->exposure_state_output)))
    return false_v;
  slot->shadow_cascade_count = renderer->prepared_frame.shadow_cascade_count;
  slot->local_shadow_view_count =
      renderer->prepared_frame.local_shadow_view_count;
  const VkBufferCopy copies[] = {
      {.dstOffset = VKR_VULKAN_READBACK_DRAW_STATE_OFFSET,
       .size = (1u + renderer->prepared_frame.shadow_cascade_count +
                renderer->prepared_frame.local_shadow_view_count) *
               sizeof(VkrGpuDrawCompactionState)},
      {.dstOffset = VKR_VULKAN_READBACK_TRANSMISSION_STATE_OFFSET,
       .size = sizeof(VkrGpuTransmissionDiagnostics)},
      {.dstOffset = VKR_VULKAN_READBACK_SDSM_STATE_OFFSET,
       .size = VKR_VULKAN_SDSM_STATE_SIZE},
      {.dstOffset = VKR_VULKAN_READBACK_EXPOSURE_HISTOGRAM_OFFSET,
       .size = sizeof(VkrExposureGpuHistogram)},
      {.dstOffset = VKR_VULKAN_READBACK_EXPOSURE_STATE_OFFSET,
       .size = sizeof(VkrExposureGpuState)},
  };
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
    prepared->copies[prepared->count] = copies[i];
    prepared->barriers[prepared->count++] = (VkBufferMemoryBarrier2){
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
  return true_v;
}

void vkr_vk_record_deferred_readback(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const VkrVulkanPreparedReadback *prepared = &slot->deferred_readback;
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = prepared->count,
      .pBufferMemoryBarriers = prepared->barriers,
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
  for (uint32_t i = 0u; i < prepared->count; ++i)
    vkCmdCopyBuffer(command, prepared->barriers[i].buffer,
                    slot->readback.handle, 1u, &prepared->copies[i]);
}

vkr_internal bool8_t vkr_vk_deferred_cull_root(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrRgPass *pass, VkrVulkanDeferredPipeline pipeline,
    bool8_t transmission, VkrVulkanCullRoot *out_root,
    uint64_t *out_root_address) {
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
      transmission ? 1u
                   : 1u + renderer->prepared_frame.shadow_cascade_count +
                         renderer->prepared_frame.local_shadow_view_count;
  uint64_t views_address = 0u;
  uint64_t planes_address = 0u;
  Mat4 *views = NULL;
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY) {
    const VkrPreparedFrame *packet = renderer->graph->packet;
    views = vkr_vk_frame_upload_allocate(slot,
                                         (uint64_t)view_count * sizeof(*views),
                                         _Alignof(Mat4), &views_address, NULL);
    Vec4 *planes = vkr_vk_frame_upload_allocate(
        slot, (uint64_t)view_count * VKR_FRUSTUM_PLANE_COUNT * sizeof(*planes),
        _Alignof(Vec4), &planes_address, NULL);
    if (!views || !planes)
      return false_v;
    views[0] =
        mat4_mul(packet->input.globals.projection, packet->input.globals.view);
    const VkrFrustum camera = vkr_frustum_from_view_projection(
        packet->input.globals.view, packet->input.globals.projection);
    for (uint32_t plane = 0u; plane < VKR_FRUSTUM_PLANE_COUNT; ++plane) {
      const VkrPlane *source = &camera.planes[plane];
      planes[plane] = (Vec4){source->normal.x, source->normal.y,
                             source->normal.z, source->d};
    }
    for (uint32_t i = 1u; i < view_count; ++i) {
      const uint32_t cascade_count =
          renderer->prepared_frame.shadow_cascade_count;
      views[i] =
          i <= cascade_count
              ? packet->input.shadow->cascades[i - 1u].light_view_projection
              : packet->input.local_shadow->views[i - 1u - cascade_count]
                    .light_view_projection;
      const VkrFrustum shadow = vkr_frustum_from_matrix(views[i]);
      for (uint32_t plane = 0u; plane < VKR_FRUSTUM_PLANE_COUNT; ++plane) {
        const VkrPlane *source = &shadow.planes[plane];
        planes[i * VKR_FRUSTUM_PLANE_COUNT + plane] = (Vec4){
            source->normal.x, source->normal.y, source->normal.z, source->d};
      }
    }
    if (!renderer->config.frustum_enabled)
      MemZero(planes,
              (uint64_t)view_count * VKR_FRUSTUM_PLANE_COUNT * sizeof(*planes));
  }
  const uint32_t visible_capacity =
      transmission
          ? renderer->prepared_frame.transmission_gpu_draw_visible_capacity
          : renderer->prepared_frame.gpu_draw_visible_capacity;
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
      .candidate_capacity = visible_capacity,
      .command_partition_capacity =
          visible_capacity / VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
      .hzb_depth_epsilon = 1e-3f,
      .camera_required_flags =
          transmission ? 0u : VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE,
      .shadow_required_flags = VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER,
  };
  for (uint32_t mip = 0u; mip < VKR_VULKAN_TEXTURE_MIP_MAX; ++mip)
    out_root->hzb_textures[mip] = UINT32_MAX;
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY && !transmission) {
    slot->packet_build.hzb_history_checks_valid = true_v;
    if (!renderer->prepared_frame.hzb_build_enabled)
      slot->packet_build.hzb_history_rejections[VKR_HZB_HISTORY_DISABLED]++;
  }
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY && !transmission &&
      renderer->prepared_frame.hzb_build_enabled) {
    const VkrRgImageUse *hzb_use =
        vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u);
    if (hzb_use && vkr_rg_image_handle_valid(hzb_use->image)) {
      VkrVulkanGraphImage *hzb =
          &renderer->graph_images[hzb_use->image.id - 1u];
      const Mat4 current_view_projection = views[0];
      const VkrPreparedFrame *packet = renderer->graph->packet;
      const Mat4 raster_view_projection = mat4_mul(
          packet->temporal.jittered_projection, packet->input.globals.view);
      VkrVulkanGraphImageInstance *selected = NULL;
      for (uint32_t i = 0u; i < hzb->instance_count; ++i) {
        VkrVulkanGraphImageInstance *candidate = &hzb->instances[i];
        if (i == renderer->history_output_index)
          continue;
        VkrHzbHistoryRejection rejection = VKR_HZB_HISTORY_REJECTION_COUNT;
        if (!candidate->history_valid)
          rejection = VKR_HZB_HISTORY_INVALID;
        else if (candidate->history_producer_submit_value >
                 renderer->completed_value)
          rejection = VKR_HZB_HISTORY_INCOMPLETE;
        else if (candidate->history_world_epoch != slot->gpu_world_epoch)
          rejection = VKR_HZB_HISTORY_WORLD_CHANGED;
        else if (candidate->history_width !=
                     renderer->prepared_frame.viewport_width ||
                 candidate->history_height !=
                     renderer->prepared_frame.viewport_height)
          rejection = VKR_HZB_HISTORY_EXTENT_CHANGED;
        else if (MemCompare(&candidate->history_view_projection,
                            &current_view_projection, sizeof(Mat4)) != 0)
          rejection = VKR_HZB_HISTORY_CAMERA_CHANGED;
        /* Another jitter phase cannot prove coverage at the current sample. */
        else if (MemCompare(&candidate->history_raster_view_projection,
                            &raster_view_projection, sizeof(Mat4)) != 0)
          rejection = VKR_HZB_HISTORY_RASTER_CHANGED;
        if (rejection != VKR_HZB_HISTORY_REJECTION_COUNT) {
          slot->packet_build.hzb_history_rejections[rejection]++;
          continue;
        }
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
        prepared->image_barriers[0] = barrier;
        prepared->image_barrier_count = 1u;
        out_root->hzb_mip_count = selected->image.mip_levels;
        for (uint32_t mip = 0u; mip < selected->image.mip_levels; ++mip)
          out_root->hzb_textures[mip] = selected->storage_mip_slots[mip].index;
        out_root->hzb_extent[0] = selected->image.width;
        out_root->hzb_extent[1] = selected->image.height;
        out_root->hzb_enabled = 1u;
        views[0] = selected->history_raster_view_projection;
        slot->hzb_history_valid = true_v;
        slot->hzb_history_input = selected;
      }
    }
  }
  return vkr_vk_deferred_push_root(renderer, out_root, sizeof(*out_root),
                                   _Alignof(VkrVulkanCullRoot),
                                   out_root_address);
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
  const VkrPreparedFrame *packet = renderer->graph->packet;
  instance->history_producer_submit_value = submit_value;
  instance->history_world_epoch = slot->gpu_world_epoch;
  instance->history_view_projection =
      mat4_mul(packet->input.globals.projection, packet->input.globals.view);
  instance->history_raster_view_projection = mat4_mul(
      packet->temporal.jittered_projection, packet->input.globals.view);
  instance->history_width = renderer->prepared_frame.viewport_width;
  instance->history_height = renderer->prepared_frame.viewport_height;
  instance->history_valid = true_v;
}

vkr_internal void
vkr_vk_prepare_fsr31_history(VkrVulkanRenderer *renderer,
                             VkrVulkanPreparedCompute *prepared,
                             VkrVulkanGraphBufferInstance *output) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrVulkanFsr31History *history = &renderer->fsr31_history;
  slot->temporal_transform_output = output;
  if (!packet->temporal.history_valid || !history->valid ||
      history->frame_index + 1u != packet->input.frame.frame_index ||
      history->scene_generation != packet->input.frame.scene_generation ||
      history->history_index == renderer->history_output_index)
    return;
  VkrVulkanGraphBuffer *transforms =
      &renderer
           ->graph_buffers[renderer->temporal_transform_history_handle.id - 1u];
  VkrVulkanGraphBufferInstance *previous =
      &transforms->instances[history->history_index];
  if (!previous->history_valid ||
      previous->history_producer_submit_value != history->submit_value)
    return;
  prepared->buffer_barrier = (VkBufferMemoryBarrier2){
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = previous->buffer.handle,
      .size = VK_WHOLE_SIZE,
  };
  prepared->has_buffer_barrier = true_v;
  slot->temporal_transform_input = previous;
  slot->temporal_previous_view_projection = history->view_projection;
  slot->temporal_previous_frame_index = history->frame_index;
  slot->temporal_history_valid = true_v;
}

bool8_t vkr_vk_prepare_temporal_transform(VkrVulkanRenderer *renderer,
                                          VkrVulkanPreparedCompute *prepared,
                                          const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *instances =
      vkr_vk_deferred_buffer(renderer, pass, 0u);
  VkrVulkanGraphBufferInstance *output =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  if (!instances || !output ||
      !vkr_rg_buffer_handle_valid(renderer->temporal_transform_history_handle))
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if (renderer->prepared_frame.fsr31_enabled) {
    vkr_vk_prepare_fsr31_history(renderer, prepared, output);
  } else {
    VkrVulkanGraphImage *colors =
        vkr_vk_temporal_graph_image(renderer, "temporal_history_color");
    VkrVulkanGraphImage *depths =
        vkr_vk_temporal_graph_image(renderer, "temporal_history_depth");
    VkrVulkanGraphImage *identities =
        vkr_vk_temporal_graph_image(renderer, "temporal_history_identity");
    VkrVulkanGraphImage *surfaces =
        vkr_vk_temporal_graph_image(renderer, "temporal_history_surface");
    if (!instances || !output || !colors || !depths || !identities ||
        !surfaces ||
        !vkr_rg_buffer_handle_valid(
            renderer->temporal_transform_history_handle) ||
        colors->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
        depths->instance_count != colors->instance_count ||
        identities->instance_count != colors->instance_count ||
        surfaces->instance_count != colors->instance_count)
      return false_v;

    const uint32_t current = renderer->history_output_index;
    slot->temporal_transform_output = output;
    slot->temporal_color_output = &colors->instances[current];
    slot->temporal_depth_output = &depths->instances[current];
    slot->temporal_identity_output = &identities->instances[current];
    slot->temporal_surface_output = &surfaces->instances[current];

    VkrVulkanGraphImageInstance *selected = NULL;
    uint32_t selected_index = UINT32_MAX;
    if (packet->temporal.history_valid) {
      for (uint32_t i = 0u; i < colors->instance_count; ++i) {
        VkrVulkanGraphImageInstance *candidate = &colors->instances[i];
        if (i == current || !candidate->history_valid ||
            candidate->history_frame_index + 1u !=
                packet->input.frame.frame_index ||
            candidate->history_scene_generation !=
                packet->input.frame.scene_generation ||
            candidate->history_width !=
                renderer->prepared_frame.viewport_width ||
            candidate->history_height !=
                renderer->prepared_frame.viewport_height)
          continue;
        if (!selected || candidate->history_producer_submit_value >
                             selected->history_producer_submit_value) {
          selected = candidate;
          selected_index = i;
        }
      }
    } else if ((renderer->prepared_frame.ssr_enabled ||
                renderer->prepared_frame.ssgi_enabled ||
                packet->motion_blur_enabled) &&
               packet->temporal.reset_reasons == VKR_TEMPORAL_RESET_NONE) {
      VkrVulkanGraphBuffer *transforms =
          &renderer
               ->graph_buffers[renderer->temporal_transform_history_handle.id -
                               1u];
      for (uint32_t i = 0u; i < colors->instance_count; ++i) {
        VkrVulkanGraphImageInstance *candidate = &colors->instances[i];
        if (i == current || !candidate->history_valid ||
            candidate->history_producer_submit_value >
                renderer->completed_value ||
            candidate->history_frame_index >= packet->input.frame.frame_index ||
            packet->input.frame.frame_index - candidate->history_frame_index >
                colors->instance_count ||
            candidate->history_scene_generation !=
                packet->input.frame.scene_generation ||
            candidate->history_width !=
                renderer->prepared_frame.viewport_width ||
            candidate->history_height !=
                renderer->prepared_frame.viewport_height ||
            i >= transforms->instance_count)
          continue;
        VkrVulkanGraphBufferInstance *previous = &transforms->instances[i];
        if (!previous->history_valid ||
            previous->history_producer_submit_value !=
                candidate->history_producer_submit_value ||
            previous->history_frame_index != candidate->history_frame_index)
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
      VkrVulkanGraphImageInstance *previous_surface =
          &surfaces->instances[selected_index];
      if (!previous_transform->history_valid ||
          (packet->temporal.enabled &&
           (!selected->has_sampled_slot || !previous_depth->has_storage_slot ||
            !previous_identity->has_storage_slot ||
            !previous_surface->has_storage_slot))) {
        selected = NULL;
      } else {
        /* The preceding submission may still be in flight. All render work
           uses this queue; these dependencies order its writes before every
           history reader. Output reuse still requires completed last use. */
        const VkBufferMemoryBarrier2 buffer_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = previous_transform->buffer.handle,
            .size = VK_WHOLE_SIZE,
        };
        VkImageMemoryBarrier2 image_barriers[4] = {0};
        VkrVulkanGraphImageInstance *history_images[] = {
            selected, previous_depth, previous_identity, previous_surface};
        if (packet->temporal.enabled)
          for (uint32_t i = 0u; i < ArrayCount(history_images); ++i) {
            image_barriers[i] = (VkImageMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    (i == 0u ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0u),
                .srcAccessMask =
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                    (i == 0u ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0u),
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
        if (packet->temporal.enabled) {
          MemCopy(prepared->image_barriers, image_barriers,
                  sizeof(image_barriers));
          prepared->image_barrier_count = ArrayCount(image_barriers);
        }
        prepared->buffer_barrier = buffer_barrier;
        prepared->has_buffer_barrier = true_v;
        slot->temporal_transform_input = previous_transform;
        slot->temporal_previous_view_projection =
            selected->history_view_projection;
        slot->temporal_previous_frame_index = selected->history_frame_index;
        slot->temporal_history_valid = true_v;
        /* SSR and SSGI consume this transform predecessor even when final TAA
         * is disabled. Keep final TAA's color tuple absent in that mode so its
         * resolve remains current-frame-only. */
        if (packet->temporal.enabled) {
          slot->temporal_color_input = selected;
          slot->temporal_depth_input = previous_depth;
          slot->temporal_identity_input = previous_identity;
          slot->temporal_surface_input = previous_surface;
        }
      }
    }
  }

  const VkrVulkanTemporalTransformRoot root = {
      .instances = instances->buffer.address,
      .transforms = output->buffer.address,
      .instance_count = slot->gpu_candidate_count,
      .transform_capacity = VKR_TEMPORAL_TRANSFORM_CAPACITY,
      .frame_index = packet->input.frame.frame_index,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanTemporalTransformRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_TRANSFORM];
  if (root.instance_count) {
    prepared->groups[prepared->dispatch_count][0] =
        (root.instance_count + 63u) / 64u;
    prepared->groups[prepared->dispatch_count][1] = 1u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

void vkr_vk_mark_temporal_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImageInstance *inputs[] = {
      slot->temporal_color_input, slot->temporal_depth_input,
      slot->temporal_identity_input, slot->temporal_surface_input};
  for (uint32_t i = 0u; i < ArrayCount(inputs); ++i)
    if (inputs[i])
      inputs[i]->last_use_submit_value = submit_value;
  if (slot->temporal_transform_input)
    slot->temporal_transform_input->last_use_submit_value = submit_value;

  const VkrPreparedFrame *packet = renderer->graph->packet;
  VkrVulkanGraphImageInstance *outputs[] = {
      slot->temporal_color_output, slot->temporal_depth_output,
      slot->temporal_identity_output, slot->temporal_surface_output};
  if (packet->temporal.reset_reasons != 0u) {
    /* Retire the old logical epoch only after this reset was submitted. GPU
       last-use values still own physical image reuse. */
    VkrVulkanGraphImage *colors =
        vkr_vk_temporal_graph_image(renderer, "temporal_history_color");
    if (colors)
      for (uint32_t i = 0u; i < colors->instance_count; ++i)
        colors->instances[i].history_valid = false_v;
  }
  for (uint32_t i = 0u; i < ArrayCount(outputs); ++i) {
    VkrVulkanGraphImageInstance *instance = outputs[i];
    if (!instance)
      continue;
    instance->history_producer_submit_value = submit_value;
    instance->history_frame_index = packet->input.frame.frame_index;
    instance->history_scene_generation = packet->input.frame.scene_generation;
    instance->history_view_projection =
        packet->temporal.current_view_projection;
    instance->history_width = instance->image.width;
    instance->history_height = instance->image.height;
    instance->history_valid = true_v;
  }
  renderer->motion_seconds += packet->motion_blur_delta_seconds;
  if (packet->temporal.reset_reasons != 0u &&
      vkr_rg_buffer_handle_valid(renderer->temporal_transform_history_handle)) {
    VkrVulkanGraphBuffer *transforms = &renderer->graph_buffers[
        renderer->temporal_transform_history_handle.id - 1u];
    for (uint32_t i = 0u; i < transforms->instance_count; ++i)
      transforms->instances[i].history_motion_valid = false_v;
  }
  if (slot->temporal_transform_output) {
    slot->temporal_transform_output->history_motion_seconds = renderer->motion_seconds;
    slot->temporal_transform_output->history_motion_valid = true_v;
    slot->temporal_transform_output->history_producer_submit_value =
        submit_value;
    slot->temporal_transform_output->history_frame_index =
        packet->input.frame.frame_index;
    slot->temporal_transform_output->history_scene_generation =
        packet->input.frame.scene_generation;
    slot->temporal_transform_output->history_valid = true_v;
  }
  if (slot->temporal_color_output)
    slot->temporal_color_output->history_scene = slot->temporal_scene;
  if (slot->fsr31_recorded) {
    renderer->fsr31_history = (VkrVulkanFsr31History){
        .view_projection = packet->temporal.current_view_projection,
        .scene = slot->temporal_scene,
        .frame_index = packet->input.frame.frame_index,
        .scene_generation = packet->input.frame.scene_generation,
        .submit_value = submit_value,
        .history_index = renderer->history_output_index,
        .valid = true_v,
    };
    renderer->fsr31_dispatch_uses[renderer->fsr31_dispatch_slot] = submit_value;
    renderer->fsr31_dispatch_slot =
        (renderer->fsr31_dispatch_slot + 1u) % VKR_VULKAN_FRAME_SLOT_COUNT;
    slot->fsr31_recorded = false_v;
  }
}

bool8_t vkr_vk_prepare_deferred_cull(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass,
                                     VkrVulkanDeferredPipeline pipeline,
                                     bool8_t transmission) {
  VkrVulkanCullRoot root = {0};
  if (!vkr_vk_deferred_cull_root(renderer, prepared, pass, pipeline,
                                 transmission, &root, &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[pipeline];
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_PREFIX) {
    {
      prepared->groups[prepared->dispatch_count][0] =
          (root.view_count + VKR_VULKAN_DEFERRED_PREFIX_GROUP_SIZE - 1u) /
          VKR_VULKAN_DEFERRED_PREFIX_GROUP_SIZE;
      prepared->groups[prepared->dispatch_count][1] = 1u;
      prepared->groups[prepared->dispatch_count][2] = 1u;
      prepared->dispatch_count++;
    }
  } else if (root.candidate_count) {
    prepared->groups[prepared->dispatch_count][0] =
        (root.candidate_count + 63u) / 64u;
    prepared->groups[prepared->dispatch_count][1] = root.view_count;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_raster(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedRaster *prepared,
                                       const VkrRgPass *pass, bool8_t shadow,
                                       bool8_t transmission,
                                       bool8_t local_shadow) {
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
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const uint32_t layer = pass->desc.depth_attachment.desc.slice.base_layer;
  const uint32_t view_index =
      shadow ? 1u + layer +
                   (local_shadow ? renderer->prepared_frame.shadow_cascade_count
                                 : 0u)
             : 0u;
  const Mat4 view_projection =
      local_shadow
          ? packet->input.local_shadow->views[layer].light_view_projection
      : shadow ? packet->input.shadow->cascades[layer].light_view_projection
               : mat4_mul(packet->temporal.jittered_projection,
                          packet->input.globals.view);
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
      VKR_VULKAN_SENTINEL_SLOT_INDEX, VKR_VULKAN_SENTINEL_SLOT_INDEX, false_v);
  VkrVulkanRasterRoot root = {
      .geometry_rows = slot->gpu_geometry_rows,
      .visible_rows = visible->buffer.address,
      .states = states->buffer.address,
      .frame = frame_address,
      .view_index = view_index,
      .visible_capacity =
          transmission
              ? renderer->prepared_frame.transmission_gpu_draw_visible_capacity
              : renderer->prepared_frame.gpu_draw_visible_capacity,
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
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanRasterRoot), &root_address))
    return false_v;
  // Opaque camera buckets can use the discard-free visibility fragment. A
  // transmission peel cannot because every bucket may discard against the
  // preceding layer's depth.
  const bool8_t early_z_opaque =
      !shadow && root.previous_depth_texture == UINT32_MAX;
  prepared->indices = renderer->geometry_megabuffer.indices.handle;
  prepared->arguments = commands->buffer.handle;
  prepared->counts = states->buffer.handle;
  prepared->root_address = root_address;
  prepared->command_partition_capacity =
      root.visible_capacity / VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    const bool8_t opaque_bucket = (bucket & 2u) == 0u;
    const VkrVulkanPacketPipeline wanted_pipeline =
        shadow ? (opaque_bucket
                      ? VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW_OPAQUE
                      : VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW)
        : (early_z_opaque && opaque_bucket)
            ? VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE
            : VKR_VULKAN_PACKET_PIPELINE_VISIBILITY;
    prepared->pipelines[bucket] = renderer->packet_pipelines[wanted_pipeline];
    prepared->cull_modes[bucket] =
        (bucket & 1u) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    prepared->front_faces[bucket] = (bucket & VKR_GPU_DRAW_STATE_MIRRORED_BIT)
                                        ? VK_FRONT_FACE_CLOCKWISE
                                        : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const VkDeviceSize argument_offset =
        ((VkDeviceSize)view_index * VKR_WORLD_DRAW_STATE_BUCKET_COUNT +
         bucket) *
        prepared->command_partition_capacity *
        sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize count_offset =
        (VkDeviceSize)view_index * sizeof(VkrGpuDrawCompactionState) +
        offsetof(VkrGpuDrawCompactionState, bucket_counts) +
        bucket * sizeof(uint32_t);
    prepared->argument_offsets[bucket] = argument_offset;
    prepared->count_offsets[bucket] = count_offset;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_gbuffer(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 7u);
  uint32_t indices[10] = {0};
  if (!visible || !state || !vkr_vk_deferred_buffer(renderer, pass, 10u))
    return false_v;
  const uint32_t bindings[] = {0u, 2u, 3u, 4u, 8u, 11u, 12u, 13u, 14u, 15u};
  for (uint32_t i = 0u; i < ArrayCount(bindings); ++i)
    if (!vkr_vk_deferred_storage_index(renderer, pass, bindings[i],
                                       &indices[i]))
      return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const bool8_t emissive_capture =
      packet->input.debug &&
      vkr_renderer_capture_request_contains(packet->input.debug->capture,
                                            "deferred_emissive");
  const bool8_t resolve_debug_capture =
      packet->input.debug &&
      vkr_renderer_capture_request_contains(packet->input.debug->capture,
                                            "resolve_barycentric_lod");
  /* The selected no-capture modules compile out every access through these
     fields; slot zero is never an optional-output write target. */
  uint32_t emissive_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t debug_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if ((emissive_capture &&
       !vkr_vk_deferred_storage_index(renderer, pass, 5u, &emissive_texture)) ||
      (resolve_debug_capture &&
       !vkr_vk_deferred_storage_index(renderer, pass, 6u, &debug_texture)))
    return false_v;
  slot->motion_blur_interval_scale = 0.0f;
  if (packet->motion_blur_enabled && packet->temporal.reset_reasons == 0u &&
      slot->temporal_history_valid &&
      slot->temporal_transform_input->history_motion_valid) {
    const float64_t elapsed = renderer->motion_seconds +
        packet->motion_blur_delta_seconds -
        slot->temporal_transform_input->history_motion_seconds;
    if (elapsed > 0.0)
      slot->motion_blur_interval_scale =
          (float32_t)(packet->motion_blur_delta_seconds / elapsed);
  }
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
      .view_projection = mat4_mul(packet->temporal.jittered_projection,
                                  packet->input.globals.view),
      .current_view_projection = packet->temporal.current_view_projection,
      .previous_view_projection =
          slot->temporal_history_valid
              ? slot->temporal_previous_view_projection
              : packet->temporal.current_view_projection,
      .vbuffer_texture = indices[0],
      .albedo_texture = indices[1],
      .specular_texture = indices[2],
      .normal_texture = indices[3],
      .emissive_texture = emissive_texture,
      .debug_texture = debug_texture,
      .scene_texture = indices[4],
      .motion_texture = indices[5],
      .validity_texture = indices[6],
      .clearcoat_texture = indices[7],
      .sheen_texture = indices[8],
      .anisotropy_texture = indices[9],
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .visible_capacity = renderer->prepared_frame.gpu_draw_visible_capacity,
      .geometry_count = renderer->config.geometry_capacity,
      .material_count = renderer->config.material_slot_capacity,
      .instance_count = slot->gpu_candidate_count,
      .render_mode = packet->input.globals.render_mode,
      .history_valid = slot->temporal_history_valid,
      .previous_frame_index = slot->temporal_history_valid
                                  ? slot->temporal_previous_frame_index
                                  : packet->input.frame.frame_index,
      .sky_reprojection = slot->temporal_history_valid
                              ? vkr_temporal_sky_reprojection(
                                    packet->temporal.current_view_projection,
                                    slot->temporal_previous_view_projection,
                                    packet->input.globals.view_position)
                              : mat4_identity(),
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanResolveRoot),
                                 &prepared->root_address))
    return false_v;
  const VkrVulkanDeferredPipeline pipeline =
      emissive_capture
          ? (resolve_debug_capture
                 ? VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_EMISSIVE_DEBUG
                 : VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_EMISSIVE)
          : (resolve_debug_capture ? VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_DEBUG
                                   : VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_NONE);
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[pipeline];
  {
    prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_lighting(VkrVulkanRenderer *renderer,
                                         VkrVulkanPreparedCompute *prepared,
                                         const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint32_t vbuffer = 0u, depth = 0u, albedo = 0u, specular = 0u, normal = 0u,
           scene = 0u, clearcoat = 0u, sheen = 0u, anisotropy = 0u;
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 13u);
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u, &albedo) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 3u, &specular) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &normal) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 6u, &scene) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 10u, &clearcoat) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 11u, &sheen) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 12u, &anisotropy) ||
      !visible)
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.shadow_cascade_count > 0u &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 5u, &shadow_texture))
    return false_v;
  uint32_t local_shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.local_shadow_view_count > 0u &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 8u, &local_shadow_texture))
    return false_v;
  uint32_t gtao_visibility = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 7u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 7u, &gtao_visibility))
    return false_v;
  uint32_t direct_source = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.ssgi_enabled &&
      !vkr_vk_deferred_storage_index(renderer, pass, 9u, &direct_source))
    return false_v;
  /* A cubemap still publishing its initial upload is not sampled this frame;
     the fallback colour matches the authored forward skybox clear. */
  uint32_t sky_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t sky_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t sky_enabled = 0u;
  VkrVulkanPublishedTexture *sky =
      packet->input.skybox ? vkr_vk_published_texture(
                                 renderer, packet->input.skybox->cubemap, NULL)
                           : NULL;
  if (packet->input.skybox && !sky)
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
                                local_shadow_texture, true_v);
  uint32_t subsurface_source = scene;
  if (packet->subsurface_enabled &&
      !vkr_vk_deferred_storage_index(renderer, pass, 14u, &subsurface_source))
    return false_v;
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
      .solar_disk_radiance =
          packet->input.skybox
              ? (Vec4){packet->input.skybox->solar_disk_radiance.x,
                       packet->input.skybox->solar_disk_radiance.y,
                       packet->input.skybox->solar_disk_radiance.z, 0.0f}
              : (Vec4){0},
      .direct_source_texture = direct_source,
      .ssgi_enabled = renderer->prepared_frame.ssgi_enabled,
      .clearcoat_texture = clearcoat,
      .sheen_texture = sheen,
      .anisotropy_texture = anisotropy,
      .visible_rows = visible->buffer.address,
      .subsurface_source_texture = subsurface_source,
      .subsurface_profile_count = packet->subsurface_enabled ? packet->subsurface.dimensions[2] : 0u,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanLightingRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_LIGHTING];
  {
    prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_temporal_resolve(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
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

  const VkrPreparedFrame *packet = renderer->graph->packet;
  const bool8_t history_valid =
      packet->temporal.enabled && slot->temporal_history_valid &&
      slot->temporal_color_input && slot->temporal_depth_input &&
      slot->temporal_identity_input && slot->temporal_surface_input;
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
      .history_surface_texture =
          history_valid ? slot->temporal_surface_input->storage_slot.index
                        : storage[4],
      .output_color_texture = storage[1],
      .output_depth_texture = storage[2],
      .output_identity_texture = storage[3],
      .output_surface_texture = storage[4],
      .history_sampler = renderer->transmission_sampler_slot,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .history_valid = history_valid,
      .render_mode = packet->input.globals.render_mode,
      .camera_stationary =
          history_valid && MemCompare(&slot->temporal_previous_view_projection,
                                      &packet->temporal.current_view_projection,
                                      sizeof(Mat4)) == 0,
      .scene_stationary =
          history_valid && vkr_vk_temporal_scene_equal(
                               &slot->temporal_scene,
                               &slot->temporal_color_input->history_scene),
      .transmission_visible_rows =
          transmission_visible ? transmission_visible->buffer.address : 0u,
      .transmission_instances =
          transmission_instances ? transmission_instances->buffer.address : 0u,
      .transmission_vbuffer_texture = transmission_vbuffer,
      .transmission_depth_texture = transmission_depth,
      .transmission_enabled = transmission_enabled,
      .current_jitter_pixels = packet->temporal.jitter_pixels,
      .previous_jitter_pixels =
          history_valid ? vkr_temporal_jitter_for_frame(
                              (uint32_t)slot->temporal_previous_frame_index)
                        : (Vec2){0},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanTemporalResolveRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_RESOLVE];
  {
    prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_hzb(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
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
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanHzbRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_HZB];
  {
    prepared->groups[prepared->dispatch_count][0] =
        (root.destination_extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] =
        (root.destination_extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

vkr_internal VkrSsrGpuParams vkr_vk_ssr_params(VkrVulkanRenderer *renderer,
                                               bool8_t history_valid,
                                               Mat4 previous_projection) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 projection = packet->temporal.jittered_projection;
  VkrSsrGpuParams params = vkr_ssr_gpu_params(
      &renderer->ssr_config, projection, mat4_inverse(projection),
      packet->input.globals.view, previous_projection,
      renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height, history_valid);
  if (history_valid && packet->temporal.enabled) {
    const VkrVulkanFrameSlot *slot =
        &renderer->frame_slots[renderer->active_frame_slot];
    const uint32_t phase_count =
        renderer->prepared_frame.fsr31_enabled
            ? vkr_temporal_upscale_sequence_length(
                  renderer->prepared_frame.viewport_width,
                  renderer->prepared_frame.scene_output_width)
            : VKR_TEMPORAL_SEQUENCE_LENGTH;
    const Vec2 previous_jitter = vkr_temporal_jitter_for_frame_phases(
        (uint32_t)slot->ssr_color_input->history_frame_index, phase_count);
    params.history_jitter_uv_x =
        (previous_jitter.x - packet->temporal.jitter_pixels.x) /
        (float32_t)params.source_width;
    params.history_jitter_uv_y =
        (previous_jitter.y - packet->temporal.jitter_pixels.y) /
        (float32_t)params.source_height;
  }
  return params;
}

bool8_t vkr_vk_prepare_ssr_depth_base(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass) {
  uint32_t depth = 0u, vbuffer = 0u, destination = 0u, receiver = 0u;
  if (!vkr_vk_deferred_sampled_index(renderer, pass, 0u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &vbuffer) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u, &destination) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 3u, &receiver))
    return false_v;
  const VkrSsrGpuParams params = vkr_vk_ssr_params(
      renderer, false_v, renderer->graph->packet->temporal.jittered_projection);
  if (!params.trace_width || !params.trace_height)
    return false_v;
  const VkrVulkanSsrDepthBaseRoot root = {
      .params = params,
      .depth_texture = depth,
      .vbuffer_texture = vbuffer,
      .destination_depth_texture = destination,
      .receiver_texture = receiver,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsrDepthBaseRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSR_DEPTH_BASE];
  prepared->groups[0][0] = (params.trace_width + 7u) / 8u;
  prepared->groups[0][1] = (params.trace_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_ssr_depth_mip(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass) {
  const VkrRgImageUse *source_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  VkrVulkanGraphImageInstance *source =
      source_use ? vkr_vk_deferred_image(renderer, source_use->image) : NULL;
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  uint32_t source_texture = 0u, destination_texture = 0u;
  if (!source || !destination ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &source_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 1u, &destination_texture))
    return false_v;
  const uint32_t source_mip =
      source_use->has_slice ? source_use->slice.mip_level : 0u;
  const uint32_t destination_mip =
      destination_use->has_slice ? destination_use->slice.mip_level : 0u;
  const VkrVulkanSsrDepthMipRoot root = {
      .source_depth_texture = source_texture,
      .destination_depth_texture = destination_texture,
      .source_extent = {Max(1u, source->image.width >> source_mip),
                        Max(1u, source->image.height >> source_mip)},
      .destination_extent = {Max(1u,
                                 destination->image.width >> destination_mip),
                             Max(1u,
                                 destination->image.height >> destination_mip)},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsrDepthMipRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSR_DEPTH_MIP];
  prepared->groups[0][0] = (root.destination_extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.destination_extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_ssr_trace(VkrVulkanRenderer *renderer,
                                 VkrVulkanPreparedCompute *prepared,
                                 const VkrRgPass *pass) {
  uint32_t textures[9] = {0};
  for (uint32_t binding = 0u; binding < 7u; ++binding)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, binding,
                                       &textures[binding]))
      return false_v;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 7u, &textures[7]))
    return false_v;
  if (!vkr_vk_deferred_sampled_index(renderer, pass, 8u, &textures[8]))
    return false_v;
  const VkrSsrGpuParams params = vkr_vk_ssr_params(
      renderer, false_v, renderer->graph->packet->temporal.jittered_projection);
  if (params.depth_mip_count != renderer->prepared_frame.ssr_depth_mip_count)
    return false_v;
  const VkrVulkanSsrTraceRoot root = {
      .params = params,
      .depth_texture = textures[0],
      .vbuffer_texture = textures[1],
      .normal_texture = textures[2],
      .specular_texture = textures[3],
      .depth_pyramid_texture = textures[4],
      .receiver_texture = textures[5],
      .source_texture = textures[6],
      .destination_texture = textures[7],
      .clearcoat_texture = textures[8],
      .source_sampler = renderer->transmission_sampler_slot,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsrTraceRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSR_TRACE];
  prepared->groups[0][0] = (params.trace_width + 7u) / 8u;
  prepared->groups[0][1] = (params.trace_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

vkr_internal bool8_t
vkr_vk_ssr_history_scene_equal(const VkrVulkanTemporalSceneState *current,
                               const VkrVulkanTemporalSceneState *previous) {
  return current->signature.eligible && previous->signature.eligible &&
         current->signature.hash[0] == previous->signature.hash[0] &&
         current->signature.hash[1] == previous->signature.hash[1] &&
         current->radiance_revision == previous->radiance_revision &&
         current->publication_generation == previous->publication_generation &&
         current->graph_revision == previous->graph_revision;
}

vkr_internal bool8_t vkr_vk_prepare_ssr_history(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    VkrVulkanGraphImage **out_colors, VkrVulkanGraphImage **out_depths,
    VkrVulkanGraphImage **out_identities, Mat4 *out_previous_projection) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImage *colors =
      vkr_vk_temporal_graph_image(renderer, "ssr_history_color");
  VkrVulkanGraphImage *depths =
      vkr_vk_temporal_graph_image(renderer, "ssr_history_depth");
  VkrVulkanGraphImage *identities =
      vkr_vk_temporal_graph_image(renderer, "ssr_history_identity");
  if (!colors || !depths || !identities ||
      colors->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
      depths->instance_count != colors->instance_count ||
      identities->instance_count != colors->instance_count)
    return false_v;
  const uint32_t current = renderer->history_output_index;
  if (current >= colors->instance_count)
    return false_v;
  slot->ssr_color_output = &colors->instances[current];
  slot->ssr_depth_output = &depths->instances[current];
  slot->ssr_identity_output = &identities->instances[current];
  *out_previous_projection =
      renderer->graph->packet->temporal.jittered_projection;

  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrVulkanTemporalSceneState scene = {
      .signature = vkr_ssr_content_signature(packet),
      .radiance_revision = renderer->radiance_revision,
      .publication_generation = renderer->candidate_publication_generation,
      .graph_revision = renderer->graph_revision,
  };
  if (packet->temporal.reset_reasons != VKR_TEMPORAL_RESET_NONE ||
      !scene.signature.eligible || !slot->temporal_history_valid ||
      !slot->temporal_transform_input)
    goto selected;

  VkrVulkanGraphImageInstance *selected = NULL;
  uint32_t selected_index = UINT32_MAX;
  for (uint32_t i = 0u; i < colors->instance_count; ++i) {
    VkrVulkanGraphImageInstance *candidate = &colors->instances[i];
    if (i == current || !candidate->history_valid ||
        candidate->history_producer_submit_value !=
            slot->temporal_transform_input->history_producer_submit_value ||
        candidate->history_frame_index != slot->temporal_previous_frame_index ||
        candidate->history_scene_generation !=
            packet->input.frame.scene_generation ||
        candidate->history_width != slot->ssr_color_output->image.width ||
        candidate->history_height != slot->ssr_color_output->image.height ||
        !vkr_vk_ssr_history_scene_equal(&scene, &candidate->history_scene))
      continue;
    VkrVulkanGraphImageInstance *depth = &depths->instances[i];
    VkrVulkanGraphImageInstance *identity = &identities->instances[i];
    if (!depth->history_valid || !identity->history_valid ||
        depth->history_producer_submit_value !=
            candidate->history_producer_submit_value ||
        identity->history_producer_submit_value !=
            candidate->history_producer_submit_value ||
        depth->history_frame_index != candidate->history_frame_index ||
        identity->history_frame_index != candidate->history_frame_index ||
        depth->history_width != candidate->history_width ||
        depth->history_height != candidate->history_height ||
        identity->history_width != candidate->history_width ||
        identity->history_height != candidate->history_height ||
        !candidate->has_sampled_slot || !depth->has_sampled_slot ||
        !identity->has_storage_slot)
      continue;
    if (!selected || candidate->history_producer_submit_value >
                         selected->history_producer_submit_value) {
      selected = candidate;
      selected_index = i;
    }
  }
  if (selected) {
    /* The matching temporal producer may still be in flight. Its transform
       dependency and these same-queue image barriers order every history read;
       output reuse continues to require completed producer and reader uses. */
    VkrVulkanGraphImageInstance *depth = &depths->instances[selected_index];
    VkrVulkanGraphImageInstance *identity =
        &identities->instances[selected_index];
    const VkImageMemoryBarrier2 color_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = selected->image.handle,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1u,
                             .layerCount = 1u},
    };
    const VkImageMemoryBarrier2 depth_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth->image.handle,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1u,
                             .layerCount = 1u},
    };
    const VkImageMemoryBarrier2 identity_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = identity->image.handle,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1u,
                             .layerCount = 1u},
    };
    prepared->image_barriers[0] = color_barrier;
    prepared->image_barriers[1] = depth_barrier;
    prepared->image_barriers[2] = identity_barrier;
    prepared->image_barrier_count = 3u;
    slot->ssr_color_input = selected;
    slot->ssr_depth_input = depth;
    slot->ssr_identity_input = identity;
    slot->ssr_history_valid = true_v;
    *out_previous_projection = selected->history_projection;
  }
selected:
  *out_colors = colors;
  *out_depths = depths;
  *out_identities = identities;
  return true_v;
}

bool8_t vkr_vk_prepare_ssr_temporal(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImage *colors = NULL, *depths = NULL, *identities = NULL;
  Mat4 previous_projection;
  if (!vkr_vk_prepare_ssr_history(renderer, prepared, &colors, &depths,
                                  &identities, &previous_projection))
    return false_v;
  uint32_t sampled[7] = {0};
  for (uint32_t binding = 0u; binding < ArrayCount(sampled); ++binding)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, binding,
                                       &sampled[binding]))
      return false_v;
  uint32_t output_color = 0u, output_depth = 0u, output_identity = 0u,
           specular = 0u, clearcoat = 0u, albedo = 0u, sheen = 0u, anisotropy = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 10u, &output_color) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 11u, &output_depth) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 12u, &output_identity) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 15u, &specular) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 16u, &clearcoat) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 17u, &albedo) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 19u, &sheen) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 20u, &anisotropy))
    return false_v;
  uint32_t gtao = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 18u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 18u, &gtao))
    return false_v;
  uint64_t frame_address = 0u;
  if (!vkr_vk_packet_frame_root(slot, &frame_address))
    return false_v;
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 13u);
  VkrVulkanGraphBufferInstance *instances =
      vkr_vk_deferred_buffer(renderer, pass, 14u);
  if (!visible || !instances)
    return false_v;
  const uint32_t history_color =
      slot->ssr_history_valid ? slot->ssr_color_input->sampled_slot.index
                              : slot->ssr_color_output->sampled_slot.index;
  const uint32_t history_depth =
      slot->ssr_history_valid ? slot->ssr_depth_input->sampled_slot.index
                              : slot->ssr_depth_output->sampled_slot.index;
  const uint32_t history_identity =
      slot->ssr_history_valid ? slot->ssr_identity_input->storage_slot.index
                              : slot->ssr_identity_output->storage_slot.index;
  const VkrSsrGpuParams params =
      vkr_vk_ssr_params(renderer, slot->ssr_history_valid, previous_projection);
  const VkrVulkanSsrTemporalRoot root = {
      .params = params,
      .visible_rows = visible->buffer.address,
      .instances = instances->buffer.address,
      .raw_texture = sampled[0],
      .receiver_texture = sampled[1],
      .vbuffer_texture = sampled[2],
      .depth_texture = sampled[3],
      .normal_texture = sampled[4],
      .motion_texture = sampled[5],
      .validity_texture = sampled[6],
      .history_color_texture = history_color,
      .history_depth_texture = history_depth,
      .history_identity_texture = history_identity,
      .output_color_texture = output_color,
      .output_depth_texture = output_depth,
      .output_identity_texture = output_identity,
      .linear_sampler = renderer->transmission_sampler_slot,
      .specular_texture = specular,
      .clearcoat_texture = clearcoat,
      .frame = frame_address,
      .albedo_texture = albedo,
      .gtao_visibility_texture = gtao,
      .sheen_texture = sheen,
      .anisotropy_texture = anisotropy,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsrTemporalRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSR_TEMPORAL];
  prepared->groups[0][0] = (params.source_width + 7u) / 8u;
  prepared->groups[0][1] = (params.source_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_ssr_composite(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass) {
  uint32_t scene = 0u, reflection = 0u, vbuffer = 0u, depth = 0u, albedo = 0u,
           specular = 0u, normal = 0u, clearcoat = 0u, sheen = 0u,
           anisotropy = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &scene) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &reflection) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 3u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 4u, &albedo) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 5u, &specular) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 6u, &normal) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 10u, &clearcoat) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 11u, &sheen) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 12u, &anisotropy))
    return false_v;
  uint32_t gtao = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 7u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 7u, &gtao))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t frame_address = 0u;
  if (!vkr_vk_packet_frame_root(slot, &frame_address))
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
  const VkrSsrGpuParams params = vkr_vk_ssr_params(
      renderer, false_v, packet->temporal.jittered_projection);
  const VkrVulkanSsrCompositeRoot root = {
      .params = params,
      .frame = frame_address,
      .inverse_view_projection = mat4_inverse(view_projection),
      .scene_texture = scene,
      .reflection_texture = reflection,
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .albedo_texture = albedo,
      .specular_texture = specular,
      .normal_texture = normal,
      .gtao_visibility_texture = gtao,
      .linear_sampler = renderer->transmission_sampler_slot,
      .clearcoat_texture = clearcoat,
      .sheen_texture = sheen,
      .anisotropy_texture = anisotropy,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsrCompositeRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSR_COMPOSITE];
  prepared->groups[0][0] = (params.source_width + 7u) / 8u;
  prepared->groups[0][1] = (params.source_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_fog_apply(VkrVulkanRenderer *renderer,
                                 VkrVulkanPreparedCompute *prepared,
                                 const VkrRgPass *pass) {
  uint32_t depth = 0u, target = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &target) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth))
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
  const VkrVulkanFogRoot root = {
      .params = packet->fog,
      .inverse_view_projection = mat4_inverse(view_projection),
      .camera_position = {packet->input.globals.view_position.x,
                          packet->input.globals.view_position.y,
                          packet->input.globals.view_position.z, 1.0f},
      .depth_texture = depth,
      .target_texture = target,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanFogRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_FOG_APPLY];
  prepared->groups[0][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

vkr_internal bool8_t vkr_vk_prepare_froxel_frame_root(
    VkrVulkanRenderer *renderer, const VkrRgPass *pass,
    uint64_t *out_frame_address) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t local_shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 2u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &shadow_texture))
    return false_v;
  if (vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 3u, &local_shadow_texture))
    return false_v;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, out_frame_address);
  if (!frame_root)
    return false_v;
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, slot->gpu_candidate_instances,
      packet->temporal.current_view_projection, shadow_texture,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, local_shadow_texture, true_v);
  return true_v;
}

vkr_internal VkrVulkanGraphImage *
vkr_vk_froxel_graph_image(VkrVulkanRenderer *renderer, const char *name) {
  VkrVulkanGraphImage *image = vkr_vk_temporal_graph_image(renderer, name);
  return image && image->live && image->desc.type == VKR_TEXTURE_TYPE_3D &&
                 image->desc.depth == VKR_FROXEL_FOG_DEPTH
             ? image
             : NULL;
}

vkr_internal uint32_t
vkr_vk_froxel_image_generation(VkrVulkanRenderer *renderer, const char *name) {
  VkrVulkanGraphImage *image = vkr_vk_temporal_graph_image(renderer, name);
  return image && image->live ? image->graph_generation : 0u;
}

vkr_internal uint32_t vkr_vk_froxel_shadow_valid_mask(
    const VkrPreparedFrame *packet, uint32_t retained_mask) {
  return retained_mask |
         (packet->input.shadow ? packet->input.shadow->cascade_render_mask
                               : 0u);
}

vkr_internal uint32_t vkr_vk_froxel_local_shadow_valid_mask(
    const VkrPreparedFrame *packet, uint32_t retained_mask) {
  return retained_mask |
         (packet->input.local_shadow ? packet->input.local_shadow->render_mask
                                     : 0u);
}

vkr_internal bool8_t vkr_vk_prepare_froxel_history(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    VkrVulkanGraphImage **out_history) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImage *history =
      vkr_vk_froxel_graph_image(renderer, "froxel_scattering_history");
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if (!history ||
      history->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
      !slot->froxel_fog_params)
    return false_v;
  const uint32_t current = renderer->history_output_index;
  if (current >= history->instance_count)
    return false_v;
  VkrVulkanGraphImageInstance *output = &history->instances[current];
  if (!output->has_sampled_slot || !output->has_storage_slot)
    return false_v;
  slot->froxel_history_output = output;

  const uint32_t dimensions[3] = {output->image.width, output->image.height,
                                  output->image.depth};
  const uint32_t shadow_generation =
      vkr_vk_froxel_image_generation(renderer, "shadow_map");
  const uint32_t local_shadow_generation =
      vkr_vk_froxel_image_generation(renderer, "local_shadow_map");
  VkrRetainedShadowToken shadow_token;
  VkrRetainedLocalShadowToken local_shadow_token;
  vkr_vulkan_renderer_retained_shadow_token(
      renderer, renderer->prepared_frame.image_index, &shadow_token);
  vkr_vulkan_renderer_retained_local_shadow_token(
      renderer, renderer->prepared_frame.image_index, &local_shadow_token);
  const uint32_t shadow_valid_mask =
      vkr_vk_froxel_shadow_valid_mask(packet, shadow_token.valid_layer_mask);
  const uint32_t local_shadow_valid_mask =
      vkr_vk_froxel_local_shadow_valid_mask(
          packet, local_shadow_token.valid_layer_mask);
  VkrVulkanGraphImageInstance *selected = NULL;
  uint32_t selected_index = UINT32_MAX;
  if (packet->temporal.reset_reasons == VKR_TEMPORAL_RESET_NONE) {
    for (uint32_t i = 0u; i < history->instance_count; ++i) {
      const VkrVulkanFroxelHistory *candidate = &renderer->froxel_histories[i];
      VkrVulkanGraphImageInstance *image = &history->instances[i];
      if (i == current || !candidate->valid || !image->history_valid ||
          candidate->producer_submit_value > renderer->completed_value ||
          candidate->producer_submit_value !=
              image->history_producer_submit_value ||
          candidate->signature != packet->froxel_fog_signature ||
          candidate->frame_index >= packet->input.frame.frame_index ||
          candidate->scattering_generation != history->graph_generation ||
          candidate->shadow_generation != shadow_generation ||
          candidate->local_shadow_generation != local_shadow_generation ||
          candidate->shadow_valid_layer_mask != shadow_valid_mask ||
          candidate->local_shadow_valid_layer_mask != local_shadow_valid_mask ||
          MemCompare(candidate->dimensions, dimensions, sizeof(dimensions)) !=
              0 ||
          !image->has_sampled_slot)
        continue;
      if (!selected || candidate->producer_submit_value >
                           renderer->froxel_histories[selected_index]
                               .producer_submit_value) {
        selected = image;
        selected_index = i;
      }
    }
  }

  VkrFroxelFogGpuParams *params = slot->froxel_fog_params;
  params->previous_view_projection = packet->temporal.current_view_projection;
  params->previous_view = packet->input.globals.view;
  if (selected) {
    const VkrVulkanFroxelHistory *metadata =
        &renderer->froxel_histories[selected_index];
    params->previous_view_projection = metadata->view_projection;
    params->previous_view = metadata->view;
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = selected->image.handle,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1u,
                             .layerCount = 1u},
    };
    prepared->image_barriers[0] = barrier;
    prepared->image_barrier_count = 1u;
    slot->froxel_history_input = selected;
    slot->froxel_history_valid = true_v;
  }
  *out_history = history;
  return true_v;
}

bool8_t vkr_vk_prepare_froxel_inject(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass) {
  VkrVulkanGraphImage *history = NULL;
  if (!vkr_vk_prepare_froxel_history(renderer, prepared, &history))
    return false_v;
  uint32_t output = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 1u, &output))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t frame = 0u;
  if (!vkr_vk_prepare_froxel_frame_root(renderer, pass, &frame))
    return false_v;
  const VkrVulkanGraphImageInstance *source = slot->froxel_history_valid
                                                  ? slot->froxel_history_input
                                                  : slot->froxel_history_output;
  const VkrVulkanFroxelInjectRoot root = {
      .frame = frame,
      .params = slot->froxel_fog,
      .history_texture = source->sampled_slot.index,
      .history_sampler = renderer->transmission_sampler_slot,
      .output_texture = output,
      .history_valid = slot->froxel_history_valid,
      .extent = {slot->froxel_fog_params->grid_dimensions_cell_pixels[0],
                 slot->froxel_fog_params->grid_dimensions_cell_pixels[1],
                 slot->froxel_fog_params->grid_dimensions_cell_pixels[2]},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanFroxelInjectRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_FROXEL_INJECT];
  prepared->groups[0][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[0][2] = (root.extent[2] + 3u) / 4u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_froxel_integrate(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass) {
  uint32_t scattering = 0u, integrated = 0u;
  if (!vkr_vk_deferred_sampled_index(renderer, pass, 0u, &scattering) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 1u, &integrated))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->froxel_fog_params)
    return false_v;
  slot->froxel_integrated_texture = integrated;
  uint64_t frame = 0u;
  if (!vkr_vk_prepare_froxel_frame_root(renderer, pass, &frame))
    return false_v;
  const VkrVulkanFroxelIntegrateRoot root = {
      .frame = frame,
      .params = slot->froxel_fog,
      .scattering_texture = scattering,
      .scattering_sampler = renderer->transmission_sampler_slot,
      .integrated_texture = integrated,
      .extent = {slot->froxel_fog_params->grid_dimensions_cell_pixels[0],
                 slot->froxel_fog_params->grid_dimensions_cell_pixels[1],
                 slot->froxel_fog_params->grid_dimensions_cell_pixels[2]},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanFroxelIntegrateRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_FROXEL_INTEGRATE];
  prepared->groups[0][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_froxel_apply(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  uint32_t target = 0u, depth = 0u, integrated = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &target) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &integrated))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->froxel_fog_params || integrated != slot->froxel_integrated_texture)
    return false_v;
  uint64_t frame = 0u;
  if (!vkr_vk_prepare_froxel_frame_root(renderer, pass, &frame))
    return false_v;
  const VkrVulkanFroxelApplyRoot root = {
      .frame = frame,
      .params = slot->froxel_fog,
      .depth_texture = depth,
      .integrated_texture = integrated,
      .integrated_sampler = renderer->transmission_sampler_slot,
      .target_texture = target,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanFroxelApplyRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_FROXEL_APPLY];
  prepared->groups[0][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

void vkr_vk_mark_froxel_submitted(VkrVulkanRenderer *renderer,
                                  uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (slot->froxel_history_input)
    slot->froxel_history_input->last_use_submit_value = submit_value;
  if (!slot->froxel_history_output || !slot->froxel_fog_params)
    return;
  const uint32_t current = renderer->history_output_index;
  if (current >= VKR_VULKAN_HISTORY_INSTANCE_COUNT)
    return;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const uint32_t dimensions[3] = {slot->froxel_history_output->image.width,
                                  slot->froxel_history_output->image.height,
                                  slot->froxel_history_output->image.depth};
  VkrRetainedShadowToken shadow_token;
  VkrRetainedLocalShadowToken local_shadow_token;
  vkr_vulkan_renderer_retained_shadow_token(
      renderer, renderer->prepared_frame.image_index, &shadow_token);
  vkr_vulkan_renderer_retained_local_shadow_token(
      renderer, renderer->prepared_frame.image_index, &local_shadow_token);
  const uint32_t shadow_valid_mask =
      vkr_vk_froxel_shadow_valid_mask(packet, shadow_token.valid_layer_mask);
  const uint32_t local_shadow_valid_mask =
      vkr_vk_froxel_local_shadow_valid_mask(
          packet, local_shadow_token.valid_layer_mask);
  VkrVulkanFroxelHistory *history = &renderer->froxel_histories[current];
  *history = (VkrVulkanFroxelHistory){
      .view_projection = packet->temporal.current_view_projection,
      .view = packet->input.globals.view,
      .signature = packet->froxel_fog_signature,
      .producer_submit_value = submit_value,
      .frame_index = packet->input.frame.frame_index,
      .dimensions = {dimensions[0], dimensions[1], dimensions[2]},
      .scattering_generation =
          vkr_vk_froxel_image_generation(renderer, "froxel_scattering_history"),
      .shadow_generation =
          vkr_vk_froxel_image_generation(renderer, "shadow_map"),
      .local_shadow_generation =
          vkr_vk_froxel_image_generation(renderer, "local_shadow_map"),
      .shadow_valid_layer_mask = shadow_valid_mask,
      .local_shadow_valid_layer_mask = local_shadow_valid_mask,
      .valid = true_v,
  };
  slot->froxel_history_output->history_producer_submit_value = submit_value;
  slot->froxel_history_output->history_valid = true_v;
}

void vkr_vk_mark_ssr_submitted(VkrVulkanRenderer *renderer,
                               uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImageInstance *inputs[] = {
      slot->ssr_color_input, slot->ssr_depth_input, slot->ssr_identity_input};
  for (uint32_t i = 0u; i < ArrayCount(inputs); ++i)
    if (inputs[i])
      inputs[i]->last_use_submit_value = submit_value;
  VkrVulkanGraphImageInstance *outputs[] = {slot->ssr_color_output,
                                            slot->ssr_depth_output,
                                            slot->ssr_identity_output};
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrVulkanTemporalSceneState scene = {
      .signature = vkr_ssr_content_signature(packet),
      .radiance_revision = renderer->radiance_revision,
      .publication_generation = renderer->candidate_publication_generation,
      .graph_revision = renderer->graph_revision,
  };
  for (uint32_t i = 0u; i < ArrayCount(outputs); ++i) {
    VkrVulkanGraphImageInstance *instance = outputs[i];
    if (!instance)
      continue;
    instance->history_producer_submit_value = submit_value;
    instance->history_frame_index = packet->input.frame.frame_index;
    instance->history_scene_generation = packet->input.frame.scene_generation;
    instance->history_projection = packet->temporal.jittered_projection;
    instance->history_width = instance->image.width;
    instance->history_height = instance->image.height;
    instance->history_scene = scene;
    instance->history_valid = true_v;
  }
}

vkr_internal VkrSsgiGpuParams vkr_vk_ssgi_params(VkrVulkanRenderer *renderer,
                                                 bool8_t history_valid,
                                                 Mat4 previous_projection) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 projection = packet->temporal.jittered_projection;
  return vkr_ssgi_gpu_params(&renderer->ssgi_config, projection,
                             mat4_inverse(projection),
                             packet->input.globals.view, previous_projection,
                             renderer->prepared_frame.viewport_width,
                             renderer->prepared_frame.viewport_height,
                             history_valid, packet->input.frame.frame_index);
}

bool8_t vkr_vk_prepare_ssgi_depth_base(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass) {
  uint32_t depth = 0u, vbuffer = 0u, destination = 0u;
  if (!vkr_vk_deferred_sampled_index(renderer, pass, 0u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &vbuffer) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u, &destination))
    return false_v;
  const VkrSsgiGpuParams params = vkr_vk_ssgi_params(
      renderer, false_v, renderer->graph->packet->temporal.jittered_projection);
  if (!params.trace_width || !params.trace_height)
    return false_v;
  const VkrVulkanSsgiDepthBaseRoot root = {
      .params = params,
      .depth_texture = depth,
      .vbuffer_texture = vbuffer,
      .destination_depth_texture = destination,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsgiDepthBaseRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSGI_DEPTH_BASE];
  prepared->groups[0][0] = (params.trace_width + 7u) / 8u;
  prepared->groups[0][1] = (params.trace_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_ssgi_depth_mip(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass) {
  const VkrRgImageUse *source_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  VkrVulkanGraphImageInstance *source =
      source_use ? vkr_vk_deferred_image(renderer, source_use->image) : NULL;
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  uint32_t source_texture = 0u, destination_texture = 0u;
  if (!source || !destination ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &source_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 1u, &destination_texture))
    return false_v;
  const uint32_t source_mip =
      source_use->has_slice ? source_use->slice.mip_level : 0u;
  const uint32_t destination_mip =
      destination_use->has_slice ? destination_use->slice.mip_level : 0u;
  const VkrVulkanSsgiDepthMipRoot root = {
      .source_depth_texture = source_texture,
      .destination_depth_texture = destination_texture,
      .source_extent = {Max(1u, source->image.width >> source_mip),
                        Max(1u, source->image.height >> source_mip)},
      .destination_extent = {Max(1u,
                                 destination->image.width >> destination_mip),
                             Max(1u,
                                 destination->image.height >> destination_mip)},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsgiDepthMipRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSGI_DEPTH_MIP];
  prepared->groups[0][0] = (root.destination_extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.destination_extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_ssgi_trace(VkrVulkanRenderer *renderer,
                                  VkrVulkanPreparedCompute *prepared,
                                  const VkrRgPass *pass) {
  uint32_t textures[7] = {0};
  for (uint32_t binding = 0u; binding < 6u; ++binding)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, binding,
                                       &textures[binding]))
      return false_v;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 6u, &textures[6]))
    return false_v;
  const VkrSsgiGpuParams params = vkr_vk_ssgi_params(
      renderer, false_v, renderer->graph->packet->temporal.jittered_projection);
  if (params.depth_mip_count != renderer->prepared_frame.ssgi_depth_mip_count)
    return false_v;
  const VkrVulkanSsgiTraceRoot root = {
      .params = params,
      .depth_texture = textures[0],
      .vbuffer_texture = textures[1],
      .normal_texture = textures[2],
      .albedo_texture = textures[3],
      .depth_pyramid_texture = textures[4],
      .direct_source_texture = textures[5],
      .destination_texture = textures[6],
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsgiTraceRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSGI_TRACE];
  prepared->groups[0][0] = (params.trace_width + 7u) / 8u;
  prepared->groups[0][1] = (params.trace_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

vkr_internal bool8_t vkr_vk_prepare_ssgi_history(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    VkrVulkanGraphImage **out_colors, VkrVulkanGraphImage **out_depths,
    VkrVulkanGraphImage **out_identities, Mat4 *out_previous_projection) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImage *colors =
      vkr_vk_temporal_graph_image(renderer, "ssgi_history_color");
  VkrVulkanGraphImage *depths =
      vkr_vk_temporal_graph_image(renderer, "ssgi_history_depth");
  VkrVulkanGraphImage *identities =
      vkr_vk_temporal_graph_image(renderer, "ssgi_history_identity");
  if (!colors || !depths || !identities ||
      colors->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
      depths->instance_count != colors->instance_count ||
      identities->instance_count != colors->instance_count)
    return false_v;
  const uint32_t current = renderer->history_output_index;
  if (current >= colors->instance_count)
    return false_v;
  slot->ssgi_color_output = &colors->instances[current];
  slot->ssgi_depth_output = &depths->instances[current];
  slot->ssgi_identity_output = &identities->instances[current];
  *out_previous_projection =
      renderer->graph->packet->temporal.jittered_projection;

  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrVulkanTemporalSceneState scene = {
      .signature = vkr_ssgi_content_signature(packet),
      .radiance_revision = renderer->radiance_revision,
      .publication_generation = renderer->candidate_publication_generation,
      .graph_revision = renderer->graph_revision,
  };
  const uint32_t dimensions[2] = {slot->ssgi_color_output->image.width,
                                  slot->ssgi_color_output->image.height};
  const uint32_t shadow_generation =
      vkr_vk_froxel_image_generation(renderer, "shadow_map");
  const uint32_t local_shadow_generation =
      vkr_vk_froxel_image_generation(renderer, "local_shadow_map");
  VkrRetainedShadowToken shadow_token;
  VkrRetainedLocalShadowToken local_shadow_token;
  vkr_vulkan_renderer_retained_shadow_token(
      renderer, renderer->prepared_frame.image_index, &shadow_token);
  vkr_vulkan_renderer_retained_local_shadow_token(
      renderer, renderer->prepared_frame.image_index, &local_shadow_token);
  const uint32_t shadow_valid_mask =
      vkr_vk_froxel_shadow_valid_mask(packet, shadow_token.valid_layer_mask);
  const uint32_t local_shadow_valid_mask =
      vkr_vk_froxel_local_shadow_valid_mask(
          packet, local_shadow_token.valid_layer_mask);
  if (packet->temporal.reset_reasons != VKR_TEMPORAL_RESET_NONE ||
      !scene.signature.eligible)
    goto selected;

  VkrVulkanGraphImageInstance *selected = NULL;
  uint32_t selected_index = UINT32_MAX;
  for (uint32_t i = 0u; i < colors->instance_count; ++i) {
    VkrVulkanGraphImageInstance *candidate = &colors->instances[i];
    const VkrVulkanSsgiHistory *metadata = &renderer->ssgi_histories[i];
    if (i == current || !metadata->valid || !candidate->history_valid ||
        metadata->producer_submit_value > renderer->completed_value ||
        metadata->producer_submit_value !=
            candidate->history_producer_submit_value ||
        !slot->temporal_history_valid ||
        candidate->history_frame_index != slot->temporal_previous_frame_index ||
        candidate->history_scene_generation !=
            packet->input.frame.scene_generation ||
        MemCompare(metadata->dimensions, dimensions, sizeof(dimensions)) != 0 ||
        metadata->shadow_generation != shadow_generation ||
        metadata->local_shadow_generation != local_shadow_generation ||
        metadata->shadow_valid_layer_mask != shadow_valid_mask ||
        metadata->local_shadow_valid_layer_mask != local_shadow_valid_mask ||
        !vkr_vk_ssr_history_scene_equal(&scene, &candidate->history_scene))
      continue;
    VkrVulkanGraphImageInstance *depth = &depths->instances[i];
    VkrVulkanGraphImageInstance *identity = &identities->instances[i];
    if (!depth->history_valid || !identity->history_valid ||
        depth->history_producer_submit_value !=
            candidate->history_producer_submit_value ||
        identity->history_producer_submit_value !=
            candidate->history_producer_submit_value ||
        depth->history_frame_index != candidate->history_frame_index ||
        identity->history_frame_index != candidate->history_frame_index ||
        !candidate->has_sampled_slot || !depth->has_sampled_slot ||
        !identity->has_storage_slot)
      continue;
    if (!selected ||
        metadata->producer_submit_value >
            renderer->ssgi_histories[selected_index].producer_submit_value) {
      selected = candidate;
      selected_index = i;
    }
  }
  if (selected) {
    VkrVulkanGraphImageInstance *depth = &depths->instances[selected_index];
    VkrVulkanGraphImageInstance *identity =
        &identities->instances[selected_index];
    prepared->image_barriers[0] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = selected->image.handle,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1u,
                             .layerCount = 1u},
    };
    prepared->image_barriers[1] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth->image.handle,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1u,
                             .layerCount = 1u},
    };
    prepared->image_barriers[2] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = identity->image.handle,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1u,
                             .layerCount = 1u},
    };
    prepared->image_barrier_count = 3u;
    slot->ssgi_color_input = selected;
    slot->ssgi_depth_input = depth;
    slot->ssgi_identity_input = identity;
    slot->ssgi_history_valid = true_v;
    *out_previous_projection = selected->history_projection;
  }
selected:
  *out_colors = colors;
  *out_depths = depths;
  *out_identities = identities;
  return true_v;
}

bool8_t vkr_vk_prepare_ssgi_temporal(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImage *colors = NULL, *depths = NULL, *identities = NULL;
  Mat4 previous_projection;
  if (!vkr_vk_prepare_ssgi_history(renderer, prepared, &colors, &depths,
                                   &identities, &previous_projection))
    return false_v;
  uint32_t sampled[6] = {0};
  for (uint32_t binding = 0u; binding < ArrayCount(sampled); ++binding)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, binding,
                                       &sampled[binding]))
      return false_v;
  uint32_t output_color = 0u, output_depth = 0u, output_identity = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 9u, &output_color) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 10u, &output_depth) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 11u, &output_identity))
    return false_v;
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 12u);
  VkrVulkanGraphBufferInstance *instances =
      vkr_vk_deferred_buffer(renderer, pass, 13u);
  if (!visible || !instances)
    return false_v;
  const uint32_t history_color =
      slot->ssgi_history_valid ? slot->ssgi_color_input->sampled_slot.index
                               : slot->ssgi_color_output->sampled_slot.index;
  const uint32_t history_depth =
      slot->ssgi_history_valid ? slot->ssgi_depth_input->sampled_slot.index
                               : slot->ssgi_depth_output->sampled_slot.index;
  const uint32_t history_identity =
      slot->ssgi_history_valid ? slot->ssgi_identity_input->storage_slot.index
                               : slot->ssgi_identity_output->storage_slot.index;
  const VkrSsgiGpuParams params = vkr_vk_ssgi_params(
      renderer, slot->ssgi_history_valid, previous_projection);
  const VkrVulkanSsgiTemporalRoot root = {
      .params = params,
      .visible_rows = visible->buffer.address,
      .instances = instances->buffer.address,
      .raw_texture = sampled[0],
      .vbuffer_texture = sampled[1],
      .depth_texture = sampled[2],
      .normal_texture = sampled[3],
      .motion_texture = sampled[4],
      .validity_texture = sampled[5],
      .history_color_texture = history_color,
      .history_depth_texture = history_depth,
      .history_identity_texture = history_identity,
      .output_color_texture = output_color,
      .output_depth_texture = output_depth,
      .output_identity_texture = output_identity,
      .linear_sampler = renderer->transmission_sampler_slot,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsgiTemporalRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSGI_TEMPORAL];
  prepared->groups[0][0] = (params.trace_width + 7u) / 8u;
  prepared->groups[0][1] = (params.trace_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_ssgi_composite(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass) {
  uint32_t scene = 0u, reflection = 0u, vbuffer = 0u, depth = 0u, albedo = 0u,
           normal = 0u, history_depth = 0u, specular = 0u, clearcoat = 0u,
           sheen = 0u, anisotropy = 0u;
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 11u);
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &scene) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &reflection) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 3u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 4u, &albedo) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 5u, &normal) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 6u, &history_depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 7u, &specular) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 8u, &clearcoat) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 9u, &sheen) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 10u, &anisotropy) ||
      !visible)
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t frame_address = 0u;
  if (!vkr_vk_packet_frame_root(slot, &frame_address))
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
  const VkrSsgiGpuParams params = vkr_vk_ssgi_params(
      renderer, false_v, packet->temporal.jittered_projection);
  uint32_t subsurface_source = scene;
  if (packet->subsurface_enabled &&
      !vkr_vk_deferred_storage_index(renderer, pass, 12u, &subsurface_source))
    return false_v;
  const VkrVulkanSsgiCompositeRoot root = {
      .params = params,
      .frame = frame_address,
      .inverse_view_projection = mat4_inverse(view_projection),
      .scene_texture = scene,
      .reflection_texture = reflection,
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .albedo_texture = albedo,
      .normal_texture = normal,
      .history_depth_texture = history_depth,
      .specular_texture = specular,
      .linear_sampler = renderer->transmission_sampler_slot,
      .clearcoat_texture = clearcoat,
      .sheen_texture = sheen,
      .anisotropy_texture = anisotropy,
      .visible_rows = visible->buffer.address,
      .subsurface_source_texture = subsurface_source,
      .subsurface_profile_count = packet->subsurface_enabled ? packet->subsurface.dimensions[2] : 0u,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSsgiCompositeRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SSGI_COMPOSITE];
  prepared->groups[0][0] = (params.source_width + 7u) / 8u;
  prepared->groups[0][1] = (params.source_height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

void vkr_vk_mark_ssgi_submitted(VkrVulkanRenderer *renderer,
                                uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImageInstance *inputs[] = {slot->ssgi_color_input,
                                           slot->ssgi_depth_input,
                                           slot->ssgi_identity_input};
  for (uint32_t i = 0u; i < ArrayCount(inputs); ++i)
    if (inputs[i])
      inputs[i]->last_use_submit_value = submit_value;
  VkrVulkanGraphImageInstance *outputs[] = {slot->ssgi_color_output,
                                            slot->ssgi_depth_output,
                                            slot->ssgi_identity_output};
  if (!outputs[0] || !outputs[1] || !outputs[2])
    return;
  const uint32_t current = renderer->history_output_index;
  if (current >= VKR_VULKAN_HISTORY_INSTANCE_COUNT)
    return;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrVulkanTemporalSceneState scene = {
      .signature = vkr_ssgi_content_signature(packet),
      .radiance_revision = renderer->radiance_revision,
      .publication_generation = renderer->candidate_publication_generation,
      .graph_revision = renderer->graph_revision,
  };
  for (uint32_t i = 0u; i < ArrayCount(outputs); ++i) {
    VkrVulkanGraphImageInstance *instance = outputs[i];
    instance->history_producer_submit_value = submit_value;
    instance->history_frame_index = packet->input.frame.frame_index;
    instance->history_scene_generation = packet->input.frame.scene_generation;
    instance->history_projection = packet->temporal.jittered_projection;
    instance->history_width = instance->image.width;
    instance->history_height = instance->image.height;
    instance->history_scene = scene;
    instance->history_valid = true_v;
  }
  VkrRetainedShadowToken shadow_token;
  VkrRetainedLocalShadowToken local_shadow_token;
  vkr_vulkan_renderer_retained_shadow_token(
      renderer, renderer->prepared_frame.image_index, &shadow_token);
  vkr_vulkan_renderer_retained_local_shadow_token(
      renderer, renderer->prepared_frame.image_index, &local_shadow_token);
  renderer->ssgi_histories[current] = (VkrVulkanSsgiHistory){
      .producer_submit_value = submit_value,
      .frame_index = packet->input.frame.frame_index,
      .dimensions = {outputs[0]->image.width, outputs[0]->image.height},
      .shadow_generation =
          vkr_vk_froxel_image_generation(renderer, "shadow_map"),
      .local_shadow_generation =
          vkr_vk_froxel_image_generation(renderer, "local_shadow_map"),
      .shadow_valid_layer_mask = vkr_vk_froxel_shadow_valid_mask(
          packet, shadow_token.valid_layer_mask),
      .local_shadow_valid_layer_mask = vkr_vk_froxel_local_shadow_valid_mask(
          packet, local_shadow_token.valid_layer_mask),
      .valid = true_v,
  };
}

bool8_t vkr_vk_prepare_deferred_sdsm(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
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
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSdsmRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SDSM];
  {
    prepared->groups[prepared->dispatch_count][0] =
        (root.extent[0] + 15u) / 16u;
    prepared->groups[prepared->dispatch_count][1] =
        (root.extent[1] + 15u) / 16u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
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
  const VkrPreparedFrame *packet = renderer->graph->packet;
  *out_root = (VkrVulkanExposureRoot){
      .histogram = histogram->buffer.address,
      .reset_reasons = packet->exposure.reset_reasons,
      .metering = vkr_exposure_gpu_metering(&renderer->exposure_metering,
                                            &packet->exposure),
  };
  return true_v;
}

bool8_t vkr_vk_prepare_exposure_histogram(VkrVulkanRenderer *renderer,
                                          VkrVulkanPreparedCompute *prepared,
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

  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanExposureRoot),
                                 &prepared->root_address))
    return false_v;
  /* The bounded histogram is cleared and filled inside one pass, so the first
     use of a frame slot does not depend on device memory arriving zeroed. */
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_CLEAR];
  {
    prepared->groups[prepared->dispatch_count][0] = 1u;
    prepared->groups[prepared->dispatch_count][1] = 1u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  prepared->pipelines[prepared->dispatch_count] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_HISTOGRAM];
  {
    prepared->groups[prepared->dispatch_count][0] =
        (root.extent[0] + 15u) / 16u;
    prepared->groups[prepared->dispatch_count][1] =
        (root.extent[1] + 15u) / 16u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_exposure_resolve(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
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
  if (renderer->graph->packet->exposure.history_valid) {
    for (uint32_t i = 0u; i < states->instance_count; ++i) {
      VkrVulkanGraphBufferInstance *candidate = &states->instances[i];
      if (candidate == output || !candidate->history_valid ||
          candidate->history_producer_submit_value >
              renderer->completed_value ||
          candidate->history_scene_generation !=
              renderer->graph->packet->input.frame.scene_generation)
        continue;
      if (!previous || candidate->history_producer_submit_value >
                           previous->history_producer_submit_value)
        previous = candidate;
    }
  }
  if (previous) {
    slot->exposure_state_input = previous;
    root.metering.delta_seconds = vkr_exposure_history_delta(
        renderer->exposure_seconds + root.metering.delta_seconds,
        previous->history_exposure_seconds);
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
    prepared->buffer_barrier = barrier;
    prepared->has_buffer_barrier = true_v;
  } else {
    /* No completed record. The kernel still reads this address and discards the
       value through `history_valid`, so it points at the output instance rather
       than at nothing. */
    root.metering.history_valid = 0u;
  }
  root.state = output->buffer.address;
  root.previous_state =
      previous ? previous->buffer.address : output->buffer.address;

  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanExposureRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_RESOLVE];
  {
    prepared->groups[prepared->dispatch_count][0] = 1u;
    prepared->groups[prepared->dispatch_count][1] = 1u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
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
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if (packet->exposure.reset_reasons && slot->exposure_state_history) {
    for (uint32_t i = 0u; i < slot->exposure_state_history->instance_count; ++i)
      slot->exposure_state_history->instances[i].history_valid = false_v;
  }
  renderer->exposure_seconds += packet->exposure.delta_seconds;
  slot->exposure_state_output->history_exposure_seconds =
      renderer->exposure_seconds;
  slot->exposure_state_output->history_producer_submit_value = submit_value;
  slot->exposure_state_output->history_frame_index =
      packet->input.frame.frame_index;
  slot->exposure_state_output->history_scene_generation =
      packet->input.frame.scene_generation;
  slot->exposure_state_output->history_valid = true_v;
}

bool8_t vkr_vk_prepare_subsurface(VkrVulkanRenderer *renderer,
                                  VkrVulkanPreparedCompute *prepared,
                                  const VkrRgPass *pass) {
  static const uint32_t bindings[] = {0u, 1u, 2u, 3u, 4u, 7u, 9u, 10u, 11u, 12u};
  uint32_t textures[ArrayCount(bindings)] = {0};
  for (uint32_t i = 0u; i < ArrayCount(bindings); ++i)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, bindings[i], &textures[i]))
      return false_v;
  uint32_t destination = 0u;
  VkrVulkanGraphBufferInstance *visible = vkr_vk_deferred_buffer(renderer, pass, 6u);
  if (!visible || !vkr_vk_deferred_storage_index(renderer, pass, 8u, &destination))
    return false_v;
  VkrVulkanFrameSlot *slot = &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t frame_address = 0u;
  if (!vkr_vk_packet_frame_root(slot, &frame_address))
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
  const VkrVulkanSubsurfaceRoot root = {
      .params = packet->subsurface,
      .inverse_view_projection = mat4_inverse(view_projection),
      .frame = frame_address,
      .visible_rows = visible->buffer.address,
      .hdr = textures[0],
      .source = textures[1],
      .depth = textures[2],
      .normal = textures[3],
      .vbuffer = textures[4],
      .albedo = textures[5],
      .specular = textures[6],
      .clearcoat = textures[7],
      .sheen = textures[8],
      .anisotropy = textures[9],
      .profile_bank = slot->subsurface_texture,
      .destination = destination,
      .source_sampler = renderer->transmission_sampler_slot,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSubsurfaceRoot), &prepared->root_address))
    return false_v;
  prepared->pipelines[0] = renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SUBSURFACE_GATHER];
  prepared->groups[0][0] = (root.params.dimensions[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.params.dimensions[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_motion_blur(VkrVulkanRenderer *renderer,
                                   VkrVulkanPreparedCompute *prepared,
                                   const VkrRgPass *pass,
                                   VkrVulkanDeferredPipeline pipeline) {
  static const uint32_t source_counts[] = {3u, 1u, 5u};
  const uint32_t stage =
      pipeline - VKR_VULKAN_DEFERRED_PIPELINE_MOTION_BLUR_TILE_MAX;
  uint32_t sources[5] = {0};
  for (uint32_t i = 0u; i < source_counts[stage]; ++i)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, i, &sources[i]))
      return false_v;
  for (uint32_t i = source_counts[stage]; i < ArrayCount(sources); ++i)
    sources[i] = sources[0];
  uint32_t destination_index = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 8u, &destination_index))
    return false_v;
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 8u, 0u);
  VkrVulkanGraphImageInstance *destination =
      vkr_vk_deferred_image(renderer, destination_use->image);
  if (!destination)
    return false_v;
  VkrVulkanMotionBlurRoot root = {
      .params = renderer->graph->packet->motion_blur,
      .source0 = sources[0],
      .source1 = sources[1],
      .source2 = sources[2],
      .source3 = sources[3],
      .source4 = sources[4],
      .destination0 = destination_index,
      .source_sampler = renderer->transmission_sampler_slot,
  };
  root.params.motion.x *= renderer->frame_slots[renderer->active_frame_slot]
                              .motion_blur_interval_scale;
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanMotionBlurRoot),
                                 &prepared->root_address))
    return false_v;
  const uint32_t dispatch = prepared->dispatch_count++;
  prepared->pipelines[dispatch] = renderer->deferred_pipelines[pipeline];
  const uint32_t divisor = stage == 0u ? 1u : 8u;
  prepared->groups[dispatch][0] =
      (destination->image.width + divisor - 1u) / divisor;
  prepared->groups[dispatch][1] =
      (destination->image.height + divisor - 1u) / divisor;
  prepared->groups[dispatch][2] = 1u;
  return true_v;
}

bool8_t vkr_vk_prepare_dof(VkrVulkanRenderer *renderer,
                           VkrVulkanPreparedCompute *prepared,
                           const VkrRgPass *pass,
                           VkrVulkanDeferredPipeline pipeline) {
  static const uint32_t source_counts[] = {1u, 1u, 1u, 3u, 4u, 5u};
  static const uint32_t destination_counts[] = {1u, 1u, 1u, 2u, 2u, 1u};
  const uint32_t stage = pipeline - VKR_VULKAN_DEFERRED_PIPELINE_DOF_COC;
  uint32_t sources[5] = {0};
  uint32_t destinations[2] = {0};
  for (uint32_t i = 0u; i < source_counts[stage]; ++i)
    if (!vkr_vk_deferred_sampled_index(renderer, pass, i, &sources[i]))
      return false_v;
  for (uint32_t i = source_counts[stage]; i < ArrayCount(sources); ++i)
    sources[i] = sources[0];
  for (uint32_t i = 0u; i < destination_counts[stage]; ++i)
    if (!vkr_vk_deferred_storage_index(renderer, pass, 8u + i,
                                       &destinations[i]))
      return false_v;
  if (destination_counts[stage] == 1u)
    destinations[1] = destinations[0];
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 8u, 0u);
  VkrVulkanGraphImageInstance *destination =
      vkr_vk_deferred_image(renderer, destination_use->image);
  if (!destination)
    return false_v;
  const VkrVulkanDofRoot root = {
      .params = renderer->graph->packet->dof,
      .source0 = sources[0],
      .source1 = sources[1],
      .source2 = sources[2],
      .source3 = sources[3],
      .source4 = sources[4],
      .destination0 = destinations[0],
      .destination1 = destinations[1],
      .source_sampler = renderer->transmission_sampler_slot,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanDofRoot),
                                 &prepared->root_address))
    return false_v;
  const uint32_t dispatch = prepared->dispatch_count++;
  prepared->pipelines[dispatch] = renderer->deferred_pipelines[pipeline];
  prepared->groups[dispatch][0] = (destination->image.width + 7u) / 8u;
  prepared->groups[dispatch][1] = (destination->image.height + 7u) / 8u;
  prepared->groups[dispatch][2] = 1u;
  return true_v;
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
  const VkrPreparedFrame *packet = renderer->graph->packet;
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
      .params = vkr_bloom_gpu_params(&renderer->bloom_config, &packet->bloom,
                                     renderer->prepared_frame.bloom_mip_count),
  };
  return true_v;
}

vkr_internal bool8_t vkr_vk_prepare_dispatch_bloom(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrVulkanBloomRoot *root, VkrVulkanDeferredPipeline pipeline) {
  if (!vkr_vk_deferred_push_root(renderer, root, sizeof(*root),
                                 _Alignof(VkrVulkanBloomRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[pipeline];
  {
    prepared->groups[prepared->dispatch_count][0] =
        (root->destination_extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] =
        (root->destination_extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_bloom_prefilter(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root))
    return false_v;
  return vkr_vk_prepare_dispatch_bloom(
      renderer, prepared, &root, VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_PREFILTER);
}

bool8_t vkr_vk_prepare_bloom_downsample(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root))
    return false_v;
  /* Both filters are resident; the cold configuration selects which one this
     build measures. Neither is a fallback for the other. */
  return vkr_vk_prepare_dispatch_bloom(
      renderer, prepared, &root,
      renderer->bloom_config.filter == VKR_BLOOM_FILTER_BOX_4
          ? VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_DOWNSAMPLE_BOX4
          : VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_DOWNSAMPLE_TENT13);
}

bool8_t
vkr_vk_prepare_transmission_downsample(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root))
    return false_v;
  return vkr_vk_prepare_dispatch_bloom(
      renderer, prepared, &root,
      VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_DOWNSAMPLE_BOX4);
}

bool8_t vkr_vk_prepare_bloom_upsample(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
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
  return vkr_vk_prepare_dispatch_bloom(
      renderer, prepared, &root, VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_UPSAMPLE);
}

bool8_t vkr_vk_prepare_bloom_combine(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass) {
  VkrVulkanBloomRoot root = {0};
  if (!vkr_vk_bloom_root(renderer, pass, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &root.coarse_texture))
    return false_v;
  return vkr_vk_prepare_dispatch_bloom(
      renderer, prepared, &root, VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_COMBINE);
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

vkr_internal bool8_t vkr_vk_prepare_dispatch_gtao(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrVulkanGtaoRoot *root, VkrVulkanDeferredPipeline pipeline) {
  if (!vkr_vk_deferred_push_root(renderer, root, sizeof(*root),
                                 _Alignof(VkrVulkanGtaoRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[pipeline];
  {
    prepared->groups[prepared->dispatch_count][0] =
        (root->destination_extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] =
        (root->destination_extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_gtao_depth_prefilter(VkrVulkanRenderer *renderer,
                                            VkrVulkanPreparedCompute *prepared,
                                            const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 0u, 1u, &root))
    return false_v;
  return vkr_vk_prepare_dispatch_gtao(
      renderer, prepared, &root,
      VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DEPTH_PREFILTER);
}

bool8_t vkr_vk_prepare_gtao_depth_mip(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 0u, 1u, &root))
    return false_v;
  return vkr_vk_prepare_dispatch_gtao(
      renderer, prepared, &root, VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DEPTH_MIP);
}

bool8_t vkr_vk_prepare_gtao_evaluate(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 1u, 3u, &root) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u,
                                     &root.vbuffer_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u,
                                     &root.normal_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &root.edges_texture))
    return false_v;
  return vkr_vk_prepare_dispatch_gtao(
      renderer, prepared, &root, VKR_VULKAN_DEFERRED_PIPELINE_GTAO_EVALUATE);
}

bool8_t vkr_vk_prepare_gtao_denoise(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 0u, 2u, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &root.edges_texture))
    return false_v;
  return vkr_vk_prepare_dispatch_gtao(
      renderer, prepared, &root, VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DENOISE);
}

bool8_t vkr_vk_prepare_deferred_picking(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
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
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanPickingRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_PICKING];
  {
    prepared->groups[prepared->dispatch_count][0] = 1u;
    prepared->groups[prepared->dispatch_count][1] = 1u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_transmission(VkrVulkanRenderer *renderer,
                                             VkrVulkanPreparedCompute *prepared,
                                             const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 2u);
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 3u);
  VkrVulkanGraphBufferInstance *pixel_list =
      vkr_vk_deferred_buffer(renderer, pass, 6u);
  VkrVulkanGraphBufferInstance *indirect_arguments =
      vkr_vk_deferred_buffer(renderer, pass, 7u);
  uint32_t vbuffer = 0u, depth = 0u, feedback = 0u, opaque = 0u, output = 0u;
  const VkrRgImageUse *opaque_use =
      vkr_rg_pass_find_image_use(&pass->desc, 13u, 0u);
  VkrVulkanGraphImageInstance *opaque_pyramid =
      opaque_use ? vkr_vk_deferred_image(renderer, opaque_use->image) : NULL;
  if (!visible || !state ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 4u, &feedback) ||
      !opaque_pyramid ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 13u, &opaque) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 5u, &output))
    return false_v;
  const bool8_t compact = pixel_list && indirect_arguments;
  uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.shadow_cascade_count > 0u &&
      !vkr_vk_deferred_sampled_index(renderer, pass, compact ? 8u : 6u,
                                     &shadow_texture))
    return false_v;
  uint32_t local_shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.local_shadow_view_count > 0u &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 15u,
                                     &local_shadow_texture))
    return false_v;
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const uint32_t layer = vbuffer_use && vbuffer_use->has_slice
                             ? vbuffer_use->slice.base_layer
                             : 0u;
  const uint64_t pixel_capacity =
      compact ? pixel_list->buffer.size / (2u * sizeof(uint32_t)) : 0u;
  if ((pixel_list != NULL) != (indirect_arguments != NULL) ||
      (compact &&
       (pass->desc.dispatch.kind != VKR_RG_DISPATCH_INDIRECT ||
        pixel_list->buffer.size % (2u * sizeof(uint32_t)) != 0u ||
        pixel_capacity != (uint64_t)renderer->prepared_frame.viewport_width *
                              renderer->prepared_frame.viewport_height ||
        pixel_capacity > UINT32_MAX ||
        layer >= VKR_GPU_TRANSMISSION_LAYER_COUNT ||
        indirect_arguments->buffer.size < 8u * sizeof(uint32_t))))
    return false_v;
  uint32_t motion = 0u;
  uint32_t validity = 0u;
  if (layer == 0u &&
      (!vkr_vk_deferred_buffer(renderer, pass, 10u) ||
       !vkr_vk_deferred_storage_index(renderer, pass, 11u, &motion) ||
       !vkr_vk_deferred_storage_index(renderer, pass, 12u, &validity)))
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
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
                                VKR_VULKAN_SENTINEL_SLOT_INDEX,
                                local_shadow_texture, true_v);
  const VkrVulkanTransmissionRoot root = {
      .visible_rows = visible->buffer.address,
      .materials = renderer->materials.address,
      .transmission_materials =
          renderer->materials.address + renderer->transmission_material_offset,
      .geometry_rows = slot->gpu_geometry_rows,
      .instances = slot->transmission_gpu_candidate_instances,
      .vertices = renderer->geometry_megabuffer.vertices.address,
      .indices = renderer->geometry_megabuffer.indices.address,
      .compaction_state = state->buffer.address,
      .pixel_list = compact ? pixel_list->buffer.address : 0u,
      .compact_counts =
          compact ? indirect_arguments->buffer.address + 6u * sizeof(uint32_t)
                  : 0u,
      .frame = frame_address,
      .opaque_texture = opaque,
      .opaque_mip_count =
          renderer->prepared_frame.transmission_rough_mip_pass_count + 1u,
      .view_projection = view_projection,
      .inverse_view_projection = mat4_inverse(view_projection),
      .previous_transforms =
          (slot->temporal_history_valid ? slot->temporal_transform_input
                                        : slot->temporal_transform_output)
              ->buffer.address,
      .current_view_projection = packet->temporal.current_view_projection,
      .previous_view_projection =
          slot->temporal_history_valid
              ? slot->temporal_previous_view_projection
              : packet->temporal.current_view_projection,
      .motion_texture = motion,
      .validity_texture = validity,
      .history_valid = slot->temporal_history_valid,
      .previous_frame_index = slot->temporal_history_valid
                                  ? slot->temporal_previous_frame_index
                                  : packet->input.frame.frame_index,
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .feedback_texture = feedback,
      .feedback_sampler = renderer->transmission_sampler_slot,
      .output_texture = output,
      .layer = layer,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .visible_capacity =
          renderer->prepared_frame.transmission_gpu_draw_visible_capacity,
      .geometry_count = renderer->config.geometry_capacity,
      .material_count = renderer->config.material_slot_capacity,
      .instance_count = slot->transmission_gpu_candidate_count,
      .pixel_capacity = (uint32_t)pixel_capacity,
      .compact_layer = layer,
      .compact_enabled = compact,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanTransmissionRoot),
                                 &prepared->root_address))
    return false_v;
  const bool8_t diagnostic =
      frame.render_mode != 0u || frame.shadow_debug_mode != 0u;
  if (compact) {
    const VkrVulkanDeferredPipeline pipeline =
        diagnostic
            ? VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PARTITIONED
            : (layer == 0u
                   ? VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PARTITIONED_PRODUCTION_TEMPORAL
                   : VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PARTITIONED_PRODUCTION);
    prepared->pipelines[prepared->dispatch_count] =
        renderer->deferred_pipelines[pipeline];
    prepared->indirect_buffer = indirect_arguments->buffer.handle;
    prepared->indirect_offset = pass->desc.dispatch.indirect_offset;
    prepared->dispatch_count = 1u;
  } else {
    const VkrVulkanDeferredPipeline pipeline =
        diagnostic
            ? VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION
            : (layer == 0u
                   ? VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PRODUCTION_TEMPORAL
                   : VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PRODUCTION);
    prepared->pipelines[prepared->dispatch_count] =
        renderer->deferred_pipelines[pipeline];
    {
      prepared->groups[prepared->dispatch_count][0] =
          (root.extent[0] + 7u) / 8u;
      prepared->groups[prepared->dispatch_count][1] =
          (root.extent[1] + 7u) / 8u;
      prepared->groups[prepared->dispatch_count][2] = 1u;
      prepared->dispatch_count++;
    }
  }
  return true_v;
}

bool8_t
vkr_vk_prepare_deferred_transmission_compact(VkrVulkanRenderer *renderer,
                                             VkrVulkanPreparedCompute *prepared,
                                             const VkrRgPass *pass) {
  VkrVulkanGraphBufferInstance *pixel_list =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 2u);
  VkrVulkanGraphBufferInstance *indirect_arguments =
      vkr_vk_deferred_buffer(renderer, pass, 3u);
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 6u);
  uint32_t vbuffer = 0u, source = 0u, destination = 0u;
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const uint32_t layer = vbuffer_use && vbuffer_use->has_slice
                             ? vbuffer_use->slice.base_layer
                             : UINT32_MAX;
  const uint32_t width = renderer->prepared_frame.viewport_width;
  const uint32_t height = renderer->prepared_frame.viewport_height;
  const uint64_t capacity =
      pixel_list ? pixel_list->buffer.size / (2u * sizeof(uint32_t)) : 0u;
  if (!pixel_list || !state || !indirect_arguments || !visible ||
      !vbuffer_use ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &source) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 5u, &destination) ||
      width == 0u || height == 0u || width > UINT16_MAX ||
      height > UINT16_MAX ||
      pixel_list->buffer.size % (2u * sizeof(uint32_t)) != 0u ||
      capacity != (uint64_t)width * height || capacity > UINT32_MAX ||
      layer >= VKR_GPU_TRANSMISSION_LAYER_COUNT ||
      indirect_arguments->buffer.size < 8u * sizeof(uint32_t))
    return false_v;
  const VkrVulkanTransmissionCompactRoot root = {
      .pixel_list = pixel_list->buffer.address,
      .covered_pixels = state->buffer.address +
                        offsetof(VkrGpuTransmissionDiagnostics, covered_pixels),
      .overflow_counts =
          state->buffer.address +
          offsetof(VkrGpuTransmissionDiagnostics, compact_overflow),
      .indirect_arguments = indirect_arguments->buffer.address,
      .visible_rows = visible->buffer.address,
      .materials = renderer->materials.address,
      .vbuffer_texture = vbuffer,
      .source_texture = source,
      .destination_texture = destination,
      .extent = {width, height},
      .layer = layer,
      .capacity = (uint32_t)capacity,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanTransmissionCompactRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines
          [VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT_CLEAR];
  {
    prepared->groups[prepared->dispatch_count][0] = 1u;
    prepared->groups[prepared->dispatch_count][1] = 1u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines
          [VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT];
  {
    prepared->groups[prepared->dispatch_count][0] = (width + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] = (height + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines
          [VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT_FINALIZE];
  {
    prepared->groups[prepared->dispatch_count][0] = 1u;
    prepared->groups[prepared->dispatch_count][1] = 1u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_deferred_transmission_coverage(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrRgPass *pass) {
  VkrVulkanGraphBufferInstance *state =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  uint32_t vbuffer = 0u;
  const VkrRgImageUse *vbuffer_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const uint32_t layer = vbuffer_use && vbuffer_use->has_slice
                             ? vbuffer_use->slice.base_layer
                             : 0u;
  if (!state || !vbuffer_use ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      layer >= VKR_GPU_TRANSMISSION_DIAGNOSTIC_LAYER_COUNT)
    return false_v;
  const VkrVulkanTransmissionCoverageRoot root = {
      .covered_pixels = state->buffer.address +
                        offsetof(VkrGpuTransmissionDiagnostics, covered_pixels),
      .vbuffer_texture = vbuffer,
      .layer = layer,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanTransmissionCoverageRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines
          [VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COVERAGE];
  {
    prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

void vkr_vk_record_prepared_compute(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrVulkanPreparedCompute *prepared) {
  const VkDependencyInfo history = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = prepared->image_barrier_count,
      .pImageMemoryBarriers = prepared->image_barriers,
      .bufferMemoryBarrierCount = prepared->has_buffer_barrier ? 1u : 0u,
      .pBufferMemoryBarriers = &prepared->buffer_barrier,
  };
  if (history.imageMemoryBarrierCount || history.bufferMemoryBarrierCount)
    vkCmdPipelineBarrier2(command, &history);
  const VkrVulkanPushConstants push = {.root = prepared->root_address};
  if (prepared->dispatch_count)
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
  if (prepared->indirect_buffer) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      prepared->pipelines[0]);
    vkCmdDispatchIndirect(command, prepared->indirect_buffer,
                          prepared->indirect_offset);
    return;
  }
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
  for (uint32_t i = 0u; i < prepared->dispatch_count; ++i) {
    if (i)
      vkCmdPipelineBarrier2(command, &dependency);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      prepared->pipelines[i]);
    vkCmdDispatch(command, prepared->groups[i][0], prepared->groups[i][1],
                  prepared->groups[i][2]);
  }
}

void vkr_vk_record_prepared_raster(VkrVulkanRenderer *renderer,
                                   VkCommandBuffer command,
                                   const VkrVulkanPreparedRaster *prepared) {
  vkCmdBindIndexBuffer(command, prepared->indices, 0u, VK_INDEX_TYPE_UINT32);
  VkPipeline bound = VK_NULL_HANDLE;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    if (prepared->pipelines[bucket] != bound) {
      bound = prepared->pipelines[bucket];
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
    }
    const VkrVulkanPushConstants push = {.root = prepared->root_address,
                                         .material_index = bucket};
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdSetCullMode(command, prepared->cull_modes[bucket]);
    vkCmdSetFrontFace(command, prepared->front_faces[bucket]);
    vkCmdDrawIndexedIndirectCount(
        command, prepared->arguments, prepared->argument_offsets[bucket],
        prepared->counts, prepared->count_offsets[bucket],
        prepared->command_partition_capacity,
        sizeof(VkDrawIndexedIndirectCommand));
  }
}

void vkr_vk_record_prepared_upload(VkCommandBuffer command,
                                   const VkrVulkanPreparedUpload *prepared) {
  for (uint32_t i = 0u; i < prepared->copy_count; ++i) {
    vkCmdCopyBuffer(command, prepared->source, prepared->candidates, 1u,
                    &prepared->candidate_copies[i]);
    vkCmdCopyBuffer(command, prepared->source, prepared->instances, 1u,
                    &prepared->instance_copies[i]);
  }
  vkCmdFillBuffer(command, prepared->state, 0u, VK_WHOLE_SIZE, 0u);
  if (prepared->sdsm) {
    vkCmdFillBuffer(command, prepared->sdsm, 0u, sizeof(uint32_t), UINT32_MAX);
    vkCmdFillBuffer(command, prepared->sdsm, sizeof(uint32_t),
                    VKR_VULKAN_SDSM_STATE_SIZE - sizeof(uint32_t), 0u);
  }
}

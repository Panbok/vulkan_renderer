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
    const VkrRgImage *image = array_get_VkrRgImage(&renderer->graph->images, i);
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

/* Ratio that brings a history written by `producer_submit_value` to this
   frame's pre-exposure. Both scales are powers of two, so it is exact. A
   producer outside the record window is treated as this frame's scale. */
vkr_internal float32_t vkr_vk_history_pre_exposure_scale(
    const VkrVulkanRenderer *renderer, uint64_t producer_submit_value) {
  if (producer_submit_value == 0u)
    return 1.0f;
  for (uint32_t i = 0u; i < VKR_VULKAN_PRE_EXPOSURE_HISTORY; ++i) {
    const VkrVulkanPreExposureRecord *record =
        &renderer->pre_exposure_history[i];
    if (record->submit_value == producer_submit_value)
      return vkr_ldexp_f32(
          1.0f,
          renderer->graph->packet->exposure.pre_exposure_stops - record->stops);
  }
  return 1.0f;
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
  prepared->light_contribution =
      !transmission && slot->light_contribution_requested
          ? slot->light_contribution.handle
          : VK_NULL_HANDLE;
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
  slot->local_shadow_first_view = 0u;
  slot->shadow_cascade_dynamic_render_mask = 0u;
  slot->local_shadow_render_count = 0u;
  slot->local_shadow_transmission_render_count = 0u;
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
  slot->local_shadow_first_view =
      vkr_render_graph_local_shadow_first_view(&renderer->prepared_frame);
  slot->shadow_cascade_dynamic_render_mask =
      renderer->prepared_frame.shadow_cascade_dynamic_view_count > 0u
          ? renderer->prepared_frame.shadow_cascade_dynamic_render_mask
          : 0u;
  slot->local_shadow_render_count =
      renderer->prepared_frame.local_shadow_render_count;
  slot->local_shadow_transmission_render_count =
      renderer->prepared_frame.local_shadow_transmission_render_count;
  const VkBufferCopy copies[] = {
      {.dstOffset = VKR_VULKAN_READBACK_DRAW_STATE_OFFSET,
       .size = vkr_render_graph_gpu_draw_view_count(&renderer->prepared_frame) *
               sizeof(VkrGpuDrawCompactionState)},
      {.dstOffset = VKR_VULKAN_READBACK_TRANSMISSION_STATE_OFFSET,
       .size = sizeof(VkrGpuTransmissionDiagnostics)},
      {.dstOffset = VKR_VULKAN_READBACK_SDSM_STATE_OFFSET,
       .size = VKR_VULKAN_SDSM_STATE_SIZE},
      {.dstOffset = VKR_VULKAN_READBACK_EXPOSURE_HISTOGRAM_OFFSET,
       .size = sizeof(VkrExposureGpuHistogram)},
      {.dstOffset = VKR_VULKAN_READBACK_EXPOSURE_STATE_OFFSET,
       .size = sizeof(VkrExposureGpuState)},
      {.dstOffset = VKR_VULKAN_READBACK_LIGHT_CONTRIBUTION_OFFSET,
       .size = VKR_VULKAN_LIGHT_CONTRIBUTION_SIZE},
  };
  const VkBuffer sources[] = {
      opaque->buffer.handle,
      transmission ? transmission->buffer.handle : VK_NULL_HANDLE,
      slot->sdsm_requested ? slot->sdsm_reduce_state->buffer.handle
                           : VK_NULL_HANDLE,
      slot->exposure_requested ? slot->exposure_histogram->buffer.handle
                               : VK_NULL_HANDLE,
      slot->exposure_requested ? slot->exposure_state_output->buffer.handle
                               : VK_NULL_HANDLE,
      slot->light_contribution_written ? slot->light_contribution.handle
                                       : VK_NULL_HANDLE,
  };
  _Static_assert(ArrayCount(sources) == ArrayCount(copies) &&
                     ArrayCount(sources) == ArrayCount(prepared->copies),
                 "every readback source has one copy");
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
        .buffer = sources[i],
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

/* The mirrored view-projection cropped to the reflector's screen rectangle,
   so the culling frustum holds only what that rectangle can show. Clip x
   and y map the rectangle's NDC range onto [-1, 1]. */
vkr_internal Mat4 vkr_vk_reflection_cull_view_projection(
    Mat4 projection, const VkrPlanarReflectionPayload *mirror) {
  const Vec4 rect = mirror->screen_rect;
  const float32_t scale_x = rect.z - rect.x;
  const float32_t scale_y = rect.w - rect.y;
  const float32_t center_x = rect.x + rect.z - 1.0f;
  const float32_t center_y = rect.y + rect.w - 1.0f;
  const Mat4 crop = mat4_new(
      1.0f / scale_x, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f / scale_y, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, -center_x / scale_x, -center_y / scale_y, 0.0f, 1.0f);
  return mat4_mul(crop, mat4_mul(projection, mirror->view));
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
      transmission
          ? 1u
          : vkr_render_graph_gpu_draw_view_count(&renderer->prepared_frame);
  const uint32_t local_first =
      vkr_render_graph_local_shadow_first_view(&renderer->prepared_frame);
  uint64_t views_address = 0u;
  uint64_t planes_address = 0u;
  uint64_t lod_views_address = 0u;
  Mat4 *views = NULL;
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY) {
    const VkrPreparedFrame *packet = renderer->graph->packet;
    /* Transmission culling keeps level 0; terrain never transmits. */
    VkrGpuLodView *lod_views = vkr_vk_frame_upload_allocate(
        slot, (uint64_t)view_count * sizeof(*lod_views),
        _Alignof(VkrGpuLodView), &lod_views_address, NULL);
    if (!lod_views)
      return false_v;
    if (transmission)
      lod_views[0] = (VkrGpuLodView){0};
    else
      vkr_packet_write_lod_views(packet, &renderer->prepared_frame, lod_views,
                                 view_count);
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
    const uint32_t reflection_view =
        renderer->prepared_frame.planar_reflection_active
            ? vkr_render_graph_reflection_view(&renderer->prepared_frame)
            : UINT32_MAX;
    for (uint32_t i = 1u; i < view_count; ++i) {
      /* Cascade and dynamic-caster views, opaque and transmission culling
         views, one per render slot, then the planar reflection's, which
         follows a frame that draws no local face too. */
      views[i] =
          i == reflection_view ? vkr_vk_reflection_cull_view_projection(
                                     packet->input.globals.projection,
                                     packet->input.planar_reflection)
          : i < local_first
              ? packet->input.shadow
                    ->cascades[vkr_render_graph_view_cascade(
                        &renderer->prepared_frame, i)]
                    .light_view_projection
              : packet->input.local_shadow
                    ->views[packet->input.local_shadow
                                ->render_views[(i - local_first) %
                                               renderer->prepared_frame
                                                   .local_shadow_render_count]]
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
  /* A view writes at most one row per candidate in each table; each bucket's
     commands fill the partition its packed candidates size. */
  const uint32_t candidate_capacity =
      transmission
          ? renderer->prepared_frame.transmission_gpu_draw_candidate_capacity
          : renderer->prepared_frame.gpu_draw_candidate_capacity;
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
      .lod_views = lod_views_address,
      .geometry_rows = slot->gpu_geometry_rows,
      .candidate_count = transmission ? slot->transmission_gpu_candidate_count
                                      : slot->gpu_candidate_count,
      .view_count = view_count,
      .candidate_capacity = candidate_capacity,
      .hzb_depth_epsilon = 1e-3f,
      .camera_required_flags =
          transmission ? 0u : VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE,
      .shadow_required_flags = VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER,
      .local_shadow_first_view = local_first,
      .transmission_first_view =
          local_first + renderer->prepared_frame.local_shadow_render_count,
      .transmission_required_flags =
          VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER |
          VKR_WORLD_DRAW_CANDIDATE_SHADOW_TRANSMISSION,
      .local_shadow_excluded_flags =
          renderer->prepared_frame.local_shadow_refractive_casters
              ? VKR_WORLD_DRAW_CANDIDATE_SHADOW_TRANSMISSION
              : 0u,
      /* Glass casts no sun shadow on either pipeline. */
      .cascade_excluded_flags = VKR_WORLD_DRAW_CANDIDATE_SHADOW_TRANSMISSION,
      .static_candidate_count =
          transmission ? 0u : slot->gpu_static_candidate_count,
      .command_partitions = transmission
                                ? slot->transmission_gpu_command_partitions
                                : slot->gpu_command_partitions,
      .reflection_view = UINT32_MAX,
  };
  if (!transmission && renderer->prepared_frame.planar_reflection_active) {
    const VkrPlanarReflectionPayload *reflection =
        renderer->graph->packet->input.planar_reflection;
    out_root->reflection_plane = reflection->plane;
    out_root->reflection_view =
        vkr_render_graph_reflection_view(&renderer->prepared_frame);
    out_root->reflection_object_id = reflection->object_id;
  }
  /* A retained cascade draws nothing this frame: its view still counts
     casters for the cascade metrics, but encodes no commands. Directional
     views end below bit 1 + 2 * VKR_SHADOW_CASCADE_COUNT_MAX. */
  if (!transmission)
    out_root->encode_idle_view_mask =
        (uint32_t)vkr_render_graph_cascade_idle_view_mask(
            &renderer->prepared_frame);
  for (uint32_t mip = 0u; mip < VKR_VULKAN_TEXTURE_MIP_MAX; ++mip)
    out_root->hzb_textures[mip] = UINT32_MAX;
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY && !transmission) {
    slot->packet_build.hzb_history_checks_valid = true_v;
    if (!renderer->prepared_frame.hzb_build_enabled)
      slot->packet_build.hzb_history_rejections[VKR_HZB_HISTORY_DISABLED]++;
  }
  if (pipeline == VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY && !transmission &&
      renderer->prepared_frame.hzb_build_enabled &&
      (!renderer->graph->packet->input.world ||
       !renderer->graph->packet->input.world->skinning_count)) {
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
  /* The depth image anchors the history tuple: its instances carry the
     producer metadata, and identity and surface share their indices. */
  VkrVulkanGraphImage *depths =
      vkr_vk_temporal_graph_image(renderer, "temporal_history_depth");
  VkrVulkanGraphImage *identities =
      vkr_vk_temporal_graph_image(renderer, "temporal_history_identity");
  VkrVulkanGraphImage *surfaces =
      vkr_vk_temporal_graph_image(renderer, "temporal_history_surface");
  if (!instances || !output || !depths || !identities || !surfaces ||
      !vkr_rg_buffer_handle_valid(
          renderer->temporal_transform_history_handle) ||
      depths->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
      identities->instance_count != depths->instance_count ||
      surfaces->instance_count != depths->instance_count)
    return false_v;

  const uint32_t current = renderer->history_output_index;
  slot->temporal_transform_output = output;
  /* Motion and the transform tuple anchor on these instances on every
     temporal frame. FSR frames do not write T2x's depth, identity and
     surface; a change of antialiasing mode resets temporal history
     (vkr_renderer_prepare_frame_data), so T2x never reads them. */
  slot->temporal_depth_output = &depths->instances[current];
  slot->temporal_identity_output = &identities->instances[current];
  slot->temporal_surface_output = &surfaces->instances[current];

  /* SMAA T2x's accept mask and motion blur read the immediately preceding
     frame's tuple, in flight or complete, or none. Without T2x the tuple
     still names the transforms and view that motion blur reprojects with.
     An older completed tuple would make the history age depend on GPU timing
     (ADR-037). */
  const bool8_t history_requested =
      packet->temporal.history_valid ||
      (packet->motion_blur_enabled &&
       packet->temporal.reset_reasons == VKR_TEMPORAL_RESET_NONE);
  VkrVulkanGraphImageInstance *selected = NULL;
  uint32_t selected_index = UINT32_MAX;
  if (history_requested) {
    VkrVulkanGraphBuffer *transforms =
        &renderer
             ->graph_buffers[renderer->temporal_transform_history_handle.id -
                             1u];
    for (uint32_t i = 0u; i < depths->instance_count; ++i) {
      VkrVulkanGraphImageInstance *candidate = &depths->instances[i];
      if (i == current || !candidate->history_valid ||
          candidate->history_frame_index + 1u !=
              packet->input.frame.frame_index ||
          candidate->history_scene_generation !=
              packet->input.frame.scene_generation ||
          candidate->history_width != renderer->prepared_frame.viewport_width ||
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
    VkrVulkanGraphImageInstance *previous_identity =
        &identities->instances[selected_index];
    VkrVulkanGraphImageInstance *previous_surface =
        &surfaces->instances[selected_index];
    if (!previous_transform->history_valid ||
        (packet->temporal.enabled &&
         (!selected->has_storage_slot || !previous_identity->has_storage_slot ||
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
      VkrVulkanGraphImageInstance *history_images[] = {
          selected, previous_identity, previous_surface};
      if (packet->temporal.enabled) {
        for (uint32_t i = 0u; i < ArrayCount(history_images); ++i) {
          prepared->image_barriers[i] = (VkImageMemoryBarrier2){
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
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
        prepared->image_barrier_count = ArrayCount(history_images);
      }
      prepared->buffer_barrier = buffer_barrier;
      prepared->has_buffer_barrier = true_v;
      slot->temporal_transform_input = previous_transform;
      slot->temporal_previous_view_projection =
          selected->history_view_projection;
      slot->temporal_previous_frame_index = selected->history_frame_index;
      slot->temporal_history_valid = true_v;
      /* Motion blur consumes this transform predecessor even when T2x is
       * disabled; its depth, identity and surface stay absent then. */
      if (packet->temporal.enabled) {
        slot->temporal_depth_input = selected;
        slot->temporal_identity_input = previous_identity;
        slot->temporal_surface_input = previous_surface;
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

/* FSR 3.1's depth, reactive and composition inputs. Bindings: the Scene's
   HDR colour (0), its colour before transmission (1), temporal validity (2),
   opaque depth (3), the nearest transmission layer's depth (4, with
   transmission), and the three outputs (5-7). */
bool8_t vkr_vk_prepare_fsr31_inputs(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  VkrVulkanFsr31PrepareRoot root = {
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .transmission_enabled = renderer->prepared_frame.transmission_pending,
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
                                 &prepared->root_address)) {
    return false_v;
  }
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_FSR31_PREPARE];
  prepared->groups[0][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

void vkr_vk_mark_temporal_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImageInstance *inputs[] = {
      slot->temporal_depth_input, slot->temporal_identity_input,
      slot->temporal_surface_input, slot->smaa_history_input};
  for (uint32_t i = 0u; i < ArrayCount(inputs); ++i)
    if (inputs[i])
      inputs[i]->last_use_submit_value = submit_value;
  if (slot->temporal_transform_input)
    slot->temporal_transform_input->last_use_submit_value = submit_value;

  const VkrPreparedFrame *packet = renderer->graph->packet;
  VkrVulkanGraphImageInstance *outputs[] = {
      slot->temporal_depth_output, slot->temporal_identity_output,
      slot->temporal_surface_output, slot->smaa_history_output};
  if (packet->temporal.reset_reasons != 0u) {
    /* Retire the old logical epoch only after this reset was submitted. GPU
       last-use values still own physical image reuse. */
    const char *const anchors[] = {"temporal_history_depth",
                                   "smaa_output_fullscreen",
                                   "smaa_output_editor"};
    for (uint32_t a = 0u; a < ArrayCount(anchors); ++a) {
      VkrVulkanGraphImage *image =
          vkr_vk_temporal_graph_image(renderer, anchors[a]);
      if (image)
        for (uint32_t i = 0u; i < image->instance_count; ++i)
          image->instances[i].history_valid = false_v;
    }
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
  /* A frame without FSR leaves its history stale. */
  if (!renderer->prepared_frame.fsr31_enabled)
    renderer->fsr31_history.valid = false_v;
  if (slot->fsr31_recorded) {
    renderer->fsr31_history = (VkrVulkanFsr31History){
        .submit_value = submit_value,
        .valid = true_v,
    };
    renderer->fsr31_dispatch_uses[renderer->fsr31_dispatch_slot] = submit_value;
    renderer->fsr31_dispatch_slot =
        (renderer->fsr31_dispatch_slot + 1u) % VKR_VULKAN_FRAME_SLOT_COUNT;
    slot->fsr31_recorded = false_v;
  }
  if (packet->temporal.reset_reasons != 0u &&
      vkr_rg_buffer_handle_valid(renderer->temporal_transform_history_handle)) {
    VkrVulkanGraphBuffer *transforms =
        &renderer
             ->graph_buffers[renderer->temporal_transform_history_handle.id -
                             1u];
    for (uint32_t i = 0u; i < transforms->instance_count; ++i)
      transforms->instances[i].history_motion_valid = false_v;
  }
  if (slot->temporal_transform_output) {
    slot->temporal_transform_output->history_view = packet->input.globals.view;
    slot->temporal_transform_output->history_motion_seconds =
        renderer->motion_seconds;
    slot->temporal_transform_output->history_motion_valid = true_v;
    slot->temporal_transform_output->history_producer_submit_value =
        submit_value;
    slot->temporal_transform_output->history_frame_index =
        packet->input.frame.frame_index;
    slot->temporal_transform_output->history_scene_generation =
        packet->input.frame.scene_generation;
    slot->temporal_transform_output->history_valid = true_v;
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
                                       const VkrRgPass *pass,
                                       bool8_t transmission,
                                       uint32_t view_index) {
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
  /* View zero is the camera. A shadow pass draws with the culling view it
     names: a cascade's, its dynamic casters' or a local face's render
     slot's. A planar reflection shades its mirrored view forward. */
  const bool8_t reflection =
      renderer->prepared_frame.planar_reflection_active &&
      view_index == vkr_render_graph_reflection_view(&renderer->prepared_frame);
  const bool8_t shadow = view_index != 0u && !reflection;
  const uint32_t local_first =
      vkr_render_graph_local_shadow_first_view(&renderer->prepared_frame);
  const bool8_t local_shadow = shadow && view_index >= local_first;
  /* The camera's opaque raster into 2x multisampled targets (SMAA S2x)
     needs the pipelines that rasterize two samples. */
  const VkrVulkanGraphImageInstance *depth_target =
      pass->desc.has_depth_attachment
          ? vkr_vk_deferred_image(renderer, pass->desc.depth_attachment.image)
          : NULL;
  const bool8_t msaa2 =
      depth_target && depth_target->image.samples == VK_SAMPLE_COUNT_2_BIT;
  const Mat4 view_projection =
      reflection     ? mat4_mul(packet->input.globals.projection,
                                packet->input.planar_reflection->view)
      : local_shadow ? packet->input.local_shadow
                           ->views[packet->input.local_shadow
                                       ->render_views[view_index - local_first]]
                           .light_view_projection
      : shadow ? packet->input.shadow
                     ->cascades[vkr_render_graph_view_cascade(
                         &renderer->prepared_frame, view_index)]
                     .light_view_projection
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
  /* The reflection lights its surfaces with the camera's sun cascades and
     local shadow atlas. */
  uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t local_shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (reflection &&
      ((renderer->prepared_frame.shadow_cascade_count > 0u &&
        !vkr_vk_deferred_sampled_index(renderer, pass, 5u, &shadow_texture)) ||
       (renderer->prepared_frame.local_shadow_view_count > 0u &&
        !vkr_vk_deferred_sampled_index(renderer, pass, 8u,
                                       &local_shadow_texture))))
    return false_v;
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame,
      transmission ? slot->transmission_gpu_candidate_instances
                   : slot->gpu_candidate_instances,
      view_projection, shadow_texture, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      local_shadow_texture, reflection);
  /* The mirrored eye sets the view direction; `view` stays the camera's,
     whose depth picks the sun cascade. */
  if (reflection) {
    const Vec3 eye = packet->input.planar_reflection->view_position;
    frame_root->view_position = (Vec4){eye.x, eye.y, eye.z, 1.0f};
  }
  VkrVulkanRasterRoot root = {
      .geometry_rows = slot->gpu_geometry_rows,
      .visible_rows = visible->buffer.address,
      .states = states->buffer.address,
      .frame = frame_address,
      .view_index = view_index,
      .visible_capacity =
          transmission ? renderer->prepared_frame
                             .transmission_gpu_draw_candidate_capacity
                       : renderer->prepared_frame.gpu_draw_candidate_capacity,
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
  const VkrVulkanCommandPartitions *partitions =
      transmission ? &slot->transmission_gpu_command_partitions
                   : &slot->gpu_command_partitions;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    const bool8_t opaque_bucket = (bucket & 2u) == 0u;
    const VkrVulkanPacketPipeline wanted_pipeline =
        reflection ? VKR_VULKAN_PACKET_PIPELINE_REFLECTION
        : local_shadow
            ? (opaque_bucket ? VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW_OPAQUE
                             : VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW)
        : shadow ? (opaque_bucket
                        ? VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW_OPAQUE
                        : VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW)
        : msaa2  ? ((early_z_opaque && opaque_bucket)
                        ? VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE_MSAA2
                        : VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_MSAA2)
        : (early_z_opaque && opaque_bucket)
            ? VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE
            : VKR_VULKAN_PACKET_PIPELINE_VISIBILITY;
    prepared->pipelines[bucket] = renderer->packet_pipelines[wanted_pipeline];
    prepared->cull_modes[bucket] =
        (bucket & 1u) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    /* The mirrored view reverses every triangle's winding. */
    const bool8_t mirrored =
        ((bucket & VKR_GPU_DRAW_STATE_MIRRORED_BIT) != 0u) != reflection;
    prepared->front_faces[bucket] =
        mirrored ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const VkDeviceSize argument_offset =
        ((VkDeviceSize)view_index * root.visible_capacity +
         partitions->first[bucket]) *
        sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize count_offset =
        (VkDeviceSize)view_index * sizeof(VkrGpuDrawCompactionState) +
        offsetof(VkrGpuDrawCompactionState, bucket_counts) +
        bucket * sizeof(uint32_t);
    prepared->max_draw_counts[bucket] = partitions->capacity[bucket];
    prepared->argument_offsets[bucket] = argument_offset;
    prepared->count_offsets[bucket] = count_offset;
  }
  return true_v;
}

bool8_t vkr_vk_prepare_local_shadow_transmission(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedRaster *prepared,
    const VkrRgPass *pass, bool8_t overflow) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 1u);
  VkrVulkanGraphBufferInstance *states =
      vkr_vk_deferred_buffer(renderer, pass, 2u);
  VkrVulkanGraphBufferInstance *commands =
      vkr_vk_deferred_buffer(renderer, pass, 3u);
  VkrVulkanGraphBufferInstance *instances =
      vkr_vk_deferred_buffer(renderer, pass, 9u);
  const VkrPreparedFrame *packet = renderer->graph->packet;
  /* Render slot i culls with transmission view i and writes the transmission
     layer of the view it draws. */
  const uint32_t render_slot = pass->desc.repeat_index;
  const uint32_t layer = pass->desc.depth_attachment.desc.slice.base_layer;
  if (!visible || !states || !commands || !instances ||
      !packet->input.local_shadow ||
      render_slot >=
          renderer->prepared_frame.local_shadow_transmission_render_count ||
      layer != vkr_local_shadow_render_transmission_layer(
                   packet->input.local_shadow, render_slot))
    return false_v;
  const uint32_t view_index =
      vkr_render_graph_local_shadow_first_view(&renderer->prepared_frame) +
      renderer->prepared_frame.local_shadow_render_count + render_slot;
  const VkrLocalShadowView *view =
      &packet->input.local_shadow
           ->views[packet->input.local_shadow->render_views[render_slot]];
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  uint64_t frame_address = 0u;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, instances->buffer.address,
      view->light_view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, VKR_VULKAN_SENTINEL_SLOT_INDEX, false_v);
  const uint32_t visible_capacity =
      renderer->prepared_frame.gpu_draw_candidate_capacity;
  VkrVulkanLocalShadowTransmissionRoot root = {
      .raster =
          {
              .geometry_rows = slot->gpu_geometry_rows,
              .visible_rows = visible->buffer.address,
              .states = states->buffer.address,
              .frame = frame_address,
              .view_index = view_index,
              .visible_capacity = visible_capacity,
              .previous_depth_texture = UINT32_MAX,
              .previous_depth_layer = layer,
          },
      .transmission_materials =
          renderer->materials.address + renderer->transmission_material_offset,
      .previous_color_texture = UINT32_MAX,
      .light_position = view->light_position_near,
  };
  const VkrRgImageUse *previous_depth =
      vkr_rg_pass_find_image_use(&pass->desc, 10u, 0u);
  if (previous_depth &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 10u,
                                     &root.raster.previous_depth_texture))
    return false_v;
  if (vkr_rg_pass_find_image_use(&pass->desc, 11u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 11u,
                                     &root.previous_color_texture))
    return false_v;
  if (overflow && !previous_depth)
    return false_v;
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanLocalShadowTransmissionRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->indices = renderer->geometry_megabuffer.indices.handle;
  prepared->arguments = commands->buffer.handle;
  prepared->counts = states->buffer.handle;
  const VkrVulkanCommandPartitions *partitions = &slot->gpu_command_partitions;
  const VkrVulkanPacketPipeline pipeline =
      overflow ? VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW_TRANSMISSION_OVERFLOW
               : VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW_TRANSMISSION;
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    prepared->pipelines[bucket] = renderer->packet_pipelines[pipeline];
    prepared->cull_modes[bucket] =
        (bucket & 1u) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    prepared->front_faces[bucket] = (bucket & VKR_GPU_DRAW_STATE_MIRRORED_BIT)
                                        ? VK_FRONT_FACE_CLOCKWISE
                                        : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    prepared->max_draw_counts[bucket] = partitions->capacity[bucket];
    prepared->argument_offsets[bucket] =
        ((VkDeviceSize)view_index * visible_capacity +
         partitions->first[bucket]) *
        sizeof(VkDrawIndexedIndirectCommand);
    prepared->count_offsets[bucket] =
        (VkDeviceSize)view_index * sizeof(VkrGpuDrawCompactionState) +
        offsetof(VkrGpuDrawCompactionState, bucket_counts) +
        bucket * sizeof(uint32_t);
  }
  return true_v;
}

/* `sample_one`: SMAA S2x's second sample, resolved only at the pixels
   VBuffer.Split listed (binding 24, counted at 25 for the indirect dispatch;
   the edge mask at 18 rejects stale slots) into its own G-buffer; the motion
   and validity images stay the first sample's, so its pass binds none. */
bool8_t vkr_vk_prepare_deferred_gbuffer(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass,
                                        bool8_t sample_one) {
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
  for (uint32_t i = 0u; i < ArrayCount(bindings); ++i) {
    if ((i >= 7u || (sample_one && i >= 5u)) &&
        !vkr_rg_pass_find_image_use(&pass->desc, bindings[i], 0u)) {
      indices[i] = VKR_VULKAN_SENTINEL_SLOT_INDEX;
      continue;
    }
    if (!vkr_vk_deferred_storage_index(renderer, pass, bindings[i],
                                       &indices[i]))
      return false_v;
  }
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
    const float64_t elapsed =
        renderer->motion_seconds + packet->motion_blur_delta_seconds -
        slot->temporal_transform_input->history_motion_seconds;
    if (elapsed > 0.0)
      slot->motion_blur_interval_scale =
          (float32_t)(packet->motion_blur_delta_seconds / elapsed);
  }
  VkrVulkanResolveRoot root = {
      .geometry_rows = slot->gpu_geometry_rows,
      .visible_rows = visible->buffer.address,
      .vertices = renderer->geometry_megabuffer.vertices.address,
      .instances = slot->gpu_candidate_instances,
      .materials = renderer->materials.address,
      .terrain_materials =
          renderer->materials.address + renderer->terrain_material_offset,
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
      .pre_exposure = packet->exposure.pre_exposure,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .visible_capacity = renderer->prepared_frame.gpu_draw_candidate_capacity,
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
      .decals = slot->decals,
      .decal_masks = slot->decal_masks,
  };
  /* Baked lamps (ADR-104): the resolve sums the
     active lamp layers of each lightmapped draw's rectangle. */
  root.baked_lamps_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  root.baked_direction_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  root.lightmap_direction_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  VkrVulkanGraphBufferInstance *sample1_arguments = NULL;
  if (sample_one) {
    VkrVulkanGraphBufferInstance *sample1_pixels =
        vkr_vk_deferred_buffer(renderer, pass, 24u);
    sample1_arguments = vkr_vk_deferred_buffer(renderer, pass, 25u);
    if (!sample1_pixels || !sample1_arguments ||
        !vkr_vk_deferred_storage_index(renderer, pass, 18u,
                                       &root.edge_mask_texture)) {
      return false_v;
    }
    root.sample1_pixels = sample1_pixels->buffer.address;
  }
  if (vkr_rg_pass_find_image_use(&pass->desc, 16u, 0u)) {
    if (!vkr_vk_deferred_storage_index(renderer, pass, 16u,
                                       &root.baked_lamps_texture) ||
        !vkr_vk_deferred_storage_index(renderer, pass, 17u,
                                       &root.baked_direction_texture))
      return false_v;
    const VkrLightmapBinding *lightmap = &packet->input.lighting->lightmap;
    if (slot->lightmap_rects != 0u) {
      root.lightmap_rects = slot->lightmap_rects;
      root.lightmap_texture = slot->lightmap_texture;
      root.lightmap_sampler = slot->lightmap_sampler;
      root.lightmap_direction_texture = slot->lightmap_direction_texture;
      root.lightmap_rect_count = lightmap->rect_count;
      root.lightmap_layer_count = lightmap->layer_count;
      root.lightmap_active_count = lightmap->active_layer_count;
      root.lightmap_inverse_page_size = 1.0f / (float32_t)lightmap->page_size;
      for (uint32_t i = 0u; i < lightmap->active_layer_count; ++i) {
        root.lightmap_active_layers[i] = lightmap->active_layers[i];
        root.lightmap_active_weights[i] = lightmap->active_weights[i];
      }
    }
  }
  if (slot->decal_count) {
    const VkrDecalGrid *grid = packet->input.world->decal_grid;
    root.decal_grid_origin_cell_size =
        (Vec4){grid->origin.x, grid->origin.y, grid->origin.z, grid->cell_size};
    root.decal_grid_dimensions_count[0] = grid->dimensions[0];
    root.decal_grid_dimensions_count[1] = grid->dimensions[1];
    root.decal_grid_dimensions_count[2] = grid->dimensions[2];
    root.decal_grid_dimensions_count[3] = slot->decal_count;
  }
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanResolveRoot),
                                 &prepared->root_address))
    return false_v;
  const VkrVulkanDeferredPipeline pipeline =
      sample_one ? VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_NONE
      : emissive_capture
          ? (resolve_debug_capture
                 ? VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_EMISSIVE_DEBUG
                 : VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_EMISSIVE)
          : (resolve_debug_capture ? VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_DEBUG
                                   : VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_NONE);
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[pipeline];
  if (sample1_arguments) {
    prepared->indirect_buffer = sample1_arguments->buffer.handle;
    prepared->indirect_offset = pass->desc.dispatch.indirect_offset;
    prepared->dispatch_count = 1u;
    return true_v;
  }
  {
    prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
    prepared->groups[prepared->dispatch_count][2] = 1u;
    prepared->dispatch_count++;
  }
  return true_v;
}

/* Shadow.LocalMask shares the lighting root; it binds only the visibility
   inputs and the mask it writes. */
/* The sparse volume's response for deferred lighting, which reads it in
   place of its own lookup: the eight-probe walk in the lighting kernel cost
   more than here. The sample pass looks the volume up at half resolution;
   the upsample takes each pixel's matching half-resolution texels and
   lists the pixels none matches, which the fallback pass looks up one by
   one, so neither kernel that covers the screen carries the lookup at full
   resolution (ADR-054). */
bool8_t vkr_vk_prepare_diffuse_volume_sample(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrRgPass *pass, VkrVulkanDiffuseVolumeStage stage) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const bool8_t upsample = stage == VKR_VULKAN_DIFFUSE_VOLUME_UPSAMPLE;
  const bool8_t fallback = stage == VKR_VULKAN_DIFFUSE_VOLUME_FALLBACK;
  uint32_t vbuffer = 0u;
  uint32_t depth = 0u;
  uint32_t normal = 0u;
  uint32_t output = 0u;
  uint32_t guide = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t half = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t baked_lamps = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  VkrVulkanGraphBufferInstance *arguments =
      vkr_vk_deferred_buffer(renderer, pass, 22u);
  VkrVulkanGraphBufferInstance *pixels =
      fallback || upsample ? vkr_vk_deferred_buffer(renderer, pass, 23u) : NULL;
  /* The camera rows name each pixel's instance, whose lighting origin places
     a mover's lookup (ADR-120). */
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 13u);
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &normal) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 16u, &output) ||
      (!fallback && !vkr_vk_deferred_storage_index(
                        renderer, pass, upsample ? 21u : 17u, &guide)) ||
      (upsample &&
       !vkr_vk_deferred_storage_index(renderer, pass, 20u, &half)) ||
      (vkr_rg_pass_find_image_use(&pass->desc, 18u, 0u) &&
       !vkr_vk_deferred_storage_index(renderer, pass, 18u, &baked_lamps)) ||
      !arguments || ((fallback || upsample) && !pixels) || !visible)
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
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, slot->gpu_candidate_instances,
      view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, VKR_VULKAN_SENTINEL_SLOT_INDEX, true_v);
  /* The graph's viewport divisor rounds down, at least one texel. */
  const uint32_t width = renderer->prepared_frame.viewport_width;
  const uint32_t height = renderer->prepared_frame.viewport_height;
  const VkrVulkanDiffuseVolumeSampleRoot root = {
      .frame = frame_address,
      .inverse_view_projection = mat4_inverse(view_projection),
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .normal_texture = normal,
      .baked_lamps_texture = baked_lamps,
      .extent = {width, height},
      .half_extent = {ClampBot(width / 2u, 1u), ClampBot(height / 2u, 1u)},
      .output_texture = output,
      .guide_texture = guide,
      .half_texture = half,
      .fallback_arguments = arguments->buffer.address,
      .fallback_pixels = pixels ? pixels->buffer.address : 0u,
      .visible_rows = visible->buffer.address,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanDiffuseVolumeSampleRoot),
                                 &prepared->root_address))
    return false_v;
  if (fallback) {
    /* The upsample's count sizes the dispatch, 64 listed pixels a group. */
    prepared->pipelines[0] =
        renderer->deferred_pipelines
            [VKR_VULKAN_DEFERRED_PIPELINE_DIFFUSE_VOLUME_FALLBACK];
    prepared->indirect_buffer = arguments->buffer.handle;
    prepared->indirect_offset = pass->desc.dispatch.indirect_offset;
    prepared->dispatch_count = 1u;
    return true_v;
  }
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines
          [upsample ? VKR_VULKAN_DEFERRED_PIPELINE_DIFFUSE_VOLUME_UPSAMPLE
                    : VKR_VULKAN_DEFERRED_PIPELINE_DIFFUSE_VOLUME_SAMPLE];
  const uint32_t *dispatch = upsample ? root.extent : root.half_extent;
  prepared->groups[prepared->dispatch_count][0] = (dispatch[0] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][1] = (dispatch[1] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
  return true_v;
}

bool8_t vkr_vk_prepare_local_shadow_mask(VkrVulkanRenderer *renderer,
                                         VkrVulkanPreparedCompute *prepared,
                                         const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint32_t vbuffer = 0u, depth = 0u, normal = 0u, mask = 0u;
  uint32_t local_shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 13u);
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &normal) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 16u, &mask) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 8u,
                                     &local_shadow_texture) ||
      !visible)
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
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, slot->gpu_candidate_instances,
      view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, local_shadow_texture, true_v);
  /* The selected baked lamps' rows, with their static and composite views;
     their irradiance hidden by moving casters goes to binding 21. */
  const VkrLocalShadowPassPayload *local = packet->input.local_shadow;
  const uint32_t baked_lamp_count =
      renderer->prepared_frame.local_shadow_baked_lamp_count;
  uint32_t baked_lamp_shadow = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t baked_lamps_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint64_t baked_lamps_address = 0u;
  if (baked_lamp_count != 0u) {
    if (!local ||
        !vkr_vk_deferred_storage_index(renderer, pass, 21u,
                                       &baked_lamp_shadow) ||
        !vkr_vk_deferred_storage_index(renderer, pass, 18u,
                                       &baked_lamps_texture))
      return false_v;
    VkrVulkanBakedShadowLamp *lamps = vkr_vk_frame_upload_allocate(
        slot, baked_lamp_count * sizeof(*lamps),
        _Alignof(VkrVulkanBakedShadowLamp), &baked_lamps_address, NULL);
    if (!lamps)
      return false_v;
    for (uint32_t i = 0u; i < baked_lamp_count; ++i) {
      const VkrLocalShadowBakedLamp *baked = &local->baked_lamps[i];
      lamps[i] = (VkrVulkanBakedShadowLamp){
          .static_first_view = baked->static_first_view,
          .composite_first_view = baked->composite_first_view,
      };
      vkr_point_light_pack(&baked->light, packet->exposure.pre_exposure,
                           &lamps[i].light);
    }
  }
  const VkrVulkanLocalShadowMaskRoot root = {
      .frame = frame_address,
      .inverse_view_projection = mat4_inverse(view_projection),
      .visible_rows = visible->buffer.address,
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .normal_texture = normal,
      .mask_texture = mask,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      /* Under SMAA T2x the contact march and the four rotated filter taps
         alternate between two phases in step with the jitter, and the
         resolve averages them (ADR-037). */
      .contact_noise_index =
          packet->temporal.enabled
              ? packet->input.frame.frame_index % VKR_TEMPORAL_SEQUENCE_LENGTH
              : 0u,
      .temporal_filter = packet->temporal.enabled ? 1u : 0u,
      .baked_lamp_shadow_texture = baked_lamp_shadow,
      .baked_lamp_count = baked_lamp_count,
      .baked_lamps = baked_lamps_address,
      .baked_lamps_texture = baked_lamps_texture,
      .mask_lights = renderer->prepared_frame.local_shadow_mask_lights,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanLocalShadowMaskRoot),
                                 &prepared->root_address))
    return false_v;
  /* Contact shadows and the baked lamps' hidden light are separate kernels
     so the default one does not carry their registers. */
  const bool8_t contact = renderer->prepared_frame.local_shadow_contact;
  const VkrVulkanDeferredPipeline mask_pipeline =
      baked_lamp_count != 0u
          ? (contact
                 ? VKR_VULKAN_DEFERRED_PIPELINE_LOCAL_SHADOW_MASK_CONTACT_BAKED
                 : VKR_VULKAN_DEFERRED_PIPELINE_LOCAL_SHADOW_MASK_BAKED)
          : (contact ? VKR_VULKAN_DEFERRED_PIPELINE_LOCAL_SHADOW_MASK_CONTACT
                     : VKR_VULKAN_DEFERRED_PIPELINE_LOCAL_SHADOW_MASK);
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[mask_pipeline];
  prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
  return true_v;
}

/* The base kernel shades every tile unless the frame has layered materials;
   then the layered pass owns tiles that carry layer data. */
/* `sample_one`: SMAA S2x's second sample, lit at the pixels VBuffer.Split
   listed (binding 24, counted at 25; the edge mask at 22 rejects stale
   slots) from its own G-buffer and depth with the base kernel. Its per-pixel
   inputs (GTAO, shadow masks, volume, baked lamps) come from a neighbour
   whose first sample (visibility at 26) lies on its draw. */
bool8_t vkr_vk_prepare_deferred_lighting(VkrVulkanRenderer *renderer,
                                         VkrVulkanPreparedCompute *prepared,
                                         const VkrRgPass *pass, bool8_t layered,
                                         bool8_t sample_one) {
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
      (vkr_rg_pass_find_image_use(&pass->desc, 10u, 0u) &&
       !vkr_vk_deferred_storage_index(renderer, pass, 10u, &clearcoat)) ||
      (vkr_rg_pass_find_image_use(&pass->desc, 11u, 0u) &&
       !vkr_vk_deferred_storage_index(renderer, pass, 11u, &sheen)) ||
      (vkr_rg_pass_find_image_use(&pass->desc, 12u, 0u) &&
       !vkr_vk_deferred_storage_index(renderer, pass, 12u, &anisotropy)) ||
      !visible) {
    return false_v;
  }
  uint32_t local_shadow_mask = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 16u, 0u) &&
      !vkr_vk_deferred_storage_index(renderer, pass, 16u, &local_shadow_mask))
    return false_v;
  uint32_t shadow_moments = UINT32_MAX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 17u, 0u) &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 17u, &shadow_moments))
    return false_v;
  uint32_t diffuse_volume = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 20u, 0u) &&
      !vkr_vk_deferred_storage_index(renderer, pass, 20u, &diffuse_volume))
    return false_v;
  uint32_t baked_lamp_shadow = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 21u, 0u) &&
      !vkr_vk_deferred_storage_index(renderer, pass, 21u, &baked_lamp_shadow))
    return false_v;
  uint32_t baked_lamps = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  uint32_t baked_direction = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (vkr_rg_pass_find_image_use(&pass->desc, 18u, 0u) &&
      (!vkr_vk_deferred_storage_index(renderer, pass, 18u, &baked_lamps) ||
       !vkr_vk_deferred_storage_index(renderer, pass, 19u, &baked_direction)))
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
  const VkrSkyPassPayload *sky = packet->input.sky;
  const VkrSkyMode sky_mode = !sky ? VKR_SKY_MODE_NONE
                              : sky->atmosphere.enabled
                                  ? VKR_SKY_MODE_ATMOSPHERE
                                  : VKR_SKY_MODE_CONSTANT;
  uint32_t sky_view = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (sky_mode == VKR_SKY_MODE_ATMOSPHERE &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 15u, &sky_view))
    return false_v;
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
  if (!sample_one && renderer->prepared_frame.subsurface_enabled &&
      !vkr_vk_deferred_storage_index(renderer, pass, 14u, &subsurface_source)) {
    return false_v;
  }
  uint32_t edge_mask = 0u;
  uint32_t side_vbuffer = 0u;
  VkrVulkanGraphBufferInstance *sample1_pixels = NULL;
  VkrVulkanGraphBufferInstance *sample1_arguments = NULL;
  if (sample_one) {
    sample1_pixels = vkr_vk_deferred_buffer(renderer, pass, 24u);
    sample1_arguments = vkr_vk_deferred_buffer(renderer, pass, 25u);
    if (!sample1_pixels || !sample1_arguments ||
        !vkr_vk_deferred_storage_index(renderer, pass, 22u, &edge_mask) ||
        !vkr_vk_deferred_storage_index(renderer, pass, 26u, &side_vbuffer)) {
      return false_v;
    }
  }
  /* Deferred lighting measures every frame it shades; the readback publishes
   * the counters only for such frames. */
  slot->light_contribution_written = slot->light_contribution_requested;
  /* Reflector pixels take the mirrored view where it covers them. */
  uint32_t planar_reflection = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (renderer->prepared_frame.planar_reflection_active &&
      !vkr_vk_deferred_sampled_index(renderer, pass, 45u, &planar_reflection))
    return false_v;
  const VkrRenderGraphFrameInfo *frame_info = &renderer->prepared_frame;
  const float32_t planar_image_width =
      (float32_t)Max(frame_info->viewport_width / 2u, 1u);
  const float32_t planar_image_height =
      (float32_t)Max(frame_info->viewport_height / 2u, 1u);
  const VkrVulkanLightingRoot root = {
      .frame = frame_address,
      .inverse_view_projection = mat4_inverse(view_projection),
      .view_rays = vkr_atmosphere_view_rays(
          packet->temporal.jittered_projection, packet->input.globals.view),
      .vbuffer_texture = vbuffer,
      .depth_texture = depth,
      .albedo_texture = albedo,
      .specular_texture = specular,
      .normal_texture = normal,
      .scene_texture = scene,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .sky_view_texture = sky_view,
      .sky_sampler = renderer->transmission_sampler_slot,
      .sky_mode = sky_mode,
      .gtao_visibility_texture = gtao_visibility,
      .sky_radiance =
          sky_mode == VKR_SKY_MODE_CONSTANT
              ? (Vec4){sky->constant_radiance.x * packet->exposure.pre_exposure,
                       sky->constant_radiance.y * packet->exposure.pre_exposure,
                       sky->constant_radiance.z * packet->exposure.pre_exposure,
                       0.0f}
              : (Vec4){0},
      .clearcoat_texture = clearcoat,
      .sheen_texture = sheen,
      .anisotropy_texture = anisotropy,
      .layered_tiles =
          sample_one
              ? 0u
              : (renderer->prepared_frame.lighting_layers_enabled ? 1u : 0u) |
                    (renderer->prepared_frame.brdf_extensions_enabled ? 2u
                                                                      : 0u),
      .local_shadow_mask_texture = local_shadow_mask,
      .shadow_moments_texture = shadow_moments,
      .baked_lamps_texture = baked_lamps,
      .baked_direction_texture = baked_direction,
      .diffuse_volume_texture = diffuse_volume,
      .baked_lamp_shadow_texture = baked_lamp_shadow,
      .visible_rows = visible->buffer.address,
      .light_contribution = slot->light_contribution_requested && !sample_one
                                ? slot->light_contribution.address
                                : 0u,
      .shadow_moments_sampler = renderer->transmission_sampler_slot,
      .planar_reflection_texture = planar_reflection,
      .planar_reflection_sampler = renderer->dfg_sampler_slot.index,
      .planar_reflection_plane = frame_info->planar_reflection_active
                                     ? packet->input.planar_reflection->plane
                                     : (Vec4){0},
      .planar_reflection_uv_scale =
          {(float32_t)vkr_render_graph_reflection_extent(
               frame_info, frame_info->viewport_width) /
               planar_image_width,
           (float32_t)vkr_render_graph_reflection_extent(
               frame_info, frame_info->viewport_height) /
               planar_image_height},
      .planar_reflection_object_id =
          frame_info->planar_reflection_active
              ? packet->input.planar_reflection->object_id
              : 0u,
      .subsurface_source_texture = subsurface_source,
      .subsurface_profile_count =
          !sample_one && renderer->prepared_frame.subsurface_enabled &&
                  slot->subsurface_texture != VKR_VULKAN_SENTINEL_SLOT_INDEX
              ? packet->subsurface.dimensions[2]
              : 0u,
      .edge_mask_texture = edge_mask,
      .side_vbuffer_texture = side_vbuffer,
      .sample1_pixels = sample1_pixels ? sample1_pixels->buffer.address : 0u,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanLightingRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines
          [layered ? VKR_VULKAN_DEFERRED_PIPELINE_LIGHTING_LAYERED
                   : VKR_VULKAN_DEFERRED_PIPELINE_LIGHTING];
  if (sample1_arguments) {
    prepared->indirect_buffer = sample1_arguments->buffer.handle;
    prepared->indirect_offset = pass->desc.dispatch.indirect_offset;
    prepared->dispatch_count = 1u;
    return true_v;
  }
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
  /* Sampled: scene, motion, validity, depth. Storage: the opaque vbuffer,
     then the outputs colour, depth, identity, surface and accept mask. */
  uint32_t sampled[4] = {0};
  uint32_t storage[6] = {0};
  const uint32_t sampled_bindings[] = {0u, 2u, 3u, 4u};
  const uint32_t storage_bindings[] = {5u, 8u, 9u, 10u, 11u, 16u};
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
      slot->temporal_depth_input && slot->temporal_identity_input &&
      slot->temporal_surface_input;
  const VkrVulkanTemporalResolveRoot root = {
      .visible_rows = visible->buffer.address,
      .instances = instances->buffer.address,
      .scene_texture = sampled[0],
      .motion_texture = sampled[1],
      .validity_texture = sampled[2],
      .depth_texture = sampled[3],
      .vbuffer_texture = storage[0],
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
      .output_accept_texture = storage[5],
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .history_valid = history_valid,
      .render_mode = packet->input.globals.render_mode,
      .transmission_visible_rows =
          transmission_visible ? transmission_visible->buffer.address : 0u,
      .transmission_instances =
          transmission_instances ? transmission_instances->buffer.address : 0u,
      .transmission_vbuffer_texture = transmission_vbuffer,
      .transmission_depth_texture = transmission_depth,
      .transmission_enabled = transmission_enabled,
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

bool8_t vkr_vk_prepare_shadow_moments(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass) {
  const VkrRgImageUse *write = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  VkrVulkanGraphImageInstance *moments =
      write ? vkr_vk_deferred_image(renderer, write->image) : NULL;
  uint32_t depth_index = 0u, moments_index = 0u;
  if (!moments || !write->has_slice ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &depth_index) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 1u, &moments_index))
    return false_v;
  /* The repeat index names the moments layer; its cascade follows the first
     filtered cascade. */
  const uint32_t layer = write->slice.base_layer;
  const VkrVulkanShadowMomentsRoot root = {
      .depth_texture = depth_index,
      .moments_texture = moments_index,
      .depth_layer = layer + VKR_SHADOW_EVSM_FIRST_CASCADE,
      .moments_layer = layer,
      .depth_size = renderer->prepared_frame.shadow_map_size,
      .moments_size = moments->image.width,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanShadowMomentsRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SHADOW_MOMENTS];
  prepared->groups[prepared->dispatch_count][0] = (root.moments_size + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][1] = (root.moments_size + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
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

bool8_t vkr_vk_prepare_fog_apply(VkrVulkanRenderer *renderer,
                                 VkrVulkanPreparedCompute *prepared,
                                 const VkrRgPass *pass) {
  uint32_t depth = 0u, target = 0u;
  uint32_t aerial = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &target) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      (renderer->prepared_frame.aerial_perspective_enabled &&
       !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &aerial)))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->sky)
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
  /* Sky-lit fog reads the sun and the sky light's SH through a lit frame
     root; it samples no shadow maps. */
  uint64_t frame_address = 0u;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, slot->gpu_candidate_instances,
      packet->temporal.current_view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, VKR_VULKAN_SENTINEL_SLOT_INDEX, true_v);
  const VkrVulkanFogRoot root = {
      .params =
          vkr_fog_pre_exposed(&packet->fog, packet->exposure.pre_exposure),
      .inverse_view_projection = mat4_inverse(view_projection),
      .camera_position = {packet->input.globals.view_position.x,
                          packet->input.globals.view_position.y,
                          packet->input.globals.view_position.z, 1.0f},
      .depth_texture = depth,
      .target_texture = target,
      .extent = {renderer->prepared_frame.viewport_width,
                 renderer->prepared_frame.viewport_height},
      .sky = slot->sky,
      .aerial_perspective_texture = aerial,
      .frame = frame_address,
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

/* Builds the frame's sky-view lookup or aerial-perspective volume from the
   published generation's lookup textures in the frame-slot sky record. The
   aerial pass also patches its volume slot into that mapped record for the
   forward consumers recorded after it. */
bool8_t vkr_vk_prepare_sky(VkrVulkanRenderer *renderer,
                           VkrVulkanPreparedCompute *prepared,
                           const VkrRgPass *pass, bool8_t sky_view) {
  uint32_t output = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &output))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->sky || !slot->sky_record)
    return false_v;
  /* Consumers sample the volume through the sampled heap, whose slot differs
     from the storage slot this pass writes. */
  uint32_t sampled = 0u;
  if (!sky_view) {
    if (!vkr_vk_deferred_sampled_index(renderer, pass, 0u, &sampled))
      return false_v;
    slot->sky_record->aerial_perspective_texture = sampled;
  }
  const VkrVulkanSkyBuildRoot root = {
      .sky = slot->sky,
      .output_texture = output,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSkyBuildRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines
          [sky_view ? VKR_VULKAN_DEFERRED_PIPELINE_SKY_VIEW
                    : VKR_VULKAN_DEFERRED_PIPELINE_AERIAL_PERSPECTIVE];
  /* Aerial perspective marches one froxel column per thread. */
  const uint32_t width =
      sky_view ? VKR_ATMOSPHERE_SKY_VIEW_WIDTH * VKR_ATMOSPHERE_SKY_VIEW_TABLES
               : VKR_ATMOSPHERE_AERIAL_SIZE;
  const uint32_t height =
      sky_view ? VKR_ATMOSPHERE_SKY_VIEW_HEIGHT : VKR_ATMOSPHERE_AERIAL_SIZE;
  prepared->groups[0][0] = (width + 7u) / 8u;
  prepared->groups[0][1] = (height + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

/* Sun-projected cloud transmittance. Consumers sample the map through the sky
   record's sampled slot (ADR-074). */
bool8_t vkr_vk_prepare_cloud_shadow(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  uint32_t output = 0u;
  uint32_t sampled = 0u;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &output) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &sampled))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->sky || !slot->sky_record)
    return false_v;
  slot->sky_record->cloud_shadow_texture = sampled;
  const VkrVulkanSkyBuildRoot root = {
      .sky = slot->sky,
      .output_texture = output,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSkyBuildRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_CLOUD_SHADOW];
  prepared->groups[0][0] = (VKR_CLOUD_SHADOW_SIZE + 7u) / 8u;
  prepared->groups[0][1] = (VKR_CLOUD_SHADOW_SIZE + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

/* Cloud radiance history holds camera-independent sky radiance; its stored
   view-projection reprojects it. With temporal antialiasing only the exact
   temporal-transform predecessor is eligible. Without it, the newest
   submitted producer of the same traced layer and grid seeds the trace. Either
   may still be in flight: the trace's same-queue image barrier orders its
   writes, so accumulation never depends on GPU timing. */
vkr_internal VkrVulkanGraphImageInstance *
vkr_vk_select_cloud_history(VkrVulkanRenderer *renderer,
                            VkrVulkanGraphImage *history, uint32_t *out_index) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const uint32_t current = renderer->history_output_index;
  if ((packet->temporal.reset_reasons & ~VKR_CLOUD_HISTORY_KEPT_RESETS) !=
      VKR_TEMPORAL_RESET_NONE)
    return NULL;
  const uint64_t signature = vkr_cloud_history_signature(
      &packet->sky.clouds, packet->sky.camera_position.w);
  const uint64_t frame_index = packet->input.frame.frame_index;
  const VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const VkrVulkanGraphBuffer *transforms =
      vkr_rg_buffer_handle_valid(renderer->temporal_transform_history_handle)
          ? &renderer->graph_buffers
                 [renderer->temporal_transform_history_handle.id - 1u]
          : NULL;
  VkrVulkanGraphImageInstance *selected = NULL;
  for (uint32_t i = 0u; i < history->instance_count; ++i) {
    const VkrVulkanCloudHistory *candidate = &renderer->cloud_histories[i];
    VkrVulkanGraphImageInstance *image = &history->instances[i];
    const bool8_t source_age_in_ring =
        candidate->frame_index < frame_index &&
        frame_index - candidate->frame_index <= history->instance_count;
    const bool8_t predecessor =
        !packet->temporal.enabled ||
        (slot->temporal_history_valid && transforms &&
         i < transforms->instance_count &&
         slot->temporal_transform_input == &transforms->instances[i]);
    if (i == current || !candidate->valid || !image->history_valid ||
        !image->has_sampled_slot || !source_age_in_ring || !predecessor ||
        candidate->producer_submit_value !=
            image->history_producer_submit_value ||
        candidate->signature != signature ||
        candidate->graph_generation != history->graph_generation ||
        candidate->dimensions[0] != image->image.width ||
        candidate->dimensions[1] != image->image.height)
      continue;
    if (!selected ||
        candidate->producer_submit_value >
            renderer->cloud_histories[*out_index].producer_submit_value) {
      selected = image;
      *out_index = i;
    }
  }
  return selected;
}

/* Half-resolution cloud trace into this frame's history output, which
   deferred lighting samples through the sky record. */
bool8_t vkr_vk_prepare_cloud_trace(VkrVulkanRenderer *renderer,
                                   VkrVulkanPreparedCompute *prepared,
                                   const VkrRgPass *pass) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanGraphImage *history =
      vkr_vk_temporal_graph_image(renderer, "cloud_history");
  const uint32_t current = renderer->history_output_index;
  uint32_t output = 0u;
  uint32_t output_sampled = 0u;
  uint32_t depth = 0u;
  if (!history || !history->live ||
      history->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
      current >= history->instance_count || !slot->sky || !slot->sky_record ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &output) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &output_sampled) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth))
    return false_v;
  slot->cloud_history_output = &history->instances[current];
  slot->sky_record->cloud_radiance_texture = output_sampled;

  uint32_t selected_index = UINT32_MAX;
  VkrVulkanGraphImageInstance *selected =
      vkr_vk_select_cloud_history(renderer, history, &selected_index);
  slot->cloud_history_input = selected;
  if (selected) {
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
    prepared->image_barrier_count = 1u;
  }

  /* The trace reads only the sky light's SH through a lit frame root. */
  uint64_t frame = 0u;
  VkrVulkanPacketFrameRoot *frame_root = vkr_vk_packet_frame_root(slot, &frame);
  if (!frame_root)
    return false_v;
  const VkrPacketFrameConstants constants = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &constants, slot->gpu_candidate_instances,
      packet->temporal.current_view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, VKR_VULKAN_SENTINEL_SLOT_INDEX, true_v);
  const VkrVulkanCloudTraceRoot root = {
      .previous_view_projection =
          selected ? renderer->cloud_histories[selected_index].view_projection
                   : packet->sky.view_projection,
      .sky = slot->sky,
      .frame = frame,
      .depth_texture = depth,
      .history_texture =
          selected ? selected->sampled_slot.index : output_sampled,
      .history_sampler = renderer->transmission_sampler_slot,
      .output_texture = output,
      .extent = {history->instances[current].image.width,
                 history->instances[current].image.height},
      .depth_extent = {renderer->prepared_frame.viewport_width,
                       renderer->prepared_frame.viewport_height},
      .frame_index = (uint32_t)packet->input.frame.frame_index,
      .history_valid = selected ? 1u : 0u,
      .history_pre_exposure_scale =
          selected ? vkr_vk_history_pre_exposure_scale(
                         renderer, renderer->cloud_histories[selected_index]
                                       .producer_submit_value)
                   : 1.0f,
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanCloudTraceRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_CLOUD_TRACE];
  prepared->groups[0][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[0][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  return true_v;
}

/* The cloud-lit sky light (ADR-074): every texel of the clear sky source's
   16-texel mip is seen through this frame's cloud layer from the camera and
   projected into this frame slot's SH slot with the source's own deringing,
   and the cloud's in-scatter and transmittance halve down to one texel for
   global reflections. Frame roots prepared afterwards read that slot as the
   global sky light and the chain through the sky record; the layer itself
   keeps the clear slot through the sky record. A
   frame without a published source or its exact-texel view dispatches
   nothing and keeps the clear slot. */
bool8_t vkr_vk_prepare_cloud_sky_light(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass) {
  (void)pass;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  VkrVulkanPublishedTexture *source = slot->ibl_source;
  uint32_t mip = 0u;
  uint32_t extent = 0u;
  prepared->dispatch_count = 0u;
  if (!slot->ibl_ready || !source || !slot->sky || !slot->sky_record ||
      source->ibl_sh_slot == VKR_SH_SLOT_BLACK ||
      source->ibl_sh_texel_view == VK_NULL_HANDLE)
    return true_v;
  /* The first mip no wider than the composite's face. */
  extent = source->image.width;
  while (extent > VKR_CLOUD_SKY_LIGHT_FACE_SIZE &&
         mip + 1u < source->image.mip_levels) {
    extent = Max(1u, extent >> 1u);
    ++mip;
  }
  if (extent > VKR_CLOUD_SKY_LIGHT_FACE_SIZE || extent == 0u)
    return true_v;
  _Static_assert(VKR_VULKAN_FRAME_SLOT_COUNT <= VKR_SH_CLOUD_SLOT_COUNT,
                 "Each Vulkan frame slot needs its own cloud SH slot");

  /* The march reads the clear slot through the sky record; this root is
     filled before the frame's global slot moves. */
  uint64_t frame = 0u;
  VkrVulkanPacketFrameRoot *frame_root = vkr_vk_packet_frame_root(slot, &frame);
  if (!frame_root)
    return false_v;
  const VkrPacketFrameConstants constants = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &constants, slot->gpu_candidate_instances,
      packet->temporal.current_view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, VKR_VULKAN_SENTINEL_SLOT_INDEX, true_v);
  /* The chain of a 16-texel face holds every smaller one too. */
  uint64_t radiance = 0u;
  if (!vkr_vk_frame_upload_allocate(
          slot, (uint64_t)VKR_CLOUD_SKY_LIGHT_TEXELS * 4u * sizeof(float32_t),
          4u * sizeof(float32_t), &radiance, NULL))
    return false_v;

  const uint32_t cloud_slot =
      VKR_SH_CLOUD_SLOT_FIRST + renderer->active_frame_slot;
  /* Cloud radiance is pre-exposed; the source holds radiance scaled by
     2^radiance_stops. */
  const VkrVulkanCloudSkyLightRoot root = {
      .sky = slot->sky,
      .frame = frame,
      .radiance = radiance,
      .destination = renderer->sh_coefficients.address +
                     (uint64_t)cloud_slot * VKR_SH_SLOT_BYTES,
      .source_texture = source->ibl_sh_texel_slot.index,
      .source_mip = mip,
      .face_size = extent,
      .cloud_to_source_scale = vkr_ldexp_f32(1.0f, source->radiance_stops) /
                               Max(constants.pre_exposure, 1e-20f),
      .window_band_0 = vkr_ibl_sh_window_factor(0u, source->ibl_sh_deringing),
      .window_band_1 = vkr_ibl_sh_window_factor(1u, source->ibl_sh_deringing),
      .window_band_2 = vkr_ibl_sh_window_factor(2u, source->ibl_sh_deringing),
  };
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanCloudSkyLightRoot),
                                 &prepared->root_address))
    return false_v;
  source->last_use_submit_value = renderer->submit_value + 1u;
  prepared->pipelines[0] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_CLOUD_SKY_LIGHT];
  prepared->groups[0][0] = (extent + 7u) / 8u;
  prepared->groups[0][1] = (extent + 7u) / 8u;
  prepared->groups[0][2] = 6u;
  prepared->pipelines[1] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_CLOUD_SKY_LIGHT_SH];
  prepared->groups[1][0] = 1u;
  prepared->groups[1][1] = 1u;
  prepared->groups[1][2] = 1u;
  prepared->pipelines[2] =
      renderer->deferred_pipelines
          [VKR_VULKAN_DEFERRED_PIPELINE_CLOUD_SKY_LIGHT_MIPS];
  prepared->groups[2][0] = 1u;
  prepared->groups[2][1] = 1u;
  prepared->groups[2][2] = 6u;
  prepared->dispatch_count = 3u;
  slot->sh_global_slot = cloud_slot;
  /* Global reflections read the chain through the sky record. */
  slot->sky_record->cloud_sky_light_face_size = extent;
  slot->sky_record->cloud_sky_light = radiance;
  return true_v;
}

void vkr_vk_mark_cloud_submitted(VkrVulkanRenderer *renderer,
                                 uint64_t submit_value) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (slot->cloud_history_input)
    slot->cloud_history_input->last_use_submit_value = submit_value;
  const uint32_t current = renderer->history_output_index;
  if (!slot->cloud_history_output ||
      current >= VKR_VULKAN_HISTORY_INSTANCE_COUNT)
    return;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  if ((packet->temporal.reset_reasons & ~VKR_CLOUD_HISTORY_KEPT_RESETS) !=
      VKR_TEMPORAL_RESET_NONE) {
    for (uint32_t i = 0u; i < VKR_VULKAN_HISTORY_INSTANCE_COUNT; ++i)
      renderer->cloud_histories[i].valid = false_v;
  }
  VkrVulkanGraphImage *history =
      vkr_vk_temporal_graph_image(renderer, "cloud_history");
  renderer->cloud_histories[current] = (VkrVulkanCloudHistory){
      .view_projection = packet->sky.view_projection,
      .signature = vkr_cloud_history_signature(&packet->sky.clouds,
                                               packet->sky.camera_position.w),
      .producer_submit_value = submit_value,
      .frame_index = packet->input.frame.frame_index,
      .dimensions = {slot->cloud_history_output->image.width,
                     slot->cloud_history_output->image.height},
      .graph_generation = history ? history->graph_generation : 0u,
      .valid = history != NULL,
  };
  slot->cloud_history_output->history_producer_submit_value = submit_value;
  slot->cloud_history_output->history_valid = history != NULL;
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

/* Cascade layers with content once this frame's drawn and copied layers
   are written. */
vkr_internal uint32_t vkr_vk_froxel_shadow_valid_mask(
    const VkrPreparedFrame *packet, uint32_t retained_mask) {
  return retained_mask |
         (packet->input.shadow ? packet->input.shadow->cascade_render_mask |
                                     packet->input.shadow->copy_mask
                               : 0u);
}

vkr_internal uint64_t vkr_vk_froxel_local_shadow_valid_mask(
    const VkrPreparedFrame *packet, uint64_t retained_mask) {
  return retained_mask |
         vkr_local_shadow_render_layer_mask(packet->input.local_shadow);
}

vkr_internal uint64_t vkr_vk_froxel_local_shadow_transmission_valid_mask(
    const VkrPreparedFrame *packet, uint64_t retained_mask) {
  return retained_mask |
         vkr_local_shadow_render_transmission_mask(packet->input.local_shadow);
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
  vkr_vulkan_renderer_retained_local_shadow_token(renderer,
                                                  &local_shadow_token);
  const uint32_t shadow_valid_mask =
      vkr_vk_froxel_shadow_valid_mask(packet, shadow_token.valid_layer_mask);
  const uint64_t local_shadow_valid_mask =
      vkr_vk_froxel_local_shadow_valid_mask(
          packet, local_shadow_token.valid_layer_mask);
  const uint64_t transmission_valid_mask =
      renderer->prepared_frame.local_shadow_transmission_layer_count > 0u
          ? vkr_vk_froxel_local_shadow_transmission_valid_mask(
                packet, local_shadow_token.transmission_valid_layer_mask)
          : 0u;
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
          candidate->local_shadow_transmission_valid_layer_mask !=
              transmission_valid_mask ||
          MemCompare(
              candidate->local_shadow_transmission_generations,
              local_shadow_token.transmission_resource_generations,
              sizeof(candidate->local_shadow_transmission_generations)) != 0 ||
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
      .history_pre_exposure_scale =
          slot->froxel_history_valid
              ? vkr_vk_history_pre_exposure_scale(
                    renderer, source->history_producer_submit_value)
              : 1.0f,
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
  /* Later passes sample what this one writes: they take the image's sampled
     slot, which the storage slot's table does not share. */
  const VkrRgImageUse *output = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  const VkrVulkanGraphImageInstance *output_image =
      output ? vkr_vk_deferred_image(renderer, output->image) : NULL;
  if (!output_image || !output_image->has_sampled_slot)
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!slot->froxel_fog_params)
    return false_v;
  slot->froxel_integrated_texture = output_image->sampled_slot.index;
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
  uint32_t aerial = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (!vkr_vk_deferred_storage_index(renderer, pass, 0u, &target) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u, &integrated) ||
      (renderer->prepared_frame.aerial_perspective_enabled &&
       !vkr_vk_deferred_sampled_index(renderer, pass, 3u, &aerial)))
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
      .aerial_perspective_texture = aerial,
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
  vkr_vulkan_renderer_retained_local_shadow_token(renderer,
                                                  &local_shadow_token);
  const uint32_t shadow_valid_mask =
      vkr_vk_froxel_shadow_valid_mask(packet, shadow_token.valid_layer_mask);
  const uint64_t local_shadow_valid_mask =
      vkr_vk_froxel_local_shadow_valid_mask(
          packet, local_shadow_token.valid_layer_mask);
  const uint64_t transmission_valid_mask =
      renderer->prepared_frame.local_shadow_transmission_layer_count > 0u
          ? vkr_vk_froxel_local_shadow_transmission_valid_mask(
                packet, local_shadow_token.transmission_valid_layer_mask)
          : 0u;
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
      .local_shadow_transmission_valid_layer_mask = transmission_valid_mask,
      .valid = true_v,
  };
  MemCopy(history->local_shadow_transmission_generations,
          local_shadow_token.transmission_resource_generations,
          sizeof(history->local_shadow_transmission_generations));
  slot->froxel_history_output->history_producer_submit_value = submit_value;
  slot->froxel_history_output->history_valid = true_v;
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

/* Copies the exposure state that submission `producer` wrote from its frame
   slot's readback. A slot keeps that copy until it records another frame, and
   a completed producer's slot has not recorded one before its retire value
   changes. */
vkr_internal bool8_t vkr_vk_completed_exposure_readback(
    VkrVulkanRenderer *renderer, uint64_t producer,
    VkrExposureGpuState *out_state) {
  if (!producer || producer > renderer->completed_value)
    return false_v;
  for (uint32_t i = 0u; i < VKR_VULKAN_FRAME_SLOT_COUNT; ++i) {
    const VkrVulkanFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->retire_value != producer || !slot->exposure_requested ||
        !slot->readback.allocation.mapped)
      continue;
    if (!vkr_vk_invalidate(renderer, &slot->readback.allocation,
                           VKR_VULKAN_READBACK_EXPOSURE_STATE_OFFSET,
                           sizeof(*out_state)))
      return false_v;
    MemCopy(out_state,
            (const uint8_t *)slot->readback.allocation.mapped +
                VKR_VULKAN_READBACK_EXPOSURE_STATE_OFFSET,
            sizeof(*out_state));
    return true_v;
  }
  return false_v;
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
  /* The chosen record is complete, so its frame's readback also holds it. Its
     host copy goes up with this frame's roots and the GPU never reads the old
     instance again. A GPU read would keep that instance in use until this
     frame completes, and the oldest history instances are the ones the
     history ring reuses next, so the ring would wait for in-flight frames. */
  VkrExposureGpuState host_previous = {0};
  const bool8_t host_copy =
      previous &&
      vkr_vk_completed_exposure_readback(
          renderer, previous->history_producer_submit_value, &host_previous);
  uint64_t host_previous_address = 0u;
  if (host_copy && !vkr_vk_deferred_push_root(
                       renderer, &host_previous, sizeof(host_previous),
                       _Alignof(VkrExposureGpuState), &host_previous_address))
    return false_v;
  if (previous) {
    root.metering.delta_seconds = vkr_exposure_history_delta(
        renderer->exposure_seconds + root.metering.delta_seconds,
        previous->history_exposure_seconds);
  }
  if (previous && !host_copy) {
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
    prepared->buffer_barrier = barrier;
    prepared->has_buffer_barrier = true_v;
  } else if (!previous) {
    /* No completed record. The kernel still reads this address and discards the
       value through `history_valid`, so it points at the output instance rather
       than at nothing. */
    root.metering.history_valid = 0u;
  }
  root.state = output->buffer.address;
  root.previous_state = host_copy  ? host_previous_address
                        : previous ? previous->buffer.address
                                   : output->buffer.address;

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
  static const uint32_t bindings[] = {0u, 1u, 2u,  3u,  4u,
                                      7u, 9u, 10u, 11u, 12u};
  uint32_t textures[ArrayCount(bindings)] = {0};
  for (uint32_t i = 0u; i < ArrayCount(bindings); ++i) {
    if (i >= 7u && !vkr_rg_pass_find_image_use(&pass->desc, bindings[i], 0u)) {
      continue;
    }
    if (!vkr_vk_deferred_sampled_index(renderer, pass, bindings[i],
                                       &textures[i]))
      return false_v;
  }
  uint32_t destination = 0u;
  VkrVulkanGraphBufferInstance *visible =
      vkr_vk_deferred_buffer(renderer, pass, 6u);
  if (!visible ||
      !vkr_vk_deferred_storage_index(renderer, pass, 8u, &destination))
    return false_v;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t frame_address = 0u;
  VkrVulkanPacketFrameRoot *frame_root =
      vkr_vk_packet_frame_root(slot, &frame_address);
  if (!frame_root)
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                        packet->input.globals.view);
  const VkrPacketFrameConstants frame = vkr_packet_derive_frame_constants(
      packet, renderer->prepared_frame.viewport_width,
      renderer->prepared_frame.viewport_height);
  vkr_vk_fill_packet_frame_root(
      renderer, frame_root, slot, &frame, slot->gpu_candidate_instances,
      view_projection, VKR_VULKAN_SENTINEL_SLOT_INDEX,
      VKR_VULKAN_SENTINEL_SLOT_INDEX, VKR_VULKAN_SENTINEL_SLOT_INDEX, true_v);
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
                                 _Alignof(VkrVulkanSubsurfaceRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[0] =
      renderer
          ->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SUBSURFACE_GATHER];
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

/* An SMAA pass's root with its storage target at `destination_binding`;
   the caller adds the sampled inputs it reads. */
vkr_internal bool8_t vkr_vk_smaa_root(VkrVulkanRenderer *renderer,
                                      const VkrRgPass *pass,
                                      uint32_t destination_binding,
                                      VkrVulkanSmaaRoot *out_root) {
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, destination_binding, 0u);
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  uint32_t destination_texture = 0u;
  if (!destination ||
      !vkr_vk_deferred_storage_index(renderer, pass, destination_binding,
                                     &destination_texture))
    return false_v;
  *out_root = (VkrVulkanSmaaRoot){
      .source_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .destination_texture = destination_texture,
      .edges_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .blend_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .area_texture =
          renderer->smaa_texture_slots[VKR_VULKAN_SMAA_TABLE_AREA].index,
      .search_texture =
          renderer->smaa_texture_slots[VKR_VULKAN_SMAA_TABLE_SEARCH].index,
      .linear_sampler = renderer->dfg_sampler_slot.index,
      .point_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX,
      .extent = {destination->image.width, destination->image.height},
  };
  return true_v;
}

vkr_internal bool8_t vkr_vk_prepare_dispatch_smaa(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrVulkanSmaaRoot *root, VkrVulkanDeferredPipeline pipeline) {
  if (!vkr_vk_deferred_push_root(renderer, root, sizeof(*root),
                                 _Alignof(VkrVulkanSmaaRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[pipeline];
  prepared->groups[prepared->dispatch_count][0] = (root->extent[0] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][1] = (root->extent[1] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
  return true_v;
}

bool8_t vkr_vk_prepare_smaa_edges(VkrVulkanRenderer *renderer,
                                  VkrVulkanPreparedCompute *prepared,
                                  const VkrRgPass *pass) {
  VkrVulkanSmaaRoot root = {0};
  if (!vkr_vk_smaa_root(renderer, pass, 1u, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &root.source_texture))
    return false_v;
  /* Binding 2: the camera's visibility image, which predicates the edges. */
  if (vkr_rg_pass_find_image_use(&pass->desc, 2u, 0u) &&
      !vkr_vk_deferred_storage_index(renderer, pass, 2u,
                                     &root.predication_texture)) {
    return false_v;
  }
  return vkr_vk_prepare_dispatch_smaa(renderer, prepared, &root,
                                      VKR_VULKAN_DEFERRED_PIPELINE_SMAA_EDGES);
}

/* `sample_one`: SMAA S2x's second sample, whose area-texture rows differ
   from the first's (SMAA 4x). */
bool8_t vkr_vk_prepare_smaa_weights(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass, bool8_t sample_one) {
  VkrVulkanSmaaRoot root = {0};
  if (!vkr_vk_smaa_root(renderer, pass, 1u, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u, &root.edges_texture))
    return false_v;
  const VkrPreparedFrame *packet = renderer->graph->packet;
  root.subsample_indices = sample_one
                               ? packet->temporal.smaa_subsample_indices_sample1
                               : packet->temporal.smaa_subsample_indices;
  return vkr_vk_prepare_dispatch_smaa(
      renderer, prepared, &root, VKR_VULKAN_DEFERRED_PIPELINE_SMAA_WEIGHTS);
}

/* `sample_one`: SMAA S2x's second sample, averaged at half weight into the
   first sample's antialiased image (the reference's 0.5 alpha blend). */
bool8_t vkr_vk_prepare_smaa_blend(VkrVulkanRenderer *renderer,
                                  VkrVulkanPreparedCompute *prepared,
                                  const VkrRgPass *pass, bool8_t sample_one) {
  VkrVulkanSmaaRoot root = {0};
  if (!vkr_vk_smaa_root(renderer, pass, 2u, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u,
                                     &root.source_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &root.blend_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 3u, &root.motion_texture))
    return false_v;
  root.blend_half = sample_one ? 1u : 0u;
  return vkr_vk_prepare_dispatch_smaa(renderer, prepared, &root,
                                      VKR_VULKAN_DEFERRED_PIPELINE_SMAA_BLEND);
}

/* SMAA T2x's resolve. The antialiased subsample at binding 0 is a history
   image: this frame writes the current instance, and the resolve blends it
   with the previous frame's instance, chosen as the temporal tuple is. */
bool8_t vkr_vk_prepare_smaa_resolve(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrRgImageUse *current_use =
      vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u);
  VkrVulkanGraphImage *outputs =
      current_use && current_use->image.id >= 1u &&
              current_use->image.id <= renderer->graph->images.length
          ? &renderer->graph_images[current_use->image.id - 1u]
          : NULL;
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  VkrVulkanSmaaResolveRoot root = {
      .linear_sampler = renderer->dfg_sampler_slot.index,
      .point_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX,
  };
  if (!outputs || !destination ||
      outputs->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u,
                                     &root.current_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u,
                                     &root.motion_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 2u,
                                     &root.accept_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 3u,
                                     &root.destination_texture))
    return false_v;
  const uint32_t current = renderer->history_output_index;
  VkrVulkanGraphImageInstance *output = &outputs->instances[current];
  slot->smaa_history_output = output;
  slot->smaa_history_input = NULL;
  VkrVulkanGraphImageInstance *previous = NULL;
  if (packet->temporal.history_valid) {
    for (uint32_t i = 0u; i < outputs->instance_count; ++i) {
      VkrVulkanGraphImageInstance *candidate = &outputs->instances[i];
      if (i == current || !candidate->history_valid ||
          !candidate->has_sampled_slot ||
          candidate->history_frame_index + 1u !=
              packet->input.frame.frame_index ||
          candidate->history_scene_generation !=
              packet->input.frame.scene_generation ||
          candidate->history_width != output->image.width ||
          candidate->history_height != output->image.height)
        continue;
      if (!previous || candidate->history_producer_submit_value >
                           previous->history_producer_submit_value)
        previous = candidate;
    }
  }
  if (previous) {
    /* Last frame's resolve sampled it last, possibly still in flight. */
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
        .image = previous->image.handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1u,
                .layerCount = 1u,
            },
    };
    prepared->image_barrier_count = 1u;
    slot->smaa_history_input = previous;
  }
  root.previous_texture =
      previous ? previous->sampled_slot.index : root.current_texture;
  root.history_valid = previous != NULL;
  root.extent[0] = destination->image.width;
  root.extent[1] = destination->image.height;
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanSmaaResolveRoot),
                                 &prepared->root_address))
    return false_v;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_SMAA_RESOLVE];
  prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
  return true_v;
}

/* SMAA S2x: sample zero of the multisampled visibility raster becomes the
   single-sample visibility image every consumer reads; sample one and its
   depth go to their own images for the second sample's shading. */
bool8_t vkr_vk_prepare_vbuffer_split(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass) {
  VkrVulkanVbufferSplitRoot root = {0};
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 2u, 0u);
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  if (!destination ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 0u,
                                     &root.ms_vbuffer_texture) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u,
                                     &root.ms_depth_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u,
                                     &root.vbuffer_storage) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 3u,
                                     &root.vbuffer_s1_storage) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u,
                                     &root.depth_s1_storage) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 5u,
                                     &root.edge_mask_storage)) {
    return false_v;
  }
  VkrVulkanGraphBufferInstance *arguments =
      vkr_vk_deferred_buffer(renderer, pass, 6u);
  VkrVulkanGraphBufferInstance *pixels =
      vkr_vk_deferred_buffer(renderer, pass, 7u);
  if (!arguments || !pixels) {
    return false_v;
  }
  root.extent[0] = destination->image.width;
  root.extent[1] = destination->image.height;
  root.sample1_arguments = arguments->buffer.address;
  root.sample1_pixels = pixels->buffer.address;
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanVbufferSplitRoot),
                                 &prepared->root_address)) {
    return false_v;
  }
  /* The list empties before the split appends to it; the record path orders
     the two dispatches. */
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines
          [VKR_VULKAN_DEFERRED_PIPELINE_MSAA_SAMPLE1_LIST_RESET];
  prepared->groups[prepared->dispatch_count][0] = 1u;
  prepared->groups[prepared->dispatch_count][1] = 1u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_VBUFFER_SPLIT];
  prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
  return true_v;
}

/* SMAA S2x: the second sample's colour for display. Bindings: the first
   sample's resolved colour (0), the second's lit colour (1), the edge mask
   (2), the visibility image after the blended overlays (3), the first
   transmission layer (4, when transmission is pending), the output (5) and
   the first sample's bloomed colour (6, with bloom). */
bool8_t vkr_vk_prepare_msaa_compose(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass) {
  VkrVulkanMsaaComposeRoot root = {0};
  const VkrRgImageUse *destination_use =
      vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u);
  VkrVulkanGraphImageInstance *destination =
      destination_use ? vkr_vk_deferred_image(renderer, destination_use->image)
                      : NULL;
  if (!destination ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u,
                                     &root.sample0_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 1u,
                                     &root.sample1_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 2u,
                                     &root.edge_mask_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 3u,
                                     &root.vbuffer_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 5u,
                                     &root.destination_texture)) {
    return false_v;
  }
  if (vkr_rg_pass_find_image_use(&pass->desc, 4u, 0u) &&
      !vkr_vk_deferred_storage_index(renderer, pass, 4u,
                                     &root.transmission_vbuffer_texture)) {
    return false_v;
  }
  if (vkr_rg_pass_find_image_use(&pass->desc, 6u, 0u) &&
      !vkr_vk_deferred_storage_index(renderer, pass, 6u, &root.post_texture)) {
    return false_v;
  }
  root.extent[0] = destination->image.width;
  root.extent[1] = destination->image.height;
  if (!vkr_vk_deferred_push_root(renderer, &root, sizeof(root),
                                 _Alignof(VkrVulkanMsaaComposeRoot),
                                 &prepared->root_address)) {
    return false_v;
  }
  prepared->pipelines[prepared->dispatch_count] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_MSAA_COMPOSE];
  prepared->groups[prepared->dispatch_count][0] = (root.extent[0] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][1] = (root.extent[1] + 7u) / 8u;
  prepared->groups[prepared->dispatch_count][2] = 1u;
  prepared->dispatch_count++;
  return true_v;
}

bool8_t vkr_vk_prepare_gtao_denoise(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass, bool8_t final_pass) {
  VkrVulkanGtaoRoot root = {0};
  if (!vkr_vk_gtao_root(renderer, pass, 0u, 2u, &root) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &root.edges_texture))
    return false_v;
  root.denoise_final = final_pass ? 1u : 0u;
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
  uint32_t depth_texture = 0u;
  if (!opaque ||
      !vkr_vk_deferred_storage_index(renderer, pass, 0u, &opaque_vbuffer) ||
      !vkr_vk_deferred_sampled_index(renderer, pass, 1u, &depth_texture) ||
      !vkr_vk_deferred_storage_index(renderer, pass, 4u, &output) ||
      !slot->readback.address)
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
      .depth_texture = depth_texture,
      .depth_output =
          slot->readback.address + VKR_VULKAN_READBACK_PICKING_DEPTH_OFFSET,
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
  VkrVulkanTransmissionRoot root = {
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
          renderer->prepared_frame.transmission_gpu_draw_candidate_capacity,
      .geometry_count = renderer->config.geometry_capacity,
      .material_count = renderer->config.material_slot_capacity,
      .instance_count = slot->transmission_gpu_candidate_count,
      .pixel_capacity = (uint32_t)pixel_capacity,
      .compact_layer = layer,
      .compact_enabled = compact,
  };
  /* Baked lamps (ADR-104) on lightmapped transmissive draws: the resolve's
     lightmap binding, present on frames that sample baked lamps. */
  root.lightmap_direction_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
  if (slot->lightmap_rects != 0u) {
    const VkrLightmapBinding *lightmap = &packet->input.lighting->lightmap;
    root.lightmap_rects = slot->lightmap_rects;
    root.lightmap_texture = slot->lightmap_texture;
    root.lightmap_sampler = slot->lightmap_sampler;
    root.lightmap_direction_texture = slot->lightmap_direction_texture;
    root.lightmap_rect_count = lightmap->rect_count;
    root.lightmap_layer_count = lightmap->layer_count;
    root.lightmap_active_count = lightmap->active_layer_count;
    root.lightmap_inverse_page_size = 1.0f / (float32_t)lightmap->page_size;
    for (uint32_t i = 0u; i < lightmap->active_layer_count; ++i) {
      root.lightmap_active_layers[i] = lightmap->active_layers[i];
      root.lightmap_active_weights[i] = lightmap->active_weights[i];
    }
  }
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
  /* Every opaque bucket, mirrored or not, draws before any cutout bucket, so
     alpha tests reject against all opaque depth, and opaque and cutout
     pipelines each bind once. */
  static const uint32_t bucket_order[] = {
      VKR_WORLD_DRAW_STATE_OPAQUE_BACK,
      VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED,
      VKR_WORLD_DRAW_STATE_OPAQUE_BACK_MIRRORED,
      VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED_MIRRORED,
      VKR_WORLD_DRAW_STATE_CUTOUT_BACK,
      VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED,
      VKR_WORLD_DRAW_STATE_CUTOUT_BACK_MIRRORED,
      VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED_MIRRORED,
  };
  _Static_assert(ArrayCount(bucket_order) == VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
                 "every visibility bucket draws exactly once");

  vkCmdBindIndexBuffer(command, prepared->indices, 0u, VK_INDEX_TYPE_UINT32);
  VkPipeline bound = VK_NULL_HANDLE;
  for (uint32_t i = 0u; i < ArrayCount(bucket_order); ++i) {
    const uint32_t bucket = bucket_order[i];
    /* A bucket without packed candidates has no command partition. */
    if (prepared->max_draw_counts[bucket] == 0u)
      continue;
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
        prepared->max_draw_counts[bucket],
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
  if (prepared->light_contribution) {
    /* The graph does not track this slot-owned buffer, so its clear orders
     * itself before deferred lighting's atomics. */
    vkCmdFillBuffer(command, prepared->light_contribution, 0u,
                    VKR_VULKAN_LIGHT_CONTRIBUTION_SIZE, 0u);
    const VkBufferMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = prepared->light_contribution,
        .size = VKR_VULKAN_LIGHT_CONTRIBUTION_SIZE,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1u,
        .pBufferMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
}

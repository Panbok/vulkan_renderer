#include "renderer/vulkan/bindless/vkr_bindless_vulkan_internal.h"

vkr_internal void vkr_bindless_vk_graph_noop(VkrRgPassContext *ctx,
                                             void *user_data) {
  (void)ctx;
  (void)user_data;
}

typedef enum VkrBindlessVkGraphExecutorKind {
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_SHADOW = 0,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING_READBACK,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_IBL_BAKE,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_SKYBOX,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_OPAQUE,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_TRANSMISSION,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_BLEND,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_TONEMAP,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_EDITOR,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_UI,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_COUNT,
} VkrBindlessVkGraphExecutorKind;

typedef struct VkrBindlessVkGraphExecutorSpec {
  const char *name;
  VkrRgPassType type;
} VkrBindlessVkGraphExecutorSpec;

vkr_global const VkrBindlessVkGraphExecutorSpec
    s_bindless_vk_graph_executors[] = {
        {"pass.shadow.cascade", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.picking", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.picking.readback", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.ibl_bake", VKR_RG_PASS_TYPE_COMPUTE},
        {"pass.skybox", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.world.opaque", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.copy.pre_transmission.fullscreen", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.copy.pre_transmission.editor", VKR_RG_PASS_TYPE_TRANSFER},
        {"pass.world.transmission", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.world.blend", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.tonemap", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.editor", VKR_RG_PASS_TYPE_GRAPHICS},
        {"pass.ui", VKR_RG_PASS_TYPE_GRAPHICS},
};
_Static_assert(ArrayCount(s_bindless_vk_graph_executors) ==
                   VKR_BINDLESS_VK_GRAPH_EXECUTOR_COUNT,
               "Bindless Vulkan graph executor table is incomplete");

vkr_internal bool8_t vkr_bindless_vk_graph_executor_kind(
    const VkrRgPass *pass, VkrBindlessVkGraphExecutorKind *out_kind) {
  const uintptr_t encoded = (uintptr_t)pass->desc.user_data;
  if (!encoded || encoded > VKR_BINDLESS_VK_GRAPH_EXECUTOR_COUNT)
    return false_v;
  *out_kind = (VkrBindlessVkGraphExecutorKind)(encoded - 1u);
  return true_v;
}

bool8_t
vkr_bindless_vk_register_graph_executors(VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < ArrayCount(s_bindless_vk_graph_executors); ++i) {
    const VkrBindlessVkGraphExecutorSpec *spec =
        &s_bindless_vk_graph_executors[i];
    const VkrRgPassExecutor executor = {
        .name = string8_create_from_cstr((const uint8_t *)spec->name,
                                         string_length(spec->name)),
        .execute = vkr_bindless_vk_graph_noop,
        .user_data = (void *)(uintptr_t)(i + 1u),
    };
    if (!vkr_rg_executor_registry_register(&renderer->executors, &executor))
      return false_v;
  }
  return true_v;
}

bool8_t
vkr_bindless_vk_validate_graph(const VkrBindlessVulkanRenderer *renderer) {
  for (uint64_t order = 0; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t pass_index =
        *vector_get_uint32_t(&renderer->graph->execution_order, order);
    const VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, pass_index);
    VkrBindlessVkGraphExecutorKind kind;
    if (!vkr_bindless_vk_graph_executor_kind(pass, &kind)) {
      log_error("Bindless Vulkan graph pass '%.*s' has no executor kind",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    const VkrBindlessVkGraphExecutorSpec *executor =
        &s_bindless_vk_graph_executors[kind];
    if (pass->desc.type != executor->type) {
      log_error("Bindless Vulkan graph pass '%.*s' has type %u; executor '%s' "
                "requires type %u",
                (int)pass->desc.name.length, pass->desc.name.str,
                (uint32_t)pass->desc.type, executor->name,
                (uint32_t)executor->type);
      return false_v;
    }
    for (uint64_t i = 0; i < pass->pre_image_barriers.length; ++i) {
      const VkrRgImageBarrier *barrier =
          vector_get_VkrRgImageBarrier(&pass->pre_image_barriers, i);
      VkrBindlessVkDependency lowered = {0};
      const VkrBindlessVkDependencyResult result =
          vkr_bindless_vk_lower_image_dependency(
              barrier->src_access, barrier->dst_access, &barrier->dependency,
              barrier->src_layout != barrier->dst_layout, &lowered);
      if (result != VKR_BINDLESS_VK_DEPENDENCY_OK) {
        log_error("Bindless Vulkan graph pass '%.*s' image dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_bindless_vk_dependency_result_string(result));
        return false_v;
      }
    }
    for (uint64_t i = 0; i < pass->pre_buffer_barriers.length; ++i) {
      const VkrRgBufferBarrier *barrier =
          vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
      VkrBindlessVkDependency lowered = {0};
      const VkrBindlessVkDependencyResult result =
          vkr_bindless_vk_lower_buffer_dependency(
              barrier->src_access, barrier->dst_access, &barrier->dependency,
              &lowered);
      if (result != VKR_BINDLESS_VK_DEPENDENCY_OK) {
        log_error("Bindless Vulkan graph pass '%.*s' buffer dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_bindless_vk_dependency_result_string(result));
        return false_v;
      }
    }
  }
  return true_v;
}

vkr_internal VkImageUsageFlags
vkr_bindless_vk_graph_image_usage(VkrTextureUsageFlags usage) {
  VkImageUsageFlags result = 0;
  if (usage.set & VKR_TEXTURE_USAGE_SAMPLED)
    result |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_STORAGE)
    result |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_COLOR_ATTACHMENT)
    result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT)
    result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_SRC)
    result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_DST)
    result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  return result;
}

vkr_internal bool8_t vkr_bindless_vk_graph_image_desc_equal(
    const VkrRgImageDesc *a, const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->format == b->format && a->usage.set == b->usage.set &&
         a->samples == b->samples && a->layers == b->layers &&
         a->mip_levels == b->mip_levels && a->type == b->type &&
         a->flags == b->flags;
}

vkr_internal void vkr_bindless_vk_destroy_graph_image_instance(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkGraphImageInstance *instance) {
  const VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  if (instance->has_sampled_slot) {
    (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                    instance->sampled_slot, completed);
    (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                     NULL);
  }
  if (instance->has_storage_slot) {
    (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                    instance->storage_slot, completed);
    (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                     NULL);
  }
  for (uint32_t layer = 0; layer < VKR_BINDLESS_VK_GRAPH_LAYER_MAX; ++layer) {
    if (instance->layer_views[layer])
      vkDestroyImageView(device, instance->layer_views[layer], NULL);
  }
  vkr_bindless_vk_destroy_image(renderer, &instance->image);
  MemZero(instance, sizeof(*instance));
}

void vkr_bindless_vk_destroy_graph_image(VkrBindlessVulkanRenderer *renderer,
                                         VkrBindlessVkGraphImage *slot) {
  if (!slot || slot->external_swapchain) {
    if (slot)
      MemZero(slot, sizeof(*slot));
    return;
  }
  for (uint32_t i = 0; i < slot->instance_count; ++i)
    vkr_bindless_vk_destroy_graph_image_instance(renderer, &slot->instances[i]);
  MemZero(slot, sizeof(*slot));
}

vkr_internal bool8_t vkr_bindless_vk_create_graph_image_instance(
    VkrBindlessVulkanRenderer *renderer, const VkrRgImageDesc *desc,
    VkrBindlessVkGraphImageInstance *out_instance) {
  const VkFormat format = vkr_bindless_vk_texture_format(desc->format);
  VkImageUsageFlags usage = vkr_bindless_vk_graph_image_usage(desc->usage);
  /* A capture request arrives with a packet, after graph images have already

   * been realized. Capture-enabled renderers therefore provision graph-owned

   * images as legal transfer sources up front; no per-capture image churn is

   * allowed in the frame path. */
  if (renderer->config.capture_ring_capacity > 0u)
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (format == VK_FORMAT_UNDEFINED || usage == 0u || desc->samples != 1u ||
      desc->layers == 0u || desc->layers > VKR_BINDLESS_VK_GRAPH_LAYER_MAX ||
      desc->mip_levels == 0u)
    return false_v;
  const bool8_t array_view =
      desc->layers > 1u || (desc->flags & VKR_RG_RESOURCE_FLAG_FORCE_ARRAY);
  if (!vkr_bindless_vk_create_image_ex(
          renderer, desc->width, desc->height, desc->mip_levels, desc->layers,
          format, 0u,
          array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
          usage, &out_instance->image))
    return false_v;
  if (desc->layers == 1u)
    goto publish_descriptors;
  const VkImageAspectFlags aspects = vkr_bindless_vk_format_aspects(format);
  for (uint32_t layer = 0; layer < desc->layers; ++layer) {
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out_instance->image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange =
            {
                .aspectMask = aspects,
                .levelCount = desc->mip_levels,
                .baseArrayLayer = layer,
                .layerCount = 1u,
            },
    };
    if (vkCreateImageView(vkr_bindless_vk_renderer_device(renderer), &view_info,
                          NULL,
                          &out_instance->layer_views[layer]) != VK_SUCCESS) {
      vkr_bindless_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
  }
publish_descriptors:
  if ((desc->usage.set & VKR_TEXTURE_USAGE_SAMPLED) != 0u) {
    if (!vkr_bindless_vk_publish_sampled_view(
            renderer, out_instance->image.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            &out_instance->sampled_slot)) {
      vkr_bindless_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_sampled_slot = true_v;
  }
  if ((desc->usage.set & VKR_TEXTURE_USAGE_STORAGE) != 0u) {
    if (!vkr_bindless_vk_publish_storage_view(
            renderer, out_instance->image.view, &out_instance->storage_slot)) {
      vkr_bindless_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_storage_slot = true_v;
  }
  return true_v;
}

bool8_t
vkr_bindless_vk_realize_graph_images(VkrBindlessVulkanRenderer *renderer) {
  if (renderer->graph->images.length > renderer->config.max_graph_images)
    return false_v;
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrBindlessVkGraphImage *slot = &renderer->graph_images[i];
    const bool8_t external_swapchain =
        image->imported && vkr_string8_equals_cstr(&image->name, "swapchain");
    const uint32_t instance_count =
        (image->desc.flags & VKR_RG_RESOURCE_FLAG_PER_IMAGE)
            ? renderer->targets.image_count
            : 1u;
    if (slot->live && slot->graph_generation == image->generation &&
        slot->external_swapchain == external_swapchain &&
        slot->instance_count == instance_count &&
        vkr_bindless_vk_graph_image_desc_equal(&slot->desc, &image->desc))
      continue;
    if (slot->live) {
      if (!vkr_bindless_vulkan_renderer_wait_idle(renderer))
        return false_v;
      vkr_bindless_vk_destroy_graph_image(renderer, slot);
    }
    *slot = (VkrBindlessVkGraphImage){
        .desc = image->desc,
        .graph_generation = image->generation,
        .instance_count = instance_count,
        .live = true_v,
        .external_swapchain = external_swapchain,
    };
    if (external_swapchain)
      continue;
    for (uint32_t instance = 0; instance < instance_count; ++instance) {
      if (!vkr_bindless_vk_create_graph_image_instance(
              renderer, &image->desc, &slot->instances[instance])) {
        log_error("Bindless Vulkan failed to realize graph image '%.*s' "
                  "(%ux%u, format=%u, usage=0x%x, samples=%u, layers=%u, "
                  "mips=%u, instance=%u/%u)",
                  (int)image->name.length, image->name.str, image->desc.width,
                  image->desc.height, image->desc.format, image->desc.usage.set,
                  image->desc.samples, image->desc.layers,
                  image->desc.mip_levels, instance, instance_count);
        vkr_bindless_vk_destroy_graph_image(renderer, slot);
        return false_v;
      }
    }
  }
  return true_v;
}

VkrBindlessVkGraphImageInstance *
vkr_bindless_vk_graph_image(VkrBindlessVulkanRenderer *renderer,
                            VkrRgImageHandle handle, uint32_t image_index) {
  if (!vkr_rg_image_handle_valid(handle) ||
      handle.id > renderer->graph->images.length)
    return NULL;
  VkrBindlessVkGraphImage *slot = &renderer->graph_images[handle.id - 1u];
  if (!slot->live || slot->graph_generation != handle.generation)
    return NULL;
  if (slot->external_swapchain) {
    if (image_index >= renderer->targets.image_count)
      return NULL;
    /* The target set owns this image. The graph slot is only a resolving view.
     */
    slot->instances[0].image = renderer->targets.images[image_index];
    return &slot->instances[0];
  }
  const uint32_t instance = slot->instance_count > 1u ? image_index : 0u;
  return instance < slot->instance_count ? &slot->instances[instance] : NULL;
}

vkr_internal bool8_t vkr_bindless_vk_record_graph_image_barriers(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const Vector_VkrRgImageBarrier *barriers) {
  /* The scratch array is sized by max_graph_images. A pass may barrier one
     image more than once (distinct subresource ranges), so this bound is not
     implied by the image count and must be checked, not assumed. */
  if (barriers->length > renderer->config.max_graph_images) {
    log_error("Bindless Vulkan pass needs %llu image barriers; the scratch "
              "array holds max_graph_images=%u",
              (unsigned long long)barriers->length,
              renderer->config.max_graph_images);
    return false_v;
  }
  for (uint64_t i = 0; i < barriers->length; ++i) {
    const VkrRgImageBarrier *barrier =
        vector_get_VkrRgImageBarrier(barriers, i);
    VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
        renderer, barrier->image, renderer->prepared_frame.image_index);
    if (!instance)
      return false_v;
    VkrBindlessVkDependency lowered = {0};
    const VkrBindlessVkDependencyResult result =
        vkr_bindless_vk_lower_image_dependency(
            barrier->src_access, barrier->dst_access, &barrier->dependency,
            barrier->src_layout != barrier->dst_layout, &lowered);
    if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
      return false_v;
    uint32_t base_mip = 0, mip_count = 0, base_layer = 0, layer_count = 0;
    vkr_image_subresource_range_resolve(&barrier->range,
                                        instance->image.mip_levels,
                                        instance->image.array_layers, &base_mip,
                                        &mip_count, &base_layer, &layer_count);
    renderer->graph_image_barriers[i] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = lowered.src_stages,
        .srcAccessMask = lowered.src_access,
        .dstStageMask = lowered.dst_stages,
        .dstAccessMask = lowered.dst_access,
        .oldLayout = vkr_bindless_vk_texture_layout(barrier->src_layout),
        .newLayout = vkr_bindless_vk_texture_layout(barrier->dst_layout),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = instance->image.handle,
        .subresourceRange =
            {
                .aspectMask =
                    vkr_bindless_vk_format_aspects(instance->image.format),
                .baseMipLevel = base_mip,
                .levelCount = mip_count,
                .baseArrayLayer = base_layer,
                .layerCount = layer_count,
            },
    };
  }
  if (barriers->length > 0u) {
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = (uint32_t)barriers->length,
        .pImageMemoryBarriers = renderer->graph_image_barriers,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_record_graph_pass_barriers(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass) {
  /*
   * Buffer-barrier emission is designed but not implemented: no authored pass
   * declares a graph buffer, so there is no caller to validate against. The
   * graph is rejected rather than silently executed without the barrier the
   * pass asked for. Implementing it means lowering each VkrRgBufferBarrier into
   * a VkBufferMemoryBarrier2 and batching it into the same
   * vkCmdPipelineBarrier2 as the image barriers below.
   */
  if (pass->pre_buffer_barriers.length > 0u) {
    log_error("Bindless Vulkan graph pass '%.*s' declares %llu buffer "
              "barrier(s); buffer-barrier lowering is not implemented",
              (int)pass->desc.name.length, pass->desc.name.str,
              (unsigned long long)pass->pre_buffer_barriers.length);
    return false_v;
  }
  return vkr_bindless_vk_record_graph_image_barriers(renderer, command,
                                                     &pass->pre_image_barriers);
}

vkr_internal VkAttachmentLoadOp
vkr_bindless_vk_attachment_load_op(VkrAttachmentLoadOp op) {
  switch (op) {
  case VKR_ATTACHMENT_LOAD_OP_LOAD:
    return VK_ATTACHMENT_LOAD_OP_LOAD;
  case VKR_ATTACHMENT_LOAD_OP_CLEAR:
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
  default:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
}

vkr_internal VkAttachmentStoreOp
vkr_bindless_vk_attachment_store_op(VkrAttachmentStoreOp op) {
  return op == VKR_ATTACHMENT_STORE_OP_STORE ? VK_ATTACHMENT_STORE_OP_STORE
                                             : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

vkr_internal bool8_t vkr_bindless_vk_graph_attachment(
    VkrBindlessVulkanRenderer *renderer, const VkrRgAttachment *attachment,
    VkImageLayout layout, VkRenderingAttachmentInfo *out_info,
    uint32_t *out_width, uint32_t *out_height) {
  VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
      renderer, attachment->image, renderer->prepared_frame.image_index);
  if (!instance ||
      attachment->desc.slice.mip_level >= instance->image.mip_levels ||
      attachment->desc.slice.base_layer >= instance->image.array_layers ||
      attachment->desc.slice.layer_count == 0u ||
      attachment->desc.slice.layer_count >
          instance->image.array_layers - attachment->desc.slice.base_layer)
    return false_v;
  VkImageView view = instance->image.view;
  if (instance->image.array_layers > 1u &&
      attachment->desc.slice.layer_count == 1u)
    view = instance->layer_views[attachment->desc.slice.base_layer];
  if (!view)
    return false_v;
  *out_info = (VkRenderingAttachmentInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = layout,
      .loadOp = vkr_bindless_vk_attachment_load_op(attachment->desc.load_op),
      .storeOp = vkr_bindless_vk_attachment_store_op(attachment->desc.store_op),
  };
  MemCopy(&out_info->clearValue, &attachment->desc.clear_value,
          sizeof(out_info->clearValue));
  *out_width =
      Max(1u, instance->image.width >> attachment->desc.slice.mip_level);
  *out_height =
      Max(1u, instance->image.height >> attachment->desc.slice.mip_level);
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_graph_sampled_index(
    VkrBindlessVulkanRenderer *renderer, const VkrRgPass *pass,
    uint32_t read_index, uint32_t *out_index) {
  if (!out_index || read_index >= pass->desc.image_reads.length)
    return false_v;
  const VkrRgImageUse *read =
      vector_get_VkrRgImageUse(&pass->desc.image_reads, read_index);
  VkrBindlessVkGraphImageInstance *image = vkr_bindless_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  if (!image || !image->has_sampled_slot)
    return false_v;
  *out_index = image->sampled_slot.index;
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_record_graphics_body(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, VkrBindlessVkGraphExecutorKind kind) {
  const VkrRenderPacket *packet = renderer->graph->packet;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!packet)
    return false_v;
  switch (kind) {
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SHADOW: {
    if (!packet->shadow)
      return true_v;
    const uint32_t cascade = pass->desc.depth_attachment.desc.slice.base_layer;
    if (cascade >= packet->shadow->cascade_count)
      return false_v;
    const VkrShadowConfigOverride *override = packet->shadow->config_override;
    vkCmdSetDepthBias(command, override ? override->depth_bias_constant : 1.25f,
                      override ? override->depth_bias_clamp : 0.0f,
                      override ? override->depth_bias_slope : 1.75f);
    const uint32_t opaque_draw_begin = slot->shadow_draw_count;
    if (!vkr_bindless_vk_record_packet_draws(
            renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_SHADOW,
            packet->shadow->opaque_draws, packet->shadow->opaque_draw_count,
            slot->shadow_instances, packet->shadow->instance_count,
            packet->shadow->light_view_proj[cascade], false_v, 0u, 0u, false_v))
      return false_v;
    slot->shadow_opaque_draw_count[cascade] =
        slot->shadow_draw_count - opaque_draw_begin;
    const uint32_t alpha_draw_begin = slot->shadow_draw_count;
    if (!vkr_bindless_vk_record_packet_draws(
            renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_SHADOW,
            packet->shadow->alpha_draws, packet->shadow->alpha_draw_count,
            slot->shadow_instances, packet->shadow->instance_count,
            packet->shadow->light_view_proj[cascade], true_v, 0u, 0u, false_v))
      return false_v;
    slot->shadow_alpha_draw_count[cascade] =
        slot->shadow_draw_count - alpha_draw_begin;
    return true_v;
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING: {
    if (!packet->picking || !packet->picking->pending)
      return true_v;
    const Mat4 view_projection =
        mat4_mul(packet->globals.projection, packet->globals.view);
    return vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING,
               packet->picking->draws, packet->picking->draw_count,
               slot->picking_instances, packet->picking->instance_count,
               view_projection, false_v, 0u, 0u, false_v) &&
           (!packet->world ||
            vkr_bindless_vk_record_text_draws(
                renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING_TEXT,
                packet->world->text_draws, packet->world->text_draw_count,
                view_projection, renderer->config.width,
                renderer->config.height, false_v)) &&
           (!packet->ui ||
            vkr_bindless_vk_record_text_draws(
                renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING_TEXT,
                packet->ui->text_draws, packet->ui->text_draw_count,
                mat4_identity(), renderer->config.width,
                renderer->config.height, true_v));
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_OPAQUE: {
    if (!packet->world)
      return true_v;
    uint32_t shadow_texture = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &shadow_texture))
      return false_v;
    return vkr_bindless_vk_record_packet_draws(
        renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_OPAQUE,
        packet->world->opaque_draws, packet->world->opaque_draw_count,
        slot->world_instances, packet->world->instance_count,
        mat4_mul(packet->globals.projection, packet->globals.view), false_v,
        shadow_texture, 0u, false_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_TRANSMISSION: {
    if (!packet->world)
      return true_v;
    uint32_t shadow_texture = 0u;
    uint32_t transmission_texture = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &shadow_texture) ||
        !vkr_bindless_vk_graph_sampled_index(renderer, pass, 1u,
                                             &transmission_texture))
      return false_v;
    return vkr_bindless_vk_record_packet_draws(
        renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_BLEND,
        packet->world->transmission_draws,
        packet->world->transmission_draw_count, slot->world_instances,
        packet->world->instance_count,
        mat4_mul(packet->globals.projection, packet->globals.view), false_v,
        shadow_texture, transmission_texture, true_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_BLEND: {
    if (!packet->world)
      return true_v;
    const Mat4 view_projection =
        mat4_mul(packet->globals.projection, packet->globals.view);
    uint32_t shadow_texture = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &shadow_texture))
      return false_v;
    return vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_BLEND,
               packet->world->transparent_draws,
               packet->world->transparent_draw_count, slot->world_instances,
               packet->world->instance_count, view_projection, false_v,
               shadow_texture, 0u, false_v) &&
           vkr_bindless_vk_record_text_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_TEXT,
               packet->world->text_draws, packet->world->text_draw_count,
               view_projection, renderer->config.width, renderer->config.height,
               false_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_EDITOR: {
    uint32_t texture_index = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &texture_index))
      return false_v;
    if (!vkr_bindless_vk_record_packet_fullscreen(
            renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_FINAL,
            texture_index, 0u))
      return false_v;
    return !packet->editor ||
           vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_UI,
               packet->editor->draws, packet->editor->draw_count,
               slot->editor_instances, packet->editor->instance_count,
               mat4_identity(), false_v, 0u, 0u, false_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_TONEMAP: {
    uint32_t texture_index = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &texture_index))
      return false_v;
    return vkr_bindless_vk_record_packet_fullscreen(
        renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_FINAL,
        texture_index, 2u);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SKYBOX:
    return vkr_bindless_vk_record_packet_skybox(renderer, command,
                                                packet->skybox);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_UI: {
    if (!packet->ui)
      return true_v;
    return vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_UI,
               packet->ui->draws, packet->ui->draw_count, slot->ui_instances,
               packet->ui->instance_count, mat4_identity(), false_v, 0u, 0u,
               false_v) &&
           vkr_bindless_vk_record_text_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_UI_TEXT,
               packet->ui->text_draws, packet->ui->text_draw_count,
               mat4_identity(), renderer->config.width, renderer->config.height,
               true_v);
  }
  default:
    return false_v;
  }
}

vkr_internal bool8_t vkr_bindless_vk_record_graph_graphics_pass(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, VkrBindlessVkGraphExecutorKind kind) {
  enum { VKR_BINDLESS_VK_GRAPH_COLOR_ATTACHMENT_MAX = 8 };
  if (pass->desc.color_attachments.length >
      VKR_BINDLESS_VK_GRAPH_COLOR_ATTACHMENT_MAX)
    return false_v;
  VkRenderingAttachmentInfo colors[VKR_BINDLESS_VK_GRAPH_COLOR_ATTACHMENT_MAX] =
      {0};
  VkRenderingAttachmentInfo depth = {0};
  uint32_t width = 0, height = 0;
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    uint32_t attachment_width = 0, attachment_height = 0;
    if (!vkr_bindless_vk_graph_attachment(
            renderer,
            vector_get_VkrRgAttachment(&pass->desc.color_attachments, i),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &colors[i],
            &attachment_width, &attachment_height))
      return false_v;
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
  }
  if (pass->desc.has_depth_attachment) {
    uint32_t attachment_width = 0, attachment_height = 0;
    if (!vkr_bindless_vk_graph_attachment(
            renderer, &pass->desc.depth_attachment,
            pass->desc.depth_attachment.read_only
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            &depth, &attachment_width, &attachment_height))
      return false_v;
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
  }
  if (!width || !height)
    return false_v;
  const VkRenderingInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.extent = {.width = width, .height = height}},
      .layerCount = 1u,
      .colorAttachmentCount = (uint32_t)pass->desc.color_attachments.length,
      .pColorAttachments = colors,
      .pDepthAttachment = pass->desc.has_depth_attachment ? &depth : NULL,
      .pStencilAttachment =
          pass->desc.has_depth_attachment &&
                  (vkr_bindless_vk_format_aspects(
                       vkr_bindless_vk_graph_image(
                           renderer, pass->desc.depth_attachment.image,
                           renderer->prepared_frame.image_index)
                           ->image.format) &
                   VK_IMAGE_ASPECT_STENCIL_BIT)
              ? &depth
              : NULL,
  };
  vkCmdBeginRendering(command, &rendering);
  const VkViewport viewport = {
      .width = (float32_t)width,
      .height = (float32_t)height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor = {.extent = {.width = width, .height = height}};
  vkCmdSetViewport(command, 0u, 1u, &viewport);
  vkCmdSetScissor(command, 0u, 1u, &scissor);
  if (!vkr_bindless_vk_record_graphics_body(renderer, command, pass, kind)) {
    vkCmdEndRendering(command);
    return false_v;
  }
  vkCmdEndRendering(command);
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_record_graph_transfer_pass(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass) {
  if (pass->desc.image_reads.length == 0u ||
      pass->desc.image_writes.length == 0u)
    return true_v;
  const VkrRgImageUse *read =
      vector_get_VkrRgImageUse(&pass->desc.image_reads, 0u);
  const VkrRgImageUse *write =
      vector_get_VkrRgImageUse(&pass->desc.image_writes, 0u);
  VkrBindlessVkGraphImageInstance *source = vkr_bindless_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  VkrBindlessVkGraphImageInstance *destination = vkr_bindless_vk_graph_image(
      renderer, write->image, renderer->prepared_frame.image_index);
  if (!source || !destination ||
      source->image.format != destination->image.format)
    return false_v;
  const VkImageCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = {.aspectMask = vkr_bindless_vk_format_aspects(
                             source->image.format),
                         .layerCount = 1u},
      .dstSubresource = {.aspectMask = vkr_bindless_vk_format_aspects(
                             destination->image.format),
                         .layerCount = 1u},
      .extent = {.width = Min(source->image.width, destination->image.width),
                 .height = Min(source->image.height, destination->image.height),
                 .depth = 1u},
  };
  const VkCopyImageInfo2 copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
      .srcImage = source->image.handle,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstImage = destination->image.handle,
      .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .regionCount = 1u,
      .pRegions = &region,
  };
  vkCmdCopyImage2(command, &copy);
  return true_v;
}

vkr_internal bool8_t vkr_bindless_vk_record_graph_pass(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass) {
  VkrBindlessVkGraphExecutorKind kind;
  if (!vkr_bindless_vk_graph_executor_kind(pass, &kind)) {
    log_error("Bindless Vulkan graph pass '%.*s' has no executor kind",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  switch (kind) {
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SHADOW:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SKYBOX:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_OPAQUE:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_TRANSMISSION:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_BLEND:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_TONEMAP:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_EDITOR:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_UI:
    return vkr_bindless_vk_record_graph_graphics_pass(renderer, command, pass,
                                                      kind);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_IBL_BAKE:
    return vkr_bindless_vk_record_ibl_bakes(renderer, command);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR:
    return vkr_bindless_vk_record_graph_transfer_pass(renderer, command, pass);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING_READBACK:
    // The one-pixel copy is recorded after capture selection in record_draw().
    return true_v;
  default:
    return false_v;
  }
}

bool8_t vkr_bindless_vk_record_graph(VkrBindlessVulkanRenderer *renderer,
                                     VkCommandBuffer command) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->pass_timing_count = 0u;
  for (uint64_t order = 0; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t pass_index =
        *vector_get_uint32_t(&renderer->graph->execution_order, order);
    const VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, pass_index);
    const float64_t cpu_begin = vkr_platform_get_absolute_time();
    const uint32_t timing_index = slot->pass_timing_count;
    const bool8_t timestamp_pass =
        slot->timing_requested &&
        timing_index < VKR_RENDERER_IMPL_MAX_PASS_TIMINGS;
    if (timestamp_pass) {
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                           slot->timestamp_pool, timing_index * 2u);
    }
    if (!vkr_bindless_vk_record_graph_pass_barriers(renderer, command, pass)) {
      log_error("Bindless Vulkan failed to record barriers for pass '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (!vkr_bindless_vk_record_graph_pass(renderer, command, pass)) {
      log_error("Bindless Vulkan failed to record pass '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (timestamp_pass) {
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                           slot->timestamp_pool, timing_index * 2u + 1u);
    }
    if (slot->pass_timing_count < VKR_RENDERER_IMPL_MAX_PASS_TIMINGS) {
      VkrRendererImplPassTiming *timing =
          &slot->pass_timings[slot->pass_timing_count++];
      MemZero(timing, sizeof(*timing));
      const uint64_t length =
          Min(pass->desc.name.length,
              (uint64_t)VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY - 1u);
      if (length > 0u)
        MemCopy(timing->name, pass->desc.name.str, length);
      timing->name[length] = '\0';
      timing->pass_index = pass_index;
      timing->cpu_ms = (vkr_platform_get_absolute_time() - cpu_begin) * 1000.0;
      timing->valid = false_v;
    }
  }
  slot->timestamp_query_count =
      slot->timing_requested ? slot->pass_timing_count * 2u : 0u;
  return vkr_bindless_vk_record_graph_image_barriers(
      renderer, command, &renderer->graph->terminal_image_barriers);
}

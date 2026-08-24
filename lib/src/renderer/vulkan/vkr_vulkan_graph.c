#include "renderer/vulkan/vkr_vulkan_internal.h"

vkr_internal void vkr_vk_graph_noop(VkrRgPassContext *ctx, void *user_data) {
  (void)ctx;
  (void)user_data;
}

typedef enum VkrVulkanGraphExecutorKind {
  VKR_VULKAN_GRAPH_EXECUTOR_SHADOW = 0,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING_DEPTH_SEED,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING_RESOLVE,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING_READBACK,
  VKR_VULKAN_GRAPH_EXECUTOR_IBL_BAKE,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_UPLOAD,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_PREFIX,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_ENCODE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_DEPTH_SEED,
  VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE,
  VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION,
  VKR_VULKAN_GRAPH_EXECUTOR_GBUFFER_RESOLVE,
  VKR_VULKAN_GRAPH_EXECUTOR_LIGHTING_DEFERRED,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_SHADE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COVERAGE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COMPACT,
  VKR_VULKAN_GRAPH_EXECUTOR_HZB_BUILD,
  VKR_VULKAN_GRAPH_EXECUTOR_SDSM_REDUCE,
  VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN,
  VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR,
  VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND,
  VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP,
  VKR_VULKAN_GRAPH_EXECUTOR_EDITOR,
  VKR_VULKAN_GRAPH_EXECUTOR_UI,
  VKR_VULKAN_GRAPH_EXECUTOR_COUNT,
} VkrVulkanGraphExecutorKind;

typedef struct VkrVulkanGraphExecutorSpec {
  const char *name;
  VkrRgPassType type;
} VkrVulkanGraphExecutorSpec;

vkr_global const VkrVulkanGraphExecutorSpec s_vk_graph_executors[] = {
    {"pass.shadow.cascade", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.picking", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.picking.depth_seed", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.picking.resolve", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.picking.readback", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ibl_bake", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gpu_draw_upload", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.gpu_draw_classify", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gpu_draw_prefix", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gpu_draw_encode", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.gpu_draw_upload", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.transmission.gpu_draw_classify", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.gpu_draw_prefix", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.gpu_draw_encode", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.depth_seed", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.vbuffer.opaque", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.vbuffer.transmission", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.gbuffer.resolve", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.lighting.deferred", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.shade", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.coverage", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.compact", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.hzb.build", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.sdsm.reduce", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.copy.pre_transmission.fullscreen", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.copy.pre_transmission.editor", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.world.blend", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.tonemap", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.editor", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.ui", VKR_RG_PASS_TYPE_GRAPHICS},
};
_Static_assert(ArrayCount(s_vk_graph_executors) ==
                   VKR_VULKAN_GRAPH_EXECUTOR_COUNT,
               "Vulkan graph executor table is incomplete");

vkr_internal bool8_t vkr_vk_graph_executor_kind(
    const VkrRgPass *pass, VkrVulkanGraphExecutorKind *out_kind) {
  const uint32_t encoded = pass->desc.executor_id;
  if (!encoded || encoded > VKR_VULKAN_GRAPH_EXECUTOR_COUNT)
    return false_v;
  *out_kind = (VkrVulkanGraphExecutorKind)(encoded - 1u);
  return true_v;
}

bool8_t vkr_vk_register_graph_executors(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < ArrayCount(s_vk_graph_executors); ++i) {
    const VkrVulkanGraphExecutorSpec *spec = &s_vk_graph_executors[i];
    const VkrRgPassExecutor executor = {
        .name = string8_create_from_cstr((const uint8_t *)spec->name,
                                         string_length(spec->name)),
        .id = i + 1u,
        .type = spec->type,
        .execute = vkr_vk_graph_noop,
    };
    if (!vkr_rg_executor_registry_register(&renderer->executors, &executor))
      return false_v;
  }
  return true_v;
}

bool8_t vkr_vk_validate_graph(const VkrVulkanRenderer *renderer) {
  for (uint64_t order = 0; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t pass_index =
        *vector_get_uint32_t(&renderer->graph->execution_order, order);
    const VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, pass_index);
    VkrVulkanGraphExecutorKind kind;
    if (!vkr_vk_graph_executor_kind(pass, &kind)) {
      log_error("Vulkan graph pass '%.*s' has no executor kind",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    const VkrVulkanGraphExecutorSpec *executor = &s_vk_graph_executors[kind];
    if (pass->desc.type != executor->type) {
      log_error("Vulkan graph pass '%.*s' has type %u; executor '%s' "
                "requires type %u",
                (int)pass->desc.name.length, pass->desc.name.str,
                (uint32_t)pass->desc.type, executor->name,
                (uint32_t)executor->type);
      return false_v;
    }
    for (uint64_t i = 0; i < pass->pre_image_barriers.length; ++i) {
      const VkrRgImageBarrier *barrier =
          vector_get_VkrRgImageBarrier(&pass->pre_image_barriers, i);
      VkrVulkanDependency lowered = {0};
      const VkrVulkanDependencyResult result = vkr_vk_lower_image_dependency(
          barrier->src_access, barrier->dst_access, &barrier->dependency,
          barrier->src_layout != barrier->dst_layout, &lowered);
      if (result != VKR_VULKAN_DEPENDENCY_OK) {
        log_error("Vulkan graph pass '%.*s' image dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_vk_dependency_result_string(result));
        return false_v;
      }
    }
    for (uint64_t i = 0; i < pass->pre_buffer_barriers.length; ++i) {
      const VkrRgBufferBarrier *barrier =
          vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
      VkrVulkanDependency lowered = {0};
      const VkrVulkanDependencyResult result = vkr_vk_lower_buffer_dependency(
          barrier->src_access, barrier->dst_access, &barrier->dependency,
          &lowered);
      if (result != VKR_VULKAN_DEPENDENCY_OK) {
        log_error("Vulkan graph pass '%.*s' buffer dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_vk_dependency_result_string(result));
        return false_v;
      }
    }
  }
  return true_v;
}

vkr_internal VkImageUsageFlags
vkr_vk_graph_image_usage(VkrTextureUsageFlags usage) {
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

vkr_internal bool8_t vkr_vk_graph_format_supports(VkrVulkanRenderer *renderer,
                                                  VkFormat format,
                                                  VkrTextureUsageFlags usage) {
  VkFormatFeatureFlags2 required = 0u;
  if (usage.set & VKR_TEXTURE_USAGE_SAMPLED)
    required |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_STORAGE)
    required |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_COLOR_ATTACHMENT)
    required |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT)
    required |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_SRC)
    required |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_DST)
    required |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  VkFormatProperties3 properties3 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
  };
  VkFormatProperties2 properties2 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &properties3,
  };
  vkGetPhysicalDeviceFormatProperties2(
      vkr_vulkan_device_physical(renderer->device), format, &properties2);
  return required && (properties3.optimalTilingFeatures & required) == required;
}

vkr_internal bool8_t vkr_vk_graph_image_desc_equal(const VkrRgImageDesc *a,
                                                   const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->format == b->format && a->usage.set == b->usage.set &&
         a->samples == b->samples && a->layers == b->layers &&
         a->mip_levels == b->mip_levels && a->type == b->type &&
         a->flags == b->flags;
}

vkr_internal void
vkr_vk_destroy_graph_image_instance(VkrVulkanRenderer *renderer,
                                    VkrVulkanGraphImageInstance *instance) {
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
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
  for (uint32_t mip = 0u; mip < VKR_VULKAN_TEXTURE_MIP_MAX; ++mip) {
    if (instance->mip_views[mip])
      vkDestroyImageView(device, instance->mip_views[mip], NULL);
    if (instance->has_sampled_mip_slot[mip]) {
      (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                      instance->sampled_mip_slots[mip],
                                      completed);
    }
    if (instance->has_storage_mip_slot[mip]) {
      (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                      instance->storage_mip_slots[mip],
                                      completed);
    }
    for (uint32_t layer = 0; layer < VKR_VULKAN_GRAPH_LAYER_MAX; ++layer) {
      if (instance->mip_layer_views[mip][layer])
        vkDestroyImageView(device, instance->mip_layer_views[mip][layer], NULL);
    }
  }
  (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                   NULL);
  (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                   NULL);
  vkr_vk_destroy_image(renderer, &instance->image);
  MemZero(instance, sizeof(*instance));
}

void vkr_vk_destroy_graph_image(VkrVulkanRenderer *renderer,
                                VkrVulkanGraphImage *slot) {
  if (!slot || slot->external_swapchain) {
    if (slot)
      MemZero(slot, sizeof(*slot));
    return;
  }
  for (uint32_t i = 0; i < slot->instance_count; ++i)
    vkr_vk_destroy_graph_image_instance(renderer, &slot->instances[i]);
  MemZero(slot, sizeof(*slot));
}

vkr_internal bool8_t vkr_vk_create_graph_image_instance(
    VkrVulkanRenderer *renderer, const VkrRgImageDesc *desc,
    VkrVulkanGraphImageInstance *out_instance) {
  const VkFormat format = vkr_vk_texture_format(desc->format);
  VkrTextureUsageFlags checked_usage = desc->usage;
  /* A capture request arrives with a packet, after graph images have already

   * been realized. Capture-enabled renderers therefore provision graph-owned

   * images as legal transfer sources up front; no per-capture image churn is

   * allowed in the frame path. */
  if (renderer->config.capture_ring_capacity > 0u)
    checked_usage.set |= VKR_TEXTURE_USAGE_TRANSFER_SRC;
  const VkImageUsageFlags usage = vkr_vk_graph_image_usage(checked_usage);
  if (format == VK_FORMAT_UNDEFINED || usage == 0u ||
      !vkr_vk_graph_format_supports(renderer, format, checked_usage) ||
      desc->samples != 1u || desc->layers == 0u ||
      desc->layers > VKR_VULKAN_GRAPH_LAYER_MAX || desc->mip_levels == 0u ||
      desc->mip_levels > VKR_VULKAN_TEXTURE_MIP_MAX)
    return false_v;
  const bool8_t array_view =
      desc->layers > 1u || (desc->flags & VKR_RG_RESOURCE_FLAG_FORCE_ARRAY);
  if (!vkr_vk_create_image_ex(renderer, desc->width, desc->height,
                              desc->mip_levels, desc->layers, format, 0u,
                              array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                         : VK_IMAGE_VIEW_TYPE_2D,
                              usage, &out_instance->image))
    return false_v;
  const VkImageAspectFlags aspects = vkr_vk_format_aspects(format);
  for (uint32_t mip = 0u; mip < desc->mip_levels; ++mip) {
    const VkImageViewCreateInfo mip_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out_instance->image.handle,
        .viewType =
            array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange =
            {
                .aspectMask = aspects,
                .baseMipLevel = mip,
                .levelCount = 1u,
                .baseArrayLayer = 0u,
                .layerCount = desc->layers,
            },
    };
    if (vkCreateImageView(vkr_vk_renderer_device(renderer), &mip_view_info,
                          NULL, &out_instance->mip_views[mip]) != VK_SUCCESS) {
      vkr_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    for (uint32_t layer = 0u; layer < desc->layers; ++layer) {
      const VkImageViewCreateInfo view_info = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = out_instance->image.handle,
          .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = format,
          .subresourceRange =
              {
                  .aspectMask = aspects,
                  .baseMipLevel = mip,
                  .levelCount = 1u,
                  .baseArrayLayer = layer,
                  .layerCount = 1u,
              },
      };
      if (vkCreateImageView(vkr_vk_renderer_device(renderer), &view_info, NULL,
                            &out_instance->mip_layer_views[mip][layer]) !=
          VK_SUCCESS) {
        vkr_vk_destroy_graph_image_instance(renderer, out_instance);
        return false_v;
      }
    }
    VkImageView mip_view = out_instance->mip_views[mip];
    if ((desc->usage.set & VKR_TEXTURE_USAGE_SAMPLED) != 0u) {
      if (!vkr_vk_publish_sampled_view(renderer, mip_view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       &out_instance->sampled_mip_slots[mip])) {
        vkr_vk_destroy_graph_image_instance(renderer, out_instance);
        return false_v;
      }
      out_instance->has_sampled_mip_slot[mip] = true_v;
    }
    if ((desc->usage.set & VKR_TEXTURE_USAGE_STORAGE) != 0u) {
      if (!vkr_vk_publish_storage_view(renderer, mip_view,
                                       &out_instance->storage_mip_slots[mip])) {
        vkr_vk_destroy_graph_image_instance(renderer, out_instance);
        return false_v;
      }
      out_instance->has_storage_mip_slot[mip] = true_v;
    }
  }
  if ((desc->usage.set & VKR_TEXTURE_USAGE_SAMPLED) != 0u) {
    if (!vkr_vk_publish_sampled_view(renderer, out_instance->image.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     &out_instance->sampled_slot)) {
      vkr_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_sampled_slot = true_v;
  }
  if ((desc->usage.set & VKR_TEXTURE_USAGE_STORAGE) != 0u) {
    if (!vkr_vk_publish_storage_view(renderer, out_instance->image.view,
                                     &out_instance->storage_slot)) {
      vkr_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_storage_slot = true_v;
  }
  return true_v;
}

vkr_internal uint32_t vkr_vk_graph_image_instance_count(
    const VkrVulkanRenderer *renderer, VkrRgResourceFlags flags) {
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    return renderer->targets.image_count;
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    return VKR_VULKAN_FRAME_SLOT_COUNT;
  return 1u;
}

bool8_t vkr_vk_realize_graph_images(VkrVulkanRenderer *renderer) {
  if (renderer->graph->images.length > renderer->config.max_graph_images)
    return false_v;
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrVulkanGraphImage *slot = &renderer->graph_images[i];
    const bool8_t external_swapchain =
        image->imported && vkr_string8_equals_cstr(&image->name, "swapchain");
    const uint32_t instance_count =
        vkr_vk_graph_image_instance_count(renderer, image->desc.flags);
    if (!instance_count || instance_count > VKR_VULKAN_TARGET_IMAGE_MAX)
      return false_v;
    if (slot->live && slot->graph_generation == image->generation &&
        slot->external_swapchain == external_swapchain &&
        slot->instance_count == instance_count &&
        vkr_vk_graph_image_desc_equal(&slot->desc, &image->desc))
      continue;
    if (slot->live) {
      const uint64_t completed = vkr_vk_refresh_completed(renderer);
      for (uint32_t instance = 0u; instance < slot->instance_count;
           ++instance) {
        if (slot->instances[instance].last_use_submit_value > completed) {
          log_error("Vulkan graph image '%.*s' replacement is busy "
                    "through submit %llu (completed %llu)",
                    (int)image->name.length, image->name.str,
                    (unsigned long long)slot->instances[instance]
                        .last_use_submit_value,
                    (unsigned long long)completed);
          return false_v;
        }
      }
      vkr_vk_destroy_graph_image(renderer, slot);
    }
    /* Wholesale reassignment zeroes every instance's retained_states, so
       content_valid goes false. That is the ADR-029 invalidation rule for
       resize, format, layer, mip, and image-count changes: reaching this line
       at all means the descriptor changed, and stale contents must not be
       advertised against a differently shaped image. */
    *slot = (VkrVulkanGraphImage){
        .desc = image->desc,
        .graph_generation = image->generation,
        .instance_count = instance_count,
        .live = true_v,
        .external_swapchain = external_swapchain,
    };
    if (external_swapchain)
      continue;
    for (uint32_t instance = 0; instance < instance_count; ++instance) {
      if (!vkr_vk_create_graph_image_instance(renderer, &image->desc,
                                              &slot->instances[instance])) {
        log_error("Vulkan failed to realize graph image '%.*s' "
                  "(%ux%u, format=%u, usage=0x%x, samples=%u, layers=%u, "
                  "mips=%u, instance=%u/%u)",
                  (int)image->name.length, image->name.str, image->desc.width,
                  image->desc.height, image->desc.format, image->desc.usage.set,
                  image->desc.samples, image->desc.layers,
                  image->desc.mip_levels, instance, instance_count);
        vkr_vk_destroy_graph_image(renderer, slot);
        return false_v;
      }
    }
  }
  return true_v;
}

/**
 * Resolves one retained subresource slot, or NULL when the request cannot name
 * a live instance. Shared by the read and commit halves of the provider so both
 * agree on bounds and liveness.
 */
vkr_internal VkrRgRetainedState *
vkr_vk_retained_slot(VkrVulkanRenderer *renderer, uint32_t image_index,
                     uint32_t instance_index, uint32_t subresource) {
  if (image_index >= renderer->graph->images.length)
    return NULL;
  const VkrRgImage *image =
      vector_get_VkrRgImage(&renderer->graph->images, image_index);
  VkrVulkanGraphImage *slot = &renderer->graph_images[image_index];
  if (!image || !slot->live || slot->graph_generation != image->generation ||
      instance_index >= slot->instance_count)
    return NULL;
  if (subresource >=
      ArrayCount(slot->instances[instance_index].retained_states))
    return NULL;
  return &slot->instances[instance_index].retained_states[subresource];
}

vkr_internal void vkr_vk_retained_read(void *context, uint32_t image_index,
                                       uint32_t instance_index,
                                       uint32_t subresource,
                                       VkrRgRetainedState *out_state) {
  const VkrRgRetainedState *state = vkr_vk_retained_slot(
      (VkrVulkanRenderer *)context, image_index, instance_index, subresource);
  /* An unresolvable slot reports invalid rather than failing: the compiler
     treats invalid contents as "must be written this frame", which is the safe
     reading of "this backend cannot vouch for it". */
  if (state)
    *out_state = *state;
}

vkr_internal void vkr_vk_retained_commit(void *context, uint32_t image_index,
                                         uint32_t instance_index,
                                         uint32_t subresource,
                                         const VkrRgRetainedState *state) {
  VkrRgRetainedState *slot = vkr_vk_retained_slot(
      (VkrVulkanRenderer *)context, image_index, instance_index, subresource);
  if (slot)
    *slot = *state;
}

void vkr_vk_install_retained_provider(VkrVulkanRenderer *renderer) {
  const VkrRgRetainedStateProvider provider = {
      .context = renderer,
      .read = vkr_vk_retained_read,
      .commit = vkr_vk_retained_commit,
  };
  vkr_rg_set_retained_state_provider(renderer->graph, &provider);
}

void vkr_vulkan_renderer_retained_shadow_token(
    VkrVulkanRenderer *renderer, uint32_t image_index,
    VkrRetainedShadowToken *out_token) {
  *out_token = (VkrRetainedShadowToken){0};
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !vkr_string8_equals_cstr(&image->name, "shadow_map"))
      continue;
    VkrVulkanGraphImage *slot = &renderer->graph_images[i];
    if (!slot->live || slot->graph_generation != image->generation ||
        slot->instance_count != renderer->targets.image_count ||
        image_index >= slot->instance_count || slot->desc.mip_levels != 1u ||
        slot->desc.samples != VKR_SAMPLE_COUNT_1 ||
        slot->desc.type != VKR_TEXTURE_TYPE_2D ||
        slot->desc.width != renderer->prepared_frame.shadow_map_size ||
        slot->desc.height != renderer->prepared_frame.shadow_map_size ||
        slot->desc.format != renderer->prepared_frame.shadow_depth_format ||
        slot->desc.layers != renderer->prepared_frame.shadow_map_layer_count)
      return;
    const VkrVulkanGraphImageInstance *instance = &slot->instances[image_index];
    out_token->resource_generation = slot->graph_generation;
    const uint32_t layer_count = Min(slot->desc.layers, 32u);
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
      if (instance->retained_states[layer].content_valid)
        out_token->valid_layer_mask |= UINT32_C(1) << layer;
    }
    return;
  }
}

VkrVulkanGraphImageInstance *vkr_vk_graph_image(VkrVulkanRenderer *renderer,
                                                VkrRgImageHandle handle,
                                                uint32_t image_index) {
  if (!vkr_rg_image_handle_valid(handle) ||
      handle.id > renderer->graph->images.length)
    return NULL;
  VkrVulkanGraphImage *slot = &renderer->graph_images[handle.id - 1u];
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
  uint32_t instance = 0u;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(slot->desc.flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    instance = image_index;
  else if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    instance = renderer->active_frame_slot;
  return instance < slot->instance_count ? &slot->instances[instance] : NULL;
}

void vkr_vk_mark_graph_images_submitted(VkrVulkanRenderer *renderer,
                                        uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrVulkanGraphImageInstance *instance =
        vkr_vk_graph_image(renderer,
                           (VkrRgImageHandle){.id = (uint32_t)i + 1u,
                                              .generation = image->generation},
                           renderer->prepared_frame.image_index);
    if (instance && !renderer->graph_images[i].external_swapchain)
      instance->last_use_submit_value = submit_value;
  }
}

vkr_internal VkBufferUsageFlags
vkr_vk_graph_buffer_usage(VkrBufferUsageFlags usage) {
  VkBufferUsageFlags result = 0u;
  if (usage.set & VKR_BUFFER_USAGE_VERTEX_BUFFER)
    result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (usage.set & VKR_BUFFER_USAGE_INDEX_BUFFER)
    result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (usage.set &
      (VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER | VKR_BUFFER_USAGE_UNIFORM))
    result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (usage.set & VKR_BUFFER_USAGE_STORAGE)
    result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (usage.set & VKR_BUFFER_USAGE_TRANSFER_SRC)
    result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (usage.set & VKR_BUFFER_USAGE_TRANSFER_DST)
    result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (usage.set & VKR_BUFFER_USAGE_INDIRECT)
    result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  return result;
}

void vkr_vk_destroy_graph_buffer(VkrVulkanRenderer *renderer,
                                 VkrVulkanGraphBuffer *slot) {
  if (!slot)
    return;
  for (uint32_t i = 0u; i < slot->instance_count; ++i)
    vkr_vk_destroy_buffer(renderer, &slot->instances[i].buffer);
  MemZero(slot, sizeof(*slot));
}

vkr_internal uint32_t vkr_vk_graph_buffer_instance_count(
    const VkrVulkanRenderer *renderer, VkrRgResourceFlags flags) {
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    return renderer->targets.image_count;
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    return VKR_VULKAN_FRAME_SLOT_COUNT;
  return 1u;
}

bool8_t vkr_vk_realize_graph_buffers(VkrVulkanRenderer *renderer) {
  if (renderer->graph->buffers.length > renderer->config.max_graph_buffers)
    return false_v;
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  renderer->gpu_candidate_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->gpu_candidate_instance_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->transmission_gpu_candidate_instance_buffer_handle =
      VKR_RG_BUFFER_HANDLE_INVALID;
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame)
      continue;
    const VkrRgBufferHandle handle = {
        .id = (uint32_t)i + 1u,
        .generation = buffer->generation,
    };
    if (vkr_string8_equals_cstr(&buffer->name, "gpu_draw_candidates"))
      renderer->gpu_candidate_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name, "gpu_draw_instances"))
      renderer->gpu_candidate_instance_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name,
                                     "transmission_gpu_draw_instances"))
      renderer->transmission_gpu_candidate_instance_buffer_handle = handle;
    if (buffer->imported ||
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_EXTERNAL)) {
      log_error("Vulkan graph buffer '%.*s' has no imported native "
                "buffer binding",
                (int)buffer->name.length, buffer->name.str);
      return false_v;
    }
    const uint32_t instance_count =
        vkr_vk_graph_buffer_instance_count(renderer, buffer->desc.flags);
    if (!instance_count || instance_count > VKR_VULKAN_TARGET_IMAGE_MAX)
      return false_v;
    VkrVulkanGraphBuffer *slot = &renderer->graph_buffers[i];
    if (slot->live && slot->graph_generation == buffer->generation &&
        slot->instance_count == instance_count &&
        slot->desc.size == buffer->desc.size &&
        slot->desc.usage.set == buffer->desc.usage.set &&
        slot->desc.flags == buffer->desc.flags)
      continue;
    if (slot->live) {
      for (uint32_t instance = 0u; instance < slot->instance_count;
           ++instance) {
        if (slot->instances[instance].last_use_submit_value > completed) {
          log_error("Vulkan graph buffer '%.*s' replacement is busy "
                    "through submit %llu (completed %llu)",
                    (int)buffer->name.length, buffer->name.str,
                    (unsigned long long)slot->instances[instance]
                        .last_use_submit_value,
                    (unsigned long long)completed);
          return false_v;
        }
      }
      vkr_vk_destroy_graph_buffer(renderer, slot);
    }
    const VkBufferUsageFlags usage =
        vkr_vk_graph_buffer_usage(buffer->desc.usage);
    if (!buffer->desc.size || !usage)
      return false_v;
    *slot = (VkrVulkanGraphBuffer){
        .desc = buffer->desc,
        .graph_generation = buffer->generation,
        .instance_count = instance_count,
        .live = true_v,
    };
    for (uint32_t instance = 0u; instance < instance_count; ++instance) {
      if (!vkr_vk_create_buffer(renderer, VKR_VULKAN_MEMORY_CLASS_DEVICE,
                                buffer->desc.size, usage,
                                &slot->instances[instance].buffer)) {
        vkr_vk_destroy_graph_buffer(renderer, slot);
        return false_v;
      }
    }
  }
  return true_v;
}

VkrVulkanGraphBufferInstance *vkr_vk_graph_buffer(VkrVulkanRenderer *renderer,
                                                  VkrRgBufferHandle handle) {
  if (!vkr_rg_buffer_handle_valid(handle) ||
      handle.id > renderer->graph->buffers.length)
    return NULL;
  VkrVulkanGraphBuffer *slot = &renderer->graph_buffers[handle.id - 1u];
  if (!slot->live || slot->graph_generation != handle.generation)
    return NULL;
  uint32_t instance = 0u;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(slot->desc.flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    instance = renderer->prepared_frame.image_index;
  else if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    instance = renderer->active_frame_slot;
  return instance < slot->instance_count ? &slot->instances[instance] : NULL;
}

void vkr_vk_mark_graph_buffers_submitted(VkrVulkanRenderer *renderer,
                                         uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame)
      continue;
    VkrVulkanGraphBufferInstance *instance = vkr_vk_graph_buffer(
        renderer, (VkrRgBufferHandle){.id = (uint32_t)i + 1u,
                                      .generation = buffer->generation});
    if (instance)
      instance->last_use_submit_value = submit_value;
  }
}

vkr_internal bool8_t vkr_vk_record_graph_image_barriers(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    const Vector_VkrRgImageBarrier *barriers) {
  const uint32_t batch_capacity = renderer->config.max_graph_images;
  if (!batch_capacity)
    return barriers->length == 0u;
  for (uint64_t begin = 0u; begin < barriers->length; begin += batch_capacity) {
    const uint32_t batch_count =
        (uint32_t)Min((uint64_t)batch_capacity, barriers->length - begin);
    for (uint32_t i = 0u; i < batch_count; ++i) {
      const VkrRgImageBarrier *barrier =
          vector_get_VkrRgImageBarrier(barriers, begin + i);
      VkrVulkanGraphImageInstance *instance = vkr_vk_graph_image(
          renderer, barrier->image, renderer->prepared_frame.image_index);
      if (!instance)
        return false_v;
      VkrVulkanDependency lowered = {0};
      const VkrVulkanDependencyResult result = vkr_vk_lower_image_dependency(
          barrier->src_access, barrier->dst_access, &barrier->dependency,
          barrier->src_layout != barrier->dst_layout, &lowered);
      if (result != VKR_VULKAN_DEPENDENCY_OK)
        return false_v;
      uint32_t base_mip = 0, mip_count = 0, base_layer = 0, layer_count = 0;
      vkr_image_subresource_range_resolve(
          &barrier->range, instance->image.mip_levels,
          instance->image.array_layers, &base_mip, &mip_count, &base_layer,
          &layer_count);
      renderer->graph_image_barriers[i] = (VkImageMemoryBarrier2){
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = lowered.src_stages,
          .srcAccessMask = lowered.src_access,
          .dstStageMask = lowered.dst_stages,
          .dstAccessMask = lowered.dst_access,
          .oldLayout = vkr_vk_texture_layout(barrier->src_layout),
          .newLayout = vkr_vk_texture_layout(barrier->dst_layout),
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = instance->image.handle,
          .subresourceRange =
              {
                  .aspectMask = vkr_vk_format_aspects(instance->image.format),
                  .baseMipLevel = base_mip,
                  .levelCount = mip_count,
                  .baseArrayLayer = base_layer,
                  .layerCount = layer_count,
              },
      };
    }
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = batch_count,
        .pImageMemoryBarriers = renderer->graph_image_barriers,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_record_graph_pass_barriers(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass) {
  if (pass->pre_buffer_barriers.length > renderer->config.max_graph_buffers) {
    log_error("Vulkan pass needs %llu buffer barriers; capacity=%u",
              (unsigned long long)pass->pre_buffer_barriers.length,
              renderer->config.max_graph_buffers);
    return false_v;
  }
  for (uint64_t i = 0u; i < pass->pre_buffer_barriers.length; ++i) {
    const VkrRgBufferBarrier *barrier =
        vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
    VkrVulkanGraphBufferInstance *instance =
        vkr_vk_graph_buffer(renderer, barrier->buffer);
    VkrVulkanDependency lowered = {0};
    if (!instance ||
        vkr_vk_lower_buffer_dependency(barrier->src_access, barrier->dst_access,
                                       &barrier->dependency,
                                       &lowered) != VKR_VULKAN_DEPENDENCY_OK)
      return false_v;
    renderer->graph_buffer_barriers[i] = (VkBufferMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = lowered.src_stages,
        .srcAccessMask = lowered.src_access,
        .dstStageMask = lowered.dst_stages,
        .dstAccessMask = lowered.dst_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = instance->buffer.handle,
        .offset = 0u,
        .size = VK_WHOLE_SIZE,
    };
  }
  if (pass->pre_buffer_barriers.length > 0u) {
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = (uint32_t)pass->pre_buffer_barriers.length,
        .pBufferMemoryBarriers = renderer->graph_buffer_barriers,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
  return vkr_vk_record_graph_image_barriers(renderer, command,
                                            &pass->pre_image_barriers);
}

vkr_internal VkAttachmentLoadOp
vkr_vk_attachment_load_op(VkrAttachmentLoadOp op) {
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
vkr_vk_attachment_store_op(VkrAttachmentStoreOp op) {
  return op == VKR_ATTACHMENT_STORE_OP_STORE ? VK_ATTACHMENT_STORE_OP_STORE
                                             : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

vkr_internal bool8_t vkr_vk_graph_attachment(
    VkrVulkanRenderer *renderer, const VkrRgAttachment *attachment,
    VkImageLayout layout, VkRenderingAttachmentInfo *out_info,
    uint32_t *out_width, uint32_t *out_height, uint32_t *out_layers) {
  VkrVulkanGraphImageInstance *instance = vkr_vk_graph_image(
      renderer, attachment->image, renderer->prepared_frame.image_index);
  if (!instance ||
      attachment->desc.slice.mip_level >= instance->image.mip_levels ||
      attachment->desc.slice.base_layer >= instance->image.array_layers ||
      attachment->desc.slice.layer_count == 0u ||
      attachment->desc.slice.layer_count >
          instance->image.array_layers - attachment->desc.slice.base_layer)
    return false_v;
  const VkrVulkanGraphImage *graph_image =
      &renderer->graph_images[attachment->image.id - 1u];
  VkImageView view =
      graph_image->external_swapchain ? instance->image.view
      : attachment->desc.slice.layer_count == 1u
          ? instance->mip_layer_views[attachment->desc.slice.mip_level]
                                     [attachment->desc.slice.base_layer]
          : instance->mip_views[attachment->desc.slice.mip_level];
  if (!view)
    return false_v;
  *out_info = (VkRenderingAttachmentInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = layout,
      .loadOp = vkr_vk_attachment_load_op(attachment->desc.load_op),
      .storeOp = vkr_vk_attachment_store_op(attachment->desc.store_op),
  };
  MemCopy(&out_info->clearValue, &attachment->desc.clear_value,
          sizeof(out_info->clearValue));
  *out_width =
      Max(1u, instance->image.width >> attachment->desc.slice.mip_level);
  *out_height =
      Max(1u, instance->image.height >> attachment->desc.slice.mip_level);
  *out_layers = attachment->desc.slice.layer_count;
  return true_v;
}

vkr_internal bool8_t vkr_vk_graph_sampled_index(VkrVulkanRenderer *renderer,
                                                const VkrRgPass *pass,
                                                uint32_t binding,
                                                uint32_t *out_index) {
  if (!out_index)
    return false_v;
  const VkrRgImageUse *read =
      vkr_rg_pass_find_image_use(&pass->desc, binding, 0u);
  if (!read)
    return false_v;
  VkrVulkanGraphImageInstance *image = vkr_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  if (!image)
    return false_v;
  if (read->has_slice &&
      (read->slice.mip_count == 0u || read->slice.mip_count == 1u)) {
    const uint32_t mip = read->slice.mip_level;
    if (mip >= image->image.mip_levels || !image->has_sampled_mip_slot[mip])
      return false_v;
    *out_index = image->sampled_mip_slots[mip].index;
  } else {
    if (!image->has_sampled_slot)
      return false_v;
    *out_index = image->sampled_slot.index;
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_record_graphics_body(
    VkrVulkanRenderer *renderer, VkCommandBuffer command, const VkrRgPass *pass,
    VkrVulkanGraphExecutorKind kind) {
  const VkrRenderPacket *packet = renderer->graph->packet;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!packet)
    return false_v;
  switch (kind) {
  case VKR_VULKAN_GRAPH_EXECUTOR_SHADOW: {
    if (!packet->shadow)
      return true_v;
    const uint32_t cascade = pass->desc.depth_attachment.desc.slice.base_layer;
    if (cascade >= packet->shadow->cascade_count)
      return false_v;
    /* No hardcoded fallback: the configured value is the contract, and a
       backend-local default would silently disagree with the other selected
       implementation. A packet without an override means no raster bias. */
    const VkrShadowConfigOverride bias = packet->shadow->config_override
                                             ? *packet->shadow->config_override
                                             : (VkrShadowConfigOverride){0};
    vkCmdSetDepthBias(command, bias.depth_bias_constant, bias.depth_bias_clamp,
                      bias.depth_bias_slope);
    return vkr_vk_record_deferred_raster(renderer, command, pass, true_v,
                                         false_v);
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING: {
    if (!packet->picking || !packet->picking->pending)
      return true_v;
    const Mat4 view_projection =
        mat4_mul(packet->globals.projection, packet->globals.view);
    const VkrDrawItem *picking_draws =
        packet->world ? packet->world->transparent_draws : NULL;
    const uint32_t picking_draw_count =
        packet->world ? packet->world->transparent_draw_count : 0u;
    return vkr_vk_record_packet_draws(
               renderer, command, VKR_VULKAN_PACKET_PIPELINE_PICKING,
               picking_draws, picking_draw_count, slot->world_instances,
               view_projection, false_v, 0u, 0u, false_v) &&
           (!packet->world ||
            vkr_vk_record_text_draws(
                renderer, command, VKR_VULKAN_PACKET_PIPELINE_PICKING_TEXT,
                packet->world->text_draws, packet->world->text_draw_count,
                view_projection, renderer->config.width,
                renderer->config.height, false_v)) &&
           (!packet->ui ||
            vkr_vk_record_text_draws(
                renderer, command, VKR_VULKAN_PACKET_PIPELINE_PICKING_TEXT,
                packet->ui->text_draws, packet->ui->text_draw_count,
                mat4_identity(), renderer->config.width,
                renderer->config.height, true_v));
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE:
    return vkr_vk_record_deferred_raster(renderer, command, pass, false_v,
                                         false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION:
    return vkr_vk_record_deferred_raster(renderer, command, pass, false_v,
                                         true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND: {
    if (!packet->world)
      return true_v;
    const Mat4 view_projection =
        mat4_mul(packet->globals.projection, packet->globals.view);
    uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
    if (renderer->prepared_frame.shadow_cascade_count > 0u &&
        !vkr_vk_graph_sampled_index(renderer, pass, 0u, &shadow_texture))
      return false_v;
    return vkr_vk_record_packet_draws(
               renderer, command, VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND,
               packet->world->transparent_draws,
               packet->world->transparent_draw_count, slot->world_instances,
               view_projection, false_v, shadow_texture, 0u, false_v) &&
           vkr_vk_record_text_draws(
               renderer, command, VKR_VULKAN_PACKET_PIPELINE_WORLD_TEXT,
               packet->world->text_draws, packet->world->text_draw_count,
               view_projection, renderer->config.width, renderer->config.height,
               false_v);
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR: {
    uint32_t texture_index = 0u;
    if (!vkr_vk_graph_sampled_index(renderer, pass, 0u, &texture_index))
      return false_v;
    if (!vkr_vk_record_packet_fullscreen(
            renderer, command, VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
            texture_index, 0u))
      return false_v;
    return !packet->editor ||
           vkr_vk_record_packet_draws(
               renderer, command, VKR_VULKAN_PACKET_PIPELINE_UI,
               packet->editor->draws, packet->editor->draw_count,
               slot->editor_instances, mat4_identity(), false_v, 0u, 0u,
               false_v);
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP: {
    uint32_t texture_index = 0u;
    if (!vkr_vk_graph_sampled_index(renderer, pass, 0u, &texture_index)) {
      log_error("Vulkan tonemap input has no sampled descriptor");
      return false_v;
    }
    const bool8_t recorded = vkr_vk_record_packet_fullscreen(
        renderer, command, VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
        texture_index, 2u);
    if (!recorded)
      log_error("Vulkan tonemap root allocation failed at %llu/%u bytes",
                (unsigned long long)slot->frame_upload_cursor,
                VKR_VULKAN_FRAME_UPLOAD_SIZE);
    return recorded;
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_UI: {
    if (!packet->ui)
      return true_v;
    return vkr_vk_record_packet_draws(
               renderer, command, VKR_VULKAN_PACKET_PIPELINE_UI,
               packet->ui->draws, packet->ui->draw_count, slot->ui_instances,
               mat4_identity(), false_v, 0u, 0u, false_v) &&
           vkr_vk_record_text_draws(
               renderer, command, VKR_VULKAN_PACKET_PIPELINE_UI_TEXT,
               packet->ui->text_draws, packet->ui->text_draw_count,
               mat4_identity(), renderer->config.width, renderer->config.height,
               true_v);
  }
  default:
    return false_v;
  }
}

vkr_internal bool8_t vkr_vk_record_graph_graphics_pass(
    VkrVulkanRenderer *renderer, VkCommandBuffer command, const VkrRgPass *pass,
    VkrVulkanGraphExecutorKind kind) {
  enum { VKR_VULKAN_GRAPH_COLOR_ATTACHMENT_MAX = 8 };
  if (pass->desc.color_attachments.length >
      VKR_VULKAN_GRAPH_COLOR_ATTACHMENT_MAX)
    return false_v;
  VkRenderingAttachmentInfo colors[VKR_VULKAN_GRAPH_COLOR_ATTACHMENT_MAX] = {0};
  VkRenderingAttachmentInfo depth = {0};
  uint32_t width = 0, height = 0, layers = 0;
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    uint32_t attachment_width = 0, attachment_height = 0, attachment_layers = 0;
    const bool8_t attachment_ready = vkr_vk_graph_attachment(
        renderer, vector_get_VkrRgAttachment(&pass->desc.color_attachments, i),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &colors[i], &attachment_width,
        &attachment_height, &attachment_layers);
    if (!attachment_ready || (layers != 0u && layers != attachment_layers)) {
      log_error("Vulkan graphics pass '%.*s' color attachment %llu "
                "is unavailable (image=%u, layers=%u/%u)",
                (int)pass->desc.name.length, pass->desc.name.str,
                (unsigned long long)i, renderer->prepared_frame.image_index,
                layers, attachment_layers);
      return false_v;
    }
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
    layers = attachment_layers;
  }
  if (pass->desc.has_depth_attachment) {
    uint32_t attachment_width = 0, attachment_height = 0, attachment_layers = 0;
    const bool8_t attachment_ready = vkr_vk_graph_attachment(
        renderer, &pass->desc.depth_attachment,
        pass->desc.depth_attachment.read_only
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        &depth, &attachment_width, &attachment_height, &attachment_layers);
    if (!attachment_ready || (layers != 0u && layers != attachment_layers)) {
      log_error("Vulkan graphics pass '%.*s' depth attachment is "
                "unavailable (image=%u, layers=%u/%u)",
                (int)pass->desc.name.length, pass->desc.name.str,
                renderer->prepared_frame.image_index, layers,
                attachment_layers);
      return false_v;
    }
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
    layers = attachment_layers;
  }
  if (!width || !height || !layers) {
    log_error("Vulkan graphics pass '%.*s' has an empty render area",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  const VkRenderingInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.extent = {.width = width, .height = height}},
      .layerCount = layers,
      .colorAttachmentCount = (uint32_t)pass->desc.color_attachments.length,
      .pColorAttachments = colors,
      .pDepthAttachment = pass->desc.has_depth_attachment ? &depth : NULL,
      .pStencilAttachment =
          pass->desc.has_depth_attachment &&
                  (vkr_vk_format_aspects(
                       vkr_vk_graph_image(renderer,
                                          pass->desc.depth_attachment.image,
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
  vkCmdSetCullMode(command, VK_CULL_MODE_NONE);
  if (!vkr_vk_record_graphics_body(renderer, command, pass, kind)) {
    vkCmdEndRendering(command);
    return false_v;
  }
  vkCmdEndRendering(command);
  return true_v;
}

vkr_internal bool8_t vkr_vk_record_graph_transfer_pass(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass) {
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *write = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  if (!read && !write)
    return true_v;
  if (!read || !write)
    return false_v;
  VkrVulkanGraphImageInstance *source = vkr_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  VkrVulkanGraphImageInstance *destination = vkr_vk_graph_image(
      renderer, write->image, renderer->prepared_frame.image_index);
  if (!source || !destination ||
      source->image.format != destination->image.format)
    return false_v;
  const VkImageCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource =
          {.aspectMask = vkr_vk_format_aspects(source->image.format),
           .mipLevel = read->has_slice ? read->slice.mip_level : 0u,
           .baseArrayLayer = read->has_slice ? read->slice.base_layer : 0u,
           .layerCount = read->has_slice ? read->slice.layer_count : 1u},
      .dstSubresource =
          {.aspectMask = vkr_vk_format_aspects(destination->image.format),
           .mipLevel = write->has_slice ? write->slice.mip_level : 0u,
           .baseArrayLayer = write->has_slice ? write->slice.base_layer : 0u,
           .layerCount = write->has_slice ? write->slice.layer_count : 1u},
      .extent =
          {.width = Min(
               Max(1u, source->image.width >>
                           (read->has_slice ? read->slice.mip_level : 0u)),
               Max(1u, destination->image.width >>
                           (write->has_slice ? write->slice.mip_level : 0u))),
           .height = Min(
               Max(1u, source->image.height >>
                           (read->has_slice ? read->slice.mip_level : 0u)),
               Max(1u, destination->image.height >>
                           (write->has_slice ? write->slice.mip_level : 0u))),
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

vkr_internal bool8_t vkr_vk_record_graph_pass(VkrVulkanRenderer *renderer,
                                              VkCommandBuffer command,
                                              const VkrRgPass *pass) {
  VkrVulkanGraphExecutorKind kind;
  if (!vkr_vk_graph_executor_kind(pass, &kind)) {
    log_error("Vulkan graph pass '%.*s' has no executor kind",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  switch (kind) {
  case VKR_VULKAN_GRAPH_EXECUTOR_SHADOW:
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING:
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE:
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION:
  case VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND:
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR:
  case VKR_VULKAN_GRAPH_EXECUTOR_UI:
    return vkr_vk_record_graph_graphics_pass(renderer, command, pass, kind);
  case VKR_VULKAN_GRAPH_EXECUTOR_IBL_BAKE:
    return vkr_vk_record_ibl_bakes(renderer, command);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_UPLOAD:
    return vkr_vk_record_deferred_upload(renderer, command, pass, false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD:
    return vkr_vk_record_deferred_upload(renderer, command, pass, true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY:
    return vkr_vk_record_deferred_cull(renderer, command, pass,
                                       VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY,
                                       false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_PREFIX:
    return vkr_vk_record_deferred_cull(
        renderer, command, pass, VKR_VULKAN_DEFERRED_PIPELINE_PREFIX, false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_ENCODE:
    return vkr_vk_record_deferred_cull(
        renderer, command, pass, VKR_VULKAN_DEFERRED_PIPELINE_ENCODE, false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY:
    return vkr_vk_record_deferred_cull(
        renderer, command, pass, VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY, true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX:
    return vkr_vk_record_deferred_cull(
        renderer, command, pass, VKR_VULKAN_DEFERRED_PIPELINE_PREFIX, true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE:
    return vkr_vk_record_deferred_cull(
        renderer, command, pass, VKR_VULKAN_DEFERRED_PIPELINE_ENCODE, true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GBUFFER_RESOLVE:
    return vkr_vk_record_deferred_gbuffer(renderer, command, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_LIGHTING_DEFERRED:
    return vkr_vk_record_deferred_lighting(renderer, command, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_HZB_BUILD:
    return vkr_vk_record_deferred_hzb(renderer, command, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SDSM_REDUCE:
    return vkr_vk_record_deferred_sdsm(renderer, command, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_SHADE:
    return vkr_vk_record_deferred_transmission(renderer, command, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN:
  case VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR:
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_DEPTH_SEED:
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_DEPTH_SEED:
    return vkr_vk_record_graph_transfer_pass(renderer, command, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_RESOLVE:
    return vkr_vk_record_deferred_picking(renderer, command, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COVERAGE:
    return vkr_vk_record_deferred_transmission_coverage(renderer, command,
                                                        pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COMPACT:
    return true_v;
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_READBACK:
    // The one-pixel copy is recorded after capture selection in record_draw().
    return true_v;
  default:
    return false_v;
  }
}

bool8_t vkr_vk_record_graph(VkrVulkanRenderer *renderer,
                            VkCommandBuffer command) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->pass_timing_count = 0u;
  slot->gpu_compaction_state = NULL;
  slot->transmission_gpu_compaction_state = NULL;
  slot->hzb_history_input = NULL;
  slot->hzb_history_output = NULL;
  slot->sdsm_reduce_state = NULL;
  const PFN_vkCmdBeginDebugUtilsLabelEXT begin_label =
      vkr_vulkan_device_cmd_begin_debug_label(renderer->device);
  const PFN_vkCmdEndDebugUtilsLabelEXT end_label =
      vkr_vulkan_device_cmd_end_debug_label(renderer->device);
  for (uint64_t order = 0; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t pass_index =
        *vector_get_uint32_t(&renderer->graph->execution_order, order);
    const VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, pass_index);
    const float64_t cpu_begin = vkr_platform_get_absolute_time();
    // Graph names are String8 and not null-terminated; pLabelName is a C
    // string. The copy is stack-only and bounded, so no per-pass allocation.
    if (begin_label) {
      char label_name[VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY];
      const uint64_t label_length =
          Min(pass->desc.name.length,
              (uint64_t)VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY - 1u);
      if (label_length > 0u)
        MemCopy(label_name, pass->desc.name.str, label_length);
      label_name[label_length] = '\0';
      const VkDebugUtilsLabelEXT label = {
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
          .pLabelName = label_name,
      };
      begin_label(command, &label);
    }
    const uint32_t timing_index = slot->pass_timing_count;
    const bool8_t timestamp_pass =
        slot->timing_requested &&
        timing_index < VKR_RENDERER_IMPL_MAX_PASS_TIMINGS;
    if (timestamp_pass) {
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                           slot->timestamp_pool, timing_index * 2u);
    }
    if (!vkr_vk_record_graph_pass_barriers(renderer, command, pass)) {
      log_error("Vulkan failed to record barriers for pass '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str);
      if (end_label)
        end_label(command);
      return false_v;
    }
    if (!vkr_vk_record_graph_pass(renderer, command, pass)) {
      log_error("Vulkan failed to record pass '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str);
      if (end_label)
        end_label(command);
      return false_v;
    }
    if (timestamp_pass) {
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                           slot->timestamp_pool, timing_index * 2u + 1u);
    }
    if (end_label)
      end_label(command);
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
  return vkr_vk_record_graph_image_barriers(
      renderer, command, &renderer->graph->terminal_image_barriers);
}

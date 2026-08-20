#include "renderer/vulkan/bindless/vkr_bindless_vulkan_internal.h"

vkr_internal uint64_t vkr_bindless_vk_capture_align(uint64_t value) {
  return (value + VKR_CAPTURE_BUFFER_ALIGNMENT - 1u) &
         ~(uint64_t)(VKR_CAPTURE_BUFFER_ALIGNMENT - 1u);
}

vkr_internal bool8_t vkr_bindless_vk_capture_source(
    const VkrCaptureChannelDescription *channel, const VkrRenderPacket *packet,
    bool8_t deferred_enabled, const char **out_name, uint32_t *out_layer) {
  const char *name = channel->source_name;
  uint32_t layer = 0u;
  if (string_equals(channel->name, "scene_color")) {
    name = packet->frame.editor_enabled ? "scene_color" : "hdr_scene_color";
  } else if (string_equals(channel->name, "depth")) {
    name = deferred_enabled
               ? "opaque_vbuffer_depth"
               : (packet->frame.editor_enabled ? "scene_depth"
                                               : "swapchain_depth");
  } else if (string_n_equals(channel->name, "shadow_cascade_", 15u)) {
    const char suffix = channel->name[15];
    if (suffix < '0' || suffix > '3')
      return false_v;
    layer = (uint32_t)(suffix - '0');
  } else if (string_equals(channel->name, "picking_ids") &&
             (!packet->picking || !packet->picking->pending)) {
    return false_v;
  }
  *out_name = name;
  *out_layer = layer;
  return true_v;
}

bool8_t vkr_bindless_vk_plan_capture(VkrBindlessVulkanRenderer *renderer,
                                     const VkrRenderPacket *packet,
                                     VkrBindlessVkFrameSlot *slot) {
  slot->capture_request_id = 0u;
  slot->capture_item_count = 0u;
  const VkrCaptureBatchRequest *request =
      packet->debug ? packet->debug->capture : NULL;
  if (!request)
    return true_v;
  if (!renderer->capture_ring.initialized || request->request_id == 0u ||
      !request->items || request->item_count == 0u ||
      request->item_count > VKR_CAPTURE_MAX_ITEMS)
    return false_v;

  uint64_t seen = 0u;
  uint64_t offset = 0u;
  for (uint32_t i = 0; i < request->item_count; ++i) {
    const VkrCaptureItemRequest *item = &request->items[i];
    const VkrCaptureChannelDescription *channel =
        vkr_renderer_capture_channel_get(item->channel);
    if (!channel || item->channel >= 64u || (seen & (1ull << item->channel)) ||
        item->mip != 0u || item->layer != 0u)
      return false_v;
    seen |= 1ull << item->channel;

    const char *source_name = NULL;
    uint32_t source_layer = 0u;
    if (!vkr_bindless_vk_capture_source(
            channel, packet, renderer->prepared_frame.deferred_enabled,
            &source_name, &source_layer))
      return false_v;
    const String8 graph_name = string8_create_from_cstr(
        (const uint8_t *)source_name, string_length(source_name));
    const VkrRgImageHandle handle =
        vkr_rg_find_image(renderer->graph, graph_name);
    VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
        renderer, handle, renderer->prepared_frame.image_index);
    if (!instance || source_layer >= instance->image.array_layers)
      return false_v;

    const VkrRgImage *graph_image =
        vector_get_VkrRgImage(&renderer->graph->images, handle.id - 1u);
    VkrTextureFormatInfo format_info = {0};
    if (!graph_image ||
        !vkr_texture_format_get_info(graph_image->desc.format, &format_info) ||
        format_info.block_width != 1u || format_info.block_height != 1u ||
        format_info.bytes_per_block == 0u)
      return false_v;
    const uint64_t row_pitch =
        (uint64_t)instance->image.width * format_info.bytes_per_block;
    const uint64_t data_size = row_pitch * instance->image.height;
    offset = vkr_bindless_vk_capture_align(offset);
    if (data_size == 0u || offset > UINT64_MAX - data_size ||
        offset + data_size > renderer->config.capture_max_batch_bytes)
      return false_v;

    slot->capture_images[i] = handle;
    slot->capture_plans[i] = (VkrCaptureBackendItemPlan){
        .result = {.channel = item->channel,
                   .width = instance->image.width,
                   .height = instance->image.height,
                   .row_pitch = row_pitch,
                   .format = graph_image->desc.format,
                   .value_kind = channel->value_kind,
                   .color_space = channel->color_space,
                   .origin = VKR_CAPTURE_ORIGIN_TOP_LEFT,
                   .data_size = data_size,
                   .mip = 0u,
                   .layer = source_layer,
                   .display_exposure = packet->globals.exposure},
        .buffer_offset = offset,
    };
    string_format(slot->capture_plans[i].result.producer_resource,
                  sizeof(slot->capture_plans[i].result.producer_resource), "%s",
                  source_name);
    offset += data_size;
  }
  const VkrRendererError reserve =
      vkr_capture_ring_reserve(&renderer->capture_ring, request,
                               slot->capture_plans, packet->frame.frame_index);
  if (reserve != VKR_RENDERER_ERROR_NONE) {
    log_error("Bindless Vulkan capture reservation %llu failed (%u)",
              (unsigned long long)request->request_id, reserve);
    return false_v;
  }
  slot->capture_request_id = request->request_id;
  slot->capture_item_count = request->item_count;
  return true_v;
}

bool8_t vkr_bindless_vk_record_capture(VkrBindlessVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       VkrBindlessVkFrameSlot *slot) {
  if (slot->capture_request_id == 0u)
    return true_v;
  for (uint32_t i = 0; i < slot->capture_item_count; ++i) {
    const VkrRgImageHandle handle = slot->capture_images[i];
    VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
        renderer, handle, renderer->prepared_frame.image_index);
    const VkrRgImage *graph_image =
        vkr_rg_image_handle_valid(handle)
            ? vector_get_VkrRgImage(&renderer->graph->images, handle.id - 1u)
            : NULL;
    if (!instance || !graph_image ||
        graph_image->final_layout == VKR_TEXTURE_LAYOUT_UNDEFINED)
      return false_v;
    const VkrCaptureBackendItemPlan *plan = &slot->capture_plans[i];
    const VkImageAspectFlags aspects =
        vkr_bindless_vk_format_aspects(instance->image.format);
    const VkImageLayout old_layout =
        vkr_bindless_vk_texture_layout(graph_image->final_layout);
    if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      const VkImageMemoryBarrier2 barrier = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          .srcAccessMask =
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
          .oldLayout = old_layout,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = instance->image.handle,
          .subresourceRange = {.aspectMask = aspects,
                               .baseMipLevel = plan->result.mip,
                               .levelCount = 1u,
                               .baseArrayLayer = plan->result.layer,
                               .layerCount = 1u},
      };
      const VkDependencyInfo dependency = {
          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
          .imageMemoryBarrierCount = 1u,
          .pImageMemoryBarriers = &barrier,
      };
      vkCmdPipelineBarrier2(command, &dependency);
    }
    const VkBufferImageCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = plan->buffer_offset,
        .imageSubresource = {.aspectMask = aspects,
                             .mipLevel = plan->result.mip,
                             .baseArrayLayer = plan->result.layer,
                             .layerCount = 1u},
        .imageExtent = {.width = plan->result.width,
                        .height = plan->result.height,
                        .depth = 1u},
    };
    const VkCopyImageToBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage = instance->image.handle,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = slot->capture_readback.handle,
        .regionCount = 1u,
        .pRegions = &region,
    };
    vkCmdCopyImageToBuffer2(command, &copy);
  }
  const VkBufferMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->capture_readback.handle,
      .size = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
  return true_v;
}

bool8_t vkr_bindless_vk_collect_captures(VkrBindlessVulkanRenderer *renderer,
                                         uint64_t completed_value) {
  if (!renderer->capture_ring.initialized)
    return true_v;
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->capture_request_id && slot->retire_value &&
        slot->retire_value <= completed_value &&
        !vkr_bindless_vk_invalidate(renderer,
                                    &slot->capture_readback.allocation, 0u,
                                    renderer->config.capture_max_batch_bytes)) {
      return false_v;
    }
  }
  vkr_capture_ring_collect(&renderer->capture_ring, completed_value);
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->capture_request_id && slot->retire_value &&
        slot->retire_value <= completed_value) {
      slot->capture_request_id = 0u;
      slot->capture_item_count = 0u;
    }
  }
  return true_v;
}

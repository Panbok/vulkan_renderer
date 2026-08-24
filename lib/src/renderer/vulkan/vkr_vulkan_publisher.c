#include "renderer/vulkan/vkr_vulkan_internal.h"
vkr_internal void
vkr_vk_advance_candidate_publication_generation(VkrVulkanRenderer *renderer) {
  if (renderer->candidate_publication_generation == UINT64_MAX) {
    renderer->candidate_publication_generation = 0u;
    log_fatal("Vulkan candidate publication generation exhausted");
    return;
  }
  renderer->candidate_publication_generation++;
}

vkr_internal uint64_t vkr_vk_candidate_publication_generation(void *state) {
  const VkrVulkanRenderer *renderer = state;
  return renderer ? renderer->candidate_publication_generation : 0u;
}

vkr_internal void vkr_vk_cmd_image_barrier_range(
    VkCommandBuffer command_buffer, VkImage image,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
    VkImageLayout old_layout, VkImageLayout new_layout, uint32_t level_count,
    uint32_t layer_count) {
  VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = level_count,
                           .layerCount = layer_count},
  };
  VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1u,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command_buffer, &dependency);
}

void vkr_vk_cmd_image_barrier(VkCommandBuffer command_buffer, VkImage image,
                              VkPipelineStageFlags2 src_stage,
                              VkAccessFlags2 src_access,
                              VkPipelineStageFlags2 dst_stage,
                              VkAccessFlags2 dst_access,
                              VkImageLayout old_layout,
                              VkImageLayout new_layout) {
  vkr_vk_cmd_image_barrier_range(command_buffer, image, src_stage, src_access,
                                 dst_stage, dst_access, old_layout, new_layout,
                                 1u, 1u);
}

vkr_internal void *vkr_vk_publication_source_alloc(VkrVulkanRenderer *renderer,
                                                   uint64_t size) {
  VkrDMemory *memory = &renderer->publication_staging_memory;
  if (size > UINT64_MAX - 64u)
    return NULL;
  const uint64_t required_free = size + 64u;
  if (vkr_dmemory_get_free_space(memory) < required_free) {
    if (memory->total_size >= memory->reserve_size)
      return NULL;
    const uint64_t growth_available = memory->reserve_size - memory->total_size;
    const uint64_t growth_required =
        required_free - vkr_dmemory_get_free_space(memory);
    const uint64_t growth_slack = growth_required <= UINT64_MAX - MB(8)
                                      ? growth_required + MB(8)
                                      : UINT64_MAX;
    const uint64_t grow_by = Min(growth_slack, growth_available);
    if (grow_by < growth_required ||
        !vkr_dmemory_resize(memory, memory->total_size + grow_by))
      return NULL;
  }
  void *result = vkr_dmemory_alloc(memory, size);
  if (!result && memory->total_size < memory->reserve_size) {
    const uint64_t grow_by =
        Min(size + MB(8), memory->reserve_size - memory->total_size);
    if (grow_by && vkr_dmemory_resize(memory, memory->total_size + grow_by))
      result = vkr_dmemory_alloc(memory, size);
  }
  return result;
}

vkr_internal void vkr_vk_release_texture_initialization(
    VkrVulkanRenderer *renderer,
    VkrVulkanPendingTextureInitialization *initialization) {
  vkr_vk_destroy_buffer(renderer, &initialization->staging);
  if (initialization->batches)
    vkr_allocator_free(renderer->allocator, initialization->batches,
                       initialization->batches_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (initialization->upload_data)
    (void)vkr_dmemory_free(&renderer->publication_staging_memory,
                           initialization->upload_data,
                           initialization->upload_data_size);
  MemZero(initialization, sizeof(*initialization));
}

vkr_internal uint8_t vkr_vk_material_pending_texture_count(
    const VkrVulkanRenderer *renderer,
    const VkrVulkanPublishedMaterial *material) {
  uint8_t pending = 0u;
  for (uint32_t i = 0u; i < ArrayCount(material->texture_record_indices); ++i) {
    const uint32_t texture_index = material->texture_record_indices[i];
    if (texture_index != UINT32_MAX &&
        renderer->published_textures[texture_index].initialization_pending)
      pending++;
  }
  return pending;
}

vkr_internal void
vkr_vk_refresh_material_texture_readiness(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0u; i < renderer->config.material_record_capacity; ++i) {
    VkrVulkanPublishedMaterial *material = &renderer->published_materials[i];
    if (material->live)
      material->pending_texture_count =
          vkr_vk_material_pending_texture_count(renderer, material);
  }
}

vkr_internal bool8_t vkr_vk_enqueue_texture_initialization(
    VkrVulkanRenderer *renderer,
    const VkrVulkanPendingTextureInitialization *initialization) {
  if (renderer->pending_texture_initialization_count >=
      renderer->config.texture_capacity)
    return false_v;
  renderer->pending_texture_initializations
      [renderer->pending_texture_initialization_count++] = *initialization;
  return true_v;
}

vkr_internal bool8_t vkr_vk_upload_prepared_texture(
    VkrVulkanRenderer *renderer, const VkrTexturePreparedLoad *prepared,
    VkrTextureHandle texture, VkrVulkanImage *out_image,
    VkrVulkanPendingTextureInitialization *out_initialization) {
  if (!prepared || !out_image || !out_initialization ||
      !prepared->upload_data || !prepared->upload_data_size ||
      !prepared->upload_regions || !prepared->upload_region_count ||
      !prepared->upload_mip_levels || !prepared->upload_array_layers ||
      (prepared->description.sample_count != 0u &&
       prepared->description.sample_count != VKR_SAMPLE_COUNT_1) ||
      (prepared->description.type != VKR_TEXTURE_TYPE_2D &&
       prepared->description.type != VKR_TEXTURE_TYPE_CUBE_MAP)) {
    log_error("Vulkan rejected invalid prepared texture metadata");
    return false_v;
  }
  const VkFormat format = vkr_vk_texture_format(prepared->description.format);
  if (format == VK_FORMAT_UNDEFINED) {
    log_error("Vulkan does not map prepared texture format %u",
              prepared->description.format);
    return false_v;
  }
  VkFormatProperties format_properties;
  vkGetPhysicalDeviceFormatProperties(
      vkr_vulkan_device_physical(renderer->device), format, &format_properties);
  if ((format_properties.optimalTilingFeatures &
       (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) !=
      (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
    log_error("Vulkan format %u lacks sampled/transfer-dst support", format);
    return false_v;
  }

  const bool8_t cube = prepared->description.type == VKR_TEXTURE_TYPE_CUBE_MAP;
  if (cube && prepared->upload_array_layers != 6u) {
    log_error("Vulkan cube texture has %u layers instead of 6",
              prepared->upload_array_layers);
    return false_v;
  }
  if (!vkr_vk_create_image_ex(
          renderer, prepared->description.width, prepared->description.height,
          prepared->upload_mip_levels, prepared->upload_array_layers, format,
          cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
          cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          out_image)) {
    log_error("Vulkan failed to create prepared texture image "
              "(%ux%u, format=%u, mips=%u, layers=%u, bytes=%llu)",
              prepared->description.width, prepared->description.height, format,
              prepared->upload_mip_levels, prepared->upload_array_layers,
              (unsigned long long)prepared->upload_data_size);
    return false_v;
  }

  uint32_t block_width = 0u;
  uint32_t block_height = 0u;
  uint32_t block_bytes = 0u;
  if (!vkr_vk_format_block_info(format, &block_width, &block_height,
                                &block_bytes)) {
    log_error("Vulkan cannot lower upload blocks for format %u", format);
    vkr_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  const uint64_t staging_limit = renderer->config.upload_buffer_block_size;
  uint64_t batch_count_u64 = 0u;
  bool8_t valid = true_v;
  for (uint32_t i = 0u; i < prepared->upload_region_count; ++i) {
    const VkrTextureUploadRegion *source = &prepared->upload_regions[i];
    if (!source->width || !source->height || !source->depth ||
        source->mip_level >= prepared->upload_mip_levels ||
        source->array_layer >= prepared->upload_array_layers ||
        source->byte_offset > prepared->upload_data_size ||
        source->byte_size > prepared->upload_data_size - source->byte_offset) {
      valid = false_v;
      break;
    }
    if (source->byte_size <= staging_limit) {
      batch_count_u64++;
      continue;
    }
    const uint64_t blocks_w = (source->width + block_width - 1u) / block_width;
    const uint64_t blocks_h =
        (source->height + block_height - 1u) / block_height;
    const uint64_t row_bytes = blocks_w * block_bytes;
    const uint64_t expected_size = row_bytes * blocks_h * source->depth;
    const uint64_t rows_per_batch = row_bytes ? staging_limit / row_bytes : 0u;
    if (!row_bytes || !rows_per_batch || expected_size != source->byte_size) {
      valid = false_v;
      break;
    }
    batch_count_u64 += (uint64_t)source->depth *
                       ((blocks_h + rows_per_batch - 1u) / rows_per_batch);
  }
  if (!valid || !batch_count_u64 || batch_count_u64 > UINT32_MAX) {
    log_error("Vulkan rejected texture upload region layout");
    vkr_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  const uint32_t batch_count = (uint32_t)batch_count_u64;
  const uint64_t batches_size =
      batch_count_u64 * sizeof(VkrVulkanTextureUploadBatch);
  VkrVulkanTextureUploadBatch *batches = vkr_allocator_alloc(
      renderer->allocator, batches_size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  uint8_t *upload_data =
      vkr_vk_publication_source_alloc(renderer, prepared->upload_data_size);
  if (!batches || !upload_data) {
    log_error("Vulkan failed to retain %llu texture bytes and %u "
              "upload regions",
              (unsigned long long)prepared->upload_data_size, batch_count);
    if (batches)
      vkr_allocator_free(renderer->allocator, batches, batches_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (upload_data)
      (void)vkr_dmemory_free(&renderer->publication_staging_memory, upload_data,
                             prepared->upload_data_size);
    vkr_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  MemZero(batches, batches_size);
  MemCopy(upload_data, prepared->upload_data, prepared->upload_data_size);
  uint32_t batch_index = 0u;
  for (uint32_t i = 0; i < prepared->upload_region_count; ++i) {
    const VkrTextureUploadRegion *source = &prepared->upload_regions[i];
    if (source->byte_size <= staging_limit) {
      VkrVulkanTextureUploadBatch *batch = &batches[batch_index++];
      batch->source_offset = source->byte_offset;
      batch->source_size = source->byte_size;
      batch->region = (VkBufferImageCopy2){
          .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
          .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .mipLevel = source->mip_level,
                               .baseArrayLayer = source->array_layer,
                               .layerCount = 1u},
          .imageExtent = {.width = source->width,
                          .height = source->height,
                          .depth = source->depth},
      };
      continue;
    }
    const uint64_t blocks_w = (source->width + block_width - 1u) / block_width;
    const uint64_t blocks_h =
        (source->height + block_height - 1u) / block_height;
    const uint64_t row_bytes = blocks_w * block_bytes;
    const uint64_t rows_per_batch = staging_limit / row_bytes;
    for (uint32_t z = 0u; z < source->depth; ++z) {
      for (uint64_t first_row = 0u; first_row < blocks_h;
           first_row += rows_per_batch) {
        const uint64_t row_count = Min(rows_per_batch, blocks_h - first_row);
        const uint32_t y = (uint32_t)(first_row * block_height);
        VkrVulkanTextureUploadBatch *batch = &batches[batch_index++];
        batch->source_offset = source->byte_offset +
                               ((uint64_t)z * blocks_h + first_row) * row_bytes;
        batch->source_size = row_count * row_bytes;
        batch->region = (VkBufferImageCopy2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = source->mip_level,
                                 .baseArrayLayer = source->array_layer,
                                 .layerCount = 1u},
            .imageOffset = {.x = 0, .y = (int32_t)y, .z = (int32_t)z},
            .imageExtent = {.width = source->width,
                            .height = Min((uint32_t)(row_count * block_height),
                                          source->height - y),
                            .depth = 1u},
        };
      }
    }
  }
  valid = batch_index == batch_count;
  if (valid) {
    *out_initialization = (VkrVulkanPendingTextureInitialization){
        .batches = batches,
        .batches_size = batches_size,
        .upload_data = upload_data,
        .upload_data_size = prepared->upload_data_size,
        .texture = texture,
        .batch_count = batch_count,
    };
    out_image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  } else {
    log_error("Vulkan rejected an out-of-range texture upload region");
    VkrVulkanPendingTextureInitialization initialization = {
        .batches = batches,
        .batches_size = batches_size,
        .upload_data = upload_data,
        .upload_data_size = prepared->upload_data_size,
        .batch_count = batch_count,
    };
    vkr_vk_release_texture_initialization(renderer, &initialization);
    vkr_vk_destroy_image(renderer, out_image);
  }
  return valid;
}

vkr_internal bool8_t
vkr_vk_stage_next_buffer_batch(VkrVulkanRenderer *renderer) {
  if (renderer->staging_buffer_count)
    return true_v;
  for (uint32_t i = 0u; i < renderer->pending_buffer_initialization_count;
       ++i) {
    VkrVulkanPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    if (initialization->next_offset >= initialization->size)
      continue;
    const VkDeviceSize chunk_size =
        Min((VkDeviceSize)renderer->config.upload_buffer_block_size,
            initialization->size - initialization->next_offset);
    if (!vkr_vk_create_buffer(renderer, VKR_VULKAN_MEMORY_CLASS_STAGING,
                              chunk_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              &initialization->staging)) {
      log_error("Vulkan failed to create bounded %llu-byte buffer "
                "staging chunk at offset %llu/%llu",
                (unsigned long long)chunk_size,
                (unsigned long long)initialization->next_offset,
                (unsigned long long)initialization->size);
      return false_v;
    }
    MemCopy(initialization->staging.allocation.mapped,
            initialization->upload_data + initialization->next_offset,
            chunk_size);
    if (!vkr_vk_flush(renderer, &initialization->staging.allocation, 0u,
                      chunk_size)) {
      vkr_vk_destroy_buffer(renderer, &initialization->staging);
      return false_v;
    }
    renderer->staging_buffer_count++;
    return true_v;
  }
  return true_v;
}

/** Retains source bytes in CPU memory and materializes at most one bounded
 *
 * host-visible batch at a time. This keeps large texture publication below
 *
 * small discrete-GPU host-visible heap budgets without waiting the queue. */
vkr_internal bool8_t
vkr_vk_stage_next_texture_batch(VkrVulkanRenderer *renderer) {
  if (renderer->staging_buffer_count)
    return true_v;
  for (uint32_t i = 0u; i < renderer->pending_texture_initialization_count;
       ++i) {
    VkrVulkanPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    if (initialization->writable ||
        initialization->next_batch >= initialization->batch_count)
      continue;
    VkrVulkanTextureUploadBatch *first_batch =
        &initialization->batches[initialization->next_batch];
    if (first_batch->source_size > renderer->config.upload_buffer_block_size) {
      log_error("Vulkan texture upload region %u/%u is %llu bytes; "
                "the bounded staging limit is %llu bytes",
                initialization->next_batch + 1u, initialization->batch_count,
                (unsigned long long)first_batch->source_size,
                (unsigned long long)renderer->config.upload_buffer_block_size);
      return false_v;
    }
    uint64_t staging_size = 0u;
    uint32_t staged_count = 0u;
    for (uint32_t batch_index = initialization->next_batch;
         batch_index < initialization->batch_count; ++batch_index) {
      const VkrVulkanTextureUploadBatch *batch =
          &initialization->batches[batch_index];
      const uint64_t offset = vkr_vk_align_up(staging_size, 16u);
      if (offset > renderer->config.upload_buffer_block_size ||
          batch->source_size >
              renderer->config.upload_buffer_block_size - offset)
        break;
      staging_size = offset + batch->source_size;
      staged_count++;
    }
    if (!vkr_vk_create_buffer(renderer, VKR_VULKAN_MEMORY_CLASS_STAGING,
                              staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              &initialization->staging)) {
      log_error("Vulkan failed to create bounded %llu-byte texture "
                "staging chunk at region %u/%u",
                (unsigned long long)staging_size,
                initialization->next_batch + 1u, initialization->batch_count);
      return false_v;
    }
    uint64_t destination_offset = 0u;
    for (uint32_t batch_offset = 0u; batch_offset < staged_count;
         ++batch_offset) {
      VkrVulkanTextureUploadBatch *batch =
          &initialization->batches[initialization->next_batch + batch_offset];
      destination_offset = vkr_vk_align_up(destination_offset, 16u);
      batch->region.bufferOffset = destination_offset;
      MemCopy((uint8_t *)initialization->staging.allocation.mapped +
                  destination_offset,
              initialization->upload_data + batch->source_offset,
              batch->source_size);
      destination_offset += batch->source_size;
    }
    if (!vkr_vk_flush(renderer, &initialization->staging.allocation, 0u,
                      staging_size)) {
      log_error("Vulkan failed to flush bounded texture staging "
                "chunk at region %u/%u",
                initialization->next_batch + 1u, initialization->batch_count);
      vkr_vk_destroy_buffer(renderer, &initialization->staging);
      return false_v;
    }
    initialization->staged_batch_count = staged_count;
    renderer->staging_buffer_count++;
    return true_v;
  }
  return true_v;
}

/** Claims the single bounded host-visible staging chunk for one publication
 *
 * class per frame, alternating between buffers and textures.
 *
 * Both classes share `staging_buffer_count`, and a chunk stays claimed until
 * the GPU completes its copy. Trying buffers first unconditionally therefore
 * starves textures outright rather than merely delaying them: a scene that
 * still has geometry to publish reclaims the slot every time it frees, so
 * material textures never leave `initialization_pending`, every draw whose
 * material references them is skipped, and the deferred resolve samples an
 * image that was never uploaded. Alternation preserves the memory bound and
 * the one-chunk-per-frame submit invariant while giving both classes
 * throughput. The flag only turns when a chunk is actually claimed, so a class
 * with nothing pending never costs the other one a frame. */
bool8_t vkr_vk_stage_next_publication_batch(VkrVulkanRenderer *renderer) {
  const uint32_t claimed_before = renderer->staging_buffer_count;
  const bool8_t textures_first = renderer->stage_textures_first;
  const bool8_t staged = textures_first
                             ? (vkr_vk_stage_next_texture_batch(renderer) &&
                                vkr_vk_stage_next_buffer_batch(renderer))
                             : (vkr_vk_stage_next_buffer_batch(renderer) &&
                                vkr_vk_stage_next_texture_batch(renderer));
  if (renderer->staging_buffer_count != claimed_before)
    renderer->stage_textures_first = !textures_first;
  return staged;
}

void vkr_vk_record_texture_initializations(VkrVulkanRenderer *renderer,
                                           VkCommandBuffer command) {
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    const VkrVulkanPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    const VkrVulkanPublishedTexture *texture =
        vkr_vk_texture_publication(renderer, initialization->texture);
    if (!texture) {
      log_fatal("Vulkan lost texture %u:%u before initialization "
                "recording",
                initialization->texture.id, initialization->texture.generation);
      return;
    }
    if (initialization->writable) {
      vkr_vk_cmd_image_barrier_range(
          command, texture->image.handle, VK_PIPELINE_STAGE_2_NONE,
          VK_ACCESS_2_NONE,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          texture->image.mip_levels, texture->image.array_layers);
      continue;
    }
    if (initialization->next_batch >= initialization->batch_count ||
        !initialization->staged_batch_count || !initialization->staging.handle)
      continue;
    if (initialization->next_batch == 0u)
      vkr_vk_cmd_image_barrier_range(
          command, texture->image.handle, VK_PIPELINE_STAGE_2_NONE,
          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture->image.mip_levels,
          texture->image.array_layers);
    for (uint32_t batch_offset = 0u;
         batch_offset < initialization->staged_batch_count; ++batch_offset) {
      const VkrVulkanTextureUploadBatch *batch =
          &initialization->batches[initialization->next_batch + batch_offset];
      const VkCopyBufferToImageInfo2 copy_info = {
          .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
          .srcBuffer = initialization->staging.handle,
          .dstImage = texture->image.handle,
          .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .regionCount = 1u,
          .pRegions = &batch->region,
      };
      vkCmdCopyBufferToImage2(command, &copy_info);
    }
    if (initialization->next_batch + initialization->staged_batch_count ==
        initialization->batch_count)
      vkr_vk_cmd_image_barrier_range(
          command, texture->image.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
          VK_ACCESS_2_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture->image.mip_levels,
          texture->image.array_layers);
  }
}

void vkr_vk_record_buffer_initializations(VkrVulkanRenderer *renderer,
                                          VkCommandBuffer command) {
  VkrVulkanGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  if (mega->copy_pending) {
    VkBufferCopy2 regions[2] = {
        {.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
         .size = mega->copy_vertex_size},
        {.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
         .size = mega->copy_index_size},
    };
    VkCopyBufferInfo2 copies[2] = {
        {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
         .srcBuffer = mega->copy_source_vertices.handle,
         .dstBuffer = mega->vertices.handle,
         .regionCount = 1u,
         .pRegions = &regions[0]},
        {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
         .srcBuffer = mega->copy_source_indices.handle,
         .dstBuffer = mega->indices.handle,
         .regionCount = 1u,
         .pRegions = &regions[1]},
    };
    if (regions[0].size)
      vkCmdCopyBuffer2(command, &copies[0]);
    if (regions[1].size)
      vkCmdCopyBuffer2(command, &copies[1]);
    const VkBufferMemoryBarrier2 barriers[2] = {
        {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = mega->vertices.handle,
         .size = VK_WHOLE_SIZE},
        {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         .dstAccessMask =
             VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = mega->indices.handle,
         .size = VK_WHOLE_SIZE},
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = ArrayCount(barriers),
        .pBufferMemoryBarriers = barriers,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
  for (uint32_t i = 0; i < renderer->pending_buffer_initialization_count; ++i) {
    const VkrVulkanPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    if (!initialization->staging.handle)
      continue;
    const VkBufferCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .dstOffset =
            initialization->destination_offset + initialization->next_offset,
        .size = initialization->staging.size,
    };
    const VkCopyBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer = initialization->staging.handle,
        .dstBuffer = initialization->destination,
        .regionCount = 1u,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(command, &copy);
    if (initialization->next_offset + initialization->staging.size <
        initialization->size)
      continue;
    const VkBufferMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = initialization->destination_stage,
        .dstAccessMask = initialization->destination_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = initialization->destination,
        .size = VK_WHOLE_SIZE,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1u,
        .pBufferMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
}

vkr_internal VkrVulkanRetiredStagingBuffer *
vkr_vk_reserve_staging_retirement(VkrVulkanRenderer *renderer) {
  for (uint32_t slot = 0; slot < renderer->retired_staging_buffer_capacity;
       ++slot) {
    if (!renderer->retired_staging_buffers[slot].occupied)
      return &renderer->retired_staging_buffers[slot];
  }
  return NULL;
}

vkr_internal bool8_t vkr_vk_retire_submitted_staging(
    VkrVulkanRenderer *renderer, VkrVulkanBuffer *staging,
    uint64_t retire_value) {
  VkrVulkanRetiredStagingBuffer *retired =
      vkr_vk_reserve_staging_retirement(renderer);
  if (!retired) {
    log_error("Vulkan exhausted bounded staging retirement capacity");
    return false_v;
  }
  if (!vkr_vk_retire_buffer(renderer, staging, retire_value)) {
    log_error("Vulkan failed to retire submitted staging memory");
    return false_v;
  }
  *retired = (VkrVulkanRetiredStagingBuffer){
      .buffer = *staging,
      .retire_value = retire_value,
      .occupied = true_v,
  };
  return true_v;
}

vkr_internal VkrVulkanRetiredGeometryMegabuffer *
vkr_vk_reserve_retired_geometry_megabuffer(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0u; i < ArrayCount(renderer->geometry_megabuffer.retired);
       ++i) {
    VkrVulkanRetiredGeometryMegabuffer *retired =
        &renderer->geometry_megabuffer.retired[i];
    if (!retired->occupied)
      return retired;
  }
  return NULL;
}

vkr_internal void vkr_vk_release_buffer_initialization(
    VkrVulkanRenderer *renderer,
    VkrVulkanPendingBufferInitialization *initialization) {
  vkr_vk_destroy_buffer(renderer, &initialization->staging);
  if (initialization->upload_data)
    (void)vkr_dmemory_free(&renderer->publication_staging_memory,
                           initialization->upload_data, initialization->size);
  MemZero(initialization, sizeof(*initialization));
}

bool8_t vkr_vk_commit_buffer_initializations(VkrVulkanRenderer *renderer,
                                             uint64_t retire_value) {
  VkrVulkanGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  if (mega->copy_pending) {
    VkrVulkanRetiredGeometryMegabuffer *retired =
        vkr_vk_reserve_retired_geometry_megabuffer(renderer);
    if (!retired ||
        !vkr_vk_retire_buffer(renderer, &mega->copy_source_vertices,
                              retire_value) ||
        !vkr_vk_retire_buffer(renderer, &mega->copy_source_indices,
                              retire_value)) {
      log_error("Vulkan could not retire a submitted geometry "
                "megabuffer generation");
      return false_v;
    }
    *retired = (VkrVulkanRetiredGeometryMegabuffer){
        .vertices = mega->copy_source_vertices,
        .indices = mega->copy_source_indices,
        .retire_value = retire_value,
        .occupied = true_v,
    };
    mega->copy_source_vertices = (VkrVulkanBuffer){0};
    mega->copy_source_indices = (VkrVulkanBuffer){0};
    mega->copy_vertex_size = 0u;
    mega->copy_index_size = 0u;
    mega->copy_pending = false_v;
  }
  VkrVulkanPendingBufferInitialization *submitted = NULL;
  for (uint32_t i = 0u; i < renderer->pending_buffer_initialization_count;
       ++i) {
    VkrVulkanPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    if (!initialization->staging.handle)
      continue;
    if (submitted) {
      log_error("Vulkan submitted more than one bounded staging "
                "buffer in a frame");
      return false_v;
    }
    submitted = initialization;
  }
  if (submitted && !vkr_vk_retire_submitted_staging(
                       renderer, &submitted->staging, retire_value))
    return false_v;

  uint32_t write_index = 0u;
  const uint32_t pending_count = renderer->pending_buffer_initialization_count;
  for (uint32_t read_index = 0u; read_index < pending_count; ++read_index) {
    VkrVulkanPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[read_index];
    VkrVulkanPublishedGeometry *geometry =
        &renderer->published_geometries[initialization->geometry_record_index];
    if (!initialization->staging.handle) {
      if (write_index != read_index) {
        renderer->pending_buffer_initializations[write_index] = *initialization;
        MemZero(initialization, sizeof(*initialization));
      }
      write_index++;
      continue;
    }
    initialization->next_offset += initialization->staging.size;
    MemZero(&initialization->staging, sizeof(initialization->staging));
    geometry->last_use_submit_value =
        Max(geometry->last_use_submit_value, retire_value);
    if (initialization->next_offset == initialization->size) {
      if (geometry->pending_initialization_count)
        geometry->pending_initialization_count--;
      if (!geometry->pending_initialization_count)
        vkr_vk_advance_candidate_publication_generation(renderer);
      vkr_vk_release_buffer_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index) {
      renderer->pending_buffer_initializations[write_index] = *initialization;
      MemZero(initialization, sizeof(*initialization));
    }
    write_index++;
  }
  for (uint32_t i = write_index; i < pending_count; ++i)
    MemZero(&renderer->pending_buffer_initializations[i],
            sizeof(renderer->pending_buffer_initializations[i]));
  renderer->pending_buffer_initialization_count = write_index;
  return true_v;
}

void vkr_vk_discard_buffer_initializations(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->pending_buffer_initialization_count; ++i) {
    VkrVulkanPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    VkrVulkanPublishedGeometry *geometry =
        &renderer->published_geometries[initialization->geometry_record_index];
    if (initialization->staging.handle && renderer->staging_buffer_count)
      renderer->staging_buffer_count--;
    if (geometry->pending_initialization_count)
      geometry->pending_initialization_count--;
    vkr_vk_release_buffer_initialization(renderer, initialization);
  }
  renderer->pending_buffer_initialization_count = 0u;
}

vkr_internal void
vkr_vk_discard_geometry_initializations(VkrVulkanRenderer *renderer,
                                        uint32_t geometry_record_index) {
  uint32_t write_index = 0u;
  for (uint32_t read_index = 0;
       read_index < renderer->pending_buffer_initialization_count;
       ++read_index) {
    VkrVulkanPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[read_index];
    if (initialization->geometry_record_index == geometry_record_index) {
      if (initialization->staging.handle && renderer->staging_buffer_count)
        renderer->staging_buffer_count--;
      vkr_vk_release_buffer_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index)
      renderer->pending_buffer_initializations[write_index] = *initialization;
    write_index++;
  }
  for (uint32_t i = write_index;
       i < renderer->pending_buffer_initialization_count; ++i)
    MemZero(&renderer->pending_buffer_initializations[i],
            sizeof(renderer->pending_buffer_initializations[i]));
  renderer->pending_buffer_initialization_count = write_index;
  renderer->published_geometries[geometry_record_index]
      .pending_initialization_count = 0u;
}

bool8_t vkr_vk_commit_texture_initializations(VkrVulkanRenderer *renderer,
                                              uint64_t retire_value) {
  VkrVulkanPendingTextureInitialization *submitted = NULL;
  for (uint32_t i = 0u; i < renderer->pending_texture_initialization_count;
       ++i) {
    VkrVulkanPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    if (!vkr_vk_texture_publication(renderer, initialization->texture)) {
      log_error("Vulkan lost texture %u:%u before initialization "
                "commit",
                initialization->texture.id, initialization->texture.generation);
      return false_v;
    }
    if (!initialization->staging.handle)
      continue;
    if (submitted) {
      log_error("Vulkan submitted more than one bounded staging "
                "buffer in a frame");
      return false_v;
    }
    submitted = initialization;
  }
  if (submitted && !vkr_vk_retire_submitted_staging(
                       renderer, &submitted->staging, retire_value))
    return false_v;

  uint32_t write_index = 0u;
  bool8_t readiness_changed = false_v;
  const uint32_t pending_count = renderer->pending_texture_initialization_count;
  for (uint32_t read_index = 0; read_index < pending_count; ++read_index) {
    VkrVulkanPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[read_index];
    VkrVulkanPublishedTexture *texture =
        vkr_vk_texture_publication(renderer, initialization->texture);
    if (!texture)
      return false_v;
    bool8_t progressed = initialization->writable;
    if (!initialization->writable &&
        initialization->next_batch < initialization->batch_count &&
        initialization->staging.handle && initialization->staged_batch_count) {
      MemZero(&initialization->staging, sizeof(initialization->staging));
      initialization->next_batch += initialization->staged_batch_count;
      initialization->staged_batch_count = 0u;
      progressed = true_v;
    }
    if (progressed)
      texture->last_use_submit_value =
          Max(texture->last_use_submit_value, retire_value);
    const bool8_t completed =
        initialization->writable ||
        initialization->next_batch == initialization->batch_count;
    if (completed) {
      texture->initialization_pending = false_v;
      readiness_changed = true_v;
      vkr_vk_release_texture_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index) {
      renderer->pending_texture_initializations[write_index] = *initialization;
      MemZero(initialization, sizeof(*initialization));
    }
    write_index++;
  }
  for (uint32_t i = write_index; i < pending_count; ++i)
    MemZero(&renderer->pending_texture_initializations[i],
            sizeof(renderer->pending_texture_initializations[i]));
  renderer->pending_texture_initialization_count = write_index;
  if (readiness_changed)
    vkr_vk_refresh_material_texture_readiness(renderer);
  return true_v;
}

void vkr_vk_discard_texture_initializations(VkrVulkanRenderer *renderer) {
  const bool8_t readiness_changed =
      renderer->pending_texture_initialization_count > 0u;
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    VkrVulkanPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    VkrVulkanPublishedTexture *texture =
        vkr_vk_texture_publication(renderer, initialization->texture);
    if (texture)
      texture->initialization_pending = false_v;
    if (initialization->staging.handle && renderer->staging_buffer_count)
      renderer->staging_buffer_count--;
    vkr_vk_release_texture_initialization(renderer, initialization);
  }
  renderer->pending_texture_initialization_count = 0u;
  if (readiness_changed)
    vkr_vk_refresh_material_texture_readiness(renderer);
}

vkr_internal void
vkr_vk_cancel_texture_initialization(VkrVulkanRenderer *renderer,
                                     VkrTextureHandle texture_handle) {
  uint32_t write_index = 0u;
  const uint32_t pending_count = renderer->pending_texture_initialization_count;
  for (uint32_t read_index = 0u; read_index < pending_count; ++read_index) {
    VkrVulkanPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[read_index];
    if (initialization->texture.id == texture_handle.id &&
        initialization->texture.generation == texture_handle.generation) {
      if (initialization->staging.handle && renderer->staging_buffer_count)
        renderer->staging_buffer_count--;
      vkr_vk_release_texture_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index) {
      renderer->pending_texture_initializations[write_index] = *initialization;
      MemZero(initialization, sizeof(*initialization));
    }
    write_index++;
  }
  for (uint32_t i = write_index; i < pending_count; ++i)
    MemZero(&renderer->pending_texture_initializations[i],
            sizeof(renderer->pending_texture_initializations[i]));
  renderer->pending_texture_initialization_count = write_index;
  VkrVulkanPublishedTexture *texture =
      vkr_vk_texture_publication(renderer, texture_handle);
  if (texture) {
    texture->initialization_pending = false_v;
    vkr_vk_refresh_material_texture_readiness(renderer);
  }
}

bool8_t vkr_vk_publish_sampled_view(VkrVulkanRenderer *renderer,
                                    VkImageView view,
                                    VkImageLayout image_layout,
                                    VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_vulkan_device_descriptor_properties(renderer->device);
  const VkrVulkanDescriptorLayout *layout =
      vkr_vulkan_device_resource_layout(renderer->device);
  const VkDescriptorImageInfo image_info = {
      .imageView = view,
      .imageLayout = image_layout,
  };
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .data.pSampledImage = &image_info,
  };
  vkr_vulkan_device_get_descriptor(renderer->device)(
      vkr_vk_renderer_device(renderer), &get_info,
      properties->sampledImageDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampled_image_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  if (vkr_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                        &renderer->resource_descriptors,
                        layout->sampled_image_offset +
                            (VkDeviceSize)out_handle->index *
                                properties->sampledImageDescriptorSize,
                        properties->sampledImageDescriptorSize))
    return true_v;
  (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots, *out_handle,
                                  renderer->completed_value);
  (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots,
                                   renderer->completed_value, NULL);
  *out_handle = (VkrGpuSlotHandle){0};
  return false_v;
}

bool8_t vkr_vk_publish_storage_view(VkrVulkanRenderer *renderer,
                                    VkImageView view,
                                    VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_vulkan_device_descriptor_properties(renderer->device);
  const VkrVulkanDescriptorLayout *layout =
      vkr_vulkan_device_resource_layout(renderer->device);
  const VkDescriptorImageInfo image_info = {
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .data.pStorageImage = &image_info,
  };
  vkr_vulkan_device_get_descriptor(renderer->device)(
      vkr_vk_renderer_device(renderer), &get_info,
      properties->storageImageDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->storage_image_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  if (vkr_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                        &renderer->resource_descriptors,
                        layout->storage_image_offset +
                            (VkDeviceSize)out_handle->index *
                                properties->storageImageDescriptorSize,
                        properties->storageImageDescriptorSize))
    return true_v;
  (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots, *out_handle,
                                  renderer->completed_value);
  (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots,
                                   renderer->completed_value, NULL);
  *out_handle = (VkrGpuSlotHandle){0};
  return false_v;
}

vkr_internal bool8_t vkr_vk_publish_sampler(VkrVulkanRenderer *renderer,
                                            VkSampler sampler,
                                            VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_vulkan_device_descriptor_properties(renderer->device);
  const VkrVulkanDescriptorLayout *layout =
      vkr_vulkan_device_sampler_layout(renderer->device);
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .data.pSampler = &sampler,
  };
  vkr_vulkan_device_get_descriptor(renderer->device)(
      vkr_vk_renderer_device(renderer), &get_info,
      properties->samplerDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampler_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_vk_mark_dirty(
      &renderer->sampler_descriptor_dirty, &renderer->sampler_descriptors,
      layout->sampler_offset +
          (VkDeviceSize)out_handle->index * properties->samplerDescriptorSize,
      properties->samplerDescriptorSize);
}

vkr_internal bool8_t vkr_vk_publish_material_gpu_row(
    VkrVulkanRenderer *renderer, const VkrVulkanMaterialGpuRow *row,
    VkrGpuSlotHandle *out_handle) {
  if (vkr_gpu_slot_table_publish(renderer->material_slots, row, out_handle) !=
      VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_vk_mark_dirty(&renderer->material_dirty, &renderer->materials,
                           (VkDeviceSize)out_handle->index * sizeof(*row),
                           sizeof(*row));
}

vkr_internal bool8_t vkr_vk_replace_material_gpu_row(
    VkrVulkanRenderer *renderer, VkrGpuSlotHandle old_handle,
    const VkrVulkanMaterialGpuRow *row, uint64_t old_last_use_submit_value,
    VkrGpuSlotHandle *out_handle) {
  if (vkr_gpu_slot_table_replace(renderer->material_slots, old_handle, row,
                                 old_last_use_submit_value,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_vk_mark_dirty(&renderer->material_dirty, &renderer->materials,
                           (VkDeviceSize)out_handle->index * sizeof(*row),
                           sizeof(*row));
}

vkr_internal bool8_t vkr_vk_publish_material_row(VkrVulkanRenderer *renderer,
                                                 uint32_t texture_index,
                                                 uint32_t sampler_index,
                                                 uint32_t material_id,
                                                 VkrGpuSlotHandle *out_handle) {
  const VkrPbrProperties pbr = {
      .roughness = 1.0f,
      .normal_scale = 1.0f,
      .occlusion_strength = 1.0f,
      .dielectric_specular = {0.04f, 0.04f, 0.04f},
      .ior = 1.5f,
      .attenuation_color = {1.0f, 1.0f, 1.0f},
  };
  const VkrPacketMaterialConstants material =
      vkr_packet_derive_material_constants(&pbr, 0.5f,
                                           VKR_MATERIAL_ALPHA_OPAQUE);
  const VkrVulkanMaterialGpuRow row = {
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
      .base_color_texture = texture_index,
      .normal_texture = texture_index,
      .orm_texture = texture_index,
      .emissive_texture = texture_index,
      .base_color_sampler = sampler_index,
      .normal_sampler = sampler_index,
      .orm_sampler = sampler_index,
      .emissive_sampler = sampler_index,
      .material_id = material_id,
      .alpha_mode = material.alpha_mode,
      .material_emissive = material.emissive,
      .material_dielectric_specular = material.dielectric_specular,
      .material_surface = material.surface,
      .material_alpha = material.alpha,
      .material_attenuation_color = material.attenuation_color,
  };
  return vkr_vk_publish_material_gpu_row(renderer, &row, out_handle);
}

vkr_internal VkSamplerAddressMode
vkr_vk_sampler_address_mode(VkrTextureRepeatMode mode) {
  switch (mode) {
  case VKR_TEXTURE_REPEAT_MODE_MIRRORED_REPEAT:
    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  default:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

vkr_internal bool8_t vkr_vk_create_published_sampler(
    VkrVulkanRenderer *renderer, const VkrTextureDescription *description,
    uint32_t mip_levels, VkSampler *out_sampler) {
  const VkSamplerCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = description->mag_filter == VKR_FILTER_LINEAR
                       ? VK_FILTER_LINEAR
                       : VK_FILTER_NEAREST,
      .minFilter = description->min_filter == VKR_FILTER_LINEAR
                       ? VK_FILTER_LINEAR
                       : VK_FILTER_NEAREST,
      .mipmapMode = description->mip_filter == VKR_MIP_FILTER_LINEAR
                        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                        : VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = vkr_vk_sampler_address_mode(description->u_repeat_mode),
      .addressModeV = vkr_vk_sampler_address_mode(description->v_repeat_mode),
      .addressModeW = vkr_vk_sampler_address_mode(description->w_repeat_mode),
      .maxLod = description->mip_filter == VKR_MIP_FILTER_NONE
                    ? 0.0f
                    : (float32_t)(mip_levels - 1u),
      .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
  };
  return vkCreateSampler(vkr_vk_renderer_device(renderer), &info, NULL,
                         out_sampler) == VK_SUCCESS;
}

vkr_internal bool8_t vkr_vk_sampler_description_equal(
    const VkrTextureDescription *a, uint32_t a_mip_levels,
    const VkrTextureDescription *b, uint32_t b_mip_levels) {
  if (!a || !b)
    return false_v;
  const uint32_t a_sampled_mip_levels =
      a->mip_filter == VKR_MIP_FILTER_NONE ? 1u : a_mip_levels;
  const uint32_t b_sampled_mip_levels =
      b->mip_filter == VKR_MIP_FILTER_NONE ? 1u : b_mip_levels;
  return a->min_filter == b->min_filter && a->mag_filter == b->mag_filter &&
         a->mip_filter == b->mip_filter &&
         a->u_repeat_mode == b->u_repeat_mode &&
         a->v_repeat_mode == b->v_repeat_mode &&
         a->w_repeat_mode == b->w_repeat_mode &&
         a_sampled_mip_levels == b_sampled_mip_levels;
}

vkr_internal uint32_t vkr_vk_mip_count(uint32_t width, uint32_t height) {
  uint32_t levels = 1u;
  for (uint32_t extent = Max(width, height); extent > 1u; extent >>= 1u)
    levels++;
  return levels;
}

vkr_internal bool8_t vkr_vk_acquire_sampler(
    VkrVulkanRenderer *renderer, const VkrTextureDescription *description,
    uint32_t mip_levels, uint32_t *out_record_index) {
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    VkrVulkanPublishedSampler *record = &renderer->published_samplers[i];
    if (record->live && vkr_vk_sampler_description_equal(
                            &record->description, record->mip_levels,
                            description, mip_levels)) {
      record->reference_count++;
      *out_record_index = i;
      return true_v;
    }
  }
  uint32_t free_index = UINT32_MAX;
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    if (!renderer->published_samplers[i].live &&
        !renderer->published_samplers[i].pending_retire) {
      free_index = i;
      break;
    }
  }
  if (free_index == UINT32_MAX)
    return false_v;
  VkSampler sampler = VK_NULL_HANDLE;
  VkrGpuSlotHandle slot = {0};
  if (!vkr_vk_create_published_sampler(renderer, description, mip_levels,
                                       &sampler) ||
      !vkr_vk_publish_sampler(renderer, sampler, &slot)) {
    if (sampler)
      vkDestroySampler(vkr_vk_renderer_device(renderer), sampler, NULL);
    return false_v;
  }
  renderer->published_samplers[free_index] = (VkrVulkanPublishedSampler){
      .description = *description,
      .sampler = sampler,
      .slot = slot,
      .mip_levels = mip_levels,
      .reference_count = 1u,
      .live = true_v,
  };
  *out_record_index = free_index;
  return true_v;
}

vkr_internal bool8_t vkr_vk_release_sampler(VkrVulkanRenderer *renderer,
                                            uint32_t record_index,
                                            uint64_t last_use_submit_value) {
  if (record_index >= renderer->config.sampler_capacity)
    return false_v;
  VkrVulkanPublishedSampler *record =
      &renderer->published_samplers[record_index];
  if (!record->live || !record->reference_count)
    return false_v;
  if (record->reference_count > 1u) {
    record->reference_count--;
    record->last_use_submit_value =
        Max(record->last_use_submit_value, last_use_submit_value);
    return true_v;
  }
  last_use_submit_value =
      Max(record->last_use_submit_value, last_use_submit_value);
  if (vkr_gpu_slot_table_retire(renderer->sampler_slots, record->slot,
                                last_use_submit_value) !=
      VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  record->reference_count = 0u;
  record->last_use_submit_value = last_use_submit_value;
  record->live = false_v;
  record->pending_retire = true_v;
  return true_v;
}

vkr_internal void vkr_vk_collect_samplers(VkrVulkanRenderer *renderer,
                                          uint64_t completed) {
  (void)vkr_gpu_slot_table_collect(renderer->sampler_slots, completed, NULL);
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    VkrVulkanPublishedSampler *record = &renderer->published_samplers[i];
    if (!record->pending_retire || record->last_use_submit_value > completed)
      continue;
    vkDestroySampler(vkr_vk_renderer_device(renderer), record->sampler, NULL);
    MemZero(record, sizeof(*record));
  }
}

vkr_internal void vkr_vk_prepare_writable_initialization(
    VkrTextureHandle texture, VkrVulkanImage *image,
    VkrVulkanPendingTextureInitialization *out_initialization) {
  *out_initialization = (VkrVulkanPendingTextureInitialization){
      .texture = texture,
      .writable = true_v,
  };
  image->layout = VK_IMAGE_LAYOUT_GENERAL;
}

VkrVulkanPublishedTexture *vkr_vk_published_texture(VkrVulkanRenderer *renderer,
                                                    VkrTextureHandle handle,
                                                    uint32_t *out_index) {
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.texture_capacity)
    return NULL;
  const uint32_t index = handle.id - 1u;
  VkrVulkanPublishedTexture *texture = &renderer->published_textures[index];
  if (!texture->live || texture->handle.generation != handle.generation)
    return NULL;
  if (out_index)
    *out_index = index;
  return texture;
}

VkrVulkanPublishedTexture *
vkr_vk_texture_publication(VkrVulkanRenderer *renderer,
                           VkrTextureHandle handle) {
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.texture_capacity)
    return NULL;
  VkrVulkanPublishedTexture *active =
      &renderer->published_textures[handle.id - 1u];
  if ((active->live || active->pending_retire) &&
      active->handle.id == handle.id &&
      active->handle.generation == handle.generation)
    return active;
  for (uint32_t i = 0u; i < renderer->config.texture_capacity; ++i) {
    VkrVulkanPublishedTexture *retired = &renderer->retired_textures[i];
    if (retired->pending_retire && retired->handle.id == handle.id &&
        retired->handle.generation == handle.generation)
      return retired;
  }
  return NULL;
}

vkr_internal void
vkr_vk_destroy_texture_storage_views(VkrVulkanRenderer *renderer,
                                     VkrVulkanPublishedTexture *texture) {
  const VkDevice device = vkr_vk_renderer_device(renderer);
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (texture->storage_views[i])
      vkDestroyImageView(device, texture->storage_views[i], NULL);
    texture->storage_views[i] = VK_NULL_HANDLE;
  }
  texture->storage_slot_count = 0u;
}

vkr_internal bool8_t vkr_vk_retire_unreferenced_texture(
    VkrVulkanRenderer *renderer, VkrVulkanPublishedTexture *texture,
    uint64_t completed) {
  if (!texture->pending_retire || texture->material_reference_count != 0u ||
      texture->ibl_reference_count != 0u || texture->initialization_pending ||
      texture->last_use_submit_value > completed)
    return true_v;

  VkrVulkanPublishedSampler *sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  bool8_t storage_can_retire = true_v;
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (vkr_gpu_slot_table_can_retire(renderer->storage_image_slots,
                                      texture->storage_slots[i]) !=
        VKR_GPU_SLOT_STATUS_OK) {
      storage_can_retire = false_v;
      break;
    }
  }
  if (vkr_gpu_slot_table_can_retire(renderer->sampled_image_slots,
                                    texture->sampled_slot) !=
          VKR_GPU_SLOT_STATUS_OK ||
      !storage_can_retire ||
      (sampler->reference_count == 1u &&
       vkr_gpu_slot_table_can_retire(renderer->sampler_slots, sampler->slot) !=
           VKR_GPU_SLOT_STATUS_OK)) {
    log_error("Vulkan texture retirement could not reserve every "
              "descriptor retirement; preserving the complete publication");
    return false_v;
  }

  if (vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                texture->sampled_slot,
                                completed) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                  texture->storage_slots[i],
                                  completed) != VKR_GPU_SLOT_STATUS_OK)
      log_fatal("Vulkan lost a validated storage-image retirement");
  }
  if (!vkr_vk_release_sampler(renderer, texture->sampler_record_index,
                              completed) ||
      vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK) {
    log_error("Vulkan failed to retire a validated texture "
              "publication; preserving the native texture");
    return false_v;
  }
  if (!vkr_vk_retire_allocation(renderer, &texture->image.allocation,
                                completed))
    log_fatal("Vulkan failed to retire completed texture memory");
  vkr_vk_destroy_texture_storage_views(renderer, texture);
  vkr_vk_destroy_image(renderer, &texture->image);
  MemZero(texture, sizeof(*texture));
  return true_v;
}

vkr_internal VkrVulkanPublishedTexture *
vkr_vk_reserve_retired_texture(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0u; i < renderer->config.texture_capacity; ++i) {
    VkrVulkanPublishedTexture *retired = &renderer->retired_textures[i];
    if (!retired->pending_retire)
      return retired;
  }
  return NULL;
}

void vkr_vk_collect_asset_publications(VkrVulkanRenderer *renderer,
                                       uint64_t completed) {
  (void)vkr_gpu_slot_table_collect(renderer->material_slots, completed, NULL);
  vkr_vk_collect_samplers(renderer, completed);
  for (uint32_t i = 0u; i < ArrayCount(renderer->geometry_megabuffer.retired);
       ++i) {
    VkrVulkanRetiredGeometryMegabuffer *retired =
        &renderer->geometry_megabuffer.retired[i];
    if (!retired->occupied || retired->retire_value > completed)
      continue;
    vkr_vk_destroy_buffer(renderer, &retired->indices);
    vkr_vk_destroy_buffer(renderer, &retired->vertices);
    MemZero(retired, sizeof(*retired));
  }
  for (uint32_t i = 0; i < renderer->retired_staging_buffer_capacity; ++i) {
    VkrVulkanRetiredStagingBuffer *retired =
        &renderer->retired_staging_buffers[i];
    if (!retired->occupied || retired->retire_value > completed)
      continue;
    vkr_vk_destroy_buffer(renderer, &retired->buffer);
    renderer->staging_buffer_count--;
    MemZero(retired, sizeof(*retired));
  }
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrVulkanRetiredMaterial *retired = &renderer->retired_materials[i];
    if (!retired->occupied || retired->retire_value > completed)
      continue;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (retired->texture_record_indices[texture_slot] == UINT32_MAX)
        continue;
      VkrVulkanPublishedTexture *texture =
          &renderer->published_textures
               [retired->texture_record_indices[texture_slot]];
      if (texture->material_reference_count > 0u)
        texture->material_reference_count--;
    }
    MemZero(retired, sizeof(*retired));
  }
  for (uint32_t i = 0; i < renderer->config.geometry_capacity; ++i) {
    VkrVulkanPublishedGeometry *geometry = &renderer->published_geometries[i];
    if (!geometry->pending_retire ||
        geometry->last_use_submit_value > completed)
      continue;
    if (geometry->submeshes)
      vkr_allocator_free(renderer->allocator, geometry->submeshes,
                         geometry->submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemZero(geometry, sizeof(*geometry));
  }
  for (uint32_t i = 0; i < renderer->config.geometry_capacity; ++i) {
    VkrVulkanPublishedGeometry *geometry = &renderer->retired_geometries[i];
    if (!geometry->pending_retire ||
        geometry->last_use_submit_value > completed)
      continue;
    if (geometry->submeshes)
      vkr_allocator_free(renderer->allocator, geometry->submeshes,
                         geometry->submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemZero(geometry, sizeof(*geometry));
  }
  for (uint32_t i = 0; i < renderer->config.texture_capacity; ++i)
    vkr_vk_retire_unreferenced_texture(
        renderer, &renderer->published_textures[i], completed);
  for (uint32_t i = 0; i < renderer->config.texture_capacity; ++i)
    vkr_vk_retire_unreferenced_texture(renderer, &renderer->retired_textures[i],
                                       completed);
  vkr_vk_collect_samplers(renderer, completed);
}

vkr_internal VkrVulkanRetiredMaterial *
vkr_vk_reserve_material_retirement(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    if (!renderer->retired_materials[i].occupied)
      return &renderer->retired_materials[i];
  }
  return NULL;
}

vkr_internal bool8_t vkr_vk_megabuffer_capacity(uint64_t current,
                                                uint64_t initial,
                                                uint64_t required,
                                                uint64_t *out_capacity) {
  if (!out_capacity || !initial || !required)
    return false_v;
  uint64_t capacity = current ? current : initial;
  while (capacity < required) {
    if (capacity > UINT64_MAX / 2u)
      return false_v;
    capacity *= 2u;
  }
  *out_capacity = capacity;
  return true_v;
}

vkr_internal bool8_t vkr_vk_ensure_geometry_megabuffer(
    VkrVulkanRenderer *renderer, uint64_t vertex_end, uint64_t index_end) {
  VkrVulkanGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  if (mega->live && vertex_end <= mega->vertices.size &&
      index_end <= mega->indices.size)
    return true_v;
  if (mega->copy_pending || renderer->pending_buffer_initialization_count) {
    return false_v;
  }
  bool8_t retirement_available = mega->live ? false_v : true_v;
  for (uint32_t i = 0u; mega->live && i < ArrayCount(mega->retired); ++i)
    retirement_available |= !mega->retired[i].occupied;
  if (!retirement_available) {
    return false_v;
  }
  uint64_t vertex_capacity = 0u;
  uint64_t index_capacity = 0u;
  if (!vkr_vk_megabuffer_capacity(mega->live ? mega->vertices.size : 0u,
                                  MB(128), vertex_end, &vertex_capacity) ||
      !vkr_vk_megabuffer_capacity(mega->live ? mega->indices.size : 0u, MB(64),
                                  index_end, &index_capacity)) {
    return false_v;
  }
  const VkBufferUsageFlags vertex_usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  const VkBufferUsageFlags index_usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  VkrVulkanBuffer vertices = {0};
  VkrVulkanBuffer indices = {0};
  if (!vkr_vk_create_buffer(renderer, VKR_VULKAN_MEMORY_CLASS_DEVICE,
                            vertex_capacity, vertex_usage, &vertices) ||
      !vkr_vk_create_buffer(renderer, VKR_VULKAN_MEMORY_CLASS_DEVICE,
                            index_capacity, index_usage, &indices)) {
    vkr_vk_destroy_buffer(renderer, &indices);
    vkr_vk_destroy_buffer(renderer, &vertices);
    return false_v;
  }
  if (mega->live) {
    mega->copy_source_vertices = mega->vertices;
    mega->copy_source_indices = mega->indices;
    mega->copy_vertex_size = mega->vertex_cursor;
    mega->copy_index_size = mega->index_cursor;
    mega->copy_pending = true_v;
    mega->generation_replacements++;
  }
  mega->vertices = vertices;
  mega->indices = indices;
  mega->generation =
      mega->generation == UINT32_MAX ? 1u : mega->generation + 1u;
  if (mega->generation == 0u)
    mega->generation = 1u;
  mega->live = true_v;
  for (uint32_t i = 0u; i < renderer->config.geometry_capacity; ++i) {
    VkrVulkanPublishedGeometry *geometry = &renderer->published_geometries[i];
    if (!geometry->live)
      continue;
    geometry->vertices = mega->vertices;
    geometry->indices = mega->indices;
    geometry->gpu_row.vertex_address = mega->vertices.address;
    geometry->gpu_row.index_address = mega->indices.address;
    geometry->gpu_row.publication_generation = mega->generation;
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_prepare_published_upload(
    VkrVulkanRenderer *renderer, const void *data, uint64_t size,
    const VkrVulkanBuffer *destination, uint64_t destination_offset,
    VkPipelineStageFlags2 destination_stage, VkAccessFlags2 destination_access,
    uint32_t geometry_record_index,
    VkrVulkanPendingBufferInitialization *out_initialization) {
  if (!data || !size || !destination || !destination->handle ||
      destination_offset > destination->size ||
      size > destination->size - destination_offset || !out_initialization)
    return false_v;
  uint8_t *upload_data = vkr_vk_publication_source_alloc(renderer, size);
  if (!upload_data) {
    log_error(
        "Vulkan failed to retain %llu geometry bytes "
        "(free=%llu, committed=%llu, reserve=%llu)",
        (unsigned long long)size,
        (unsigned long long)vkr_dmemory_get_free_space(
            &renderer->publication_staging_memory),
        (unsigned long long)renderer->publication_staging_memory.total_size,
        (unsigned long long)renderer->publication_staging_memory.reserve_size);
    return false_v;
  }
  MemCopy(upload_data, data, size);
  *out_initialization = (VkrVulkanPendingBufferInitialization){
      .destination = destination->handle,
      .upload_data = upload_data,
      .size = size,
      .destination_offset = destination_offset,
      .destination_stage = destination_stage,
      .destination_access = destination_access,
      .geometry_record_index = geometry_record_index,
  };
  return true_v;
}

vkr_internal bool8_t vkr_vk_asset_publish_geometry_internal(
    void *state, VkrGeometryHandle handle, const VkrGeometryConfig *geometry,
    const VkrMeshLoaderSubmeshRange *submeshes, uint32_t submesh_count) {
  VkrVulkanRenderer *renderer = state;
  if (!renderer || !geometry || handle.id == 0u ||
      handle.id > renderer->config.geometry_capacity || !geometry->vertices ||
      !geometry->indices || !geometry->vertex_count || !geometry->index_count ||
      !submeshes || !submesh_count ||
      renderer->pending_buffer_initialization_count >
          renderer->pending_buffer_initialization_capacity - 2u ||
      renderer->staging_buffer_count >
          renderer->retired_staging_buffer_capacity - 2u ||
      (geometry->vertex_size != sizeof(VkrVertex3d) &&
       geometry->vertex_size != sizeof(VkrVertex2d)) ||
      (geometry->index_size != sizeof(uint16_t) &&
       geometry->index_size != sizeof(uint32_t)))
    return false_v;
  for (uint32_t i = 0; i < submesh_count; ++i) {
    if (!submeshes[i].index_count ||
        submeshes[i].first_index > geometry->index_count ||
        submeshes[i].index_count >
            geometry->index_count - submeshes[i].first_index)
      return false_v;
  }
  VkrVulkanPublishedGeometry *record =
      &renderer->published_geometries[handle.id - 1u];
  const VkIndexType index_type = VK_INDEX_TYPE_UINT32;
  const uint64_t submeshes_size =
      (uint64_t)submesh_count * sizeof(VkrVulkanSubmeshRange);
  if (record->live) {
    if (record->handle.generation != handle.generation ||
        record->vertex_count != geometry->vertex_count ||
        record->index_count != geometry->index_count ||
        record->index_type != index_type) {
      log_error("Vulkan geometry %u:%u conflicts with %u:%u", handle.id,
                handle.generation, record->handle.id,
                record->handle.generation);
      return false_v;
    }
    if (record->submeshes_size != submeshes_size) {
      VkrVulkanSubmeshRange *replacement =
          vkr_allocator_alloc(renderer->allocator, submeshes_size,
                              VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (!replacement)
        return false_v;
      vkr_allocator_free(renderer->allocator, record->submeshes,
                         record->submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      record->submeshes = replacement;
      record->submeshes_size = submeshes_size;
    }
    for (uint32_t i = 0u; i < submesh_count; ++i) {
      record->submeshes[i] = (VkrVulkanSubmeshRange){
          .first_index = submeshes[i].first_index,
          .index_count = submeshes[i].index_count,
          .vertex_offset = submeshes[i].vertex_offset,
      };
    }
    record->submesh_count = submesh_count;
    vkr_vk_advance_candidate_publication_generation(renderer);
    return true_v;
  }
  if (record->pending_retire) {
    log_error("Vulkan geometry %u:%u conflicts with %u:%u "
              "(live=%u, pending_retire=%u)",
              handle.id, handle.generation, record->handle.id,
              record->handle.generation, record->live, record->pending_retire);
    return false_v;
  }
  const uint64_t vertex_size =
      (uint64_t)sizeof(VkrVertex3d) * geometry->vertex_count;
  const uint64_t index_size =
      (uint64_t)sizeof(uint32_t) * geometry->index_count;
  VkrVulkanGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  const uint64_t vertex_offset = vkr_vk_align_up(mega->vertex_cursor, 16u);
  const uint64_t index_offset =
      vkr_vk_align_up(mega->index_cursor, sizeof(uint32_t));
  if (vertex_offset > UINT64_MAX - vertex_size ||
      index_offset > UINT64_MAX - index_size ||
      vertex_offset / sizeof(VkrVertex3d) > UINT32_MAX ||
      index_offset / sizeof(uint32_t) > UINT32_MAX ||
      !vkr_vk_ensure_geometry_megabuffer(renderer, vertex_offset + vertex_size,
                                         index_offset + index_size)) {
    mega->rejected_publications++;
    return false_v;
  }
  VkrVulkanPublishedGeometry pending = {
      .handle = handle,
      .vertices = mega->vertices,
      .indices = mega->indices,
      .vertex_count = geometry->vertex_count,
      .index_count = geometry->index_count,
      .index_type = index_type,
      .submeshes_size = submeshes_size,
      .submesh_count = submesh_count,
  };
  pending.submeshes =
      vkr_allocator_alloc(renderer->allocator, pending.submeshes_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!pending.submeshes)
    return false_v;
  for (uint32_t i = 0; i < submesh_count; ++i) {
    pending.submeshes[i] = (VkrVulkanSubmeshRange){
        .first_index = submeshes[i].first_index,
        .index_count = submeshes[i].index_count,
        .vertex_offset = submeshes[i].vertex_offset,
    };
  }
  VkrVulkanPendingBufferInitialization initializations[2] = {0};
  const VkrVertex3d *vertices = geometry->vertices;
  VkrVertex3d *converted_vertices = NULL;
  if (geometry->vertex_size == sizeof(VkrVertex2d)) {
    converted_vertices = vkr_allocator_alloc(renderer->allocator, vertex_size,
                                             VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!converted_vertices) {
      vkr_allocator_free(renderer->allocator, pending.submeshes,
                         pending.submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      return false_v;
    }
    const VkrVertex2d *source = geometry->vertices;
    for (uint32_t i = 0; i < geometry->vertex_count; ++i) {
      converted_vertices[i] = (VkrVertex3d){
          .position = {source[i].position.x, source[i].position.y, 0.0f},
          .normal = {0.0f, 0.0f, 1.0f},
          .texcoord = source[i].texcoord,
          .colour = {1.0f, 1.0f, 1.0f, 1.0f},
          .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
      };
    }
    vertices = converted_vertices;
  }
  const uint32_t *indices = geometry->indices;
  uint32_t *converted_indices = NULL;
  if (geometry->index_size == sizeof(uint16_t)) {
    converted_indices = vkr_allocator_alloc(renderer->allocator, index_size,
                                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!converted_indices) {
      if (converted_vertices)
        vkr_allocator_free(renderer->allocator, converted_vertices, vertex_size,
                           VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      vkr_allocator_free(renderer->allocator, pending.submeshes,
                         pending.submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      return false_v;
    }
    const uint16_t *source = geometry->indices;
    for (uint32_t i = 0u; i < geometry->index_count; ++i)
      converted_indices[i] = source[i];
    indices = converted_indices;
  }
  const bool8_t created =
      vkr_vk_prepare_published_upload(
          renderer, vertices, vertex_size, &mega->vertices, vertex_offset,
          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_STORAGE_READ_BIT, handle.id - 1u,
          &initializations[0]) &&
      vkr_vk_prepare_published_upload(
          renderer, indices, index_size, &mega->indices, index_offset,
          VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
          handle.id - 1u, &initializations[1]);
  if (converted_indices)
    vkr_allocator_free(renderer->allocator, converted_indices, index_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (converted_vertices)
    vkr_allocator_free(renderer->allocator, converted_vertices, vertex_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!created) {
    vkr_vk_release_buffer_initialization(renderer, &initializations[1]);
    vkr_vk_release_buffer_initialization(renderer, &initializations[0]);
    vkr_allocator_free(renderer->allocator, pending.submeshes,
                       pending.submeshes_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }
  if (renderer->pending_buffer_initialization_count >
          renderer->pending_buffer_initialization_capacity - 2u ||
      renderer->staging_buffer_count >
          renderer->retired_staging_buffer_capacity - 2u) {
    vkr_vk_release_buffer_initialization(renderer, &initializations[1]);
    vkr_vk_release_buffer_initialization(renderer, &initializations[0]);
    vkr_allocator_free(renderer->allocator, pending.submeshes,
                       pending.submeshes_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }
  pending.pending_initialization_count = 2u;
  pending.gpu_row = (VkrGpuGeometryRow){
      .vertex_address = pending.vertices.address,
      .index_address = pending.indices.address,
      .first_vertex = (uint32_t)(vertex_offset / sizeof(VkrVertex3d)),
      .first_index = (uint32_t)(index_offset / sizeof(uint32_t)),
      .vertex_stride = sizeof(VkrVertex3d),
      .vertex_layout = VKR_GPU_VERTEX_LAYOUT_3D,
      .publication_generation = mega->generation,
      .flags = 1u,
  };
  pending.live = true_v;
  *record = pending;
  mega->vertex_cursor = vertex_offset + vertex_size;
  mega->index_cursor = index_offset + index_size;
  mega->vertex_live_bytes += vertex_size;
  mega->index_live_bytes += index_size;
  mega->vertex_high_water = Max(mega->vertex_high_water, mega->vertex_cursor);
  mega->index_high_water = Max(mega->index_high_water, mega->index_cursor);
  renderer->pending_buffer_initializations
      [renderer->pending_buffer_initialization_count++] = initializations[0];
  renderer->pending_buffer_initializations
      [renderer->pending_buffer_initialization_count++] = initializations[1];
  vkr_vk_advance_candidate_publication_generation(renderer);
  return true_v;
}

vkr_internal bool8_t vkr_vk_asset_publish_geometry(
    void *state, VkrGeometryHandle handle, const VkrGeometryConfig *geometry) {
  if (!geometry)
    return false_v;
  const VkrMeshLoaderSubmeshRange submesh = {
      .index_count = geometry->index_count,
  };
  return vkr_vk_asset_publish_geometry_internal(state, handle, geometry,
                                                &submesh, 1u);
}

vkr_internal bool8_t vkr_vk_publish_writable_storage_views(
    VkrVulkanRenderer *renderer, VkrVulkanPublishedTexture *texture) {
  if (texture->image.mip_levels > VKR_VULKAN_TEXTURE_MIP_MAX)
    return false_v;
  VkDevice device = vkr_vk_renderer_device(renderer);
  for (uint32_t mip = 0u; mip < texture->image.mip_levels; ++mip) {
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = texture->image.format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = mip,
                .levelCount = 1u,
                .baseArrayLayer = 0u,
                .layerCount = texture->image.array_layers,
            },
    };
    if (vkCreateImageView(device, &view_info, NULL,
                          &texture->storage_views[mip]) != VK_SUCCESS)
      return false_v;
    texture->storage_slot_count++;
    if (!vkr_vk_publish_storage_view(renderer, texture->storage_views[mip],
                                     &texture->storage_slots[mip]))
      return false_v;
  }
  return true_v;
}

vkr_internal void
vkr_vk_discard_writable_storage_views(VkrVulkanRenderer *renderer,
                                      VkrVulkanPublishedTexture *texture) {
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (texture->storage_slots[i].generation) {
      (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                      texture->storage_slots[i],
                                      renderer->completed_value);
    }
  }
  (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots,
                                   renderer->completed_value, NULL);
  vkr_vk_destroy_texture_storage_views(renderer, texture);
}

vkr_internal bool8_t vkr_vk_asset_publish_writable_texture(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  VkrVulkanRenderer *renderer = state;
  if (!renderer || !description || handle.id == 0u ||
      handle.id > renderer->config.texture_capacity ||
      renderer->pending_texture_initialization_count >=
          renderer->config.texture_capacity ||
      description->id != handle.id ||
      description->generation != handle.generation || !description->width ||
      !description->height ||
      (description->sample_count != 0u &&
       description->sample_count != VKR_SAMPLE_COUNT_1) ||
      (description->type != VKR_TEXTURE_TYPE_2D &&
       description->type != VKR_TEXTURE_TYPE_CUBE_MAP) ||
      (description->type == VKR_TEXTURE_TYPE_CUBE_MAP &&
       description->width != description->height)) {
    log_error("Vulkan rejected writable texture metadata "
              "(handle=%u:%u, size=%ux%u, type=%u, format=%u, pending=%u)",
              handle.id, handle.generation,
              description ? description->width : 0u,
              description ? description->height : 0u,
              description ? description->type : 0u,
              description ? description->format : 0u,
              renderer ? renderer->pending_texture_initialization_count : 0u);
    return false_v;
  }
  VkrVulkanPublishedTexture *record =
      &renderer->published_textures[handle.id - 1u];
  if (record->live || record->pending_retire) {
    log_error("Vulkan writable texture %u:%u collides with a %s "
              "publication %u:%u",
              handle.id, handle.generation, record->live ? "live" : "retiring",
              record->handle.id, record->handle.generation);
    return false_v;
  }
  const VkFormat format = vkr_vk_texture_format(description->format);
  if (format == VK_FORMAT_UNDEFINED) {
    log_error("Vulkan writable texture %u:%u has unmapped format %u", handle.id,
              handle.generation, description->format);
    return false_v;
  }
  VkFormatProperties properties = {0};
  vkGetPhysicalDeviceFormatProperties(
      vkr_vulkan_device_physical(renderer->device), format, &properties);
  const VkFormatFeatureFlags required =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  if ((properties.optimalTilingFeatures & required) != required) {
    log_error("Vulkan writable texture %u:%u format %u lacks "
              "sampled/storage features",
              handle.id, handle.generation, format);
    return false_v;
  }
  const bool8_t cube = description->type == VKR_TEXTURE_TYPE_CUBE_MAP;
  const uint32_t mip_levels =
      description->mip_filter == VKR_MIP_FILTER_NONE
          ? 1u
          : vkr_vk_mip_count(description->width, description->height);
  VkrVulkanPublishedTexture pending = {.handle = handle};
  VkrVulkanPendingTextureInitialization initialization = {0};
  const bool8_t image_created = vkr_vk_create_image_ex(
      renderer, description->width, description->height, mip_levels,
      cube ? 6u : 1u, format, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
      cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      &pending.image);
  const bool8_t sampler_acquired =
      image_created && vkr_vk_acquire_sampler(renderer, description, mip_levels,
                                              &pending.sampler_record_index);
  const bool8_t sampled_published =
      sampler_acquired && vkr_vk_publish_sampled_view(
                              renderer, pending.image.view,
                              VK_IMAGE_LAYOUT_GENERAL, &pending.sampled_slot);
  const bool8_t storage_published =
      sampled_published &&
      vkr_vk_publish_writable_storage_views(renderer, &pending);
  if (storage_published)
    vkr_vk_prepare_writable_initialization(handle, &pending.image,
                                           &initialization);
  const bool8_t initialization_queued =
      storage_published &&
      vkr_vk_enqueue_texture_initialization(renderer, &initialization);
  if (!initialization_queued) {
    log_error("Vulkan writable texture %u:%u failed at %s "
              "(%ux%u, mips=%u, layers=%u, format=%u)",
              handle.id, handle.generation,
              !image_created       ? "image creation"
              : !sampler_acquired  ? "sampler publication"
              : !sampled_published ? "sampled descriptor publication"
              : !storage_published ? "storage descriptor publication"
                                   : "initialization queue",
              description->width, description->height, mip_levels,
              cube ? 6u : 1u, format);
    if (pending.sampled_slot.generation) {
      (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                      pending.sampled_slot,
                                      renderer->completed_value);
      (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots,
                                       renderer->completed_value, NULL);
    }
    vkr_vk_discard_writable_storage_views(renderer, &pending);
    if (sampler_acquired) {
      (void)vkr_vk_release_sampler(renderer, pending.sampler_record_index,
                                   renderer->completed_value);
      vkr_vk_collect_samplers(renderer, renderer->completed_value);
    }
    vkr_vk_destroy_image(renderer, &pending.image);
    return false_v;
  }
  pending.initialization_pending = true_v;
  pending.live = true_v;
  *record = pending;
  return true_v;
}

vkr_internal void vkr_vk_material_row_set_sampler(VkrVulkanMaterialGpuRow *row,
                                                  uint32_t texture_slot,
                                                  uint32_t sampler_index) {
  switch (texture_slot) {
  case 0u:
    row->base_color_sampler = sampler_index;
    break;
  case 1u:
    row->normal_sampler = sampler_index;
    break;
  case 2u:
    row->orm_sampler = sampler_index;
    break;
  default:
    row->emissive_sampler = sampler_index;
    break;
  }
}

vkr_internal bool8_t
vkr_vk_asset_update_texture_sampler(void *state, VkrTextureHandle handle,
                                    const VkrTextureDescription *description) {
  VkrVulkanRenderer *renderer = state;
  uint32_t texture_record_index = 0u;
  VkrVulkanPublishedTexture *texture =
      vkr_vk_published_texture(renderer, handle, &texture_record_index);
  if (!texture || !description || description->id != handle.id ||
      description->generation != handle.generation)
    return false_v;
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  vkr_vk_collect_asset_publications(renderer, completed);
  VkrVulkanPublishedSampler *old_sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  if (vkr_vk_sampler_description_equal(&old_sampler->description,
                                       old_sampler->mip_levels, description,
                                       texture->image.mip_levels))
    return true_v;

  uint32_t dependent_material_count = 0u;
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    const VkrVulkanPublishedMaterial *material =
        &renderer->published_materials[i];
    if (!material->live)
      continue;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (material->texture_record_indices[texture_slot] ==
          texture_record_index) {
        dependent_material_count++;
        break;
      }
    }
  }
  VkrGpuSlotTableMetrics material_metrics = {0};
  vkr_gpu_slot_table_get_metrics(renderer->material_slots, &material_metrics);
  if (material_metrics.slots_live + material_metrics.slots_retired +
              dependent_material_count >
          material_metrics.slots_capacity ||
      material_metrics.slots_retired + dependent_material_count >
          material_metrics.slots_capacity ||
      (old_sampler->reference_count == 1u &&
       vkr_gpu_slot_table_can_retire(renderer->sampler_slots,
                                     old_sampler->slot) !=
           VKR_GPU_SLOT_STATUS_OK))
    return false_v;

  uint32_t replacement_sampler_index = UINT32_MAX;
  if (!vkr_vk_acquire_sampler(renderer, description, texture->image.mip_levels,
                              &replacement_sampler_index))
    return false_v;
  const uint32_t replacement_slot =
      renderer->published_samplers[replacement_sampler_index].slot.index;
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrVulkanPublishedMaterial *material = &renderer->published_materials[i];
    if (!material->live)
      continue;
    VkrVulkanMaterialGpuRow replacement_row = material->row;
    bool8_t dependent = false_v;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (material->texture_record_indices[texture_slot] !=
          texture_record_index)
        continue;
      vkr_vk_material_row_set_sampler(&replacement_row, texture_slot,
                                      replacement_slot);
      dependent = true_v;
    }
    if (!dependent)
      continue;
    VkrGpuSlotHandle replacement_material_slot = {0};
    if (!vkr_vk_replace_material_gpu_row(
            renderer, material->slot, &replacement_row, renderer->submit_value,
            &replacement_material_slot)) {
      log_error("Vulkan failed a preflighted dependent material "
                "sampler republication");
      renderer->terminal_failure = true_v;
      return false_v;
    }
    material->slot = replacement_material_slot;
    material->row = replacement_row;
  }
  const uint32_t old_sampler_index = texture->sampler_record_index;
  texture->sampler_record_index = replacement_sampler_index;
  if (!vkr_vk_release_sampler(renderer, old_sampler_index,
                              renderer->submit_value)) {
    log_error("Vulkan failed a preflighted sampler retirement");
    renderer->terminal_failure = true_v;
    return false_v;
  }
  vkr_vk_collect_samplers(renderer, completed);
  return true_v;
}

vkr_internal bool8_t vkr_vk_asset_publish_loaded_mesh(
    void *state, VkrGeometryHandle handle, const VkrMeshLoaderResult *mesh) {
  if (!mesh || !mesh->has_mesh_buffer || !mesh->submeshes.data ||
      !mesh->submeshes.length ||
      mesh->mesh_buffer.vertex_size != sizeof(VkrVertex3d) ||
      (mesh->mesh_buffer.index_size != sizeof(uint16_t) &&
       mesh->mesh_buffer.index_size != sizeof(uint32_t)))
    return false_v;
  for (uint64_t i = 0; i < mesh->submeshes.length; ++i) {
    const VkrMeshLoaderSubmeshRange *range = &mesh->submeshes.data[i];
    if (!range->index_count ||
        range->first_index > mesh->mesh_buffer.index_count ||
        range->index_count > mesh->mesh_buffer.index_count - range->first_index)
      return false_v;
  }
  const VkrGeometryConfig geometry = {
      .vertex_size = mesh->mesh_buffer.vertex_size,
      .vertex_count = mesh->mesh_buffer.vertex_count,
      .vertices = mesh->mesh_buffer.vertices,
      .index_size = mesh->mesh_buffer.index_size,
      .index_count = mesh->mesh_buffer.index_count,
      .indices = mesh->mesh_buffer.indices,
  };
  return vkr_vk_asset_publish_geometry_internal(
      state, handle, &geometry, mesh->submeshes.data,
      (uint32_t)mesh->submeshes.length);
}

vkr_internal VkrVulkanPublishedGeometry *
vkr_vk_reserve_retired_geometry(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0u; i < renderer->config.geometry_capacity; ++i) {
    VkrVulkanPublishedGeometry *retired = &renderer->retired_geometries[i];
    if (!retired->pending_retire)
      return retired;
  }
  return NULL;
}

bool8_t vkr_vk_asset_unpublish_geometry(void *state, VkrGeometryHandle handle) {
  VkrVulkanRenderer *renderer = state;
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.geometry_capacity)
    return false_v;
  VkrVulkanPublishedGeometry *record =
      &renderer->published_geometries[handle.id - 1u];
  if (!record->live || record->handle.generation != handle.generation)
    return false_v;
  if (record->pending_initialization_count)
    vkr_vk_discard_geometry_initializations(renderer, handle.id - 1u);
  VkrVulkanGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  const uint64_t vertex_bytes =
      (uint64_t)record->vertex_count * sizeof(VkrVertex3d);
  const uint64_t index_bytes = (uint64_t)record->index_count * sizeof(uint32_t);
  mega->vertex_live_bytes -= Min(mega->vertex_live_bytes, vertex_bytes);
  mega->index_live_bytes -= Min(mega->index_live_bytes, index_bytes);
  record->live = false_v;
  record->pending_retire = true_v;
  record->last_use_submit_value =
      Max(record->last_use_submit_value, renderer->submit_value);
  vkr_vk_collect_asset_publications(renderer,
                                    vkr_vk_refresh_completed(renderer));
  if (!record->pending_retire) {
    vkr_vk_advance_candidate_publication_generation(renderer);
    return true_v;
  }
  VkrVulkanPublishedGeometry *retired =
      vkr_vk_reserve_retired_geometry(renderer);
  if (!retired) {
    log_error("Vulkan retired-geometry capacity exhausted");
    return false_v;
  }
  *retired = *record;
  MemZero(record, sizeof(*record));
  vkr_vk_advance_candidate_publication_generation(renderer);
  return true_v;
}

vkr_internal bool8_t
vkr_vk_asset_publish_texture(void *state, VkrTextureHandle handle,
                             const VkrTexturePreparedLoad *prepared) {
  VkrVulkanRenderer *renderer = state;
  if (!renderer || !prepared || handle.id == 0u ||
      handle.id > renderer->config.texture_capacity ||
      renderer->pending_texture_initialization_count >=
          renderer->config.texture_capacity ||
      renderer->staging_buffer_count >=
          renderer->retired_staging_buffer_capacity)
    return false_v;
  VkrVulkanPublishedTexture *record =
      &renderer->published_textures[handle.id - 1u];
  if (record->live || record->pending_retire) {
    log_error("Vulkan texture %u:%u is already published", handle.id,
              handle.generation);
    return false_v;
  }
  VkrVulkanPublishedTexture pending = {.handle = handle};
  VkrVulkanPendingTextureInitialization initialization = {0};
  const bool8_t image_uploaded = vkr_vk_upload_prepared_texture(
      renderer, prepared, handle, &pending.image, &initialization);
  const bool8_t sampler_acquired =
      image_uploaded && vkr_vk_acquire_sampler(renderer, &prepared->description,
                                               prepared->upload_mip_levels,
                                               &pending.sampler_record_index);
  const bool8_t sampled_published =
      sampler_acquired &&
      vkr_vk_publish_sampled_view(renderer, pending.image.view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  &pending.sampled_slot);
  const bool8_t initialization_queued =
      sampled_published &&
      vkr_vk_enqueue_texture_initialization(renderer, &initialization);
  if (!initialization_queued) {
    log_error("Vulkan texture %u:%u publication failed at %s", handle.id,
              handle.generation,
              !image_uploaded      ? "image upload"
              : !sampler_acquired  ? "sampler publication"
              : !sampled_published ? "sampled descriptor"
                                   : "initialization queue");
    if (pending.sampled_slot.generation) {
      (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                      pending.sampled_slot,
                                      renderer->completed_value);
      (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots,
                                       renderer->completed_value, NULL);
    }
    if (sampler_acquired) {
      (void)vkr_vk_release_sampler(renderer, pending.sampler_record_index,
                                   renderer->completed_value);
      vkr_vk_collect_samplers(renderer, renderer->completed_value);
    }
    if (image_uploaded)
      vkr_vk_release_texture_initialization(renderer, &initialization);
    vkr_vk_destroy_image(renderer, &pending.image);
    return false_v;
  }
  pending.initialization_pending = true_v;
  pending.live = true_v;
  *record = pending;
  return true_v;
}

bool8_t vkr_vk_asset_unpublish_texture(void *state, VkrTextureHandle handle) {
  VkrVulkanRenderer *renderer = state;
  VkrVulkanPublishedTexture *texture =
      vkr_vk_published_texture(renderer, handle, NULL);
  if (!texture)
    return false_v;
  if (texture->ibl_reference_count) {
    if (!texture->material_reference_count) {
      VkrVulkanPublishedTexture *retired =
          vkr_vk_reserve_retired_texture(renderer);
      if (!retired) {
        log_error("Vulkan retired-texture capacity exhausted");
        return false_v;
      }
      texture->live = false_v;
      texture->pending_retire = true_v;
      texture->last_use_submit_value =
          Max(texture->last_use_submit_value, renderer->submit_value);
      *retired = *texture;
      MemZero(texture, sizeof(*texture));
      vkr_vk_collect_asset_publications(renderer,
                                        vkr_vk_refresh_completed(renderer));
      return true_v;
    }
    texture->unpublish_requested = true_v;
    return true_v;
  }
  if (texture->initialization_pending)
    vkr_vk_cancel_texture_initialization(renderer, handle);
  texture->live = false_v;
  texture->pending_retire = true_v;
  texture->last_use_submit_value = renderer->submit_value;
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  vkr_vk_collect_asset_publications(renderer, completed);
  if (!texture->pending_retire)
    return true_v;
  if (texture->material_reference_count)
    return true_v;
  VkrVulkanPublishedTexture *retired = vkr_vk_reserve_retired_texture(renderer);
  if (!retired) {
    log_error("Vulkan retired-texture capacity exhausted");
    return false_v;
  }
  *retired = *texture;
  MemZero(texture, sizeof(*texture));
  return true_v;
}

vkr_internal bool8_t vkr_vk_asset_publish_material(
    void *state, VkrMaterialHandle handle, const VkrMaterial *material) {
  VkrVulkanRenderer *renderer = state;
  if (!renderer || !material || handle.id == 0u ||
      handle.id > renderer->config.material_record_capacity ||
      material->id != handle.id || material->generation != handle.generation)
    return false_v;
  VkrVulkanPublishedMaterial *record =
      &renderer->published_materials[handle.id - 1u];
  if (record->live && record->handle.generation != handle.generation)
    return false_v;
  VkrVulkanRetiredMaterial *retirement =
      record->live ? vkr_vk_reserve_material_retirement(renderer) : NULL;
  if (record->live && !retirement)
    return false_v;

  vkr_local_persist const VkrTextureSlot row_slots[4] = {
      VKR_TEXTURE_SLOT_DIFFUSE,
      VKR_TEXTURE_SLOT_NORMAL,
      VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS,
      VKR_TEXTURE_SLOT_EMISSION,
  };
  uint32_t texture_indices[4] = {0};
  uint32_t sampler_indices[4] = {0};
  uint32_t texture_record_indices[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                        UINT32_MAX};
  uint32_t material_flags = 0u;
  for (uint32_t i = 0; i < ArrayCount(row_slots); ++i) {
    const VkrMaterialTexture *source = &material->textures[row_slots[i]];
    if (!source->enabled)
      continue;
    uint32_t record_index = 0u;
    VkrVulkanPublishedTexture *texture =
        vkr_vk_published_texture(renderer, source->handle, &record_index);
    if (!texture)
      return false_v;
    texture_indices[i] = texture->sampled_slot.index;
    sampler_indices[i] =
        renderer->published_samplers[texture->sampler_record_index].slot.index;
    texture_record_indices[i] = record_index;
  }
  if (material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled)
    material_flags |= VKR_VULKAN_MATERIAL_TEXTURE_NORMAL;
  if (material->textures[VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS].enabled)
    material_flags |= VKR_VULKAN_MATERIAL_TEXTURE_ORM;
  if (material->textures[VKR_TEXTURE_SLOT_EMISSION].enabled)
    material_flags |= VKR_VULKAN_MATERIAL_TEXTURE_EMISSIVE;
  const Vec4 tint = material->material_type == VKR_MATERIAL_TYPE_PBR
                        ? material->pbr.base_color
                        : material->phong.diffuse_color;
  VkrPbrProperties pbr = material->pbr;
  if (material->material_type != VKR_MATERIAL_TYPE_PBR) {
    pbr.base_color = material->phong.diffuse_color;
    pbr.emissive_factor = material->phong.emission_color;
  }
  const VkrPacketMaterialConstants material_constants =
      vkr_packet_derive_material_constants(&pbr, material->alpha_cutoff,
                                           material->alpha_mode);
  const VkrVulkanMaterialGpuRow row = {
      .tint = {tint.x, tint.y, tint.z, tint.w},
      .base_color_texture = texture_indices[0],
      .normal_texture = texture_indices[1],
      .orm_texture = texture_indices[2],
      .emissive_texture = texture_indices[3],
      .base_color_sampler = sampler_indices[0],
      .normal_sampler = sampler_indices[1],
      .orm_sampler = sampler_indices[2],
      .emissive_sampler = sampler_indices[3],
      .material_id = handle.id,
      .flags = material_flags,
      .alpha_mode = material_constants.alpha_mode,
      .material_emissive = material_constants.emissive,
      .material_dielectric_specular = material_constants.dielectric_specular,
      .material_surface = material_constants.surface,
      .material_alpha = material_constants.alpha,
      .material_attenuation_color = material_constants.attenuation_color,
  };
  VkrGpuSlotHandle new_slot = {0};
  const bool8_t row_published =
      record->live
          ? vkr_vk_replace_material_gpu_row(renderer, record->slot, &row,
                                            renderer->submit_value, &new_slot)
          : vkr_vk_publish_material_gpu_row(renderer, &row, &new_slot);
  if (!row_published)
    return false_v;
  for (uint32_t i = 0; i < 4u; ++i) {
    if (texture_record_indices[i] != UINT32_MAX)
      renderer->published_textures[texture_record_indices[i]]
          .material_reference_count++;
  }
  if (record->live) {
    *retirement = (VkrVulkanRetiredMaterial){
        .retire_value = renderer->submit_value,
        .occupied = true_v,
    };
    MemCopy(retirement->texture_record_indices, record->texture_record_indices,
            sizeof(retirement->texture_record_indices));
  }
  *record = (VkrVulkanPublishedMaterial){
      .handle = handle,
      .slot = new_slot,
      .row = row,
      .pbr = material->pbr,
      .alpha_cutoff = material->alpha_cutoff,
      .alpha_mode = material->alpha_mode,
      .double_sided = material->double_sided,
      .live = true_v,
  };
  MemCopy(record->texture_record_indices, texture_record_indices,
          sizeof(record->texture_record_indices));
  record->pending_texture_count =
      vkr_vk_material_pending_texture_count(renderer, record);
  vkr_vk_advance_candidate_publication_generation(renderer);
  return true_v;
}

bool8_t vkr_vk_asset_unpublish_material(void *state, VkrMaterialHandle handle) {
  VkrVulkanRenderer *renderer = state;
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.material_record_capacity)
    return false_v;
  VkrVulkanPublishedMaterial *record =
      &renderer->published_materials[handle.id - 1u];
  if (!record->live || record->handle.generation != handle.generation)
    return false_v;
  VkrVulkanRetiredMaterial *retirement =
      vkr_vk_reserve_material_retirement(renderer);
  if (!retirement || vkr_gpu_slot_table_retire(
                         renderer->material_slots, record->slot,
                         renderer->submit_value) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  *retirement = (VkrVulkanRetiredMaterial){
      .retire_value = renderer->submit_value,
      .occupied = true_v,
  };
  MemCopy(retirement->texture_record_indices, record->texture_record_indices,
          sizeof(retirement->texture_record_indices));
  record->live = false_v;
  vkr_vk_advance_candidate_publication_generation(renderer);
  return true_v;
}

vkr_internal bool8_t vkr_vk_queue_ibl_bake(VkrVulkanRenderer *renderer,
                                           VkrTextureHandle equirect,
                                           VkrTextureHandle source,
                                           VkrTextureHandle irradiance,
                                           VkrTextureHandle prefilter,
                                           bool8_t convert_equirect) {
  if (!renderer ||
      renderer->pending_ibl_bake_count >= VKR_VULKAN_PENDING_IBL_BAKE_MAX)
    return false_v;
  VkrVulkanPublishedTexture *source_texture =
      vkr_vk_published_texture(renderer, source, NULL);
  VkrVulkanPublishedTexture *irradiance_texture =
      vkr_vk_published_texture(renderer, irradiance, NULL);
  VkrVulkanPublishedTexture *prefilter_texture =
      vkr_vk_published_texture(renderer, prefilter, NULL);
  VkrVulkanPublishedTexture *equirect_texture =
      convert_equirect ? vkr_vk_published_texture(renderer, equirect, NULL)
                       : NULL;
  if (!source_texture || !irradiance_texture || !prefilter_texture ||
      (convert_equirect && !equirect_texture) ||
      source_texture->image.array_layers != 6u ||
      irradiance_texture->image.array_layers != 6u ||
      prefilter_texture->image.array_layers != 6u ||
      source_texture->image.width != source_texture->image.height ||
      irradiance_texture->image.width != irradiance_texture->image.height ||
      prefilter_texture->image.width != prefilter_texture->image.height ||
      irradiance_texture->image.mip_levels != 1u ||
      prefilter_texture->storage_slot_count !=
          prefilter_texture->image.mip_levels ||
      irradiance_texture->storage_slot_count != 1u ||
      irradiance_texture->image.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
      prefilter_texture->image.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
      (convert_equirect &&
       (source_texture->image.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
        equirect_texture->image.array_layers != 1u ||
        equirect_texture->image.width != equirect_texture->image.height * 2u ||
        source_texture->storage_slot_count !=
            source_texture->image.mip_levels)))
    return false_v;
  VkrVulkanPublishedTexture *referenced[] = {
      equirect_texture, source_texture, irradiance_texture, prefilter_texture};
  for (uint32_t i = convert_equirect ? 0u : 1u; i < ArrayCount(referenced); ++i)
    referenced[i]->ibl_reference_count++;
  source_texture->ibl_irradiance = irradiance;
  source_texture->ibl_prefilter = prefilter;
  renderer->pending_ibl_bakes[renderer->pending_ibl_bake_count++] =
      (VkrVulkanPendingIblBake){
          .equirect = equirect,
          .source = source,
          .irradiance = irradiance,
          .prefilter = prefilter,
          .convert_equirect = convert_equirect,
      };
  return true_v;
}

vkr_internal bool8_t vkr_vk_asset_bake_ibl_cubemap(void *state,
                                                   VkrTextureHandle source,
                                                   VkrTextureHandle irradiance,
                                                   VkrTextureHandle prefilter) {
  return vkr_vk_queue_ibl_bake(state, VKR_TEXTURE_HANDLE_INVALID, source,
                               irradiance, prefilter, false_v);
}

vkr_internal bool8_t vkr_vk_asset_bake_hdr_environment(
    void *state, VkrTextureHandle equirect, VkrTextureHandle source,
    VkrTextureHandle irradiance, VkrTextureHandle prefilter) {
  return vkr_vk_queue_ibl_bake(state, equirect, source, irradiance, prefilter,
                               true_v);
}

vkr_internal bool8_t vkr_vk_asset_publications_idle(void *state) {
  const VkrVulkanRenderer *renderer = state;
  return renderer && !renderer->pending_texture_initialization_count &&
         !renderer->pending_buffer_initialization_count &&
         !renderer->pending_ibl_bake_count &&
         !renderer->geometry_megabuffer.copy_pending;
}

void vkr_vulkan_renderer_get_asset_publisher(VkrVulkanRenderer *renderer,
                                             VkrAssetPublisher *out_publisher) {
  if (!out_publisher)
    return;
  *out_publisher = renderer
                       ? (VkrAssetPublisher){
                             .state = renderer,
                             .publications_idle =
                                 vkr_vk_asset_publications_idle,
                             .publication_generation =
                                 vkr_vk_candidate_publication_generation,
                             .publish_geometry =
                                  vkr_vk_asset_publish_geometry,
                              .publish_loaded_mesh =
                                  vkr_vk_asset_publish_loaded_mesh,
                              .unpublish_geometry =
                                  vkr_vk_asset_unpublish_geometry,
                              .publish_texture =
                                  vkr_vk_asset_publish_texture,
                              .publish_writable_texture =
                                  vkr_vk_asset_publish_writable_texture,
                              .update_texture_sampler =
                                  vkr_vk_asset_update_texture_sampler,
                              .bake_ibl_cubemap =
                                  vkr_vk_asset_bake_ibl_cubemap,
                              .bake_hdr_environment =
                                  vkr_vk_asset_bake_hdr_environment,
                             .unpublish_texture =
                                 vkr_vk_asset_unpublish_texture,
                             .publish_material =
                                 vkr_vk_asset_publish_material,
                             .unpublish_material =
                                 vkr_vk_asset_unpublish_material,
                         }
                       : (VkrAssetPublisher){0};
}

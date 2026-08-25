#include "renderer/vulkan/vkr_vulkan_internal.h"
#include <math.h>

vkr_internal bool8_t vkr_vk_create_timeline(VkrVulkanRenderer *renderer) {
  VkSemaphoreTypeCreateInfo type_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0u,
  };
  VkSemaphoreCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &type_info,
  };
  return vkCreateSemaphore(vkr_vk_renderer_device(renderer), &create_info, NULL,
                           &renderer->timeline) == VK_SUCCESS;
}

uint64_t vkr_vk_refresh_completed(VkrVulkanRenderer *renderer) {
  uint64_t completed = renderer->completed_value;
  if (renderer->timeline && vkGetSemaphoreCounterValue(
                                vkr_vk_renderer_device(renderer),
                                renderer->timeline, &completed) == VK_SUCCESS) {
    renderer->completed_value = completed;
  }
  return renderer->completed_value;
}

vkr_internal bool8_t vkr_vk_collect_slot_timings(VkrVulkanRenderer *renderer,
                                                 VkrVulkanFrameSlot *slot) {
  if (!slot->timing_requested || slot->timing_collected)
    return true_v;
  if (!slot->retire_value || slot->retire_value > renderer->completed_value ||
      !slot->timestamp_query_count)
    return false_v;
  uint64_t timestamps[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS * 2u] = {0};
  if (vkGetQueryPoolResults(
          vkr_vk_renderer_device(renderer), slot->timestamp_pool, 0u,
          slot->timestamp_query_count,
          (VkDeviceSize)slot->timestamp_query_count * sizeof(*timestamps),
          timestamps, sizeof(*timestamps),
          VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return false_v;
  const float64_t timestamp_ms =
      (float64_t)vkr_vulkan_device_properties(renderer->device)
          ->properties.limits.timestampPeriod /
      1000000.0;
  for (uint32_t i = 0; i < slot->pass_timing_count; ++i) {
    const size_t query_index = (size_t)i * 2u;
    const uint64_t begin = timestamps[query_index];
    const uint64_t end = timestamps[query_index + 1u];
    slot->pass_timings[i].valid = end >= begin;
    if (slot->pass_timings[i].valid)
      slot->pass_timings[i].gpu_ms = (float64_t)(end - begin) * timestamp_ms;
  }
  slot->timing_collected = true_v;
  return true_v;
}

bool8_t vkr_vulkan_renderer_create(const VkrVulkanRendererConfig *config,
                                   VkrVulkanRenderer **out_renderer) {
  if (!out_renderer)
    return false_v;
  *out_renderer = NULL;
  if (!config || !config->allocator || !config->width || !config->height ||
      (config->target_kind == VKR_PRESENT_TARGET_OFFSCREEN &&
       !config->image_count) ||
      config->image_count > VKR_VULKAN_TARGET_IMAGE_MAX ||
      !config->sampled_image_capacity || !config->storage_image_capacity ||
      /* Sentinel, shadow comparison and transmission-feedback samplers occupy
         the first three permanent rows before asset publication begins. */
      config->sampler_capacity < 3u || !config->geometry_capacity ||
      !config->texture_capacity ||
      /* Every published texture also takes one sampled descriptor slot, so a
         texture ID space wider than the heap could never be fully resident. */
      config->texture_capacity > config->sampled_image_capacity ||
      !config->material_record_capacity || !config->device_buffer_block_size ||
      !config->device_image_block_size || !config->upload_buffer_block_size ||
      !config->readback_buffer_block_size || !config->memory_block_capacity ||
      !config->memory_blocks_per_pool ||
      config->memory_blocks_per_pool > config->memory_block_capacity ||
      !config->memory_block_allocation_capacity ||
      config->publication_staging_capacity < 2u ||
      ((config->capture_ring_capacity == 0u) !=
       (config->capture_max_batch_bytes == 0u)) ||
      config->capture_ring_capacity > VKR_VULKAN_FRAME_SLOT_COUNT ||
      config->capture_ring_capacity > VKR_CAPTURE_RING_CAPACITY_MAX ||
      (uint64_t)config->material_slot_capacity <
          (uint64_t)config->material_record_capacity * 2u + 1u) {
    log_error("Invalid Vulkan renderer configuration (target=%u, "
              "extent=%ux%u, images=%u)",
              config ? (uint32_t)config->target_kind : UINT32_MAX,
              config ? config->width : 0u, config ? config->height : 0u,
              config ? config->image_count : 0u);
    return false_v;
  }
  VkrVulkanRenderer *renderer = vkr_allocator_alloc(
      config->allocator, sizeof(*renderer), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!renderer) {
    return false_v;
  }
  MemZero(renderer, sizeof(*renderer));
  renderer->allocator = config->allocator;
  renderer->config = *config;
  renderer->capture_storage_size = vkr_capture_ring_storage_requirement(
      config->capture_ring_capacity, config->capture_max_batch_bytes);
  if (config->capture_ring_capacity > 0u) {
    if (renderer->capture_storage_size > SIZE_MAX - KB(64))
      goto cleanup;
    const uint64_t capture_allocator_size =
        renderer->capture_storage_size + KB(64);
    if (!vkr_dmemory_create(capture_allocator_size, capture_allocator_size,
                            &renderer->capture_storage_memory))
      goto cleanup;
    renderer->capture_storage = vkr_dmemory_alloc(
        &renderer->capture_storage_memory, renderer->capture_storage_size);
    if (!renderer->capture_storage ||
        !vkr_capture_ring_init(
            &renderer->capture_ring, config->capture_ring_capacity,
            config->capture_max_batch_bytes, renderer->capture_storage,
            renderer->capture_storage_size))
      goto cleanup;
  }
  if (!renderer->config.graph_path)
    renderer->config.graph_path = "assets/render_graphs/main.rendergraph.json";
  renderer->candidate_publication_generation = 1u;
  if (!renderer->config.max_graph_images)
    renderer->config.max_graph_images = 128u;
  if (!renderer->config.max_graph_buffers)
    renderer->config.max_graph_buffers = 128u;
  if (!renderer->config.max_graph_passes)
    renderer->config.max_graph_passes = 64u;
  if (vkr_gpu_submit_ring_create(
          &renderer->command_ring, VKR_VULKAN_FRAME_SLOT_COUNT,
          VKR_VULKAN_FRAME_SLOT_COUNT, renderer->command_ring_slots,
          sizeof(renderer->command_ring_slots)) !=
      VKR_GPU_SUBMIT_RING_STATUS_OK)
    goto cleanup;
  if (!vkr_dmemory_create(MB(8), GB(2),
                          &renderer->publication_staging_memory)) {
    log_error("Vulkan failed to reserve publication source memory");
    goto cleanup;
  }
  renderer->graph_images_size = (uint64_t)renderer->config.max_graph_images *
                                sizeof(*renderer->graph_images);
  renderer->graph_buffers_size = (uint64_t)renderer->config.max_graph_buffers *
                                 sizeof(*renderer->graph_buffers);
  renderer->graph_image_barriers_size =
      (uint64_t)renderer->config.max_graph_images *
      sizeof(*renderer->graph_image_barriers);
  renderer->graph_buffer_barriers_size =
      (uint64_t)renderer->config.max_graph_buffers *
      sizeof(*renderer->graph_buffer_barriers);
  renderer->graph_images =
      vkr_allocator_alloc(renderer->allocator, renderer->graph_images_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->graph_buffers =
      vkr_allocator_alloc(renderer->allocator, renderer->graph_buffers_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->graph_image_barriers = vkr_allocator_alloc(
      renderer->allocator, renderer->graph_image_barriers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->graph_buffer_barriers = vkr_allocator_alloc(
      renderer->allocator, renderer->graph_buffer_barriers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->graph_frame_arena = arena_create(MB(8), KB(256));
  renderer->graph_frame_allocator =
      (VkrAllocator){.ctx = renderer->graph_frame_arena};
  const bool8_t graph_source_ready =
      renderer->graph_images && renderer->graph_buffers &&
      renderer->graph_image_barriers && renderer->graph_buffer_barriers &&
      renderer->graph_frame_arena &&
      vkr_allocator_arena(&renderer->graph_frame_allocator) &&
      vkr_rg_executor_registry_init(&renderer->executors,
                                    renderer->allocator) &&
      vkr_vk_register_graph_executors(renderer) &&
      vkr_rg_json_load_file(renderer->allocator, renderer->config.graph_path,
                            &renderer->json_graph) &&
      vkr_rg_json_bind_executors(&renderer->json_graph, &renderer->executors);
  renderer->graph =
      graph_source_ready ? vkr_rg_create(renderer->allocator) : NULL;
  if (!renderer->graph ||
      !vkr_rg_set_frame_allocator(renderer->graph,
                                  &renderer->graph_frame_allocator)) {
    log_error("Vulkan failed to initialize the authored render graph");
    goto cleanup;
  }
  MemZero(renderer->graph_images, renderer->graph_images_size);
  MemZero(renderer->graph_buffers, renderer->graph_buffers_size);
  MemZero(renderer->graph_image_barriers, renderer->graph_image_barriers_size);
  MemZero(renderer->graph_buffer_barriers,
          renderer->graph_buffer_barriers_size);
  VkrVulkanDeviceConfig device_config = {
      .allocator = config->allocator,
      .window = config->window,
      .sampled_image_capacity = config->sampled_image_capacity,
      .storage_image_capacity = config->storage_image_capacity,
      .sampler_capacity = config->sampler_capacity,
      .root_push_constant_size = sizeof(VkrVulkanPushConstants),
      .windowed = config->target_kind != VKR_PRESENT_TARGET_OFFSCREEN,
      .enable_validation = config->enable_validation,
      .enable_synchronization_validation =
          config->enable_synchronization_validation,
      .enable_gpu_assisted = config->enable_gpu_assisted,
  };
  if (!vkr_vulkan_device_create(&device_config, &renderer->device) ||
      !vkr_vk_create_timeline(renderer) ||
      !vkr_vk_pipeline_cache_initialize(renderer)) {
    log_error("Vulkan failed to create the selected device, timeline, "
              "or pipeline cache");
    goto cleanup;
  }
  const VkrVulkanMemoryPoolConfig memory_config = {
      .allocator = renderer->allocator,
      .device = vkr_vk_renderer_device(renderer),
      .block_sizes =
          {
              [VKR_VULKAN_MEMORY_CLASS_DEVICE] =
                  {
                      [VKR_VULKAN_MEMORY_KIND_BUFFER] =
                          config->device_buffer_block_size,
                      [VKR_VULKAN_MEMORY_KIND_IMAGE] =
                          config->device_image_block_size,
                  },
              [VKR_VULKAN_MEMORY_CLASS_UPLOAD] =
                  {
                      [VKR_VULKAN_MEMORY_KIND_BUFFER] =
                          config->upload_buffer_block_size,
                      [VKR_VULKAN_MEMORY_KIND_IMAGE] =
                          config->upload_buffer_block_size,
                  },
              [VKR_VULKAN_MEMORY_CLASS_STAGING] =
                  {
                      [VKR_VULKAN_MEMORY_KIND_BUFFER] =
                          config->upload_buffer_block_size,
                      [VKR_VULKAN_MEMORY_KIND_IMAGE] =
                          config->upload_buffer_block_size,
                  },
              [VKR_VULKAN_MEMORY_CLASS_READBACK] =
                  {
                      [VKR_VULKAN_MEMORY_KIND_BUFFER] =
                          config->readback_buffer_block_size,
                      [VKR_VULKAN_MEMORY_KIND_IMAGE] =
                          config->readback_buffer_block_size,
                  },
          },
      .max_blocks = config->memory_block_capacity,
      .max_blocks_per_pool = config->memory_blocks_per_pool,
      .max_allocations_per_block = config->memory_block_allocation_capacity,
  };
  if (!vkr_vulkan_memory_pool_create(&memory_config, &renderer->memory_pool)) {
    log_error("Vulkan failed to create the pooled allocator");
    goto cleanup;
  }
  if (config->target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    if (!vkr_vk_create_acquire_semaphores(renderer) ||
        !vkr_vk_create_window_target(renderer, config->width, config->height,
                                     config->image_count, VK_NULL_HANDLE,
                                     &renderer->window_target)) {
      log_error("Vulkan failed to create the window target");
      goto cleanup;
    }
    renderer->config.width = renderer->window_target.width;
    renderer->config.height = renderer->window_target.height;
    renderer->config.image_count = renderer->window_target.image_count;
  }
  if (!vkr_vk_create_resources(renderer)) {
    log_error("Vulkan failed to create renderer resources");
    goto cleanup;
  }
  if (!vkr_vk_create_descriptor_slot_tables(renderer)) {
    log_error("Vulkan failed to create descriptor tables");
    goto cleanup;
  }
  if (!vkr_vk_publish_sentinel_descriptors(renderer)) {
    log_error("Vulkan failed to publish sentinel descriptors");
    goto cleanup;
  }
  if (!vkr_vk_create_pipelines(renderer)) {
    log_error("Vulkan failed to create renderer pipeline");
    goto cleanup;
  }
  *out_renderer = renderer;
  return true_v;

cleanup:
  vkr_vulkan_renderer_destroy(renderer);
  return false_v;
}

bool8_t vkr_vulkan_renderer_prepare_frame(VkrVulkanRenderer *renderer,
                                          uint64_t source_frame_index,
                                          uint32_t shadow_map_size,
                                          uint32_t shadow_cascade_count,
                                          VkrFrameSetup *out_setup) {
  if (renderer->terminal_failure) {
    return false_v;
  }
  uint64_t completed = vkr_vk_refresh_completed(renderer);
  if (!vkr_vk_collect_captures(renderer, completed))
    return false_v;
  vkr_vk_collect_retired_targets(renderer, completed);
  vkr_vk_collect_retired_window_targets(renderer, completed);
  vkr_vk_collect_asset_publications(renderer, completed);
  if (renderer->target_dirty &&
      !vkr_vk_recreate_window_target(renderer, renderer->config.width,
                                     renderer->config.height,
                                     renderer->config.image_count))
    return false_v;
  completed = vkr_vk_refresh_completed(renderer);
  const uint32_t slot_index = renderer->command_ring.next_slot;
  VkrVulkanFrameSlot *slot = &renderer->frame_slots[slot_index];
  if (slot->retire_value > completed) {
    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1u,
        .pSemaphores = &renderer->timeline,
        .pValues = &slot->retire_value,
    };
    if (vkWaitSemaphores(vkr_vk_renderer_device(renderer), &wait_info,
                         UINT64_MAX) != VK_SUCCESS) {
      return false_v;
    }
    renderer->command_slot_wait_count++;
    completed = vkr_vk_refresh_completed(renderer);
    if (!vkr_vk_collect_captures(renderer, completed))
      return false_v;
    vkr_vk_collect_retired_targets(renderer, completed);
    vkr_vk_collect_asset_publications(renderer, completed);
  }
  if (slot->retire_value && slot->retire_value <= completed &&
      slot->timing_requested && !slot->timing_collected &&
      !vkr_vk_collect_slot_timings(renderer, slot))
    return false_v;
  if (!vkr_vk_stage_next_publication_batch(renderer))
    return false_v;
  if (vkResetCommandPool(vkr_vk_renderer_device(renderer), slot->command_pool,
                         0u) != VK_SUCCESS) {
    return false_v;
  }
  if (vkr_gpu_submit_ring_acquire(&renderer->command_ring, 1u, completed,
                                  &renderer->active_command_slice) !=
          VKR_GPU_SUBMIT_RING_STATUS_OK ||
      renderer->active_command_slice.slot_index != slot_index)
    return false_v;
  renderer->active_frame_slot = slot_index;
  renderer->frame_active = true_v;
  slot->source_frame_index = source_frame_index;
  slot->acquired_window_image = false_v;
  slot->reacquired_presented_image = false_v;
  if (renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN) {
    slot->image_index = renderer->next_image_index;
    renderer->next_image_index =
        (renderer->next_image_index + 1u) % renderer->targets.image_count;
  } else {
    const VkResult acquire_result = vkAcquireNextImageKHR(
        vkr_vk_renderer_device(renderer), renderer->window_target.swapchain,
        UINT64_MAX, renderer->acquire_semaphores[slot_index], VK_NULL_HANDLE,
        &slot->image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
      renderer->target_dirty = true_v;
      vkr_vulkan_renderer_cancel_frame(renderer);
      return false_v;
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
      vkr_vulkan_renderer_cancel_frame(renderer);
      return false_v;
    }
    if (acquire_result == VK_SUBOPTIMAL_KHR)
      renderer->target_dirty = true_v;
    slot->acquired_window_image = true_v;
    const uint32_t image_index = slot->image_index;
    slot->reacquired_presented_image =
        renderer->window_target.image_presented[image_index];
    if (renderer->window_target.present_fence_pending[image_index]) {
      VkFence *present_fence =
          &renderer->window_target.present_complete[image_index];
      if (vkWaitForFences(vkr_vk_renderer_device(renderer), 1u, present_fence,
                          VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
          vkResetFences(vkr_vk_renderer_device(renderer), 1u, present_fence) !=
              VK_SUCCESS) {
        vkr_vulkan_renderer_cancel_frame(renderer);
        return false_v;
      }
      renderer->window_target.present_fence_pending[image_index] = false_v;
    }
    const uint64_t image_submit =
        renderer->window_target.image_last_submit_value[image_index];
    if (image_submit > completed) {
      VkSemaphoreWaitInfo image_wait = {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
          .semaphoreCount = 1u,
          .pSemaphores = &renderer->timeline,
          .pValues = &image_submit,
      };
      if (vkWaitSemaphores(vkr_vk_renderer_device(renderer), &image_wait,
                           UINT64_MAX) != VK_SUCCESS) {
        vkr_vulkan_renderer_cancel_frame(renderer);
        return false_v;
      }
    }
  }
  *out_setup = (VkrFrameSetup){
      .image_index = slot->image_index,
      .window_width =
          renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? renderer->targets.width
              : renderer->window_target.width,
      .window_height =
          renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? renderer->targets.height
              : renderer->window_target.height,
      .swapchain_format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,
      .swapchain_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
  };
  const VkrVulkanImage *target = &renderer->targets.images[slot->image_index];
  renderer->prepared_frame = (VkrRenderGraphFrameInfo){
      .frame_index = (uint32_t)source_frame_index,
      .image_index = slot->image_index,
      .delta_time = 1.0 / 60.0,
      .target_width = out_setup->window_width,
      .target_height = out_setup->window_height,
      .window_width = out_setup->window_width,
      .window_height = out_setup->window_height,
      .viewport_width = out_setup->window_width,
      .viewport_height = out_setup->window_height,
      .target_color_format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .target_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .target_color_initial_state =
          target->layout == VK_IMAGE_LAYOUT_UNDEFINED
              ? (VkrPresentTargetImageState){
                    .access = VKR_IMAGE_ACCESS_NONE,
                    .layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
                }
              : (VkrPresentTargetImageState){
                    .access = VKR_IMAGE_ACCESS_TRANSFER_SRC,
                    .layout = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                },
      .target_depth_initial_state = {
          .access = VKR_IMAGE_ACCESS_NONE,
          .layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
      },
      .target_terminal_state = {
          .access = VKR_IMAGE_ACCESS_TRANSFER_SRC,
          .layout = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      },
      .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_map_size = shadow_map_size,
      .shadow_map_layer_count = shadow_cascade_count,
      .shadow_cascade_count = shadow_cascade_count,
  };
  vkr_vulkan_renderer_retained_shadow_token(renderer, slot->image_index,
                                            &out_setup->retained_shadow);
  return true_v;
}

/**
 * Reports a frame rejected for want of frame-upload bytes rather than for a
 * malformed packet. Both surface as a false return from recording, and without
 * this the two are indistinguishable in a log.
 */
vkr_internal void
vkr_vk_report_upload_exhaustion(VkrVulkanRenderer *renderer,
                                const VkrVulkanFrameSlot *slot) {
  if (slot->frame_upload_exhaustions) {
    renderer->frame_upload_exhaustion_count += slot->frame_upload_exhaustions;
    log_warn("Vulkan frame rejected: %u frame-upload allocation(s) "
             "failed against a %llu-byte per-slot budget (%llu consumed). The "
             "packet is not malformed; the upload buffer is too small for it",
             slot->frame_upload_exhaustions,
             (unsigned long long)slot->frame_upload.size,
             (unsigned long long)slot->frame_upload_cursor);
  }
}

vkr_internal bool8_t vkr_vk_record_draw(VkrVulkanRenderer *renderer,
                                        VkrVulkanFrameSlot *slot) {
  VkCommandBuffer command = slot->command_buffer;
  if (!vkr_vk_prepare_packet_uploads(renderer, slot, renderer->graph->packet)) {
    vkr_vk_report_upload_exhaustion(renderer, slot);
    return false_v;
  }
  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (slot->timing_requested)
    vkResetQueryPool(vkr_vk_renderer_device(renderer), slot->timestamp_pool, 0u,
                     VKR_RENDERER_IMPL_MAX_PASS_TIMINGS * 2u);
  if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
    log_error("Vulkan failed to begin the frame command buffer");
    return false_v;
  }
  vkr_vk_record_buffer_initializations(renderer, command);
  vkr_vk_record_texture_initializations(renderer, command);
  if (!renderer->sentinel_uploaded) {
    vkr_vk_cmd_image_barrier(
        command, renderer->sentinel_image.handle, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy2 copy_region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = 0u,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1u},
        .imageExtent = {.width = 1u, .height = 1u, .depth = 1u},
    };
    VkCopyBufferToImageInfo2 copy_info = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = renderer->upload.handle,
        .dstImage = renderer->sentinel_image.handle,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1u,
        .pRegions = &copy_region,
    };
    vkCmdCopyBufferToImage2(command, &copy_info);
    vkr_vk_cmd_image_barrier(
        command, renderer->sentinel_image.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  const VkDescriptorBufferBindingInfoEXT descriptor_bindings[] = {
      {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
          .address = renderer->resource_descriptors.address,
          .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
      },
      {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
          .address = renderer->sampler_descriptors.address,
          .usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
      },
  };
  vkr_vulkan_device_cmd_bind_descriptor_buffers(renderer->device)(
      command, ArrayCount(descriptor_bindings), descriptor_bindings);
  const uint32_t buffer_indices[] = {0u, 1u};
  const VkDeviceSize descriptor_offsets[] = {0u, 0u};
  vkr_vulkan_device_cmd_set_descriptor_offsets(renderer->device)(
      command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline_layout, 0u,
      ArrayCount(buffer_indices), buffer_indices, descriptor_offsets);
  vkr_vulkan_device_cmd_set_descriptor_offsets(renderer->device)(
      command, VK_PIPELINE_BIND_POINT_COMPUTE, renderer->pipeline_layout, 0u,
      ArrayCount(buffer_indices), buffer_indices, descriptor_offsets);

  if (!vkr_vk_record_graph(renderer, command)) {
    vkr_vk_report_upload_exhaustion(renderer, slot);
    return false_v;
  }

  VkrVulkanImage *target = &renderer->targets.images[slot->image_index];
  if (!vkr_vk_record_capture(renderer, command, slot)) {
    log_error("Vulkan failed to record capture copies");
    return false_v;
  }
  if (!vkr_vk_record_deferred_readback(renderer, command)) {
    log_error("Vulkan failed to record deferred diagnostics readback");
    return false_v;
  }
  VkrVulkanImage *readback_image = target;
  uint32_t readback_x = 0u;
  uint32_t readback_y = 0u;
  slot->picking_readback_pending = false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  if (packet->picking && packet->picking->pending) {
    const VkrRgImageHandle picking_handle =
        vkr_rg_find_image(renderer->graph, string8_lit("picking_color"));
    VkrVulkanGraphImageInstance *picking =
        vkr_vk_graph_image(renderer, picking_handle, slot->image_index);
    if (!picking || packet->picking->x >= picking->image.width ||
        packet->picking->y >= picking->image.height) {
      log_error("Vulkan picking readback target is unavailable");
      return false_v;
    }
    readback_image = &picking->image;
    readback_x = packet->picking->x;
    readback_y = packet->picking->y;
    slot->picking_x = readback_x;
    slot->picking_y = readback_y;
    slot->picking_readback_pending = true_v;
  }
  VkBufferImageCopy2 readback_region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1u},
      .imageOffset = {.x = (int32_t)readback_x,
                      .y = (int32_t)readback_y,
                      .z = 0},
      .imageExtent = {.width = 1u, .height = 1u, .depth = 1u},
  };
  VkCopyImageToBufferInfo2 readback_info = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
      .srcImage = readback_image->handle,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstBuffer = slot->readback.handle,
      .regionCount = 1u,
      .pRegions = &readback_region,
  };
  vkCmdCopyImageToBuffer2(command, &readback_info);
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    VkrVulkanWindowTarget *window = &renderer->window_target;
    const uint32_t image_index = slot->image_index;
    vkr_vk_cmd_image_barrier(
        command, window->images[image_index], VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        window->image_presented[image_index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                             : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageBlit2 blit_region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1u},
        .srcOffsets = {{0, 0, 0},
                       {(int32_t)target->width, (int32_t)target->height, 1}},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1u},
        .dstOffsets = {{0, 0, 0},
                       {(int32_t)window->width, (int32_t)window->height, 1}},
    };
    const VkBlitImageInfo2 blit_info = {
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = target->handle,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = window->images[image_index],
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1u,
        .pRegions = &blit_region,
        .filter = VK_FILTER_NEAREST,
    };
    vkCmdBlitImage2(command, &blit_info);
    vkr_vk_cmd_image_barrier(
        command, window->images[image_index], VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  VkBufferMemoryBarrier2 readback_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->readback.handle,
      .size = VK_WHOLE_SIZE,
  };
  VkDependencyInfo readback_dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &readback_barrier,
  };
  vkCmdPipelineBarrier2(command, &readback_dependency);
  if (!vkr_vk_flush(renderer, &slot->frame_upload.allocation, 0u,
                    slot->frame_upload_cursor)) {
    log_error("Vulkan failed to flush %llu frame-upload bytes",
              (unsigned long long)slot->frame_upload_cursor);
    return false_v;
  }
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    log_error("Vulkan failed to end the frame command buffer");
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_fail_after_submit(VkrVulkanRenderer *renderer,
                                              const char *reason) {
  log_error("Vulkan failed after queue submit: %s", reason);
  renderer->frame_active = false_v;
  renderer->terminal_failure = true_v;
  vkr_rg_end_frame(renderer->graph);
  return false_v;
}

bool8_t vkr_vulkan_renderer_submit_packet(VkrVulkanRenderer *renderer,
                                          const VkrRenderPacket *packet,
                                          VkrVulkanResult *out_result) {
  const bool8_t timing_requested = packet->debug &&
                                   packet->debug->enable_timing &&
                                   packet->debug->capture_pass_timestamps;
  renderer->prepared_frame.editor_enabled = packet->frame.editor_enabled;
  renderer->prepared_frame.picking_pending =
      packet->picking && packet->picking->pending;
  renderer->prepared_frame.transmission_pending =
      packet->world && packet->world->transmission_gpu_candidate_count > 0u;
  renderer->prepared_frame.transmission_compact_enabled = false_v;
  renderer->prepared_frame.timing_enabled = timing_requested;
  renderer->prepared_frame.sdsm_enabled =
      packet->shadow && packet->shadow->sdsm_enabled;
  renderer->prepared_frame.shadow_cascade_count =
      packet->shadow
          ? Min(packet->shadow->cascade_count, VKR_SHADOW_CASCADE_COUNT_MAX)
          : 0u;
  renderer->prepared_frame.shadow_cascade_render_mask =
      packet->shadow ? packet->shadow->cascade_render_mask : 0u;
  uint32_t hzb_mip_count = 1u;
  uint32_t hzb_extent = Max(renderer->prepared_frame.viewport_width,
                            renderer->prepared_frame.viewport_height);
  while (hzb_extent > 1u) {
    hzb_extent >>= 1u;
    hzb_mip_count++;
  }
  renderer->prepared_frame.hzb_reduce_pass_count = hzb_mip_count - 1u;
  vkr_rg_begin_frame(renderer->graph, &renderer->prepared_frame);
  vkr_rg_set_packet(renderer->graph, packet);
  /* Installed before compilation, because seeding a retained subresource reads
     through this provider. */
  vkr_vk_install_retained_provider(renderer);
  if (!vkr_rg_build_from_json(renderer->graph, &renderer->json_graph,
                              &renderer->prepared_frame)) {
    log_error("Vulkan failed to build the authored render graph");
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_rg_compile_schedule(renderer->graph)) {
    log_error("Vulkan failed to compile the authored render graph");
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (renderer->graph->images.length > renderer->config.max_graph_images ||
      renderer->graph->buffers.length > renderer->config.max_graph_buffers ||
      renderer->graph->passes.length > renderer->config.max_graph_passes) {
    log_error("Vulkan authored graph exceeds configured capacity "
              "(%llu/%u images, %llu/%u buffers, %llu/%u passes)",
              (unsigned long long)renderer->graph->images.length,
              renderer->config.max_graph_images,
              (unsigned long long)renderer->graph->buffers.length,
              renderer->config.max_graph_buffers,
              (unsigned long long)renderer->graph->passes.length,
              renderer->config.max_graph_passes);
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_vk_validate_graph(renderer)) {
    log_error("Vulkan failed to validate the authored graph");
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_vk_realize_graph_images(renderer)) {
    log_error("Vulkan failed to realize authored graph images");
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_vk_realize_graph_buffers(renderer)) {
    log_error("Vulkan failed to realize authored graph buffers");
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!vkr_vk_plan_capture(renderer, packet, slot)) {
    log_error("Vulkan failed to plan the requested capture batch");
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  slot->timing_requested = timing_requested;
  slot->timing_collected = !slot->timing_requested;
  slot->sdsm_requested = renderer->prepared_frame.sdsm_enabled;
  slot->shadow_depth_range = (VkrShadowDepthRangeSample){0};
  slot->transmission_coverage_requested =
      renderer->prepared_frame.transmission_pending &&
      renderer->prepared_frame.timing_enabled &&
      !renderer->prepared_frame.transmission_compact_enabled;
  slot->transmission_coverage_extent[0] =
      renderer->prepared_frame.viewport_width;
  slot->transmission_coverage_extent[1] =
      renderer->prepared_frame.viewport_height;
  slot->timestamp_query_count = 0u;
  if (!vkr_vk_record_draw(renderer, slot)) {
    log_error("Vulkan command recording failed");
    if (slot->capture_request_id)
      (void)vkr_capture_ring_fail(&renderer->capture_ring,
                                  slot->capture_request_id,
                                  VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED);
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_vk_flush_publication_ranges(renderer)) {
    log_error("Vulkan publication range flush failed");
    if (slot->capture_request_id)
      (void)vkr_capture_ring_fail(&renderer->capture_ring,
                                  slot->capture_request_id,
                                  VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    vkr_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  const uint64_t signal_value = renderer->submit_value + 1u;
  if (slot->sdsm_requested) {
    const float32_t a = packet->globals.projection.elements[10];
    const float32_t b = packet->globals.projection.elements[14];
    slot->shadow_depth_range = (VkrShadowDepthRangeSample){
        .projection_convention = 0u,
        .source_depth_linearize = {a, b, 0.0f, 0.0f},
        .source_near = a != 0.0f ? b / a : 0.0f,
        .source_far = (1.0f + a) != 0.0f ? b / (1.0f + a) : 0.0f,
        .source_frame_index = packet->frame.frame_index,
        .source_projection_generation =
            vkr_shadow_projection_generation(&packet->globals.projection),
        .source_scene_generation = packet->frame.scene_generation,
        .submit_value = signal_value,
    };
  }
  VkCommandBufferSubmitInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = slot->command_buffer,
  };
  VkSemaphoreSubmitInfo signal_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = renderer->timeline,
      .value = signal_value,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  VkSemaphoreSubmitInfo binary_signal = {0};
  VkSemaphoreSubmitInfo acquire_wait = {0};
  VkSemaphoreSubmitInfo signals[2] = {signal_info};
  uint32_t signal_count = 1u;
  uint32_t wait_count = 0u;
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    acquire_wait = (VkSemaphoreSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderer->acquire_semaphores[renderer->active_frame_slot],
        .stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
    };
    binary_signal = (VkSemaphoreSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderer->window_target.render_complete[slot->image_index],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    signals[signal_count++] = binary_signal;
    wait_count = 1u;
  }
  VkSubmitInfo2 submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = wait_count,
      .pWaitSemaphoreInfos = wait_count ? &acquire_wait : NULL,
      .commandBufferInfoCount = 1u,
      .pCommandBufferInfos = &command_info,
      .signalSemaphoreInfoCount = signal_count,
      .pSignalSemaphoreInfos = signals,
  };
  const VkResult submit_result =
      vkQueueSubmit2(vkr_vulkan_device_queue(renderer->device), 1u,
                     &submit_info, VK_NULL_HANDLE);
  if (submit_result != VK_SUCCESS) {
    log_error("Vulkan queue submission failed (result=%d)", (int)submit_result);
    if (slot->capture_request_id)
      (void)vkr_capture_ring_fail(&renderer->capture_ring,
                                  slot->capture_request_id,
                                  VKR_RENDERER_ERROR_SUBMISSION_FAILED);
    vkr_gpu_submit_ring_cancel(&renderer->command_ring,
                               renderer->active_command_slice);
    renderer->frame_active = false_v;
    renderer->terminal_failure = true_v;
    vkr_rg_end_frame(renderer->graph);
    return false_v;
  }
  renderer->submit_value = signal_value;
  if (slot->candidate_residency_pending) {
    slot->candidate_residency = slot->pending_candidate_residency;
    slot->candidate_residency_pending = false_v;
  }
  /* Past the submit, so retained contents are now proven to have been written.
     Every earlier failure path returns without reaching here, which is the
     ADR-029 rollback: a cancelled frame commits nothing and the previous
     contents stay authoritative. */
  vkr_rg_commit_retained_state(renderer->graph);
  vkr_vk_mark_hzb_submitted(renderer, signal_value);
  vkr_vk_mark_graph_images_submitted(renderer, signal_value);
  vkr_vk_mark_graph_buffers_submitted(renderer, signal_value);
  slot->retire_value = signal_value;
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN)
    vkr_vulkan_reacquire_record(&renderer->window_target.reacquire_state,
                                slot->reacquired_presented_image, signal_value);
  if (vkr_gpu_submit_ring_submit(&renderer->command_ring,
                                 renderer->active_command_slice,
                                 signal_value) != VKR_GPU_SUBMIT_RING_STATUS_OK)
    return vkr_vk_fail_after_submit(renderer,
                                    "the command ring lost its acquired slice");
  uint32_t pending_ibl_write = 0u;
  const uint32_t pending_ibl_count = renderer->pending_ibl_bake_count;
  for (uint32_t i = 0u; i < pending_ibl_count; ++i) {
    VkrVulkanPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    if (!job->recorded) {
      if (pending_ibl_write != i)
        renderer->pending_ibl_bakes[pending_ibl_write] = *job;
      pending_ibl_write++;
      continue;
    }
    const VkrTextureHandle handles[] = {job->equirect, job->source,
                                        job->irradiance, job->prefilter};
    for (uint32_t handle_index = job->convert_equirect ? 0u : 1u;
         handle_index < ArrayCount(handles); ++handle_index) {
      VkrVulkanPublishedTexture *texture =
          vkr_vk_texture_publication(renderer, handles[handle_index]);
      if (texture) {
        texture->last_use_submit_value =
            Max(texture->last_use_submit_value, signal_value);
        if (!texture->ibl_reference_count)
          return vkr_vk_fail_after_submit(
              renderer, "an IBL texture lost its ownership reference");
        texture->ibl_reference_count--;
        if (!texture->ibl_reference_count && texture->unpublish_requested) {
          texture->live = false_v;
          texture->pending_retire = true_v;
        }
      }
    }
    MemZero(&renderer->pending_ibl_bakes[i],
            sizeof(renderer->pending_ibl_bakes[i]));
  }
  for (uint32_t i = pending_ibl_write; i < pending_ibl_count; ++i)
    MemZero(&renderer->pending_ibl_bakes[i],
            sizeof(renderer->pending_ibl_bakes[i]));
  renderer->pending_ibl_bake_count = pending_ibl_write;
  if (slot->capture_request_id &&
      !vkr_capture_ring_submit(&renderer->capture_ring,
                               slot->capture_request_id, signal_value,
                               slot->capture_readback.allocation.mapped)) {
    return vkr_vk_fail_after_submit(
        renderer, "the capture ring lost its reserved request");
  }
  if (!vkr_vk_commit_buffer_initializations(renderer, signal_value) ||
      !vkr_vk_commit_texture_initializations(renderer, signal_value))
    return vkr_vk_fail_after_submit(
        renderer, "asset staging retirement could not be committed");
  renderer->targets.images[slot->image_index].layout =
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  renderer->sentinel_image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    VkrVulkanWindowTarget *window = &renderer->window_target;
    const uint32_t image_index = slot->image_index;
    window->image_last_submit_value[image_index] = signal_value;
    const VkSwapchainPresentFenceInfoKHR present_fence_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1u,
        .pFences = &window->present_complete[image_index],
    };
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext =
            window->present_complete[image_index] ? &present_fence_info : NULL,
        .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &window->render_complete[image_index],
        .swapchainCount = 1u,
        .pSwapchains = &window->swapchain,
        .pImageIndices = &image_index,
    };
    const VkResult present_result = vkQueuePresentKHR(
        vkr_vulkan_device_queue(renderer->device), &present_info);
    const VkrVulkanPresentResult disposition =
        vkr_vulkan_present_result_classify(present_result);
    if (disposition.present_completion_tracking_required) {
      window->image_presented[image_index] = true_v;
      window->present_fence_pending[image_index] =
          window->present_complete[image_index] != VK_NULL_HANDLE;
    }
    if (disposition.target_recreate_required)
      renderer->target_dirty = true_v;
    if (disposition.acquired_image_recovery_required ||
        !disposition.enqueue_state_known || disposition.device_lost) {
      return vkr_vk_fail_after_submit(
          renderer, "presentation completion became unprovable");
    }
    slot->acquired_window_image = false_v;
  }
  renderer->sentinel_uploaded = true_v;
  renderer->frame_active = false_v;
  vkr_rg_end_frame(renderer->graph);
  if (out_result) {
    *out_result = (VkrVulkanResult){
        .submit_value = signal_value,
        .source_frame_index = slot->source_frame_index,
        .indexed_draw_count = slot->indexed_draw_count,
        .blend_draw_count = slot->blend_draw_count,
        .image_index = slot->image_index,
        .pass_timing_count = slot->pass_timing_count,
        .packet_build = slot->packet_build,
    };
    MemCopy(out_result->pass_timings, slot->pass_timings,
            (uint64_t)slot->pass_timing_count *
                sizeof(*out_result->pass_timings));
  }
  return true_v;
}

vkr_internal void
vkr_vk_decode_sdsm_result(const VkrVulkanFrameSlot *slot,
                          const uint8_t *readback,
                          VkrShadowDepthRangeSample *out_sample) {
  *out_sample = slot->shadow_depth_range;
  if (!slot->sdsm_requested)
    return;
  VkrVulkanSdsmState state = {0};
  MemCopy(&state, readback + VKR_VULKAN_READBACK_SDSM_STATE_OFFSET,
          sizeof(state));
  out_sample->occupied_count = state.occupied_count;
  if (state.occupied_count == 0u) {
    out_sample->valid = true_v;
    return;
  }
  if (state.min_device_z_bits == UINT32_MAX)
    return;
  MemCopy(&out_sample->min_device_z, &state.min_device_z_bits,
          sizeof(float32_t));
  MemCopy(&out_sample->max_device_z, &state.max_device_z_bits,
          sizeof(float32_t));
  out_sample->valid =
      isfinite(out_sample->min_device_z) &&
              isfinite(out_sample->max_device_z) &&
              out_sample->min_device_z >= 0.0f &&
              out_sample->max_device_z <= 1.0f &&
              out_sample->min_device_z <= out_sample->max_device_z
          ? true_v
          : false_v;
}

bool8_t vkr_vulkan_renderer_poll_result(VkrVulkanRenderer *renderer,
                                        uint64_t after_submit_value,
                                        VkrVulkanResult *out_result) {
  if (!renderer || !out_result) {
    return false_v;
  }
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  VkrVulkanFrameSlot *best = NULL;
  for (uint32_t i = 0; i < VKR_VULKAN_FRAME_SLOT_COUNT; ++i) {
    VkrVulkanFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->retire_value > after_submit_value &&
        slot->retire_value <= completed &&
        (!best || slot->retire_value < best->retire_value)) {
      best = slot;
    }
  }
  if (!best || !vkr_vk_invalidate(renderer, &best->readback.allocation, 0u,
                                  VKR_VULKAN_READBACK_SIZE)) {
    return false_v;
  }
  if (best->timing_requested && !best->timing_collected &&
      !vkr_vk_collect_slot_timings(renderer, best))
    return false_v;
  const uint8_t *color = best->readback.allocation.mapped;
  const VkrGpuDrawCompactionState *opaque =
      (const VkrGpuDrawCompactionState
           *)(color + VKR_VULKAN_READBACK_DRAW_STATE_OFFSET);
  const VkrGpuDrawCompactionState *transmission =
      (const VkrGpuDrawCompactionState
           *)(color + VKR_VULKAN_READBACK_TRANSMISSION_STATE_OFFSET);
  const VkrGpuTransmissionDiagnostics *transmission_diagnostics =
      (const VkrGpuTransmissionDiagnostics
           *)(color + VKR_VULKAN_READBACK_TRANSMISSION_STATE_OFFSET);
  *out_result = (VkrVulkanResult){
      .submit_value = best->retire_value,
      .source_frame_index = best->source_frame_index,
      .indexed_draw_count = best->indexed_draw_count,
      .blend_draw_count = best->blend_draw_count,
      .gpu_visible_count = opaque[0].visible_count,
      .gpu_overflow_count = opaque[0].overflow_count,
      .gpu_resolve_invalid_count = opaque[0].resolve_invalid_count,
      .gpu_occlusion_culled_count = opaque[0].occlusion_culled_count,
      .transmission_gpu_visible_count = transmission->visible_count,
      .transmission_gpu_overflow_count = transmission->overflow_count,
      .transmission_gpu_occlusion_culled_count =
          transmission->occlusion_culled_count,
      .image_index = best->image_index,
      .color = {color[0], color[1], color[2], color[3]},
      .identifier = (uint32_t)color[0] | ((uint32_t)color[1] << 8u) |
                    ((uint32_t)color[2] << 16u) | ((uint32_t)color[3] << 24u),
      .pass_timing_count = best->pass_timing_count,
      .readback_ready = true_v,
      .hzb_history_valid = best->hzb_history_valid,
      .has_gpu_draw_diagnostics = true_v,
      .has_transmission_coverage = best->transmission_coverage_requested,
  };
  vkr_vk_decode_sdsm_result(best, color, &out_result->shadow_depth_range);
  MemCopy(out_result->gpu_bucket_counts, opaque[0].bucket_counts,
          sizeof(out_result->gpu_bucket_counts));
  MemCopy(out_result->transmission_gpu_bucket_counts,
          transmission->bucket_counts,
          sizeof(out_result->transmission_gpu_bucket_counts));
  if (best->transmission_coverage_requested) {
    MemCopy(out_result->transmission_covered_pixels,
            transmission_diagnostics->covered_pixels,
            sizeof(out_result->transmission_covered_pixels));
    MemCopy(out_result->transmission_coverage_extent,
            best->transmission_coverage_extent,
            sizeof(out_result->transmission_coverage_extent));
  }
  for (uint32_t cascade = 0u; cascade < VKR_SHADOW_CASCADE_COUNT_MAX;
       ++cascade) {
    out_result->shadow_gpu_visible_count[cascade] =
        opaque[cascade + 1u].visible_count;
    out_result->shadow_gpu_overflow_count[cascade] =
        opaque[cascade + 1u].overflow_count;
    MemCopy(out_result->shadow_gpu_bucket_counts[cascade],
            opaque[cascade + 1u].bucket_counts,
            sizeof(out_result->shadow_gpu_bucket_counts[cascade]));
  }
  MemCopy(out_result->pass_timings, best->pass_timings,
          (uint64_t)best->pass_timing_count *
              sizeof(*out_result->pass_timings));
  return true_v;
}

VkrCaptureStatus
vkr_vulkan_renderer_capture_poll(VkrVulkanRenderer *renderer,
                                 VkrCaptureRequestId request_id,
                                 VkrCapturePollResult *out_result) {
  if (!renderer || !out_result || request_id == 0u) {
    if (out_result)
      MemZero(out_result, sizeof(*out_result));
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  if (!vkr_vk_collect_captures(renderer, completed)) {
    MemZero(out_result, sizeof(*out_result));
    out_result->status = VKR_CAPTURE_STATUS_FAILED;
    out_result->error = VKR_RENDERER_ERROR_DEVICE_ERROR;
    return out_result->status;
  }
  return vkr_capture_ring_poll(&renderer->capture_ring, request_id, completed,
                               out_result);
}

bool8_t vkr_vulkan_renderer_capture_release(VkrVulkanRenderer *renderer,
                                            VkrCaptureRequestId request_id) {
  return renderer && request_id != 0u &&
         vkr_capture_ring_release(&renderer->capture_ring, request_id);
}

bool8_t vkr_vulkan_renderer_wait_idle(VkrVulkanRenderer *renderer) {
  if (!renderer || !renderer->timeline || renderer->submit_value == 0u) {
    return renderer != NULL;
  }
  const VkSemaphoreWaitInfo wait_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1u,
      .pSemaphores = &renderer->timeline,
      .pValues = &renderer->submit_value,
  };
  if (vkWaitSemaphores(vkr_vk_renderer_device(renderer), &wait_info,
                       UINT64_MAX) != VK_SUCCESS) {
    return false_v;
  }
  vkr_vk_refresh_completed(renderer);
  if (!vkr_vk_collect_captures(renderer, renderer->completed_value))
    return false_v;
  vkr_vk_collect_retired_targets(renderer, renderer->completed_value);
  vkr_vk_collect_retired_window_targets(renderer, renderer->completed_value);
  vkr_vk_collect_asset_publications(renderer, renderer->completed_value);
  return true_v;
}

uint64_t vkr_vulkan_renderer_submit_value(const VkrVulkanRenderer *renderer) {
  return renderer ? renderer->submit_value : 0u;
}

uint64_t
vkr_vulkan_renderer_completed_value(const VkrVulkanRenderer *renderer) {
  if (!renderer) {
    return 0u;
  }
  return vkr_vk_refresh_completed((VkrVulkanRenderer *)renderer);
}

bool8_t
vkr_vulkan_renderer_get_and_reset_upload_wait_count(VkrVulkanRenderer *renderer,
                                                    uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count)
    return false_v;
  *out_wait_count = renderer->upload_wait_count;
  renderer->upload_wait_count = 0u;
  return true_v;
}

bool8_t vkr_vulkan_renderer_get_and_reset_frame_upload_exhaustion_count(
    VkrVulkanRenderer *renderer, uint64_t *out_exhaustion_count) {
  if (!renderer || !out_exhaustion_count)
    return false_v;
  *out_exhaustion_count = renderer->frame_upload_exhaustion_count;
  renderer->frame_upload_exhaustion_count = 0u;
  return true_v;
}

bool8_t vkr_vulkan_renderer_get_and_reset_command_slot_wait_count(
    VkrVulkanRenderer *renderer, uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count) {
    return false_v;
  }
  *out_wait_count = renderer->command_slot_wait_count;
  renderer->command_slot_wait_count = 0u;
  return true_v;
}

void vkr_vulkan_renderer_memory_metrics(const VkrVulkanRenderer *renderer,
                                        VkrVulkanMemoryMetrics *out_metrics) {
  if (!out_metrics)
    return;
  MemZero(out_metrics, sizeof(*out_metrics));
  if (!renderer)
    return;
  VkrVulkanMemoryPoolMetrics metrics = {0};
  vkr_vulkan_memory_pool_get_metrics(renderer->memory_pool, &metrics);
  out_metrics->physical_allocations_live = metrics.physical_allocations_live;
  out_metrics->physical_allocations_peak = metrics.physical_allocations_peak;
  out_metrics->physical_allocations_created =
      metrics.physical_allocations_created;
  out_metrics->physical_allocated_bytes = metrics.physical_allocated_bytes;
  out_metrics->physical_allocated_bytes_peak =
      metrics.physical_allocated_bytes_peak;
  out_metrics->block_capacity_failures = metrics.block_capacity_failures;
  out_metrics->aggregate = metrics.aggregate;
}

void vkr_vulkan_renderer_geometry_megabuffer_metrics(
    const VkrVulkanRenderer *renderer,
    VkrGeometryMegabufferMetrics *out_metrics) {
  if (!out_metrics)
    return;
  *out_metrics = (VkrGeometryMegabufferMetrics){0};
  if (!renderer)
    return;
  const VkrVulkanGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
  out_metrics->vertex_capacity_bytes = mega->vertices.size;
  out_metrics->index_capacity_bytes = mega->indices.size;
  out_metrics->vertex_live_bytes = mega->vertex_live_bytes;
  out_metrics->index_live_bytes = mega->index_live_bytes;
  out_metrics->decode_metadata_live_bytes = mega->decode_metadata_live_bytes;
  out_metrics->live_bytes = mega->vertex_live_bytes + mega->index_live_bytes +
                            mega->decode_metadata_live_bytes;
  out_metrics->fragmentation_bytes =
      mega->vertex_cursor + mega->index_cursor - out_metrics->live_bytes;
  out_metrics->high_water_bytes =
      mega->vertex_high_water + mega->index_high_water;
  out_metrics->vertex_high_water_bytes = mega->vertex_high_water;
  out_metrics->index_high_water_bytes = mega->index_high_water;
  out_metrics->vertex_uploaded_bytes_total = mega->vertex_uploaded_bytes_total;
  out_metrics->index_uploaded_bytes_total = mega->index_uploaded_bytes_total;
  out_metrics->decode_metadata_high_water_bytes =
      mega->decode_metadata_high_water;
  out_metrics->decode_metadata_uploaded_bytes_total =
      mega->decode_metadata_uploaded_bytes_total;
  out_metrics->rejected_publications = mega->rejected_publications;
  out_metrics->generation_replacements = mega->generation_replacements;
  out_metrics->generation = mega->generation;
}

void vkr_vulkan_renderer_device_memory_stats(const VkrVulkanRenderer *renderer,
                                             VkrDeviceMemoryStats *out_stats) {
  if (!out_stats)
    return;
  MemZero(out_stats, sizeof(*out_stats));
  if (!renderer || !renderer->device)
    return;
  VkrVulkanMemoryMetrics metrics = {0};
  vkr_vulkan_renderer_memory_metrics(renderer, &metrics);
  out_stats->live_allocation_count = metrics.physical_allocations_live;
  out_stats->peak_allocation_count = metrics.physical_allocations_peak;
  out_stats->total_allocation_count = metrics.physical_allocations_created;
  out_stats->max_allocation_count =
      vkr_vulkan_device_properties(renderer->device)
          ->properties.limits.maxMemoryAllocationCount;
  out_stats->live_bytes = metrics.physical_allocated_bytes;
  out_stats->peak_bytes = metrics.physical_allocated_bytes_peak;
  out_stats->live_totals_exact = true_v;

  const VkPhysicalDeviceMemoryProperties *memory =
      vkr_vulkan_device_memory_properties(renderer->device);
  out_stats->heap_count =
      Min(memory->memoryHeapCount, (uint32_t)VKR_DEVICE_MEMORY_HEAP_MAX);
  for (uint32_t heap = 0; heap < out_stats->heap_count; ++heap) {
    out_stats->heap_size_bytes[heap] = memory->memoryHeaps[heap].size;
    out_stats->heap_budget_bytes[heap] = memory->memoryHeaps[heap].size;
  }
}

void vkr_vulkan_renderer_heap_metrics(const VkrVulkanRenderer *renderer,
                                      VkrVulkanHeapMetrics *out_metrics) {
  if (!out_metrics)
    return;
  MemZero(out_metrics, sizeof(*out_metrics));
  if (!renderer)
    return;
  vkr_gpu_slot_table_get_metrics(renderer->sampled_image_slots,
                                 &out_metrics->sampled_images);
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &out_metrics->samplers);
  vkr_gpu_slot_table_get_metrics(renderer->storage_image_slots,
                                 &out_metrics->storage_images);
  vkr_gpu_slot_table_get_metrics(renderer->material_slots,
                                 &out_metrics->materials);
}

uint32_t vkr_vulkan_renderer_frame_slot(const VkrVulkanRenderer *renderer) {
  return renderer ? renderer->active_frame_slot : 0u;
}

const VkrVulkanCapabilityProfile *
vkr_vulkan_renderer_profile(const VkrVulkanRenderer *renderer) {
  return renderer ? vkr_vulkan_device_profile(renderer->device) : NULL;
}

VkrAllocator *vkr_vulkan_renderer_allocator(VkrVulkanRenderer *renderer) {
  return renderer ? renderer->allocator : NULL;
}

bool8_t vkr_vulkan_renderer_hdr_ibl_limits(const VkrVulkanRenderer *renderer,
                                           uint32_t *out_max_cube_extent,
                                           uint32_t *out_max_mip_levels) {
  if (out_max_cube_extent)
    *out_max_cube_extent = 0u;
  if (out_max_mip_levels)
    *out_max_mip_levels = 0u;
  if (!renderer || !renderer->device)
    return false_v;
  VkFormatProperties3 properties3 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
  };
  VkFormatProperties2 properties2 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &properties3,
  };
  vkGetPhysicalDeviceFormatProperties2(
      vkr_vulkan_device_physical(renderer->device),
      VK_FORMAT_R16G16B16A16_SFLOAT, &properties2);
  const VkFormatFeatureFlags2 required =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_2_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_2_BLIT_DST_BIT |
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((properties3.optimalTilingFeatures & required) != required)
    return false_v;
  const VkPhysicalDeviceProperties2 *device_properties =
      vkr_vulkan_device_properties(renderer->device);
  if (!device_properties)
    return false_v;
  const uint32_t extent =
      Min(device_properties->properties.limits.maxImageDimensionCube,
          (uint32_t)VKR_IBL_PREFILTER_SIZE);
  if (!extent)
    return false_v;
  uint32_t mip_count = 1u;
  for (uint32_t size = extent; size > 1u; size >>= 1u)
    mip_count++;
  mip_count = Min(mip_count, (uint32_t)VKR_IBL_PREFILTER_MIP_COUNT);
  if (out_max_cube_extent)
    *out_max_cube_extent = extent;
  if (out_max_mip_levels)
    *out_max_mip_levels = mip_count;
  return true_v;
}

VkrRendererError vkr_vulkan_renderer_get_pixel_readback_result(
    VkrVulkanRenderer *renderer, VkrPixelReadbackResult *out_result) {
  if (!renderer || !out_result)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  *out_result = (VkrPixelReadbackResult){.status = VKR_READBACK_STATUS_IDLE};
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  VkrVulkanFrameSlot *best = NULL;
  for (uint32_t i = 0u; i < VKR_VULKAN_FRAME_SLOT_COUNT; ++i) {
    VkrVulkanFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->picking_readback_pending &&
        (!best || slot->retire_value < best->retire_value))
      best = slot;
  }
  if (!best)
    return VKR_RENDERER_ERROR_NONE;
  out_result->x = best->picking_x;
  out_result->y = best->picking_y;
  if (best->retire_value > completed) {
    out_result->status = VKR_READBACK_STATUS_PENDING;
    return VKR_RENDERER_ERROR_NONE;
  }
  if (!vkr_vk_invalidate(renderer, &best->readback.allocation, 0u,
                         sizeof(uint32_t))) {
    out_result->status = VKR_READBACK_STATUS_ERROR;
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }
  MemCopy(&out_result->data, best->readback.allocation.mapped,
          sizeof(out_result->data));
  out_result->valid = true_v;
  out_result->status = VKR_READBACK_STATUS_READY;
  best->picking_readback_pending = false_v;
  return VKR_RENDERER_ERROR_NONE;
}

bool8_t
vkr_vulkan_renderer_texture_format_supported(const VkrVulkanRenderer *renderer,
                                             VkrTextureFormat format) {
  if (!renderer || !renderer->device)
    return false_v;
  const VkFormat native_format = vkr_vk_texture_format(format);
  if (native_format == VK_FORMAT_UNDEFINED)
    return false_v;
  VkFormatProperties properties = {0};
  vkGetPhysicalDeviceFormatProperties(
      vkr_vulkan_device_physical(renderer->device), native_format, &properties);
  const VkFormatFeatureFlags required =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return (properties.optimalTilingFeatures & required) == required;
}

bool8_t vkr_vulkan_renderer_graph_resource_stats(
    const VkrVulkanRenderer *renderer, VkrRenderGraphResourceStats *out_stats) {
  if (!renderer || !renderer->graph || !out_stats)
    return false_v;
  MemZero(out_stats, sizeof(*out_stats));
  for (uint32_t i = 0u; i < renderer->config.max_graph_images; ++i) {
    const VkrVulkanGraphImage *image = &renderer->graph_images[i];
    if (!image->live || image->external_swapchain)
      continue;
    VkrTextureFormatInfo format = {0};
    if (!vkr_texture_format_get_info(image->desc.format, &format) ||
        format.block_width != 1u || format.block_height != 1u)
      continue;
    uint64_t texels = 0u;
    for (uint32_t mip = 0u; mip < Max(image->desc.mip_levels, 1u); ++mip) {
      texels += (uint64_t)Max(image->desc.width >> mip, 1u) *
                Max(image->desc.height >> mip, 1u);
    }
    const uint64_t bytes_per_image = texels * Max(image->desc.layers, 1u) *
                                     Max(image->desc.samples, 1u) *
                                     format.bytes_per_block;
    out_stats->live_image_textures += image->instance_count;
    out_stats->live_image_bytes += bytes_per_image * image->instance_count;
  }
  out_stats->peak_image_textures = out_stats->live_image_textures;
  out_stats->peak_image_bytes = out_stats->live_image_bytes;
  return true_v;
}

void vkr_vulkan_renderer_target_information(
    const VkrVulkanRenderer *renderer, VkrPresentMode *out_present_mode,
    VkrSurfaceColorFormat *out_color_format,
    VkrSurfaceDepthFormat *out_depth_format,
    VkrSurfaceColorSpace *out_color_space, float32_t *out_max_anisotropy) {
  if (!renderer)
    return;
  const bool8_t offscreen =
      renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN;
  const VkFormat format =
      offscreen ? VK_FORMAT_R8G8B8A8_UNORM : renderer->window_target.format;
  if (out_present_mode) {
    *out_present_mode =
        offscreen ? VKR_PRESENT_MODE_DEFAULT
        : renderer->window_target.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR
            ? VKR_PRESENT_MODE_IMMEDIATE
        : renderer->window_target.present_mode == VK_PRESENT_MODE_MAILBOX_KHR
            ? VKR_PRESENT_MODE_MAILBOX
            : VKR_PRESENT_MODE_FIFO;
  }
  if (out_color_format) {
    *out_color_format = format == VK_FORMAT_B8G8R8A8_SRGB
                            ? VKR_SURFACE_COLOR_FORMAT_BGRA8_SRGB
                        : format == VK_FORMAT_R8G8B8A8_SRGB
                            ? VKR_SURFACE_COLOR_FORMAT_RGBA8_SRGB
                        : format == VK_FORMAT_B8G8R8A8_UNORM
                            ? VKR_SURFACE_COLOR_FORMAT_BGRA8_UNORM
                            : VKR_SURFACE_COLOR_FORMAT_RGBA8_UNORM;
  }
  if (out_depth_format)
    *out_depth_format = VKR_SURFACE_DEPTH_FORMAT_D32_SFLOAT;
  if (out_color_space)
    *out_color_space = VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR;
  if (out_max_anisotropy) {
    const VkPhysicalDeviceProperties2 *properties =
        vkr_vulkan_device_properties(renderer->device);
    *out_max_anisotropy =
        properties ? properties->properties.limits.maxSamplerAnisotropy : 1.0f;
  }
}

vkr_internal void vkr_vk_drain_asset_publications(VkrVulkanRenderer *renderer) {
  // The material slot table is the last publication table initialized.
  // Before then, renderer destruction only needs to release setup resources.
  if (!renderer->material_slots)
    return;
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrVulkanPublishedMaterial *material = &renderer->published_materials[i];
    if (material->live)
      (void)vkr_vk_asset_unpublish_material(renderer, material->handle);
  }
  vkr_vk_collect_asset_publications(renderer, completed);
  for (uint32_t i = 0; i < renderer->config.texture_capacity; ++i) {
    VkrVulkanPublishedTexture *texture = &renderer->published_textures[i];
    if (texture->live)
      (void)vkr_vk_asset_unpublish_texture(renderer, texture->handle);
  }
  for (uint32_t i = 0; i < renderer->config.geometry_capacity; ++i) {
    VkrVulkanPublishedGeometry *geometry = &renderer->published_geometries[i];
    if (geometry->live)
      (void)vkr_vk_asset_unpublish_geometry(renderer, geometry->handle);
  }
  vkr_vk_collect_asset_publications(renderer, completed);
}

void vkr_vulkan_renderer_destroy(VkrVulkanRenderer *renderer) {
  if (!renderer) {
    return;
  }
  VkrAllocator *allocator = renderer->allocator;
  VkDevice device = renderer->device
                        ? vkr_vulkan_device_handle(renderer->device)
                        : VK_NULL_HANDLE;
  if (device) {
    vkr_vulkan_renderer_wait_idle(renderer);
    if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN)
      (void)vkDeviceWaitIdle(device);
    vkr_vk_discard_buffer_initializations(renderer);
    vkr_vk_discard_texture_initializations(renderer);
    vkr_vk_drain_asset_publications(renderer);
    for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets);
         ++i) {
      if (renderer->retired_window_targets[i].occupied) {
        (void)vkr_vk_window_presents_complete(
            renderer, &renderer->retired_window_targets[i].target, true_v);
        vkr_vk_destroy_window_target(
            renderer, &renderer->retired_window_targets[i].target);
      }
    }
    (void)vkr_vk_window_presents_complete(renderer, &renderer->window_target,
                                          true_v);
    vkr_vk_destroy_window_target(renderer, &renderer->window_target);
    for (uint32_t i = 0; i < ArrayCount(renderer->acquire_semaphores); ++i) {
      if (renderer->acquire_semaphores[i])
        vkDestroySemaphore(device, renderer->acquire_semaphores[i], NULL);
    }
    for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
      if (renderer->retired_targets[i].occupied) {
        vkr_vk_destroy_target_set(renderer,
                                  &renderer->retired_targets[i].targets);
      }
    }
    for (uint32_t i = 0; i < renderer->config.max_graph_images; ++i) {
      if (renderer->graph_images && renderer->graph_images[i].live)
        vkr_vk_destroy_graph_image(renderer, &renderer->graph_images[i]);
    }
    for (uint32_t i = 0; i < renderer->config.max_graph_buffers; ++i) {
      if (renderer->graph_buffers && renderer->graph_buffers[i].live)
        vkr_vk_destroy_graph_buffer(renderer, &renderer->graph_buffers[i]);
    }
    vkr_vk_destroy_target_set(renderer, &renderer->targets);
    vkr_vk_destroy_frame_slots(renderer);
    for (uint32_t i = 0u; i < VKR_VULKAN_PACKET_PIPELINE_COUNT; ++i) {
      if (renderer->packet_pipelines[i])
        vkDestroyPipeline(device, renderer->packet_pipelines[i], NULL);
    }
    for (uint32_t i = 0u; i < VKR_VULKAN_DEFERRED_PIPELINE_COUNT; ++i) {
      if (renderer->deferred_pipelines[i])
        vkDestroyPipeline(device, renderer->deferred_pipelines[i], NULL);
      if (renderer->deferred_shaders[i])
        vkDestroyShaderModule(device, renderer->deferred_shaders[i], NULL);
    }
    for (uint32_t i = 0u; i < VKR_VULKAN_IBL_PIPELINE_COUNT; ++i) {
      if (renderer->ibl_pipelines[i])
        vkDestroyPipeline(device, renderer->ibl_pipelines[i], NULL);
    }
    if (renderer->pipeline_layout) {
      vkDestroyPipelineLayout(device, renderer->pipeline_layout, NULL);
    }
    for (uint32_t i = 0u; i < VKR_VULKAN_PACKET_SHADER_COUNT; ++i) {
      if (renderer->packet_shaders[i])
        vkDestroyShaderModule(device, renderer->packet_shaders[i], NULL);
    }
    for (uint32_t i = 0u; i < VKR_VULKAN_IBL_PIPELINE_COUNT; ++i) {
      if (renderer->ibl_shaders[i])
        vkDestroyShaderModule(device, renderer->ibl_shaders[i], NULL);
    }
    if (renderer->sentinel_sampler) {
      vkDestroySampler(device, renderer->sentinel_sampler, NULL);
    }
    if (renderer->transmission_sampler) {
      vkDestroySampler(device, renderer->transmission_sampler, NULL);
    }
    if (renderer->shadow_comparison_sampler) {
      vkDestroySampler(device, renderer->shadow_comparison_sampler, NULL);
    }
    for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
      if (renderer->published_samplers &&
          renderer->published_samplers[i].sampler)
        vkDestroySampler(device, renderer->published_samplers[i].sampler, NULL);
    }
    for (uint32_t i = 0; i < renderer->retired_staging_buffer_capacity; ++i) {
      if (renderer->retired_staging_buffers &&
          renderer->retired_staging_buffers[i].occupied)
        vkr_vk_destroy_buffer(renderer,
                              &renderer->retired_staging_buffers[i].buffer);
    }
    VkrVulkanGeometryMegabuffer *mega = &renderer->geometry_megabuffer;
    for (uint32_t i = 0u; i < ArrayCount(mega->retired); ++i) {
      if (!mega->retired[i].occupied)
        continue;
      vkr_vk_destroy_buffer(renderer, &mega->retired[i].indices);
      vkr_vk_destroy_buffer(renderer, &mega->retired[i].vertices);
    }
    vkr_vk_destroy_buffer(renderer, &mega->copy_source_indices);
    vkr_vk_destroy_buffer(renderer, &mega->copy_source_vertices);
    vkr_vk_destroy_buffer(renderer, &mega->indices);
    vkr_vk_destroy_buffer(renderer, &mega->vertices);
    *mega = (VkrVulkanGeometryMegabuffer){0};
    vkr_vk_destroy_image(renderer, &renderer->sentinel_image);
    vkr_vk_destroy_buffer(renderer, &renderer->materials);
    vkr_vk_destroy_buffer(renderer, &renderer->upload);
    vkr_vk_destroy_buffer(renderer, &renderer->sampler_descriptors);
    vkr_vk_destroy_buffer(renderer, &renderer->resource_descriptors);
    vkr_vulkan_memory_pool_destroy(renderer->memory_pool);
    renderer->memory_pool = NULL;
    if (renderer->timeline) {
      vkDestroySemaphore(device, renderer->timeline, NULL);
    }
    vkr_vk_pipeline_cache_shutdown(renderer);
  }
  if (renderer->descriptor_scratch) {
    vkr_allocator_free(allocator, renderer->descriptor_scratch,
                       renderer->descriptor_scratch_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_materials) {
    vkr_allocator_free(allocator, renderer->retired_materials,
                       renderer->retired_materials_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_staging_buffers) {
    vkr_allocator_free(allocator, renderer->retired_staging_buffers,
                       renderer->retired_staging_buffers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->pending_texture_initializations) {
    vkr_allocator_free(allocator, renderer->pending_texture_initializations,
                       renderer->pending_texture_initializations_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->pending_buffer_initializations) {
    vkr_allocator_free(allocator, renderer->pending_buffer_initializations,
                       renderer->pending_buffer_initializations_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_materials) {
    vkr_allocator_free(allocator, renderer->published_materials,
                       renderer->published_materials_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_textures) {
    vkr_allocator_free(allocator, renderer->published_textures,
                       renderer->published_textures_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_textures) {
    vkr_allocator_free(allocator, renderer->retired_textures,
                       renderer->retired_textures_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_samplers) {
    vkr_allocator_free(allocator, renderer->published_samplers,
                       renderer->published_samplers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_geometries) {
    vkr_allocator_free(allocator, renderer->published_geometries,
                       renderer->published_geometries_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_geometries) {
    vkr_allocator_free(allocator, renderer->retired_geometries,
                       renderer->retired_geometries_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->sampler_slot_storage) {
    vkr_allocator_free(allocator, renderer->sampler_slot_storage,
                       renderer->sampler_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->storage_image_slot_storage) {
    vkr_allocator_free(allocator, renderer->storage_image_slot_storage,
                       renderer->storage_image_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->material_slot_storage) {
    vkr_allocator_free(allocator, renderer->material_slot_storage,
                       renderer->material_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->sampled_image_slot_storage) {
    vkr_allocator_free(allocator, renderer->sampled_image_slot_storage,
                       renderer->sampled_image_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->capture_storage_memory.base_memory) {
    vkr_dmemory_destroy(&renderer->capture_storage_memory);
  }
  if (renderer->graph_image_barriers) {
    vkr_allocator_free(allocator, renderer->graph_image_barriers,
                       renderer->graph_image_barriers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->graph_buffer_barriers) {
    vkr_allocator_free(allocator, renderer->graph_buffer_barriers,
                       renderer->graph_buffer_barriers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->graph_images) {
    vkr_allocator_free(allocator, renderer->graph_images,
                       renderer->graph_images_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->graph_buffers) {
    vkr_allocator_free(allocator, renderer->graph_buffers,
                       renderer->graph_buffers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  vkr_rg_destroy(renderer->graph);
  arena_destroy(renderer->graph_frame_arena);
  vkr_rg_json_destroy(&renderer->json_graph);
  vkr_rg_executor_registry_destroy(&renderer->executors);
  vkr_vulkan_device_destroy(renderer->device);
  if (renderer->publication_staging_memory.base_memory)
    vkr_dmemory_destroy(&renderer->publication_staging_memory);
  vkr_allocator_free(allocator, renderer, sizeof(*renderer),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

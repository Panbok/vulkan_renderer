#include "vulkan/vkr_vulkan_internal.h"

/* Sparse diffuse volume (DVOL v3) on the desktop pipeline: the frame root's
   texture slots and the composition of the active light layers into the
   volume's SH texture (ADR-054). */

vkr_internal uint32_t vkr_vk_diffuse_volume_rows(uint32_t probe_count) {
  return (probe_count + VKR_DIFFUSE_VOLUME_ROW_PROBES - 1u) /
         VKR_DIFFUSE_VOLUME_ROW_PROBES;
}

/* The published texture of `handle` when it has the format and size the
   volume's layout gives it, else NULL. */
vkr_internal VkrVulkanPublishedTexture *
vkr_vk_diffuse_volume_texture(VkrVulkanRenderer *renderer,
                              VkrTextureHandle handle, VkFormat format,
                              uint32_t width, uint32_t height) {
  VkrVulkanPublishedTexture *texture =
      vkr_vk_published_texture(renderer, handle, NULL);
  if (!texture || texture->initialization_pending ||
      texture->image.format != format || texture->image.width != width ||
      texture->image.height != height || texture->image.mip_levels != 1u ||
      texture->image.array_layers != 1u) {
    log_error("Vulkan diffuse volume texture %u is not usable (published=%u, "
              "pending=%u, format=%u/%u, extent=%ux%u/%ux%u, mips=%u, "
              "layers=%u)",
              handle.id, texture != NULL,
              texture ? texture->initialization_pending : 0u,
              texture ? (uint32_t)texture->image.format : 0u, (uint32_t)format,
              texture ? texture->image.width : 0u,
              texture ? texture->image.height : 0u, width, height,
              texture ? texture->image.mip_levels : 0u,
              texture ? texture->image.array_layers : 0u);
    return NULL;
  }
  return texture;
}

/* Whether every texture of the volume has finished its upload. The runtime
   binds a volume once its publications are confirmed, which precedes the
   GPU completion of their initialization; until then frames light without
   it and do not compose it. */
vkr_internal bool8_t
vkr_vk_diffuse_volume_ready(VkrVulkanRenderer *renderer,
                            const VkrDiffuseVolumeBinding *volume) {
  const VkrTextureHandle handles[5] = {volume->indirection, volume->probes,
                                       volume->moments, volume->layer_sh,
                                       volume->sh};
  for (uint32_t i = 0u; i < ArrayCount(handles); ++i) {
    const VkrVulkanPublishedTexture *texture =
        vkr_vk_published_texture(renderer, handles[i], NULL);
    if (texture && texture->initialization_pending) {
      return false_v;
    }
  }
  return true_v;
}

/* Whether this frame samples baked lamps, so the volume's lamp groups
   compose apart from the rest (VkrDiffuseVolumeBinding). */
vkr_internal bool8_t vkr_vk_diffuse_volume_split(const VkrVulkanFrameSlot *slot) {
  return slot->lightmap_rects != 0u;
}

bool8_t vkr_vk_resolve_diffuse_volume(VkrVulkanRenderer *renderer,
                                      const VkrDiffuseVolumeBinding *volume,
                                      VkrVulkanFrameSlot *slot) {
  if (!vkr_vk_diffuse_volume_ready(renderer, volume))
    return true_v;
  const uint32_t rows = vkr_vk_diffuse_volume_rows(volume->probe_count);
  const uint32_t tile_rows =
      (volume->probe_count + VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES - 1u) /
      VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES;
  if (!vkr_vk_diffuse_volume_texture(
          renderer, volume->indirection, VK_FORMAT_R32_UINT,
          volume->dimensions[0], volume->dimensions[1] * volume->dimensions[2]) ||
      !vkr_vk_diffuse_volume_texture(renderer, volume->probes,
                                     VK_FORMAT_R16G16B16A16_SFLOAT,
                                     VKR_DIFFUSE_VOLUME_ROW_PROBES, rows) ||
      !vkr_vk_diffuse_volume_texture(
          renderer, volume->moments, VK_FORMAT_R16G16_SFLOAT,
          VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES * VKR_DIFFUSE_VOLUME_MOMENT_TILE,
          tile_rows * VKR_DIFFUSE_VOLUME_MOMENT_TILE) ||
      !vkr_vk_diffuse_volume_texture(renderer, volume->sh,
                                     VK_FORMAT_R16G16B16A16_SFLOAT,
                                     VKR_DIFFUSE_VOLUME_ROW_PROBES * 3u,
                                     2u * rows))
    return false_v;
  const VkrTextureHandle handles[4] = {volume->indirection, volume->probes,
                                       volume->moments, volume->sh};
  uint32_t samplers[4] = {0};
  for (uint32_t i = 0u; i < 4u; ++i) {
    if (!vkr_vk_resolve_sampled_pair(renderer, handles[i],
                                     &slot->diffuse_volume_textures[i],
                                     &samplers[i]))
      return false_v;
  }
  slot->diffuse_volume_origin = (Vec4){volume->origin.x, volume->origin.y,
                                       volume->origin.z, volume->spacing};
  /* A frame that samples baked lamps keeps the volume's lamp groups apart,
     for surfaces without a lightmap only. */
  slot->diffuse_volume_params =
      (Vec4){volume->sh_scale, (float32_t)tile_rows,
             vkr_vk_diffuse_volume_split(slot) ? (float32_t)rows : 0.0f, 0.0f};
  slot->diffuse_volume_dimensions[0] = volume->dimensions[0];
  slot->diffuse_volume_dimensions[1] = volume->dimensions[1];
  slot->diffuse_volume_dimensions[2] = volume->dimensions[2];
  /* The moment atlas is the only filtered read. */
  slot->diffuse_volume_dimensions[3] = samplers[2];
  return true_v;
}

bool8_t vkr_vk_prepare_diffuse_volume_compose(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrRgPass *pass) {
  (void)pass;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  prepared->dispatch_count = 0u;
  prepared->image_barrier_count = 0u;
  slot->diffuse_volume_compose_pending = false_v;
  const VkrFrameLighting *lighting = renderer->graph->packet->input.lighting;
  if (!lighting || lighting->diffuse_volume.indirection.id == 0u)
    return true_v;
  const VkrDiffuseVolumeBinding *volume = &lighting->diffuse_volume;
  if (!vkr_vk_diffuse_volume_ready(renderer, volume))
    return true_v;
  const bool8_t split = vkr_vk_diffuse_volume_split(slot);
  if (renderer->diffuse_volume_composed_sh.id == volume->sh.id &&
      renderer->diffuse_volume_composed_sh.generation ==
          volume->sh.generation &&
      renderer->diffuse_volume_composition == volume->composition &&
      renderer->diffuse_volume_composed_split == split)
    return true_v;

  /* The binding names only confirmed publications, so a texture that does
     not match its layout fails the frame rather than leaving the volume's
     SH unwritten. */
  const uint32_t rows = vkr_vk_diffuse_volume_rows(volume->probe_count);
  VkrVulkanPublishedTexture *layer_sh = vkr_vk_diffuse_volume_texture(
      renderer, volume->layer_sh, VK_FORMAT_R16G16B16A16_SFLOAT,
      VKR_DIFFUSE_VOLUME_ROW_PROBES * 3u, rows * volume->band_count);
  VkrVulkanPublishedTexture *sh = vkr_vk_diffuse_volume_texture(
      renderer, volume->sh, VK_FORMAT_R16G16B16A16_SFLOAT,
      VKR_DIFFUSE_VOLUME_ROW_PROBES * 3u, 2u * rows);
  if (!layer_sh || !sh || !sh->storage_slot_count)
    return false_v;
  uint64_t root_address = 0u;
  VkrVulkanDiffuseVolumeComposeRoot *root = vkr_vk_frame_upload_allocate(
      slot, sizeof(*root), _Alignof(VkrVulkanDiffuseVolumeComposeRoot),
      &root_address, NULL);
  if (!root)
    return false_v;
  *root = (VkrVulkanDiffuseVolumeComposeRoot){
      .layer_sh_texture = layer_sh->sampled_slot.index,
      .sh_storage = sh->storage_slots[0].index,
      .probe_count = volume->probe_count,
      .rows = rows,
      .active_layer_count = volume->active_layer_count,
      .lamp_rows = split ? rows : 0u,
      .lamp_mask = volume->active_lamp_mask,
  };
  for (uint32_t i = 0u; i < ArrayCount(root->active_direct); ++i) {
    root->active_direct[i] = VKR_DIFFUSE_VOLUME_NO_BAND;
  }
  for (uint32_t i = 0u; i < volume->active_layer_count; ++i) {
    root->active_layers[i] = volume->active_layers[i];
    root->active_weights[i] = volume->active_weights[i];
    root->active_direct[i] = volume->active_direct_bands[i];
  }

  /* Earlier frames read the texture before this write in queue order; the
     write waits for their lighting and compute stages. */
  prepared->image_barriers[0] = (VkImageMemoryBarrier2){
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = 0u,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = sh->image.handle,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0u,
              .levelCount = 1u,
              .baseArrayLayer = 0u,
              .layerCount = 1u,
          },
  };
  prepared->image_barrier_count = 1u;
  prepared->root_address = root_address;
  prepared->pipelines[0] =
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_DIFFUSE_VOLUME_COMPOSE];
  prepared->groups[0][0] = (volume->probe_count + 63u) / 64u;
  prepared->groups[0][1] = 1u;
  prepared->groups[0][2] = 1u;
  prepared->dispatch_count = 1u;
  layer_sh->last_use_submit_value = renderer->submit_value + 1u;
  sh->last_use_submit_value = renderer->submit_value + 1u;
  slot->diffuse_volume_compose_pending = true_v;
  slot->diffuse_volume_compose_sh = volume->sh;
  slot->diffuse_volume_compose_revision = volume->composition;
  slot->diffuse_volume_compose_split = split;
  slot->diffuse_volume_compose_image = sh->image.handle;
  return true_v;
}

void vkr_vk_record_diffuse_volume_compose(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrVulkanPreparedCompute *prepared) {
  vkr_vk_record_prepared_compute(renderer, command, prepared);
  if (!prepared->dispatch_count)
    return;
  const VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  /* This frame's lighting and every later frame read the composition. */
  const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = slot->diffuse_volume_compose_image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0u,
              .levelCount = 1u,
              .baseArrayLayer = 0u,
              .layerCount = 1u,
          },
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1u,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
}

void vkr_vk_commit_diffuse_volume_compose(VkrVulkanRenderer *renderer,
                                          VkrVulkanFrameSlot *slot) {
  if (!slot->diffuse_volume_compose_pending)
    return;
  renderer->diffuse_volume_composed_sh = slot->diffuse_volume_compose_sh;
  renderer->diffuse_volume_composition = slot->diffuse_volume_compose_revision;
  renderer->diffuse_volume_composed_split = slot->diffuse_volume_compose_split;
  slot->diffuse_volume_compose_pending = false_v;
}

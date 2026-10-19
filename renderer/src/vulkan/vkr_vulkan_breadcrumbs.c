#include "vulkan/vkr_vulkan_internal.h"

/*
 * GPU breadcrumbs (VKR_VULKAN_BREADCRUMBS=1, proposal P3): every graph pass
 * writes a marker when the GPU's command front-end reaches it and another
 * when all its work has finished, into host-visible memory the GPU writes
 * through its caches (VK_AMD_device_coherent_memory). After a device loss or
 * a GPU-completion timeout the host reads them back, so the log names the
 * pass the GPU was inside when it stopped. Each frame slot has its own pair,
 * since the slots' frames run in queue order.
 *
 * A marker packs the frame's submit value (low 16 bits) and the pass index
 * plus one, so zero means no pass of that slot has reached the GPU.
 */

enum {
  VKR_VULKAN_BREADCRUMB_BEGUN = 0,
  VKR_VULKAN_BREADCRUMB_FINISHED = 1,
  VKR_VULKAN_BREADCRUMB_WORDS = 2,
};

vkr_internal uint32_t vkr_vk_breadcrumb_memory_type(VkrVulkanRenderer *renderer,
                                                    uint32_t type_bits) {
  const VkPhysicalDeviceMemoryProperties *memory =
      vkr_vulkan_device_memory_properties(renderer->device);
  const VkMemoryPropertyFlags required =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  /* Uncached as well when the driver offers it: the write then leaves the
     GPU at once instead of when its caches flush. */
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0u; i < memory->memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
    if (!(type_bits & (1u << i)) || (flags & required) != required)
      continue;
    if (flags & VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)
      return i;
    if (fallback == UINT32_MAX)
      fallback = i;
  }
  return fallback;
}

bool8_t vkr_vk_breadcrumbs_create(VkrVulkanRenderer *renderer) {
  renderer->breadcrumbs = (VkrVulkanBreadcrumbs){0};
  if (!vkr_vulkan_device_buffer_marker(renderer->device))
    return true_v;
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)VKR_VULKAN_FRAME_SLOT_COUNT *
              VKR_VULKAN_BREADCRUMB_WORDS * sizeof(uint32_t),
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkrVulkanBreadcrumbs *crumbs = &renderer->breadcrumbs;
  if (vkCreateBuffer(device, &buffer_info, NULL, &crumbs->buffer) != VK_SUCCESS)
    goto fail;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(device, crumbs->buffer, &requirements);
  const uint32_t type =
      vkr_vk_breadcrumb_memory_type(renderer, requirements.memoryTypeBits);
  if (type == UINT32_MAX)
    goto fail;
  /* A few dozen bytes for the renderer's life, outside the pools: its
     memory type exists only for this diagnostic. */
  const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = type,
  };
  void *mapped = NULL;
  if (vkAllocateMemory(device, &allocate_info, NULL, &crumbs->memory) !=
          VK_SUCCESS ||
      vkBindBufferMemory(device, crumbs->buffer, crumbs->memory, 0u) !=
          VK_SUCCESS ||
      vkMapMemory(device, crumbs->memory, 0u, VK_WHOLE_SIZE, 0u, &mapped) !=
          VK_SUCCESS)
    goto fail;
  crumbs->markers = mapped;
  MemZero((void *)crumbs->markers, (uint64_t)buffer_info.size);
  log_info("Vulkan GPU breadcrumbs on: each pass marks its start and end");
  return true_v;

fail:
  log_warn("Vulkan GPU breadcrumbs requested but their coherent buffer could "
           "not be created; running without them");
  vkr_vk_breadcrumbs_destroy(renderer);
  return true_v;
}

void vkr_vk_breadcrumbs_destroy(VkrVulkanRenderer *renderer) {
  VkrVulkanBreadcrumbs *crumbs = &renderer->breadcrumbs;
  const VkDevice device = vkr_vk_renderer_device(renderer);
  if (crumbs->memory && crumbs->markers)
    vkUnmapMemory(device, crumbs->memory);
  if (crumbs->buffer)
    vkDestroyBuffer(device, crumbs->buffer, NULL);
  if (crumbs->memory)
    vkFreeMemory(device, crumbs->memory, NULL);
  *crumbs = (VkrVulkanBreadcrumbs){0};
}

void vkr_vk_breadcrumbs_mark(VkrVulkanRenderer *renderer,
                             VkCommandBuffer command, uint32_t slot_index,
                             uint32_t pass_index, bool8_t finished) {
  const VkrVulkanBreadcrumbs *crumbs = &renderer->breadcrumbs;
  if (!crumbs->markers)
    return;
  const uint32_t word =
      slot_index * VKR_VULKAN_BREADCRUMB_WORDS +
      (finished ? VKR_VULKAN_BREADCRUMB_FINISHED : VKR_VULKAN_BREADCRUMB_BEGUN);
  const uint32_t marker =
      (uint32_t)(((renderer->submit_value + 1u) & 0xffffu) << 16u) |
      (pass_index + 1u);
  /* Top of pipe: the front-end reached the pass. Bottom of pipe: all work
     recorded before it, the pass included, has finished. */
  vkr_vulkan_device_buffer_marker(renderer->device)(
      command,
      finished ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
               : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      crumbs->buffer, (VkDeviceSize)word * sizeof(uint32_t), marker);
}

/* The slot's pass names belong to the frame it recorded last, so a marker
   from an older submission keeps only its index. */
vkr_internal const char *
vkr_vk_breadcrumb_pass_name(const VkrVulkanFrameSlot *slot, uint32_t marker) {
  const uint32_t index = marker & 0xffffu;
  if (index == 0u)
    return "<none>";
  if ((marker >> 16u) != (uint32_t)(slot->retire_value & 0xffffu))
    return "<an older frame's pass>";
  return index - 1u < slot->pass_timing_count
             ? slot->pass_timings[index - 1u].name
             : "<out of range>";
}

void vkr_vk_breadcrumbs_log(VkrVulkanRenderer *renderer) {
  VkrVulkanBreadcrumbs *crumbs = &renderer->breadcrumbs;
  if (!crumbs->markers || crumbs->logged)
    return;
  crumbs->logged = true_v;
  for (uint32_t i = 0u; i < VKR_VULKAN_FRAME_SLOT_COUNT; ++i) {
    const VkrVulkanFrameSlot *slot = &renderer->frame_slots[i];
    const uint32_t begun = crumbs->markers[i * VKR_VULKAN_BREADCRUMB_WORDS +
                                           VKR_VULKAN_BREADCRUMB_BEGUN];
    const uint32_t finished = crumbs->markers[i * VKR_VULKAN_BREADCRUMB_WORDS +
                                              VKR_VULKAN_BREADCRUMB_FINISHED];
    /* Markers count passes from one; the log numbers them from zero, as
       debug.graph does. */
    const uint32_t begun_count = begun & 0xffffu;
    const uint32_t finished_count =
        (finished >> 16u) == (begun >> 16u) ? finished & 0xffffu : 0u;
    if (begun_count == 0u || begun_count <= finished_count) {
      log_error("Vulkan GPU breadcrumbs, frame slot %u (last submit %u): "
                "submit %u finished through pass %u '%s'",
                i, (uint32_t)(slot->retire_value & 0xffffu), finished >> 16u,
                (finished & 0xffffu) ? (finished & 0xffffu) - 1u : 0u,
                vkr_vk_breadcrumb_pass_name(slot, finished));
      continue;
    }
    /* The front-end may run ahead of finished work: the GPU stopped in the
       first pass that has not finished. */
    const uint32_t stuck = (begun & 0xffff0000u) | (finished_count + 1u);
    log_error("Vulkan GPU breadcrumbs, frame slot %u (last submit %u): "
              "submit %u stopped in pass %u '%s' (finished through pass %d, "
              "began up to pass %u '%s')",
              i, (uint32_t)(slot->retire_value & 0xffffu), begun >> 16u,
              finished_count, vkr_vk_breadcrumb_pass_name(slot, stuck),
              (int32_t)finished_count - 1, begun_count - 1u,
              vkr_vk_breadcrumb_pass_name(slot, begun));
  }
}

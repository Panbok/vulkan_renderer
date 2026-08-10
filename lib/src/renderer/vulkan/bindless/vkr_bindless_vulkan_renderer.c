#include "renderer/vulkan/bindless/vkr_bindless_vulkan_renderer.h"

#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_slot_table.h"
#include "renderer/vkr_gpu_submit_ring.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_memory.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_wsi.h"

#include <spirv_reflect.h>
#include <stddef.h>

#ifndef VKR_BINDLESS_VK_VERTEX_SPV
#define VKR_BINDLESS_VK_VERTEX_SPV "walking.vert.spv"
#endif
#ifndef VKR_BINDLESS_VK_FRAGMENT_SPV
#define VKR_BINDLESS_VK_FRAGMENT_SPV "walking.frag.spv"
#endif

enum {
  VKR_BINDLESS_VK_ROOT_OFFSET = 0,
  VKR_BINDLESS_VK_VERTEX_OFFSET = 112,
  VKR_BINDLESS_VK_INDEX_OFFSET = 304,
  VKR_BINDLESS_VK_TEXTURE_OFFSET = 312,
  VKR_BINDLESS_VK_UPLOAD_SIZE = 320,
  VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX = 8,
  VKR_BINDLESS_VK_RETIRED_SWAPCHAIN_MAX = 8,
};

typedef enum VkrBindlessVkMaterialFlag {
  VKR_BINDLESS_VK_MATERIAL_TEXTURE_NORMAL = 1u << 0u,
  VKR_BINDLESS_VK_MATERIAL_TEXTURE_ORM = 1u << 1u,
  VKR_BINDLESS_VK_MATERIAL_TEXTURE_EMISSIVE = 1u << 2u,
} VkrBindlessVkMaterialFlag;

typedef struct VkrBindlessVkDrawRoot {
  uint64_t vertices;
  float32_t tint[4];
  float32_t transform[16];
  uint32_t texture_index;
  uint32_t sampler_index;
  uint64_t materials;
} VkrBindlessVkDrawRoot;

typedef struct VKR_SIMD_ALIGN VkrBindlessVkMaterialGpuRow {
  float32_t tint[4];
  uint32_t base_color_texture;
  uint32_t normal_texture;
  uint32_t orm_texture;
  uint32_t emissive_texture;
  uint32_t base_color_sampler;
  uint32_t normal_sampler;
  uint32_t orm_sampler;
  uint32_t emissive_sampler;
  uint32_t material_id;
  uint32_t flags;
  uint32_t reserved[2];
} VkrBindlessVkMaterialGpuRow;

typedef struct VkrBindlessVkPushConstants {
  uint64_t root;
  uint32_t material_index;
  uint32_t flags;
} VkrBindlessVkPushConstants;

_Static_assert(sizeof(VkrVertex3d) == 64u, "Shared vertex ABI drift");
_Static_assert(sizeof(VkrBindlessVkDrawRoot) == 104u, "Draw-root ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, vertices) == 0u,
               "Draw-root vertices ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, tint) == 8u,
               "Draw-root tint ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, transform) == 24u,
               "Draw-root transform ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, texture_index) == 88u,
               "Draw-root texture ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, sampler_index) == 92u,
               "Draw-root sampler ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, materials) == 96u,
               "Draw-root materials ABI drift");
_Static_assert(sizeof(VkrBindlessVkMaterialGpuRow) == 64u,
               "Vulkan material row ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, base_color_texture) == 16u,
               "Vulkan material texture ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, base_color_sampler) == 32u,
               "Vulkan material sampler ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, material_id) == 48u,
               "Vulkan material identifier ABI drift");
_Static_assert(sizeof(VkrBindlessVkPushConstants) == 16u,
               "Push-constant ABI drift");

typedef struct VkrBindlessVkAllocation {
  VkDeviceMemory memory;
  VkDeviceSize memory_size;
  VkDeviceSize offset;
  void *mapped;
  uint32_t memory_type_index;
  VkMemoryPropertyFlags properties;
  VkrBindlessVkPooledAllocation pooled_allocation;
  VkrBindlessVkMemoryPoolKey pool_key;
  bool8_t pooled;
  bool8_t dedicated;
  bool8_t retired;
} VkrBindlessVkAllocation;

typedef struct VkrBindlessVkBuffer {
  VkBuffer handle;
  VkrBindlessVkAllocation allocation;
  VkDeviceAddress address;
  VkDeviceSize size;
} VkrBindlessVkBuffer;

typedef struct VkrBindlessVkImage {
  VkImage handle;
  VkImageView view;
  VkrBindlessVkAllocation allocation;
  VkImageLayout layout;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels;
  uint32_t array_layers;
  VkFormat format;
} VkrBindlessVkImage;

typedef struct VkrBindlessVkDirtyRange {
  VkDeviceSize offset;
  VkDeviceSize end;
  bool8_t dirty;
} VkrBindlessVkDirtyRange;

typedef struct VkrBindlessVkTargetSet {
  VkrBindlessVkImage images[VKR_BINDLESS_VK_TARGET_IMAGE_MAX];
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
} VkrBindlessVkTargetSet;

typedef struct VkrBindlessVkRetiredTargetSet {
  VkrBindlessVkTargetSet targets;
  uint64_t retire_value;
  bool8_t occupied;
} VkrBindlessVkRetiredTargetSet;

typedef struct VkrBindlessVkFrameSlot {
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkrBindlessVkBuffer readback;
  uint64_t retire_value;
  uint64_t source_frame_index;
  uint32_t image_index;
  bool8_t acquired_window_image;
} VkrBindlessVkFrameSlot;

typedef struct VkrBindlessVkWindowTarget {
  VkSwapchainKHR swapchain;
  VkImage images[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  VkSemaphore render_complete[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  uint64_t image_last_submit_value[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  bool8_t image_presented[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  VkrBindlessVulkanReacquireState reacquire_state;
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
  VkFormat format;
  VkColorSpaceKHR color_space;
  VkPresentModeKHR present_mode;
  bool8_t occupied;
} VkrBindlessVkWindowTarget;

typedef struct VkrBindlessVkRetiredWindowTarget {
  VkrBindlessVkWindowTarget target;
  bool8_t occupied;
} VkrBindlessVkRetiredWindowTarget;

typedef struct VkrBindlessVkPublishedTexture {
  VkrTextureHandle handle;
  VkrBindlessVkImage image;
  VkrGpuSlotHandle sampled_slot;
  VkrGpuSlotHandle storage_slot;
  uint32_t sampler_record_index;
  uint32_t material_reference_count;
  uint64_t last_use_submit_value;
  bool8_t has_storage_slot;
  bool8_t initialization_pending;
  bool8_t live;
  bool8_t pending_retire;
} VkrBindlessVkPublishedTexture;

typedef struct VkrBindlessVkPublishedSampler {
  VkrTextureDescription description;
  VkSampler sampler;
  VkrGpuSlotHandle slot;
  uint64_t last_use_submit_value;
  uint32_t mip_levels;
  uint32_t reference_count;
  bool8_t live;
  bool8_t pending_retire;
} VkrBindlessVkPublishedSampler;

typedef struct VkrBindlessVkPendingTextureInitialization {
  VkrBindlessVkBuffer staging;
  VkBufferImageCopy2 *regions;
  uint64_t regions_size;
  uint32_t texture_record_index;
  uint32_t region_count;
  bool8_t writable;
} VkrBindlessVkPendingTextureInitialization;

typedef struct VkrBindlessVkPendingBufferInitialization {
  VkrBindlessVkBuffer staging;
  VkBuffer destination;
  VkDeviceSize size;
  VkPipelineStageFlags2 destination_stage;
  VkAccessFlags2 destination_access;
  uint32_t geometry_record_index;
} VkrBindlessVkPendingBufferInitialization;

typedef struct VkrBindlessVkRetiredStagingBuffer {
  VkrBindlessVkBuffer buffer;
  uint64_t retire_value;
  bool8_t occupied;
} VkrBindlessVkRetiredStagingBuffer;

typedef struct VkrBindlessVkPublishedGeometry {
  VkrGeometryHandle handle;
  VkrBindlessVkBuffer vertices;
  VkrBindlessVkBuffer indices;
  uint32_t vertex_count;
  uint32_t index_count;
  VkIndexType index_type;
  uint64_t last_use_submit_value;
  uint32_t pending_initialization_count;
  bool8_t live;
  bool8_t pending_retire;
} VkrBindlessVkPublishedGeometry;

typedef struct VkrBindlessVkPublishedMaterial {
  VkrMaterialHandle handle;
  VkrGpuSlotHandle slot;
  VkrBindlessVkMaterialGpuRow row;
  uint32_t texture_record_indices[4];
  bool8_t live;
} VkrBindlessVkPublishedMaterial;

typedef struct VkrBindlessVkRetiredMaterial {
  uint32_t texture_record_indices[4];
  uint64_t retire_value;
  bool8_t occupied;
} VkrBindlessVkRetiredMaterial;

struct VkrBindlessVulkanRenderer {
  VkrAllocator *allocator;
  VkrBindlessVulkanRendererConfig config;
  VkrBindlessVulkanDevice *device;
  VkrBindlessVkTargetSet targets;
  VkrBindlessVkRetiredTargetSet retired_targets[4];
  VkrBindlessVkWindowTarget window_target;
  VkrBindlessVkRetiredWindowTarget
      retired_window_targets[VKR_BINDLESS_VK_RETIRED_SWAPCHAIN_MAX];
  VkSemaphore acquire_semaphores[VKR_BINDLESS_VK_FRAME_SLOT_COUNT];
  VkrBindlessVkFrameSlot frame_slots[VKR_BINDLESS_VK_FRAME_SLOT_COUNT];
  VkrGpuSubmitRing command_ring;
  VkrGpuSubmitRingSlot command_ring_slots[VKR_BINDLESS_VK_FRAME_SLOT_COUNT];
  VkrGpuRingSlice active_command_slice;
  VkrBindlessVkMemoryPoolManager *memory_pool;
  VkrBindlessVkBuffer resource_descriptors;
  VkrBindlessVkBuffer sampler_descriptors;
  VkrGpuSlotTable *sampled_image_slots;
  VkrGpuSlotTable *storage_image_slots;
  VkrGpuSlotTable *sampler_slots;
  VkrGpuSlotTable *material_slots;
  VkrBindlessVkPublishedGeometry *published_geometries;
  VkrBindlessVkPublishedTexture *published_textures;
  VkrBindlessVkPublishedSampler *published_samplers;
  VkrBindlessVkPublishedMaterial *published_materials;
  VkrBindlessVkRetiredMaterial *retired_materials;
  VkrBindlessVkPendingTextureInitialization *pending_texture_initializations;
  VkrBindlessVkPendingBufferInitialization *pending_buffer_initializations;
  VkrBindlessVkRetiredStagingBuffer *retired_staging_buffers;
  uint64_t published_geometries_size;
  uint64_t published_textures_size;
  uint64_t published_samplers_size;
  uint64_t published_materials_size;
  uint64_t retired_materials_size;
  uint64_t pending_texture_initializations_size;
  uint64_t pending_buffer_initializations_size;
  uint64_t retired_staging_buffers_size;
  uint32_t pending_texture_initialization_count;
  uint32_t pending_buffer_initialization_count;
  uint32_t pending_buffer_initialization_capacity;
  uint32_t retired_staging_buffer_capacity;
  uint32_t staging_buffer_count;
  void *sampled_image_slot_storage;
  void *storage_image_slot_storage;
  void *sampler_slot_storage;
  void *material_slot_storage;
  uint64_t sampled_image_slot_storage_size;
  uint64_t storage_image_slot_storage_size;
  uint64_t sampler_slot_storage_size;
  uint64_t material_slot_storage_size;
  uint8_t *descriptor_scratch;
  uint32_t descriptor_scratch_size;
  VkrBindlessVkDirtyRange resource_descriptor_dirty;
  VkrBindlessVkDirtyRange sampler_descriptor_dirty;
  VkrBindlessVkDirtyRange material_dirty;
  VkrBindlessVkBuffer upload;
  VkrBindlessVkBuffer materials;
  VkrBindlessVkImage sentinel_image;
  VkSampler sentinel_sampler;
  VkShaderModule vertex_shader;
  VkShaderModule fragment_shader;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkSemaphore timeline;
  uint64_t submit_value;
  uint64_t completed_value;
  uint64_t upload_wait_count;
  uint64_t command_slot_wait_count;
  uint64_t wsi_reacquire_proofs;
  uint64_t wsi_retired_swapchains;
  uint64_t wsi_retired_swapchains_collected;
  uint32_t active_frame_slot;
  uint32_t next_image_index;
  uint32_t active_material_index;
  uint32_t active_geometry_record_index;
  bool8_t active_geometry;
  bool8_t frame_active;
  bool8_t sentinel_uploaded;
  bool8_t shader_abi_validated;
  bool8_t target_dirty;
  bool8_t terminal_failure;
};

static bool8_t
vkr_bindless_vk_recreate_window_target(VkrBindlessVulkanRenderer *renderer,
                                       uint32_t width, uint32_t height,
                                       uint32_t image_count);
static void
vkr_bindless_vk_collect_asset_publications(VkrBindlessVulkanRenderer *renderer,
                                           uint64_t completed);

static VkDevice
vkr_bindless_vk_renderer_device(const VkrBindlessVulkanRenderer *renderer) {
  return vkr_bindless_vulkan_device_handle(renderer->device);
}

static bool8_t vkr_bindless_vk_choose_memory_type(
    const VkrBindlessVulkanRenderer *renderer, uint32_t memory_type_bits,
    VkrBindlessVkMemoryClass memory_class, uint32_t *out_index,
    VkMemoryPropertyFlags *out_properties) {
  const VkPhysicalDeviceMemoryProperties *memory =
      vkr_bindless_vulkan_device_memory_properties(renderer->device);
  int32_t best_rank = INT32_MAX;
  uint32_t best_index = UINT32_MAX;
  for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags available =
        memory->memoryTypes[i].propertyFlags;
    if (!(memory_type_bits & (1u << i)))
      continue;
    const int32_t rank =
        vkr_bindless_vulkan_memory_type_rank(memory_class, available);
    if (rank >= 0 && rank < best_rank) {
      best_rank = rank;
      best_index = i;
    }
  }
  if (best_index == UINT32_MAX)
    return false_v;
  *out_index = best_index;
  *out_properties = memory->memoryTypes[best_index].propertyFlags;
  if (memory_class == VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE && best_rank == 2)
    log_warn("Bindless Vulkan DEVICE placement degraded to memory type %u",
             best_index);
  return true_v;
}

static VkFormat vkr_bindless_vk_texture_format(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
    return VK_FORMAT_B8G8R8A8_SRGB;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UINT:
    return VK_FORMAT_R8G8B8A8_UINT;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM:
    return VK_FORMAT_R8G8B8A8_SNORM;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SINT:
    return VK_FORMAT_R8G8B8A8_SINT;
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
    return VK_FORMAT_BC7_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    return VK_FORMAT_BC7_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_BC5_UNORM:
    return VK_FORMAT_BC5_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM:
    return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB:
    return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
    return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM:
    return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return VK_FORMAT_R8_UNORM;
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
    return VK_FORMAT_R16_SFLOAT;
  case VKR_TEXTURE_FORMAT_R32_SFLOAT:
    return VK_FORMAT_R32_SFLOAT;
  case VKR_TEXTURE_FORMAT_R32_UINT:
    return VK_FORMAT_R32_UINT;
  case VKR_TEXTURE_FORMAT_R8G8_UNORM:
    return VK_FORMAT_R8G8_UNORM;
  case VKR_TEXTURE_FORMAT_D16_UNORM:
    return VK_FORMAT_D16_UNORM;
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return VK_FORMAT_D32_SFLOAT;
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
    return VK_FORMAT_D24_UNORM_S8_UINT;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

static bool8_t vkr_bindless_vk_flush(const VkrBindlessVulkanRenderer *renderer,
                                     const VkrBindlessVkAllocation *allocation,
                                     VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true_v;
  }
  const VkDeviceSize atom =
      vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.nonCoherentAtomSize;
  VkrBindlessVkMappedRange aligned = {0};
  if (offset > UINT64_MAX - allocation->offset ||
      !vkr_bindless_vulkan_noncoherent_range(allocation->offset + offset, size,
                                             allocation->memory_size, atom,
                                             &aligned))
    return false_v;
  const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = aligned.offset,
      .size = aligned.size,
  };
  return vkFlushMappedMemoryRanges(vkr_bindless_vk_renderer_device(renderer),
                                   1u, &range) == VK_SUCCESS;
}

static bool8_t vkr_bindless_vk_mark_dirty(VkrBindlessVkDirtyRange *dirty,
                                          const VkrBindlessVkBuffer *buffer,
                                          VkDeviceSize offset,
                                          VkDeviceSize size) {
  if (!dirty || !buffer || !size || offset > buffer->size ||
      size > buffer->size - offset)
    return false_v;
  const VkDeviceSize end = offset + size;
  if (!dirty->dirty) {
    *dirty = (VkrBindlessVkDirtyRange){
        .offset = offset,
        .end = end,
        .dirty = true_v,
    };
  } else {
    dirty->offset = Min(dirty->offset, offset);
    dirty->end = Max(dirty->end, end);
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_flush_publication_ranges(VkrBindlessVulkanRenderer *renderer) {
  VkrBindlessVkDirtyRange *ranges[] = {
      &renderer->resource_descriptor_dirty,
      &renderer->sampler_descriptor_dirty,
      &renderer->material_dirty,
  };
  VkrBindlessVkBuffer *buffers[] = {
      &renderer->resource_descriptors,
      &renderer->sampler_descriptors,
      &renderer->materials,
  };
  for (uint32_t i = 0; i < ArrayCount(ranges); ++i) {
    VkrBindlessVkDirtyRange *range = ranges[i];
    if (range->dirty &&
        !vkr_bindless_vk_flush(renderer, &buffers[i]->allocation, range->offset,
                               range->end - range->offset))
      return false_v;
  }
  for (uint32_t i = 0; i < ArrayCount(ranges); ++i)
    MemZero(ranges[i], sizeof(*ranges[i]));
  return true_v;
}

static bool8_t
vkr_bindless_vk_invalidate(const VkrBindlessVulkanRenderer *renderer,
                           const VkrBindlessVkAllocation *allocation,
                           VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true_v;
  }
  const VkDeviceSize atom =
      vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.nonCoherentAtomSize;
  VkrBindlessVkMappedRange aligned = {0};
  if (offset > UINT64_MAX - allocation->offset ||
      !vkr_bindless_vulkan_noncoherent_range(allocation->offset + offset, size,
                                             allocation->memory_size, atom,
                                             &aligned))
    return false_v;
  const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = aligned.offset,
      .size = aligned.size,
  };
  return vkInvalidateMappedMemoryRanges(
             vkr_bindless_vk_renderer_device(renderer), 1u, &range) ==
         VK_SUCCESS;
}

static bool8_t
vkr_bindless_vk_release_allocation(VkrBindlessVulkanRenderer *renderer,
                                   VkrBindlessVkAllocation *allocation) {
  if (allocation->pooled)
    return vkr_bindless_vulkan_memory_pool_release(
        renderer->memory_pool, &allocation->pooled_allocation,
        renderer->completed_value, renderer->completed_value);
  if (!allocation->memory)
    return true_v;

  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (allocation->mapped)
    vkUnmapMemory(device, allocation->memory);
  vkFreeMemory(device, allocation->memory, NULL);
  if (allocation->dedicated)
    vkr_bindless_vulkan_memory_pool_record_dedicated_release(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size,
        allocation->retired);
  return true_v;
}

static void vkr_bindless_vk_destroy_buffer(VkrBindlessVulkanRenderer *renderer,
                                           VkrBindlessVkBuffer *buffer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (buffer->handle)
    vkDestroyBuffer(device, buffer->handle, NULL);
  if (!vkr_bindless_vk_release_allocation(renderer, &buffer->allocation))
    log_error("Bindless Vulkan failed to release a proven buffer placement");
  MemZero(buffer, sizeof(*buffer));
}

static bool8_t
vkr_bindless_vk_retire_allocation(VkrBindlessVulkanRenderer *renderer,
                                  VkrBindlessVkAllocation *allocation,
                                  uint64_t retire_value) {
  if (!allocation || allocation->retired)
    return false_v;
  if (allocation->pooled) {
    if (!vkr_bindless_vulkan_memory_pool_retire(renderer->memory_pool,
                                                &allocation->pooled_allocation,
                                                retire_value))
      return false_v;
  } else if (allocation->dedicated) {
    if (!vkr_bindless_vulkan_memory_pool_record_dedicated_retire(
            renderer->memory_pool, allocation->pool_key,
            allocation->memory_size))
      return false_v;
  } else {
    return false_v;
  }
  allocation->retired = true_v;
  return true_v;
}

static bool8_t
vkr_bindless_vk_retire_buffer(VkrBindlessVulkanRenderer *renderer,
                              VkrBindlessVkBuffer *buffer,
                              uint64_t retire_value) {
  return buffer && buffer->handle &&
         vkr_bindless_vk_retire_allocation(renderer, &buffer->allocation,
                                           retire_value);
}

static bool8_t
vkr_bindless_vk_create_buffer(VkrBindlessVulkanRenderer *renderer,
                              VkrBindlessVkMemoryClass memory_class,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkrBindlessVkBuffer *out_buffer) {
  MemZero(out_buffer, sizeof(*out_buffer));
  out_buffer->size = size;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  const VkDeviceBufferMemoryRequirements device_requirements = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pCreateInfo = &buffer_info,
  };
  vkGetDeviceBufferMemoryRequirements(device, &device_requirements,
                                      &requirements);
  VkrBindlessVkAllocation *allocation = &out_buffer->allocation;
  if (!vkr_bindless_vk_choose_memory_type(
          renderer, requirements.memoryRequirements.memoryTypeBits,
          memory_class, &allocation->memory_type_index,
          &allocation->properties)) {
    return false_v;
  }
  const bool8_t has_address =
      (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
  allocation->pool_key = (VkrBindlessVkMemoryPoolKey){
      .memory_class = memory_class,
      .kind = VKR_BINDLESS_VK_MEMORY_KIND_BUFFER,
      .memory_type_index = allocation->memory_type_index,
      .device_address_required = has_address,
  };
  const bool8_t dedicated =
      dedicated_requirements.requiresDedicatedAllocation ||
      dedicated_requirements.prefersDedicatedAllocation;
  if (!dedicated &&
      !vkr_bindless_vulkan_memory_pool_allocate(
          renderer->memory_pool, allocation->pool_key, allocation->properties,
          requirements.memoryRequirements.size,
          requirements.memoryRequirements.alignment,
          &allocation->pooled_allocation))
    return false_v;
  if (vkCreateBuffer(device, &buffer_info, NULL, &out_buffer->handle) !=
      VK_SUCCESS) {
    if (allocation->pooled_allocation.valid)
      (void)vkr_bindless_vulkan_memory_pool_release(
          renderer->memory_pool, &allocation->pooled_allocation,
          renderer->completed_value, renderer->completed_value);
    return false_v;
  }
  if (dedicated) {
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = out_buffer->handle,
    };
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = has_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u,
        .pNext = &dedicated_info,
    };
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext =
            has_address ? (const void *)&flags : (const void *)&dedicated_info,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = allocation->memory_type_index,
    };
    allocation->memory_size = requirements.memoryRequirements.size;
    allocation->dedicated = true_v;
    if (vkAllocateMemory(device, &allocate_info, NULL, &allocation->memory) !=
        VK_SUCCESS) {
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
    vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size);
    if ((allocation->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        memory_class != VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE &&
        vkMapMemory(device, allocation->memory, 0u, allocation->memory_size, 0u,
                    &allocation->mapped) != VK_SUCCESS) {
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
  } else {
    allocation->pooled = true_v;
    allocation->memory = allocation->pooled_allocation.memory;
    allocation->memory_size = allocation->pooled_allocation.memory_size;
    allocation->offset = allocation->pooled_allocation.offset;
    allocation->mapped = allocation->pooled_allocation.mapped;
  }
  if (vkBindBufferMemory(device, out_buffer->handle, allocation->memory,
                         allocation->offset) != VK_SUCCESS) {
    vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
    return false_v;
  }
  if (has_address) {
    const VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = out_buffer->handle,
    };
    out_buffer->address = vkGetBufferDeviceAddress(device, &address_info);
    if (!out_buffer->address) {
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_create_upload_buffers(VkrBindlessVulkanRenderer *renderer) {
  if (!vkr_gpu_abi_validate_host()) {
    log_error("Bindless Vulkan shared host ABI validation failed");
    return false_v;
  }
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  return vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             resource_layout->size,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             &renderer->resource_descriptors) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             sampler_layout->size,
             VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             &renderer->sampler_descriptors) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             VKR_BINDLESS_VK_UPLOAD_SIZE,
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
             &renderer->upload) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             (VkDeviceSize)renderer->config.material_slot_capacity *
                 sizeof(VkrBindlessVkMaterialGpuRow),
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, &renderer->materials);
}

static void vkr_bindless_vk_destroy_image(VkrBindlessVulkanRenderer *renderer,
                                          VkrBindlessVkImage *image) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (image->view)
    vkDestroyImageView(device, image->view, NULL);
  if (image->handle)
    vkDestroyImage(device, image->handle, NULL);
  if (!vkr_bindless_vk_release_allocation(renderer, &image->allocation))
    log_error("Bindless Vulkan failed to release a proven image placement");
  MemZero(image, sizeof(*image));
}

static bool8_t vkr_bindless_vk_create_image_ex(
    VkrBindlessVulkanRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t mip_levels, uint32_t array_layers, VkFormat format,
    VkImageCreateFlags flags, VkImageViewType view_type,
    VkImageUsageFlags usage, VkrBindlessVkImage *out_image) {
  if (!width || !height || !mip_levels || !array_layers ||
      format == VK_FORMAT_UNDEFINED)
    return false_v;
  MemZero(out_image, sizeof(*out_image));
  out_image->width = width;
  out_image->height = height;
  out_image->mip_levels = mip_levels;
  out_image->array_layers = array_layers;
  out_image->format = format;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = flags,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {.width = width, .height = height, .depth = 1u},
      .mipLevels = mip_levels,
      .arrayLayers = array_layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  const VkDeviceImageMemoryRequirements device_requirements = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
      .pCreateInfo = &image_info,
  };
  vkGetDeviceImageMemoryRequirements(device, &device_requirements,
                                     &requirements);
  VkrBindlessVkAllocation *allocation = &out_image->allocation;
  if (!vkr_bindless_vk_choose_memory_type(
          renderer, requirements.memoryRequirements.memoryTypeBits,
          VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE, &allocation->memory_type_index,
          &allocation->properties)) {
    return false_v;
  }
  allocation->pool_key = (VkrBindlessVkMemoryPoolKey){
      .memory_class = VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE,
      .kind = VKR_BINDLESS_VK_MEMORY_KIND_IMAGE,
      .memory_type_index = allocation->memory_type_index,
  };
  const bool8_t dedicated =
      dedicated_requirements.requiresDedicatedAllocation ||
      dedicated_requirements.prefersDedicatedAllocation;
  if (!dedicated &&
      !vkr_bindless_vulkan_memory_pool_allocate(
          renderer->memory_pool, allocation->pool_key, allocation->properties,
          requirements.memoryRequirements.size,
          requirements.memoryRequirements.alignment,
          &allocation->pooled_allocation))
    return false_v;
  if (vkCreateImage(device, &image_info, NULL, &out_image->handle) !=
      VK_SUCCESS) {
    if (allocation->pooled_allocation.valid)
      (void)vkr_bindless_vulkan_memory_pool_release(
          renderer->memory_pool, &allocation->pooled_allocation,
          renderer->completed_value, renderer->completed_value);
    return false_v;
  }
  if (dedicated) {
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = out_image->handle,
    };
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = allocation->memory_type_index,
    };
    allocation->memory_size = requirements.memoryRequirements.size;
    allocation->dedicated = true_v;
    if (vkAllocateMemory(device, &allocate_info, NULL, &allocation->memory) !=
        VK_SUCCESS) {
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_image(renderer, out_image);
      return false_v;
    }
    vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size);
  } else {
    allocation->pooled = true_v;
    allocation->memory = allocation->pooled_allocation.memory;
    allocation->memory_size = allocation->pooled_allocation.memory_size;
    allocation->offset = allocation->pooled_allocation.offset;
  }
  if (vkBindImageMemory(device, out_image->handle, allocation->memory,
                        allocation->offset) != VK_SUCCESS) {
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out_image->handle,
      .viewType = view_type,
      .format = format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = mip_levels,
                           .layerCount = array_layers},
  };
  if (vkCreateImageView(device, &view_info, NULL, &out_image->view) !=
      VK_SUCCESS) {
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_bindless_vk_create_image(VkrBindlessVulkanRenderer *renderer,
                                            uint32_t width, uint32_t height,
                                            VkImageUsageFlags usage,
                                            VkrBindlessVkImage *out_image) {
  return vkr_bindless_vk_create_image_ex(
      renderer, width, height, 1u, 1u, VK_FORMAT_R8G8B8A8_UNORM, 0u,
      VK_IMAGE_VIEW_TYPE_2D, usage, out_image);
}

static void
vkr_bindless_vk_destroy_target_set(VkrBindlessVulkanRenderer *renderer,
                                   VkrBindlessVkTargetSet *targets) {
  for (uint32_t i = 0; i < targets->image_count; ++i) {
    vkr_bindless_vk_destroy_image(renderer, &targets->images[i]);
  }
  MemZero(targets, sizeof(*targets));
}

static bool8_t vkr_bindless_vk_create_target_set(
    VkrBindlessVulkanRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t image_count, VkrBindlessVkTargetSet *out_targets) {
  if (!width || !height || !image_count ||
      image_count > VKR_BINDLESS_VK_TARGET_IMAGE_MAX) {
    return false_v;
  }
  MemZero(out_targets, sizeof(*out_targets));
  out_targets->width = width;
  out_targets->height = height;
  out_targets->image_count = image_count;
  for (uint32_t i = 0; i < image_count; ++i) {
    if (!vkr_bindless_vk_create_image(renderer, width, height,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      &out_targets->images[i])) {
      vkr_bindless_vk_destroy_target_set(renderer, out_targets);
      return false_v;
    }
  }
  return true_v;
}

static void
vkr_bindless_vk_destroy_window_target(VkrBindlessVulkanRenderer *renderer,
                                      VkrBindlessVkWindowTarget *target) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t i = 0; i < target->image_count; ++i) {
    if (target->render_complete[i])
      vkDestroySemaphore(device, target->render_complete[i], NULL);
  }
  if (target->swapchain)
    vkDestroySwapchainKHR(device, target->swapchain, NULL);
  MemZero(target, sizeof(*target));
}

static void vkr_bindless_vk_collect_retired_window_targets(
    VkrBindlessVulkanRenderer *renderer) {
  if (!renderer->window_target.reacquire_state.successor_present_complete)
    return;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    VkrBindlessVkRetiredWindowTarget *retired =
        &renderer->retired_window_targets[i];
    if (retired->occupied) {
      vkr_bindless_vk_destroy_window_target(renderer, &retired->target);
      retired->occupied = false_v;
      renderer->wsi_retired_swapchains_collected++;
    }
  }
}

static VkSurfaceFormatKHR
vkr_bindless_vk_choose_surface_format(const VkSurfaceFormatKHR *formats,
                                      uint32_t count) {
  if (count == 1u && formats[0].format == VK_FORMAT_UNDEFINED)
    return (VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_SRGB,
                                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  for (uint32_t i = 0; i < count; ++i) {
    if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
        formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return formats[i];
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB &&
        formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return formats[i];
  }
  return formats[0];
}

static VkPresentModeKHR
vkr_bindless_vk_choose_present_mode(const VkPresentModeKHR *modes,
                                    uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
      return modes[i];
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

static bool8_t vkr_bindless_vk_create_window_target(
    VkrBindlessVulkanRenderer *renderer, uint32_t requested_width,
    uint32_t requested_height, uint32_t requested_image_count,
    VkSwapchainKHR old_swapchain, VkrBindlessVkWindowTarget *out_target) {
  VkPhysicalDevice physical =
      vkr_bindless_vulkan_device_physical(renderer->device);
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkSurfaceKHR surface = vkr_bindless_vulkan_device_surface(renderer->device);
  VkSurfaceCapabilitiesKHR capabilities = {0};
  uint32_t format_count = 0, mode_count = 0;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface,
                                                &capabilities) != VK_SUCCESS ||
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
                                           NULL) != VK_SUCCESS ||
      !format_count || format_count > 64u ||
      vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count,
                                                NULL) != VK_SUCCESS ||
      !mode_count || mode_count > 64u)
    return false_v;
  VkSurfaceFormatKHR formats[64];
  VkPresentModeKHR modes[64];
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
                                           formats) != VK_SUCCESS ||
      vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count,
                                                modes) != VK_SUCCESS)
    return false_v;

  const VkSurfaceFormatKHR surface_format =
      vkr_bindless_vk_choose_surface_format(formats, format_count);
  VkFormatProperties source_properties = {0}, target_properties = {0};
  vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                      &source_properties);
  vkGetPhysicalDeviceFormatProperties(physical, surface_format.format,
                                      &target_properties);
  if ((source_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0u ||
      (target_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0u)
    return false_v;
  VkExtent2D extent = capabilities.currentExtent;
  if (extent.width == UINT32_MAX) {
    extent.width = Clamp(requested_width, capabilities.minImageExtent.width,
                         capabilities.maxImageExtent.width);
    extent.height = Clamp(requested_height, capabilities.minImageExtent.height,
                          capabilities.maxImageExtent.height);
  }
  if (!extent.width || !extent.height)
    return false_v;
  if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ==
      0u)
    return false_v;
  uint32_t minimum_count =
      Max(requested_image_count, capabilities.minImageCount);
  if (capabilities.maxImageCount)
    minimum_count = Min(minimum_count, capabilities.maxImageCount);
  VkCompositeAlphaFlagBitsKHR composite_alpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  const VkCompositeAlphaFlagBitsKHR composite_candidates[] = {
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
  };
  for (uint32_t i = 0; i < ArrayCount(composite_candidates); ++i) {
    if (capabilities.supportedCompositeAlpha & composite_candidates[i]) {
      composite_alpha = composite_candidates[i];
      break;
    }
  }
  VkSwapchainCreateInfoKHR create_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = minimum_count,
      .imageFormat = surface_format.format,
      .imageColorSpace = surface_format.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1u,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = composite_alpha,
      .presentMode = vkr_bindless_vk_choose_present_mode(modes, mode_count),
      .clipped = VK_TRUE,
      .oldSwapchain = old_swapchain,
  };
  MemZero(out_target, sizeof(*out_target));
  if (vkCreateSwapchainKHR(device, &create_info, NULL,
                           &out_target->swapchain) != VK_SUCCESS)
    return false_v;
  uint32_t actual_count = 0;
  if (vkGetSwapchainImagesKHR(device, out_target->swapchain, &actual_count,
                              NULL) != VK_SUCCESS ||
      !actual_count || actual_count > VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX) {
    vkr_bindless_vk_destroy_window_target(renderer, out_target);
    return false_v;
  }
  out_target->image_count = actual_count;
  if (vkGetSwapchainImagesKHR(device, out_target->swapchain, &actual_count,
                              out_target->images) != VK_SUCCESS) {
    vkr_bindless_vk_destroy_window_target(renderer, out_target);
    return false_v;
  }
  out_target->width = extent.width;
  out_target->height = extent.height;
  out_target->format = surface_format.format;
  out_target->color_space = surface_format.colorSpace;
  out_target->present_mode = create_info.presentMode;
  for (uint32_t i = 0; i < actual_count; ++i) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device, &semaphore_info, NULL,
                          &out_target->render_complete[i]) != VK_SUCCESS) {
      vkr_bindless_vk_destroy_window_target(renderer, out_target);
      return false_v;
    }
  }
  out_target->occupied = true_v;
  return true_v;
}

static bool8_t
vkr_bindless_vk_create_acquire_semaphores(VkrBindlessVulkanRenderer *renderer) {
  VkSemaphoreCreateInfo info = {.sType =
                                    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < ArrayCount(renderer->acquire_semaphores); ++i) {
    if (vkCreateSemaphore(vkr_bindless_vk_renderer_device(renderer), &info,
                          NULL, &renderer->acquire_semaphores[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}

static void
vkr_bindless_vk_collect_retired_targets(VkrBindlessVulkanRenderer *renderer,
                                        uint64_t completed_value) {
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
    VkrBindlessVkRetiredTargetSet *retired = &renderer->retired_targets[i];
    if (retired->occupied && retired->retire_value <= completed_value) {
      vkr_bindless_vk_destroy_target_set(renderer, &retired->targets);
      MemZero(retired, sizeof(*retired));
    }
  }
}

static bool8_t
vkr_bindless_vk_create_frame_slots(VkrBindlessVulkanRenderer *renderer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  const VkDeviceSize readback_size = 4u;
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex =
            vkr_bindless_vulkan_device_queue_family(renderer->device),
    };
    if (vkCreateCommandPool(device, &pool_info, NULL, &slot->command_pool) !=
        VK_SUCCESS) {
      return false_v;
    }
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = slot->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
    };
    if (vkAllocateCommandBuffers(device, &command_info,
                                 &slot->command_buffer) != VK_SUCCESS ||
        !vkr_bindless_vk_create_buffer(
            renderer, VKR_BINDLESS_VK_MEMORY_CLASS_READBACK, readback_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slot->readback)) {
      return false_v;
    }
  }
  return true_v;
}

static void
vkr_bindless_vk_destroy_frame_slots(VkrBindlessVulkanRenderer *renderer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    vkr_bindless_vk_destroy_buffer(renderer, &slot->readback);
    if (slot->command_pool) {
      vkDestroyCommandPool(device, slot->command_pool, NULL);
    }
    MemZero(slot, sizeof(*slot));
  }
}

static bool8_t
vkr_bindless_vk_write_upload_data(VkrBindlessVulkanRenderer *renderer) {
  uint8_t *mapped = renderer->upload.allocation.mapped;
  MemZero(mapped, renderer->upload.size);
  const VkrVertex3d vertices[] = {
      {.position = {-1.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {3.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 2.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {-1.0f, 3.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 2.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
  };
  const uint16_t indices[] = {0u, 1u, 2u};
  VkrBindlessVkDrawRoot root = {
      .vertices = renderer->upload.address + VKR_BINDLESS_VK_VERTEX_OFFSET,
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
      .texture_index = 0u,
      .sampler_index = 0u,
      .materials = renderer->materials.address,
  };
  root.transform[0] = 1.0f;
  root.transform[5] = 1.0f;
  root.transform[10] = 1.0f;
  root.transform[15] = 1.0f;
  const uint8_t sentinel_pixel[] = {37u, 91u, 173u, 255u};
  MemCopy(mapped + VKR_BINDLESS_VK_ROOT_OFFSET, &root, sizeof(root));
  MemCopy(mapped + VKR_BINDLESS_VK_VERTEX_OFFSET, vertices, sizeof(vertices));
  MemCopy(mapped + VKR_BINDLESS_VK_INDEX_OFFSET, indices, sizeof(indices));
  MemCopy(mapped + VKR_BINDLESS_VK_TEXTURE_OFFSET, sentinel_pixel,
          sizeof(sentinel_pixel));
  return vkr_bindless_vk_flush(renderer, &renderer->upload.allocation, 0u,
                               VKR_BINDLESS_VK_TEXTURE_OFFSET +
                                   sizeof(sentinel_pixel));
}

static bool8_t
vkr_bindless_vk_create_resources(VkrBindlessVulkanRenderer *renderer) {
  if (!vkr_bindless_vk_create_upload_buffers(renderer) ||
      !vkr_bindless_vk_create_image(renderer, 1u, 1u,
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    &renderer->sentinel_image) ||
      !vkr_bindless_vk_create_target_set(
          renderer, renderer->config.width, renderer->config.height,
          renderer->config.image_count, &renderer->targets) ||
      !vkr_bindless_vk_create_frame_slots(renderer)) {
    return false_v;
  }
  const VkDeviceSize descriptor_alignment =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device)
          ->descriptorBufferOffsetAlignment;
  if ((renderer->resource_descriptors.address % descriptor_alignment) != 0u ||
      (renderer->sampler_descriptors.address % descriptor_alignment) != 0u ||
      !vkr_bindless_vk_write_upload_data(renderer)) {
    return false_v;
  }
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
  };
  return vkCreateSampler(vkr_bindless_vk_renderer_device(renderer),
                         &sampler_info, NULL,
                         &renderer->sentinel_sampler) == VK_SUCCESS;
}

static bool8_t vkr_bindless_vk_create_descriptor_slot_tables(
    VkrBindlessVulkanRenderer *renderer) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  if (!properties->sampledImageDescriptorSize ||
      !properties->storageImageDescriptorSize ||
      !properties->samplerDescriptorSize ||
      properties->sampledImageDescriptorSize > UINT32_MAX ||
      properties->storageImageDescriptorSize > UINT32_MAX ||
      properties->samplerDescriptorSize > UINT32_MAX) {
    log_error("Bindless Vulkan descriptor row size is not representable");
    return false_v;
  }
  const VkrGpuSlotTableConfig sampled_config = {
      .max_slots = renderer->config.sampled_image_capacity,
      .max_retirements = renderer->config.sampled_image_capacity,
      .row_size = (uint32_t)properties->sampledImageDescriptorSize,
  };
  const VkrGpuSlotTableConfig sampler_config = {
      .max_slots = renderer->config.sampler_capacity,
      .max_retirements = renderer->config.sampler_capacity,
      .row_size = (uint32_t)properties->samplerDescriptorSize,
  };
  const VkrGpuSlotTableConfig storage_config = {
      .max_slots = renderer->config.storage_image_capacity,
      .max_retirements = renderer->config.storage_image_capacity,
      .row_size = (uint32_t)properties->storageImageDescriptorSize,
  };
  const VkrGpuSlotTableConfig material_config = {
      .max_slots = renderer->config.material_slot_capacity,
      .max_retirements = renderer->config.material_slot_capacity,
      .row_size = sizeof(VkrBindlessVkMaterialGpuRow),
  };
  renderer->sampled_image_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&sampled_config);
  renderer->sampler_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&sampler_config);
  renderer->storage_image_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&storage_config);
  renderer->material_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&material_config);
  renderer->published_geometries_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->published_geometries);
  renderer->published_textures_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->published_textures);
  renderer->published_samplers_size =
      (uint64_t)renderer->config.sampler_capacity *
      sizeof(*renderer->published_samplers);
  renderer->published_materials_size =
      (uint64_t)renderer->config.material_record_capacity *
      sizeof(*renderer->published_materials);
  renderer->retired_materials_size =
      (uint64_t)renderer->config.material_record_capacity *
      sizeof(*renderer->retired_materials);
  renderer->pending_texture_initializations_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->pending_texture_initializations);
  renderer->pending_buffer_initialization_capacity =
      renderer->config.publication_staging_capacity;
  renderer->pending_buffer_initializations_size =
      (uint64_t)renderer->pending_buffer_initialization_capacity *
      sizeof(*renderer->pending_buffer_initializations);
  renderer->retired_staging_buffer_capacity =
      renderer->config.publication_staging_capacity;
  renderer->retired_staging_buffers_size =
      (uint64_t)renderer->retired_staging_buffer_capacity *
      sizeof(*renderer->retired_staging_buffers);
  renderer->descriptor_scratch_size =
      (uint32_t)Max(Max(properties->sampledImageDescriptorSize,
                        properties->storageImageDescriptorSize),
                    properties->samplerDescriptorSize);
  renderer->sampled_image_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->sampled_image_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->sampler_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->sampler_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->storage_image_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->storage_image_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->material_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->material_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->descriptor_scratch = vkr_allocator_alloc(
      renderer->allocator, renderer->descriptor_scratch_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_geometries = vkr_allocator_alloc(
      renderer->allocator, renderer->published_geometries_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_textures = vkr_allocator_alloc(
      renderer->allocator, renderer->published_textures_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_samplers = vkr_allocator_alloc(
      renderer->allocator, renderer->published_samplers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_materials = vkr_allocator_alloc(
      renderer->allocator, renderer->published_materials_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_materials =
      vkr_allocator_alloc(renderer->allocator, renderer->retired_materials_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->pending_texture_initializations = vkr_allocator_alloc(
      renderer->allocator, renderer->pending_texture_initializations_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->pending_buffer_initializations = vkr_allocator_alloc(
      renderer->allocator, renderer->pending_buffer_initializations_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_staging_buffers = vkr_allocator_alloc(
      renderer->allocator, renderer->retired_staging_buffers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!renderer->sampled_image_slot_storage ||
      !renderer->storage_image_slot_storage ||
      !renderer->sampler_slot_storage || !renderer->material_slot_storage ||
      !renderer->descriptor_scratch || !renderer->published_geometries ||
      !renderer->published_textures || !renderer->published_samplers ||
      !renderer->published_materials || !renderer->retired_materials ||
      !renderer->pending_texture_initializations ||
      !renderer->pending_buffer_initializations ||
      !renderer->retired_staging_buffers) {
    return false_v;
  }
  MemZero(renderer->published_geometries, renderer->published_geometries_size);
  MemZero(renderer->published_textures, renderer->published_textures_size);
  MemZero(renderer->published_samplers, renderer->published_samplers_size);
  MemZero(renderer->published_materials, renderer->published_materials_size);
  MemZero(renderer->retired_materials, renderer->retired_materials_size);
  MemZero(renderer->pending_texture_initializations,
          renderer->pending_texture_initializations_size);
  MemZero(renderer->pending_buffer_initializations,
          renderer->pending_buffer_initializations_size);
  MemZero(renderer->retired_staging_buffers,
          renderer->retired_staging_buffers_size);
  return vkr_gpu_slot_table_create(
             &sampled_config, renderer->sampled_image_slot_storage,
             renderer->sampled_image_slot_storage_size,
             (uint8_t *)renderer->resource_descriptors.allocation.mapped +
                 resource_layout->sampled_image_offset,
             &renderer->sampled_image_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &sampler_config, renderer->sampler_slot_storage,
             renderer->sampler_slot_storage_size,
             (uint8_t *)renderer->sampler_descriptors.allocation.mapped +
                 sampler_layout->sampler_offset,
             &renderer->sampler_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &storage_config, renderer->storage_image_slot_storage,
             renderer->storage_image_slot_storage_size,
             (uint8_t *)renderer->resource_descriptors.allocation.mapped +
                 resource_layout->storage_image_offset,
             &renderer->storage_image_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &material_config, renderer->material_slot_storage,
             renderer->material_slot_storage_size,
             renderer->materials.allocation.mapped,
             &renderer->material_slots) == VKR_GPU_SLOT_STATUS_OK;
}

static bool8_t vkr_bindless_vk_publish_sentinel_descriptors(
    VkrBindlessVulkanRenderer *renderer) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  VkDescriptorImageInfo image_info = {
      .imageView = renderer->sentinel_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo storage_info = {
      .imageView = renderer->sentinel_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  VkDescriptorGetInfoEXT image_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .data.pSampledImage = &image_info,
  };
  VkDescriptorGetInfoEXT sampler_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .data.pSampler = &renderer->sentinel_sampler,
  };
  VkDescriptorGetInfoEXT storage_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .data.pStorageImage = &storage_info,
  };
  PFN_vkGetDescriptorEXT get_descriptor =
      vkr_bindless_vulkan_device_get_descriptor(renderer->device);
  VkrGpuSlotHandle sampled_handle = {0};
  VkrGpuSlotHandle sampler_handle = {0};
  VkrGpuSlotHandle storage_handle = {0};
  VkrGpuSlotHandle material_handle = {0};
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &image_get,
                 properties->sampledImageDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampled_image_slots,
                                 renderer->descriptor_scratch,
                                 &sampled_handle) != VKR_GPU_SLOT_STATUS_OK ||
      sampled_handle.index != 0u) {
    return false_v;
  }
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &sampler_get,
                 properties->samplerDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampler_slots,
                                 renderer->descriptor_scratch,
                                 &sampler_handle) != VKR_GPU_SLOT_STATUS_OK ||
      sampler_handle.index != 0u) {
    return false_v;
  }
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &storage_get,
                 properties->storageImageDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->storage_image_slots,
                                 renderer->descriptor_scratch,
                                 &storage_handle) != VKR_GPU_SLOT_STATUS_OK ||
      storage_handle.index != 0u) {
    return false_v;
  }
  const VkrBindlessVkMaterialGpuRow material = {
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
      .base_color_texture = sampled_handle.index,
      .normal_texture = sampled_handle.index,
      .orm_texture = sampled_handle.index,
      .emissive_texture = sampled_handle.index,
      .base_color_sampler = sampler_handle.index,
      .normal_sampler = sampler_handle.index,
      .orm_sampler = sampler_handle.index,
      .emissive_sampler = sampler_handle.index,
      .material_id = 0xffad5b25u,
  };
  if (vkr_gpu_slot_table_publish(renderer->material_slots, &material,
                                 &material_handle) != VKR_GPU_SLOT_STATUS_OK ||
      material_handle.index != 0u) {
    return false_v;
  }
  return vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                    &renderer->resource_descriptors,
                                    resource_layout->sampled_image_offset,
                                    properties->sampledImageDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                    &renderer->resource_descriptors,
                                    resource_layout->storage_image_offset,
                                    properties->storageImageDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->sampler_descriptor_dirty,
                                    &renderer->sampler_descriptors,
                                    sampler_layout->sampler_offset,
                                    properties->samplerDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->material_dirty,
                                    &renderer->materials, 0u, sizeof(material));
}

static SpvReflectBlockVariable *
vkr_bindless_vk_reflect_member(SpvReflectBlockVariable *parent,
                               const char *name) {
  if (!parent || !name)
    return NULL;
  for (uint32_t i = 0; i < parent->member_count; ++i) {
    SpvReflectBlockVariable *member = &parent->members[i];
    if (member->name && string_equals(member->name, name))
      return member;
  }
  return NULL;
}

static bool8_t
vkr_bindless_vk_reflect_member_offset(SpvReflectBlockVariable *parent,
                                      const char *name, uint32_t offset,
                                      SpvReflectBlockVariable **out_member) {
  SpvReflectBlockVariable *member =
      vkr_bindless_vk_reflect_member(parent, name);
  if (out_member)
    *out_member = member;
  if (!member || member->offset != offset) {
    log_error("Bindless Vulkan shader ABI member %s is %s (offset %u, "
              "expected %u)",
              name, member ? "misaligned" : "missing",
              member ? member->offset : UINT32_MAX, offset);
    return false_v;
  }
  return true_v;
}

static uint32_t
vkr_bindless_vk_reflected_struct_size(const SpvReflectBlockVariable *value) {
  uint32_t size = 0u;
  if (!value)
    return size;
  for (uint32_t i = 0; i < value->member_count; ++i) {
    const SpvReflectBlockVariable *member = &value->members[i];
    const uint32_t member_size = Max(member->size, member->padded_size);
    if (member->offset <= UINT32_MAX - member_size)
      size = Max(size, member->offset + member_size);
  }
  return size;
}

static bool8_t vkr_bindless_vk_validate_vertex_pointer_abi(
    SpvReflectBlockVariable *push_block) {
  if (!push_block || push_block->size != sizeof(VkrBindlessVkPushConstants)) {
    log_error("Bindless Vulkan vertex push block is %u bytes (expected %zu)",
              push_block ? push_block->size : 0u,
              sizeof(VkrBindlessVkPushConstants));
    return false_v;
  }
  SpvReflectBlockVariable *root = NULL;
  bool8_t valid = vkr_bindless_vk_reflect_member_offset(
      push_block, "root", offsetof(VkrBindlessVkPushConstants, root), &root);
  valid &= vkr_bindless_vk_reflect_member_offset(
      push_block, "material_index",
      offsetof(VkrBindlessVkPushConstants, material_index), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      push_block, "flags", offsetof(VkrBindlessVkPushConstants, flags), NULL);
  if (!root || root->member_count == 0u) {
    log_error("Bindless Vulkan reflection did not recurse through draw-root "
              "PhysicalStorageBuffer");
    return false_v;
  }

  SpvReflectBlockVariable *vertices = NULL;
  SpvReflectBlockVariable *materials = NULL;
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "vertices", offsetof(VkrBindlessVkDrawRoot, vertices), &vertices);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "tint", offsetof(VkrBindlessVkDrawRoot, tint), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "transform", offsetof(VkrBindlessVkDrawRoot, transform), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "texture_index", offsetof(VkrBindlessVkDrawRoot, texture_index),
      NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "sampler_index", offsetof(VkrBindlessVkDrawRoot, sampler_index),
      NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "materials", offsetof(VkrBindlessVkDrawRoot, materials),
      &materials);
  if (!vertices || vertices->member_count == 0u || !materials ||
      materials->member_count == 0u) {
    log_error("Bindless Vulkan reflection did not recurse through vertex and "
              "material PhysicalStorageBuffer pointers");
    return false_v;
  }

  const VkrGpuAbiRecord *vertex_abi = vkr_gpu_abi_record(VKR_GPU_ABI_VERTEX);
  const uint32_t reflected_vertex_size =
      vkr_bindless_vk_reflected_struct_size(vertices);
  if (!vertex_abi || reflected_vertex_size != vertex_abi->expected_size) {
    log_error("Bindless Vulkan reflected vertex size is %u (expected %u)",
              reflected_vertex_size,
              vertex_abi ? vertex_abi->expected_size : 0u);
    valid = false_v;
  }
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "position", offsetof(VkrVertex3d, position), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "normal", offsetof(VkrVertex3d, normal), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "texcoord", offsetof(VkrVertex3d, texcoord), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "color", offsetof(VkrVertex3d, colour), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "tangent", offsetof(VkrVertex3d, tangent), NULL);
  const uint32_t reflected_material_size =
      vkr_bindless_vk_reflected_struct_size(materials);
  if (reflected_material_size != sizeof(VkrBindlessVkMaterialGpuRow)) {
    log_error("Bindless Vulkan reflected material size is %u (expected "
              "%zu)",
              reflected_material_size, sizeof(VkrBindlessVkMaterialGpuRow));
    valid = false_v;
  }
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "tint", offsetof(VkrBindlessVkMaterialGpuRow, tint), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "base_color_texture",
      offsetof(VkrBindlessVkMaterialGpuRow, base_color_texture), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "base_color_sampler",
      offsetof(VkrBindlessVkMaterialGpuRow, base_color_sampler), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "material_id",
      offsetof(VkrBindlessVkMaterialGpuRow, material_id), NULL);
  return valid;
}

static bool8_t vkr_bindless_vk_validate_shader_module_abi(
    VkrBindlessVulkanRenderer *renderer, const char *path,
    const char *entry_point, SpvReflectShaderStageFlagBits stage) {
  FilePath shader_path =
      file_path_create(path, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u) {
    log_error("Bindless Vulkan could not load SPIR-V for ABI validation: %s",
              path);
    return false_v;
  }
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult create_result =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (create_result != SPV_REFLECT_RESULT_SUCCESS) {
    log_error("Bindless Vulkan SPIR-V reflection failed for %s (%d)", path,
              create_result);
    return false_v;
  }

  bool8_t valid = true_v;
  const SpvReflectEntryPoint *entry =
      spvReflectGetEntryPoint(&module, entry_point);
  if (!entry || entry->shader_stage != stage) {
    log_error("Bindless Vulkan entry point %s is missing or has wrong stage",
              entry_point);
    valid = false_v;
  }
  uint32_t push_count = 0u;
  SpvReflectBlockVariable *push_blocks[1] = {0};
  if (spvReflectEnumerateEntryPointPushConstantBlocks(&module, entry_point,
                                                      &push_count, NULL) !=
          SPV_REFLECT_RESULT_SUCCESS ||
      push_count != 1u ||
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry_point, &push_count, push_blocks) !=
          SPV_REFLECT_RESULT_SUCCESS) {
    log_error("Bindless Vulkan entry point %s has %u push blocks (expected 1)",
              entry_point, push_count);
    valid = false_v;
  } else if (stage == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) {
    valid &= vkr_bindless_vk_validate_vertex_pointer_abi(push_blocks[0]);
  } else if (push_blocks[0]->size != sizeof(VkrBindlessVkPushConstants)) {
    log_error("Bindless Vulkan fragment push block is %u bytes (expected %zu)",
              push_blocks[0]->size, sizeof(VkrBindlessVkPushConstants));
    valid = false_v;
  }

  if (stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT) {
    uint32_t binding_count = 0u;
    SpvReflectDescriptorBinding *bindings[4] = {0};
    if (spvReflectEnumerateEntryPointDescriptorBindings(&module, entry_point,
                                                        &binding_count, NULL) !=
            SPV_REFLECT_RESULT_SUCCESS ||
        binding_count > ArrayCount(bindings) ||
        spvReflectEnumerateEntryPointDescriptorBindings(
            &module, entry_point, &binding_count, bindings) !=
            SPV_REFLECT_RESULT_SUCCESS) {
      log_error("Bindless Vulkan fragment descriptor reflection failed");
      valid = false_v;
    } else {
      bool8_t sampled = false_v;
      bool8_t sampler = false_v;
      for (uint32_t i = 0; i < binding_count; ++i) {
        const SpvReflectDescriptorBinding *binding = bindings[i];
        const bool8_t runtime_array =
            binding->array.dims_count == 1u && binding->array.dims[0] == 0u;
        sampled |= binding->set == 0u && binding->binding == 0u &&
                   binding->descriptor_type ==
                       SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                   runtime_array;
        sampler |=
            binding->set == 1u && binding->binding == 0u &&
            binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER &&
            runtime_array;
      }
      if (!sampled || !sampler) {
        log_error("Bindless Vulkan runtime descriptor arrays are incomplete "
                  "(sampled=%u sampler=%u)",
                  sampled, sampler);
        valid = false_v;
      }
    }
  }
  spvReflectDestroyShaderModule(&module);
  return valid;
}

static bool8_t
vkr_bindless_vk_validate_shader_abi(VkrBindlessVulkanRenderer *renderer) {
  return vkr_bindless_vk_validate_shader_module_abi(
             renderer, VKR_BINDLESS_VK_VERTEX_SPV, "vert_main",
             SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) &&
         vkr_bindless_vk_validate_shader_module_abi(
             renderer, VKR_BINDLESS_VK_FRAGMENT_SPV, "frag_main",
             SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT);
}

static bool8_t
vkr_bindless_vk_create_shader_module(VkrBindlessVulkanRenderer *renderer,
                                     const char *path,
                                     VkShaderModule *out_module) {
  FilePath shader_path =
      file_path_create(path, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0 || (size % sizeof(uint32_t)) != 0) {
    return false_v;
  }
  VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)size,
      .pCode = (const uint32_t *)bytes,
  };
  const VkResult result =
      vkCreateShaderModule(vkr_bindless_vk_renderer_device(renderer),
                           &module_info, NULL, out_module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return result == VK_SUCCESS;
}

static bool8_t
vkr_bindless_vk_create_pipeline(VkrBindlessVulkanRenderer *renderer) {
  renderer->shader_abi_validated =
      vkr_bindless_vk_validate_shader_abi(renderer);
  if (!renderer->shader_abi_validated ||
      !vkr_bindless_vk_create_shader_module(
          renderer, VKR_BINDLESS_VK_VERTEX_SPV, &renderer->vertex_shader) ||
      !vkr_bindless_vk_create_shader_module(
          renderer, VKR_BINDLESS_VK_FRAGMENT_SPV, &renderer->fragment_shader)) {
    return false_v;
  }
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  VkDescriptorSetLayout layouts[] = {
      resource_layout->handle,
      sampler_layout->handle,
  };
  VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0u,
      .size = sizeof(VkrBindlessVkPushConstants),
  };
  VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = ArrayCount(layouts),
      .pSetLayouts = layouts,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_range,
  };
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (vkCreatePipelineLayout(device, &layout_info, NULL,
                             &renderer->pipeline_layout) != VK_SUCCESS) {
    return false_v;
  }
  VkPipelineShaderStageCreateInfo stages[] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = renderer->vertex_shader,
          .pName = "vert_main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = renderer->fragment_shader,
          .pName = "frag_main",
      },
  };
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkPipelineViewportStateCreateInfo viewport = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState color_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1u,
      .pAttachments = &color_attachment,
  };
  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = ArrayCount(dynamic_states),
      .pDynamicStates = dynamic_states,
  };
  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipelineRenderingCreateInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1u,
      .pColorAttachmentFormats = &color_format,
  };
  VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering,
      .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .stageCount = ArrayCount(stages),
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic,
      .layout = renderer->pipeline_layout,
  };
  return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &pipeline_info,
                                   NULL, &renderer->pipeline) == VK_SUCCESS;
}

static bool8_t
vkr_bindless_vk_create_timeline(VkrBindlessVulkanRenderer *renderer) {
  VkSemaphoreTypeCreateInfo type_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0u,
  };
  VkSemaphoreCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &type_info,
  };
  return vkCreateSemaphore(vkr_bindless_vk_renderer_device(renderer),
                           &create_info, NULL,
                           &renderer->timeline) == VK_SUCCESS;
}

static uint64_t
vkr_bindless_vk_refresh_completed(VkrBindlessVulkanRenderer *renderer) {
  uint64_t completed = renderer->completed_value;
  if (renderer->timeline && vkGetSemaphoreCounterValue(
                                vkr_bindless_vk_renderer_device(renderer),
                                renderer->timeline, &completed) == VK_SUCCESS) {
    renderer->completed_value = completed;
  }
  return renderer->completed_value;
}

bool8_t vkr_bindless_vulkan_renderer_create(
    const VkrBindlessVulkanRendererConfig *config,
    VkrBindlessVulkanRenderer **out_renderer) {
  if (!out_renderer)
    return false_v;
  *out_renderer = NULL;
  if (!config || !config->allocator || !config->width || !config->height ||
      !config->image_count ||
      config->image_count > VKR_BINDLESS_VK_TARGET_IMAGE_MAX ||
      !config->sampled_image_capacity || !config->storage_image_capacity ||
      !config->sampler_capacity || !config->material_record_capacity ||
      !config->device_buffer_block_size || !config->device_image_block_size ||
      !config->upload_buffer_block_size ||
      !config->readback_buffer_block_size || !config->memory_block_capacity ||
      !config->memory_blocks_per_pool ||
      config->memory_blocks_per_pool > config->memory_block_capacity ||
      !config->memory_block_allocation_capacity ||
      config->publication_staging_capacity < 2u ||
      (uint64_t)config->material_slot_capacity <
          (uint64_t)config->material_record_capacity * 2u + 1u) {
    return false_v;
  }
  VkrBindlessVulkanRenderer *renderer = vkr_allocator_alloc(
      config->allocator, sizeof(*renderer), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!renderer) {
    return false_v;
  }
  MemZero(renderer, sizeof(*renderer));
  renderer->allocator = config->allocator;
  renderer->config = *config;
  if (vkr_gpu_submit_ring_create(
          &renderer->command_ring, VKR_BINDLESS_VK_FRAME_SLOT_COUNT,
          VKR_BINDLESS_VK_FRAME_SLOT_COUNT, renderer->command_ring_slots,
          sizeof(renderer->command_ring_slots)) !=
      VKR_GPU_SUBMIT_RING_STATUS_OK) {
    vkr_allocator_free(config->allocator, renderer, sizeof(*renderer),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }
  *out_renderer = renderer;
  VkrBindlessVulkanDeviceConfig device_config = {
      .allocator = config->allocator,
      .window = config->window,
      .sampled_image_capacity = config->sampled_image_capacity,
      .storage_image_capacity = config->storage_image_capacity,
      .sampler_capacity = config->sampler_capacity,
      .root_push_constant_size = sizeof(VkrBindlessVkPushConstants),
      .windowed = config->target_kind != VKR_PRESENT_TARGET_OFFSCREEN,
      .enable_validation = config->enable_validation,
      .enable_synchronization_validation =
          config->enable_synchronization_validation,
      .enable_gpu_assisted = config->enable_gpu_assisted,
  };
  if (!vkr_bindless_vulkan_device_create(&device_config, &renderer->device) ||
      !vkr_bindless_vk_create_timeline(renderer)) {
    return false_v;
  }
  const VkrBindlessVkMemoryPoolConfig memory_config = {
      .allocator = renderer->allocator,
      .device = vkr_bindless_vk_renderer_device(renderer),
      .block_sizes =
          {
              [VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE] =
                  {
                      [VKR_BINDLESS_VK_MEMORY_KIND_BUFFER] =
                          config->device_buffer_block_size,
                      [VKR_BINDLESS_VK_MEMORY_KIND_IMAGE] =
                          config->device_image_block_size,
                  },
              [VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD] =
                  {
                      [VKR_BINDLESS_VK_MEMORY_KIND_BUFFER] =
                          config->upload_buffer_block_size,
                      [VKR_BINDLESS_VK_MEMORY_KIND_IMAGE] =
                          config->upload_buffer_block_size,
                  },
              [VKR_BINDLESS_VK_MEMORY_CLASS_READBACK] =
                  {
                      [VKR_BINDLESS_VK_MEMORY_KIND_BUFFER] =
                          config->readback_buffer_block_size,
                      [VKR_BINDLESS_VK_MEMORY_KIND_IMAGE] =
                          config->readback_buffer_block_size,
                  },
          },
      .max_blocks = config->memory_block_capacity,
      .max_blocks_per_pool = config->memory_blocks_per_pool,
      .max_allocations_per_block = config->memory_block_allocation_capacity,
  };
  if (!vkr_bindless_vulkan_memory_pool_create(&memory_config,
                                              &renderer->memory_pool))
    return false_v;
  if (config->target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    if (!vkr_bindless_vk_create_acquire_semaphores(renderer) ||
        !vkr_bindless_vk_create_window_target(
            renderer, config->width, config->height, config->image_count,
            VK_NULL_HANDLE, &renderer->window_target)) {
      return false_v;
    }
    renderer->config.width = renderer->window_target.width;
    renderer->config.height = renderer->window_target.height;
    renderer->config.image_count = renderer->window_target.image_count;
  }
  if (!vkr_bindless_vk_create_resources(renderer) ||
      !vkr_bindless_vk_create_descriptor_slot_tables(renderer) ||
      !vkr_bindless_vk_publish_sentinel_descriptors(renderer) ||
      !vkr_bindless_vk_create_pipeline(renderer)) {
    return false_v;
  }
  return true_v;
}

static void vkr_bindless_vk_cmd_image_barrier_range(
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

static void vkr_bindless_vk_cmd_image_barrier(
    VkCommandBuffer command_buffer, VkImage image,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
    VkImageLayout old_layout, VkImageLayout new_layout) {
  vkr_bindless_vk_cmd_image_barrier_range(command_buffer, image, src_stage,
                                          src_access, dst_stage, dst_access,
                                          old_layout, new_layout, 1u, 1u);
}

static void vkr_bindless_vk_release_texture_initialization(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkPendingTextureInitialization *initialization) {
  if (initialization->regions)
    vkr_allocator_free(renderer->allocator, initialization->regions,
                       initialization->regions_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  vkr_bindless_vk_destroy_buffer(renderer, &initialization->staging);
  MemZero(initialization, sizeof(*initialization));
}

static bool8_t vkr_bindless_vk_enqueue_texture_initialization(
    VkrBindlessVulkanRenderer *renderer,
    const VkrBindlessVkPendingTextureInitialization *initialization) {
  if (renderer->pending_texture_initialization_count >=
          renderer->config.sampled_image_capacity ||
      (initialization->staging.handle &&
       renderer->staging_buffer_count >=
           renderer->retired_staging_buffer_capacity))
    return false_v;
  renderer->pending_texture_initializations
      [renderer->pending_texture_initialization_count++] = *initialization;
  if (initialization->staging.handle)
    renderer->staging_buffer_count++;
  return true_v;
}

static bool8_t vkr_bindless_vk_upload_prepared_texture(
    VkrBindlessVulkanRenderer *renderer, const VkrTexturePreparedLoad *prepared,
    uint32_t texture_record_index, VkrBindlessVkImage *out_image,
    VkrBindlessVkPendingTextureInitialization *out_initialization) {
  if (!prepared || !out_image || !out_initialization ||
      !prepared->upload_data || !prepared->upload_data_size ||
      !prepared->upload_regions || !prepared->upload_region_count ||
      !prepared->upload_mip_levels || !prepared->upload_array_layers ||
      (prepared->description.sample_count != 0u &&
       prepared->description.sample_count != VKR_SAMPLE_COUNT_1) ||
      (prepared->description.type != VKR_TEXTURE_TYPE_2D &&
       prepared->description.type != VKR_TEXTURE_TYPE_CUBE_MAP))
    return false_v;
  const VkFormat format =
      vkr_bindless_vk_texture_format(prepared->description.format);
  if (format == VK_FORMAT_UNDEFINED)
    return false_v;
  VkFormatProperties format_properties;
  vkGetPhysicalDeviceFormatProperties(
      vkr_bindless_vulkan_device_physical(renderer->device), format,
      &format_properties);
  if ((format_properties.optimalTilingFeatures &
       (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) !=
      (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
    return false_v;

  const bool8_t cube = prepared->description.type == VKR_TEXTURE_TYPE_CUBE_MAP;
  if ((cube && prepared->upload_array_layers != 6u) ||
      !vkr_bindless_vk_create_image_ex(
          renderer, prepared->description.width, prepared->description.height,
          prepared->upload_mip_levels, prepared->upload_array_layers, format,
          cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
          cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          out_image))
    return false_v;

  VkrBindlessVkBuffer staging = {0};
  if (!vkr_bindless_vk_create_buffer(
          renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
          prepared->upload_data_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          &staging)) {
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  MemCopy(staging.allocation.mapped, prepared->upload_data,
          prepared->upload_data_size);
  if (!vkr_bindless_vk_flush(renderer, &staging.allocation, 0u,
                             prepared->upload_data_size)) {
    vkr_bindless_vk_destroy_buffer(renderer, &staging);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }

  const uint64_t regions_size =
      (uint64_t)prepared->upload_region_count * sizeof(VkBufferImageCopy2);
  VkBufferImageCopy2 *regions = vkr_allocator_alloc(
      renderer->allocator, regions_size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!regions) {
    vkr_bindless_vk_destroy_buffer(renderer, &staging);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  bool8_t valid = true_v;
  for (uint32_t i = 0; i < prepared->upload_region_count; ++i) {
    const VkrTextureUploadRegion *source = &prepared->upload_regions[i];
    if (!source->width || !source->height || !source->depth ||
        source->mip_level >= prepared->upload_mip_levels ||
        source->array_layer >= prepared->upload_array_layers ||
        source->byte_offset > prepared->upload_data_size ||
        source->byte_size > prepared->upload_data_size - source->byte_offset) {
      valid = false_v;
      break;
    }
    regions[i] = (VkBufferImageCopy2){
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = source->byte_offset,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = source->mip_level,
                             .baseArrayLayer = source->array_layer,
                             .layerCount = 1u},
        .imageExtent = {.width = source->width,
                        .height = source->height,
                        .depth = source->depth},
    };
  }
  if (valid) {
    *out_initialization = (VkrBindlessVkPendingTextureInitialization){
        .staging = staging,
        .regions = regions,
        .regions_size = regions_size,
        .texture_record_index = texture_record_index,
        .region_count = prepared->upload_region_count,
    };
    out_image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  } else {
    VkrBindlessVkPendingTextureInitialization initialization = {
        .staging = staging,
        .regions = regions,
        .regions_size = regions_size,
    };
    vkr_bindless_vk_release_texture_initialization(renderer, &initialization);
    vkr_bindless_vk_destroy_image(renderer, out_image);
  }
  return valid;
}

static void vkr_bindless_vk_record_texture_initializations(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command) {
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    const VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    const VkrBindlessVkPublishedTexture *texture =
        &renderer->published_textures[initialization->texture_record_index];
    if (initialization->writable) {
      vkr_bindless_vk_cmd_image_barrier_range(
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
    vkr_bindless_vk_cmd_image_barrier_range(
        command, texture->image.handle, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture->image.mip_levels,
        texture->image.array_layers);
    const VkCopyBufferToImageInfo2 copy_info = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = initialization->staging.handle,
        .dstImage = texture->image.handle,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = initialization->region_count,
        .pRegions = initialization->regions,
    };
    vkCmdCopyBufferToImage2(command, &copy_info);
    vkr_bindless_vk_cmd_image_barrier_range(
        command, texture->image.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture->image.mip_levels,
        texture->image.array_layers);
  }
}

static void vkr_bindless_vk_record_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command) {
  for (uint32_t i = 0; i < renderer->pending_buffer_initialization_count; ++i) {
    const VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    const VkBufferCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .size = initialization->size,
    };
    const VkCopyBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer = initialization->staging.handle,
        .dstBuffer = initialization->destination,
        .regionCount = 1u,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(command, &copy);
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

static VkrBindlessVkRetiredStagingBuffer *
vkr_bindless_vk_reserve_staging_retirement(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t slot = 0; slot < renderer->retired_staging_buffer_capacity;
       ++slot) {
    if (!renderer->retired_staging_buffers[slot].occupied)
      return &renderer->retired_staging_buffers[slot];
  }
  return NULL;
}

static void vkr_bindless_vk_commit_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer, uint64_t retire_value) {
  for (uint32_t i = 0; i < renderer->pending_buffer_initialization_count; ++i) {
    VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[initialization->geometry_record_index];
    VkrBindlessVkRetiredStagingBuffer *retired =
        vkr_bindless_vk_reserve_staging_retirement(renderer);
    if (!retired)
      log_fatal("Bindless Vulkan lost bounded staging retirement capacity");
    if (!vkr_bindless_vk_retire_buffer(renderer, &initialization->staging,
                                       retire_value))
      log_fatal("Bindless Vulkan failed to retire submitted staging memory");
    *retired = (VkrBindlessVkRetiredStagingBuffer){
        .buffer = initialization->staging,
        .retire_value = retire_value,
        .occupied = true_v,
    };
    geometry->last_use_submit_value =
        Max(geometry->last_use_submit_value, retire_value);
    if (geometry->pending_initialization_count)
      geometry->pending_initialization_count--;
    MemZero(initialization, sizeof(*initialization));
  }
  renderer->pending_buffer_initialization_count = 0u;
}

static void vkr_bindless_vk_discard_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->pending_buffer_initialization_count; ++i) {
    VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[initialization->geometry_record_index];
    vkr_bindless_vk_destroy_buffer(renderer, &initialization->staging);
    if (renderer->staging_buffer_count)
      renderer->staging_buffer_count--;
    if (geometry->pending_initialization_count)
      geometry->pending_initialization_count--;
    MemZero(initialization, sizeof(*initialization));
  }
  renderer->pending_buffer_initialization_count = 0u;
}

static void vkr_bindless_vk_discard_geometry_initializations(
    VkrBindlessVulkanRenderer *renderer, uint32_t geometry_record_index) {
  uint32_t write_index = 0u;
  for (uint32_t read_index = 0;
       read_index < renderer->pending_buffer_initialization_count;
       ++read_index) {
    VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[read_index];
    if (initialization->geometry_record_index == geometry_record_index) {
      vkr_bindless_vk_destroy_buffer(renderer, &initialization->staging);
      if (renderer->staging_buffer_count)
        renderer->staging_buffer_count--;
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

static void vkr_bindless_vk_commit_texture_initializations(
    VkrBindlessVulkanRenderer *renderer, uint64_t retire_value) {
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    VkrBindlessVkPublishedTexture *texture =
        &renderer->published_textures[initialization->texture_record_index];
    texture->initialization_pending = false_v;
    texture->last_use_submit_value =
        Max(texture->last_use_submit_value, retire_value);
    if (initialization->staging.handle) {
      VkrBindlessVkRetiredStagingBuffer *retired =
          vkr_bindless_vk_reserve_staging_retirement(renderer);
      if (!retired)
        log_fatal("Bindless Vulkan lost bounded staging retirement capacity");
      if (!vkr_bindless_vk_retire_buffer(renderer, &initialization->staging,
                                         retire_value))
        log_fatal("Bindless Vulkan failed to retire submitted staging memory");
      *retired = (VkrBindlessVkRetiredStagingBuffer){
          .buffer = initialization->staging,
          .retire_value = retire_value,
          .occupied = true_v,
      };
      MemZero(&initialization->staging, sizeof(initialization->staging));
    }
    vkr_bindless_vk_release_texture_initialization(renderer, initialization);
  }
  renderer->pending_texture_initialization_count = 0u;
}

static void vkr_bindless_vk_discard_texture_initializations(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    renderer->published_textures[initialization->texture_record_index]
        .initialization_pending = false_v;
    if (initialization->staging.handle)
      renderer->staging_buffer_count--;
    vkr_bindless_vk_release_texture_initialization(renderer, initialization);
  }
  renderer->pending_texture_initialization_count = 0u;
}

bool8_t
vkr_bindless_vulkan_renderer_prepare_frame(VkrBindlessVulkanRenderer *renderer,
                                           uint64_t source_frame_index,
                                           VkrFrameSetup *out_setup) {
  if (!renderer || !out_setup || renderer->frame_active ||
      renderer->terminal_failure) {
    return false_v;
  }
  uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_collect_retired_targets(renderer, completed);
  vkr_bindless_vk_collect_retired_window_targets(renderer);
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
  if (renderer->target_dirty &&
      !vkr_bindless_vk_recreate_window_target(renderer, renderer->config.width,
                                              renderer->config.height,
                                              renderer->config.image_count))
    return false_v;
  completed = vkr_bindless_vk_refresh_completed(renderer);
  const uint32_t slot_index = renderer->command_ring.next_slot;
  VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[slot_index];
  if (slot->retire_value > completed) {
    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1u,
        .pSemaphores = &renderer->timeline,
        .pValues = &slot->retire_value,
    };
    if (vkWaitSemaphores(vkr_bindless_vk_renderer_device(renderer), &wait_info,
                         UINT64_MAX) != VK_SUCCESS) {
      return false_v;
    }
    renderer->command_slot_wait_count++;
    completed = vkr_bindless_vk_refresh_completed(renderer);
    vkr_bindless_vk_collect_retired_targets(renderer, completed);
  }
  if (vkResetCommandPool(vkr_bindless_vk_renderer_device(renderer),
                         slot->command_pool, 0u) != VK_SUCCESS) {
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
  if (renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN) {
    slot->image_index = renderer->next_image_index;
    renderer->next_image_index =
        (renderer->next_image_index + 1u) % renderer->targets.image_count;
  } else {
    const VkResult acquire_result =
        vkAcquireNextImageKHR(vkr_bindless_vk_renderer_device(renderer),
                              renderer->window_target.swapchain, UINT64_MAX,
                              renderer->acquire_semaphores[slot_index],
                              VK_NULL_HANDLE, &slot->image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
      renderer->target_dirty = true_v;
      vkr_bindless_vulkan_renderer_cancel_frame(renderer);
      return false_v;
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
      vkr_bindless_vulkan_renderer_cancel_frame(renderer);
      return false_v;
    }
    if (acquire_result == VK_SUBOPTIMAL_KHR)
      renderer->target_dirty = true_v;
    slot->acquired_window_image = true_v;
    const uint32_t image_index = slot->image_index;
    const VkrBindlessVulkanReacquireResult reacquire =
        vkr_bindless_vulkan_reacquire_record(
            &renderer->window_target.reacquire_state,
            renderer->window_target.image_presented[image_index]);
    if (reacquire.image_present_complete)
      renderer->wsi_reacquire_proofs++;
    if (reacquire.collect_retired_swapchains)
      vkr_bindless_vk_collect_retired_window_targets(renderer);
    const uint64_t image_submit =
        renderer->window_target.image_last_submit_value[image_index];
    if (image_submit > completed) {
      VkSemaphoreWaitInfo image_wait = {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
          .semaphoreCount = 1u,
          .pSemaphores = &renderer->timeline,
          .pValues = &image_submit,
      };
      if (vkWaitSemaphores(vkr_bindless_vk_renderer_device(renderer),
                           &image_wait, UINT64_MAX) != VK_SUCCESS) {
        vkr_bindless_vulkan_renderer_cancel_frame(renderer);
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
  return true_v;
}

static bool8_t vkr_bindless_vk_record_draw(VkrBindlessVulkanRenderer *renderer,
                                           VkrBindlessVkFrameSlot *slot) {
  VkCommandBuffer command = slot->command_buffer;
  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
    return false_v;
  }
  vkr_bindless_vk_record_buffer_initializations(renderer, command);
  vkr_bindless_vk_record_texture_initializations(renderer, command);
  if (!renderer->sentinel_uploaded) {
    vkr_bindless_vk_cmd_image_barrier(
        command, renderer->sentinel_image.handle, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy2 copy_region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = VKR_BINDLESS_VK_TEXTURE_OFFSET,
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
    vkr_bindless_vk_cmd_image_barrier(
        command, renderer->sentinel_image.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  VkrBindlessVkImage *target = &renderer->targets.images[slot->image_index];
  const bool8_t target_initialized =
      target->layout != VK_IMAGE_LAYOUT_UNDEFINED;
  vkr_bindless_vk_cmd_image_barrier(
      command, target->handle,
      target_initialized
          ? VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT
          : VK_PIPELINE_STAGE_2_NONE,
      target_initialized ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_NONE,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, target->layout,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = target->view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}},
  };
  VkRenderingInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.extent = {.width = target->width,
                                .height = target->height}},
      .layerCount = 1u,
      .colorAttachmentCount = 1u,
      .pColorAttachments = &attachment,
  };
  vkCmdBeginRendering(command, &rendering);
  VkDescriptorBufferBindingInfoEXT descriptor_bindings[] = {
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
  vkr_bindless_vulkan_device_cmd_bind_descriptor_buffers(renderer->device)(
      command, ArrayCount(descriptor_bindings), descriptor_bindings);
  const uint32_t buffer_indices[] = {0u, 1u};
  const VkDeviceSize descriptor_offsets[] = {0u, 0u};
  vkr_bindless_vulkan_device_cmd_set_descriptor_offsets(renderer->device)(
      command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline_layout, 0u,
      ArrayCount(buffer_indices), buffer_indices, descriptor_offsets);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->pipeline);
  const VkViewport viewport = {
      .width = (float32_t)target->width,
      .height = (float32_t)target->height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor = {
      .extent = {.width = target->width, .height = target->height},
  };
  vkCmdSetViewport(command, 0u, 1u, &viewport);
  vkCmdSetScissor(command, 0u, 1u, &scissor);
  VkrBindlessVkPublishedGeometry *active_geometry =
      renderer->active_geometry
          ? &renderer
                 ->published_geometries[renderer->active_geometry_record_index]
          : NULL;
  vkCmdBindIndexBuffer2(command,
                        active_geometry ? active_geometry->indices.handle
                                        : renderer->upload.handle,
                        active_geometry ? 0u : VKR_BINDLESS_VK_INDEX_OFFSET,
                        active_geometry ? active_geometry->indices.size
                                        : (VkDeviceSize)(sizeof(uint16_t) * 3u),
                        active_geometry ? active_geometry->index_type
                                        : VK_INDEX_TYPE_UINT16);
  const VkrBindlessVkPushConstants push = {
      .root = renderer->upload.address + VKR_BINDLESS_VK_ROOT_OFFSET,
      .material_index = renderer->active_material_index,
  };
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0u, sizeof(push), &push);
  vkCmdDrawIndexed(command, active_geometry ? active_geometry->index_count : 3u,
                   1u, 0u, 0, 0u);
  vkCmdEndRendering(command);

  vkr_bindless_vk_cmd_image_barrier(
      command, target->handle, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
      VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy2 readback_region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1u},
      .imageExtent = {.width = 1u, .height = 1u, .depth = 1u},
  };
  VkCopyImageToBufferInfo2 readback_info = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
      .srcImage = target->handle,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstBuffer = slot->readback.handle,
      .regionCount = 1u,
      .pRegions = &readback_region,
  };
  vkCmdCopyImageToBuffer2(command, &readback_info);
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    VkrBindlessVkWindowTarget *window = &renderer->window_target;
    const uint32_t image_index = slot->image_index;
    vkr_bindless_vk_cmd_image_barrier(
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
    vkr_bindless_vk_cmd_image_barrier(
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
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    return false_v;
  }
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_submit_packet(
    VkrBindlessVulkanRenderer *renderer, const VkrRenderPacket *packet,
    VkrBindlessVulkanResult *out_result) {
  if (!renderer || !packet || !renderer->frame_active) {
    return false_v;
  }
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!vkr_bindless_vk_record_draw(renderer, slot)) {
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_bindless_vk_flush_publication_ranges(renderer)) {
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  const uint64_t signal_value = renderer->submit_value + 1u;
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
  if (vkQueueSubmit2(vkr_bindless_vulkan_device_queue(renderer->device), 1u,
                     &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
    vkr_gpu_submit_ring_cancel(&renderer->command_ring,
                               renderer->active_command_slice);
    renderer->frame_active = false_v;
    renderer->terminal_failure = true_v;
    return false_v;
  }
  renderer->submit_value = signal_value;
  slot->retire_value = signal_value;
  vkr_bindless_vk_commit_buffer_initializations(renderer, signal_value);
  vkr_bindless_vk_commit_texture_initializations(renderer, signal_value);
  renderer->targets.images[slot->image_index].layout =
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  renderer->sentinel_image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (renderer->active_geometry)
    renderer->published_geometries[renderer->active_geometry_record_index]
        .last_use_submit_value = signal_value;
  if (vkr_gpu_submit_ring_submit(&renderer->command_ring,
                                 renderer->active_command_slice,
                                 signal_value) != VKR_GPU_SUBMIT_RING_STATUS_OK)
    log_fatal("Vulkan command ring lost its acquired slot after queue submit");
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    VkrBindlessVkWindowTarget *window = &renderer->window_target;
    const uint32_t image_index = slot->image_index;
    window->image_last_submit_value[image_index] = signal_value;
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &window->render_complete[image_index],
        .swapchainCount = 1u,
        .pSwapchains = &window->swapchain,
        .pImageIndices = &image_index,
    };
    const VkResult present_result = vkQueuePresentKHR(
        vkr_bindless_vulkan_device_queue(renderer->device), &present_info);
    const VkrBindlessVulkanPresentResult disposition =
        vkr_bindless_vulkan_present_result_classify(present_result);
    if (disposition.present_completion_tracking_required) {
      window->image_presented[image_index] = true_v;
    }
    if (disposition.target_recreate_required)
      renderer->target_dirty = true_v;
    if (disposition.acquired_image_recovery_required ||
        !disposition.enqueue_state_known || disposition.device_lost) {
      renderer->frame_active = false_v;
      renderer->terminal_failure = true_v;
      return false_v;
    }
    slot->acquired_window_image = false_v;
  }
  renderer->sentinel_uploaded = true_v;
  renderer->frame_active = false_v;
  if (out_result) {
    *out_result = (VkrBindlessVulkanResult){
        .submit_value = signal_value,
        .source_frame_index = slot->source_frame_index,
        .indexed_draw_count = 1u,
        .image_index = slot->image_index,
    };
  }
  return true_v;
}

bool8_t
vkr_bindless_vulkan_renderer_poll_result(VkrBindlessVulkanRenderer *renderer,
                                         uint64_t after_submit_value,
                                         VkrBindlessVulkanResult *out_result) {
  if (!renderer || !out_result) {
    return false_v;
  }
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  VkrBindlessVkFrameSlot *best = NULL;
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->retire_value > after_submit_value &&
        slot->retire_value <= completed &&
        (!best || slot->retire_value < best->retire_value)) {
      best = slot;
    }
  }
  if (!best || !vkr_bindless_vk_invalidate(renderer, &best->readback.allocation,
                                           0u, 4u)) {
    return false_v;
  }
  const uint8_t *color = best->readback.allocation.mapped;
  *out_result = (VkrBindlessVulkanResult){
      .submit_value = best->retire_value,
      .source_frame_index = best->source_frame_index,
      .indexed_draw_count = 1u,
      .image_index = best->image_index,
      .color = {color[0], color[1], color[2], color[3]},
      .identifier = (uint32_t)color[0] | ((uint32_t)color[1] << 8u) |
                    ((uint32_t)color[2] << 16u) | ((uint32_t)color[3] << 24u),
      .readback_ready = true_v,
  };
  return true_v;
}

static bool8_t
vkr_bindless_vk_recreate_window_target(VkrBindlessVulkanRenderer *renderer,
                                       uint32_t width, uint32_t height,
                                       uint32_t image_count) {
  if (renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN)
    return false_v;
  if (!vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;
  vkr_bindless_vk_collect_retired_window_targets(renderer);
  VkrBindlessVkRetiredWindowTarget *retired = NULL;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    if (!renderer->retired_window_targets[i].occupied) {
      retired = &renderer->retired_window_targets[i];
      break;
    }
  }
  if (!retired) {
    log_error("Bindless Vulkan exhausted %u deferred swapchains before a "
              "successor presentation completed",
              (uint32_t)ArrayCount(renderer->retired_window_targets));
    return false_v;
  }

  VkrBindlessVkWindowTarget replacement_window = {0};
  if (!vkr_bindless_vk_create_window_target(
          renderer, width, height, image_count,
          renderer->window_target.swapchain, &replacement_window))
    return false_v;
  VkrBindlessVkTargetSet replacement_targets = {0};
  if (!vkr_bindless_vk_create_target_set(
          renderer, replacement_window.width, replacement_window.height,
          replacement_window.image_count, &replacement_targets)) {
    vkr_bindless_vk_destroy_window_target(renderer, &replacement_window);
    return false_v;
  }
  retired->target = renderer->window_target;
  retired->occupied = true_v;
  renderer->wsi_retired_swapchains++;
  renderer->window_target = replacement_window;
  vkr_bindless_vk_destroy_target_set(renderer, &renderer->targets);
  renderer->targets = replacement_targets;
  renderer->config.width = replacement_window.width;
  renderer->config.height = replacement_window.height;
  renderer->config.image_count = replacement_window.image_count;
  renderer->target_dirty = false_v;
  return true_v;
}

static bool8_t
vkr_bindless_vk_present_cancelled_frame(VkrBindlessVulkanRenderer *renderer,
                                        VkrBindlessVkFrameSlot *slot) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkrBindlessVkWindowTarget *window = &renderer->window_target;
  const uint32_t image_index = slot->image_index;
  if (vkResetCommandPool(device, slot->command_pool, 0u) != VK_SUCCESS)
    return false_v;
  const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (vkBeginCommandBuffer(slot->command_buffer, &begin_info) != VK_SUCCESS)
    return false_v;
  if (!window->image_presented[image_index]) {
    vkr_bindless_vk_cmd_image_barrier(
        slot->command_buffer, window->images[image_index],
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  if (vkEndCommandBuffer(slot->command_buffer) != VK_SUCCESS)
    return false_v;

  const uint64_t signal_value = renderer->submit_value + 1u;
  const VkCommandBufferSubmitInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = slot->command_buffer,
  };
  const VkSemaphoreSubmitInfo wait_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = renderer->acquire_semaphores[renderer->active_frame_slot],
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  const VkSemaphoreSubmitInfo signals[] = {
      {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = renderer->timeline,
          .value = signal_value,
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
      {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = window->render_complete[image_index],
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
  };
  const VkSubmitInfo2 submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = 1u,
      .pWaitSemaphoreInfos = &wait_info,
      .commandBufferInfoCount = 1u,
      .pCommandBufferInfos = &command_info,
      .signalSemaphoreInfoCount = ArrayCount(signals),
      .pSignalSemaphoreInfos = signals,
  };
  if (vkQueueSubmit2(vkr_bindless_vulkan_device_queue(renderer->device), 1u,
                     &submit_info, VK_NULL_HANDLE) != VK_SUCCESS)
    return false_v;

  renderer->submit_value = signal_value;
  slot->retire_value = signal_value;
  window->image_last_submit_value[image_index] = signal_value;
  if (vkr_gpu_submit_ring_submit(&renderer->command_ring,
                                 renderer->active_command_slice,
                                 signal_value) != VKR_GPU_SUBMIT_RING_STATUS_OK)
    log_fatal("Vulkan command ring lost a cancelled frame after queue submit");

  const VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1u,
      .pWaitSemaphores = &window->render_complete[image_index],
      .swapchainCount = 1u,
      .pSwapchains = &window->swapchain,
      .pImageIndices = &image_index,
  };
  const VkrBindlessVulkanPresentResult disposition =
      vkr_bindless_vulkan_present_result_classify(vkQueuePresentKHR(
          vkr_bindless_vulkan_device_queue(renderer->device), &present_info));
  if (disposition.present_completion_tracking_required)
    window->image_presented[image_index] = true_v;
  if (disposition.target_recreate_required)
    renderer->target_dirty = true_v;
  if (!disposition.enqueue_state_known || disposition.device_lost ||
      disposition.acquired_image_recovery_required) {
    log_error("Bindless Vulkan could not enqueue cancelled-frame presentation");
    renderer->terminal_failure = true_v;
  }
  slot->acquired_window_image = false_v;
  return true_v;
}

void vkr_bindless_vulkan_renderer_cancel_frame(
    VkrBindlessVulkanRenderer *renderer) {
  if (!renderer)
    return;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const bool8_t submitted =
      slot->acquired_window_image
          ? vkr_bindless_vk_present_cancelled_frame(renderer, slot)
          : false_v;
  if (!submitted)
    vkr_gpu_submit_ring_cancel(&renderer->command_ring,
                               renderer->active_command_slice);
  if (slot->acquired_window_image && !submitted)
    renderer->terminal_failure = true_v;
  renderer->frame_active = false_v;
}

bool8_t vkr_bindless_vulkan_renderer_resize(VkrBindlessVulkanRenderer *renderer,
                                            uint32_t width, uint32_t height,
                                            uint32_t image_count) {
  if (!renderer || renderer->frame_active || renderer->terminal_failure ||
      !width || !height || !image_count ||
      image_count > VKR_BINDLESS_VK_TARGET_IMAGE_MAX) {
    return false_v;
  }
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    renderer->config.width = width;
    renderer->config.height = height;
    renderer->config.image_count = image_count;
    renderer->target_dirty = true_v;
    return true_v;
  }
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_collect_retired_targets(renderer, completed);
  VkrBindlessVkRetiredTargetSet *retired = NULL;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
    if (!renderer->retired_targets[i].occupied) {
      retired = &renderer->retired_targets[i];
      break;
    }
  }
  if (!retired) {
    return false_v;
  }
  VkrBindlessVkTargetSet replacement;
  if (!vkr_bindless_vk_create_target_set(renderer, width, height, image_count,
                                         &replacement)) {
    return false_v;
  }
  retired->targets = renderer->targets;
  retired->retire_value = renderer->submit_value;
  retired->occupied = true_v;
  renderer->targets = replacement;
  renderer->config.width = width;
  renderer->config.height = height;
  renderer->config.image_count = image_count;
  renderer->next_image_index = 0u;
  vkr_bindless_vk_collect_retired_targets(renderer, completed);
  return true_v;
}

bool8_t
vkr_bindless_vulkan_renderer_wait_idle(VkrBindlessVulkanRenderer *renderer) {
  if (!renderer || !renderer->timeline || renderer->submit_value == 0u) {
    return renderer != NULL;
  }
  const VkSemaphoreWaitInfo wait_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1u,
      .pSemaphores = &renderer->timeline,
      .pValues = &renderer->submit_value,
  };
  if (vkWaitSemaphores(vkr_bindless_vk_renderer_device(renderer), &wait_info,
                       UINT64_MAX) != VK_SUCCESS) {
    return false_v;
  }
  vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_collect_retired_targets(renderer, renderer->completed_value);
  vkr_bindless_vk_collect_retired_window_targets(renderer);
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  return true_v;
}

uint64_t vkr_bindless_vulkan_renderer_submit_value(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer ? renderer->submit_value : 0u;
}

uint64_t vkr_bindless_vulkan_renderer_completed_value(
    const VkrBindlessVulkanRenderer *renderer) {
  if (!renderer) {
    return 0u;
  }
  return vkr_bindless_vk_refresh_completed(
      (VkrBindlessVulkanRenderer *)renderer);
}

bool8_t vkr_bindless_vulkan_renderer_get_and_reset_upload_wait_count(
    VkrBindlessVulkanRenderer *renderer, uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count)
    return false_v;
  *out_wait_count = renderer->upload_wait_count;
  renderer->upload_wait_count = 0u;
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_get_and_reset_command_slot_wait_count(
    VkrBindlessVulkanRenderer *renderer, uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count) {
    return false_v;
  }
  *out_wait_count = renderer->command_slot_wait_count;
  renderer->command_slot_wait_count = 0u;
  return true_v;
}

void vkr_bindless_vulkan_renderer_memory_metrics(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanMemoryMetrics *out_metrics) {
  if (!out_metrics)
    return;
  MemZero(out_metrics, sizeof(*out_metrics));
  if (!renderer)
    return;
  VkrBindlessVkMemoryPoolMetrics metrics = {0};
  vkr_bindless_vulkan_memory_pool_get_metrics(renderer->memory_pool, &metrics);
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

void vkr_bindless_vulkan_renderer_heap_metrics(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanHeapMetrics *out_metrics) {
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

void vkr_bindless_vulkan_renderer_wsi_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanWsiStats *out_stats) {
  if (!out_stats)
    return;
  MemZero(out_stats, sizeof(*out_stats));
  if (!renderer)
    return;
  out_stats->reacquire_proofs = renderer->wsi_reacquire_proofs;
  out_stats->retired_swapchains = renderer->wsi_retired_swapchains;
  out_stats->retired_swapchains_collected =
      renderer->wsi_retired_swapchains_collected;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    if (renderer->retired_window_targets[i].occupied)
      out_stats->retired_swapchains_live++;
  }
}

uint32_t vkr_bindless_vulkan_renderer_frame_slot(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer ? renderer->active_frame_slot : 0u;
}

bool8_t vkr_bindless_vulkan_renderer_shader_abi_validated(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer && renderer->shader_abi_validated;
}

const VkrBindlessVkCapabilityProfile *vkr_bindless_vulkan_renderer_profile(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer ? vkr_bindless_vulkan_device_profile(renderer->device) : NULL;
}

VkrAllocator *
vkr_bindless_vulkan_renderer_allocator(VkrBindlessVulkanRenderer *renderer) {
  return renderer ? renderer->allocator : NULL;
}

static bool8_t vkr_bindless_vk_publish_sampled_view(
    VkrBindlessVulkanRenderer *renderer, VkImageView view,
    VkImageLayout image_layout, VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkDescriptorImageInfo image_info = {
      .imageView = view,
      .imageLayout = image_layout,
  };
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .data.pSampledImage = &image_info,
  };
  vkr_bindless_vulkan_device_get_descriptor(renderer->device)(
      vkr_bindless_vk_renderer_device(renderer), &get_info,
      properties->sampledImageDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampled_image_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->resource_descriptor_dirty, &renderer->resource_descriptors,
      layout->sampled_image_offset + (VkDeviceSize)out_handle->index *
                                         properties->sampledImageDescriptorSize,
      properties->sampledImageDescriptorSize);
}

static bool8_t
vkr_bindless_vk_publish_storage_view(VkrBindlessVulkanRenderer *renderer,
                                     VkImageView view,
                                     VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkDescriptorImageInfo image_info = {
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .data.pStorageImage = &image_info,
  };
  vkr_bindless_vulkan_device_get_descriptor(renderer->device)(
      vkr_bindless_vk_renderer_device(renderer), &get_info,
      properties->storageImageDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->storage_image_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->resource_descriptor_dirty, &renderer->resource_descriptors,
      layout->storage_image_offset + (VkDeviceSize)out_handle->index *
                                         properties->storageImageDescriptorSize,
      properties->storageImageDescriptorSize);
}

static bool8_t
vkr_bindless_vk_publish_sampler(VkrBindlessVulkanRenderer *renderer,
                                VkSampler sampler,
                                VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .data.pSampler = &sampler,
  };
  vkr_bindless_vulkan_device_get_descriptor(renderer->device)(
      vkr_bindless_vk_renderer_device(renderer), &get_info,
      properties->samplerDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampler_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->sampler_descriptor_dirty, &renderer->sampler_descriptors,
      layout->sampler_offset +
          (VkDeviceSize)out_handle->index * properties->samplerDescriptorSize,
      properties->samplerDescriptorSize);
}

static bool8_t
vkr_bindless_vk_publish_material_gpu_row(VkrBindlessVulkanRenderer *renderer,
                                         const VkrBindlessVkMaterialGpuRow *row,
                                         VkrGpuSlotHandle *out_handle) {
  if (vkr_gpu_slot_table_publish(renderer->material_slots, row, out_handle) !=
      VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->material_dirty, &renderer->materials,
      (VkDeviceSize)out_handle->index * sizeof(*row), sizeof(*row));
}

static bool8_t vkr_bindless_vk_replace_material_gpu_row(
    VkrBindlessVulkanRenderer *renderer, VkrGpuSlotHandle old_handle,
    const VkrBindlessVkMaterialGpuRow *row, uint64_t old_last_use_submit_value,
    VkrGpuSlotHandle *out_handle) {
  if (vkr_gpu_slot_table_replace(renderer->material_slots, old_handle, row,
                                 old_last_use_submit_value,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->material_dirty, &renderer->materials,
      (VkDeviceSize)out_handle->index * sizeof(*row), sizeof(*row));
}

static bool8_t vkr_bindless_vk_publish_material_row(
    VkrBindlessVulkanRenderer *renderer, uint32_t texture_index,
    uint32_t sampler_index, uint32_t material_id,
    VkrGpuSlotHandle *out_handle) {
  const VkrBindlessVkMaterialGpuRow row = {
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
  };
  return vkr_bindless_vk_publish_material_gpu_row(renderer, &row, out_handle);
}

static VkSamplerAddressMode
vkr_bindless_vk_sampler_address_mode(VkrTextureRepeatMode mode) {
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

static bool8_t vkr_bindless_vk_create_published_sampler(
    VkrBindlessVulkanRenderer *renderer,
    const VkrTextureDescription *description, uint32_t mip_levels,
    VkSampler *out_sampler) {
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
      .addressModeU =
          vkr_bindless_vk_sampler_address_mode(description->u_repeat_mode),
      .addressModeV =
          vkr_bindless_vk_sampler_address_mode(description->v_repeat_mode),
      .addressModeW =
          vkr_bindless_vk_sampler_address_mode(description->w_repeat_mode),
      .maxLod = description->mip_filter == VKR_MIP_FILTER_NONE
                    ? 0.0f
                    : (float32_t)(mip_levels - 1u),
      .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
  };
  return vkCreateSampler(vkr_bindless_vk_renderer_device(renderer), &info, NULL,
                         out_sampler) == VK_SUCCESS;
}

static bool8_t vkr_bindless_vk_sampler_description_equal(
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

static uint32_t vkr_bindless_vk_mip_count(uint32_t width, uint32_t height) {
  uint32_t levels = 1u;
  for (uint32_t extent = Max(width, height); extent > 1u; extent >>= 1u)
    levels++;
  return levels;
}

static bool8_t
vkr_bindless_vk_acquire_sampler(VkrBindlessVulkanRenderer *renderer,
                                const VkrTextureDescription *description,
                                uint32_t mip_levels,
                                uint32_t *out_record_index) {
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    VkrBindlessVkPublishedSampler *record = &renderer->published_samplers[i];
    if (record->live && vkr_bindless_vk_sampler_description_equal(
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
  if (!vkr_bindless_vk_create_published_sampler(renderer, description,
                                                mip_levels, &sampler) ||
      !vkr_bindless_vk_publish_sampler(renderer, sampler, &slot)) {
    if (sampler)
      vkDestroySampler(vkr_bindless_vk_renderer_device(renderer), sampler,
                       NULL);
    return false_v;
  }
  renderer->published_samplers[free_index] = (VkrBindlessVkPublishedSampler){
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

static bool8_t
vkr_bindless_vk_release_sampler(VkrBindlessVulkanRenderer *renderer,
                                uint32_t record_index,
                                uint64_t last_use_submit_value) {
  if (record_index >= renderer->config.sampler_capacity)
    return false_v;
  VkrBindlessVkPublishedSampler *record =
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

static void
vkr_bindless_vk_collect_samplers(VkrBindlessVulkanRenderer *renderer,
                                 uint64_t completed) {
  (void)vkr_gpu_slot_table_collect(renderer->sampler_slots, completed, NULL);
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    VkrBindlessVkPublishedSampler *record = &renderer->published_samplers[i];
    if (!record->pending_retire || record->last_use_submit_value > completed)
      continue;
    vkDestroySampler(vkr_bindless_vk_renderer_device(renderer), record->sampler,
                     NULL);
    MemZero(record, sizeof(*record));
  }
}

static void vkr_bindless_vk_prepare_writable_initialization(
    uint32_t texture_record_index, VkrBindlessVkImage *image,
    VkrBindlessVkPendingTextureInitialization *out_initialization) {
  *out_initialization = (VkrBindlessVkPendingTextureInitialization){
      .texture_record_index = texture_record_index,
      .writable = true_v,
  };
  image->layout = VK_IMAGE_LAYOUT_GENERAL;
}

static VkrBindlessVkPublishedTexture *
vkr_bindless_vk_published_texture(VkrBindlessVulkanRenderer *renderer,
                                  VkrTextureHandle handle,
                                  uint32_t *out_index) {
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity)
    return NULL;
  const uint32_t index = handle.id - 1u;
  VkrBindlessVkPublishedTexture *texture = &renderer->published_textures[index];
  if (!texture->live || texture->handle.generation != handle.generation)
    return NULL;
  if (out_index)
    *out_index = index;
  return texture;
}

static bool8_t vkr_bindless_vk_retire_unreferenced_texture(
    VkrBindlessVulkanRenderer *renderer, VkrBindlessVkPublishedTexture *texture,
    uint64_t completed) {
  if (!texture->pending_retire || texture->material_reference_count != 0u ||
      texture->initialization_pending ||
      texture->last_use_submit_value > completed)
    return true_v;

  VkrBindlessVkPublishedSampler *sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  if (vkr_gpu_slot_table_can_retire(renderer->sampled_image_slots,
                                    texture->sampled_slot) !=
          VKR_GPU_SLOT_STATUS_OK ||
      (texture->has_storage_slot &&
       vkr_gpu_slot_table_can_retire(renderer->storage_image_slots,
                                     texture->storage_slot) !=
           VKR_GPU_SLOT_STATUS_OK) ||
      (sampler->reference_count == 1u &&
       vkr_gpu_slot_table_can_retire(renderer->sampler_slots, sampler->slot) !=
           VKR_GPU_SLOT_STATUS_OK)) {
    log_error("Bindless Vulkan texture retirement could not reserve every "
              "descriptor retirement; preserving the complete publication");
    return false_v;
  }

  if (vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                texture->sampled_slot,
                                completed) != VKR_GPU_SLOT_STATUS_OK ||
      (texture->has_storage_slot &&
       vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                 texture->storage_slot,
                                 completed) != VKR_GPU_SLOT_STATUS_OK) ||
      !vkr_bindless_vk_release_sampler(renderer, texture->sampler_record_index,
                                       completed) ||
      vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK) {
    log_error("Bindless Vulkan failed to retire a validated texture "
              "publication; preserving the native texture");
    return false_v;
  }
  if (!vkr_bindless_vk_retire_allocation(renderer, &texture->image.allocation,
                                         completed))
    log_fatal("Bindless Vulkan failed to retire completed texture memory");
  vkr_bindless_vk_destroy_image(renderer, &texture->image);
  MemZero(texture, sizeof(*texture));
  return true_v;
}

static void
vkr_bindless_vk_collect_asset_publications(VkrBindlessVulkanRenderer *renderer,
                                           uint64_t completed) {
  (void)vkr_gpu_slot_table_collect(renderer->material_slots, completed, NULL);
  vkr_bindless_vk_collect_samplers(renderer, completed);
  for (uint32_t i = 0; i < renderer->retired_staging_buffer_capacity; ++i) {
    VkrBindlessVkRetiredStagingBuffer *retired =
        &renderer->retired_staging_buffers[i];
    if (!retired->occupied || retired->retire_value > completed)
      continue;
    vkr_bindless_vk_destroy_buffer(renderer, &retired->buffer);
    renderer->staging_buffer_count--;
    MemZero(retired, sizeof(*retired));
  }
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrBindlessVkRetiredMaterial *retired = &renderer->retired_materials[i];
    if (!retired->occupied || retired->retire_value > completed)
      continue;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (retired->texture_record_indices[texture_slot] == UINT32_MAX)
        continue;
      VkrBindlessVkPublishedTexture *texture =
          &renderer->published_textures
               [retired->texture_record_indices[texture_slot]];
      if (texture->material_reference_count > 0u)
        texture->material_reference_count--;
    }
    MemZero(retired, sizeof(*retired));
  }
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[i];
    if (!geometry->pending_retire ||
        geometry->last_use_submit_value > completed)
      continue;
    vkr_bindless_vk_destroy_buffer(renderer, &geometry->indices);
    vkr_bindless_vk_destroy_buffer(renderer, &geometry->vertices);
    MemZero(geometry, sizeof(*geometry));
  }
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i)
    vkr_bindless_vk_retire_unreferenced_texture(
        renderer, &renderer->published_textures[i], completed);
  vkr_bindless_vk_collect_samplers(renderer, completed);
}

static VkrBindlessVkRetiredMaterial *
vkr_bindless_vk_reserve_material_retirement(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    if (!renderer->retired_materials[i].occupied)
      return &renderer->retired_materials[i];
  }
  return NULL;
}

static bool8_t vkr_bindless_vk_create_published_buffer(
    VkrBindlessVulkanRenderer *renderer, const void *data, uint64_t size,
    VkBufferUsageFlags usage, VkPipelineStageFlags2 destination_stage,
    VkAccessFlags2 destination_access, uint32_t geometry_record_index,
    VkrBindlessVkBuffer *out_buffer,
    VkrBindlessVkPendingBufferInitialization *out_initialization) {
  if (!data || !size || !out_buffer || !out_initialization ||
      !vkr_bindless_vk_create_buffer(
          renderer, VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE, size,
          usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, out_buffer))
    return false_v;
  VkrBindlessVkBuffer staging = {0};
  if (!vkr_bindless_vk_create_buffer(
          renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging)) {
    vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
    return false_v;
  }
  MemCopy(staging.allocation.mapped, data, size);
  if (!vkr_bindless_vk_flush(renderer, &staging.allocation, 0u, size)) {
    vkr_bindless_vk_destroy_buffer(renderer, &staging);
    vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
    return false_v;
  }
  *out_initialization = (VkrBindlessVkPendingBufferInitialization){
      .staging = staging,
      .destination = out_buffer->handle,
      .size = size,
      .destination_stage = destination_stage,
      .destination_access = destination_access,
      .geometry_record_index = geometry_record_index,
  };
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_geometry(void *state, VkrGeometryHandle handle,
                                       const VkrGeometryConfig *geometry) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !geometry || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity ||
      !geometry->vertices || !geometry->indices || !geometry->vertex_count ||
      !geometry->index_count ||
      renderer->pending_buffer_initialization_count >
          renderer->pending_buffer_initialization_capacity - 2u ||
      renderer->staging_buffer_count >
          renderer->retired_staging_buffer_capacity - 2u ||
      (geometry->vertex_size != sizeof(VkrVertex3d) &&
       geometry->vertex_size != sizeof(VkrVertex2d)) ||
      (geometry->index_size != sizeof(uint16_t) &&
       geometry->index_size != sizeof(uint32_t)))
    return false_v;
  VkrBindlessVkPublishedGeometry *record =
      &renderer->published_geometries[handle.id - 1u];
  if (record->live || record->pending_retire)
    return false_v;
  const uint64_t vertex_size =
      (uint64_t)sizeof(VkrVertex3d) * geometry->vertex_count;
  const uint64_t index_size =
      (uint64_t)geometry->index_size * geometry->index_count;
  VkrBindlessVkPublishedGeometry pending = {
      .handle = handle,
      .vertex_count = geometry->vertex_count,
      .index_count = geometry->index_count,
      .index_type = geometry->index_size == sizeof(uint16_t)
                        ? VK_INDEX_TYPE_UINT16
                        : VK_INDEX_TYPE_UINT32,
  };
  VkrBindlessVkPendingBufferInitialization initializations[2] = {0};
  const VkrVertex3d *vertices = geometry->vertices;
  VkrVertex3d *converted = NULL;
  if (geometry->vertex_size == sizeof(VkrVertex2d)) {
    converted = vkr_allocator_alloc(renderer->allocator, vertex_size,
                                    VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!converted)
      return false_v;
    const VkrVertex2d *source = geometry->vertices;
    for (uint32_t i = 0; i < geometry->vertex_count; ++i) {
      converted[i] = (VkrVertex3d){
          .position = {source[i].position.x, source[i].position.y, 0.0f},
          .normal = {0.0f, 0.0f, 1.0f},
          .texcoord = source[i].texcoord,
          .colour = {1.0f, 1.0f, 1.0f, 1.0f},
          .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
      };
    }
    vertices = converted;
  }
  const bool8_t created =
      vkr_bindless_vk_create_published_buffer(
          renderer, vertices, vertex_size,
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          VK_ACCESS_2_SHADER_STORAGE_READ_BIT, handle.id - 1u,
          &pending.vertices, &initializations[0]) &&
      vkr_bindless_vk_create_published_buffer(
          renderer, geometry->indices, index_size,
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
          VK_ACCESS_2_INDEX_READ_BIT, handle.id - 1u, &pending.indices,
          &initializations[1]);
  if (converted)
    vkr_allocator_free(renderer->allocator, converted, vertex_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!created) {
    vkr_bindless_vk_destroy_buffer(renderer, &initializations[1].staging);
    vkr_bindless_vk_destroy_buffer(renderer, &initializations[0].staging);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.indices);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.vertices);
    return false_v;
  }
  if (renderer->pending_buffer_initialization_count >
          renderer->pending_buffer_initialization_capacity - 2u ||
      renderer->staging_buffer_count >
          renderer->retired_staging_buffer_capacity - 2u) {
    vkr_bindless_vk_destroy_buffer(renderer, &initializations[1].staging);
    vkr_bindless_vk_destroy_buffer(renderer, &initializations[0].staging);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.indices);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.vertices);
    return false_v;
  }
  pending.pending_initialization_count = 2u;
  pending.live = true_v;
  *record = pending;
  renderer->pending_buffer_initializations
      [renderer->pending_buffer_initialization_count++] = initializations[0];
  renderer->pending_buffer_initializations
      [renderer->pending_buffer_initialization_count++] = initializations[1];
  renderer->staging_buffer_count += 2u;
  return true_v;
}

static bool8_t vkr_bindless_vk_asset_publish_writable_texture(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !description || renderer->frame_active || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity ||
      renderer->pending_texture_initialization_count >=
          renderer->config.sampled_image_capacity ||
      description->id != handle.id ||
      description->generation != handle.generation || !description->width ||
      !description->height ||
      (description->sample_count != 0u &&
       description->sample_count != VKR_SAMPLE_COUNT_1) ||
      (description->type != VKR_TEXTURE_TYPE_2D &&
       description->type != VKR_TEXTURE_TYPE_CUBE_MAP) ||
      (description->type == VKR_TEXTURE_TYPE_CUBE_MAP &&
       description->width != description->height))
    return false_v;
  VkrBindlessVkPublishedTexture *record =
      &renderer->published_textures[handle.id - 1u];
  if (record->live || record->pending_retire)
    return false_v;
  const VkFormat format = vkr_bindless_vk_texture_format(description->format);
  if (format == VK_FORMAT_UNDEFINED)
    return false_v;
  VkFormatProperties properties = {0};
  vkGetPhysicalDeviceFormatProperties(
      vkr_bindless_vulkan_device_physical(renderer->device), format,
      &properties);
  const VkFormatFeatureFlags required =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  if ((properties.optimalTilingFeatures & required) != required)
    return false_v;
  const bool8_t cube = description->type == VKR_TEXTURE_TYPE_CUBE_MAP;
  const uint32_t mip_levels =
      description->mip_filter == VKR_MIP_FILTER_NONE
          ? 1u
          : vkr_bindless_vk_mip_count(description->width, description->height);
  VkrBindlessVkPublishedTexture pending = {
      .handle = handle,
      .has_storage_slot = true_v,
  };
  VkrBindlessVkPendingTextureInitialization initialization = {0};
  const bool8_t image_created = vkr_bindless_vk_create_image_ex(
      renderer, description->width, description->height, mip_levels,
      cube ? 6u : 1u, format, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
      cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      &pending.image);
  const bool8_t sampler_acquired =
      image_created &&
      vkr_bindless_vk_acquire_sampler(renderer, description, mip_levels,
                                      &pending.sampler_record_index);
  const bool8_t sampled_published =
      sampler_acquired && vkr_bindless_vk_publish_sampled_view(
                              renderer, pending.image.view,
                              VK_IMAGE_LAYOUT_GENERAL, &pending.sampled_slot);
  const bool8_t storage_published =
      sampled_published &&
      vkr_bindless_vk_publish_storage_view(renderer, pending.image.view,
                                           &pending.storage_slot);
  if (storage_published)
    vkr_bindless_vk_prepare_writable_initialization(
        handle.id - 1u, &pending.image, &initialization);
  const bool8_t initialization_queued =
      storage_published &&
      vkr_bindless_vk_enqueue_texture_initialization(renderer, &initialization);
  if (!initialization_queued) {
    VkrGpuSlotTable *tables[] = {renderer->sampled_image_slots,
                                 renderer->storage_image_slots};
    const VkrGpuSlotHandle handles[] = {pending.sampled_slot,
                                        pending.storage_slot};
    for (uint32_t i = 0; i < ArrayCount(tables); ++i) {
      if (handles[i].generation) {
        (void)vkr_gpu_slot_table_retire(tables[i], handles[i],
                                        renderer->completed_value);
        (void)vkr_gpu_slot_table_collect(tables[i], renderer->completed_value,
                                         NULL);
      }
    }
    if (sampler_acquired) {
      (void)vkr_bindless_vk_release_sampler(
          renderer, pending.sampler_record_index, renderer->completed_value);
      vkr_bindless_vk_collect_samplers(renderer, renderer->completed_value);
    }
    vkr_bindless_vk_destroy_image(renderer, &pending.image);
    return false_v;
  }
  pending.initialization_pending = true_v;
  pending.live = true_v;
  *record = pending;
  return true_v;
}

static void
vkr_bindless_vk_material_row_set_sampler(VkrBindlessVkMaterialGpuRow *row,
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

static bool8_t vkr_bindless_vk_asset_update_texture_sampler(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  VkrBindlessVulkanRenderer *renderer = state;
  uint32_t texture_record_index = 0u;
  VkrBindlessVkPublishedTexture *texture = vkr_bindless_vk_published_texture(
      renderer, handle, &texture_record_index);
  if (!texture || !description || renderer->frame_active ||
      description->id != handle.id ||
      description->generation != handle.generation)
    return false_v;
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
  VkrBindlessVkPublishedSampler *old_sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  if (vkr_bindless_vk_sampler_description_equal(
          &old_sampler->description, old_sampler->mip_levels, description,
          texture->image.mip_levels))
    return true_v;

  uint32_t dependent_material_count = 0u;
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    const VkrBindlessVkPublishedMaterial *material =
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
  if (!vkr_bindless_vk_acquire_sampler(renderer, description,
                                       texture->image.mip_levels,
                                       &replacement_sampler_index))
    return false_v;
  const uint32_t replacement_slot =
      renderer->published_samplers[replacement_sampler_index].slot.index;
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrBindlessVkPublishedMaterial *material =
        &renderer->published_materials[i];
    if (!material->live)
      continue;
    VkrBindlessVkMaterialGpuRow replacement_row = material->row;
    bool8_t dependent = false_v;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (material->texture_record_indices[texture_slot] !=
          texture_record_index)
        continue;
      vkr_bindless_vk_material_row_set_sampler(&replacement_row, texture_slot,
                                               replacement_slot);
      dependent = true_v;
    }
    if (!dependent)
      continue;
    VkrGpuSlotHandle replacement_material_slot = {0};
    if (!vkr_bindless_vk_replace_material_gpu_row(
            renderer, material->slot, &replacement_row, renderer->submit_value,
            &replacement_material_slot)) {
      log_error("Bindless Vulkan failed a preflighted dependent material "
                "sampler republication");
      renderer->terminal_failure = true_v;
      return false_v;
    }
    material->slot = replacement_material_slot;
    material->row = replacement_row;
  }
  const uint32_t old_sampler_index = texture->sampler_record_index;
  texture->sampler_record_index = replacement_sampler_index;
  if (!vkr_bindless_vk_release_sampler(renderer, old_sampler_index,
                                       renderer->submit_value)) {
    log_error("Bindless Vulkan failed a preflighted sampler retirement");
    renderer->terminal_failure = true_v;
    return false_v;
  }
  vkr_bindless_vk_collect_samplers(renderer, completed);
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_loaded_mesh(void *state, VkrGeometryHandle handle,
                                          const VkrMeshLoaderResult *mesh) {
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
  return vkr_bindless_vk_asset_publish_geometry(state, handle, &geometry);
}

static bool8_t
vkr_bindless_vk_activate_geometry(VkrBindlessVulkanRenderer *renderer,
                                  VkrGeometryHandle handle) {
  if (!renderer || renderer->frame_active ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;
  VkDeviceAddress vertices =
      renderer->upload.address + VKR_BINDLESS_VK_VERTEX_OFFSET;
  renderer->active_geometry = false_v;
  renderer->active_geometry_record_index = 0u;
  if (handle.id) {
    if (handle.id > renderer->config.sampled_image_capacity)
      return false_v;
    const uint32_t index = handle.id - 1u;
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[index];
    if (!geometry->live || geometry->handle.generation != handle.generation)
      return false_v;
    vertices = geometry->vertices.address;
    renderer->active_geometry = true_v;
    renderer->active_geometry_record_index = index;
  }
  VkrBindlessVkDrawRoot *root =
      (VkrBindlessVkDrawRoot *)((uint8_t *)renderer->upload.allocation.mapped +
                                VKR_BINDLESS_VK_ROOT_OFFSET);
  root->vertices = vertices;
  return vkr_bindless_vk_flush(renderer, &renderer->upload.allocation,
                               VKR_BINDLESS_VK_ROOT_OFFSET, sizeof(*root));
}

static bool8_t
vkr_bindless_vk_asset_unpublish_geometry(void *state,
                                         VkrGeometryHandle handle) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity)
    return false_v;
  VkrBindlessVkPublishedGeometry *record =
      &renderer->published_geometries[handle.id - 1u];
  if (!record->live || record->handle.generation != handle.generation ||
      (renderer->frame_active && record->pending_initialization_count))
    return false_v;
  if (record->pending_initialization_count)
    vkr_bindless_vk_discard_geometry_initializations(renderer, handle.id - 1u);
  if (!vkr_bindless_vk_retire_buffer(renderer, &record->vertices,
                                     renderer->submit_value) ||
      !vkr_bindless_vk_retire_buffer(renderer, &record->indices,
                                     renderer->submit_value))
    log_fatal("Bindless Vulkan failed to retire geometry memory");
  record->live = false_v;
  record->pending_retire = true_v;
  record->last_use_submit_value = renderer->submit_value;
  vkr_bindless_vk_collect_asset_publications(
      renderer, vkr_bindless_vk_refresh_completed(renderer));
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_texture(void *state, VkrTextureHandle handle,
                                      const VkrTexturePreparedLoad *prepared) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !prepared || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity ||
      renderer->pending_texture_initialization_count >=
          renderer->config.sampled_image_capacity ||
      renderer->staging_buffer_count >=
          renderer->retired_staging_buffer_capacity)
    return false_v;
  VkrBindlessVkPublishedTexture *record =
      &renderer->published_textures[handle.id - 1u];
  if (record->live || record->pending_retire) {
    log_error("Bindless Vulkan texture %u:%u is already published", handle.id,
              handle.generation);
    return false_v;
  }
  VkrBindlessVkPublishedTexture pending = {.handle = handle};
  VkrBindlessVkPendingTextureInitialization initialization = {0};
  const bool8_t image_uploaded = vkr_bindless_vk_upload_prepared_texture(
      renderer, prepared, handle.id - 1u, &pending.image, &initialization);
  const bool8_t sampler_acquired =
      image_uploaded &&
      vkr_bindless_vk_acquire_sampler(renderer, &prepared->description,
                                      prepared->upload_mip_levels,
                                      &pending.sampler_record_index);
  const bool8_t sampled_published =
      sampler_acquired &&
      vkr_bindless_vk_publish_sampled_view(
          renderer, pending.image.view,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &pending.sampled_slot);
  const bool8_t initialization_queued =
      sampled_published &&
      vkr_bindless_vk_enqueue_texture_initialization(renderer, &initialization);
  if (!initialization_queued) {
    log_error("Bindless Vulkan texture %u:%u publication failed at %s",
              handle.id, handle.generation,
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
      (void)vkr_bindless_vk_release_sampler(
          renderer, pending.sampler_record_index, renderer->completed_value);
      vkr_bindless_vk_collect_samplers(renderer, renderer->completed_value);
    }
    if (image_uploaded)
      vkr_bindless_vk_release_texture_initialization(renderer, &initialization);
    vkr_bindless_vk_destroy_image(renderer, &pending.image);
    return false_v;
  }
  pending.initialization_pending = true_v;
  pending.live = true_v;
  *record = pending;
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_unpublish_texture(void *state, VkrTextureHandle handle) {
  VkrBindlessVulkanRenderer *renderer = state;
  VkrBindlessVkPublishedTexture *texture =
      vkr_bindless_vk_published_texture(renderer, handle, NULL);
  if (!texture)
    return false_v;
  texture->live = false_v;
  texture->pending_retire = true_v;
  texture->last_use_submit_value = renderer->submit_value;
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_retire_unreferenced_texture(renderer, texture, completed);
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_material(void *state, VkrMaterialHandle handle,
                                       const VkrMaterial *material) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !material || handle.id == 0u ||
      handle.id > renderer->config.material_record_capacity ||
      material->id != handle.id || material->generation != handle.generation)
    return false_v;
  VkrBindlessVkPublishedMaterial *record =
      &renderer->published_materials[handle.id - 1u];
  if (record->live && record->handle.generation != handle.generation)
    return false_v;
  VkrBindlessVkRetiredMaterial *retirement =
      record->live ? vkr_bindless_vk_reserve_material_retirement(renderer)
                   : NULL;
  if (record->live && !retirement)
    return false_v;

  static const VkrTextureSlot row_slots[4] = {
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
    VkrBindlessVkPublishedTexture *texture = vkr_bindless_vk_published_texture(
        renderer, source->handle, &record_index);
    if (!texture)
      return false_v;
    texture_indices[i] = texture->sampled_slot.index;
    sampler_indices[i] =
        renderer->published_samplers[texture->sampler_record_index].slot.index;
    texture_record_indices[i] = record_index;
  }
  if (material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled)
    material_flags |= VKR_BINDLESS_VK_MATERIAL_TEXTURE_NORMAL;
  if (material->textures[VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS].enabled)
    material_flags |= VKR_BINDLESS_VK_MATERIAL_TEXTURE_ORM;
  if (material->textures[VKR_TEXTURE_SLOT_EMISSION].enabled)
    material_flags |= VKR_BINDLESS_VK_MATERIAL_TEXTURE_EMISSIVE;
  const Vec4 tint = material->material_type == VKR_MATERIAL_TYPE_PBR
                        ? material->pbr.base_color
                        : material->phong.diffuse_color;
  const VkrBindlessVkMaterialGpuRow row = {
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
  };
  VkrGpuSlotHandle new_slot = {0};
  const bool8_t row_published =
      record->live
          ? vkr_bindless_vk_replace_material_gpu_row(
                renderer, record->slot, &row, renderer->submit_value, &new_slot)
          : vkr_bindless_vk_publish_material_gpu_row(renderer, &row, &new_slot);
  if (!row_published)
    return false_v;
  for (uint32_t i = 0; i < 4u; ++i) {
    if (texture_record_indices[i] != UINT32_MAX)
      renderer->published_textures[texture_record_indices[i]]
          .material_reference_count++;
  }
  if (record->live) {
    *retirement = (VkrBindlessVkRetiredMaterial){
        .retire_value = renderer->submit_value,
        .occupied = true_v,
    };
    MemCopy(retirement->texture_record_indices, record->texture_record_indices,
            sizeof(retirement->texture_record_indices));
  }
  *record = (VkrBindlessVkPublishedMaterial){
      .handle = handle,
      .slot = new_slot,
      .row = row,
      .live = true_v,
  };
  MemCopy(record->texture_record_indices, texture_record_indices,
          sizeof(record->texture_record_indices));
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_unpublish_material(void *state,
                                         VkrMaterialHandle handle) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.material_record_capacity)
    return false_v;
  VkrBindlessVkPublishedMaterial *record =
      &renderer->published_materials[handle.id - 1u];
  if (!record->live || record->handle.generation != handle.generation)
    return false_v;
  VkrBindlessVkRetiredMaterial *retirement =
      vkr_bindless_vk_reserve_material_retirement(renderer);
  if (!retirement || vkr_gpu_slot_table_retire(
                         renderer->material_slots, record->slot,
                         renderer->submit_value) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  *retirement = (VkrBindlessVkRetiredMaterial){
      .retire_value = renderer->submit_value,
      .occupied = true_v,
  };
  MemCopy(retirement->texture_record_indices, record->texture_record_indices,
          sizeof(retirement->texture_record_indices));
  record->live = false_v;
  return true_v;
}

void vkr_bindless_vulkan_renderer_get_asset_publisher(
    VkrBindlessVulkanRenderer *renderer, VkrAssetPublisher *out_publisher) {
  if (!out_publisher)
    return;
  *out_publisher = renderer
                       ? (VkrAssetPublisher){
                             .state = renderer,
                              .publish_geometry =
                                  vkr_bindless_vk_asset_publish_geometry,
                              .publish_loaded_mesh =
                                  vkr_bindless_vk_asset_publish_loaded_mesh,
                              .unpublish_geometry =
                                  vkr_bindless_vk_asset_unpublish_geometry,
                              .publish_texture =
                                  vkr_bindless_vk_asset_publish_texture,
                              .publish_writable_texture =
                                  vkr_bindless_vk_asset_publish_writable_texture,
                              .update_texture_sampler =
                                  vkr_bindless_vk_asset_update_texture_sampler,
                             .unpublish_texture =
                                 vkr_bindless_vk_asset_unpublish_texture,
                             .publish_material =
                                 vkr_bindless_vk_asset_publish_material,
                             .unpublish_material =
                                 vkr_bindless_vk_asset_unpublish_material,
                         }
                       : (VkrAssetPublisher){0};
}

static void
vkr_bindless_vk_drain_asset_publications(VkrBindlessVulkanRenderer *renderer) {
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrBindlessVkPublishedMaterial *material =
        &renderer->published_materials[i];
    if (material->live)
      (void)vkr_bindless_vk_asset_unpublish_material(renderer,
                                                     material->handle);
  }
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedTexture *texture = &renderer->published_textures[i];
    if (texture->live)
      (void)vkr_bindless_vk_asset_unpublish_texture(renderer, texture->handle);
  }
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[i];
    if (geometry->live)
      (void)vkr_bindless_vk_asset_unpublish_geometry(renderer,
                                                     geometry->handle);
  }
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
}

static bool8_t
vkr_bindless_vk_result_is_sentinel(const VkrBindlessVulkanResult *result) {
  return result->color[0] == 37u && result->color[1] == 91u &&
         result->color[2] == 173u && result->color[3] == 255u &&
         result->identifier == 0xffad5b25u;
}

static bool8_t vkr_bindless_vk_run_asset_publisher_fixture(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanPublicationTestResult *out_result) {
  const uint8_t pixel[4] = {37u, 91u, 173u, 255u};
  VkrTextureUploadRegion region = {
      .width = 1u,
      .height = 1u,
      .depth = 1u,
      .byte_size = sizeof(pixel),
  };
  VkrTexturePreparedLoad prepared = {
      .description = {.width = 1u,
                      .height = 1u,
                      .channels = 4u,
                      .type = VKR_TEXTURE_TYPE_2D,
                      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
                      .sample_count = VKR_SAMPLE_COUNT_1,
                      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .min_filter = VKR_FILTER_NEAREST,
                      .mag_filter = VKR_FILTER_NEAREST,
                      .mip_filter = VKR_MIP_FILTER_NONE},
      .upload_data = (uint8_t *)pixel,
      .upload_data_size = sizeof(pixel),
      .upload_regions = &region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
  };
  const VkrTextureHandle shared_texture = {
      .id = renderer->config.sampled_image_capacity - 1u, .generation = 1u};
  const VkrTextureHandle replacement_texture = {
      .id = renderer->config.sampled_image_capacity, .generation = 1u};
  const VkrTextureHandle writable_texture = {
      .id = renderer->config.sampled_image_capacity - 2u, .generation = 1u};
  const VkrGeometryHandle loaded_geometry = {
      .id = renderer->config.sampled_image_capacity, .generation = 1u};
  const VkrMaterialHandle material_a = {
      .id = renderer->config.material_record_capacity - 1u, .generation = 1u};
  const VkrMaterialHandle material_b = {
      .id = renderer->config.material_record_capacity, .generation = 1u};
  VkrMaterial material = {
      .material_type = VKR_MATERIAL_TYPE_PBR,
      .pbr.base_color = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
  };
  const VkrTextureSlot material_texture_slots[] = {
      VKR_TEXTURE_SLOT_DIFFUSE,
      VKR_TEXTURE_SLOT_NORMAL,
      VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS,
      VKR_TEXTURE_SLOT_EMISSION,
  };
  for (uint32_t i = 0; i < ArrayCount(material_texture_slots); ++i) {
    const VkrTextureSlot slot = material_texture_slots[i];
    material.textures[slot] = (VkrMaterialTexture){
        .handle = shared_texture,
        .slot = slot,
        .enabled = true_v,
    };
  }
  const VkrVertex3d vertices[] = {
      {.position = {-1.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {3.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 2.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {-1.0f, 3.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 2.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
  };
  const uint32_t indices[] = {0u, 1u, 2u};
  VkrMeshLoaderSubmeshRange submesh = {.index_count = ArrayCount(indices)};
  const VkrMeshLoaderResult mesh = {
      .has_mesh_buffer = true_v,
      .mesh_buffer = {.vertex_size = sizeof(vertices[0]),
                      .vertex_count = ArrayCount(vertices),
                      .vertices = (void *)vertices,
                      .index_size = sizeof(indices[0]),
                      .index_count = ArrayCount(indices),
                      .indices = (void *)indices},
      .submeshes = {.length = 1u, .data = &submesh},
  };
  VkrTextureDescription writable_description = prepared.description;
  writable_description.id = writable_texture.id;
  writable_description.generation = writable_texture.generation;
  writable_description.width = 4u;
  writable_description.height = 4u;
  if (!vkr_bindless_vk_asset_publish_loaded_mesh(renderer, loaded_geometry,
                                                 &mesh) ||
      !vkr_bindless_vk_activate_geometry(renderer, loaded_geometry) ||
      !vkr_bindless_vk_asset_publish_writable_texture(
          renderer, writable_texture, &writable_description))
    return false_v;
  writable_description.u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT;
  if (!vkr_bindless_vk_asset_update_texture_sampler(renderer, writable_texture,
                                                    &writable_description))
    return false_v;
  VkrGpuSlotTableMetrics sampler_after_update = {0};
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &sampler_after_update);
  if (!vkr_bindless_vk_asset_update_texture_sampler(renderer, writable_texture,
                                                    &writable_description))
    return false_v;
  VkrGpuSlotTableMetrics sampler_after_duplicate = {0};
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &sampler_after_duplicate);
  if (sampler_after_duplicate.slots_published !=
          sampler_after_update.slots_published ||
      !vkr_bindless_vk_asset_publish_texture(renderer, shared_texture,
                                             &prepared))
    return false_v;

  VkrMaterial material_a_data = material;
  material_a_data.id = material_a.id;
  material_a_data.generation = material_a.generation;
  VkrMaterial invalid_material = material_a_data;
  invalid_material.textures[VKR_TEXTURE_SLOT_NORMAL].handle.generation++;
  if (vkr_bindless_vk_asset_publish_material(renderer, material_a,
                                             &invalid_material) ||
      !vkr_bindless_vk_asset_publish_material(renderer, material_a,
                                              &material_a_data))
    return false_v;
  VkrMaterial material_b_data = material;
  material_b_data.id = material_b.id;
  material_b_data.generation = material_b.generation;
  if (!vkr_bindless_vk_asset_publish_material(renderer, material_b,
                                              &material_b_data))
    return false_v;
  const uint32_t expected_material_flags =
      VKR_BINDLESS_VK_MATERIAL_TEXTURE_NORMAL |
      VKR_BINDLESS_VK_MATERIAL_TEXTURE_ORM |
      VKR_BINDLESS_VK_MATERIAL_TEXTURE_EMISSIVE;
  if (renderer->published_materials[material_a.id - 1u].row.flags !=
          expected_material_flags ||
      renderer->published_materials[material_b.id - 1u].row.flags !=
          expected_material_flags)
    return false_v;

  const VkrGpuSlotHandle material_a_before_sampler_update =
      renderer->published_materials[material_a.id - 1u].slot;
  const VkrGpuSlotHandle material_b_before_sampler_update =
      renderer->published_materials[material_b.id - 1u].slot;
  VkrTextureDescription shared_sampler_description = prepared.description;
  shared_sampler_description.id = shared_texture.id;
  shared_sampler_description.generation = shared_texture.generation;
  shared_sampler_description.u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT;
  if (!vkr_bindless_vk_asset_update_texture_sampler(
          renderer, shared_texture, &shared_sampler_description))
    return false_v;
  const VkrBindlessVkPublishedTexture *shared_texture_record =
      &renderer->published_textures[shared_texture.id - 1u];
  const uint32_t shared_sampler_slot =
      renderer->published_samplers[shared_texture_record->sampler_record_index]
          .slot.index;
  out_result->shared_sampler_reused =
      shared_texture_record->sampler_record_index ==
      renderer->published_textures[writable_texture.id - 1u]
          .sampler_record_index;
  out_result->dependent_materials_republished =
      (renderer->published_materials[material_a.id - 1u].slot.index !=
           material_a_before_sampler_update.index ||
       renderer->published_materials[material_a.id - 1u].slot.generation !=
           material_a_before_sampler_update.generation) &&
      (renderer->published_materials[material_b.id - 1u].slot.index !=
           material_b_before_sampler_update.index ||
       renderer->published_materials[material_b.id - 1u].slot.generation !=
           material_b_before_sampler_update.generation) &&
      renderer->published_materials[material_a.id - 1u]
              .row.base_color_sampler == shared_sampler_slot &&
      renderer->published_materials[material_b.id - 1u]
              .row.base_color_sampler == shared_sampler_slot;
  if (!out_result->shared_sampler_reused ||
      !out_result->dependent_materials_republished)
    return false_v;

  VkrFrameSetup setup = {0};
  VkrRenderPacket packet = {.packet_version = VKR_RENDER_PACKET_VERSION};
  VkrBindlessVulkanResult submitted[3] = {0};
  renderer->active_material_index =
      renderer->published_materials[material_a.id - 1u].slot.index;
  uint64_t expected_staging_retirements =
      renderer->pending_buffer_initialization_count;
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    if (renderer->pending_texture_initializations[i].staging.handle)
      expected_staging_retirements++;
  }
  VkrBindlessVulkanMemoryMetrics memory_before_submit = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_before_submit);
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 2001u, &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[0]))
    return false_v;
  VkrBindlessVulkanMemoryMetrics memory_after_submit = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_after_submit);
  out_result->staging_retired_at_submit =
      expected_staging_retirements > 0u &&
      memory_after_submit.aggregate.retired_allocations ==
          memory_before_submit.aggregate.retired_allocations +
              expected_staging_retirements;
  if (!out_result->staging_retired_at_submit ||
      !vkr_bindless_vk_asset_unpublish_material(renderer, material_a) ||
      !vkr_bindless_vk_asset_unpublish_texture(renderer, shared_texture))
    return false_v;

  renderer->active_material_index =
      renderer->published_materials[material_b.id - 1u].slot.index;
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 2002u, &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[1]))
    return false_v;
  out_result->shared_resource_survived =
      renderer->published_textures[shared_texture.id - 1u]
              .material_reference_count > 0u &&
      renderer->published_textures[shared_texture.id - 1u].pending_retire;

  if (!vkr_bindless_vk_asset_publish_texture(renderer, replacement_texture,
                                             &prepared))
    return false_v;
  for (uint32_t i = 0; i < ArrayCount(material_texture_slots); ++i)
    material_b_data.textures[material_texture_slots[i]].handle =
        replacement_texture;
  if (!vkr_bindless_vk_asset_publish_material(renderer, material_b,
                                              &material_b_data))
    return false_v;
  renderer->active_material_index =
      renderer->published_materials[material_b.id - 1u].slot.index;
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 2003u, &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[2]) ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;

  uint64_t after_submit = submitted[0].submit_value - 1u;
  for (uint32_t i = 0; i < ArrayCount(submitted); ++i) {
    VkrBindlessVulkanResult completed = {0};
    if (!vkr_bindless_vulkan_renderer_poll_result(renderer, after_submit,
                                                  &completed) ||
        !vkr_bindless_vk_result_is_sentinel(&completed))
      return false_v;
    after_submit = completed.submit_value;
    out_result->exact_draw_count++;
  }
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  out_result->replacement_survived =
      !renderer->published_textures[shared_texture.id - 1u].pending_retire &&
      renderer->published_textures[replacement_texture.id - 1u].live &&
      renderer->published_materials[material_b.id - 1u].live;
  if (!out_result->shared_resource_survived ||
      !out_result->replacement_survived ||
      !vkr_bindless_vk_asset_unpublish_material(renderer, material_b))
    return false_v;
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  if (!vkr_bindless_vk_asset_unpublish_texture(renderer, replacement_texture))
    return false_v;
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  renderer->active_material_index = 0u;
  if (!vkr_bindless_vk_activate_geometry(renderer, (VkrGeometryHandle){0}) ||
      !vkr_bindless_vk_asset_unpublish_geometry(renderer, loaded_geometry) ||
      !vkr_bindless_vk_asset_unpublish_texture(renderer, writable_texture))
    return false_v;
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  uint64_t upload_wait_count = 0u;
  if (!vkr_bindless_vulkan_renderer_get_and_reset_upload_wait_count(
          renderer, &upload_wait_count))
    return false_v;
  out_result->upload_wait_free = upload_wait_count == 0u;
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_run_publication_test(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanPublicationTestResult *out_result) {
  if (!renderer || !out_result || renderer->frame_active ||
      renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;
  MemZero(out_result, sizeof(*out_result));
  if (renderer->pending_buffer_initialization_count ||
      renderer->pending_texture_initialization_count) {
    VkrFrameSetup setup = {0};
    const VkrRenderPacket packet = {
        .packet_version = VKR_RENDER_PACKET_VERSION,
    };
    VkrBindlessVulkanResult submitted = {0};
    if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 999u, &setup) ||
        !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                    &submitted) ||
        !vkr_bindless_vulkan_renderer_wait_idle(renderer))
      return false_v;
    VkrBindlessVulkanResult completed = {0};
    if (!vkr_bindless_vulkan_renderer_poll_result(
            renderer, submitted.submit_value - 1u, &completed) ||
        !vkr_bindless_vk_result_is_sentinel(&completed))
      return false_v;
  }
  VkrBindlessVulkanHeapMetrics baseline = {0};
  VkrBindlessVulkanMemoryMetrics memory_baseline = {0};
  vkr_bindless_vulkan_renderer_heap_metrics(renderer, &baseline);
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_baseline);
  if (!vkr_bindless_vk_run_asset_publisher_fixture(renderer, out_result))
    return false_v;
  VkrGpuSlotHandle shared_texture = {0}, shared_storage = {0};
  VkrGpuSlotHandle shared_sampler = {0};
  VkrGpuSlotHandle replacement_texture = {0}, replacement_storage = {0};
  VkrGpuSlotHandle replacement_sampler = {0};
  VkrGpuSlotHandle material_a = {0}, material_b = {0}, replacement = {0};
  if (!vkr_bindless_vk_publish_sampled_view(
          renderer, renderer->sentinel_image.view,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &shared_texture) ||
      !vkr_bindless_vk_publish_storage_view(
          renderer, renderer->sentinel_image.view, &shared_storage) ||
      !vkr_bindless_vk_publish_sampler(renderer, renderer->sentinel_sampler,
                                       &shared_sampler) ||
      !vkr_bindless_vk_publish_material_row(renderer, shared_texture.index,
                                            shared_sampler.index, 0x1001u,
                                            &material_a) ||
      !vkr_bindless_vk_publish_material_row(renderer, shared_texture.index,
                                            shared_sampler.index, 0x1002u,
                                            &material_b))
    return false_v;

  const uint64_t before_submit = renderer->submit_value;
  VkrFrameSetup setup = {0};
  VkrRenderPacket packet = {.packet_version = VKR_RENDER_PACKET_VERSION};
  VkrBindlessVulkanResult submitted[3] = {0};
  renderer->active_material_index = material_a.index;
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 1001u, &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[0]))
    return false_v;
  if (vkr_gpu_slot_table_retire(renderer->material_slots, material_a,
                                submitted[0].submit_value) !=
      VKR_GPU_SLOT_STATUS_OK)
    return false_v;

  uint32_t survivor_index = UINT32_MAX;
  uint32_t shared_texture_index = UINT32_MAX;
  uint32_t shared_storage_index = UINT32_MAX;
  uint32_t shared_sampler_index = UINT32_MAX;
  out_result->shared_resource_survived =
      vkr_gpu_slot_table_resolve(renderer->material_slots, material_b,
                                 &survivor_index) == VKR_GPU_SLOT_STATUS_OK &&
      vkr_gpu_slot_table_resolve(renderer->sampled_image_slots, shared_texture,
                                 &shared_texture_index) ==
          VKR_GPU_SLOT_STATUS_OK &&
      vkr_gpu_slot_table_resolve(renderer->storage_image_slots, shared_storage,
                                 &shared_storage_index) ==
          VKR_GPU_SLOT_STATUS_OK &&
      vkr_gpu_slot_table_resolve(renderer->sampler_slots, shared_sampler,
                                 &shared_sampler_index) ==
          VKR_GPU_SLOT_STATUS_OK &&
      shared_texture_index == shared_texture.index &&
      shared_storage_index == shared_storage.index &&
      shared_sampler_index == shared_sampler.index;
  renderer->active_material_index = survivor_index;
  if (!out_result->shared_resource_survived ||
      !vkr_bindless_vulkan_renderer_prepare_frame(renderer, 1002u, &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[1]))
    return false_v;

  if (!vkr_bindless_vk_publish_sampled_view(
          renderer, renderer->sentinel_image.view,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &replacement_texture) ||
      !vkr_bindless_vk_publish_storage_view(
          renderer, renderer->sentinel_image.view, &replacement_storage) ||
      !vkr_bindless_vk_publish_sampler(renderer, renderer->sentinel_sampler,
                                       &replacement_sampler) ||
      !vkr_bindless_vk_publish_material_row(renderer, replacement_texture.index,
                                            replacement_sampler.index, 0x2002u,
                                            &replacement) ||
      vkr_gpu_slot_table_retire(renderer->material_slots, material_b,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->sampled_image_slots, shared_texture,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->storage_image_slots, shared_storage,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->sampler_slots, shared_sampler,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  uint32_t replacement_index = UINT32_MAX;
  out_result->replacement_survived =
      vkr_gpu_slot_table_resolve(renderer->material_slots, replacement,
                                 &replacement_index) == VKR_GPU_SLOT_STATUS_OK;
  renderer->active_material_index = replacement_index;
  if (!out_result->replacement_survived ||
      !vkr_bindless_vulkan_renderer_prepare_frame(renderer, 1003u, &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[2]) ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;

  uint64_t after_submit = before_submit;
  for (uint32_t i = 0; i < ArrayCount(submitted); ++i) {
    VkrBindlessVulkanResult completed = {0};
    if (!vkr_bindless_vulkan_renderer_poll_result(renderer, after_submit,
                                                  &completed) ||
        !vkr_bindless_vk_result_is_sentinel(&completed))
      return false_v;
    after_submit = completed.submit_value;
    out_result->exact_draw_count++;
  }
  const uint64_t completed = renderer->completed_value;
  if (vkr_gpu_slot_table_collect(renderer->material_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampler_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->material_slots, replacement,
                                submitted[2].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(
          renderer->sampled_image_slots, replacement_texture,
          submitted[2].submit_value) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(
          renderer->storage_image_slots, replacement_storage,
          submitted[2].submit_value) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->sampler_slots, replacement_sampler,
                                submitted[2].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->material_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampler_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  renderer->active_material_index = 0u;
  vkr_gpu_slot_table_get_metrics(renderer->sampled_image_slots,
                                 &out_result->sampled_images);
  vkr_gpu_slot_table_get_metrics(renderer->storage_image_slots,
                                 &out_result->storage_images);
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &out_result->samplers);
  vkr_gpu_slot_table_get_metrics(renderer->material_slots,
                                 &out_result->materials);
  VkrBindlessVulkanMemoryMetrics memory_after = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_after);
  out_result->memory_returned_to_baseline =
      memory_after.aggregate.live_allocations ==
          memory_baseline.aggregate.live_allocations &&
      memory_after.aggregate.retired_allocations ==
          memory_baseline.aggregate.retired_allocations &&
      memory_after.aggregate.live_requested_bytes ==
          memory_baseline.aggregate.live_requested_bytes &&
      memory_after.aggregate.live_reserved_bytes ==
          memory_baseline.aggregate.live_reserved_bytes &&
      memory_after.aggregate.retired_requested_bytes ==
          memory_baseline.aggregate.retired_requested_bytes &&
      memory_after.aggregate.retired_reserved_bytes ==
          memory_baseline.aggregate.retired_reserved_bytes;
  if (!out_result->memory_returned_to_baseline)
    log_debug(
        "Bindless Vulkan V4 memory baseline drift: live %llu->%llu "
        "requested %llu->%llu reserved %llu->%llu retired %llu->%llu",
        (unsigned long long)memory_baseline.aggregate.live_allocations,
        (unsigned long long)memory_after.aggregate.live_allocations,
        (unsigned long long)memory_baseline.aggregate.live_requested_bytes,
        (unsigned long long)memory_after.aggregate.live_requested_bytes,
        (unsigned long long)memory_baseline.aggregate.live_reserved_bytes,
        (unsigned long long)memory_after.aggregate.live_reserved_bytes,
        (unsigned long long)memory_baseline.aggregate.retired_allocations,
        (unsigned long long)memory_after.aggregate.retired_allocations);
  return out_result->exact_draw_count == 6u &&
         out_result->shared_sampler_reused &&
         out_result->dependent_materials_republished &&
         out_result->upload_wait_free &&
         out_result->staging_retired_at_submit &&
         out_result->memory_returned_to_baseline &&
         out_result->sampled_images.slots_live ==
             baseline.sampled_images.slots_live &&
         out_result->sampled_images.slots_retired ==
             baseline.sampled_images.slots_retired &&
         out_result->storage_images.slots_live ==
             baseline.storage_images.slots_live &&
         out_result->storage_images.slots_retired ==
             baseline.storage_images.slots_retired &&
         out_result->samplers.slots_live == baseline.samplers.slots_live &&
         out_result->samplers.slots_retired ==
             baseline.samplers.slots_retired &&
         out_result->materials.slots_live == baseline.materials.slots_live &&
         out_result->materials.slots_retired ==
             baseline.materials.slots_retired;
}

void vkr_bindless_vulkan_renderer_validation_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanValidationStats *out_stats) {
  vkr_bindless_vulkan_device_validation_stats(
      renderer ? renderer->device : NULL, out_stats);
}

void vkr_bindless_vulkan_renderer_destroy(VkrBindlessVulkanRenderer *renderer) {
  if (!renderer) {
    return;
  }
  VkrAllocator *allocator = renderer->allocator;
  VkDevice device = renderer->device
                        ? vkr_bindless_vulkan_device_handle(renderer->device)
                        : VK_NULL_HANDLE;
  if (device) {
    vkr_bindless_vulkan_renderer_wait_idle(renderer);
    if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN)
      (void)vkDeviceWaitIdle(device);
    vkr_bindless_vk_discard_buffer_initializations(renderer);
    vkr_bindless_vk_discard_texture_initializations(renderer);
    vkr_bindless_vk_drain_asset_publications(renderer);
    for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets);
         ++i) {
      if (renderer->retired_window_targets[i].occupied)
        vkr_bindless_vk_destroy_window_target(
            renderer, &renderer->retired_window_targets[i].target);
    }
    vkr_bindless_vk_destroy_window_target(renderer, &renderer->window_target);
    for (uint32_t i = 0; i < ArrayCount(renderer->acquire_semaphores); ++i) {
      if (renderer->acquire_semaphores[i])
        vkDestroySemaphore(device, renderer->acquire_semaphores[i], NULL);
    }
    for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
      if (renderer->retired_targets[i].occupied) {
        vkr_bindless_vk_destroy_target_set(
            renderer, &renderer->retired_targets[i].targets);
      }
    }
    vkr_bindless_vk_destroy_target_set(renderer, &renderer->targets);
    vkr_bindless_vk_destroy_frame_slots(renderer);
    if (renderer->pipeline) {
      vkDestroyPipeline(device, renderer->pipeline, NULL);
    }
    if (renderer->pipeline_layout) {
      vkDestroyPipelineLayout(device, renderer->pipeline_layout, NULL);
    }
    if (renderer->vertex_shader) {
      vkDestroyShaderModule(device, renderer->vertex_shader, NULL);
    }
    if (renderer->fragment_shader) {
      vkDestroyShaderModule(device, renderer->fragment_shader, NULL);
    }
    if (renderer->sentinel_sampler) {
      vkDestroySampler(device, renderer->sentinel_sampler, NULL);
    }
    for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
      if (renderer->published_samplers &&
          renderer->published_samplers[i].sampler)
        vkDestroySampler(device, renderer->published_samplers[i].sampler, NULL);
    }
    for (uint32_t i = 0; i < renderer->retired_staging_buffer_capacity; ++i) {
      if (renderer->retired_staging_buffers &&
          renderer->retired_staging_buffers[i].occupied)
        vkr_bindless_vk_destroy_buffer(
            renderer, &renderer->retired_staging_buffers[i].buffer);
    }
    vkr_bindless_vk_destroy_image(renderer, &renderer->sentinel_image);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->materials);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->upload);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->sampler_descriptors);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->resource_descriptors);
    vkr_bindless_vulkan_memory_pool_destroy(renderer->memory_pool);
    renderer->memory_pool = NULL;
    if (renderer->timeline) {
      vkDestroySemaphore(device, renderer->timeline, NULL);
    }
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
  vkr_bindless_vulkan_device_destroy(renderer->device);
  vkr_allocator_free(allocator, renderer, sizeof(*renderer),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

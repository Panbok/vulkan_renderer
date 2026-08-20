#include "renderer/vulkan/bindless/vkr_bindless_vulkan_memory.h"

#include "core/logger.h"

typedef struct VkrBindlessVkMemoryBlock {
  VkrBindlessVkMemoryPoolKey key;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped;
  VkMemoryPropertyFlags properties;
  VkrGpuMemoryCore *core;
  void *core_storage;
  uint64_t core_storage_size;
} VkrBindlessVkMemoryBlock;

struct VkrBindlessVkMemoryPoolManager {
  VkrBindlessVkMemoryPoolConfig config;
  VkrBindlessVkMemoryBlock *blocks;
  uint64_t blocks_size;
  uint32_t block_count;
  uint64_t physical_allocations_peak;
  uint64_t physical_allocations_created;
  uint64_t block_bytes;
  uint64_t physical_allocated_bytes_peak;
  uint64_t dedicated_live;
  uint64_t dedicated_bytes;
  uint64_t block_capacity_failures;
  uint64_t native_allocation_failures;
  VkrGpuMemoryMetrics dedicated_metrics;
};

bool8_t vkr_bindless_vulkan_noncoherent_range(
    uint64_t offset, uint64_t size, uint64_t allocation_size,
    uint64_t noncoherent_atom_size, VkrBindlessVkMappedRange *out_range) {
  if (!out_range || !size || !allocation_size || !noncoherent_atom_size ||
      (noncoherent_atom_size & (noncoherent_atom_size - 1u)) != 0u ||
      offset >= allocation_size || size > allocation_size - offset)
    return false_v;
  const uint64_t start = offset & ~(noncoherent_atom_size - 1u);
  const uint64_t requested_end = offset + size;
  uint64_t end = requested_end;
  if (requested_end <= UINT64_MAX - (noncoherent_atom_size - 1u))
    end = AlignPow2(requested_end, noncoherent_atom_size);
  if (end > allocation_size)
    end = allocation_size;
  *out_range = (VkrBindlessVkMappedRange){.offset = start, .size = end - start};
  return true_v;
}

int32_t
vkr_bindless_vulkan_memory_type_rank(VkrBindlessVkMemoryClass memory_class,
                                     VkMemoryPropertyFlags properties) {
  switch (memory_class) {
  case VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE:
    if ((properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
        !(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      return 0;
    if (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      return 1;
    return 2;
  case VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD:
    if (!(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      return -1;
    if ((properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
        (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      return 0;
    if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return 1;
    return 2;
  case VKR_BINDLESS_VK_MEMORY_CLASS_STAGING:
    if (!(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      return -1;
    /* Inverse of UPLOAD: a copy source is never read by a shader, so keep it
       out of the small device-local host-visible heap. Falling back to that
       heap is still better than failing the transfer. */
    if ((properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
        !(properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      return 0;
    if (!(properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      return 1;
    if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return 2;
    return 3;
  case VKR_BINDLESS_VK_MEMORY_CLASS_READBACK:
    if (!(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      return -1;
    if ((properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
        (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      return 0;
    if (properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
      return 1;
    if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return 2;
    return 3;
  default:
    return -1;
  }
}

bool8_t
vkr_bindless_vulkan_memory_pool_key_equal(VkrBindlessVkMemoryPoolKey left,
                                          VkrBindlessVkMemoryPoolKey right) {
  return left.memory_class == right.memory_class && left.kind == right.kind &&
         left.memory_type_index == right.memory_type_index &&
         left.device_address_required == right.device_address_required;
}

bool8_t vkr_bindless_vulkan_memory_block_size(uint64_t configured_size,
                                              uint64_t resource_size,
                                              uint64_t alignment,
                                              uint64_t *out_size) {
  if (!out_size || !configured_size || !resource_size || !alignment ||
      (alignment & (alignment - 1u)) != 0u)
    return false_v;
  const uint64_t minimum = Max(configured_size, resource_size);
  if (minimum > UINT64_MAX - (alignment - 1u))
    return false_v;
  *out_size = AlignPow2(minimum, alignment);
  return true_v;
}

bool8_t vkr_bindless_vulkan_memory_pool_create(
    const VkrBindlessVkMemoryPoolConfig *config,
    VkrBindlessVkMemoryPoolManager **out_manager) {
  if (!config || !out_manager || !config->allocator || !config->device ||
      !config->max_blocks || !config->max_blocks_per_pool ||
      config->max_blocks_per_pool > config->max_blocks ||
      !config->max_allocations_per_block)
    return false_v;
  for (uint32_t memory_class = 0;
       memory_class < VKR_BINDLESS_VK_MEMORY_CLASS_COUNT; ++memory_class) {
    for (uint32_t kind = 0; kind < VKR_BINDLESS_VK_MEMORY_KIND_COUNT; ++kind) {
      if (!config->block_sizes[memory_class][kind])
        return false_v;
    }
  }
  *out_manager = NULL;
  VkrBindlessVkMemoryPoolManager *manager = vkr_allocator_alloc(
      config->allocator, sizeof(*manager), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!manager)
    return false_v;
  MemZero(manager, sizeof(*manager));
  manager->config = *config;
  manager->blocks_size =
      (uint64_t)config->max_blocks * sizeof(*manager->blocks);
  manager->blocks = vkr_allocator_alloc(config->allocator, manager->blocks_size,
                                        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!manager->blocks) {
    vkr_allocator_free(config->allocator, manager, sizeof(*manager),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }
  MemZero(manager->blocks, manager->blocks_size);
  *out_manager = manager;
  return true_v;
}

vkr_internal void
vkr_bindless_vk_memory_destroy_block(VkrBindlessVkMemoryPoolManager *manager,
                                     VkrBindlessVkMemoryBlock *block) {
  if (block->mapped)
    vkUnmapMemory(manager->config.device, block->memory);
  if (block->memory)
    vkFreeMemory(manager->config.device, block->memory, NULL);
  if (block->core_storage)
    vkr_allocator_free(manager->config.allocator, block->core_storage,
                       block->core_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  MemZero(block, sizeof(*block));
}

void vkr_bindless_vulkan_memory_pool_destroy(
    VkrBindlessVkMemoryPoolManager *manager) {
  if (!manager)
    return;
  VkrBindlessVkMemoryPoolMetrics metrics = {0};
  vkr_bindless_vulkan_memory_pool_get_metrics(manager, &metrics);
  if (metrics.aggregate.live_allocations ||
      metrics.aggregate.retired_allocations) {
    log_error("Bindless Vulkan memory pool destroyed with %llu live and %llu "
              "retired placements",
              (unsigned long long)metrics.aggregate.live_allocations,
              (unsigned long long)metrics.aggregate.retired_allocations);
  }
  for (uint32_t i = 0; i < manager->block_count; ++i)
    vkr_bindless_vk_memory_destroy_block(manager, &manager->blocks[i]);
  VkrAllocator *allocator = manager->config.allocator;
  vkr_allocator_free(allocator, manager->blocks, manager->blocks_size,
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  vkr_allocator_free(allocator, manager, sizeof(*manager),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

vkr_internal VkrBindlessVkMemoryBlock *vkr_bindless_vk_memory_create_block(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    VkMemoryPropertyFlags properties, uint64_t resource_size,
    uint64_t alignment, uint32_t *out_block_index) {
  uint32_t pool_block_count = 0u;
  for (uint32_t i = 0; i < manager->block_count; ++i) {
    if (vkr_bindless_vulkan_memory_pool_key_equal(manager->blocks[i].key, key))
      pool_block_count++;
  }
  if (manager->block_count == manager->config.max_blocks ||
      pool_block_count == manager->config.max_blocks_per_pool) {
    manager->block_capacity_failures++;
    return NULL;
  }
  uint64_t block_size = 0;
  if (!vkr_bindless_vulkan_memory_block_size(
          manager->config.block_sizes[key.memory_class][key.kind],
          resource_size, alignment, &block_size))
    return NULL;
  const VkrGpuMemoryConfig core_config = {
      .heap_size = block_size,
      .max_allocations = manager->config.max_allocations_per_block,
      .max_retirements = manager->config.max_allocations_per_block,
      .max_free_ranges = manager->config.max_allocations_per_block + 1u,
  };
  VkrBindlessVkMemoryBlock pending = {
      .key = key,
      .size = block_size,
      .properties = properties,
  };
  pending.core_storage_size = vkr_gpu_memory_storage_requirement(&core_config);
  pending.core_storage =
      vkr_allocator_alloc(manager->config.allocator, pending.core_storage_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!pending.core_storage ||
      vkr_gpu_memory_create(&core_config, pending.core_storage,
                            pending.core_storage_size,
                            &pending.core) != VKR_GPU_MEMORY_STATUS_OK) {
    if (pending.core_storage)
      vkr_allocator_free(manager->config.allocator, pending.core_storage,
                         pending.core_storage_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return NULL;
  }
  const VkMemoryAllocateFlagsInfo flags = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = key.device_address_required
                   ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
                   : 0u,
  };
  const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = key.device_address_required ? &flags : NULL,
      .allocationSize = block_size,
      .memoryTypeIndex = key.memory_type_index,
  };
  if (vkAllocateMemory(manager->config.device, &allocate_info, NULL,
                       &pending.memory) != VK_SUCCESS ||
      ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
       vkMapMemory(manager->config.device, pending.memory, 0u, block_size, 0u,
                   &pending.mapped) != VK_SUCCESS)) {
    manager->native_allocation_failures++;
    vkr_bindless_vk_memory_destroy_block(manager, &pending);
    return NULL;
  }
  const uint32_t block_index = manager->block_count++;
  manager->blocks[block_index] = pending;
  manager->physical_allocations_created++;
  manager->block_bytes += block_size;
  manager->physical_allocated_bytes_peak =
      Max(manager->physical_allocated_bytes_peak,
          manager->block_bytes + manager->dedicated_bytes);
  manager->physical_allocations_peak =
      Max(manager->physical_allocations_peak,
          (uint64_t)manager->block_count + manager->dedicated_live);
  *out_block_index = block_index;
  return &manager->blocks[block_index];
}

bool8_t vkr_bindless_vulkan_memory_pool_allocate(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    VkMemoryPropertyFlags properties, VkDeviceSize size, VkDeviceSize alignment,
    VkrBindlessVkPooledAllocation *out_allocation) {
  if (!manager || !out_allocation || !size || !alignment ||
      key.memory_class >= VKR_BINDLESS_VK_MEMORY_CLASS_COUNT ||
      key.kind >= VKR_BINDLESS_VK_MEMORY_KIND_COUNT ||
      (key.kind == VKR_BINDLESS_VK_MEMORY_KIND_IMAGE &&
       key.device_address_required))
    return false_v;
  MemZero(out_allocation, sizeof(*out_allocation));
  const uint32_t gpu_kind = key.kind == VKR_BINDLESS_VK_MEMORY_KIND_BUFFER
                                ? VKR_GPU_MEMORY_CLASS_BUFFER
                                : VKR_GPU_MEMORY_CLASS_TEXTURE;
  for (uint32_t i = 0; i < manager->block_count; ++i) {
    VkrBindlessVkMemoryBlock *candidate = &manager->blocks[i];
    if (!vkr_bindless_vulkan_memory_pool_key_equal(candidate->key, key))
      continue;
    VkrGpuMemoryMetrics metrics = {0};
    vkr_gpu_memory_get_metrics(candidate->core, &metrics);
    const bool8_t empty = metrics.free_bytes == metrics.heap_size;
    const bool8_t conservative_fit =
        size <= UINT64_MAX - (alignment - 1u) &&
        metrics.largest_free_range >= size + alignment - 1u;
    if (!empty && !conservative_fit)
      continue;
    VkrGpuAllocationHandle handle = {0};
    VkrGpuPlacement placement = {0};
    if (vkr_gpu_memory_allocate(candidate->core, size, alignment, gpu_kind,
                                &handle,
                                &placement) != VKR_GPU_MEMORY_STATUS_OK)
      continue;
    *out_allocation = (VkrBindlessVkPooledAllocation){
        .memory = candidate->memory,
        .memory_size = candidate->size,
        .offset = placement.resource_offset,
        .mapped = candidate->mapped
                      ? (uint8_t *)candidate->mapped + placement.resource_offset
                      : NULL,
        .properties = candidate->properties,
        .handle = handle,
        .key = key,
        .block_index = i,
        .valid = true_v,
    };
    return true_v;
  }
  uint32_t block_index = UINT32_MAX;
  VkrBindlessVkMemoryBlock *block = vkr_bindless_vk_memory_create_block(
      manager, key, properties, size, alignment, &block_index);
  if (!block)
    return false_v;
  VkrGpuAllocationHandle handle = {0};
  VkrGpuPlacement placement = {0};
  if (vkr_gpu_memory_allocate(block->core, size, alignment, gpu_kind, &handle,
                              &placement) != VKR_GPU_MEMORY_STATUS_OK)
    return false_v;
  *out_allocation = (VkrBindlessVkPooledAllocation){
      .memory = block->memory,
      .memory_size = block->size,
      .offset = placement.resource_offset,
      .mapped = block->mapped
                    ? (uint8_t *)block->mapped + placement.resource_offset
                    : NULL,
      .properties = block->properties,
      .handle = handle,
      .key = key,
      .block_index = block_index,
      .valid = true_v,
  };
  return true_v;
}

bool8_t vkr_bindless_vulkan_memory_pool_release(
    VkrBindlessVkMemoryPoolManager *manager,
    VkrBindlessVkPooledAllocation *allocation, uint64_t last_use_submit_value,
    uint64_t completed_submit_value) {
  if (!manager || !allocation || !allocation->valid ||
      allocation->block_index >= manager->block_count ||
      last_use_submit_value > completed_submit_value ||
      (allocation->retired &&
       allocation->retire_value > completed_submit_value))
    return false_v;
  VkrBindlessVkMemoryBlock *block = &manager->blocks[allocation->block_index];
  if (!vkr_bindless_vulkan_memory_pool_key_equal(block->key, allocation->key) ||
      vkr_gpu_memory_collect(block->core, completed_submit_value, NULL, NULL,
                             NULL) != VKR_GPU_MEMORY_STATUS_OK)
    return false_v;
  if (!allocation->retired &&
      (vkr_gpu_memory_retire(block->core, allocation->handle,
                             last_use_submit_value) !=
           VKR_GPU_MEMORY_STATUS_OK ||
       vkr_gpu_memory_collect(block->core, completed_submit_value, NULL, NULL,
                              NULL) != VKR_GPU_MEMORY_STATUS_OK))
    return false_v;
  MemZero(allocation, sizeof(*allocation));
  return true_v;
}

bool8_t vkr_bindless_vulkan_memory_pool_retire(
    VkrBindlessVkMemoryPoolManager *manager,
    VkrBindlessVkPooledAllocation *allocation, uint64_t retire_value) {
  if (!manager || !allocation || !allocation->valid || allocation->retired ||
      allocation->block_index >= manager->block_count)
    return false_v;
  VkrBindlessVkMemoryBlock *block = &manager->blocks[allocation->block_index];
  if (!vkr_bindless_vulkan_memory_pool_key_equal(block->key, allocation->key) ||
      vkr_gpu_memory_retire(block->core, allocation->handle, retire_value) !=
          VKR_GPU_MEMORY_STATUS_OK)
    return false_v;
  allocation->retired = true_v;
  allocation->retire_value = retire_value;
  return true_v;
}

vkr_internal VkrGpuMemoryClassMetrics *
vkr_bindless_vk_dedicated_class_metrics(VkrGpuMemoryMetrics *metrics,
                                        VkrBindlessVkMemoryPoolKey key) {
  const uint32_t gpu_kind = key.kind == VKR_BINDLESS_VK_MEMORY_KIND_BUFFER
                                ? VKR_GPU_MEMORY_CLASS_BUFFER
                                : VKR_GPU_MEMORY_CLASS_TEXTURE;
  return &metrics->classes[gpu_kind];
}

void vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    uint64_t size) {
  if (!manager || !size)
    return;
  VkrGpuMemoryMetrics *metrics = &manager->dedicated_metrics;
  VkrGpuMemoryClassMetrics *class_metrics =
      vkr_bindless_vk_dedicated_class_metrics(metrics, key);
  metrics->allocations_created++;
  metrics->live_allocations++;
  metrics->live_requested_bytes += size;
  metrics->live_reserved_bytes += size;
  metrics->peak_allocations =
      Max(metrics->peak_allocations, metrics->live_allocations);
  metrics->peak_requested_bytes =
      Max(metrics->peak_requested_bytes, metrics->live_requested_bytes);
  metrics->peak_reserved_bytes =
      Max(metrics->peak_reserved_bytes, metrics->live_reserved_bytes);
  class_metrics->allocations_created++;
  class_metrics->live_allocations++;
  class_metrics->live_requested_bytes += size;
  class_metrics->live_reserved_bytes += size;
  class_metrics->peak_allocations =
      Max(class_metrics->peak_allocations, class_metrics->live_allocations);
  class_metrics->peak_requested_bytes = Max(
      class_metrics->peak_requested_bytes, class_metrics->live_requested_bytes);
  class_metrics->peak_reserved_bytes = Max(class_metrics->peak_reserved_bytes,
                                           class_metrics->live_reserved_bytes);
  manager->dedicated_live++;
  manager->dedicated_bytes += size;
  manager->physical_allocations_created++;
  manager->physical_allocated_bytes_peak =
      Max(manager->physical_allocated_bytes_peak,
          manager->block_bytes + manager->dedicated_bytes);
  manager->physical_allocations_peak =
      Max(manager->physical_allocations_peak,
          (uint64_t)manager->block_count + manager->dedicated_live);
}

void vkr_bindless_vulkan_memory_pool_record_dedicated_release(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    uint64_t size, bool8_t retired) {
  if (!manager || !size || !manager->dedicated_live ||
      manager->dedicated_bytes < size)
    return;
  VkrGpuMemoryMetrics *metrics = &manager->dedicated_metrics;
  VkrGpuMemoryClassMetrics *class_metrics =
      vkr_bindless_vk_dedicated_class_metrics(metrics, key);
  uint64_t *allocations =
      retired ? &metrics->retired_allocations : &metrics->live_allocations;
  uint64_t *requested = retired ? &metrics->retired_requested_bytes
                                : &metrics->live_requested_bytes;
  uint64_t *reserved = retired ? &metrics->retired_reserved_bytes
                               : &metrics->live_reserved_bytes;
  uint64_t *class_allocations = retired ? &class_metrics->retired_allocations
                                        : &class_metrics->live_allocations;
  uint64_t *class_requested = retired ? &class_metrics->retired_requested_bytes
                                      : &class_metrics->live_requested_bytes;
  uint64_t *class_reserved = retired ? &class_metrics->retired_reserved_bytes
                                     : &class_metrics->live_reserved_bytes;
  if (!*allocations || *requested < size || *reserved < size ||
      !*class_allocations || *class_requested < size || *class_reserved < size)
    return;
  (*allocations)--;
  *requested -= size;
  *reserved -= size;
  metrics->retirements_collected++;
  (*class_allocations)--;
  *class_requested -= size;
  *class_reserved -= size;
  manager->dedicated_live--;
  manager->dedicated_bytes -= size;
}

bool8_t vkr_bindless_vulkan_memory_pool_record_dedicated_retire(
    VkrBindlessVkMemoryPoolManager *manager, VkrBindlessVkMemoryPoolKey key,
    uint64_t size) {
  if (!manager || !size)
    return false_v;
  VkrGpuMemoryMetrics *metrics = &manager->dedicated_metrics;
  VkrGpuMemoryClassMetrics *class_metrics =
      vkr_bindless_vk_dedicated_class_metrics(metrics, key);
  if (!metrics->live_allocations || metrics->live_requested_bytes < size ||
      metrics->live_reserved_bytes < size || !class_metrics->live_allocations ||
      class_metrics->live_requested_bytes < size ||
      class_metrics->live_reserved_bytes < size)
    return false_v;
  metrics->live_allocations--;
  metrics->live_requested_bytes -= size;
  metrics->live_reserved_bytes -= size;
  metrics->retired_allocations++;
  metrics->retired_requested_bytes += size;
  metrics->retired_reserved_bytes += size;
  class_metrics->live_allocations--;
  class_metrics->live_requested_bytes -= size;
  class_metrics->live_reserved_bytes -= size;
  class_metrics->retired_allocations++;
  class_metrics->retired_requested_bytes += size;
  class_metrics->retired_reserved_bytes += size;
  return true_v;
}

void vkr_bindless_vulkan_memory_pool_record_native_failure(
    VkrBindlessVkMemoryPoolManager *manager) {
  if (manager)
    manager->native_allocation_failures++;
}

void vkr_bindless_vulkan_memory_pool_get_metrics(
    const VkrBindlessVkMemoryPoolManager *manager,
    VkrBindlessVkMemoryPoolMetrics *out_metrics) {
  if (!out_metrics)
    return;
  MemZero(out_metrics, sizeof(*out_metrics));
  if (!manager)
    return;
  out_metrics->physical_allocations_live =
      manager->block_count + manager->dedicated_live;
  out_metrics->physical_allocations_peak = manager->physical_allocations_peak;
  out_metrics->physical_allocations_created =
      manager->physical_allocations_created;
  out_metrics->physical_allocated_bytes =
      manager->block_bytes + manager->dedicated_bytes;
  out_metrics->physical_allocated_bytes_peak =
      manager->physical_allocated_bytes_peak;
  out_metrics->block_capacity_failures = manager->block_capacity_failures;
  for (uint32_t i = 0; i < manager->block_count; ++i) {
    VkrGpuMemoryMetrics block_metrics = {0};
    vkr_gpu_memory_get_metrics(manager->blocks[i].core, &block_metrics);
    vkr_gpu_memory_metrics_accumulate(&out_metrics->aggregate, &block_metrics);
  }
  vkr_gpu_memory_metrics_accumulate(&out_metrics->aggregate,
                                    &manager->dedicated_metrics);
  out_metrics->aggregate.native_allocation_failures +=
      manager->native_allocation_failures;
}

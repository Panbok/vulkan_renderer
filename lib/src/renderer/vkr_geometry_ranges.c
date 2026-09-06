#include "renderer/vkr_geometry_ranges.h"

bool8_t vkr_geometry_ranges_create(VkrGeometryRanges *ranges,
                                   VkrAllocator *allocator,
                                   uint32_t geometry_capacity) {
  if (!ranges || !allocator || !geometry_capacity ||
      geometry_capacity > (UINT32_MAX - 1u) / 2u)
    return false_v;
  *ranges = (VkrGeometryRanges){0};
  const uint32_t count = geometry_capacity * 2u;
  VkrGpuMemoryConfig config = {
      .heap_size = ((uint64_t)UINT32_MAX + 1u) * sizeof(VkrPackedStaticVertex),
      .max_allocations = count,
      .max_retirements = count,
      .max_free_ranges = count + 1u,
  };
  const uint64_t core_size = vkr_gpu_memory_storage_requirement(&config);
  if (!core_size || core_size > UINT64_MAX / 2u)
    return false_v;
  ranges->storage_size = core_size * 2u;
  ranges->allocator = allocator;
  ranges->storage = vkr_allocator_alloc(allocator, ranges->storage_size,
                                        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!ranges->storage)
    goto failure;
  if (vkr_gpu_memory_create(&config, ranges->storage, core_size,
                            &ranges->vertices) != VKR_GPU_MEMORY_STATUS_OK)
    goto failure;
  config.heap_size = ((uint64_t)UINT32_MAX + 1u) * sizeof(uint32_t);
  if (vkr_gpu_memory_create(&config, (uint8_t *)ranges->storage + core_size,
                            core_size,
                            &ranges->indices) != VKR_GPU_MEMORY_STATUS_OK)
    goto failure;
  return true_v;
failure:
  vkr_geometry_ranges_destroy(ranges);
  return false_v;
}

void vkr_geometry_ranges_destroy(VkrGeometryRanges *ranges) {
  if (ranges->storage)
    vkr_allocator_free(ranges->allocator, ranges->storage, ranges->storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  *ranges = (VkrGeometryRanges){0};
}

static VkrGpuMemoryStatus
vkr_geometry_range_allocate(VkrGpuMemoryCore *core, uint64_t size,
                            uint64_t alignment, uint64_t capacity,
                            VkrGpuAllocationHandle *handle,
                            VkrGpuPlacement *placement) {
  if (capacity) {
    const VkrGpuMemoryStatus status = vkr_gpu_memory_allocate_in_range(
        core, size, alignment, VKR_GPU_MEMORY_CLASS_BUFFER, 0u, capacity,
        handle, placement);
    if (status != VKR_GPU_MEMORY_STATUS_OUT_OF_BYTES &&
        status != VKR_GPU_MEMORY_STATUS_FRAGMENTED)
      return status;
  }
  return vkr_gpu_memory_allocate(
      core, size, alignment, VKR_GPU_MEMORY_CLASS_BUFFER, handle, placement);
}

bool8_t
vkr_geometry_ranges_allocate(VkrGeometryRanges *ranges, uint32_t vertex_count,
                             uint32_t decode_count, uint32_t index_count,
                             uint64_t vertex_capacity, uint64_t index_capacity,
                             VkrGeometryRangeAllocation *out_allocation) {
  if (!ranges || !ranges->vertices || !ranges->indices || !out_allocation ||
      !vertex_count || !decode_count || !index_count)
    return false_v;
  *out_allocation = (VkrGeometryRangeAllocation){0};
  const uint64_t vertex_bytes =
      (uint64_t)vertex_count * sizeof(VkrPackedStaticVertex);
  const uint64_t decode_relative = AlignPow2(vertex_bytes, 16u);
  const uint64_t vertex_span =
      decode_relative +
      (uint64_t)decode_count * sizeof(VkrGpuGeometryDecodeRecord);
  const uint64_t index_bytes = (uint64_t)index_count * sizeof(uint32_t);
  VkrGeometryRangeAllocation allocation = {0};
  VkrGpuPlacement vertices = {0}, indices = {0};
  if (vkr_geometry_range_allocate(ranges->vertices, vertex_span, 32u,
                                  vertex_capacity, &allocation.vertices,
                                  &vertices) != VKR_GPU_MEMORY_STATUS_OK)
    return false_v;
  if (vkr_geometry_range_allocate(ranges->indices, index_bytes, 4u,
                                  index_capacity, &allocation.indices,
                                  &indices) != VKR_GPU_MEMORY_STATUS_OK) {
    /* No native operation has consumed this reservation. Metadata capacity
     * guarantees that retiring a live allocation cannot exhaust the queue. */
    (void)vkr_gpu_memory_retire(ranges->vertices, allocation.vertices, 0u);
    (void)vkr_gpu_memory_collect(ranges->vertices, 0u, NULL, NULL, NULL);
    return false_v;
  }
  allocation.vertex_offset = vertices.resource_offset;
  allocation.decode_offset = vertices.resource_offset + decode_relative;
  allocation.index_offset = indices.resource_offset;
  allocation.vertex_end = vertices.resource_offset + vertex_span;
  allocation.index_end = indices.resource_offset + index_bytes;
  *out_allocation = allocation;
  return true_v;
}

bool8_t vkr_geometry_ranges_retire(VkrGeometryRanges *ranges,
                                   VkrGeometryRangeAllocation allocation,
                                   uint64_t last_use_submit_value) {
  VkrGpuPlacement placement;
  /* Resolve both before invalidating either generation. */
  if (vkr_gpu_memory_resolve(ranges->vertices, allocation.vertices,
                             &placement) != VKR_GPU_MEMORY_STATUS_OK ||
      vkr_gpu_memory_resolve(ranges->indices, allocation.indices, &placement) !=
          VKR_GPU_MEMORY_STATUS_OK)
    return false_v;
  return vkr_gpu_memory_retire(ranges->vertices, allocation.vertices,
                               last_use_submit_value) ==
             VKR_GPU_MEMORY_STATUS_OK &&
         vkr_gpu_memory_retire(ranges->indices, allocation.indices,
                               last_use_submit_value) ==
             VKR_GPU_MEMORY_STATUS_OK;
}

bool8_t vkr_geometry_ranges_collect(VkrGeometryRanges *ranges,
                                    uint64_t completed_submit_value) {
  if (!ranges->storage)
    return true_v;
  return vkr_gpu_memory_collect(ranges->vertices, completed_submit_value, NULL,
                                NULL, NULL) == VKR_GPU_MEMORY_STATUS_OK &&
         vkr_gpu_memory_collect(ranges->indices, completed_submit_value, NULL,
                                NULL, NULL) == VKR_GPU_MEMORY_STATUS_OK;
}

void vkr_geometry_ranges_metrics(const VkrGeometryRanges *ranges,
                                 VkrGeometryMegabufferMetrics *metrics) {
  if (!ranges->storage)
    return;
  VkrGpuMemoryMetrics vertices = {0}, indices = {0};
  vkr_gpu_memory_get_metrics(ranges->vertices, &vertices);
  vkr_gpu_memory_get_metrics(ranges->indices, &indices);
  metrics->retired_range_bytes =
      vertices.retired_reserved_bytes + indices.retired_reserved_bytes;
  const uint64_t capacity =
      metrics->vertex_capacity_bytes + metrics->index_capacity_bytes;
  const uint64_t reserved = vertices.live_reserved_bytes +
                            indices.live_reserved_bytes +
                            metrics->retired_range_bytes;
  metrics->reusable_range_bytes =
      capacity >= reserved ? capacity - reserved : 0u;
  metrics->live_range_count =
      vertices.live_allocations + indices.live_allocations;
  metrics->retired_range_count =
      vertices.retired_allocations + indices.retired_allocations;
}

#pragma once

#include "memory/vkr_allocator.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_gpu_memory.h"

/* Offset reservations only. Native backends retain and grow their own backing
 * buffers; these cores never allocate or release GPU memory. */
typedef struct VkrGeometryRanges {
  VkrGpuMemoryCore *vertices;
  VkrGpuMemoryCore *indices;
  VkrAllocator *allocator;
  void *storage;
  uint64_t storage_size;
} VkrGeometryRanges;

typedef struct VkrGeometryRangeAllocation {
  VkrGpuAllocationHandle vertices;
  VkrGpuAllocationHandle indices;
  uint64_t vertex_offset;
  uint64_t decode_offset;
  uint64_t index_offset;
  uint64_t vertex_end;
  uint64_t index_end;
} VkrGeometryRangeAllocation;

/* Metadata holds one live and one retired generation per geometry slot.
 * Further churn fails admission until completed retirement frees capacity. */
bool8_t vkr_geometry_ranges_create(VkrGeometryRanges *ranges,
                                   VkrAllocator *allocator,
                                   uint32_t geometry_capacity);
void vkr_geometry_ranges_destroy(VkrGeometryRanges *ranges);

/* Reserve both streams before native growth/upload. A failed reservation owns
 * nothing. The returned ends specify the required native buffer extents. */
bool8_t
vkr_geometry_ranges_allocate(VkrGeometryRanges *ranges, uint32_t vertex_count,
                             uint32_t decode_count, uint32_t index_count,
                             uint64_t vertex_capacity, uint64_t index_capacity,
                             VkrGeometryRangeAllocation *out_allocation);

bool8_t vkr_geometry_ranges_retire(VkrGeometryRanges *ranges,
                                   VkrGeometryRangeAllocation allocation,
                                   uint64_t last_use_submit_value);
bool8_t vkr_geometry_ranges_collect(VkrGeometryRanges *ranges,
                                    uint64_t completed_submit_value);
void vkr_geometry_ranges_metrics(const VkrGeometryRanges *ranges,
                                 VkrGeometryMegabufferMetrics *metrics);

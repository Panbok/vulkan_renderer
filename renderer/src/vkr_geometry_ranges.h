#pragma once

#include "memory/vkr_allocator.h"
#include "vkr_gpu_abi.h"
#include "vkr_gpu_memory.h"

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

/* Byte accounting for one native geometry megabuffer. The backend owns the
 * buffers; publication and retirement update these counters identically. */
typedef struct VkrGeometryMegabufferAccounting {
  uint64_t vertex_live_bytes;
  uint64_t index_live_bytes;
  uint64_t vertex_high_water;
  uint64_t index_high_water;
  uint64_t vertex_uploaded_bytes_total;
  uint64_t index_uploaded_bytes_total;
  uint64_t decode_metadata_live_bytes;
  uint64_t decode_metadata_high_water;
  uint64_t decode_metadata_uploaded_bytes_total;
  uint64_t rejected_publications;
  uint64_t generation_replacements;
} VkrGeometryMegabufferAccounting;

/** Records a published geometry's bytes and its reserved range ends. */
void vkr_geometry_megabuffer_account_publish(
    VkrGeometryMegabufferAccounting *accounting,
    const VkrGeometryRangeAllocation *ranges, uint64_t vertex_bytes,
    uint64_t index_bytes, uint64_t decode_bytes);

/** Removes a retired geometry's bytes from the live totals. */
void vkr_geometry_megabuffer_account_retire(
    VkrGeometryMegabufferAccounting *accounting, uint64_t vertex_bytes,
    uint64_t index_bytes, uint64_t decode_bytes);

/** Fills megabuffer metrics from the accounting, the native buffer
 * capacities, the publication generation and the range allocator. */
void vkr_geometry_megabuffer_metrics(
    const VkrGeometryMegabufferAccounting *accounting,
    uint64_t vertex_capacity_bytes, uint64_t index_capacity_bytes,
    uint32_t generation, const VkrGeometryRanges *ranges,
    VkrGeometryMegabufferMetrics *out_metrics);

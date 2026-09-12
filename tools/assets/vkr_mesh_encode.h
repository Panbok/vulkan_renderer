#pragma once

#include "assets/vkr_mesh_cooked.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkrMeshCookedEncodeInfo {
  String8 source_path;
  const String8 *dependency_paths;
  const String8 *dependency_references; // Optional portable encoded names;
                                        // paths are physical read locations.
  uint32_t dependency_count;
  VkrGeometryUploadBuffer mesh_buffer;
  VkrMeshSource source;
  const VkrGeometryUploadRange *ranges;
  uint32_t range_count;
  VkrGeometryQuantizationBudgets budgets;
} VkrMeshCookedEncodeInfo;

typedef struct VkrMeshCookStats {
  uint64_t cooked_bytes;
  uint64_t decoded_bytes;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t range_count;
} VkrMeshCookStats;

/** Encodes an immutable cooked artifact into scratch-owned memory. */
bool8_t vkr_mesh_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrMeshCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size);

/** Writes bytes to a sibling temporary file and atomically replaces output. */
bool8_t vkr_mesh_cooked_write_atomic(VkrAllocator *scratch_allocator,
                                     String8 output_path, const uint8_t *data,
                                     uint64_t size);

/** Copy a validated artifact, replacing only same-size source-node metadata.
 * The caller must decode input first. Node/mesh counts and names cannot change;
 * compressed geometry, strings and source identity remain unchanged. */
bool8_t vkr_mesh_cooked_source_variant(VkrAllocator *allocator,
                                        const uint8_t *input, uint64_t size,
                                        const VkrMeshSource *source,
                                        uint8_t **out_data);

#ifdef __cplusplus
}
#endif

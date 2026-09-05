#pragma once

#include "containers/str.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/loaders/mesh_loader.h"

#define VKR_MESH_COOKED_MAGIC 0x564B4D48u /* 'VKMH' */
#define VKR_MESH_COOKED_VERSION 16u
#define VKR_MESH_COOKED_ENDIAN_TAG 0x01020304u
#define VKR_MESH_COOKED_LAYOUT_STATIC_PACKED_V1 2u
#define VKR_MESH_COOKED_STREAM_ALIGNMENT 16u

typedef struct VkrMeshCookedEncodeInfo {
  String8 source_path;
  const String8 *dependency_paths;
  uint32_t dependency_count;
  VkrMeshLoaderBuffer mesh_buffer;
  const VkrMeshLoaderSubmeshRange *ranges;
  uint32_t range_count;
  VkrGeometryQuantizationBudgets budgets;
} VkrMeshCookedEncodeInfo;

typedef struct VkrMeshCookedDecoded {
  uint64_t source_bytes;
  uint64_t cooked_bytes;
  uint64_t decoded_bytes;
  VkrMeshLoaderBuffer mesh_buffer;
  Array_VkrMeshLoaderSubmeshRange ranges;
} VkrMeshCookedDecoded;

typedef struct VkrMeshCookStats {
  uint64_t cooked_bytes;
  uint64_t decoded_bytes;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t range_count;
} VkrMeshCookStats;

/**
 * Parses one authoring mesh, optimizes and encodes each renderable range, then
 * atomically publishes an immutable cooked artifact. Persistent imported data
 * uses source_allocator; parser scopes and encoded output use an independent
 * scratch_allocator.
 */
bool8_t vkr_mesh_cook_source(String8 source_path, String8 output_path,
                             VkrAllocator *source_allocator,
                             VkrAllocator *scratch_allocator,
                             VkrMeshCookStats *out_stats,
                             VkrRendererError *out_error);

/** Encodes an immutable cooked artifact into scratch-owned memory. */
bool8_t vkr_mesh_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrMeshCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size);

/**
 * Validates the complete self-contained artifact and decodes all ranges into
 * result-owned current-ABI buffers. Dependency hashes remain build provenance;
 * runtime decode performs no authoring-source I/O.
 */
bool8_t vkr_mesh_cooked_decode(VkrAllocator *result_allocator,
                               VkrAllocator *scratch_allocator,
                               const uint8_t *data, uint64_t size,
                               VkrMeshCookedDecoded *out_decoded);

/** Writes bytes to a sibling temporary file and atomically replaces output. */
bool8_t vkr_mesh_cooked_write_atomic(VkrAllocator *scratch_allocator,
                                     String8 output_path, const uint8_t *data,
                                     uint64_t size);

#pragma once

#include "assets/vkr_mesh_cooked.h"
#include "filesystem/filesystem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A dependency's SHA-256 and byte size, when the caller already hashed it. */
typedef struct VkrMeshCookedDependencyDigest {
  bool8_t known;
  uint64_t byte_size;
  uint8_t hash[32];
} VkrMeshCookedDependencyDigest;

/* Optional lightmap UV set (ADR-087). A positive texels_per_unit unwraps the
 * static mesh into one chart atlas of at most max_size texels per edge and
 * stores UV2 in packed word 7; skinned meshes reject it. */
typedef struct VkrMeshCookedLightmapOptions {
  float32_t texels_per_unit;
  uint32_t padding;
  uint32_t max_size;
} VkrMeshCookedLightmapOptions;

typedef struct VkrMeshCookedEncodeInfo {
  String8 source_path;
  const String8 *dependency_paths;
  const String8 *dependency_references; // Optional portable encoded names;
                                        // paths are physical read locations.
  // Optional, one per dependency; known entries are not read again.
  const VkrMeshCookedDependencyDigest *dependency_digests;
  uint32_t dependency_count;
  VkrGeometryUploadBuffer mesh_buffer;
  VkrMeshSource source;
  VkrMeshSkinData skin;
  const VkrGeometryUploadRange *ranges;
  uint32_t range_count;
  VkrGeometryQuantizationBudgets budgets;
  VkrMeshCookedLightmapOptions lightmap;
} VkrMeshCookedEncodeInfo;

typedef struct VkrMeshCookStats {
  uint64_t cooked_bytes;
  uint64_t decoded_bytes;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t range_count;
} VkrMeshCookStats;

/** SHA-256 and byte size of a file, streamed through a fixed buffer. */
bool8_t vkr_mesh_cooked_hash_file(FilePath *path, uint8_t out_hash[32],
                                  uint64_t *out_size);

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
 * compressed geometry, strings and source identity remain unchanged. Version 18
 * also preserves skin bytes and rejects changes to mesh/skin bindings or spans.
 */
bool8_t vkr_mesh_cooked_source_variant(VkrAllocator *allocator,
                                       const uint8_t *input, uint64_t size,
                                       const VkrMeshSource *source,
                                       uint8_t **out_data);

#ifdef __cplusplus
}
#endif

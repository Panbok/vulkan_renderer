#pragma once

#include "assets/vkr_mesh_source.h"
#include "vkr_geometry_upload.h"

/* Cooked influence values also supply the skinning upload ABI. Joint indices
 * address each instance's source skin.joints array. Zero weights and joints
 * mark an unskinned vertex.
 */
typedef VkrSkinningInfluence VkrMeshSkinVertex;

/* Arrays share the mesh result arena. Vertices follow the geometry's vertex
 * order, including every deduplication and fetch remap. Fingerprint uses the
 * animation bank's original JSON/buffer identity, before mesh recipe changes.
 */
typedef struct VkrMeshSkinData {
  uint64_t animation_fingerprint;
  uint32_t skin_count;
  uint32_t *joint_counts;
  uint32_t vertex_count;
  VkrMeshSkinVertex *vertices;
} VkrMeshSkinData;

/* Cold boundary validation, including every source node's mesh/skin binding.
 * The source metadata and range/index bounds must already be validated. */
bool8_t vkr_mesh_skin_validate(const VkrMeshSkinData *skin,
                               const VkrMeshSource *source,
                               const VkrGeometryUploadRange *ranges,
                               uint32_t range_count, const uint32_t *indices,
                               uint32_t index_count, uint32_t vertex_count);

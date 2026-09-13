#include "assets/vkr_mesh_skin.h"

#include <math.h>

bool8_t vkr_mesh_skin_validate(const VkrMeshSkinData *skin,
                               const VkrMeshSource *source,
                               const VkrGeometryUploadRange *ranges,
                               uint32_t range_count, const uint32_t *indices,
                               uint32_t index_count, uint32_t vertex_count) {
  if (!skin || !source) {
    return false_v;
  }
  if (!skin->skin_count) {
    return !skin->animation_fingerprint && !skin->joint_counts &&
           !skin->vertex_count && !skin->vertices;
  }
  if (skin->skin_count > 65536u || !skin->joint_counts ||
      skin->vertex_count != vertex_count || (vertex_count && !skin->vertices) ||
      (index_count && !indices) || (range_count && !ranges)) {
    return false_v;
  }
  for (uint32_t i = 0; i < skin->skin_count; ++i) {
    if (!skin->joint_counts[i] ||
        skin->joint_counts[i] > source->nodes.length) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const VkrMeshSkinVertex *vertex = &skin->vertices[i];
    float64_t sum = 0.0;
    for (uint32_t j = 0; j < ArrayCount(vertex->weights); ++j) {
      const float32_t weight = vertex->weights[j];
      if (!isfinite(weight) || weight < 0.0f || weight > 1.0f ||
          (weight == 0.0f && vertex->joints[j] != 0u) ||
          vertex->joints[j] >= source->nodes.length) {
        return false_v;
      }
      sum += weight;
    }
    if (sum != 0.0 && fabs(sum - 1.0) > 0.00001) {
      return false_v;
    }
  }
  for (uint64_t i = 0; i < source->nodes.length; ++i) {
    const VkrMeshSourceNode *node = &source->nodes.data[i];
    if (node->skin == UINT32_MAX) {
      continue;
    }
    if (node->skin >= skin->skin_count ||
        node->mesh_variant >= source->meshes.length) {
      return false_v;
    }
    const VkrMeshSourceMesh *mesh = &source->meshes.data[node->mesh_variant];
    if (mesh->first_range > range_count ||
        mesh->range_count > range_count - mesh->first_range) {
      return false_v;
    }
    const uint32_t joint_count = skin->joint_counts[node->skin];
    for (uint32_t r = 0; r < mesh->range_count; ++r) {
      const VkrGeometryUploadRange *range = &ranges[mesh->first_range + r];
      if (range->first_index > index_count ||
          range->index_count > index_count - range->first_index) {
        return false_v;
      }
      for (uint32_t k = 0; k < range->index_count; ++k) {
        const uint32_t index = indices[range->first_index + k];
        if (index >= vertex_count) {
          return false_v;
        }
        const VkrMeshSkinVertex *vertex = &skin->vertices[index];
        bool8_t weighted = false_v;
        for (uint32_t j = 0; j < ArrayCount(vertex->weights); ++j) {
          if (vertex->weights[j] > 0.0f) {
            if (vertex->joints[j] >= joint_count) {
              return false_v;
            }
            weighted = true_v;
          }
        }
        if (!weighted) {
          return false_v;
        }
      }
    }
  }
  return true_v;
}

#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "vkr_geometry_upload.h"
#include "vkr_packed_geometry.h"

/* Tool-private source-import state. Runtime loading consumes cooked
 * VkrGeometryUploadRange records directly and does not depend on these
 * mutable builder records. */
typedef struct VkrMeshLoaderBuffer {
  uint32_t vertex_size;
  uint32_t vertex_count;
  void *vertices;
  uint32_t index_size;
  uint32_t index_count;
  void *indices;
  VkrGpuVertexLayout vertex_layout;
  VkrGpuGeometryDecodeRecord *decodes;
  uint32_t decode_count;
  VkrGeometryQuantizationMetrics quantization;
} VkrMeshLoaderBuffer;

typedef struct VkrMeshLoaderSubmeshRange {
  uint32_t range_id;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t decode_index;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  String8 material_name;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
} VkrMeshLoaderSubmeshRange;
Array(VkrMeshLoaderSubmeshRange);

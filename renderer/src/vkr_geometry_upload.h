#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "vkr_gpu_abi.h"
#include "vkr_render_resources.h"
#include "vkr_renderer.h"

/**
 * Borrowed CPU geometry bytes and decode records consumed during GPU asset
 * publication. The publisher copies the data before this view expires.
 */
typedef struct VkrGeometryUploadBuffer {
  uint32_t vertex_size;
  uint32_t vertex_count;
  const void *vertices;
  uint32_t index_size;
  uint32_t index_count;
  const void *indices;
  VkrGpuVertexLayout vertex_layout;
  const VkrGpuGeometryDecodeRecord *decodes;
  uint32_t decode_count;
} VkrGeometryUploadBuffer;

/**
 * One draw range inside a shared upload buffer. Material identity remains a
 * runtime concern; the handle is resolved before publication.
 */
typedef struct VkrGeometryUploadRange {
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
} VkrGeometryUploadRange;
Array(VkrGeometryUploadRange);

/**
 * Borrowed geometry publication input. `submeshes` and every buffer pointer
 * remain valid until the publisher returns.
 */
typedef struct VkrGeometryUpload {
  VkrGeometryUploadBuffer buffer;
  const VkrGeometryUploadRange *submeshes;
  uint32_t submesh_count;
} VkrGeometryUpload;

/**
 * Geometry metadata used to create a publisher-owned GPU resource. The source
 * buffers and decode records are borrowed only for the creation call.
 */
typedef struct VkrGeometryConfig {
  uint32_t vertex_size;
  uint32_t vertex_count;
  const void *vertices;
  uint32_t index_size;
  uint32_t index_count;
  const void *indices;
  VkrGpuVertexLayout vertex_layout;
  const VkrGpuGeometryDecodeRecord *decodes;
  uint32_t decode_count;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  char name[GEOMETRY_NAME_MAX_LENGTH];
  char material_name[MATERIAL_NAME_MAX_LENGTH];
} VkrGeometryConfig;

#pragma once

#include "defines.h"
#include "math/mat.h"

/** Maximum packet instance records accepted per frame. */
#define VKR_INSTANCE_BUFFER_MAX_INSTANCES 65536
/** Fixed P3 candidate/visible capacity; growth publishes a later generation. */
#define VKR_GPU_DRAW_CANDIDATE_CAPACITY 262144u

typedef struct VkrGeometryMegabufferMetrics {
  uint64_t vertex_capacity_bytes;
  uint64_t index_capacity_bytes;
  uint64_t live_bytes;
  uint64_t fragmentation_bytes;
  uint64_t high_water_bytes;
  uint64_t rejected_publications;
  uint64_t generation_replacements;
  uint32_t generation;
} VkrGeometryMegabufferMetrics;

/** GPU-visible instance record shared by the selected implementations. */
typedef struct VkrInstanceDataGPU {
  Mat4 model;
  uint32_t object_id;
  uint32_t reserved[3];
} VkrInstanceDataGPU;

_Static_assert(sizeof(VkrInstanceDataGPU) == 80,
               "VkrInstanceDataGPU must be 80 bytes");
_Static_assert(sizeof(VkrInstanceDataGPU) % 16 == 0,
               "VkrInstanceDataGPU must be 16-byte aligned");

/** Vertex layouts addressable through a published geometry row. */
typedef enum VkrGpuVertexLayout {
  VKR_GPU_VERTEX_LAYOUT_3D = 0,
  VKR_GPU_VERTEX_LAYOUT_COUNT,
} VkrGpuVertexLayout;

/**
 * Immutable geometry-table row for one publication generation.
 *
 * Addresses are backend-native GPU virtual addresses. first_vertex and
 * first_index are element bases within the referenced generation buffers.
 */
typedef struct VkrGpuGeometryRow {
  uint64_t vertex_address;
  uint64_t index_address;
  uint32_t first_vertex;
  uint32_t first_index;
  uint32_t vertex_stride;
  uint32_t vertex_layout;
  uint32_t publication_generation;
  uint32_t flags;
  uint32_t reserved[2];
} VkrGpuGeometryRow;

/** One bounded instance x submesh source row consumed by GPU culling. */
typedef struct VkrGpuCandidateDrawRow {
  uint32_t geometry_index;
  uint32_t material_index;
  uint32_t instance_index;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t state_bucket;
  uint32_t flags;
  Vec4 local_bounding_sphere;
} VkrGpuCandidateDrawRow;

/** One compacted row consumed by table-driven raster and material resolve. */
typedef struct VkrGpuVisibleDrawRow {
  uint32_t geometry_index;
  uint32_t material_index;
  uint32_t instance_index;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t state_bucket;
  uint32_t flags;
} VkrGpuVisibleDrawRow;

_Static_assert(sizeof(VkrGpuGeometryRow) == 48u, "VkrGpuGeometryRow ABI drift");
_Static_assert(sizeof(VkrGpuCandidateDrawRow) == 48u,
               "VkrGpuCandidateDrawRow ABI drift");
_Static_assert(sizeof(VkrGpuVisibleDrawRow) == 32u,
               "VkrGpuVisibleDrawRow ABI drift");

typedef enum VkrGpuAbiRecordId {
  VKR_GPU_ABI_VERTEX = 0,
  VKR_GPU_ABI_INSTANCE,
  VKR_GPU_ABI_TEXT_VERTEX,
  VKR_GPU_ABI_GEOMETRY_ROW,
  VKR_GPU_ABI_CANDIDATE_DRAW_ROW,
  VKR_GPU_ABI_VISIBLE_DRAW_ROW,
  VKR_GPU_ABI_RECORD_COUNT,
} VkrGpuAbiRecordId;

typedef struct VkrGpuAbiField {
  const char *host_name;
  const char *shader_name;
  uint32_t expected_offset;
  uint32_t host_offset;
} VkrGpuAbiField;

typedef struct VkrGpuAbiRecord {
  const char *host_name;
  const char *shader_name;
  uint32_t expected_size;
  uint32_t expected_alignment;
  uint32_t host_size;
  uint32_t host_alignment;
  const VkrGpuAbiField *fields;
  uint32_t field_count;
} VkrGpuAbiRecord;

const VkrGpuAbiRecord *vkr_gpu_abi_record(VkrGpuAbiRecordId id);
bool8_t vkr_gpu_abi_validate_host(void);

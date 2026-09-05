#pragma once

#include "core/ui/vkr_ui_vertex.h"
#include "defines.h"
#include "math/mat.h"

/** Shared UI/world-text vertex record consumed by both native backends. */
typedef VkrUiVertex VkrTextVertex;

/** Maximum packet instance records accepted per frame. */
#define VKR_INSTANCE_BUFFER_MAX_INSTANCES 65536
/** Fixed P3 candidate/visible capacity; growth publishes a later generation. */
#define VKR_GPU_DRAW_CANDIDATE_CAPACITY 262144u
#define VKR_TEMPORAL_TRANSFORM_CAPACITY 32768u
/** Stable mesh-local submesh token occupies bits above transform-writer bit. */
#define VKR_INSTANCE_TEMPORAL_SURFACE_SHIFT 1u

typedef enum VkrInstanceTemporalFlag {
  VKR_INSTANCE_TEMPORAL_OWNER = 1u << 0u,
} VkrInstanceTemporalFlag;
#define VKR_GPU_GEOMETRY_DECODE_STATIC_V1 1u

enum {
  VKR_GPU_TRANSMISSION_LAYER_COUNT = 4,
  VKR_GPU_TRANSMISSION_DIAGNOSTIC_LAYER_COUNT = 5,
};

/** Opaque/cutout pipeline state classes used by GPU draw compaction. */
typedef enum VkrWorldDrawStateBucket {
  VKR_WORLD_DRAW_STATE_OPAQUE_BACK = 0,
  VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED,
  VKR_WORLD_DRAW_STATE_CUTOUT_BACK,
  VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED,
  VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
} VkrWorldDrawStateBucket;

#define VKR_GPU_DRAW_STATE_BUCKET_BITS 2u
#define VKR_GPU_DRAW_STATE_BUCKET_MASK 0x3u

_Static_assert(VKR_WORLD_DRAW_STATE_BUCKET_COUNT ==
                   VKR_GPU_DRAW_STATE_BUCKET_MASK + 1u,
               "Packed draw state requires exactly four buckets");

static INLINE uint32_t vkr_gpu_draw_state_flags(uint32_t state_bucket,
                                                uint32_t flags) {
  return (flags << VKR_GPU_DRAW_STATE_BUCKET_BITS) |
         (state_bucket & VKR_GPU_DRAW_STATE_BUCKET_MASK);
}

typedef struct VkrGeometryMegabufferMetrics {
  uint64_t vertex_capacity_bytes;
  uint64_t index_capacity_bytes;
  uint64_t vertex_live_bytes;
  uint64_t index_live_bytes;
  uint64_t live_bytes;
  uint64_t fragmentation_bytes;
  uint64_t high_water_bytes;
  uint64_t vertex_high_water_bytes;
  uint64_t index_high_water_bytes;
  uint64_t vertex_uploaded_bytes_total;
  uint64_t decode_metadata_live_bytes;
  uint64_t decode_metadata_high_water_bytes;
  uint64_t decode_metadata_uploaded_bytes_total;
  uint64_t index_uploaded_bytes_total;
  uint64_t rejected_publications;
  uint64_t generation_replacements;
  uint32_t generation;
} VkrGeometryMegabufferMetrics;

/** Caller-owned instance input; native upload adds derived normal columns. */
typedef struct VkrInstanceDataGPU {
  Mat4 model;
  uint32_t object_id;
  uint32_t temporal_index;
  uint32_t temporal_generation;
  /** OWNER bit zero; remaining bits carry stable mesh-local submesh + 1.
      Ordinary blend history requires a token in [1, 0x1ffff]. */
  uint32_t temporal_flags;
} VkrInstanceDataGPU;

_Static_assert(sizeof(VkrInstanceDataGPU) == 80,
               "VkrInstanceDataGPU must be 80 bytes");
_Static_assert(sizeof(VkrInstanceDataGPU) % 16 == 0,
               "VkrInstanceDataGPU must be 16-byte aligned");

/** Native instance row. The first 80 bytes retain the source field offsets. */
typedef struct VkrPreparedInstanceGPU {
  Mat4 model;
  uint32_t object_id;
  uint32_t temporal_index;
  uint32_t temporal_generation;
  /** OWNER in bit zero; stable submesh_index + 1 in the remaining bits. */
  uint32_t temporal_flags;
  /** Positive-scaled inverse transpose; column 0 w carries model handedness. */
  Vec4 normal_column0;
  Vec4 normal_column1;
  Vec4 normal_column2;
} VkrPreparedInstanceGPU;

_Static_assert(sizeof(VkrPreparedInstanceGPU) == 128,
               "Native prepared instance must be 128 bytes");

/** Computes normal transport once at native instance publication/upload. */
VkrPreparedInstanceGPU vkr_gpu_prepare_instance(const VkrInstanceDataGPU *source);

/** One completion-protected object transform indexed by stable temporal ID. */
typedef struct VkrTemporalTransformGPU {
  Mat4 model;
  uint32_t generation;
  uint32_t frame_index;
  uint32_t valid;
  uint32_t reserved;
} VkrTemporalTransformGPU;

_Static_assert(sizeof(VkrTemporalTransformGPU) == 80,
               "VkrTemporalTransformGPU must be 80 bytes");

/** Vertex layouts addressable through a published geometry row. */
typedef enum VkrGpuVertexLayout {
  VKR_GPU_VERTEX_LAYOUT_3D = 0,
  VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1 = 1,
  VKR_GPU_VERTEX_LAYOUT_COUNT,
} VkrGpuVertexLayout;

typedef struct VkrPackedStaticVertex {
  uint32_t words[8];
} VkrPackedStaticVertex;

typedef struct VkrGpuGeometryDecodeRecord {
  float32_t position_bias[3];
  uint32_t flags;
  float32_t position_scale[3];
  uint32_t reserved;
} VkrGpuGeometryDecodeRecord;

_Static_assert(sizeof(VkrPackedStaticVertex) == 32,
               "Packed static vertex ABI must be 32 bytes");
_Static_assert(sizeof(VkrGpuGeometryDecodeRecord) == 32,
               "Geometry decode record ABI must be 32 bytes");

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
  uint64_t decode_address;
} VkrGpuGeometryRow;

/**
 * Relocates megabuffer base addresses while preserving decode-record offset.
 */
void vkr_gpu_geometry_row_relocate(VkrGpuGeometryRow *row,
                                   uint64_t vertex_address,
                                   uint64_t index_address,
                                   uint32_t publication_generation);

/** One bounded instance x submesh source row consumed by GPU culling. */
typedef struct VkrGpuCandidateDrawRow {
  uint32_t geometry_index;
  uint32_t material_index;
  uint32_t instance_index;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t decode_index;
  uint32_t state_flags;
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
  uint32_t decode_index;
  uint32_t state_flags;
} VkrGpuVisibleDrawRow;

/** Shared packed punctual-light row. */
typedef struct VkrGpuPointLightRow {
  Vec4 p0;
  Vec4 p1;
  Vec4 p2;
  Vec4 p3;
} VkrGpuPointLightRow;

/** GPU-written compacted work volume and four-bucket prefix state. */
typedef struct VkrGpuDrawCompactionState {
  uint32_t execution_ranges[VKR_WORLD_DRAW_STATE_BUCKET_COUNT][2];
  uint32_t bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t bucket_cursors[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t visible_count;
  uint32_t overflow_count;
  uint32_t resolve_invalid_count;
  uint32_t occlusion_culled_count;
} VkrGpuDrawCompactionState;

/** GPU-written diagnostics stored in the authored transmission state buffer. */
typedef struct VkrGpuTransmissionDiagnostics {
  VkrGpuDrawCompactionState compaction;
  uint32_t covered_pixels[VKR_GPU_TRANSMISSION_DIAGNOSTIC_LAYER_COUNT];
  uint32_t compact_overflow[VKR_GPU_TRANSMISSION_LAYER_COUNT];
} VkrGpuTransmissionDiagnostics;

_Static_assert(sizeof(VkrGpuGeometryRow) == 48u, "VkrGpuGeometryRow ABI drift");
_Static_assert(sizeof(VkrGpuCandidateDrawRow) == 48u,
               "VkrGpuCandidateDrawRow ABI drift");
_Static_assert(sizeof(VkrGpuVisibleDrawRow) == 32u,
               "VkrGpuVisibleDrawRow ABI drift");
_Static_assert(sizeof(VkrGpuPointLightRow) == 64u,
               "VkrGpuPointLightRow ABI drift");
_Static_assert(_Alignof(VkrGpuPointLightRow) == 16u,
               "VkrGpuPointLightRow alignment drift");
_Static_assert(sizeof(VkrGpuDrawCompactionState) == 80u,
               "VkrGpuDrawCompactionState ABI drift");
_Static_assert(sizeof(VkrGpuTransmissionDiagnostics) == 116u,
               "VkrGpuTransmissionDiagnostics ABI drift");
_Static_assert(offsetof(VkrGpuTransmissionDiagnostics, covered_pixels) == 80u,
               "Transmission coverage ABI drift");
_Static_assert(offsetof(VkrGpuTransmissionDiagnostics, compact_overflow) ==
                   100u,
               "Transmission compact-overflow ABI drift");

typedef enum VkrGpuAbiRecordId {
  VKR_GPU_ABI_VERTEX = 0,
  VKR_GPU_ABI_PACKED_STATIC_VERTEX,
  VKR_GPU_ABI_GEOMETRY_DECODE_RECORD,
  VKR_GPU_ABI_INSTANCE,
  VKR_GPU_ABI_TEXT_VERTEX,
  VKR_GPU_ABI_GEOMETRY_ROW,
  VKR_GPU_ABI_CANDIDATE_DRAW_ROW,
  VKR_GPU_ABI_VISIBLE_DRAW_ROW,
  VKR_GPU_ABI_POINT_LIGHT_ROW,
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

#pragma once

#include "defines.h"
#include "math/mat.h"
#include "vkr_ui_draw_types.h"

/** World-text vertex record consumed by both native backends. The UI stream
 * uses the wider VkrUiVertex. */
typedef struct VkrTextVertex {
  Vec2 position;
  Vec2 texcoord;
  Vec4 color;
} VkrTextVertex;

_Static_assert(sizeof(VkrTextVertex) == 32u,
               "VkrTextVertex must remain a 32-byte GPU record");

/** Maximum packet instance records accepted per frame. */
#define VKR_INSTANCE_BUFFER_MAX_INSTANCES 65536
/** Fixed P3 candidate/visible capacity; growth publishes a later generation. */
#define VKR_GPU_DRAW_CANDIDATE_CAPACITY 262144u
#define VKR_TEMPORAL_TRANSFORM_CAPACITY 32768u
/** Bounded per-view deformation capacity. Overflow rejects the packet. */
#define VKR_SKINNING_BINDING_CAPACITY 64u
#define VKR_SKINNING_JOINT_CAPACITY 65536u
#define VKR_SKINNING_VERTEX_CAPACITY 2097152u

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
  VKR_WORLD_DRAW_STATE_OPAQUE_BACK_MIRRORED,
  VKR_WORLD_DRAW_STATE_OPAQUE_DOUBLE_SIDED_MIRRORED,
  VKR_WORLD_DRAW_STATE_CUTOUT_BACK_MIRRORED,
  VKR_WORLD_DRAW_STATE_CUTOUT_DOUBLE_SIDED_MIRRORED,
  VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
} VkrWorldDrawStateBucket;

#define VKR_GPU_DRAW_STATE_BUCKET_BITS 3u
#define VKR_GPU_DRAW_STATE_BUCKET_MASK 0x7u
#define VKR_GPU_DRAW_STATE_MIRRORED_BIT 0x4u

_Static_assert(VKR_WORLD_DRAW_STATE_BUCKET_COUNT ==
                   VKR_GPU_DRAW_STATE_BUCKET_MASK + 1u,
               "Packed draw state requires exactly eight buckets");

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
  uint64_t reusable_range_bytes;
  uint64_t retired_range_bytes;
  uint64_t live_range_count;
  uint64_t retired_range_count;
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
  /** Zero is static; otherwise indexes world.skinning[index - 1]. */
  uint32_t skinning_index;
  uint32_t reserved[3];
} VkrInstanceDataGPU;

_Static_assert(sizeof(VkrInstanceDataGPU) == 96,
               "VkrInstanceDataGPU must be 96 bytes");
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
  /** Positive-scaled inverse transpose; column 0 w carries model handedness.

   * Column 1 w carries the conservative affine sphere stretch. */
  Vec4 normal_column0;
  Vec4 normal_column1;
  Vec4 normal_column2;
  uint64_t deformation_address;
  uint64_t previous_deformation_address;
} VkrPreparedInstanceGPU;

_Static_assert(sizeof(VkrPreparedInstanceGPU) == 144,
               "Native prepared instance must be 144 bytes");

/** Computes normal transport and bounds once at native publication/upload. */
VkrPreparedInstanceGPU
vkr_gpu_prepare_instance(const VkrInstanceDataGPU *source);

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
  /** Record offset, from the geometry's first decode record, of this range's
      VkrGpuGeometryLodRow; zero when the range has one level. */
  uint32_t lod_record;
} VkrGpuGeometryDecodeRecord;

_Static_assert(sizeof(VkrPackedStaticVertex) == 32,
               "Packed static vertex ABI must be 32 bytes");
_Static_assert(sizeof(VkrGpuGeometryDecodeRecord) == 32,
               "Geometry decode record ABI must be 32 bytes");

/* Detail levels of one range (ADR-084). Level 0 is the range itself; each
   later level holds fewer indices over the same vertices and a larger
   model-space error. Culling picks a level per candidate and view, and
   encoding draws that level's indices. */
#define VKR_GPU_GEOMETRY_LOD_LEVEL_MAX 7u
/* The range is a terrain tile: VKR_GPU_TERRAIN_TILE_SIDE squared grid
   vertices, then a skirt vertex below each edge vertex, edges in -Z, +X, +Z,
   -X order. Level L draws every 2^L-th grid line with the same diagonal, and
   a vertex that level L + 1 drops can morph onto it. */
#define VKR_GPU_GEOMETRY_LOD_TERRAIN_GRID 0x1u
#define VKR_GPU_TERRAIN_TILE_SIDE 65u

typedef struct VkrGpuGeometryLodLevel {
  /** First index relative to the range's first index. */
  uint32_t first_index;
  uint32_t index_count;
  /** Largest model-space distance from level 0's surface, in metres. */
  float32_t error;
  uint32_t reserved;
} VkrGpuGeometryLodLevel;

/** A range's levels; occupies four decode records after its decode record. */
typedef struct VkrGpuGeometryLodRow {
  uint32_t level_count;
  uint32_t flags;
  uint32_t reserved[2];
  VkrGpuGeometryLodLevel levels[VKR_GPU_GEOMETRY_LOD_LEVEL_MAX];
} VkrGpuGeometryLodRow;

#define VKR_GPU_GEOMETRY_LOD_RECORDS                                           \
  (sizeof(VkrGpuGeometryLodRow) / sizeof(VkrGpuGeometryDecodeRecord))

_Static_assert(sizeof(VkrGpuGeometryLodLevel) == 16,
               "Geometry LOD level ABI must be 16 bytes");
_Static_assert(sizeof(VkrGpuGeometryLodRow) == 128,
               "Geometry LOD row ABI must be 128 bytes");

/* A view projects a level's error onto its image without distance (an
   orthographic camera or cascade) instead of dividing by it. */
#define VKR_GPU_LOD_VIEW_CONSTANT 0x1u
/* A view morphs terrain grids toward their next level (the camera). */
#define VKR_GPU_LOD_VIEW_MORPH 0x2u
/* A level begins morphing toward the next at this share of the distance at
   which the next level takes over. */
#define VKR_GPU_LOD_MORPH_START 0.75f

/** Level-of-detail policy of one culling view. */
typedef struct VkrGpuLodView {
  /** xyz: view position; w: image size of one metre of error at unit
      distance (or at any distance for a constant view) over the error
      threshold. Zero draws level 0. */
  Vec4 position_scale;
  /** xyz: the view position one frame earlier, for motion vectors. */
  Vec4 previous_position;
  uint32_t flags;
  uint32_t reserved[3];
} VkrGpuLodView;

_Static_assert(sizeof(VkrGpuLodView) == 48, "LOD view ABI must be 48 bytes");

/* Projected error, in pixels or texels, at which the next level takes over. */
#define VKR_GPU_LOD_THRESHOLD 1.0f

/** The LOD policy of a view whose `view_projection` covers `image_rows`
    rows: perspective projections scale with distance from `position`,
    orthographic ones are constant. `morph` lets terrain grids morph (the
    camera); `previous_position` is the view's position one frame earlier. */
VkrGpuLodView vkr_gpu_lod_view(Mat4 view_projection, float32_t image_rows,
                               Vec3 position, Vec3 previous_position,
                               bool8_t morph);

/* The LOD state encoding writes above a visible row's candidate flags: the
   level, the morph factor toward the next level now and one frame earlier
   (8-bit unorm each), and whether the range morphs as a terrain grid. */
#define VKR_GPU_DRAW_CANDIDATE_FLAG_MASK 0xFu
#define VKR_GPU_DRAW_GEOMORPH_BIT (1u << 7u)
#define VKR_GPU_DRAW_LOD_SHIFT 8u
#define VKR_GPU_DRAW_LOD_MASK 0x7u
#define VKR_GPU_DRAW_MORPH_SHIFT 11u
#define VKR_GPU_DRAW_PREVIOUS_MORPH_SHIFT 19u

/** Compute output indexed in geometry-local vertex order. */
typedef struct VkrDeformedVertex {
  float32_t position[3];
  uint32_t normal_packed;
  uint32_t tangent_packed;
  float32_t tangent_sign;
  uint32_t reserved[2];
} VkrDeformedVertex;

/** Four normalized linear-blend influences; zero weights mean no skinning. */
typedef struct VkrSkinningInfluence {
  uint32_t joints[4];
  float32_t weights[4];
} VkrSkinningInfluence;

_Static_assert(sizeof(VkrDeformedVertex) == 32, "Deformation ABI drift");
_Static_assert(sizeof(VkrSkinningInfluence) == 32, "Influence ABI drift");

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

/* Local shadow views receivers can sample in one frame: six faces of every
 * light of the scene table, which the persistent cache keeps resident. */
#define VKR_LOCAL_SHADOW_FACE_COUNT_MAX 768u
/* Faces drawn in one frame. Each render slot owns a culling view and its
 * repeated graph passes, so slot masks are 64-bit. The configured face budget
 * bounds the slots drawn per frame and the transmission layers. */
#define VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX 64u
/* The map size is the largest face; a light's face side follows its range. */
#define VKR_LOCAL_SHADOW_MAP_SIZE_DEFAULT 1024u
#define VKR_LOCAL_SHADOW_MAP_SIZE_MAX 1024u
#define VKR_LOCAL_SHADOW_FACE_SIZE_MIN 128u
/* Face texels per metre of light range, before rounding to a power of two:
 * a 7.5 m light takes 512 squared and a 15 m light 1024 squared. */
#define VKR_LOCAL_SHADOW_TEXELS_PER_RANGE_METRE 64.0f
/* Every local shadow face occupies a square of one layer of this depth atlas.
 * The cache sizes the layer count to its resident faces; the atlas clear mask
 * is 32-bit. */
#define VKR_LOCAL_SHADOW_ATLAS_SIZE 4096u
#define VKR_LOCAL_SHADOW_ATLAS_LAYER_COUNT_MAX 32u
#define VKR_LOCAL_SHADOW_TRANSMISSION_MAP_SIZE_MAX 512u
/* Texels per transmission array: 32 faces at 512 squared. The arrays hold one
 * layer per face of the face budget, for the most important lights; larger
 * budgets take smaller transmission maps, so the pool's memory does not grow
 * with the budget. */
#define VKR_LOCAL_SHADOW_TRANSMISSION_TEXEL_BUDGET (32u * 512u * 512u)
/* Layers of the screen-space shadow mask: the k-th shadowed light in range of
 * a pixel, in light order, stores its visibility in layer k. Receivers filter
 * lights past the last layer inline. Matches the shaders'
 * VKR_LOCAL_SHADOW_MASK_SLOT_COUNT and the render graph's mask layers. */
#define VKR_LOCAL_SHADOW_MASK_LAYER_COUNT 8u
/* Frames before the contact-shadow march pattern repeats under TAA. */
#define VKR_LOCAL_SHADOW_CONTACT_NOISE_PERIOD 64u

_Static_assert(VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX <= 64u,
               "local shadow render slot and transmission layer masks are "
               "64-bit");

/** Bits of `count` consecutive render slots or layers starting at `first`;
 * the whole word when they cover all 64 bits. */
static inline uint64_t vkr_local_shadow_view_bits(uint32_t first,
                                                  uint32_t count) {
  if (count == 0u || first >= 64u)
    return 0u;
  const uint64_t bits = count >= 64u ? UINT64_MAX : (UINT64_C(1) << count) - 1u;
  return bits << first;
}

/** Side of each transmission array layer for a face budget: the largest face,
 * capped at VKR_LOCAL_SHADOW_TRANSMISSION_MAP_SIZE_MAX, halved until one layer
 * per face fits VKR_LOCAL_SHADOW_TRANSMISSION_TEXEL_BUDGET. */
static inline uint32_t
vkr_local_shadow_transmission_map_size(uint32_t map_size,
                                       uint32_t face_budget) {
  uint32_t size = Min(map_size, VKR_LOCAL_SHADOW_TRANSMISSION_MAP_SIZE_MAX);
  while (size > VKR_LOCAL_SHADOW_FACE_SIZE_MIN &&
         (uint64_t)face_budget * size * size >
             VKR_LOCAL_SHADOW_TRANSMISSION_TEXEL_BUDGET)
    size /= 2u;
  return size;
}

/** Perspective shadow view. CPU stores canonical column-major matrices;
 * native upload applies the same lowering as its directional shadow views. */
typedef struct VkrLocalShadowView {
  Mat4 light_view_projection;
  Vec4 light_position_near;
  Vec4 light_direction_far;
  Vec4 projection_params; /* tan(half FOV), inverse size, bias, normal offset */
  /** x: shadow strength in [0, 1], shared by every face of the light; receivers
   * blend visibility toward one as it falls. y: the face's transmission array
   * layer plus one, or zero when refractive casters do not attenuate it. z:
   * one when the light takes one filtered tap and no contact shadows, else
   * zero. w is zero. */
  Vec4 shadow_params;
  /** The face's square in the atlas: xy is its top-left corner and z its side
   * in atlas UV, w the atlas layer. projection_params.y is one face texel. */
  Vec4 atlas_rect;
} VkrLocalShadowView;
_Static_assert(sizeof(VkrLocalShadowView) == 144u,
               "Local shadow view ABI drift");

/** Shared packed punctual-light row. p3.w is native first shadow view + 1;
 * zero selects unshadowed lighting. */
typedef struct VkrGpuPointLightRow {
  Vec4 p0;
  Vec4 p1;
  Vec4 p2;
  Vec4 p3;
} VkrGpuPointLightRow;

typedef struct VkrGpuRectangleLightRow {
  Vec4 center_half_width;
  Vec4 right_half_height;
  Vec4 up_radiance;
  Vec4 color;
} VkrGpuRectangleLightRow;
_Static_assert(sizeof(VkrGpuRectangleLightRow) == 64u,
               "Rectangle light row ABI drift");
_Static_assert(_Alignof(VkrGpuRectangleLightRow) == 16u,
               "Rectangle light row alignment drift");

/** GPU-written compacted work volume and parity-aware bucket prefix state. */
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
_Static_assert(sizeof(VkrGpuDrawCompactionState) == 144u,
               "VkrGpuDrawCompactionState ABI drift");
_Static_assert(sizeof(VkrGpuTransmissionDiagnostics) == 180u,
               "VkrGpuTransmissionDiagnostics ABI drift");
_Static_assert(offsetof(VkrGpuTransmissionDiagnostics, covered_pixels) == 144u,
               "Transmission coverage ABI drift");
_Static_assert(offsetof(VkrGpuTransmissionDiagnostics, compact_overflow) ==
                   164u,
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
  VKR_GPU_ABI_LOCAL_SHADOW_VIEW,
  VKR_GPU_ABI_COLOR_GRADING,
  VKR_GPU_ABI_RECTANGLE_LIGHT_ROW,
  VKR_GPU_ABI_GEOMETRY_LOD_ROW,
  VKR_GPU_ABI_LOD_VIEW,
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

struct VkrMaterial;

/** Material-row anisotropy {strength, cos(rotation), sin(rotation), 0}. The
 * rotation is reduced to one turn before the trigonometry. */
Vec4 vkr_gpu_material_anisotropy(float32_t strength, float32_t rotation);

/**
 * Validates the diffuse-transmission and subsurface extensions a material row
 * carries. Each is finite and within [0, 1]; a nonzero strength requires a
 * PBR material without blending, specular transmission or volume thickness,
 * and subsurface also excludes diffuse transmission. A terrain material is
 * opaque PBR without those extensions, with finite layer factors.
 */
bool8_t vkr_gpu_material_extensions_valid(const struct VkrMaterial *material);

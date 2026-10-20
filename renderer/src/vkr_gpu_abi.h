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
/** Geometry IDs the geometry system hands out. Metal's mesh table and
 * Vulkan's publication records are indexed by them, so all three take this.
 * Every brush material group is a geometry, so a large level reaches it
 * before its mesh or instance counts. */
#define VKR_GEOMETRY_CAPACITY 32768u
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
/* Set beside STATIC_V1 when a range's vertices carry a lightmap UV set in
   word 7 as two unorm16 values, U in the low half (ADR-031, ADR-087). */
#define VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV 2u

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

/* A candidate's draw-distance word (VkrWorldDrawCandidate::cull), above the
   bucket and the four candidate flags of its state: the camera's draw
   distance code in bits 0 to 7, the smallest diameter the camera draws in
   half pixels in bits 8 to 11, the shadow distance code in bits 12 to 19,
   and the room and copy bits below. A code c above zero stands for
   2^((c - 1) / 16) metres; zero is no limit. Mirrors
   shared/lod_kernel.slangh. */
#define VKR_GPU_DRAW_CULL_SHIFT 7u
#define VKR_GPU_DRAW_CULL_CODE_MASK 0xFFu
#define VKR_GPU_DRAW_CULL_PIXELS_SHIFT 8u
#define VKR_GPU_DRAW_CULL_PIXELS_MASK 0xFu
#define VKR_GPU_DRAW_CULL_SHADOW_SHIFT 12u
#define VKR_GPU_DRAW_CULL_CODES_PER_OCTAVE 16.0f
/* Bit 20: the candidate's rooms are hidden from the camera this frame; bit
   21: its rooms are sealed, so no cascade draws it (vkr_scene_rooms.h). */
#define VKR_GPU_DRAW_CULL_HIDDEN_BIT (1u << 20u)
#define VKR_GPU_DRAW_CULL_SEALED_BIT (1u << 21u)
/* Bit 22: a scatter or spline copy, which a cascade draws only while its
   bounding sphere spans VKR_GPU_CASCADE_COPY_MIN_TEXELS (lod_kernel.slangh)
   instead of one texel. */
#define VKR_GPU_DRAW_CULL_COPY_BIT (1u << 22u)
#define VKR_GPU_DRAW_CULL_BITS 23u
/* The candidate state bits a visible row keeps: the bucket and the flags. */
#define VKR_GPU_DRAW_CANDIDATE_STATE_MASK 0x7Fu

_Static_assert(VKR_GPU_DRAW_STATE_BUCKET_BITS + 4u == VKR_GPU_DRAW_CULL_SHIFT,
               "the cull word follows the four candidate flags");
_Static_assert(VKR_GPU_DRAW_CULL_SHIFT + VKR_GPU_DRAW_CULL_BITS <= 32u,
               "the cull word fits the candidate state");

static INLINE uint32_t vkr_gpu_draw_state_flags(uint32_t state_bucket,
                                                uint32_t flags, uint32_t cull) {
  return (cull << VKR_GPU_DRAW_CULL_SHIFT) |
         (flags << VKR_GPU_DRAW_STATE_BUCKET_BITS) |
         (state_bucket & VKR_GPU_DRAW_STATE_BUCKET_MASK);
}

/** The smallest code whose distance reaches `metres`, so a candidate never
    culls before its authored distance; zero for no limit (zero, negative,
    infinite or not a number). */
uint32_t vkr_gpu_draw_cull_code(float32_t metres);
/** The distance code `code` stands for; infinity for zero. */
float32_t vkr_gpu_draw_cull_metres(uint32_t code);
/** A cull word from a draw distance and a shadow distance in metres (zero
    for no limit) and the smallest drawn diameter in pixels, rounded up to
    half pixels and clamped to 7.5. A draw distance also limits the shadow
    distance, so a caster the camera stops drawing leaves the cascades that
    start past it. */
uint32_t vkr_gpu_draw_cull_word(float32_t draw_distance,
                                float32_t min_screen_pixels,
                                float32_t shadow_distance);
/** Whether the camera skips a candidate of `cull` whose bounding sphere of
    `radius` lies `distance` metres away at its nearest point, for a view of
    LOD `scale` and `flags`: its rooms are hidden, or it fails the distance
    tests; mirrors vkr_gpu_draw_distance_culled. */
bool8_t vkr_gpu_draw_distance_culled(uint32_t cull, float32_t scale,
                                     uint32_t flags, float32_t distance_scale,
                                     float32_t distance, float32_t radius);

/** Which of those camera tests skips the candidate: none, the hidden bit
    (its rooms are hidden or a proxy cell draws in its place), its draw
    distance, or its smallest drawn size. */
typedef enum VkrGpuDrawCullReason {
  VKR_GPU_DRAW_CULL_REASON_NONE = 0,
  VKR_GPU_DRAW_CULL_REASON_HIDDEN,
  VKR_GPU_DRAW_CULL_REASON_DISTANCE,
  VKR_GPU_DRAW_CULL_REASON_SIZE,
} VkrGpuDrawCullReason;

VkrGpuDrawCullReason
vkr_gpu_draw_distance_reason(uint32_t cull, float32_t scale, uint32_t flags,
                             float32_t distance_scale, float32_t distance,
                             float32_t radius);

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
  /** Zero is not lightmapped; otherwise selects the frame's lightmap
      rects[slot - 1] (ADR-088). */
  uint32_t lightmap_slot;
  uint32_t reserved[2];
  /** Where a mover's draws take baked diffuse light and reflection probes
      (ADR-120): a world point with w one; zero w takes them per pixel. */
  Vec4 lighting_origin;
} VkrInstanceDataGPU;

_Static_assert(sizeof(VkrInstanceDataGPU) == 112,
               "VkrInstanceDataGPU must be 112 bytes");
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

   * Column 1 w carries the conservative affine sphere stretch and column 2 w
   * the lightmap slot as an exact float. */
  Vec4 normal_column0;
  Vec4 normal_column1;
  Vec4 normal_column2;
  uint64_t deformation_address;
  uint64_t previous_deformation_address;
  /** The source's lighting origin (ADR-120): a world point with w one, or
      zero w to take baked light and probes per pixel. */
  Vec4 lighting_origin;
} VkrPreparedInstanceGPU;

_Static_assert(sizeof(VkrPreparedInstanceGPU) == 160 &&
                   offsetof(VkrPreparedInstanceGPU, lighting_origin) == 144,
               "Native prepared instance must be 160 bytes");

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

/* Detail levels of one range (ADR-085). Level 0 is the range itself; each
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
/* A shadow view draws only the static casters, the candidates below the
   culling root's static count (a static square or cascade layer), or only
   the dynamic ones (a dynamic square or composed cascade over a copy of the
   static one). Without either bit it draws every caster. */
#define VKR_GPU_LOD_VIEW_STATIC_CASTERS_ONLY 0x4u
#define VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY 0x8u
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
  /** The camera's multiplier of candidates' draw distances and divisor of
      their smallest drawn diameters; zero draws every candidate. */
  float32_t distance_scale;
  /** A cascade's slice start from the camera, in metres, at least
      VKR_GPU_LOD_SHADOW_NEAR_MIN: casters whose shadow distance ends before
      it, and sealed casters, skip the cascade. Zero for other views. */
  float32_t shadow_near;
  uint32_t reserved;
} VkrGpuLodView;

_Static_assert(sizeof(VkrGpuLodView) == 48, "LOD view ABI must be 48 bytes");

/* The smallest cascade slice start a view carries, so a cascade's is never
   the zero of the other views. */
#define VKR_GPU_LOD_SHADOW_NEAR_MIN 1e-3f

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
/* Above them, the tiled pipeline's reflection probe for a camera-view draw,
   as frame probe index + 1; zero takes the global environment (ADR-087). */
#define VKR_GPU_DRAW_PROBE_SHIFT 27u
#define VKR_GPU_DRAW_PROBE_MASK 0x1Fu
/* A draw that straddles a probe boundary picks its probe per pixel; above
   every frame probe index + 1. */
#define VKR_GPU_DRAW_PROBE_PER_PIXEL 0x1Fu
_Static_assert(VKR_GPU_DRAW_PREVIOUS_MORPH_SHIFT + 8u ==
                   VKR_GPU_DRAW_PROBE_SHIFT,
               "The probe bits follow the previous morph factor");

/* A view's classification word for a candidate it rejects: all ones in the
   four bucket bits, which no drawn word holds (Vulkan stores the bucket,
   Metal bucket + 1), and the rejecting test above them. Every encode skips
   such a word. VKR_GPU_DRAW_DEFERRED, every bit set, is the tiled camera's
   word for a candidate the earlier frame's pyramid hides while it culls in
   two phases (ADR-032). Mirrors shared/lod_kernel.slangh. */
#define VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK 0xFu
#define VKR_GPU_DRAW_REJECTED_BUCKET 0xFu
#define VKR_GPU_DRAW_REJECT_REASON_SHIFT 4u
typedef enum VkrGpuDrawRejectReason {
  /* The view's candidate flags or mobility exclude it, or another path draws
     it (the SDF trace, a planar reflector's own view). */
  VKR_GPU_DRAW_REJECT_FILTERED = 0,
  VKR_GPU_DRAW_REJECT_FRUSTUM = 1,
  /* The camera's room, draw distance or smallest-size tests; a cascade's
     sealed rooms or shadow distance. */
  VKR_GPU_DRAW_REJECT_DISTANCE = 2,
  VKR_GPU_DRAW_REJECT_OCCLUDED = 3,
  /* Below a directional cascade's smallest caster size. */
  VKR_GPU_DRAW_REJECT_CASTER_SMALL = 4,
} VkrGpuDrawRejectReason;
#define VKR_GPU_DRAW_DEFERRED 0xFFFFFFFFu

/* The draw census: what became of each camera candidate this frame, four
   bits per candidate and eight per word, candidate i in bits 4 * (i % 8) of
   word i / 8. */
typedef enum VkrGpuDrawCensusState {
  /* The camera view does not draw the candidate at all (its flags or
     mobility exclude it). */
  VKR_GPU_DRAW_CENSUS_NOT_CAMERA = 0,
  VKR_GPU_DRAW_CENSUS_DRAWN = 1,
  /* Deferred by the earlier frame's pyramid, then drawn by the late view. */
  VKR_GPU_DRAW_CENSUS_DRAWN_LATE = 2,
  VKR_GPU_DRAW_CENSUS_OCCLUDED = 3,
  VKR_GPU_DRAW_CENSUS_FRUSTUM = 4,
  /* Hidden rooms, past its draw distance or below its smallest size. */
  VKR_GPU_DRAW_CENSUS_DISTANCE = 5,
  VKR_GPU_DRAW_CENSUS_STATE_COUNT,
} VkrGpuDrawCensusState;
#define VKR_GPU_DRAW_CENSUS_STATE_BITS 4u
#define VKR_GPU_DRAW_CENSUS_STATE_MASK 0xFu
#define VKR_GPU_DRAW_CENSUS_STATES_PER_WORD 8u

_Static_assert(VKR_WORLD_DRAW_STATE_BUCKET_COUNT < VKR_GPU_DRAW_REJECTED_BUCKET,
               "Metal's bucket + 1 never reaches the rejected bucket bits");
_Static_assert(VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK == 0xFu &&
                   (VKR_GPU_DRAW_DEFERRED &
                    VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) ==
                       VKR_GPU_DRAW_REJECTED_BUCKET,
               "the deferred word skips encoding as a rejected one does");
_Static_assert(VKR_GPU_DRAW_REJECT_REASON_SHIFT == 4u &&
                   (VKR_GPU_DRAW_REJECT_CASTER_SMALL
                    << VKR_GPU_DRAW_REJECT_REASON_SHIFT) <
                       VKR_GPU_DRAW_DEFERRED,
               "no reason word equals the deferred word");
_Static_assert(VKR_GPU_DRAW_CENSUS_STATE_COUNT <=
                       VKR_GPU_DRAW_CENSUS_STATE_MASK + 1u &&
                   VKR_GPU_DRAW_CENSUS_STATE_BITS *
                           VKR_GPU_DRAW_CENSUS_STATES_PER_WORD ==
                       32u,
               "eight four-bit census states fill a word");

static INLINE uint32_t vkr_gpu_draw_rejected(VkrGpuDrawRejectReason reason) {
  return VKR_GPU_DRAW_REJECTED_BUCKET |
         ((uint32_t)reason << VKR_GPU_DRAW_REJECT_REASON_SHIFT);
}

static INLINE bool8_t vkr_gpu_draw_word_drawn(uint32_t word) {
  return (word & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) !=
         VKR_GPU_DRAW_REJECTED_BUCKET;
}

/** The census state of a camera word that draws nothing; mirrors
    vkr_gpu_draw_rejected_state. */
static INLINE uint32_t vkr_gpu_draw_rejected_state(uint32_t word) {
  if (word == VKR_GPU_DRAW_DEFERRED) {
    return VKR_GPU_DRAW_CENSUS_OCCLUDED;
  }
  switch (word >> VKR_GPU_DRAW_REJECT_REASON_SHIFT) {
  case VKR_GPU_DRAW_REJECT_FRUSTUM:
    return VKR_GPU_DRAW_CENSUS_FRUSTUM;
  case VKR_GPU_DRAW_REJECT_DISTANCE:
    return VKR_GPU_DRAW_CENSUS_DISTANCE;
  case VKR_GPU_DRAW_REJECT_OCCLUDED:
    return VKR_GPU_DRAW_CENSUS_OCCLUDED;
  default:
    return VKR_GPU_DRAW_CENSUS_NOT_CAMERA;
  }
}

/** A camera candidate's census state from its early camera word and, when
    that word deferred it, the late view's word; mirrors
    vkr_gpu_draw_camera_state. */
static INLINE uint32_t vkr_gpu_draw_camera_state(uint32_t early,
                                                 uint32_t late) {
  if (vkr_gpu_draw_word_drawn(early)) {
    return VKR_GPU_DRAW_CENSUS_DRAWN;
  }
  if (early != VKR_GPU_DRAW_DEFERRED) {
    return vkr_gpu_draw_rejected_state(early);
  }
  if (vkr_gpu_draw_word_drawn(late)) {
    return VKR_GPU_DRAW_CENSUS_DRAWN_LATE;
  }
  const uint32_t state = vkr_gpu_draw_rejected_state(late);
  return state == VKR_GPU_DRAW_CENSUS_NOT_CAMERA ? VKR_GPU_DRAW_CENSUS_OCCLUDED
                                                 : state;
}

/** `word` with census state `state` in lane `lane` of eight; mirrors
    vkr_gpu_draw_census_put. */
static INLINE uint32_t vkr_gpu_draw_census_put(uint32_t word, uint32_t lane,
                                               uint32_t state) {
  return word | (state << (lane * VKR_GPU_DRAW_CENSUS_STATE_BITS));
}

/** Words of packed census states for `count` candidates. */
static INLINE uint32_t vkr_gpu_draw_census_word_count(uint32_t count) {
  return (count + VKR_GPU_DRAW_CENSUS_STATES_PER_WORD - 1u) /
         VKR_GPU_DRAW_CENSUS_STATES_PER_WORD;
}

/** Candidate `index`'s census state in packed `states`. */
static INLINE uint32_t vkr_gpu_draw_census_get(const uint32_t *states,
                                               uint32_t index) {
  const uint32_t lane = index % VKR_GPU_DRAW_CENSUS_STATES_PER_WORD;
  return (states[index / VKR_GPU_DRAW_CENSUS_STATES_PER_WORD] >>
          (lane * VKR_GPU_DRAW_CENSUS_STATE_BITS)) &
         VKR_GPU_DRAW_CENSUS_STATE_MASK;
}

/** A census state's precedence when one object merges its candidates. */
static INLINE uint32_t vkr_gpu_draw_census_rank(uint32_t state) {
  switch (state) {
  case VKR_GPU_DRAW_CENSUS_DRAWN:
    return 5u;
  case VKR_GPU_DRAW_CENSUS_DRAWN_LATE:
    return 4u;
  case VKR_GPU_DRAW_CENSUS_OCCLUDED:
    return 3u;
  case VKR_GPU_DRAW_CENSUS_FRUSTUM:
    return 2u;
  case VKR_GPU_DRAW_CENSUS_DISTANCE:
    return 1u;
  default:
    return 0u;
  }
}

/** One object's state from two of its candidates' states (its submeshes):
    the object counts as drawn if any candidate drew, then drawn late,
    occluded, outside the frustum, past its distance, and not a camera
    candidate only when none is one. */
static INLINE uint32_t vkr_gpu_draw_census_merge(uint32_t a, uint32_t b) {
  return vkr_gpu_draw_census_rank(b) > vkr_gpu_draw_census_rank(a) ? b : a;
}

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
/* Faces drawn per frame into a dynamic square: a copy of the face's static
 * square, which holds the static casters, plus the dynamic casters. Each
 * takes a render slot. The squares fill a band of trailing atlas layers that
 * holds no static square; this many faces of the largest size fill it. */
#define VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX 16u
#define VKR_LOCAL_SHADOW_DYNAMIC_LAYER_COUNT 1u
/* Baked lamps a frame shadows with its moving casters (ADR-104): those
 * whose light at the moving casters the camera may see scores highest. The
 * desktop pipeline shadows up to this many; the tiled pipeline's lightmap
 * record holds VKR_LOCAL_SHADOW_TILED_BAKED_LAMP_COUNT_MAX. Their static
 * squares fill atlas layers of their own, before the dynamic band, while the
 * scene has baked lamps and moving casters: as many layers as `lamps` lamps
 * of the largest size fill. */
#define VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX 4u
#define VKR_LOCAL_SHADOW_TILED_BAKED_LAMP_COUNT_MAX 2u
#define VKR_LOCAL_SHADOW_BAKED_LAYER_COUNT(lamps)                              \
  (((lamps) * 6u * VKR_LOCAL_SHADOW_MAP_SIZE_MAX *                             \
        VKR_LOCAL_SHADOW_MAP_SIZE_MAX +                                        \
    VKR_LOCAL_SHADOW_ATLAS_SIZE * VKR_LOCAL_SHADOW_ATLAS_SIZE - 1u) /          \
   (VKR_LOCAL_SHADOW_ATLAS_SIZE * VKR_LOCAL_SHADOW_ATLAS_SIZE))
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

_Static_assert(VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX <= 64u,
               "local shadow render slot and transmission layer masks are "
               "64-bit");
_Static_assert(
    (uint64_t)VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX
            *VKR_LOCAL_SHADOW_MAP_SIZE_MAX *VKR_LOCAL_SHADOW_MAP_SIZE_MAX <=
        (uint64_t)VKR_LOCAL_SHADOW_DYNAMIC_LAYER_COUNT *
            VKR_LOCAL_SHADOW_ATLAS_SIZE * VKR_LOCAL_SHADOW_ATLAS_SIZE,
    "the dynamic band holds the largest dynamic squares");
_Static_assert(VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX <
                   VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX,
               "dynamic squares leave render slots for static squares");
_Static_assert(VKR_LOCAL_SHADOW_TILED_BAKED_LAMP_COUNT_MAX <=
                   VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX,
               "the payload holds the tiled pipeline's baked lamps");
_Static_assert(VKR_LOCAL_SHADOW_BAKED_LAYER_COUNT(
                   VKR_LOCAL_SHADOW_TILED_BAKED_LAMP_COUNT_MAX) == 1u &&
                   VKR_LOCAL_SHADOW_BAKED_LAYER_COUNT(
                       VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX) == 2u,
               "the tiled pipeline's baked lamps fill one layer and the "
               "desktop pipeline's two");

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
   * zero. w: the light's source radius over 2 tan(half FOV), which
   * Shadow.LocalMask scales by blocker and receiver distances for
   * contact-hardening; zero keeps the fixed filter radius. */
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

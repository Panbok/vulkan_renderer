#include "metal/vkr_metal_packet_renderer.h"
#include "vkr_packed_geometry.h"
#include "vkr_pipeline_registry.h"
#include "vkr_texture_upload.h"

#if defined(PLATFORM_APPLE)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <simd/simd.h>

#include <Block.h>
#include <dispatch/dispatch.h>

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_zone.h"
#include "math/vkr_frustum.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "memory/vkr_heap.h"
#include "metal/internal/vkr_metal_packet_waits.h"
#include "metal/vkr_metal_dependency.h"
#include "metal/vkr_metal_diagnostics.h"
#include "metal/vkr_metal_packet_abi.h"
#include "vkr_anisotropy_lut.h"
#include "vkr_candidate_residency.h"
#include "vkr_capture_ring.h"
#include "vkr_dfg_lut.h"
#include "vkr_geometry_ranges.h"
#include "vkr_geometry_upload.h"
#include "vkr_ibl_math.h"
#include "vkr_ibl_sh_pool.h"
#include "vkr_ltc_lut.h"
#include "vkr_packet_constants.h"
#include "vkr_render_graph_frame.h"
#include "vkr_render_graph_internal.h"
#include "vkr_renderer_metrics.h"
#include "vkr_rg_json.h"
#include "vkr_sheen_lut.h"
#include "vkr_visibility.h"

enum {
  VKR_METAL_PACKET_TIMEOUT_MS = 5000,
  VKR_METAL_PACKET_MAX_COLOR_ATTACHMENTS = 8,
  VKR_METAL_PACKET_MAX_TEXTURE_MIPS = 15,
  /* The local shadow atlas, whose layers hold the faces' squares, is the
     largest layered graph image. Every graph image instance sizes its
     per-layer views and retained states by this bound. */
  VKR_METAL_PACKET_MAX_TEXTURE_LAYERS = VKR_LOCAL_SHADOW_ATLAS_LAYER_COUNT_MAX,
  VKR_METAL_PACKET_GRAPH_INSTANCE_MAX = 8,
  VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX =
      VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX,
  VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY = 16,
  /* Presents awaiting display feedback; the display reports within a few
     refresh intervals, so eight cover the editor presenter's rate. */
  VKR_METAL_PACKET_PRESENT_MARKS = 8,
  /* Must match the shared atmosphere kernel's workgroup reduction width. */
  VKR_METAL_PACKET_ATMOSPHERE_MULTI_DIRECTIONS = 64,
  /* Cloud base volume, detail volume and weather map (ADR-074). */
  VKR_METAL_PACKET_CLOUD_NOISE_COUNT = 3,
  /* Queued environment and probe bakes awaiting the frame's IBL.Bake pass. */
  VKR_METAL_PACKET_PENDING_IBL_BAKE_MAX = 32,
  /* Recent submissions whose pre-exposure a history may still be scaled
     from; histories come from frames a few submissions back at most. */
  VKR_METAL_PACKET_PRE_EXPOSURE_HISTORY = 32,
  /* Transmittance, multiple-scattering and source dispatches. */
  VKR_METAL_PACKET_ATMOSPHERE_DISPATCH_COUNT = 3,
  /* Four address modes on three axes, two min/mag filters, three mip
     filters and anisotropy on/off. Samplers remain alive with immutable
     material rows, so this covers the complete key domain rather than only
     simultaneous textures. */
  VKR_METAL_PACKET_SAMPLER_CACHE_CAPACITY =
      VKR_TEXTURE_REPEAT_MODE_COUNT * VKR_TEXTURE_REPEAT_MODE_COUNT *
      VKR_TEXTURE_REPEAT_MODE_COUNT * VKR_FILTER_COUNT * VKR_FILTER_COUNT *
      VKR_MIP_FILTER_COUNT * 2,
  /* Argument-buffer samplers one stage can reach on Apple7 and Apple8 GPUs
     (M1, M2; Apple9 raises it to 500,000). Inline constexpr samplers in MSL
     count toward it, so a reserve stays free for them (ADR-083). */
  VKR_METAL_PACKET_APPLE7_ARGUMENT_SAMPLER_LIMIT = 996,
  VKR_METAL_PACKET_CONSTEXPR_SAMPLER_RESERVE = 64,
  /* Editor Scene resolve has already applied the scene display transfer; its
   * composite only samples those encoded pixels. */
  VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED = 1u << 3u,
};

/**
 * Metal requires a serial feedback queue. Keeping one process-lifetime queue
 * avoids ambiguous per-renderer dispatch ownership and lets a trailing block
 * prove that each callback returned before its commit options are reused.
 */
vkr_internal dispatch_queue_t vkr_metal_packet_feedback_queue(void) {
  vkr_local_persist dispatch_once_t once;
  vkr_local_persist dispatch_queue_t queue;
  dispatch_once(&once, ^{
    queue = dispatch_queue_create("com.vkr.metal.commit-feedback",
                                  DISPATCH_QUEUE_SERIAL);
  });
  return queue;
}

_Static_assert(VKR_TEXTURE_MAX_DIMENSION ==
                   (1u << (VKR_METAL_PACKET_MAX_TEXTURE_MIPS - 1u)),
               "Metal sampler mip-domain bound must match texture limits");
_Static_assert(VKR_LOCAL_SHADOW_ATLAS_LAYER_COUNT_MAX <=
                       VKR_METAL_PACKET_MAX_TEXTURE_LAYERS &&
                   VKR_SHADOW_CASCADE_COUNT_MAX <=
                       VKR_METAL_PACKET_MAX_TEXTURE_LAYERS,
               "a layered shadow graph image exceeds the graph layers");

/** Converts the renderer's Vulkan-oriented clip-Y convention to Metal. */
vkr_internal Mat4 vkr_metal_packet_clip_matrix(bool8_t convert, Mat4 matrix) {
  if (!convert)
    return matrix;
  matrix.elements[1] = -matrix.elements[1];
  matrix.elements[5] = -matrix.elements[5];
  matrix.elements[9] = -matrix.elements[9];
  matrix.elements[13] = -matrix.elements[13];
  return matrix;
}

typedef struct VkrMetalPacketPassLabel {
  NSString *value;
  char name[VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY];
  uint32_t length;
} VkrMetalPacketPassLabel;

typedef struct VkrMetalPacketImageViews {
  void *mip_views[VKR_METAL_PACKET_MAX_TEXTURE_MIPS];
  void *layer_views[VKR_METAL_PACKET_MAX_TEXTURE_LAYERS];
} VkrMetalPacketImageViews;

/* Cloud radiance history (ADR-074). `view_projection` is the producer's
   canonical unjittered matrix, which reprojects the next trace. */
typedef struct VkrMetalPacketCloudHistoryState {
  Mat4 view_projection;
  uint64_t signature;
  uint64_t producer_submit_value;
  uint64_t frame_index;
  uint32_t width;
  uint32_t height;
  uint32_t graph_generation;
  bool8_t valid;
} VkrMetalPacketCloudHistoryState;

typedef struct VkrMetalPacketImageInstance {
  VkrMetalTextureResource resource;
  VkrMetalPacketImageViews views;
  uint64_t last_use_submit_value;
  uint64_t history_producer_submit_value;
  uint64_t history_world_epoch;
  Mat4 history_view_projection;
  Mat4 history_raster_view_projection;
  uint32_t history_width;
  uint32_t history_height;
  bool8_t history_valid;
  bool8_t live;
  bool8_t owned;
  /**
   * Committed cross-frame state for a RETAINED resource (ADR-029), indexed
   * mip * layer_count + layer to match the graph's subresource ordering.
   *
   * Sized to this backend's realized image bounds. Vulkan has its own larger
   * fixed bound; retained tracking never needs slots Metal cannot realize.
   */
  VkrRgRetainedState retained_states[VKR_METAL_PACKET_MAX_TEXTURE_MIPS *
                                     VKR_METAL_PACKET_MAX_TEXTURE_LAYERS];
} VkrMetalPacketImageInstance;

typedef struct VkrMetalPacketImage {
  VkrMetalPacketImageInstance instances[VKR_METAL_PACKET_GRAPH_INSTANCE_MAX];
  VkrRgImageDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
  bool8_t external;
} VkrMetalPacketImage;

typedef struct VkrMetalPacketRetiredImageViews {
  VkrMetalPacketImageViews views;
  uint64_t last_use_submit_value;
  bool8_t live;
} VkrMetalPacketRetiredImageViews;

typedef struct VkrMetalPacketGraphBufferInstance {
  VkrMetalBufferResource resource;
  uint64_t last_use_submit_value;
  uint64_t history_producer_submit_value;
  uint64_t history_scene_generation;
  float64_t history_exposure_seconds;
  bool8_t history_valid;
  bool8_t live;
} VkrMetalPacketGraphBufferInstance;

typedef struct VkrMetalPacketGraphBuffer {
  VkrRgBufferDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
} VkrMetalPacketGraphBuffer;

/* SDF sun shadows (the shadow quality proposal's T5): one submesh's
   distance volume, generated on the GPU from its resident geometry once a
   frame's candidates reach it (vkr_metal_packet_sdf.inc). */
typedef enum VkrMetalSdfVolumeState {
  VKR_METAL_SDF_VOLUME_EMPTY = 0,
  VKR_METAL_SDF_VOLUME_READY,
  /* Too small to cast a far shadow, or its texture could not be created. */
  VKR_METAL_SDF_VOLUME_SKIPPED,
} VkrMetalSdfVolumeState;

typedef struct VkrMetalSdfVolume {
  /* R8Unorm 3D: unsigned distance to the nearest triangle over the band,
     in mesh-local voxels of `voxel`, over `dims` voxels from `box_min`.
     Before generation `box_min` and `box_size` are the submesh's quantized
     position box (its decode record). */
  VkrMetalTextureResource texture;
  Vec3 box_min;
  Vec3 box_size;
  float32_t voxel;
  uint32_t dims[3];
  uint32_t state;
  /* Its row in the field table while the table's layout is current. */
  uint32_t row;
} VkrMetalSdfVolume;

/* Distance volumes, as the mesh cooker writes them (vkr_geometry_upload.h):
   at most this many voxels a side, with a border of VKR_METAL_SDF_PAD_VOXELS,
   voxels no smaller than VKR_METAL_SDF_MIN_WORLD_VOXEL world metres (half
   the far level's texel), distances banded at VKR_METAL_SDF_BAND_VOXELS.
   Casters under VKR_METAL_SDF_MIN_EXTENT world metres cast no far shadow
   worth a volume; casters over VKR_METAL_SDF_MAX_EXTENT, such as terrain,
   keep the cascades. */
#define VKR_METAL_SDF_MAX_DIM VKR_GEOMETRY_DISTANCE_MAX_DIM
#define VKR_METAL_SDF_PAD_VOXELS VKR_GEOMETRY_DISTANCE_PAD_VOXELS
#define VKR_METAL_SDF_MIN_WORLD_VOXEL VKR_GEOMETRY_DISTANCE_MIN_VOXEL
#define VKR_METAL_SDF_BAND_VOXELS VKR_GEOMETRY_DISTANCE_BAND_VOXELS
#define VKR_METAL_SDF_MIN_EXTENT VKR_GEOMETRY_DISTANCE_MIN_EXTENT
#define VKR_METAL_SDF_MAX_EXTENT VKR_GEOMETRY_DISTANCE_MAX_EXTENT
/* The light-space grid: cells a side over the receivers the shadows reach
   around the camera, at least the minimum half extent in metres, and its
   capacity of cell entries. */
#define VKR_METAL_SDF_GRID_SIZE 256u
#define VKR_METAL_SDF_GRID_MIN_HALF_EXTENT 64.0f
#define VKR_METAL_SDF_GRID_ENTRIES 1048576u
/* Candidates a frame looks through for volumes to generate, round robin. */
#define VKR_METAL_SDF_SCAN_CANDIDATES 8192u
/* The grid scan's threadgroup and the cells each block of it scans (mirrors
   world/sun_shadow.metal); the blocks' sums follow the cells' cursors. */
#define VKR_METAL_SDF_SCAN_THREADS 256u
#define VKR_METAL_SDF_SCAN_BLOCK 1024u
#define VKR_METAL_SDF_GRID_CELLS                                               \
  (VKR_METAL_SDF_GRID_SIZE * VKR_METAL_SDF_GRID_SIZE)
#define VKR_METAL_SDF_SCAN_BLOCKS                                              \
  (VKR_METAL_SDF_GRID_CELLS / VKR_METAL_SDF_SCAN_BLOCK)
_Static_assert(VKR_METAL_SDF_GRID_CELLS % VKR_METAL_SDF_SCAN_BLOCK == 0u,
               "the grid scan takes whole blocks");
/* The grid buffer: offsets, entries, cursors and block sums. */
#define VKR_METAL_SDF_GRID_BYTES                                               \
  (((uint64_t)2u * VKR_METAL_SDF_GRID_CELLS + 1u +                             \
    VKR_METAL_SDF_GRID_ENTRIES + VKR_METAL_SDF_SCAN_BLOCKS) *                  \
   sizeof(uint32_t))
/* How far toward the sun the trace looks, and the smallest sun half angle
   its penumbrae take (half a degree). */
#define VKR_METAL_SDF_TRACE_DISTANCE 300.0f
#define VKR_METAL_SDF_MIN_SUN_TAN 0.00873f
/* The share of the far level's start over which the trace takes over. */
#define VKR_METAL_SUN_SHADOW_BAND_SHARE 0.1f

typedef struct VkrMetalPacketMesh {
  VkrMetalBufferResource vertices;
  VkrMetalBufferResource indices;
  VkrGpuGeometryRow gpu_row;
  VkrGeometryRangeAllocation ranges;
  VkrMetalPacketSubmeshCreateInfo *submeshes;
  /* One per submesh once a frame first traces SDF sun shadows; NULL
     before. */
  VkrMetalSdfVolume *sdf_volumes;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t submesh_count;
  uint32_t decode_count;
  uint32_t generation;
  uint64_t last_use_submit_value;
  bool8_t live;
} VkrMetalPacketMesh;

typedef struct VkrMetalPacketGeometryMegabuffer {
  VkrMetalBufferResource vertices;
  VkrMetalBufferResource indices;
  VkrGeometryMegabufferAccounting accounting;
  uint32_t generation;
  bool8_t live;
} VkrMetalPacketGeometryMegabuffer;

typedef struct VkrMetalPacketMaterial {
  VkrMetalTextureResource textures[12];
  VkrTextureHandle texture_handles[VKR_METAL_PACKET_MATERIAL_TEXTURE_COUNT];
  VkrMetalMaterialHandle row;
  VkrPbrProperties pbr;
  VkrMaterialAlphaMode alpha_mode;
  float32_t alpha_cutoff;
  uint32_t row_index;
  uint32_t generation;
  uint64_t last_use_submit_value;
  /* The Custom graph slot whose reference this row holds, or 0 (ADR-096). */
  uint32_t custom_slot;
  bool8_t owns_textures;
  bool8_t double_sided;
  bool8_t live;
} VkrMetalPacketMaterial;

typedef struct VkrMetalPacketSamplerKey {
  VkrTextureRepeatMode u_repeat_mode;
  VkrTextureRepeatMode v_repeat_mode;
  VkrTextureRepeatMode w_repeat_mode;
  VkrFilter min_filter;
  VkrFilter mag_filter;
  VkrMipFilter mip_filter;
  bool8_t anisotropy_enable;
} VkrMetalPacketSamplerKey;

typedef struct VkrMetalPacketSampler {
  VkrMetalPacketSamplerKey key;
  id<MTLSamplerState> state;
} VkrMetalPacketSampler;

typedef struct VkrMetalPacketTexture {
  VkrMetalTextureResource resource;
  uint64_t sampler_resource_id;
  uint32_t generation;
  uint64_t last_use_submit_value;
  /** Frame-queue value of the submission that uploaded its contents. The
      presenter waits for it before sampling (ADR-111). */
  uint64_t upload_submit_value;
  /** Published L2 coefficient slot projected from this source cubemap, or
      VKR_SH_SLOT_BLACK before the first successful projection (ADR-038). */
  uint32_t ibl_sh_slot;
  /** The deringing that projection used; the cloud-lit sky light projects
      with the same window (ADR-074). */
  float32_t ibl_sh_deringing;
  /** GGX prefilter the latest queued bake writes from this source cubemap.
      Global lighting samples it; invalid until a bake is queued. */
  VkrTextureHandle ibl_prefilter;
  /** Exponent of the radiance scale this texture's contents were written
      at: a pre-exposed atmosphere bake's, zero for physical uploads. */
  int32_t radiance_stops;
  uint64_t atmosphere_completion_submit_value;
  VkrAtmosphereBakeStatus atmosphere_status;
  bool8_t atmosphere_bake_active;
  bool8_t atmosphere_ibl_failed;
  bool8_t live;
} VkrMetalPacketTexture;

vkr_internal VkrMetalPacketTexture *
vkr_metal_packet_resolve_texture(VkrMetalPacketRenderer *renderer,
                                 VkrTextureHandle handle);
vkr_internal VkrMetalPacketMesh *
vkr_metal_packet_resolve_mesh(VkrMetalPacketRenderer *renderer,
                              VkrMeshHandle handle);
vkr_internal VkrMetalPacketMaterial *
vkr_metal_packet_resolve_material(VkrMetalPacketRenderer *renderer,
                                  VkrMaterialHandle handle);

typedef struct VkrMetalPacketTextUpload {
  uint64_t vertices_gpu;
  uint64_t indices_gpu;
  uint64_t indices_length;
} VkrMetalPacketTextUpload;

typedef struct VkrMetalPacketCandidateCopyRange {
  uint64_t candidate_source_offset;
  uint64_t instance_source_offset;
  uint32_t destination_first;
  uint32_t count;
} VkrMetalPacketCandidateCopyRange;

/* CPU-only rows borrowed from the frame upload slice until submit returns. */
typedef struct VkrMetalPacketPreparedDraw {
  VkrGpuGeometryRow geometry;
  VkrGpuVisibleDrawRow visible;
  uint64_t index_address;
  uint64_t index_length;
  uint32_t instance_count;
  MTLCullMode cull_mode;
  MTLWinding winding;
  /* The draw's Standard material uses a BRDF extension (ADR-113): a pass
     with an extension pipeline draws it with that one. */
  bool8_t brdf_extended;
} VkrMetalPacketPreparedDraw;

typedef struct VkrMetalPacketFrameUpload {
  VkrMetalPacketSkinningRoot skinning_roots[VKR_SKINNING_BINDING_CAPACITY];
  VkrMetalRingSlice slice;
  VkrMetalAddressPair addresses;
  id<MTLBuffer> buffer;
  uint8_t *root_cpu;
  uint64_t root_gpu;
  uint64_t world_instances_gpu;
  uint64_t ui_vertices_gpu;
  uint64_t ui_indices_gpu;
  uint64_t ui_indices_length;
  uint64_t gpu_draw_instances_gpu;
  uint64_t gpu_draw_geometry_rows_gpu;
  uint64_t gpu_draw_views_gpu;
  uint64_t gpu_draw_lod_views_gpu;
  uint64_t gpu_draw_icb_argument_gpu;
  uint64_t gpu_draw_icb_argument_stride;
  VkrMetalPacketCandidateCopyRange gpu_draw_candidate_copies[2];
  uint32_t gpu_draw_candidate_copy_count;
  uint64_t gpu_draw_state_zero_source_offset;
  uint64_t point_light_data_gpu;
  uint64_t point_light_masks_gpu;
  uint64_t shadow_cascades_gpu;
  uint64_t shadow_texture_id;
  uint64_t local_shadow_texture_id;
  uint64_t local_shadow_views_gpu;
  /* The tiled pipeline's refraction source, on frames that declare it. */
  uint64_t transmission_texture_id;
  uint64_t ibl_probes_gpu;
  uint64_t diffuse_volume_params_gpu;
  /** The diffuse-volume composition this frame records, committed to the
      renderer once the frame is submitted. */
  bool8_t diffuse_volume_composed;
  VkrTextureHandle diffuse_volume_compose_sh;
  uint32_t diffuse_volume_compose_revision;
  bool8_t diffuse_volume_compose_direct;
  /* This frame's VkrMetalPacketLightmap, written for every frame. */
  uint64_t lightmap_gpu;
  /* This frame's VkrMetalPacketDecals; zero without decals (ADR-092). */
  uint64_t decals_gpu;
  /* This frame's texture demand words in its readback slice, which the
     camera views' frame roots carry (ADR-024); zero without them. */
  uint64_t texture_demand_gpu;
  uint64_t ltc_gpu;
  uint64_t sheen_gpu;
  uint64_t fog_gpu;
  uint64_t sky_gpu;
  /* SDF sun shadows: this frame's copy of the field table, zero without
     them (vkr_metal_packet_sdf.inc). */
  uint64_t sdf_volume_rows_gpu;
  uint64_t sdf_mesh_volumes_gpu;
  uint32_t sdf_mesh_count;
  /** CPU view of the frame's sky record; preparation patches it before the
      submission reads it. */
  VkrMetalPacketSky *sky_cpu;
  /** This frame's cloud-lit sky light (ADR-074): its cloud chain, and
      the SH slot later frame roots read as the global sky light once its
      pass is prepared; VKR_SH_SLOT_BLACK keeps the published source's. */
  uint64_t cloud_sky_light_radiance_gpu;
  uint32_t cloud_sh_slot;
  uint64_t display_output_gpu;
  /** True once IBL.Bake recorded a queued bake; submission publishes it and
      cancellation returns its SH candidate to the pool. */
  bool8_t ibl_bake_recorded;
  VkrMetalPacketTextUpload *text_uploads;
  VkrMetalPacketPreparedDraw *direct_draws;
  VkrMetalPacketPreparedDraw overlay_draws[VKR_EDITOR_OVERLAY_DRAW_MAX];
  uint64_t overlay_root_gpu;
  uint32_t overlay_draw_count;
  /* Selection mask draws live in the upload slice; their roots follow the
   * overlay roots. */
  VkrMetalPacketPreparedDraw *selection_draws;
  uint64_t selection_root_gpu;
  uint32_t selection_draw_count;
  uint32_t direct_draw_count;
  uint32_t world_text_count;
  uint32_t root_capacity;
  uint32_t root_cursor;
  bool8_t acquired;
} VkrMetalPacketFrameUpload;

/* A queued environment or probe bake. The publisher validates and queues it;
   the next frame's IBL.Bake pass records it into the frame command buffer, and
   that frame's submission publishes its SH candidate. Roots and the candidate
   slot are valid only while recorded. */
typedef struct VkrMetalPacketPendingIblBake {
  VkrAtmosphereGpuParams params;
  VkrTextureHandle source;
  VkrTextureHandle prefilter;
  VkrTextureHandle transmittance;
  VkrTextureHandle multiple_scattering;
  uint64_t atmosphere_roots[VKR_METAL_PACKET_ATMOSPHERE_DISPATCH_COUNT];
  uint64_t prefilter_roots[VKR_METAL_PACKET_MAX_TEXTURE_MIPS];
  uint64_t sh_root;
  float32_t sh_deringing;
  uint32_t sh_slot;
  uint32_t prefilter_size;
  uint32_t prefilter_mip_count;
  bool8_t is_atmosphere;
  bool8_t recorded;
} VkrMetalPacketPendingIblBake;

/* The pre-exposure exponent one submitted frame stored its radiance at. */
typedef struct VkrMetalPacketPreExposureRecord {
  uint64_t submit_value;
  int32_t stops;
} VkrMetalPacketPreExposureRecord;

typedef struct VkrMetalPacketCapturePlan {
  id<MTLTexture> texture;
  VkrCaptureItemResult result;
  uint64_t buffer_offset;
  uint32_t source_slice;
} VkrMetalPacketCapturePlan;

typedef struct VkrMetalPacketReadbackLayout {
  uint64_t shadow_depth;
  uint64_t picking;
  uint64_t picking_depth;
  uint64_t custom_late;
  uint64_t deferred_diagnostics;
  uint64_t deferred_diagnostics_size;
  uint64_t exposure;
  uint64_t exposure_size;
  /* The draw census's packed states and object ids, sized for
     `census_candidates` candidates; zero without a census. */
  uint64_t census_states;
  uint64_t census_ids;
  uint32_t census_candidates;
  /* The texture demand words, one per material table row, which the
     camera's forward shading maxes into (ADR-024); zero words without a
     world. */
  uint64_t texture_demand;
  uint32_t texture_demand_words;
  uint64_t prefix_size;
} VkrMetalPacketReadbackLayout;

vkr_internal VkrMetalPacketReadbackLayout vkr_metal_packet_readback_layout(
    bool8_t deferred_diagnostics, bool8_t exposure,
    uint32_t gpu_draw_view_count, uint32_t census_candidates,
    uint32_t texture_demand_words) {
  /* The final target can be RGBA8 or RGBA16F. Reserve an eight-byte pixel
     before the fixed diagnostic fields so the transfer stays valid across an
     extended-linear target transition. */
  const uint64_t shadow_depth_offset = 8u;
  const uint64_t picking_offset = shadow_depth_offset + sizeof(float32_t);
  /* The picking readback copies the picked pixel's resolved depth right
     after the object id (vkr_metal_packet_prepare_picking_readback). */
  const uint64_t picking_depth_offset = picking_offset + sizeof(uint32_t);
  /* The camera's late Custom draws (ADR-096). */
  const uint64_t custom_late_offset = picking_depth_offset + sizeof(float32_t);
  const uint64_t fixed_size = 32u;
  const uint64_t deferred_offset = vkr_align_up_u64(fixed_size, 16u);
  const uint64_t deferred_bytes =
      deferred_diagnostics
          ? (uint64_t)gpu_draw_view_count * sizeof(VkrGpuDrawCompactionState)
          : 0u;
  const uint64_t exposure_offset =
      vkr_align_up_u64(deferred_offset + deferred_bytes, 16u);
  const uint64_t exposure_bytes =
      exposure ? sizeof(VkrExposureGpuState) + sizeof(VkrExposureGpuHistogram)
               : 0u;
  const uint64_t diagnostics_end =
      exposure_bytes > 0u    ? exposure_offset + exposure_bytes
      : deferred_diagnostics ? deferred_offset + deferred_bytes
                             : fixed_size;
  const uint64_t census_states_offset = vkr_align_up_u64(diagnostics_end, 16u);
  const uint64_t census_ids_offset = vkr_align_up_u64(
      census_states_offset +
          (uint64_t)vkr_gpu_draw_census_word_count(census_candidates) *
              sizeof(uint32_t),
      16u);
  const uint64_t census_end =
      census_candidates > 0u
          ? census_ids_offset + (uint64_t)census_candidates * sizeof(uint32_t)
          : diagnostics_end;
  const uint64_t texture_demand_offset = vkr_align_up_u64(census_end, 16u);
  const uint64_t prefix_size =
      texture_demand_words > 0u
          ? texture_demand_offset +
                (uint64_t)texture_demand_words * sizeof(uint32_t)
          : census_end;
  return (VkrMetalPacketReadbackLayout){
      .shadow_depth = shadow_depth_offset,
      .picking = picking_offset,
      .picking_depth = picking_depth_offset,
      .custom_late = custom_late_offset,
      .deferred_diagnostics = deferred_offset,
      .deferred_diagnostics_size = deferred_bytes,
      .exposure = exposure_offset,
      .exposure_size = exposure_bytes,
      .census_states = census_states_offset,
      .census_ids = census_ids_offset,
      .census_candidates = census_candidates,
      .texture_demand = texture_demand_offset,
      .texture_demand_words = texture_demand_words,
      .prefix_size = prefix_size,
  };
}

/**
 * Display feedback of window presents. Each present records its drawable's ID
 * and the CPU time of the call in the next mark; the presented handler, called
 * on a Core Animation thread, finds the mark and adds the time to the display,
 * or a miss, to the totals the metrics collector takes. A drawable still on
 * its way to the display holds the handler after the renderer is gone, so the
 * state is its own allocation: the renderer and each pending present hold a
 * reference, and the last one frees it.
 */
typedef struct VkrMetalPresentFeedback {
  VkrAtomicUint32 references;
  struct {
    VkrAtomicUint64 drawable_id;
    VkrAtomicUint64 present_time_ns;
  } marks[VKR_METAL_PACKET_PRESENT_MARKS];
  VkrAtomicUint64 latency_ns;
  VkrAtomicUint32 latency_count;
  VkrAtomicUint32 missed_count;
} VkrMetalPresentFeedback;

vkr_internal void
vkr_metal_present_feedback_release(VkrMetalPresentFeedback *feedback) {
  if (feedback && vkr_atomic_uint32_fetch_sub(&feedback->references, 1u,
                                              VKR_MEMORY_ORDER_ACQ_REL) == 1u)
    vkr_heap_free(feedback);
}

/**
 * Commit feedback outlives the command slot that produced it. The render
 * thread owns acquisition and recycling; Metal's serial callback queue only
 * writes the feedback half and publishes it with `feedback_ready`.
 */
typedef struct VkrMetalPacketCommitFeedbackRecord {
  MTL4CommitOptions *options;
  MTL4CommitFeedbackHandler handler;
  dispatch_block_t ready_handler;
  VkrMetalPacketResult result;
  CFTimeInterval gpu_start_time;
  CFTimeInterval gpu_end_time;
  /* The submission's error, retained by the callback and released by the
     render thread once it is logged. */
  NSError *error;
  bool8_t has_error;
  /* Every commit carries feedback so GPU errors are seen; only a commit that
     asked for submission timing publishes its duration. */
  bool8_t timing_requested;
  VkrAtomicBool feedback_ready;
  VkrAtomicBool result_ready;
  bool8_t in_use;
} VkrMetalPacketCommitFeedbackRecord;

/* Distance volumes one frame generates at most, and the voxels of the
   command-slot scratch they share, four of the largest. */
#define VKR_METAL_SDF_GENERATIONS_PER_FRAME 32u
#define VKR_METAL_SDF_SCRATCH_VOXELS                                           \
  (4u * VKR_METAL_SDF_MAX_DIM * VKR_METAL_SDF_MAX_DIM * VKR_METAL_SDF_MAX_DIM)

/* An ICB group's command pool holds at least this many views of every
   candidate before the frames' visible draws ask for more (ADR-024). */
#define VKR_METAL_PACKET_GPU_DRAW_POOL_FLOOR_VIEWS 4u
/* The pool grows once a frame needs this share of its target, to twice
   that need in whole steps. */
#define VKR_METAL_PACKET_GPU_DRAW_POOL_HEADROOM_NUM 3u
#define VKR_METAL_PACKET_GPU_DRAW_POOL_HEADROOM_DEN 2u
#define VKR_METAL_PACKET_GPU_DRAW_POOL_STEP 32768u

/* VkrMetalPacketCommandSlot.gpu_draw_view_targets: the kind above the
   cascade or local atlas layer in the low byte. */
enum {
  VKR_METAL_PACKET_VIEW_TARGET_NONE = 0u,
  VKR_METAL_PACKET_VIEW_TARGET_CASCADE = 0x100u,
  VKR_METAL_PACKET_VIEW_TARGET_LOCAL = 0x200u,
  VKR_METAL_PACKET_VIEW_TARGET_LAYER_MASK = 0xFFu,
};

typedef struct VkrMetalPacketCommandSlot {
  /* The frame's count of late Custom draws in the readback slice, which
     the result reads once the frame completes (ADR-096); NULL without
     Custom graphs. */
  const uint32_t *custom_late_readback;
  /* Atomic distance scratch of the distance volumes this slot's frame
     generates, VKR_METAL_SDF_SCRATCH_VOXELS; created the first time one
     does. */
  VkrMetalBufferResource sdf_scratch;
  id<MTL4CommandAllocator> allocator;
  id<MTL4CommandBuffer> buffer;
  id<MTL4CounterHeap> timestamp_heap;
  id<MTLResidencySet>
      gpu_draw_icb_residencies[VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX];
  id<MTLIndirectCommandBuffer>
      gpu_draw_icbs[VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX];
  /* Native command capacities; retained until this completed slot grows. */
  uint32_t
      gpu_draw_icb_capacities[VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX];
  VkrMetalPacketResult pending_result;
  VkrMetalPacketCommitFeedbackRecord *commit_feedback;
  uint64_t submit_value;
  uint64_t resource_reuse_submit_value;
  uint32_t timestamp_entry_count;
  bool8_t pass_timing_requested;
  bool8_t result_pending;
  bool8_t result_collected;
  const uint8_t *picking_readback;
  const uint8_t *gpu_draw_diagnostics_readback;
  const uint8_t *exposure_readback;
  /* The frame's draw census in the readback slice, NULL without one: its
     packed states, object ids, candidates and VkrDrawCensusFlag bits. */
  const uint32_t *census_states_readback;
  const uint32_t *census_ids_readback;
  uint32_t census_count;
  uint32_t census_flags;
  /* The frame's texture demand words in the readback slice, one per
     material table row, which its camera views' forward shading maxed into;
     NULL when no view recorded them. */
  const uint32_t *texture_demand_readback;
  uint32_t shadow_cascade_count;
  /** Cascades whose dynamic-caster view drew over a copied layer. */
  uint32_t shadow_cascade_dynamic_render_mask;
  uint32_t gpu_draw_view_count;
  /** The slot's late occlusion view, or zero for a one-phase frame
      (ADR-032). */
  uint32_t gpu_draw_late_view;
  /** What each culling view of the slot's frame drew into, for repair when
      the command pool cut its draws: VKR_METAL_PACKET_VIEW_TARGET_* above
      the cascade or the local atlas layer. */
  uint16_t gpu_draw_view_targets[VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX];
  bool8_t gpu_draw_diagnostics_requested;
  bool8_t exposure_requested;
  uint32_t gpu_draw_icb_residency_count;
  VkrCandidateResidencyState candidate_residency;
  VkrCandidateResidencyState pending_candidate_residency;
  bool8_t candidate_residency_pending;
} VkrMetalPacketCommandSlot;

/* Command slots uploads rotate through, one per upload ring slot. */
#define VKR_METAL_PACKET_UPLOAD_SLOT_COUNT_MAX 4u
#define VKR_METAL_PACKET_TEXTURE_UPLOAD_BATCH_MAX 64u
#define VKR_METAL_PACKET_TEXTURE_UPLOAD_BATCH_BYTES MB(32)
/* Texture uploads larger than this go in submissions of whole regions of at
   most this size (vkr_metal_packet_upload_texture). */
#define VKR_METAL_PACKET_TEXTURE_UPLOAD_CHUNK_BYTES MB(64)
typedef struct VkrMetalPacketTextureUploadBatch {
  id<MTL4ComputeCommandEncoder> encoder;
  id<MTLBuffer> upload_buffer;
  VkrMetalRingSlice slice;
  VkrMetalAddressPair upload;
  uint64_t capacity;
  uint64_t used;
  uint32_t texture_count;
  /* Buffer copies, such as geometry, the batch also carries. */
  uint32_t buffer_count;
  bool8_t enabled;
  bool8_t active;
} VkrMetalPacketTextureUploadBatch;

/* Samples of the tiled pipeline's opaque render pass (ADR-087): four cost
   0.3 to 0.5 ms over one on the M1 Pro. */
/* The tiled pipeline's sample count under the spatial upscale; MetalFX's
   temporal upscale draws one sample (renderer->tiled_sample_count). */
#define VKR_METAL_TILED_SAMPLE_COUNT 4u
/* The opaque pass's tile, which its resolve kernel covers with one thread
   per pixel; Metal's default for its four-sample targets on the M1 Pro. */
#define VKR_METAL_TILED_TILE_WIDTH 32u
#define VKR_METAL_TILED_TILE_HEIGHT 16u

/* The tiled pipeline's shading variants by the dynamic lights a frame has
   (ADR-087), each without the light code its frames do not need; mirrors
   VkrMetalTiledLighting in tiled.metal. */
typedef enum VkrMetalTiledLighting {
  VKR_METAL_TILED_LIGHTING_NONE = 0,
  /* Point and spot lights, none of them shadowed. */
  VKR_METAL_TILED_LIGHTING_PUNCTUAL = 1,
  /* Point and spot lights with their local shadows. */
  VKR_METAL_TILED_LIGHTING_SHADOWED = 2,
  /* Rectangle lights besides shadowed point and spot lights. */
  VKR_METAL_TILED_LIGHTING_ALL = 3,
  /* Every light, and the editor's inspection modes. */
  VKR_METAL_TILED_LIGHTING_INSPECT = 4,
  /* Moving casters' shadows of the baked lamps besides shadowed point and
     spot lights (ADR-108). */
  VKR_METAL_TILED_LIGHTING_CASTERS = 5,
  /* Moving casters' shadows besides every dynamic light, rectangle lights
     included; their code's registers cost frames without them. */
  VKR_METAL_TILED_LIGHTING_CASTERS_ALL = 6,
  VKR_METAL_TILED_LIGHTING_COUNT = 7,
} VkrMetalTiledLighting;

/* Each shading variant without and with reflection probes (ADR-087): probe
   shading costs the opaque pass registers on every pixel, so frames whose
   camera sees no probe volume take the variant without it. */
#define VKR_METAL_TILED_PROBE_VARIANT_COUNT 2u

/* Each opaque shading variant without and with decals (ADR-092), for the same
   reason: frames whose camera sees no decal box take the variant without
   them. Blended surfaces take no decals. */
#define VKR_METAL_TILED_DECAL_VARIANT_COUNT 2u

/* Each tiled shading variant without and with the BRDF material extensions
   (ADR-113): their state costs every pixel registers, so frames whose scene
   has no extended material take the variant without them. The extended
   pipelines are created in the background when a scene first publishes an
   extended material. */
#define VKR_METAL_TILED_BRDF_VARIANT_COUNT 2u

/* The opaque pass's shading variants: a BRDF variant, plus
   VKR_METAL_TILED_SUN_VARIANT in frames with sun contact shadows, whose
   entries also write the sun's light to a second target for the atmosphere
   draw's correction (VkrPreparedFrame.tiled_sun_contact). The sun variants
   are created in the background when a frame first asks for them. */
#define VKR_METAL_TILED_SUN_VARIANT 2u
/* Plus VKR_METAL_TILED_DEMAND_VARIANT in texture demand frames
   (VkrPreparedFrame.tiled_texture_demand, ADR-024), whose entries also write
   their demand words to a third target; created in the background when a
   frame first asks for them, by sun variant. */
#define VKR_METAL_TILED_DEMAND_VARIANT 4u
#define VKR_METAL_TILED_SHADING_VARIANT_COUNT 8u
/* The sun light target: the unshadowed light and the cascade visibility. */
#define VKR_METAL_TILED_SUN_LIGHT_FORMAT MTLPixelFormatRGBA16Float
/* The tiled opaque pass's texture demand words (ADR-024), a memoryless
   target the forward shading writes and vkr_metal_tiled_demand_tile reads
   (VkrMetalTiledForwardOutput.texture_demand). */
#define VKR_METAL_TILED_DEMAND_ATTACHMENT 2u
#define VKR_METAL_TILED_DEMAND_FORMAT MTLPixelFormatR32Uint

#define VKR_METAL_CUSTOM_LIBRARY_PATH 4096u

/* A Custom graph's pipelines (ADR-096): created on a background queue the
   first time a material names its function. */
typedef enum VkrMetalCustomGraphState {
  VKR_METAL_CUSTOM_GRAPH_EMPTY = 0,
  VKR_METAL_CUSTOM_GRAPH_PENDING,
  VKR_METAL_CUSTOM_GRAPH_READY,
  VKR_METAL_CUSTOM_GRAPH_FAILED,
} VkrMetalCustomGraphState;

typedef struct VkrMetalCustomGraph {
  char function[VKR_MATERIAL_CUSTOM_FUNCTION_CAPACITY];
  /* A VkrMetalCustomGraphState per shading variant (ADR-113). The base set
     is created when a material first names the function, the extension set
     when a published material of the graph first uses a BRDF extension, and
     the sun variants of each once frames ask for sun contact shadows. Each
     background creation publishes READY or FAILED with release order after
     writing its pipelines. */
  VkrAtomicUint32 state[VKR_METAL_TILED_SHADING_VARIANT_COUNT];
  id<MTLRenderPipelineState> forward[VKR_METAL_TILED_SHADING_VARIANT_COUNT]
                                    [VKR_METAL_TILED_DECAL_VARIANT_COUNT]
                                    [VKR_METAL_TILED_PROBE_VARIANT_COUNT]
                                    [VKR_METAL_TILED_LIGHTING_COUNT];
  id<MTLRenderPipelineState> coverage[VKR_METAL_TILED_SHADING_VARIANT_COUNT]
                                     [VKR_METAL_TILED_DECAL_VARIANT_COUNT]
                                     [VKR_METAL_TILED_PROBE_VARIANT_COUNT]
                                     [VKR_METAL_TILED_LIGHTING_COUNT];
  float64_t create_seconds;
  /* Published material rows naming the graph; a reload frees the slot at
     zero. Render-thread owned. */
  uint32_t references;
  /* The job creating each shading variant; a zero handle ran inline. */
  VkrJobHandle jobs[VKR_METAL_TILED_SHADING_VARIANT_COUNT];
} VkrMetalCustomGraph;

/* The editor's presenter (ADR-111) composites the newest finished Scene image
   with the UI on its own queue, so the UI presents at the display's rate while
   the frame queue renders slower Scenes. Three images let one Scene frame run
   and one wait while the presenter shows a third. */
#define VKR_METAL_PACKET_PRESENT_IMAGE_COUNT 3u
#define VKR_METAL_PACKET_PRESENT_SLOT_COUNT 3u
#define VKR_METAL_PACKET_PRESENT_RETIRED_MAX 8u

typedef struct VkrMetalPacketPresentImage {
  id<MTLTexture> texture;
  /** Frame-queue value of the Scene frame that last copied into it; zero
      until written. The copy has finished once the frame queue reaches it. */
  uint64_t scene_value;
  /** Presenter value of the last frame that sampled it. */
  uint64_t present_value;
} VkrMetalPacketPresentImage;

typedef struct VkrMetalPacketPresentSlot {
  id<MTL4CommandAllocator> allocator;
  id<MTL4CommandBuffer> buffer;
  /** Shared upload bytes: UI geometry, draw roots and the composite records.
      Grows only while this slot's last frame is complete. */
  id<MTLBuffer> upload;
  /** Presenter value of the frame recorded here; zero before the first. */
  uint64_t present_value;
  /** The frame queue's submit value when this frame was recorded. Textures it
      samples carry the value after it, and completion stays below it until
      this frame finishes (vkr_metal_packet_completed). */
  uint64_t frame_mark;
} VkrMetalPacketPresentSlot;

/* A replaced image or upload buffer, released once neither queue uses it. */
typedef struct VkrMetalPacketPresentRetired {
  id<MTLTexture> texture;
  id<MTLBuffer> buffer;
  uint64_t scene_value;
  uint64_t present_value;
} VkrMetalPacketPresentRetired;

typedef struct VkrMetalPacketPresenter {
  id<MTL4CommandQueue> queue;
  id<MTLSharedEvent> completion;
  /** The images and upload buffers; Scene frames that copy into an image use
      it too. */
  id<MTLResidencySet> residency;
  /** Counts finished Scene frames off the render thread. */
  MTLSharedEventListener *listener;
  dispatch_queue_t listener_queue;
  MTL4RenderPassDescriptor *render_pass;
  VkrMetalPacketPresentSlot slots[VKR_METAL_PACKET_PRESENT_SLOT_COUNT];
  VkrMetalPacketPresentImage images[VKR_METAL_PACKET_PRESENT_IMAGE_COUNT];
  VkrMetalPacketPresentRetired retired[VKR_METAL_PACKET_PRESENT_RETIRED_MAX];
  uint32_t retired_count;
  uint32_t next_slot;
  /** Image the current Scene frame's Editor.Present copies into. */
  uint32_t write_image;
  /** Presenter value the current Scene frame waits for before its copy
      overwrites an image presenter frames still sample; zero for none. */
  uint64_t frame_present_wait;
  uint64_t submit_value;
  /** Scene value of the newest image a presenter frame sampled. Later frames
      never show an older one, so every older image is free to rewrite. */
  uint64_t shown_scene_value;
  /** The last frame-queue value a committed presenter frame tagged its
      textures with; discarded uses never fall below it. */
  uint64_t texture_floor;
  /** Scene frames committed and finished, by the listener's queue. */
  VkrAtomicUint64 scene_frames_completed;
  VkrAtomicUint64 scene_completed_ns;
  bool8_t ready;
} VkrMetalPacketPresenter;

/** Surfaces wrapped for view streaming at once: an encoder's ring and the
    decoder's recycled images (ADR-131). */
#define VKR_METAL_PACKET_VIEW_TEXTURES 16u

/** One lib video surface wrapped as a texture, kept with a reference to the
    surface until the GPU finished the last frame that used it. */
typedef struct VkrMetalPacketViewTexture {
  VkrVideoSurface *surface;
  IOSurfaceRef platform;
  id<MTLTexture> texture;
  uint64_t last_use_value;
} VkrMetalPacketViewTexture;

/** View streaming (vkr_metal_packet_view.inc): exported and imported Scene
    images, made on first use. */
typedef struct VkrMetalPacketView {
  bool8_t ready;
  id<MTLResidencySet> residency;
  /** Reports finished exports off the render thread. */
  MTLSharedEventListener *listener;
  dispatch_queue_t listener_queue;
  id<MTLComputePipelineState> export_pipeline;
  VkrMetalPacketViewTexture textures[VKR_METAL_PACKET_VIEW_TEXTURES];
  /** The export of the frame being recorded, reported once the frame
      commits; `pending_written` once the frame copied into it. */
  VkrViewExport pending;
  bool8_t has_pending;
  bool8_t pending_written;
  /** The frame's prepared export dispatch, recorded in its readback
      encoder: its root and the surface's extent. */
  uint64_t export_root;
  uint32_t export_width;
  uint32_t export_height;
  bool8_t export_active;
  /** The frame's imported remote view the UI draws; nil without one. */
  id<MTLTexture> import_texture;
} VkrMetalPacketView;

/* The depth fade scale of a decal without depth fade: every point inside its
   box keeps full weight. */
#define VKR_METAL_DECAL_HARD_EDGE_SCALE 1.0e6f

struct VkrMetalPacketRenderer {
  VkrPixelReadbackResult picking_result;
  uint64_t picking_submit_value;
  VkrAllocator *allocator;
  VkrMetalDiagnostics diagnostics;
  Arena *graph_frame_arena;
  VkrAllocator graph_frame_allocator;
  VkrRgJsonGraph json_graph;
  VkrRenderGraph *graph;
  VkrRgExecutorRegistry executors;
  VkrExposureMeteringConfig exposure_metering;
  /** Bounded simulation time of the last submitted automatic exposure. */
  float64_t exposure_seconds;
  VkrBloomConfig bloom_config;
  VkrMetalMemoryDevice *memory;
  VkrMetalMaterialTableDevice *materials;
  VkrCaptureRing capture_ring;
  void *capture_storage;
  /**
   * The capture ring is sized by capture_max_batch_bytes times the ring
   * capacity and reaches ~96 MiB at the production defaults — far larger than
   * the renderer arena, which is sized for per-frame graph work. It therefore
   * gets its own reservation, exactly as the Vulkan renderer does,
   * instead
   * of displacing every other renderer allocation.
   */
  VkrDMemory capture_storage_memory;
  VkrAllocator capture_storage_allocator;
  /** Backs every fixed-capacity record array; see the helpers below. */
  VkrDMemory record_memory;
  VkrAllocator record_allocator;
  VkrMetalPacketImage *images;
  VkrMetalPacketRetiredImageViews *retired_image_views;
  uint64_t retired_image_view_count;
  VkrMetalPacketGraphBuffer *graph_buffers;
  VkrMetalPacketGraphBufferInstance *graph_buffer_instances;
  VkrRgBufferHandle gpu_candidate_buffer_handle;
  VkrRgBufferHandle gpu_candidate_instance_buffer_handle;
  VkrRgBufferHandle exposure_state_handle;
  VkrMetalPacketMesh *meshes;
  VkrMetalPacketGeometryMegabuffer geometry_megabuffer;
  VkrGeometryRanges geometry_ranges;
  VkrMetalPacketMaterial *packet_materials;
  VkrMetalPacketTexture *textures;
  VkrMetalPacketSampler *samplers;
  VkrMetalPacketTextUpload *text_uploads;
  MTL4RenderPassDescriptor **render_passes;
  // Encoder labels are reused while an expanded graph pass keeps the same name.
  // Conditional passes and repeat counts can move a name to another index.
  VkrMetalPacketPassLabel *pass_labels;
  id<MTLDevice> device;
  char device_name[256];
  id<MTL4Compiler> compiler;
  /** Retained only so completion-safe presentation format changes can rebuild
   * the four physical-output pipelines without borrowing config paths. */
  id<MTLLibrary> fragment_library;
  id<MTL4PipelineDataSetSerializer> pipeline_serializer;
  id<MTL4Archive> pipeline_archive;
  MTL4CompilerTaskOptions *compiler_options;
  id<MTL4CommandQueue> queue;
  id<MTLSharedEvent> completion;
  VkrMetalPacketPresenter presenter;
  VkrMetalPacketView view;
  VkrMetalPacketCommandSlot *command_slots;
  VkrMetalPacketResult *completed_timing_results;
  VkrMetalPacketCommitFeedbackRecord *commit_feedback_records;
  VkrMetalPacketCommandSlot *active_command_slot;
  /* Acquired frame storage; later frame acquisition skips it until
     render/cancel. */
  VkrMetalPacketCommandSlot *reserved_frame_slot;
  uint32_t command_slot_count;
  /* Uploads rotate through their own slots, one per upload ring slot, so a
     publication waits only for an earlier upload, never for a frame. */
  VkrMetalPacketCommandSlot
      upload_slots[VKR_METAL_PACKET_UPLOAD_SLOT_COUNT_MAX];
  uint32_t upload_slot_count;
  uint32_t next_upload_slot;
  uint32_t history_instance_count;
  uint32_t history_output_index;
  uint32_t selected_cloud_history_instance;
  VkrMetalPacketCloudHistoryState
      cloud_history[VKR_METAL_PACKET_GRAPH_INSTANCE_MAX];
  uint32_t next_command_slot;
  uint32_t next_completed_timing;
  uint32_t next_commit_feedback;
  VkrMetalPacketWaitCounters wait_counters;
  /* Present and drawable-acquire CPU time since the metrics collector last
     read it; render thread only. */
  VkrRendererImplPresentTiming present_timing;
  /* A native GPU capture: its state, request and the frames still to
     capture; render thread only. */
  VkrMetalPacketGpuTraceStatus gpu_trace;
  uint32_t gpu_trace_frames_left;
  /* Display feedback for window presents: shared state, the one handler
     every present adds, and the next mark a present writes. */
  VkrMetalPresentFeedback *present_feedback;
  void (^presented_handler)(id<MTLDrawable>);
  uint32_t next_present_mark;
  /* Submissions the GPU finished with an error; read from any thread. */
  VkrAtomicUint64 gpu_error_count;
  /* Active-slot aliases keep encoding code independent of slot selection. */
  id<MTL4CommandAllocator> command_allocator;
  id<MTL4CommandBuffer> command_buffer;
  VkrMetalPacketTextureUploadBatch texture_upload_batch;
  uint64_t timestamp_frequency;
  id<MTLRenderPipelineState> gpu_shadow_pipeline;
  id<MTLRenderPipelineState> gpu_shadow_opaque_pipeline;
  id<MTLRenderPipelineState> depth_clear_pipeline;
  /* The tiled pipeline (ADR-087) with `tiled_sample_count` samples:
     forward shading and the clear sky draw into memoryless multisampled
     targets that resolve into the graph's colour and depth, and the cloud
     draw over the resolved image. Opaque draws, which never discard, and
     alpha-tested draws, which write the samples their alpha covers; without
     and with decals and reflection probes, by shading variant. */
  id<MTLRenderPipelineState>
      tiled_forward_pipelines[VKR_METAL_TILED_SHADING_VARIANT_COUNT]
                             [VKR_METAL_TILED_DECAL_VARIANT_COUNT]
                             [VKR_METAL_TILED_PROBE_VARIANT_COUNT]
                             [VKR_METAL_TILED_LIGHTING_COUNT];
  id<MTLRenderPipelineState>
      tiled_coverage_pipelines[VKR_METAL_TILED_SHADING_VARIANT_COUNT]
                              [VKR_METAL_TILED_DECAL_VARIANT_COUNT]
                              [VKR_METAL_TILED_PROBE_VARIANT_COUNT]
                              [VKR_METAL_TILED_LIGHTING_COUNT];
  /* The sky, the tone-mapped multisample resolve at the end of the opaque
     pass and the atmosphere draw; first index 1 in frames with sun contact
     shadows, whose opaque pass has the sun's light target and whose
     atmosphere draw corrects the sun's light, second index 1 in texture
     demand frames, whose opaque pass has the demand target. */
  id<MTLRenderPipelineState> tiled_sky_pipelines[2][2];
  id<MTLRenderPipelineState> tiled_resolve_pipelines[2][2];
  /* The demand frames' texture demand tile kernel, by sun variant
     (ADR-024). */
  id<MTLRenderPipelineState> tiled_demand_pipelines[2];
  id<MTLRenderPipelineState> tiled_atmosphere_pipelines[2][2];
  /* The overdraw view (ADR-127): every opaque and alpha-tested draw adds a
     layer in the opaque pass, by sun variant, and every blended draw one in
     the blend pass, with no depth test. */
  id<MTLRenderPipelineState> tiled_overdraw_pipelines[2];
  id<MTLRenderPipelineState> tiled_overdraw_blend_pipeline;
  /* Object ids of the camera's opaque draws, single-sampled, for a pick. */
  id<MTLRenderPipelineState> tiled_picking_pipeline;
  /* Glass and blended surfaces, and world text, over the resolved image. */
  id<MTLRenderPipelineState>
      tiled_blend_pipelines[VKR_METAL_TILED_BRDF_VARIANT_COUNT]
                           [VKR_METAL_TILED_PROBE_VARIANT_COUNT]
                           [VKR_METAL_TILED_LIGHTING_COUNT];
  /* The fragment function constants of each shading variant, which every
     pipeline whose entry reaches vkr_metal_tiled_shade takes. */
  MTLFunctionConstantValues
      *tiled_shading_constants[VKR_METAL_TILED_SHADING_VARIANT_COUNT];
  /* A VkrMetalCustomGraphState for the extended variant's pipelines: EMPTY
     until a scene first publishes an extended material, then PENDING while
     the custom group creates them, READY or FAILED with release order. */
  VkrAtomicUint32 tiled_brdf_state;
  VkrJobHandle tiled_brdf_job;
  /* The same for the sun variants of both BRDF variants and the sky,
     resolve and atmosphere pipelines of sun contact frames: EMPTY until a
     frame first asks for sun contact shadows. `tiled_sun_requested` is the
     render thread's record of that request. */
  VkrAtomicUint32 tiled_sun_state;
  VkrJobHandle tiled_sun_job;
  bool8_t tiled_sun_requested;
  /* The same for the demand variants of both BRDF variants and the sky,
     resolve and demand tile pipelines of texture demand frames, by sun
     variant: EMPTY until a frame first asks for demand in that variant. */
  VkrAtomicUint32 tiled_demand_state[2];
  VkrJobHandle tiled_demand_jobs[2];
  bool8_t tiled_demand_requested[2];
  id<MTLRenderPipelineState> tiled_text_pipeline;
  /* Custom material graphs (ADR-096): the project library, each slot's
     pipelines (slot 0 stays empty) and the jobs creating them. */
  id<MTLLibrary> custom_library;
  /* Where the library loads from, kept for reloads, and whether the next
     frame preparation reloads it. */
  char custom_library_path[VKR_METAL_CUSTOM_LIBRARY_PATH];
  VkrAtomicBool custom_reload_requested;
  /* Without an offline compile (no Metal Toolchain), `vkr_bakery
     materials` leaves the library's source beside the missing metallib
     and a job compiles it: the library it gave and the text it compiled,
     which a reload takes while that text is still the file's. Written by
     the job before `custom_compiling` clears, read on the render thread
     after it does. */
  id<MTLLibrary> custom_compiled;
  NSString *custom_compiled_source;
  VkrAtomicBool custom_compiling;
  VkrJobHandle custom_compile_job;
  /* Borrowed job system for background pipeline creation; NULL creates
     pipelines on the requesting thread. Detached before its owner shuts it
     down (vkr_metal_packet_renderer_detach_jobs). */
  VkrJobSystem *job_system;
  /* Publishes each background build's time (pipeline.create). */
  VkrMetricEventProducer pipeline_create;
  /* Publishes each shader library load's time (shader.load). */
  VkrMetricEventProducer shader_load;
  /* Borrowed from the renderer: each pipeline created, with its compile
     time and threadgroup limits (ADR-130); NULL records none. */
  VkrPipelineRegistry *pipeline_registry;
  /* Background creations requested and not yet finished. */
  VkrAtomicUint32 background_builds;
  bool8_t custom_loaded;
  VkrMetalCustomGraph custom_graphs[VKR_METAL_CUSTOM_GRAPH_SLOTS];
  /* The Standard draw kinds' pipelines (ADR-129): the Standard entries with
     the kind's function constants, created in the background per shading
     variant the first time a frame asks for it. `function` stays empty. */
  VkrMetalCustomGraph standard_kinds[VKR_METAL_STANDARD_KIND_COUNT];
  /* This frame's ready kinds, bit plane * VKR_METAL_STANDARD_KIND_COUNT +
     kind; the upload plan sets it. */
  uint32_t standard_kind_mask;
  /* Slots given to graphs; slots stay given until the pipelines release. */
  uint32_t custom_graph_count;
  /* Late Custom draws of every completed frame, read beside the render
     thread (vkr_metal_packet_renderer_pipeline_stats). */
  VkrAtomicUint64 custom_late_draws;
  /* The newest completed draw census, copied from its frame's readback
     when the frame's result is decoded; storage from `allocator`. */
  VkrDrawCensusStore draw_census;
  /* The newest completed frame's texture demand (ADR-024), one entry per
     live material its camera drew, until
     vkr_metal_packet_renderer_take_texture_demand takes it; max_materials
     entries, and the submission it came from. */
  VkrTextureDemand *texture_demand;
  uint32_t texture_demand_count;
  uint64_t texture_demand_submit_value;
  /* Native work the render thread recorded since the last frame commit,
     the counts of that commit's frame (backend.* metrics), and the
     pipeline the current encoder last bound, which tells a bind that
     changes it from one that does not. */
  VkrBackendFrameCounters frame_counters;
  VkrBackendFrameCounters submitted_counters;
  const void *counted_pipeline;
  /* This frame's Custom compaction in the upload ring, and its ready
     mask. */
  VkrMetalCustomCompaction *custom_compaction;
  uint64_t custom_compaction_gpu;
  /* The late occlusion view's Custom compaction (ADR-032), which the late
     cull pass makes from the camera's ready masks; zero before it or
     without Custom graphs. */
  uint64_t late_custom_compaction_gpu;
  /* The frame's per-view draw roots, which the early encode pass writes and
     the late cull pass reads for the late view. */
  /* The selected HZB history's producer had not completed at preparation,
     so the camera's classify waits for the queue's earlier dispatches
     (ADR-032). */
  bool8_t hzb_history_in_flight;
  uint64_t gpu_draw_roots_gpu;
  uint32_t custom_ready_mask;
  /* This frame buckets extended Standard draws into Custom slot 0, which the
     tiled pipeline shades with its extension variant (ADR-113): a scene
     material uses an extension and the variant's pipelines are READY. The
     upload plan sets it. */
  bool8_t tiled_brdf_draws;
  /* The Custom slots whose extension pipelines draw this frame, bit
     slot - 1. */
  uint32_t custom_brdf_ready_mask;
  id<MTLDepthStencilState> tiled_shade_state;
  id<MTLDepthStencilState> tiled_sky_state;
  id<MTLDepthStencilState> tiled_overdraw_state;
  id<MTLTexture> tiled_msaa_color;
  id<MTLTexture> tiled_msaa_depth;
  /* The multisampled sun light of sun contact frames; nil until one runs. */
  id<MTLTexture> tiled_msaa_sun;
  uint32_t tiled_width;
  uint32_t tiled_height;
  /* Targets a resize replaced, released once the GPU completes
     `tiled_retired_submit_value`, the last submission that drew into them. */
  id<MTLTexture> tiled_retired_color;
  id<MTLTexture> tiled_retired_depth;
  id<MTLTexture> tiled_retired_sun;
  uint64_t tiled_retired_submit_value;
  /* The opaque pass's memoryless texture demand target at every sample
     count, its extent, and the one a resize replaced, released once the GPU
     completes `tiled_retired_demand_submit_value`. */
  id<MTLTexture> tiled_demand_target;
  uint32_t tiled_demand_width;
  uint32_t tiled_demand_height;
  id<MTLTexture> tiled_retired_demand;
  uint64_t tiled_retired_demand_submit_value;
  /* The opaque pass's sample count, fixed at creation: four with the spatial
     upscale, one with MetalFX, which draws into the graph's targets. */
  uint32_t tiled_sample_count;
  /* The tiled pipeline upscales with MetalFX (config, device support). */
  bool8_t tiled_temporal;
  /* The MetalFX upscaler: camera motion from the resolved depth,
     and MetalFX's temporal scaler from the internal scene to the Scene output
     with the fence its untracked graph textures need. The scaler is
     recreated, after the GPU completes every frame that used it, when either
     extent changes. */
  id<MTLComputePipelineState> tiled_motion_pipeline;
  id<MTL4FXTemporalScaler> tiled_temporal_scaler;
  id<MTLFence> tiled_temporal_fence;
  /* Object motion: the dynamic draws of the motion culling view over the
     camera motion, and the transform history the next frame reads. */
  id<MTLRenderPipelineState> tiled_object_motion_pipeline;
  id<MTLComputePipelineState> temporal_transform_pipeline;
  /* The still-camera running mean over MetalFX's output. */
  id<MTLComputePipelineState> tiled_stabilize_pipeline;
  /* The sun shafts' source and their march toward the sun (ADR-132). */
  id<MTLComputePipelineState> tiled_sun_shafts_source_pipeline;
  id<MTLComputePipelineState> tiled_sun_shafts_pipeline;
  VkrRgBufferHandle transform_history_handle;
  /* The last submitted frame: its history instance, which holds its
     transforms and skinned vertices, its frame index and skinning layout,
     and whether it ran the upscaler. Frames on the one queue order the
     reads after its writes with queue barriers. */
  uint32_t temporal_previous_history_index;
  uint64_t temporal_previous_submit_value;
  /* The last submitted frame wrote a stabilized image in its instance. */
  bool8_t temporal_previous_stabilized;
  uint32_t temporal_previous_frame_index;
  bool8_t temporal_previous_valid;
  VkrSkinningHistory temporal_previous_skinning;
  /* Each skinning binding's previous deformed vertices, or zero. */
  uint64_t skinning_previous_addresses[VKR_SKINNING_BINDING_CAPACITY];
  uint32_t tiled_temporal_input_width;
  uint32_t tiled_temporal_input_height;
  uint32_t tiled_temporal_width;
  uint32_t tiled_temporal_height;
  /* A scaler creation failed: later frames take the spatial upscale. */
  bool8_t tiled_temporal_unsupported;
  id<MTLRenderPipelineState> ui_pipeline;
  id<MTLRenderPipelineState> picking_pipeline;
  id<MTLRenderPipelineState> editor_overlay_pipeline;
  id<MTLRenderPipelineState> editor_overlay_picking_pipeline;
  /* Selection outline: overlay geometry into an R8 mask, then a full-screen
   * edge pass over the Scene image. */
  id<MTLRenderPipelineState> editor_selection_mask_pipeline;
  id<MTLRenderPipelineState> editor_selection_outline_pipeline;
  id<MTLRenderPipelineState> editor_grid_pipeline;
  id<MTLRenderPipelineState> editor_pass_output_pipeline;
  id<MTLRenderPipelineState> animation_preview_pipeline;
  id<MTLRenderPipelineState> tonemap_pipeline;
  id<MTLRenderPipelineState> display_linear_pipeline;
  id<MTLRenderPipelineState> picking_text_pipeline;
  id<MTLComputePipelineState> ibl_prefilter_pipeline;
  id<MTLComputePipelineState> ibl_sh_pipeline;
  id<MTLComputePipelineState> atmosphere_transmittance_pipeline;
  id<MTLComputePipelineState> atmosphere_multiple_scattering_pipeline;
  id<MTLComputePipelineState> atmosphere_source_pipeline;
  id<MTLComputePipelineState> sky_view_pipeline;
  id<MTLComputePipelineState> aerial_perspective_pipeline;
  id<MTLComputePipelineState>
      cloud_noise_pipelines[VKR_METAL_PACKET_CLOUD_NOISE_COUNT];
  id<MTLComputePipelineState> cloud_shadow_pipeline;
  id<MTLComputePipelineState> cloud_trace_pipeline;
  /* SDF sun shadows: a distance volume's clear, triangle splat and
     conversion, the light-space grid's clear, count, block reduction, block
     scan, offsets and scatter, and the half-resolution trace
     (vkr_metal_packet_sdf.inc). */
  id<MTLComputePipelineState> sdf_clear_pipeline;
  id<MTLComputePipelineState> sdf_splat_pipeline;
  id<MTLComputePipelineState> sdf_convert_pipeline;
  id<MTLComputePipelineState> sdf_grid_clear_pipeline;
  id<MTLComputePipelineState> sdf_grid_count_pipeline;
  id<MTLComputePipelineState> sdf_grid_reduce_pipeline;
  id<MTLComputePipelineState> sdf_grid_scan_pipeline;
  id<MTLComputePipelineState> sdf_grid_apply_pipeline;
  id<MTLComputePipelineState> sdf_grid_scatter_pipeline;
  id<MTLComputePipelineState> sun_shadow_trace_pipeline;
  /* The field table each SDF frame copies for the GPU: every live mesh's
     volume rows, and per mesh slot below `sdf_mesh_count` its first row and
     row count. Mesh creation and release lay it out again; a generation
     updates its row. */
  VkrMetalSdfVolumeRow *sdf_rows;
  uint32_t sdf_row_count;
  uint32_t sdf_row_capacity;
  uint32_t *sdf_mesh_volumes;
  uint32_t sdf_mesh_count;
  bool8_t sdf_layout_dirty;
  /* The next candidate the generation scan looks at. */
  uint32_t sdf_scan_cursor;
  /* The field's revision, which the frame loop reads: it grows on the first
     frame that generates no volume after frames that did. */
  VkrAtomicUint64 sdf_field_revision;
  bool8_t sdf_field_grew;
  /* Bytes of the distance volumes created so far, and the submission of the
     last frame that generated one, which a later trace waits for while it
     may still run. */
  uint64_t sdf_volume_bytes;
  uint64_t sdf_generation_submit_value;
  id<MTLComputePipelineState> cloud_sky_light_pipeline;
  id<MTLComputePipelineState> cloud_sky_light_sh_pipeline;
  id<MTLComputePipelineState> cloud_sky_light_mips_pipeline;
  id<MTLComputePipelineState> diffuse_volume_compose_pipeline;
  /** The diffuse-volume SH texture and composition the last submitted frame
      wrote, and whether it added the lamp direct bands; a frame whose binding
      differs composes again. */
  VkrTextureHandle diffuse_volume_composed_sh;
  uint32_t diffuse_volume_composition;
  bool8_t diffuse_volume_composed_direct;
  id<MTLComputePipelineState> gpu_draw_classify_pipeline;
  id<MTLComputePipelineState> gpu_draw_prefix_pipeline;
  /* The late occlusion phase's prefix (ADR-032). */
  id<MTLComputePipelineState> gpu_draw_prefix_late_pipeline;
  id<MTLComputePipelineState> gpu_draw_encode_pipeline;
  id<MTLComputePipelineState> gpu_draw_encode_inherited_pipeline;
  id<MTLRenderPipelineState> preview_pipeline;
  id<MTLComputePipelineState> skinning_pipeline;
  VkrRgBufferHandle skinning_output_handle;
  VkrSkinningHistory pending_skinning_history;
  uint64_t skinning_addresses[VKR_SKINNING_BINDING_CAPACITY];
  id<MTLComputePipelineState> hzb_build_pipeline;
  id<MTLComputePipelineState> exposure_clear_pipeline;
  id<MTLComputePipelineState> exposure_histogram_pipeline;
  id<MTLComputePipelineState> exposure_resolve_pipeline;
  id<MTLComputePipelineState> gpu_spike_pipeline;
  /* Cull.Census (VkrGpuDebugPayload.draw_census); NULL skips the census. */
  id<MTLComputePipelineState> gpu_draw_census_pipeline;
  id<MTLComputePipelineState> bloom_prefilter_pipeline;
  /* Both filters are resident; the cold configuration selects which one a
     build measures. Neither is a fallback for the other. */
  id<MTLComputePipelineState> bloom_downsample_tent13_pipeline;
  id<MTLComputePipelineState> bloom_downsample_box4_pipeline;
  /* The tiled pipeline's refraction copy's base level (ADR-087). */
  id<MTLComputePipelineState> bloom_downsample_box2_pipeline;
  id<MTLComputePipelineState> bloom_upsample_pipeline;
  id<MTLArgumentEncoder> gpu_draw_icb_argument_encoder;
  /* Camera depth writes test Greater (reversed depth, ADR-109); shadow map
     writes test Less. */
  id<MTLDepthStencilState> depth_write_state;
  id<MTLDepthStencilState> shadow_depth_write_state;
  id<MTLDepthStencilState> depth_clear_state;
  id<MTLDepthStencilState> depth_read_state;
  id<MTL4ArgumentTable> argument_table;
  /* L2 diffuse coefficient slots (ADR-038). Renderer lifetime: the pool
     survives scene reload, and scene reset only retires publications. */
  id<MTLBuffer> sh_coefficients;
  VkrShSlotPool sh_pool;
  CAMetalLayer *layer;
  id<CAMetalDrawable> drawable;
  void *display_output_context;
  VkrDisplayOutputSnapshot (*display_output_snapshot)(void *context);
  VkrDisplayOutputParams display_output;
  VkrRenderGraphFrameInfo prepared_frame;
  uint64_t submit_value;
  /* The newest submission that uploaded texture data. Metal 4 does not order
     later command buffers after it, so work that may sample those textures
     waits for it on the GPU (vkr_metal_packet_order_after_texture_uploads)
     instead of the CPU waiting at publication. */
  uint64_t texture_upload_order_value;
  uint64_t candidate_publication_generation;
  /* VkrPublicationChange bits since the previous packet build. */
  uint32_t candidate_publication_changes;
  uint32_t gpu_draw_count;
  /** Packed static candidates, which precede the dynamic ones. */
  uint32_t gpu_draw_static_count;
  uint64_t current_hzb_world_epoch;
  Mat4 current_hzb_view_projection;
  Mat4 current_hzb_raster_view_projection;
  /** The view-projection a selected HZB history must match: the current
      one, or a frozen culling camera's (ADR-127), whose history keeps
      without a rebuild while `hzb_frozen` is set. */
  Mat4 hzb_select_view_projection;
  bool8_t hzb_frozen;
  uint32_t selected_hzb_history_instance;
  uint32_t selected_exposure_history_instance;
  /** Accumulated during this frame's packet lowering; copied into the result.
   */
  VkrPacketBuildMetrics packet_build;
  uint32_t max_images;
  uint32_t max_passes;
  uint32_t max_meshes;
  uint32_t max_materials;
  uint32_t max_textures;
  uint32_t max_samplers;
  uint32_t sampler_count;
  /* Set once a full cache first substitutes a sampler. */
  bool8_t sampler_substitution_logged;
  uint32_t max_draws;
  uint32_t max_instances;
  uint32_t gpu_draw_icb_view_group_size;
  uint32_t gpu_draw_icb_group_count;
  /* The commands an ICB group's pool takes, from the frames' visible draws
     read back at completion with headroom; each slot grows to it. */
  uint32_t gpu_draw_pool_target;
  uint64_t upload_slot_size;
  uint32_t resize_count;
  /* Only allocation failure enables the cold resize retirement prepass. */
  bool8_t graph_memory_recovery_pending;
  VkrMetalPacketTargetKind target_kind;
  VkrPresentMode actual_present_mode;
  VkrDisplayOutputMode display_output_mode;
  bool8_t srgb_output;
  bool8_t tonemap_enabled;
  /* The renderer's resolution controller steps the tiled pipeline's spatial
     upscale. Frames then report their GPU time. */
  bool8_t dynamic_resolution_enabled;
  bool8_t deferred_candidate_drop_logged;
  bool8_t convert_vulkan_clip_y;
  bool8_t hzb_enabled;
  bool8_t frustum_enabled;
  bool8_t frame_prepared;
  /* A presentation-only frame holds the drawable (ADR-111). Unlike a
     prepared frame it records nothing until it presents, so publications
     may run meanwhile. */
  bool8_t present_prepared;
  bool8_t pipeline_archive_warm;
  bool8_t pipeline_archive_written;
  /* Immutable black cubemap for frames with local probes but no global IBL.
     It prevents an earlier scene's retained prefilter from becoming the
     residual outside a local probe's influence. */
  VkrMetalTextureResource ibl_black_prefilter;
  /** Published coefficient slot for the active global environment source. */
  uint32_t ibl_sh_slot;
  uint64_t ibl_black_last_use_submit_value;
  bool8_t ibl_black_live;
  VkrMetalPacketPendingIblBake
      pending_ibl_bakes[VKR_METAL_PACKET_PENDING_IBL_BAKE_MAX];
  uint32_t pending_ibl_bake_count;
  VkrMetalPacketPreExposureRecord
      pre_exposure_history[VKR_METAL_PACKET_PRE_EXPOSURE_HISTORY];
  uint32_t pre_exposure_history_next;
  /* Immutable RG16Float split-sum BRDF coefficients. Renderer lifetime;
     retirement waits for the last submitted frame root that references it. */
  VkrMetalTextureResource dfg_lut;
  uint64_t dfg_lut_last_use_submit_value;
  bool8_t dfg_lut_live;
  /* Cloud base, detail and weather noise (ADR-074): generated once at
     startup, renderer lifetime, retired after the last frame reading them. */
  VkrMetalTextureResource cloud_noise[VKR_METAL_PACKET_CLOUD_NOISE_COUNT];
  uint64_t cloud_noise_last_use_submit_value;
  uint32_t cloud_noise_live_count;
  VkrMetalTextureResource ltc_luts[2];
  uint64_t ltc_last_use_submit_value;
  uint32_t ltc_lut_live_count;
  VkrMetalTextureResource sheen_directional_albedo_lut;
  VkrMetalTextureResource sheen_ltc_luts[VKR_SHEEN_LTC_LUT_TABLE_COUNT];
  uint64_t sheen_lut_last_use_submit_value;
  uint32_t sheen_ltc_lut_live_count;
  bool8_t sheen_directional_albedo_lut_live;
  VkrMetalTextureResource anisotropy_luts[VKR_ANISOTROPY_LUT_TABLE_COUNT];
  uint64_t anisotropy_lut_last_use_submit_value;
  uint32_t anisotropy_lut_live_count;
  bool8_t synchronous_validation_readback;
};

/* Fixed-capacity record arrays share one tagged reservation. Variable submesh
   metadata belongs to each live mesh and uses renderer->allocator, so capacity
   follows published geometry and is reclaimed independently at destruction. */
vkr_internal void *vkr_metal_packet_alloc_zeroed(VkrAllocator *allocator,
                                                 uint64_t size) {
  if (!size)
    return NULL;
  void *memory =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (memory)
    MemZero(memory, size);
  return memory;
}

vkr_internal void vkr_metal_packet_free_sized(VkrAllocator *allocator,
                                              void *memory, uint64_t size) {
  if (memory && size)
    vkr_allocator_free(allocator, memory, size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

/** Tagged suballocation from the shared record reservation. */
vkr_internal void *vkr_metal_packet_record_alloc(VkrAllocator *records,
                                                 uint64_t size, bool8_t zero) {
  if (!size)
    return NULL;
  void *memory =
      vkr_allocator_alloc(records, size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (memory && zero)
    MemZero(memory, size);
  return memory;
}

vkr_internal bool8_t vkr_metal_packet_record_size_add(uint64_t *total,
                                                      uint64_t bytes) {
  if (!total || bytes > UINT64_MAX - (KB(4) - 1u))
    return false_v;
  const uint64_t aligned = AlignPow2(bytes, KB(4));
  if (aligned < bytes || aligned > UINT64_MAX - KB(4) ||
      *total > UINT64_MAX - aligned - KB(4))
    return false_v;
  *total += aligned + KB(4);
  return true_v;
}

#define VKR_METAL_PACKET_ARRAY_BYTES(NAME, MEMBER, COUNT_EXPR)                 \
  vkr_internal uint64_t NAME(const VkrMetalPacketRenderer *renderer) {         \
    return (uint64_t)(COUNT_EXPR) * sizeof(*renderer->MEMBER);                 \
  }

VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_images_bytes, images,
                             renderer->max_images)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_graph_buffers_bytes,
                             graph_buffers, renderer->max_images)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_graph_buffer_instances_bytes,
                             graph_buffer_instances,
                             (uint64_t)renderer->max_images *
                                 renderer->history_instance_count)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_meshes_bytes, meshes,
                             renderer->max_meshes)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_materials_bytes, packet_materials,
                             renderer->max_materials)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_texture_demand_bytes,
                             texture_demand, renderer->max_materials)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_textures_bytes, textures,
                             renderer->max_textures)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_samplers_bytes, samplers,
                             renderer->max_samplers)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_text_uploads_bytes, text_uploads,
                             renderer->max_draws)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_render_passes_bytes,
                             render_passes, renderer->max_passes)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_pass_labels_bytes, pass_labels,
                             renderer->max_passes)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_command_slots_bytes,
                             command_slots, renderer->command_slot_count)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_timing_results_bytes,
                             completed_timing_results,
                             VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_commit_feedback_bytes,
                             commit_feedback_records,
                             VKR_METAL_PACKET_COMMIT_FEEDBACK_CAPACITY)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_retired_image_views_bytes,
                             retired_image_views,
                             (uint64_t)renderer->max_images *
                                 renderer->history_instance_count * 2u)

#undef VKR_METAL_PACKET_ARRAY_BYTES

/* Diagnostics are single-owner CPU observations, never GPU completion proofs.
   Keep argument evaluation and native counter queries out of normal frames. */
#define VKR_METAL_DIAGNOSTIC(renderer, event, ...)                             \
  do {                                                                         \
    if ((renderer)->diagnostics.enabled)                                       \
      vkr_metal_diagnostics_record(                                            \
          &(renderer)->diagnostics, event, (renderer)->submit_value,           \
          (renderer)->completion.signaledValue, __VA_ARGS__);                  \
  } while (0)

#define VKR_METAL_DIAGNOSTIC_FLUSH(renderer)                                   \
  do {                                                                         \
    if ((renderer)->diagnostics.enabled)                                       \
      vkr_metal_diagnostics_flush(&(renderer)->diagnostics);                   \
  } while (0)

/*
 * Private implementation units remain one Objective-C translation unit. This
 * keeps renderer state and static helpers private while making each lifetime
 * domain independently reviewable. Public Objective-C entrypoints own lexical
 * autorelease pools because the C application loop has no surrounding pool.
 * Objects that span calls, including drawables and open upload encoders, retain
 * explicit ownership independently of those temporary-object scopes.
 */
// clang-format off
/* Copy completed pixels while their ring slices still exist. Submit order,
 * rather than caller IDs, also orders harness requests whose ID is zero. */
vkr_internal void vkr_metal_packet_collect_picking_results(
    VkrMetalPacketRenderer *renderer, uint64_t completed_submit_value) {
  for (uint32_t i = 0u; i < renderer->command_slot_count; ++i) {
    VkrMetalPacketCommandSlot *slot = &renderer->command_slots[i];
    if (!slot->picking_readback || slot->submit_value > completed_submit_value)
      continue;
    if (slot->submit_value == renderer->picking_submit_value) {
      MemCopy(&renderer->picking_result.data, slot->picking_readback,
              sizeof(renderer->picking_result.data));
      MemCopy(&renderer->picking_result.depth,
              slot->picking_readback + sizeof(renderer->picking_result.data),
              sizeof(renderer->picking_result.depth));
      renderer->picking_result.valid = true_v;
      renderer->picking_result.status = VKR_READBACK_STATUS_READY;
    }
    slot->picking_readback = NULL;
  }
}

/* The newest frame-queue value whose resources are free. A presenter frame
   samples textures tagged with the frame-queue value after its mark, so
   completion stays at that mark until the presenter frame finishes
   (ADR-111). */
vkr_internal uint64_t
vkr_metal_packet_completed(const VkrMetalPacketRenderer *renderer) {
  uint64_t completed = renderer->completion.signaledValue;
  const VkrMetalPacketPresenter *presenter = &renderer->presenter;
  if (!presenter->ready || presenter->submit_value == 0u)
    return completed;
  const uint64_t presented = presenter->completion.signaledValue;
  for (uint32_t i = 0u; i < ArrayCount(presenter->slots); ++i) {
    const VkrMetalPacketPresentSlot *slot = &presenter->slots[i];
    if (slot->present_value > presented)
      completed = Min(completed, slot->frame_mark);
  }
  return completed;
}

/* The VkrBackendCounter kinds Metal counts. Metal 4 copies on compute
   encoders, so it has no blit encoders; render passes, indirect draw calls,
   per-resource barriers and descriptor writes are Vulkan's. */
#define VKR_METAL_PACKET_COUNTED_MASK                                          \
  (VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_COMMAND_BUFFERS) |              \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_QUEUE_SUBMITS) |                \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_RENDER_ENCODERS) |              \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_COMPUTE_ENCODERS) |             \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_ICB_EXECUTIONS) |               \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_DRAWS_DIRECT) |                 \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_DISPATCHES) |                   \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_BARRIERS) |                     \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_PIPELINE_BINDS) |               \
   VKR_BACKEND_COUNTER_BIT(VKR_BACKEND_COUNTER_PIPELINE_CHANGES))

/* Counts native work for the backend.* metrics; render thread only. */
vkr_internal INLINE void
vkr_metal_packet_count(VkrMetalPacketRenderer *renderer,
                       VkrBackendCounter counter) {
  renderer->frame_counters.values[counter]++;
}

/* A new encoder: it starts without a bound pipeline. */
vkr_internal INLINE void
vkr_metal_packet_count_encoder(VkrMetalPacketRenderer *renderer,
                               VkrBackendCounter counter) {
  renderer->frame_counters.values[counter]++;
  renderer->counted_pipeline = NULL;
}

/* A pipeline bind, which changes the encoder's pipeline unless it binds
   the one bound last. */
vkr_internal INLINE void
vkr_metal_packet_count_pipeline(VkrMetalPacketRenderer *renderer,
                                const void *pipeline) {
  renderer->frame_counters.values[VKR_BACKEND_COUNTER_PIPELINE_BINDS]++;
  if (pipeline != renderer->counted_pipeline)
    renderer->frame_counters.values[VKR_BACKEND_COUNTER_PIPELINE_CHANGES]++;
  renderer->counted_pipeline = pipeline;
}

vkr_internal INLINE void vkr_metal_packet_set_render_pipeline(
    VkrMetalPacketRenderer *renderer, id<MTL4RenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline) API_AVAILABLE(macos(26.0)) {
  [encoder setRenderPipelineState:pipeline];
  vkr_metal_packet_count_pipeline(renderer, (const void *)pipeline);
}

vkr_internal INLINE void vkr_metal_packet_set_compute_pipeline(
    VkrMetalPacketRenderer *renderer, id<MTL4ComputeCommandEncoder> encoder,
    id<MTLComputePipelineState> pipeline) API_AVAILABLE(macos(26.0)) {
  [encoder setComputePipelineState:pipeline];
  vkr_metal_packet_count_pipeline(renderer, (const void *)pipeline);
}

/* At a frame's commit: its counts, the graph's dependencies included, and
   a fresh count for the next frame. */
vkr_internal void
vkr_metal_packet_commit_frame_counters(VkrMetalPacketRenderer *renderer,
                                       uint64_t frame_index,
                                       uint32_t dependency_count) {
  renderer->frame_counters.values[VKR_BACKEND_COUNTER_BARRIERS] +=
      dependency_count;
  renderer->submitted_counters = renderer->frame_counters;
  renderer->submitted_counters.frame_index = frame_index;
  renderer->submitted_counters.supported_mask = VKR_METAL_PACKET_COUNTED_MASK;
  renderer->submitted_counters.valid = true_v;
  renderer->frame_counters = (VkrBackendFrameCounters){0};
}

#include "metal/internal/vkr_metal_packet_graph.inc"
/* The presenter (vkr_metal_packet_present.inc) records with the frame's
   helpers, so it follows them; setup and pass preparation call into it. */
vkr_internal bool8_t
vkr_metal_packet_presenter_create(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0));
vkr_internal bool8_t vkr_metal_packet_prepare_editor_present(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    VkrMetalPacketPreparedPass *prepared) API_AVAILABLE(macos(26.0));
vkr_internal void
vkr_metal_packet_order_frame_commit(VkrMetalPacketRenderer *renderer,
                                    const VkrPreparedFrame *packet)
    API_AVAILABLE(macos(26.0));
vkr_internal void
vkr_metal_packet_presenter_frame_committed(VkrMetalPacketRenderer *renderer,
                                           const VkrPreparedFrame *packet,
                                           uint64_t submit_value);
vkr_internal void
vkr_metal_packet_present_committed_frame(VkrMetalPacketRenderer *renderer,
                                         const VkrPreparedFrame *packet,
                                         uint64_t submit_value)
    API_AVAILABLE(macos(26.0));
/* SDF sun shadows (vkr_metal_packet_sdf.inc) follow the frame's helpers;
   upload and pass preparation call into them. */
vkr_internal float32_t
vkr_metal_packet_sun_shadow_start(const VkrShadowPassPayload *shadow);
vkr_internal uint64_t
vkr_metal_packet_sdf_root_cells(const VkrMetalPacketRenderer *renderer);
vkr_internal bool8_t
vkr_metal_packet_sdf_layout(VkrMetalPacketRenderer *renderer);
vkr_internal bool8_t
vkr_metal_packet_write_upload_sdf_table(VkrMetalPacketRenderer *renderer,
                                        VkrMetalPacketFrameUpload *upload);
vkr_internal uint32_t
vkr_metal_packet_sdf_skip_view_mask(const VkrRenderGraphFrameInfo *frame);
vkr_internal void
vkr_metal_packet_sdf_upload_cooked(VkrMetalPacketRenderer *renderer,
                                   VkrMetalPacketMesh *mesh,
                                   const VkrGeometryDistanceVolume *cooked)
    API_AVAILABLE(macos(26.0));
vkr_internal bool8_t vkr_metal_packet_prepare_tiled_sun_shadow(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    const VkrPreparedFrame *packet, VkrMetalPacketPreparedPass *prepared,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0));
#include "metal/internal/vkr_metal_packet_commands.inc"
#include "metal/internal/vkr_metal_packet_setup.inc"
#include "metal/internal/vkr_metal_packet_custom.inc"
#include "metal/internal/vkr_metal_packet_resources.inc"
#include "metal/internal/vkr_metal_packet_animation_preview.inc"
#include "metal/internal/vkr_metal_packet_frame.inc"
#include "metal/internal/vkr_metal_packet_sdf.inc"
#include "metal/internal/vkr_metal_packet_present.inc"
#include "metal/internal/vkr_metal_packet_lifecycle.inc"
// clang-format on

VkrRendererError vkr_metal_packet_renderer_get_pixel_readback_result(
    VkrMetalPacketRenderer *renderer, VkrPixelReadbackResult *out_result) {
  if (!renderer || !out_result)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  vkr_metal_packet_collect_picking_results(renderer,
                                           renderer->completion.signaledValue);
  *out_result = renderer->picking_result;
  if (out_result->status == VKR_READBACK_STATUS_READY)
    renderer->picking_result =
        (VkrPixelReadbackResult){.status = VKR_READBACK_STATUS_IDLE};
  return VKR_RENDERER_ERROR_NONE;
}

String8
vkr_metal_packet_renderer_device_name(const VkrMetalPacketRenderer *renderer) {
  return string8_create_from_cstr((const uint8_t *)renderer->device_name,
                                  string_length(renderer->device_name));
}

void vkr_metal_packet_renderer_hardware(const VkrMetalPacketRenderer *renderer,
                                        VkrDeviceInformation *info) {
  if (!renderer || !renderer->device || !info) {
    return;
  }
  if (@available(macOS 14.0, *)) {
    NSString *architecture = renderer->device.architecture.name;
    string_format(info->gpu_architecture, sizeof(info->gpu_architecture), "%s",
                  architecture ? architecture.UTF8String : "");
  }
  info->unified_memory = renderer->device.hasUnifiedMemory ? true_v : false_v;
  info->working_set_bytes = renderer->device.recommendedMaxWorkingSetSize;
  info->max_threadgroup_memory =
      (uint32_t)renderer->device.maxThreadgroupMemoryLength;
  info->max_threads_per_threadgroup =
      (uint32_t)renderer->device.maxThreadsPerThreadgroup.width;
  /* Every Apple GPU runs 32-thread SIMD groups. */
  info->simd_width_min = 32u;
  info->simd_width_max = 32u;
}

VkrPresentMode
vkr_metal_packet_renderer_present_mode(const VkrMetalPacketRenderer *renderer) {
  return renderer ? renderer->actual_present_mode : VKR_PRESENT_MODE_DEFAULT;
}

const VkrRenderGraph *
vkr_metal_packet_renderer_graph(const VkrMetalPacketRenderer *renderer) {
  return renderer ? renderer->graph : NULL;
}

const VkrDrawCensusStore *
vkr_metal_packet_renderer_draw_census(const VkrMetalPacketRenderer *renderer) {
  return renderer ? &renderer->draw_census : NULL;
}

uint32_t
vkr_metal_packet_renderer_take_texture_demand(VkrMetalPacketRenderer *renderer,
                                              VkrTextureDemand *out,
                                              uint32_t capacity) {
  if (!renderer || !out || !renderer->texture_demand)
    return 0u;
  const uint32_t count = Min(renderer->texture_demand_count, capacity);
  MemCopy(out, renderer->texture_demand, (uint64_t)count * sizeof(*out));
  renderer->texture_demand_count = 0u;
  return count;
}

void vkr_metal_packet_renderer_frame_counters(
    const VkrMetalPacketRenderer *renderer,
    VkrBackendFrameCounters *out_counters) {
  *out_counters =
      renderer ? renderer->submitted_counters : (VkrBackendFrameCounters){0};
}

bool8_t vkr_metal_packet_renderer_graph_resource_stats(
    const VkrMetalPacketRenderer *renderer,
    VkrRenderGraphResourceStats *out_stats) {
  if (!renderer || !out_stats)
    return false_v;
  MemZero(out_stats, sizeof(*out_stats));
  for (uint32_t i = 0u; i < renderer->max_images; ++i) {
    const VkrMetalPacketImage *image = &renderer->images[i];
    if (!image->live || image->external)
      continue;
    vkr_render_graph_resource_stats_add_image(out_stats, &image->desc,
                                              image->instance_count);
  }
  for (uint32_t i = 0u; i < renderer->max_images; ++i) {
    const VkrMetalPacketGraphBuffer *buffer = &renderer->graph_buffers[i];
    if (!buffer->live)
      continue;
    vkr_render_graph_resource_stats_add_buffer(out_stats, &buffer->desc,
                                               buffer->instance_count);
  }
  out_stats->peak_image_textures = out_stats->live_image_textures;
  out_stats->peak_image_bytes = out_stats->live_image_bytes;
  out_stats->peak_buffers = out_stats->live_buffers;
  out_stats->peak_buffer_bytes = out_stats->live_buffer_bytes;
  return true_v;
}

#endif

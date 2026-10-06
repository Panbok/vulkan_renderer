#include "metal/vkr_metal_packet_renderer.h"
#include "vkr_packed_geometry.h"
#include "vkr_texture_upload.h"

#if defined(PLATFORM_APPLE)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <simd/simd.h>

#include <Block.h>
#include <dispatch/dispatch.h>
#include <float.h>

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "math/vkr_frustum.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

vkr_internal uint64_t vkr_metal_packet_align_up(uint64_t value,
                                                uint64_t alignment);

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

typedef struct VkrMetalPacketMesh {
  VkrMetalBufferResource vertices;
  VkrMetalBufferResource indices;
  VkrGpuGeometryRow gpu_row;
  VkrGeometryRangeAllocation ranges;
  VkrMetalPacketSubmeshCreateInfo *submeshes;
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
  uint64_t ibl_probes_gpu;
  uint64_t diffuse_volume_texture_id;
  uint64_t diffuse_volume_params_gpu;
  /* This frame's VkrMetalPacketLightmap, written for every frame. */
  uint64_t lightmap_gpu;
  uint64_t ltc_gpu;
  uint64_t sheen_gpu;
  uint64_t fog_gpu;
  uint64_t sky_gpu;
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
  uint64_t deferred_diagnostics;
  uint64_t deferred_diagnostics_size;
  uint64_t exposure;
  uint64_t exposure_size;
  uint64_t prefix_size;
} VkrMetalPacketReadbackLayout;

vkr_internal VkrMetalPacketReadbackLayout
vkr_metal_packet_readback_layout(bool8_t deferred_diagnostics, bool8_t exposure,
                                 uint32_t gpu_draw_view_count) {
  /* The final target can be RGBA8 or RGBA16F. Reserve an eight-byte pixel
     before the fixed diagnostic fields so the transfer stays valid across an
     extended-linear target transition. */
  const uint64_t shadow_depth_offset = 8u;
  const uint64_t picking_offset = shadow_depth_offset + sizeof(float32_t);
  /* The picking readback copies the picked pixel's resolved depth right
     after the object id (vkr_metal_packet_prepare_picking_readback). */
  const uint64_t picking_depth_offset = picking_offset + sizeof(uint32_t);
  const uint64_t fixed_size = 32u;
  const uint64_t deferred_offset = vkr_metal_packet_align_up(fixed_size, 16u);
  const uint64_t deferred_bytes =
      deferred_diagnostics
          ? (uint64_t)gpu_draw_view_count * sizeof(VkrGpuDrawCompactionState)
          : 0u;
  const uint64_t exposure_offset =
      vkr_metal_packet_align_up(deferred_offset + deferred_bytes, 16u);
  const uint64_t exposure_bytes =
      exposure ? sizeof(VkrExposureGpuState) + sizeof(VkrExposureGpuHistogram)
               : 0u;
  const uint64_t prefix_size =
      exposure_bytes > 0u    ? exposure_offset + exposure_bytes
      : deferred_diagnostics ? deferred_offset + deferred_bytes
                             : fixed_size;
  return (VkrMetalPacketReadbackLayout){
      .shadow_depth = shadow_depth_offset,
      .picking = picking_offset,
      .picking_depth = picking_depth_offset,
      .deferred_diagnostics = deferred_offset,
      .deferred_diagnostics_size = deferred_bytes,
      .exposure = exposure_offset,
      .exposure_size = exposure_bytes,
      .prefix_size = prefix_size,
  };
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
  bool8_t has_error;
  VkrAtomicBool feedback_ready;
  VkrAtomicBool result_ready;
  bool8_t in_use;
} VkrMetalPacketCommitFeedbackRecord;

typedef struct VkrMetalPacketCommandSlot {
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
  uint32_t shadow_cascade_count;
  uint32_t gpu_draw_view_count;
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
  VKR_METAL_TILED_LIGHTING_COUNT = 5,
} VkrMetalTiledLighting;

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
  /* Active-slot aliases keep encoding code independent of slot selection. */
  id<MTL4CommandAllocator> command_allocator;
  id<MTL4CommandBuffer> command_buffer;
  VkrMetalPacketTextureUploadBatch texture_upload_batch;
  uint64_t timestamp_frequency;
  id<MTLRenderPipelineState> gpu_shadow_pipeline;
  id<MTLRenderPipelineState> gpu_shadow_opaque_pipeline;
  id<MTLRenderPipelineState> depth_clear_pipeline;
  /* The tiled pipeline (ADR-087) with VKR_METAL_TILED_SAMPLE_COUNT samples: a
     depth pre-pass, forward shading and the clear sky draw into memoryless
     multisampled targets that resolve into the graph's colour and depth, and
     the cloud draw over the resolved image. */
  id<MTLRenderPipelineState> tiled_depth_pipeline;
  /* Opaque draws, which never discard, and alpha-tested draws, which write
     the samples their alpha covers. */
  id<MTLRenderPipelineState>
      tiled_forward_pipelines[VKR_METAL_TILED_LIGHTING_COUNT];
  id<MTLRenderPipelineState>
      tiled_coverage_pipelines[VKR_METAL_TILED_LIGHTING_COUNT];
  id<MTLRenderPipelineState> tiled_sky_pipeline;
  /* Tone-mapped multisample resolve at the end of the opaque pass. */
  id<MTLRenderPipelineState> tiled_resolve_pipeline;
  id<MTLRenderPipelineState> tiled_atmosphere_pipeline;
  /* Object ids of the camera's opaque draws, single-sampled, for a pick. */
  id<MTLRenderPipelineState> tiled_picking_pipeline;
  /* Glass and blended surfaces, and world text, over the resolved image. */
  id<MTLRenderPipelineState>
      tiled_blend_pipelines[VKR_METAL_TILED_LIGHTING_COUNT];
  id<MTLRenderPipelineState> tiled_text_pipeline;
  id<MTLDepthStencilState> tiled_prepass_state;
  id<MTLDepthStencilState> tiled_shade_state;
  id<MTLDepthStencilState> tiled_sky_state;
  id<MTLTexture> tiled_msaa_color;
  id<MTLTexture> tiled_msaa_depth;
  uint32_t tiled_width;
  uint32_t tiled_height;
  /* Targets a resize replaced, released once the GPU completes
     `tiled_retired_submit_value`, the last submission that drew into them. */
  id<MTLTexture> tiled_retired_color;
  id<MTLTexture> tiled_retired_depth;
  uint64_t tiled_retired_submit_value;
  id<MTLRenderPipelineState> ui_pipeline;
  id<MTLRenderPipelineState> picking_pipeline;
  id<MTLRenderPipelineState> editor_overlay_pipeline;
  id<MTLRenderPipelineState> editor_overlay_picking_pipeline;
  /* Selection outline: overlay geometry into an R8 mask, then a full-screen
   * edge pass over the Scene image. */
  id<MTLRenderPipelineState> editor_selection_mask_pipeline;
  id<MTLRenderPipelineState> editor_selection_outline_pipeline;
  id<MTLRenderPipelineState> editor_grid_pipeline;
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
  id<MTLComputePipelineState> cloud_sky_light_pipeline;
  id<MTLComputePipelineState> cloud_sky_light_sh_pipeline;
  id<MTLComputePipelineState> cloud_sky_light_mips_pipeline;
  id<MTLComputePipelineState> gpu_draw_classify_pipeline;
  id<MTLComputePipelineState> gpu_draw_prefix_pipeline;
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
  id<MTLComputePipelineState> bloom_prefilter_pipeline;
  /* Both filters are resident; the cold configuration selects which one a
     build measures. Neither is a fallback for the other. */
  id<MTLComputePipelineState> bloom_downsample_tent13_pipeline;
  id<MTLComputePipelineState> bloom_downsample_box4_pipeline;
  id<MTLComputePipelineState> bloom_upsample_pipeline;
  id<MTLArgumentEncoder> gpu_draw_icb_argument_encoder;
  id<MTLDepthStencilState> depth_write_state;
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
  uint64_t candidate_publication_generation;
  uint32_t gpu_draw_count;
  uint64_t current_hzb_world_epoch;
  Mat4 current_hzb_view_projection;
  Mat4 current_hzb_raster_view_projection;
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

#include "metal/internal/vkr_metal_packet_graph.inc"
#include "metal/internal/vkr_metal_packet_commands.inc"
#include "metal/internal/vkr_metal_packet_setup.inc"
#include "metal/internal/vkr_metal_packet_resources.inc"
#include "metal/internal/vkr_metal_packet_animation_preview.inc"
#include "metal/internal/vkr_metal_packet_frame.inc"
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
                                  strlen(renderer->device_name));
}

VkrPresentMode
vkr_metal_packet_renderer_present_mode(const VkrMetalPacketRenderer *renderer) {
  return renderer ? renderer->actual_present_mode : VKR_PRESENT_MODE_DEFAULT;
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

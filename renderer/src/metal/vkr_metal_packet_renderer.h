#pragma once

#include "metal/vkr_metal_material_table.h"
#include "metal/vkr_metal_memory_device.h"
#include "vkr_asset_publisher.h"
#include "vkr_atmosphere.h"
#include "vkr_bloom.h"
#include "vkr_buffer.h"
#include "vkr_display_output.h"
#include "vkr_gpu_abi.h"
#include "vkr_ibl_math.h"
#include "vkr_prepared_frame.h"
#include "vkr_render_graph.h"
#include "vkr_renderer_impl.h"

typedef struct VkrMetalPacketRenderer VkrMetalPacketRenderer;
struct VkrGeometryUpload;
struct VkrTexturePreparedLoad;
struct VkrMaterial;

/** Nonblocking, newest submitted picking request; READY is consumed once. */
VkrRendererError vkr_metal_packet_renderer_get_pixel_readback_result(
    VkrMetalPacketRenderer *renderer, VkrPixelReadbackResult *out_result);

/** Custom material graphs whose pipelines are still being created, and the
    late Custom draws of every completed frame so far (ADR-096). Atomic
    reads, safe beside the render thread. */
/** Asks the next frame preparation to load the project library again after
    `vkr_bakery materials` rewrote it, and to recreate every Custom graph's
    pipelines in the background. That frame waits for the GPU first: an
    authoring action, not a frame operation. Safe from any thread. */
void vkr_metal_packet_renderer_request_custom_reload(
    VkrMetalPacketRenderer *renderer);

void vkr_metal_packet_renderer_pipeline_stats(
    const VkrMetalPacketRenderer *renderer, uint32_t *out_pending_graphs,
    uint64_t *out_late_draws);

void vkr_metal_packet_renderer_geometry_megabuffer_metrics(
    const VkrMetalPacketRenderer *renderer,
    VkrGeometryMegabufferMetrics *out_metrics);

/** Private upload view used while publishing one decoded texture. */
typedef struct VkrMetalPacketTextureUpload {
  const uint8_t *data;
  uint64_t data_size;
  uint32_t mip_levels;
  uint32_t array_layers;
  bool8_t is_compressed;
  uint32_t region_count;
  const VkrTextureUploadRegion *regions;
} VkrMetalPacketTextureUpload;

typedef enum VkrMetalPacketTargetKind {
  VKR_METAL_PACKET_TARGET_OFFSCREEN = 0,
  VKR_METAL_PACKET_TARGET_WINDOW,
} VkrMetalPacketTargetKind;

/** Fixed capacities and authored graph used by the Metal packet renderer. */
typedef struct VkrMetalPacketRendererConfig {
  VkrAllocator *allocator;
  /** A zeroed record selects the production bloom defaults. */
  VkrBloomConfig bloom;
  const char *graph_path;
  const char *slang_msl_path;
  const char *fragment_msl_path;
  /** Optional precompiled libraries for the two MSL sources above. When set,
   * each is loaded instead of compiling its source; the source path remains
   * the fallback if loading fails. */
  const char *slang_metallib_path;
  const char *fragment_metallib_path;
  /** Optional Metal 4 archive path used for cold capture and warm lookup. */
  const char *pipeline_archive_path;
  /** Optional project library of Custom material graphs (ADR-096). */
  const char *custom_library_path;
  VkrMetalPacketTargetKind target_kind;
  uint32_t target_width;
  uint32_t target_height;
  /** The tiled pipeline steps the resolution of its spatial upscale
   * (ADR-087); frames then report their GPU time. */
  VkrDynamicResolutionConfig dynamic_resolution;
  /** Borrowed CAMetalLayer pointer; required only for WINDOW. */
  void *metal_layer;
  /** Window presentation policy; offscreen targets always resolve to SDR. */
  VkrDisplayOutputMode display_output_mode;
  /** Borrowed context and optional platform snapshot callback for window
   * output. */
  void *display_output_context;
  VkrDisplayOutputSnapshot (*display_output_snapshot)(void *context);
  /** Requested window presentation policy; offscreen targets ignore it. */
  VkrPresentMode requested_present_mode;
  /** Managed native heaps, transfer rings and explicit buffers/ICBs. Zero
   * selects two thirds of the device's recommended working set. */
  uint64_t managed_budget_size;
  uint64_t heap_chunk_size;
  uint64_t upload_ring_size;
  uint64_t upload_ring_max_size;
  /** At least two slots: one acquired frame and one completion-protected upload
   * slot. */
  uint32_t frame_slot_count;
  /** Request-owned capture results retained until explicit release. */
  uint32_t capture_ring_capacity;
  uint64_t capture_max_batch_bytes;
  /** Focused evidence only; production submission leaves readback asynchronous.
   */
  bool8_t synchronous_validation_readback;
  /** Production output writes an sRGB present target. */
  bool8_t srgb_output;
  /** Applies ACES in the fullscreen post stage independently of target format.
   */
  bool8_t tonemap_enabled;
  /** Converts the shared Vulkan-oriented clip-Y matrices for Metal raster. */
  bool8_t convert_vulkan_clip_y;
  /** Diagnostic rollback for P14 while retaining the graph. */
  bool8_t hzb_enabled;
  bool8_t frustum_enabled;
  uint32_t max_images;
  uint32_t max_passes;
  uint32_t max_material_rows;
  uint32_t max_meshes;
  uint32_t max_textures;
  uint32_t max_draws;
  uint32_t max_instances;
} VkrMetalPacketRendererConfig;

/** CPU-decoded indexed geometry copied into private placement buffers. */
typedef struct VkrMetalPacketSubmeshCreateInfo {
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t decode_index;
} VkrMetalPacketSubmeshCreateInfo;

typedef struct VkrMetalPacketMeshCreateInfo {
  const VkrPackedStaticVertex *vertices;
  uint32_t vertex_count;
  const VkrGpuGeometryDecodeRecord *decodes;
  uint32_t decode_count;
  const uint32_t *indices;
  uint32_t index_count;
  const VkrMetalPacketSubmeshCreateInfo *submeshes;
  uint32_t submesh_count;
  /** Indices of the one default range when `submesh_count` is zero; zero
      draws them all. */
  uint32_t default_index_count;
} VkrMetalPacketMeshCreateInfo;

/** Focused immutable material publication input for the Metal GPU table. */
typedef enum VkrMetalPacketMaterialTextureFlag {
  VKR_METAL_PACKET_MATERIAL_TEXTURE_NORMAL = 1u << 0u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_ORM = 1u << 1u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_EMISSIVE = 1u << 2u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_TRANSMISSION = 1u << 3u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_THICKNESS = 1u << 4u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_CLEARCOAT = 1u << 5u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_CLEARCOAT_ROUGHNESS = 1u << 6u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_CLEARCOAT_NORMAL = 1u << 7u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_SHEEN_COLOR = 1u << 8u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_SHEEN_ROUGHNESS = 1u << 9u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_ANISOTROPY = 1u << 10u,
  /* A terrain material: the vertex color weighs layer 0 against the terrain
     segment's layers 1 to 3. */
  VKR_METAL_PACKET_MATERIAL_TERRAIN = 1u << 11u,
  /* A rough surface (VkrMaterial::rough): thin glass with it blurs through
     the tiled pipeline's refraction copy. */
  VKR_METAL_PACKET_MATERIAL_ROUGH = 1u << 12u,
} VkrMetalPacketMaterialTextureFlag;

/* A Custom material's graph slot (1 to VKR_METAL_CUSTOM_GRAPH_SLOTS - 1)
   rides in its row flags above the feature bits; 0 is the Standard tier.
   Until the frame's ready mask holds the slot, the material draws its
   Standard fallback as a late draw (ADR-096). */
#define VKR_METAL_PACKET_MATERIAL_CUSTOM_SHIFT 24u
#define VKR_METAL_PACKET_MATERIAL_CUSTOM_MASK 0x3Fu

/* Texture references a published material tracks: the twelve common-row and
   transmission textures, three per extra terrain layer, the layer mask,
   then the eight Custom-graph textures. */
#define VKR_METAL_PACKET_MATERIAL_TEXTURE_COUNT 30u

typedef struct VkrMetalPacketRgba8TextureCreateInfo {
  const uint8_t *pixels;
  uint32_t width;
  uint32_t height;
} VkrMetalPacketRgba8TextureCreateInfo;

typedef struct VkrMetalPacketMaterialCreateInfo {
  float32_t tint[4];
  VkrMetalPacketRgba8TextureCreateInfo textures[12];
  uint32_t material_id;
  uint32_t texture_flags;
  VkrPbrProperties pbr;
  VkrMaterialAlphaMode alpha_mode;
  float32_t alpha_cutoff;
} VkrMetalPacketMaterialCreateInfo;

enum {
  VKR_METAL_PACKET_TIMING_NAME_CAPACITY = 64,
  VKR_METAL_PACKET_MAX_PASS_TIMINGS = VKR_RENDERER_IMPL_MAX_PASS_TIMINGS,
};

/** Completed Metal timestamp interval for one authored graph pass. */
typedef struct VkrMetalPacketPassTiming {
  char name[VKR_METAL_PACKET_TIMING_NAME_CAPACITY];
  float64_t cpu_ms;
  float64_t gpu_ms;
  uint32_t pass_index;
  bool8_t valid;
  /** Graphics passes stamp their start after the vertex stage, which can run
   * while earlier encoders finish; their interval starts no earlier than the
   * previous timed pass's end. */
  bool8_t starts_after_previous;
  VkrRendererImplGpuTimingReason unavailable_reason;
} VkrMetalPacketPassTiming;

/** CPU-visible evidence returned after the requested readbacks complete. */
typedef struct VkrMetalPacketResult {
  VkrRendererError error;
  uint64_t submit_value;
  uint64_t source_frame_index;
  uint64_t gpu_submission_ns;
  float32_t source_render_scale;
  uint32_t source_render_width;
  uint32_t source_render_height;
  VkrRendererImplGpuTimingReason gpu_submission_unavailable_reason;
  bool8_t gpu_submission_valid;
  uint32_t executed_pass_count;
  uint32_t graphics_pass_count;
  uint32_t compute_pass_count;
  uint32_t transfer_pass_count;
  uint32_t dependency_count;
  uint32_t indexed_draw_count;
  uint32_t shadow_draw_count;
  uint32_t opaque_draw_count;
  uint32_t blend_draw_count;
  uint32_t ui_draw_count;
  uint32_t text_draw_count;
  uint32_t ibl_dispatch_count;
  uint32_t gpu_candidate_count;
  uint32_t gpu_visible_count;
  uint32_t gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t gpu_overflow_count;
  uint32_t gpu_resolve_invalid_count;
  uint32_t gpu_occlusion_culled_count;
  /* Camera draws of Custom materials that took their Standard fallback
     because their graph's pipelines did not exist yet (ADR-096). */
  uint32_t custom_late_count;
  uint32_t shadow_gpu_visible_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_gpu_bucket_counts[VKR_SHADOW_CASCADE_COUNT_MAX]
                                   [VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t shadow_gpu_overflow_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  bool8_t hzb_history_valid;
  bool8_t has_gpu_draw_diagnostics;
  VkrExposureDebugSample exposure;
  uint32_t resize_count;
  uint8_t color[4];
  float32_t shadow_depth;
  bool8_t has_shadow_depth;
  uint32_t picking_id;
  bool8_t has_picking_id;
  uint32_t pass_timing_count;
  VkrMetalPacketPassTiming pass_timings[VKR_METAL_PACKET_MAX_PASS_TIMINGS];
  bool8_t pipeline_archive_warm;
  bool8_t pipeline_archive_written;
  /** Snapshot of the request-owned poll result, if this packet captured. */
  VkrCapturePollResult capture;
  VkrMetalMemoryDeviceMetrics memory;
  VkrMetalMaterialTableMetrics materials;
  /** Same-frame packet-lowering CPU cost, matching the Vulkan name set. */
  VkrPacketBuildMetrics packet_build;
} VkrMetalPacketResult;

bool8_t
vkr_metal_packet_renderer_create(const VkrMetalPacketRendererConfig *config,
                                 VkrMetalPacketRenderer **out_renderer);

/** Publishes immutable asset records. These calls are not frame-hot APIs. */
bool8_t vkr_metal_packet_renderer_create_mesh(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketMeshCreateInfo *create_info, VkrMeshHandle *out_handle);
/** Publishes the shared loader's merged buffer and submesh ranges directly. */
bool8_t vkr_metal_packet_renderer_create_loaded_mesh(
    VkrMetalPacketRenderer *renderer,
    const struct VkrGeometryUpload *loader_result, VkrMeshHandle *out_handle);
/** Publishes loader geometry under an existing shared mesh handle. */
bool8_t vkr_metal_packet_renderer_publish_loaded_mesh(
    VkrMetalPacketRenderer *renderer, VkrGeometryHandle handle,
    const struct VkrGeometryUpload *loader_result);
/** Publishes one non-merged geometry under its shared geometry handle. */
bool8_t vkr_metal_packet_renderer_publish_geometry(
    VkrMetalPacketRenderer *renderer, VkrGeometryHandle handle,
    const struct VkrGeometryConfig *geometry);
bool8_t vkr_metal_packet_renderer_destroy_mesh(VkrMetalPacketRenderer *renderer,
                                               VkrMeshHandle handle);
bool8_t vkr_metal_packet_renderer_create_material(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketMaterialCreateInfo *create_info,
    VkrMaterialHandle *out_handle);
/** Publishes a shared material under its existing generation handle. */
bool8_t
vkr_metal_packet_renderer_publish_material(VkrMetalPacketRenderer *renderer,
                                           VkrMaterialHandle handle,
                                           const struct VkrMaterial *material);
bool8_t
vkr_metal_packet_renderer_destroy_material(VkrMetalPacketRenderer *renderer,
                                           VkrMaterialHandle handle);
bool8_t vkr_metal_packet_renderer_create_rgba8_texture(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketRgba8TextureCreateInfo *create_info,
    VkrTextureHandle *out_handle);
/** Vertical sync on (FIFO) or off (IMMEDIATE) for the window's layer; false
 * for an offscreen target. */
bool8_t
vkr_metal_packet_renderer_set_present_mode(VkrMetalPacketRenderer *renderer,
                                           VkrPresentMode mode);
/** Dynamic resolution needs completed submission timing every frame. */
void vkr_metal_packet_renderer_set_dynamic_resolution(
    VkrMetalPacketRenderer *renderer, bool8_t enabled);
/** Authorizes completion-gated retirement before a smaller graph realization.
 */
void vkr_metal_packet_renderer_request_memory_relief(
    VkrMetalPacketRenderer *renderer);

bool8_t vkr_metal_packet_renderer_begin_texture_upload_batch(
    VkrMetalPacketRenderer *renderer);
bool8_t vkr_metal_packet_renderer_end_texture_upload_batch(
    VkrMetalPacketRenderer *renderer);

/** Publishes a shared decoder payload under its existing texture handle. */
VkrRendererError vkr_metal_packet_renderer_publish_prepared_texture(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle handle,
    const struct VkrTexturePreparedLoad *prepared);
bool8_t vkr_metal_packet_renderer_publish_writable_texture(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle handle,
    const VkrTextureDescription *description);
/** Updates the sampler selected by subsequently published material rows. */
bool8_t vkr_metal_packet_renderer_update_texture_sampler(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle handle,
    const VkrTextureDescription *description);
bool8_t vkr_metal_packet_renderer_bake_ibl_cubemap(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle source,
    VkrTextureHandle prefilter, float32_t sh_deringing);
/** Queues one source-owned atmosphere candidate that also writes its own
 * lookup textures. Query completion separately; this never blocks ordinary
 * publication polling. */
bool8_t vkr_metal_packet_renderer_bake_atmosphere(
    VkrMetalPacketRenderer *renderer, const VkrAtmosphereGpuParams *params,
    VkrTextureHandle source, VkrTextureHandle prefilter,
    VkrTextureHandle transmittance, VkrTextureHandle multiple_scattering,
    float32_t sh_deringing);
VkrAtmosphereBakeStatus vkr_metal_packet_renderer_atmosphere_bake_status(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle source);
bool8_t
vkr_metal_packet_renderer_destroy_texture(VkrMetalPacketRenderer *renderer,
                                          VkrTextureHandle handle);

/**
 * Establishes the target dimensions and frame conditions for the next packet.
 * Selection is coarse: submit contains no backend-type branch or callback
 * through the Vulkan-shaped backend interface.
 */
/** Refreshes the platform snapshot before target graph realization. Returns
 * false only when a completion-safe layer/presentation-pipeline transition
 * fails. */
bool8_t vkr_metal_packet_renderer_refresh_display_output(
    VkrMetalPacketRenderer *renderer, bool8_t *out_format_changed);
/** Current native present attachment format after the most recent refresh. */
VkrTextureFormat vkr_metal_packet_renderer_present_color_format(
    const VkrMetalPacketRenderer *renderer);
/** Current shader-visible output transfer parameters. */
VkrDisplayOutputParams vkr_metal_packet_renderer_display_output(
    const VkrMetalPacketRenderer *renderer);

bool8_t vkr_metal_packet_renderer_prepare_frame(
    VkrMetalPacketRenderer *renderer,
    const VkrRenderGraphFrameInfo *frame_info);
uint32_t vkr_metal_packet_renderer_frame_image_index(
    const VkrMetalPacketRenderer *renderer);
/** Returns the extent only when the retained editor image has committed
 * contents. */
void vkr_metal_packet_renderer_retained_editor_extent(
    VkrMetalPacketRenderer *renderer, uint32_t *out_width,
    uint32_t *out_height);
void vkr_metal_packet_renderer_retained_shadow_token(
    VkrMetalPacketRenderer *renderer, uint32_t image_index,
    VkrRetainedShadowToken *out_token);
void vkr_metal_packet_renderer_retained_local_shadow_token(
    VkrMetalPacketRenderer *renderer, VkrRetainedLocalShadowToken *out_token);

/** Releases a prepared drawable and command slot that were not submitted. */
bool8_t
vkr_metal_packet_renderer_cancel_frame(VkrMetalPacketRenderer *renderer);

/**
 * Builds and schedules the authored graph, realizes its Metal resources, and
 * executes one packet. Registered meshes/materials are resolved from numeric
 * packet handles by the Metal-private pass executors.
 */
bool8_t
vkr_metal_packet_renderer_submit_packet(VkrMetalPacketRenderer *renderer,
                                        const VkrPreparedFrame *packet,
                                        VkrMetalPacketResult *out_result);

/** Polls a bounded capture request without waiting for GPU completion. */
VkrCaptureStatus
vkr_metal_packet_renderer_capture_poll(VkrMetalPacketRenderer *renderer,
                                       VkrCaptureRequestId request_id,
                                       VkrCapturePollResult *out_result);

/** Copies retained intervals for one completed asynchronous submit. */
bool8_t vkr_metal_packet_renderer_pass_timings_poll(
    VkrMetalPacketRenderer *renderer, uint64_t submit_value,
    VkrMetalPacketPassTiming *out_timings, uint32_t capacity,
    uint32_t *out_count);

/** Copies the oldest retained completion after `after_submit_value`. */
bool8_t vkr_metal_packet_renderer_submit_result_poll_next(
    VkrMetalPacketRenderer *renderer, uint64_t after_submit_value,
    VkrMetalPacketResult *out_result);

/** Waits for submitted work without retiring live assets. */
bool8_t vkr_metal_packet_renderer_wait_idle(VkrMetalPacketRenderer *renderer);
uint64_t
vkr_metal_packet_renderer_submit_value(const VkrMetalPacketRenderer *renderer);
uint64_t vkr_metal_packet_renderer_completed_value(
    const VkrMetalPacketRenderer *renderer);
/** Borrowed UTF-8 model name; storage lives until renderer destruction. */
String8
vkr_metal_packet_renderer_device_name(const VkrMetalPacketRenderer *renderer);
/** Returns the presentation mode actually applied to the Metal target. */
VkrPresentMode
vkr_metal_packet_renderer_present_mode(const VkrMetalPacketRenderer *renderer);
bool8_t vkr_metal_packet_renderer_graph_resource_stats(
    const VkrMetalPacketRenderer *renderer,
    VkrRenderGraphResourceStats *out_stats);
bool8_t vkr_metal_packet_renderer_get_memory_metrics(
    const VkrMetalPacketRenderer *renderer,
    VkrMetalMemoryDeviceMetrics *out_metrics);

bool8_t vkr_metal_packet_renderer_get_and_reset_upload_wait_count(
    VkrMetalPacketRenderer *renderer, uint64_t *out_wait_count);
bool8_t vkr_metal_packet_renderer_get_and_reset_command_slot_wait_count(
    VkrMetalPacketRenderer *renderer, uint64_t *out_wait_count);

/** Releases request-owned capture storage, including a pending request. */
bool8_t
vkr_metal_packet_renderer_capture_release(VkrMetalPacketRenderer *renderer,
                                          VkrCaptureRequestId request_id);

/** Retires every cached GPU object at the completed submission boundary. */
bool8_t vkr_metal_packet_renderer_drain(
    VkrMetalPacketRenderer *renderer,
    VkrMetalMemoryDeviceMetrics *out_memory_metrics,
    VkrMetalMaterialTableMetrics *out_material_metrics);

void vkr_metal_packet_renderer_destroy(VkrMetalPacketRenderer *renderer);

/** Fills the coarse shared-loader publication seam for this renderer. */
void vkr_metal_packet_renderer_get_asset_publisher(
    VkrMetalPacketRenderer *renderer, VkrAssetPublisher *out_publisher);

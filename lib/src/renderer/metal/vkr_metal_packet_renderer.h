#pragma once

#include "renderer/metal/vkr_metal_material_table.h"
#include "renderer/metal/vkr_metal_memory_device.h"
#include "renderer/vkr_asset_publisher.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_graph.h"
#include "renderer/vkr_render_packet.h"

typedef struct VkrMetalPacketRenderer VkrMetalPacketRenderer;
struct VkrMeshLoaderResult;
struct VkrTexturePreparedLoad;
struct VkrMaterial;

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
  const char *graph_path;
  const char *slang_msl_path;
  const char *fragment_msl_path;
  /** Optional Metal 4 archive path used for cold capture and warm lookup. */
  const char *pipeline_archive_path;
  VkrMetalPacketTargetKind target_kind;
  /** Borrowed CAMetalLayer pointer; required only for WINDOW. */
  void *metal_layer;
  uint64_t heap_size;
  uint64_t upload_ring_size;
  uint64_t readback_ring_size;
  uint32_t frame_slot_count;
  /** Request-owned capture results retained until explicit release. */
  uint32_t capture_ring_capacity;
  uint64_t capture_max_batch_bytes;
  /** Focused evidence only; production submission leaves readback asynchronous.
   */
  bool8_t synchronous_validation_readback;
  /** Production output applies ACES and writes an sRGB present target. */
  bool8_t srgb_output;
  /** Converts the shared Vulkan-oriented clip-Y matrices for Metal raster. */
  bool8_t convert_vulkan_clip_y;
  /** Enables the provisional Metal-only deferred visibility stack. */
  bool8_t deferred_enabled;
  /** Diagnostic rollback for P14 while retaining the deferred graph. */
  bool8_t hzb_enabled;
  uint32_t max_images;
  uint32_t max_passes;
  uint32_t max_material_rows;
  uint32_t max_meshes;
  uint32_t max_submeshes_per_mesh;
  uint32_t max_textures;
  uint32_t max_draws;
  uint32_t max_instances;
} VkrMetalPacketRendererConfig;

/** CPU-decoded indexed geometry copied into private placement buffers. */
typedef struct VkrMetalPacketSubmeshCreateInfo {
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
} VkrMetalPacketSubmeshCreateInfo;

typedef struct VkrMetalPacketMeshCreateInfo {
  const VkrVertex3d *vertices;
  uint32_t vertex_count;
  const uint32_t *indices;
  uint32_t index_count;
  const VkrMetalPacketSubmeshCreateInfo *submeshes;
  uint32_t submesh_count;
} VkrMetalPacketMeshCreateInfo;

/** Focused immutable material publication input for the Metal GPU table. */
typedef enum VkrMetalPacketMaterialTextureFlag {
  VKR_METAL_PACKET_MATERIAL_TEXTURE_NORMAL = 1u << 0u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_ORM = 1u << 1u,
  VKR_METAL_PACKET_MATERIAL_TEXTURE_EMISSIVE = 1u << 2u,
} VkrMetalPacketMaterialTextureFlag;

typedef struct VkrMetalPacketRgba8TextureCreateInfo {
  const uint8_t *pixels;
  uint32_t width;
  uint32_t height;
} VkrMetalPacketRgba8TextureCreateInfo;

typedef struct VkrMetalPacketMaterialCreateInfo {
  float32_t tint[4];
  VkrMetalPacketRgba8TextureCreateInfo textures[4];
  uint32_t material_id;
  uint32_t texture_flags;
  VkrPbrProperties pbr;
  VkrMaterialAlphaMode alpha_mode;
  float32_t alpha_cutoff;
} VkrMetalPacketMaterialCreateInfo;

/** Tightly packed RGBA8 faces ordered +X, -X, +Y, -Y, +Z, -Z. */
typedef struct VkrMetalPacketCubemapCreateInfo {
  const uint8_t *rgba8_faces;
  uint32_t extent;
} VkrMetalPacketCubemapCreateInfo;

/** Decoded 2:1 RGBA16F environment payload, matching the shared HDR decoder. */
typedef struct VkrMetalPacketHdrEnvironmentCreateInfo {
  const uint16_t *rgba16_equirect;
  uint32_t width;
  uint32_t height;
  uint32_t cube_extent;
} VkrMetalPacketHdrEnvironmentCreateInfo;

enum {
  VKR_METAL_PACKET_TIMING_NAME_CAPACITY = 64,
  VKR_METAL_PACKET_MAX_PASS_TIMINGS = 64,
};

/** Completed Metal timestamp interval for one authored graph pass. */
typedef struct VkrMetalPacketPassTiming {
  char name[VKR_METAL_PACKET_TIMING_NAME_CAPACITY];
  float64_t cpu_ms;
  float64_t gpu_ms;
  uint32_t pass_index;
  bool8_t valid;
} VkrMetalPacketPassTiming;

/** CPU-visible evidence returned after the requested readbacks complete. */
typedef struct VkrMetalPacketResult {
  uint64_t submit_value;
  uint64_t source_frame_index;
  uint32_t executed_pass_count;
  uint32_t graphics_pass_count;
  uint32_t compute_pass_count;
  uint32_t transfer_pass_count;
  uint32_t dependency_count;
  uint32_t indexed_draw_count;
  uint32_t shadow_draw_count;
  uint32_t opaque_draw_count;
  uint32_t transmission_draw_count;
  uint32_t blend_draw_count;
  uint32_t editor_draw_count;
  uint32_t ui_draw_count;
  uint32_t text_draw_count;
  uint32_t picking_text_draw_count;
  uint32_t picking_draw_count;
  uint32_t skybox_draw_count;
  uint32_t ibl_dispatch_count;
  uint32_t gpu_candidate_count;
  uint32_t gpu_visible_count;
  uint32_t gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t gpu_overflow_count;
  uint32_t gpu_resolve_invalid_count;
  uint32_t gpu_occlusion_culled_count;
  uint32_t transmission_gpu_candidate_count;
  uint32_t transmission_gpu_visible_count;
  uint32_t transmission_gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t transmission_gpu_overflow_count;
  uint32_t transmission_gpu_occlusion_culled_count;
  bool8_t hzb_history_valid;
  bool8_t has_gpu_draw_diagnostics;
  uint32_t resize_count;
  uint8_t color[4];
  float32_t shadow_depth;
  bool8_t has_shadow_depth;
  uint32_t picking_id;
  bool8_t has_picking_id;
  float32_t ibl_color[4];
  bool8_t has_ibl_color;
  float32_t ibl_prefilter[VKR_IBL_PREFILTER_MIP_COUNT][4];
  uint32_t ibl_irradiance_size;
  uint32_t ibl_prefilter_size;
  uint32_t ibl_prefilter_mip_count;
  bool8_t has_ibl_convolution;
  uint32_t pipeline_count;
  uint32_t pass_timing_count;
  VkrMetalPacketPassTiming pass_timings[VKR_METAL_PACKET_MAX_PASS_TIMINGS];
  bool8_t pipeline_archive_warm;
  bool8_t pipeline_archive_written;
  /** Snapshot of the request-owned poll result, if this packet captured. */
  VkrCapturePollResult capture;
  VkrMetalMemoryDeviceMetrics memory;
  VkrMetalMaterialTableMetrics materials;
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
    const struct VkrMeshLoaderResult *loader_result, VkrMeshHandle *out_handle);
/** Publishes loader geometry under an existing shared mesh handle. */
bool8_t vkr_metal_packet_renderer_publish_loaded_mesh(
    VkrMetalPacketRenderer *renderer, VkrGeometryHandle handle,
    const struct VkrMeshLoaderResult *loader_result);
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
bool8_t vkr_metal_packet_renderer_create_cubemap(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketCubemapCreateInfo *create_info,
    VkrTextureHandle *out_handle);
bool8_t vkr_metal_packet_renderer_create_rgba8_texture(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketRgba8TextureCreateInfo *create_info,
    VkrTextureHandle *out_handle);
/** Publishes a shared decoder payload under its existing texture handle. */
bool8_t vkr_metal_packet_renderer_publish_prepared_texture(
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
    VkrTextureHandle irradiance, VkrTextureHandle prefilter);
bool8_t vkr_metal_packet_renderer_bake_hdr_environment(
    VkrMetalPacketRenderer *renderer, VkrTextureHandle equirect,
    VkrTextureHandle source, VkrTextureHandle irradiance,
    VkrTextureHandle prefilter);
bool8_t vkr_metal_packet_renderer_create_hdr_environment(
    VkrMetalPacketRenderer *renderer,
    const VkrMetalPacketHdrEnvironmentCreateInfo *create_info,
    VkrTextureHandle *out_handle);
bool8_t
vkr_metal_packet_renderer_destroy_texture(VkrMetalPacketRenderer *renderer,
                                          VkrTextureHandle handle);

/**
 * Establishes the target dimensions and frame conditions for the next packet.
 * Selection is coarse: submit contains no backend-type branch or callback
 * through the Vulkan-shaped backend interface.
 */
bool8_t vkr_metal_packet_renderer_prepare_frame(
    VkrMetalPacketRenderer *renderer,
    const VkrRenderGraphFrameInfo *frame_info);

/**
 * Builds and schedules the authored graph, realizes its Metal resources, and
 * executes one packet. Registered meshes/materials are resolved from numeric
 * packet handles by the Metal-private pass executors.
 */
bool8_t
vkr_metal_packet_renderer_submit_packet(VkrMetalPacketRenderer *renderer,
                                        const VkrRenderPacket *packet,
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

/** Copies the newest completed timing result after `after_submit_value`. */
bool8_t vkr_metal_packet_renderer_pass_timings_poll_latest(
    VkrMetalPacketRenderer *renderer, uint64_t after_submit_value,
    VkrMetalPacketResult *out_result);

/** Waits for submitted work without retiring live assets. */
bool8_t vkr_metal_packet_renderer_wait_idle(VkrMetalPacketRenderer *renderer);
uint64_t
vkr_metal_packet_renderer_submit_value(const VkrMetalPacketRenderer *renderer);
uint64_t vkr_metal_packet_renderer_completed_value(
    const VkrMetalPacketRenderer *renderer);
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

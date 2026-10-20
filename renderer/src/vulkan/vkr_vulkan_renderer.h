#pragma once

#include "vkr_anisotropy_lut.h"
#include "vkr_asset_publisher.h"
#include "vkr_bloom.h"
#include "vkr_gpu_abi.h"
#include "vkr_gpu_memory.h"
#include "vkr_gpu_slot_table.h"
#include "vkr_gtao.h"
#include "vkr_ibl_math.h"
#include "vkr_ltc_lut.h"
#include "vkr_prepared_frame.h"
#include "vkr_render_graph.h"
#include "vkr_renderer_impl.h"
#include "vkr_sheen_lut.h"
#include "vulkan/vkr_vulkan_device.h"

enum {
  VKR_VULKAN_FRAME_SLOT_COUNT = 3,
  // N-1 queued frames may each retain distinct history input and output
  // instances; the recording frame still needs one completion-safe output.
  VKR_VULKAN_HISTORY_INSTANCE_COUNT = 2 * VKR_VULKAN_FRAME_SLOT_COUNT - 1,
  VKR_VULKAN_TARGET_IMAGE_MAX = 8,
  /* Cloud base volume, detail volume and weather map (ADR-074). */
  VKR_VULKAN_CLOUD_NOISE_COUNT = 3,
  /* SMAA's area (R8G8) and search (R8) tables (vendor/smaa). */
  VKR_VULKAN_SMAA_TABLE_AREA = 0,
  VKR_VULKAN_SMAA_TABLE_SEARCH = 1,
  VKR_VULKAN_SMAA_TABLE_COUNT = 2,
  /* Sampled-image heap rows the renderer holds for its own device lifetime, so
     they are never available to published textures. Derived from the tables
     that occupy them, so adding a table moves the configuration bound and the
     creation check together instead of letting them drift apart. */
  VKR_VULKAN_PERMANENT_SAMPLED_IMAGE_ROWS =
      1 /* black sentinel */ + 1 /* DFG */ + VKR_LTC_LUT_TABLE_COUNT +
      1 /* Charlie directional albedo */ + VKR_SHEEN_LTC_LUT_TABLE_COUNT +
      VKR_ANISOTROPY_LUT_TABLE_COUNT + VKR_VULKAN_CLOUD_NOISE_COUNT +
      VKR_VULKAN_SMAA_TABLE_COUNT,
};

typedef struct VkrVulkanRenderer VkrVulkanRenderer;

typedef struct VkrVulkanRendererConfig {
  VkrAllocator *allocator;
  /** Requests FSR 3.1's device feature (VkrAntialiasingMode). */
  bool8_t fsr31_enabled;
  /** A zeroed record selects the production bloom defaults. */
  VkrBloomConfig bloom;
  /** A zeroed record selects the production GTAO defaults. */
  VkrGtaoConfig gtao;
  const char *graph_path;
  VkrNativeSurface surface;
  VkrPresentTargetKind target_kind;
  VkrPresentMode requested_present_mode;
  VkrDisplayOutputMode display_output_mode;
  uint32_t width;
  uint32_t height;
  uint32_t image_count;
  /** Sampled-image descriptor heap slots. Bounds descriptors, not assets. */
  uint32_t sampled_image_capacity;
  uint32_t storage_image_capacity;
  uint32_t sampler_capacity;
  /**
   * Logical geometry IDs admitted by the asset publisher. Independent of every
   * descriptor heap: geometry is reached by device address and occupies no
   * descriptor slot. Must cover the geometry system's whole ID space, because
   * publication records are indexed directly by `VkrGeometryHandle::id`.
   */
  uint32_t geometry_capacity;
  /**
   * Logical texture IDs admitted by the asset publisher. Must cover the texture
   * system's whole ID space for the same reason. The sampled-image heap also
   * reserves VKR_VULKAN_PERMANENT_SAMPLED_IMAGE_ROWS for its own tables, so
   * this must fit within
   * `sampled_image_capacity - VKR_VULKAN_PERMANENT_SAMPLED_IMAGE_ROWS`.
   */
  uint32_t texture_capacity;
  /** Maximum logical material IDs admitted by the asset publisher. */
  uint32_t material_record_capacity;
  /** GPU rows: sentinel row zero plus two generations of every record. */
  uint32_t material_slot_capacity;
  uint64_t device_buffer_block_size;
  uint64_t device_image_block_size;
  uint64_t upload_buffer_block_size;
  uint64_t readback_buffer_block_size;
  uint32_t capture_ring_capacity;
  uint64_t capture_max_batch_bytes;
  uint32_t memory_block_capacity;
  uint32_t memory_blocks_per_pool;
  uint32_t memory_block_allocation_capacity;
  uint32_t publication_staging_capacity;
  /** Retained decoded texture bytes awaiting bounded staging. */
  uint64_t max_pending_texture_upload_bytes;
  uint32_t max_graph_images;
  uint32_t max_graph_buffers;
  uint32_t max_graph_passes;
  /** Applies ACES in the fullscreen post stage. */
  bool8_t tonemap_enabled;
  /** Previous-frame HZB occlusion is independent from frustum culling. */
  bool8_t hzb_enabled;
  bool8_t frustum_enabled;
  /** Bounded transmission scan plus indirect sparse shading. */
  bool8_t transmission_compact_enabled;
  bool8_t enable_validation;
  bool8_t enable_synchronization_validation;
  bool8_t enable_gpu_assisted;
  /** Receives each pipeline built after creation with its time
   * (pipeline.create): the presentation pipelines a swapchain format change
   * rebuilds. Creation builds every other pipeline before the application
   * runs. An invalid source publishes none. */
  VkrMetricEventProducer pipeline_create;
  /** Borrowed for the renderer's lifetime: each pipeline created is added
   * with its compile time and, with capture_shader_statistics on a device
   * that has VK_KHR_pipeline_executable_properties, its executables'
   * statistics; NULL adds none. */
  struct VkrPipelineRegistry *pipeline_registry;
  bool8_t capture_shader_statistics;
} VkrVulkanRendererConfig;

typedef struct VkrVulkanResult {
  uint64_t submit_value;
  uint64_t source_frame_index;
  uint32_t indexed_draw_count;
  uint32_t shadow_draw_count;
  uint32_t opaque_draw_count;
  uint32_t transmission_draw_count;
  uint32_t blend_draw_count;
  uint32_t gpu_visible_count;
  uint32_t gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t gpu_overflow_count;
  uint32_t gpu_resolve_invalid_count;
  uint32_t gpu_occlusion_culled_count;
  uint32_t transmission_gpu_visible_count;
  uint32_t transmission_gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t transmission_gpu_overflow_count;
  uint32_t transmission_gpu_resolve_invalid_count;
  uint32_t transmission_gpu_occlusion_culled_count;
  uint32_t transmission_compact_overflow_count;
  uint32_t
      transmission_covered_pixels[VKR_GPU_TRANSMISSION_DIAGNOSTIC_LAYER_COUNT];
  uint32_t transmission_coverage_extent[2];
  uint32_t shadow_gpu_visible_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_gpu_bucket_counts[VKR_SHADOW_CASCADE_COUNT_MAX]
                                   [VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t shadow_gpu_overflow_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  /** Per local-shadow render slot. */
  uint32_t local_shadow_render_count;
  uint32_t
      local_shadow_gpu_visible_count[VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX];
  uint32_t
      local_shadow_gpu_overflow_count[VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX];
  uint32_t image_index;
  uint8_t color[4];
  uint32_t identifier;
  uint32_t pass_timing_count;
  VkrRendererImplPassTiming pass_timings[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS];
  /** Whole-frame GPU time; only a completed frame's poll can carry it. */
  uint64_t gpu_submission_ns;
  VkrRendererImplGpuTimingReason gpu_submission_unavailable_reason;
  bool8_t gpu_submission_valid;
  /** The frame's GPU begin and end on vkr_platform_get_monotonic_ns's clock,
      through calibrated timestamps; valid with gpu_host_valid. */
  uint64_t gpu_start_host_ns;
  uint64_t gpu_end_host_ns;
  bool8_t gpu_host_valid;
  /** Same-frame packet-lowering CPU cost; only submit fills it, not poll. */
  VkrPacketBuildMetrics packet_build;
  bool8_t readback_ready;
  bool8_t hzb_history_valid;
  VkrShadowDepthRangeSample shadow_depth_range;
  VkrLocalLightContributionSample local_light_contribution;
  bool8_t has_gpu_draw_diagnostics;
  VkrExposureDebugSample exposure;
  bool8_t has_transmission_coverage;
} VkrVulkanResult;

typedef struct VkrVulkanHeapMetrics {
  VkrGpuSlotTableMetrics sampled_images;
  VkrGpuSlotTableMetrics samplers;
  VkrGpuSlotTableMetrics storage_images;
  VkrGpuSlotTableMetrics materials;
} VkrVulkanHeapMetrics;

typedef struct VkrVulkanMemoryMetrics {
  uint64_t physical_allocations_live;
  uint64_t physical_allocations_peak;
  uint64_t physical_allocations_created;
  uint64_t physical_allocated_bytes;
  uint64_t physical_allocated_bytes_peak;
  uint64_t block_capacity_failures;
  VkrGpuMemoryMetrics aggregate;
  VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT];
  uint64_t live_bytes_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];
  uint64_t live_count_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];
  /** The driver's device-local usage and budget at the last submission;
      valid only with VK_EXT_memory_budget. */
  uint64_t driver_usage_bytes;
  uint64_t driver_budget_bytes;
  bool8_t driver_metrics_valid;
} VkrVulkanMemoryMetrics;

void vkr_vulkan_renderer_geometry_megabuffer_metrics(
    const VkrVulkanRenderer *renderer,
    VkrGeometryMegabufferMetrics *out_metrics);

/** On failure, releases partial state and leaves `*out_renderer` null. */
bool8_t vkr_vulkan_renderer_create(const VkrVulkanRendererConfig *config,
                                   VkrVulkanRenderer **out_renderer);
/** True when this build carries the FSR 3.1 SDK and the device supports it;
    otherwise FSR frames fall back to SMAA T2x. */
bool8_t vkr_vulkan_renderer_fsr31_available(const VkrVulkanRenderer *renderer);
void vkr_vulkan_renderer_destroy(VkrVulkanRenderer *renderer);
/** Terminal submission/presentation faults prohibit further frame acquisition.
 */
VkrRendererError
vkr_vulkan_renderer_get_error(const VkrVulkanRenderer *renderer);

bool8_t vkr_vulkan_renderer_prepare_frame(VkrVulkanRenderer *renderer,
                                          uint64_t source_frame_index,
                                          uint32_t shadow_map_size,
                                          uint32_t shadow_cascade_count,
                                          uint32_t local_shadow_map_size,
                                          uint32_t local_shadow_face_budget,
                                          VkrFrame *out_setup);
/** Returns the extent only when the retained editor image has committed
 * contents. */
void vkr_vulkan_renderer_retained_editor_extent(VkrVulkanRenderer *renderer,
                                                uint32_t *out_width,
                                                uint32_t *out_height);
void vkr_vulkan_renderer_retained_shadow_token(
    VkrVulkanRenderer *renderer, uint32_t image_index,
    VkrRetainedShadowToken *out_token);
void vkr_vulkan_renderer_retained_local_shadow_token(
    VkrVulkanRenderer *renderer, VkrRetainedLocalShadowToken *out_token);
bool8_t vkr_vulkan_renderer_submit_packet(VkrVulkanRenderer *renderer,
                                          const VkrPreparedFrame *packet,
                                          VkrVulkanResult *out_result);
bool8_t vkr_vulkan_renderer_poll_result(VkrVulkanRenderer *renderer,
                                        uint64_t after_submit_value,
                                        VkrVulkanResult *out_result);
VkrRendererError vkr_vulkan_renderer_get_pixel_readback_result(
    VkrVulkanRenderer *renderer, VkrPixelReadbackResult *out_result);
VkrCaptureStatus
vkr_vulkan_renderer_capture_poll(VkrVulkanRenderer *renderer,
                                 VkrCaptureRequestId request_id,
                                 VkrCapturePollResult *out_result);
bool8_t vkr_vulkan_renderer_capture_release(VkrVulkanRenderer *renderer,
                                            VkrCaptureRequestId request_id);
void vkr_vulkan_renderer_cancel_frame(VkrVulkanRenderer *renderer);
bool8_t vkr_vulkan_renderer_resize(VkrVulkanRenderer *renderer, uint32_t width,
                                   uint32_t height, uint32_t image_count);

bool8_t vkr_vulkan_renderer_wait_idle(VkrVulkanRenderer *renderer);
/** Requests a new presentation mode; the window target is recreated before
 * the next frame. False for an offscreen target. */
bool8_t vkr_vulkan_renderer_set_present_mode(VkrVulkanRenderer *renderer,
                                             VkrPresentMode mode);
/** True when the device traces the diffuse volume's dynamic layer. */
bool8_t
vkr_vulkan_renderer_dynamic_volume_supported(const VkrVulkanRenderer *renderer);
/** The device traces ray-query sun shadows (VKR_SUN_RAY_SHADOWS). */
bool8_t vkr_vulkan_renderer_sun_ray_shadows_supported(
    const VkrVulkanRenderer *renderer);
/* Probes the dynamic volume layer may trace next frame within its GPU
   budget (ADR-119). */
uint32_t vkr_vulkan_renderer_dynamic_volume_probe_budget(
    const VkrVulkanRenderer *renderer);
uint64_t vkr_vulkan_renderer_submit_value(const VkrVulkanRenderer *renderer);
uint64_t vkr_vulkan_renderer_completed_value(const VkrVulkanRenderer *renderer);
bool8_t
vkr_vulkan_renderer_get_and_reset_upload_wait_count(VkrVulkanRenderer *renderer,
                                                    uint64_t *out_wait_count);
bool8_t vkr_vulkan_renderer_get_and_reset_frame_upload_exhaustion_count(
    VkrVulkanRenderer *renderer, uint64_t *out_exhaustion_count);
bool8_t vkr_vulkan_renderer_get_and_reset_command_slot_wait_count(
    VkrVulkanRenderer *renderer, uint64_t *out_wait_count);
/* A RenderDoc frame capture for debug.gpu_capture and where it stands; the
   states are the frontend's VkrRendererGpuCaptureState, in the same order. */
typedef enum VkrVulkanGpuCaptureState {
  VKR_VULKAN_GPU_CAPTURE_IDLE = 0,
  VKR_VULKAN_GPU_CAPTURE_REQUESTED,
  VKR_VULKAN_GPU_CAPTURE_CAPTURING,
  VKR_VULKAN_GPU_CAPTURE_DONE,
  VKR_VULKAN_GPU_CAPTURE_FAILED,
} VkrVulkanGpuCaptureState;

typedef struct VkrVulkanGpuCaptureStatus {
  VkrVulkanGpuCaptureState state;
  uint32_t frames;
  /** The path template while capturing; the written .rdc once done. */
  char path[1024];
  char message[256];
} VkrVulkanGpuCaptureStatus;

/** Render thread (the frontend joins it): captures the next `frames`
 * submitted frames with RenderDoc into a .rdc named from `path_template` (a
 * path without extension). Returns false while a capture is pending or
 * running, or when RenderDoc is not in the process; the status says why. */
bool8_t vkr_vulkan_renderer_request_gpu_capture(VkrVulkanRenderer *renderer,
                                                const char *path_template,
                                                uint32_t frames);
/** Render thread (the frontend joins it). */
void vkr_vulkan_renderer_gpu_capture_status(const VkrVulkanRenderer *renderer,
                                            VkrVulkanGpuCaptureStatus *out);
/** Render thread only; the metrics collector is the sole caller. */
bool8_t vkr_vulkan_renderer_get_and_reset_present_timing(
    VkrVulkanRenderer *renderer, VkrRendererImplPresentTiming *out_timing);
/** Safe from any thread; the debug messenger may count from driver threads. */
void vkr_vulkan_renderer_get_health(const VkrVulkanRenderer *renderer,
                                    VkrRendererHealth *out_health);
void vkr_vulkan_renderer_memory_metrics(const VkrVulkanRenderer *renderer,
                                        VkrVulkanMemoryMetrics *out_metrics);
void vkr_vulkan_renderer_device_memory_stats(const VkrVulkanRenderer *renderer,
                                             VkrDeviceMemoryStats *out_stats);
void vkr_vulkan_renderer_heap_metrics(const VkrVulkanRenderer *renderer,
                                      VkrVulkanHeapMetrics *out_metrics);
bool8_t
vkr_vulkan_renderer_texture_format_supported(const VkrVulkanRenderer *renderer,
                                             VkrTextureFormat format);
bool8_t vkr_vulkan_renderer_graph_resource_stats(
    const VkrVulkanRenderer *renderer, VkrRenderGraphResourceStats *out_stats);
struct VkrDrawCensusStore;
struct VkrBackendFrameCounters;
/** Render thread (the frontend joins it): the native work the last
 * submitted frame recorded (backend.* metrics). */
void vkr_vulkan_renderer_frame_counters(
    const VkrVulkanRenderer *renderer,
    struct VkrBackendFrameCounters *out_counters);
/** Render thread (the frontend joins it): the newest completed draw census,
 * which the next completed census overwrites. */
const struct VkrDrawCensusStore *
vkr_vulkan_renderer_draw_census(const VkrVulkanRenderer *renderer);
/** Render thread (the frontend joins it): the graph of the last submitted
 * packet, valid until the next submit; NULL before the graph exists. */
const VkrRenderGraph *
vkr_vulkan_renderer_graph(const VkrVulkanRenderer *renderer);
void vkr_vulkan_renderer_target_information(
    const VkrVulkanRenderer *renderer, VkrPresentMode *out_present_mode,
    VkrSurfaceColorFormat *out_color_format,
    VkrSurfaceDepthFormat *out_depth_format,
    VkrSurfaceColorSpace *out_color_space, float32_t *out_max_anisotropy);
VkrDisplayOutputParams
vkr_vulkan_renderer_display_output(const VkrVulkanRenderer *renderer);

const VkrVulkanCapabilityProfile *
vkr_vulkan_renderer_profile(const VkrVulkanRenderer *renderer);
/** Fills the device's hardware facts for the Performance window's HW tab
 * (ADR-130): unified memory, workgroup limits, subgroup sizes and whether
 * pipelines capture their statistics. Leaves the other fields alone. */
void vkr_vulkan_renderer_hardware_information(
    const VkrVulkanRenderer *renderer, VkrDeviceInformation *out_information);
VkrAllocator *vkr_vulkan_renderer_allocator(VkrVulkanRenderer *renderer);
void vkr_vulkan_renderer_get_asset_publisher(VkrVulkanRenderer *renderer,
                                             VkrAssetPublisher *out_publisher);

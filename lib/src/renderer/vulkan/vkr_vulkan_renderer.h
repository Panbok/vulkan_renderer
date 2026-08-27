#pragma once

#include "renderer/vkr_asset_publisher.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_slot_table.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_graph.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_renderer_impl.h"
#include "renderer/vulkan/vkr_vulkan_device.h"

enum {
  VKR_VULKAN_FRAME_SLOT_COUNT = 3,
  // N-1 queued frames may each retain distinct history input and output
  // instances; the recording frame still needs one completion-safe output.
  VKR_VULKAN_HISTORY_INSTANCE_COUNT = 2 * VKR_VULKAN_FRAME_SLOT_COUNT - 1,
  VKR_VULKAN_TARGET_IMAGE_MAX = 8,
};

typedef struct VkrVulkanRenderer VkrVulkanRenderer;

typedef struct VkrVulkanRendererConfig {
  VkrAllocator *allocator;
  const char *graph_path;
  VkrWindow *window;
  VkrPresentTargetKind target_kind;
  VkrPresentMode requested_present_mode;
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
   * system's whole ID space for the same reason, and must not exceed
   * `sampled_image_capacity`, since every published texture also consumes one
   * sampled-image descriptor slot.
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
  /** Post-TAA spatial cleanup in the final fullscreen draw. */
  bool8_t fxaa_enabled;
  /** Previous-frame HZB occlusion is independent from frustum culling. */
  bool8_t hzb_enabled;
  bool8_t enable_validation;
  bool8_t enable_synchronization_validation;
  bool8_t enable_gpu_assisted;
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
  uint32_t transmission_gpu_occlusion_culled_count;
  uint32_t transmission_covered_pixels[VKR_GPU_TRANSMISSION_LAYER_COUNT];
  uint32_t transmission_coverage_extent[2];
  uint32_t shadow_gpu_visible_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_gpu_bucket_counts[VKR_SHADOW_CASCADE_COUNT_MAX]
                                   [VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t shadow_gpu_overflow_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t image_index;
  uint8_t color[4];
  uint32_t identifier;
  uint32_t pass_timing_count;
  VkrRendererImplPassTiming pass_timings[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS];
  /** Same-frame packet-lowering CPU cost; only submit fills it, not poll. */
  VkrPacketBuildMetrics packet_build;
  bool8_t readback_ready;
  bool8_t hzb_history_valid;
  VkrShadowDepthRangeSample shadow_depth_range;
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
} VkrVulkanMemoryMetrics;

void vkr_vulkan_renderer_geometry_megabuffer_metrics(
    const VkrVulkanRenderer *renderer,
    VkrGeometryMegabufferMetrics *out_metrics);

/** On failure, releases partial state and leaves `*out_renderer` null. */
bool8_t vkr_vulkan_renderer_create(const VkrVulkanRendererConfig *config,
                                   VkrVulkanRenderer **out_renderer);
void vkr_vulkan_renderer_destroy(VkrVulkanRenderer *renderer);

bool8_t vkr_vulkan_renderer_prepare_frame(VkrVulkanRenderer *renderer,
                                          uint64_t source_frame_index,
                                          uint32_t shadow_map_size,
                                          uint32_t shadow_cascade_count,
                                          VkrFrameSetup *out_setup);
void vkr_vulkan_renderer_retained_shadow_token(
    VkrVulkanRenderer *renderer, uint32_t image_index,
    VkrRetainedShadowToken *out_token);
bool8_t vkr_vulkan_renderer_submit_packet(VkrVulkanRenderer *renderer,
                                          const VkrRenderPacket *packet,
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
uint64_t vkr_vulkan_renderer_submit_value(const VkrVulkanRenderer *renderer);
uint64_t vkr_vulkan_renderer_completed_value(const VkrVulkanRenderer *renderer);
bool8_t
vkr_vulkan_renderer_get_and_reset_upload_wait_count(VkrVulkanRenderer *renderer,
                                                    uint64_t *out_wait_count);
bool8_t vkr_vulkan_renderer_get_and_reset_frame_upload_exhaustion_count(
    VkrVulkanRenderer *renderer, uint64_t *out_exhaustion_count);
bool8_t vkr_vulkan_renderer_get_and_reset_command_slot_wait_count(
    VkrVulkanRenderer *renderer, uint64_t *out_wait_count);
void vkr_vulkan_renderer_memory_metrics(const VkrVulkanRenderer *renderer,
                                        VkrVulkanMemoryMetrics *out_metrics);
void vkr_vulkan_renderer_device_memory_stats(const VkrVulkanRenderer *renderer,
                                             VkrDeviceMemoryStats *out_stats);
void vkr_vulkan_renderer_heap_metrics(const VkrVulkanRenderer *renderer,
                                      VkrVulkanHeapMetrics *out_metrics);
uint32_t vkr_vulkan_renderer_frame_slot(const VkrVulkanRenderer *renderer);
bool8_t vkr_vulkan_renderer_hdr_ibl_limits(const VkrVulkanRenderer *renderer,
                                           uint32_t *out_max_cube_extent,
                                           uint32_t *out_max_mip_levels);
bool8_t
vkr_vulkan_renderer_texture_format_supported(const VkrVulkanRenderer *renderer,
                                             VkrTextureFormat format);
bool8_t vkr_vulkan_renderer_graph_resource_stats(
    const VkrVulkanRenderer *renderer, VkrRenderGraphResourceStats *out_stats);
void vkr_vulkan_renderer_target_information(
    const VkrVulkanRenderer *renderer, VkrPresentMode *out_present_mode,
    VkrSurfaceColorFormat *out_color_format,
    VkrSurfaceDepthFormat *out_depth_format,
    VkrSurfaceColorSpace *out_color_space, float32_t *out_max_anisotropy);

const VkrVulkanCapabilityProfile *
vkr_vulkan_renderer_profile(const VkrVulkanRenderer *renderer);
VkrAllocator *vkr_vulkan_renderer_allocator(VkrVulkanRenderer *renderer);
void vkr_vulkan_renderer_get_asset_publisher(VkrVulkanRenderer *renderer,
                                             VkrAssetPublisher *out_publisher);

#pragma once

#include "renderer/vkr_asset_publisher.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_slot_table.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_graph.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_renderer_impl.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_device.h"

enum {
  VKR_BINDLESS_VK_FRAME_SLOT_COUNT = 3,
  VKR_BINDLESS_VK_TARGET_IMAGE_MAX = 8,
};

typedef struct VkrBindlessVulkanRenderer VkrBindlessVulkanRenderer;

typedef struct VkrBindlessVulkanRendererConfig {
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
  uint32_t max_graph_images;
  uint32_t max_graph_passes;
  bool8_t enable_validation;
  bool8_t enable_synchronization_validation;
  bool8_t enable_gpu_assisted;
} VkrBindlessVulkanRendererConfig;

typedef struct VkrBindlessVulkanResult {
  uint64_t submit_value;
  uint64_t source_frame_index;
  uint32_t indexed_draw_count;
  uint32_t shadow_draw_count;
  uint32_t shadow_opaque_draw_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_alpha_draw_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t opaque_draw_count;
  uint32_t transmission_draw_count;
  uint32_t blend_draw_count;
  uint32_t image_index;
  uint8_t color[4];
  uint32_t identifier;
  uint32_t pass_timing_count;
  VkrRendererImplPassTiming pass_timings[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS];
  bool8_t readback_ready;
} VkrBindlessVulkanResult;

typedef struct VkrBindlessVulkanHeapMetrics {
  VkrGpuSlotTableMetrics sampled_images;
  VkrGpuSlotTableMetrics samplers;
  VkrGpuSlotTableMetrics storage_images;
  VkrGpuSlotTableMetrics materials;
} VkrBindlessVulkanHeapMetrics;

typedef struct VkrBindlessVulkanMemoryMetrics {
  uint64_t physical_allocations_live;
  uint64_t physical_allocations_peak;
  uint64_t physical_allocations_created;
  uint64_t physical_allocated_bytes;
  uint64_t physical_allocated_bytes_peak;
  uint64_t block_capacity_failures;
  VkrGpuMemoryMetrics aggregate;
} VkrBindlessVulkanMemoryMetrics;

bool8_t vkr_bindless_vulkan_renderer_create(
    const VkrBindlessVulkanRendererConfig *config,
    VkrBindlessVulkanRenderer **out_renderer);
void vkr_bindless_vulkan_renderer_destroy(VkrBindlessVulkanRenderer *renderer);

bool8_t vkr_bindless_vulkan_renderer_prepare_frame(
    VkrBindlessVulkanRenderer *renderer, uint64_t source_frame_index,
    uint32_t shadow_map_size, uint32_t shadow_cascade_count,
    VkrFrameSetup *out_setup);
bool8_t
vkr_bindless_vulkan_renderer_submit_packet(VkrBindlessVulkanRenderer *renderer,
                                           const VkrRenderPacket *packet,
                                           VkrBindlessVulkanResult *out_result);
bool8_t
vkr_bindless_vulkan_renderer_poll_result(VkrBindlessVulkanRenderer *renderer,
                                         uint64_t after_submit_value,
                                         VkrBindlessVulkanResult *out_result);
VkrRendererError vkr_bindless_vulkan_renderer_get_pixel_readback_result(
    VkrBindlessVulkanRenderer *renderer, VkrPixelReadbackResult *out_result);
VkrCaptureStatus
vkr_bindless_vulkan_renderer_capture_poll(VkrBindlessVulkanRenderer *renderer,
                                          VkrCaptureRequestId request_id,
                                          VkrCapturePollResult *out_result);
bool8_t vkr_bindless_vulkan_renderer_capture_release(
    VkrBindlessVulkanRenderer *renderer, VkrCaptureRequestId request_id);
void vkr_bindless_vulkan_renderer_cancel_frame(
    VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vulkan_renderer_resize(VkrBindlessVulkanRenderer *renderer,
                                            uint32_t width, uint32_t height,
                                            uint32_t image_count);

bool8_t
vkr_bindless_vulkan_renderer_wait_idle(VkrBindlessVulkanRenderer *renderer);
uint64_t vkr_bindless_vulkan_renderer_submit_value(
    const VkrBindlessVulkanRenderer *renderer);
uint64_t vkr_bindless_vulkan_renderer_completed_value(
    const VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vulkan_renderer_get_and_reset_upload_wait_count(
    VkrBindlessVulkanRenderer *renderer, uint64_t *out_wait_count);
bool8_t vkr_bindless_vulkan_renderer_get_and_reset_command_slot_wait_count(
    VkrBindlessVulkanRenderer *renderer, uint64_t *out_wait_count);
void vkr_bindless_vulkan_renderer_memory_metrics(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanMemoryMetrics *out_metrics);
void vkr_bindless_vulkan_renderer_device_memory_stats(
    const VkrBindlessVulkanRenderer *renderer, VkrDeviceMemoryStats *out_stats);
void vkr_bindless_vulkan_renderer_heap_metrics(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanHeapMetrics *out_metrics);
uint32_t vkr_bindless_vulkan_renderer_frame_slot(
    const VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vulkan_renderer_hdr_ibl_limits(
    const VkrBindlessVulkanRenderer *renderer, uint32_t *out_max_cube_extent,
    uint32_t *out_max_mip_levels);
bool8_t vkr_bindless_vulkan_renderer_texture_format_supported(
    const VkrBindlessVulkanRenderer *renderer, VkrTextureFormat format);
bool8_t vkr_bindless_vulkan_renderer_graph_resource_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrRenderGraphResourceStats *out_stats);
void vkr_bindless_vulkan_renderer_target_information(
    const VkrBindlessVulkanRenderer *renderer, VkrPresentMode *out_present_mode,
    VkrSurfaceColorFormat *out_color_format,
    VkrSurfaceDepthFormat *out_depth_format,
    VkrSurfaceColorSpace *out_color_space, float32_t *out_max_anisotropy);

const VkrBindlessVkCapabilityProfile *
vkr_bindless_vulkan_renderer_profile(const VkrBindlessVulkanRenderer *renderer);
VkrAllocator *
vkr_bindless_vulkan_renderer_allocator(VkrBindlessVulkanRenderer *renderer);
void vkr_bindless_vulkan_renderer_get_asset_publisher(
    VkrBindlessVulkanRenderer *renderer, VkrAssetPublisher *out_publisher);

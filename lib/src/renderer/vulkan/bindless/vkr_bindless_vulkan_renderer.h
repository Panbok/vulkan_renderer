#pragma once

#include "renderer/vkr_asset_publisher.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_slot_table.h"
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
  VkrWindow *window;
  VkrPresentTargetKind target_kind;
  uint32_t width;
  uint32_t height;
  uint32_t image_count;
  uint32_t sampled_image_capacity;
  uint32_t storage_image_capacity;
  uint32_t sampler_capacity;
  uint32_t material_capacity;
  bool8_t enable_validation;
  bool8_t enable_synchronization_validation;
  bool8_t enable_gpu_assisted;
} VkrBindlessVulkanRendererConfig;

typedef struct VkrBindlessVulkanResult {
  uint64_t submit_value;
  uint64_t source_frame_index;
  uint32_t indexed_draw_count;
  uint32_t image_index;
  uint8_t color[4];
  uint32_t identifier;
  bool8_t readback_ready;
} VkrBindlessVulkanResult;

typedef struct VkrBindlessVulkanPublicationTestResult {
  uint32_t exact_draw_count;
  bool8_t shared_resource_survived;
  bool8_t replacement_survived;
  VkrGpuSlotTableMetrics sampled_images;
  VkrGpuSlotTableMetrics storage_images;
  VkrGpuSlotTableMetrics samplers;
  VkrGpuSlotTableMetrics materials;
} VkrBindlessVulkanPublicationTestResult;

typedef struct VkrBindlessVulkanHeapMetrics {
  VkrGpuSlotTableMetrics sampled_images;
  VkrGpuSlotTableMetrics samplers;
  VkrGpuSlotTableMetrics storage_images;
  VkrGpuSlotTableMetrics materials;
} VkrBindlessVulkanHeapMetrics;

typedef struct VkrBindlessVulkanMemoryMetrics {
  uint32_t block_count;
  VkrGpuMemoryMetrics blocks[4];
  VkrGpuMemoryMetrics aggregate;
} VkrBindlessVulkanMemoryMetrics;

typedef struct VkrBindlessVulkanWsiStats {
  uint64_t reacquire_proofs;
  uint64_t retired_swapchains;
  uint64_t retired_swapchains_collected;
  uint32_t retired_swapchains_live;
} VkrBindlessVulkanWsiStats;

bool8_t vkr_bindless_vulkan_renderer_create(
    const VkrBindlessVulkanRendererConfig *config,
    VkrBindlessVulkanRenderer **out_renderer);
void vkr_bindless_vulkan_renderer_destroy(VkrBindlessVulkanRenderer *renderer);

bool8_t
vkr_bindless_vulkan_renderer_prepare_frame(VkrBindlessVulkanRenderer *renderer,
                                           uint64_t source_frame_index,
                                           VkrFrameSetup *out_setup);
bool8_t
vkr_bindless_vulkan_renderer_submit_packet(VkrBindlessVulkanRenderer *renderer,
                                           const VkrRenderPacket *packet,
                                           VkrBindlessVulkanResult *out_result);
bool8_t
vkr_bindless_vulkan_renderer_poll_result(VkrBindlessVulkanRenderer *renderer,
                                         uint64_t after_submit_value,
                                         VkrBindlessVulkanResult *out_result);
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
void vkr_bindless_vulkan_renderer_heap_metrics(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanHeapMetrics *out_metrics);
void vkr_bindless_vulkan_renderer_wsi_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanWsiStats *out_stats);
uint32_t vkr_bindless_vulkan_renderer_frame_slot(
    const VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vulkan_renderer_shader_abi_validated(
    const VkrBindlessVulkanRenderer *renderer);

const VkrBindlessVkCapabilityProfile *
vkr_bindless_vulkan_renderer_profile(const VkrBindlessVulkanRenderer *renderer);
VkrAllocator *
vkr_bindless_vulkan_renderer_allocator(VkrBindlessVulkanRenderer *renderer);
void vkr_bindless_vulkan_renderer_get_asset_publisher(
    VkrBindlessVulkanRenderer *renderer, VkrAssetPublisher *out_publisher);
void vkr_bindless_vulkan_renderer_validation_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanValidationStats *out_stats);
bool8_t vkr_bindless_vulkan_renderer_run_publication_test(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanPublicationTestResult *out_result);

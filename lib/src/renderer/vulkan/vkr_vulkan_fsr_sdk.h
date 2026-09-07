#pragma once

#include "defines.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkrVulkanFsrSdk VkrVulkanFsrSdk;
typedef struct VkrAllocator VkrAllocator;

typedef enum VkrVulkanFsrSdkResult {
  VKR_VULKAN_FSR_SDK_SUCCESS = 0,
  VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT,
  VKR_VULKAN_FSR_SDK_OUT_OF_MEMORY,
  VKR_VULKAN_FSR_SDK_ERROR,
} VkrVulkanFsrSdkResult;

typedef struct VkrVulkanFsrSdkImage {
  VkImage image;
  VkFormat format;
  VkExtent2D extent;
  /** The graph-declared layout before FSR records, restored before return. */
  VkImageLayout layout;
} VkrVulkanFsrSdkImage;

typedef struct VkrVulkanFsrSdkConfig {
  VkrAllocator *allocator;
  VkDevice device;
  VkPhysicalDevice physical_device;
  PFN_vkGetDeviceProcAddr get_device_proc_addr;
  /** Creation bounds. Recreate only after GPU idle if either bound changes. */
  VkExtent2D max_render_extent;
  VkExtent2D max_output_extent;
} VkrVulkanFsrSdkConfig;

typedef struct VkrVulkanFsrSdkDispatch {
  /** Records only into this graphics command buffer; it never submits or
   * waits. */
  VkCommandBuffer command_buffer;
  VkrVulkanFsrSdkImage hdr_color;
  VkrVulkanFsrSdkImage depth;
  VkrVulkanFsrSdkImage motion;
  /** Optional normalized FSR reactive-mask image. */
  VkrVulkanFsrSdkImage reactive;
  /** Optional normalized FSR transparency/composition image. */
  VkrVulkanFsrSdkImage composition;
  VkrVulkanFsrSdkImage output;
  float32_t jitter_x;
  float32_t jitter_y;
  /** Pixel-space scale for the supplied motion-vector convention. */
  float32_t motion_scale_x;
  float32_t motion_scale_y;
  float32_t frame_time_ms;
  float32_t camera_near;
  float32_t camera_far;
  float32_t camera_fov_y_radians;
  /** Must be zero: RCAS sharpening is disabled for this dispatch. */
  float32_t sharpness;
  bool8_t reset;
} VkrVulkanFsrSdkDispatch;

VkrVulkanFsrSdkResult
vkr_vulkan_fsr_sdk_create(const VkrVulkanFsrSdkConfig *config,
                          VkrVulkanFsrSdk **out_sdk);
/** The caller proves the device is idle before destroying the SDK object. */
void vkr_vulkan_fsr_sdk_destroy(VkrVulkanFsrSdk *sdk);
VkrVulkanFsrSdkResult
vkr_vulkan_fsr_sdk_dispatch(VkrVulkanFsrSdk *sdk,
                            const VkrVulkanFsrSdkDispatch *dispatch);

#ifdef __cplusplus
}
#endif

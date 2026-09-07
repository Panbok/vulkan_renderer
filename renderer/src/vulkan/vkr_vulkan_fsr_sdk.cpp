#include "vulkan/vkr_vulkan_fsr_sdk.h"

extern "C" {
#include "core/logger.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
}

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <string.h>

struct VkrVulkanFsrSdk {
  FfxFsr3UpscalerContext context;
  FfxInterface backend;
  VkrAllocator *allocator;
  Arena *scratch_arena;
  void *scratch;
  size_t scratch_size;
  VkExtent2D max_render_extent;
  VkExtent2D max_output_extent;
  FfxUInt32 shared_context_id;
  FfxResourceInternal dilated_depth;
  FfxResourceInternal dilated_motion;
  FfxResourceInternal reconstructed_previous_depth;
  bool8_t shared_context_created;
  bool8_t dilated_depth_created;
  bool8_t dilated_motion_created;
  bool8_t reconstructed_previous_depth_created;
};

static bool8_t vkr_fsr_extent_valid(VkExtent2D extent) {
  return extent.width != 0u && extent.height != 0u;
}

static bool8_t vkr_fsr_extent_within(VkExtent2D extent, VkExtent2D maximum) {
  return vkr_fsr_extent_valid(extent) && extent.width <= maximum.width &&
         extent.height <= maximum.height;
}

static bool8_t vkr_fsr_image_valid(const VkrVulkanFsrSdkImage *image) {
  return image && image->image != VK_NULL_HANDLE &&
         image->format != VK_FORMAT_UNDEFINED &&
         vkr_fsr_extent_valid(image->extent);
}

static bool8_t vkr_fsr_image_state(VkImageLayout layout,
                                   FfxResourceStates *out_state) {
  if (!out_state)
    return false_v;
  switch (layout) {
  case VK_IMAGE_LAYOUT_GENERAL:
    *out_state = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    return true_v;
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    *out_state = FFX_RESOURCE_STATE_COMPUTE_READ;
    return true_v;
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    *out_state = FFX_RESOURCE_STATE_COPY_SRC;
    return true_v;
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    *out_state = FFX_RESOURCE_STATE_COPY_DEST;
    return true_v;
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    *out_state = FFX_RESOURCE_STATE_RENDER_TARGET;
    return true_v;
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    *out_state = FFX_RESOURCE_STATE_DEPTH_ATTACHEMENT;
    return true_v;
  default:
    return false_v;
  }
}

static VkrVulkanFsrSdkResult vkr_fsr_resource(const VkrVulkanFsrSdkImage *image,
                                              FfxResourceUsage usage,
                                              FfxResource *out_resource) {
  if (!vkr_fsr_image_valid(image) || !out_resource)
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;

  FfxResourceStates state = FFX_RESOURCE_STATE_COMMON;
  if (!vkr_fsr_image_state(image->layout, &state))
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;

  const FfxSurfaceFormat format = ffxGetSurfaceFormatVK(image->format);
  if (format == FFX_SURFACE_FORMAT_UNKNOWN)
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;

  FfxResourceDescription description = {};
  description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  description.format = format;
  description.width = image->extent.width;
  description.height = image->extent.height;
  description.depth = 1u;
  description.mipCount = 1u;
  description.flags = FFX_RESOURCE_FLAGS_NONE;
  description.usage = usage;
  *out_resource = ffxGetResourceVK(reinterpret_cast<void *>(image->image),
                                   description, NULL, state);
  return VKR_VULKAN_FSR_SDK_SUCCESS;
}

static VkrVulkanFsrSdkResult
vkr_fsr_optional_resource(const VkrVulkanFsrSdkImage *image,
                          FfxResource *out_resource) {
  if (!out_resource)
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;
  MemZero(out_resource, sizeof(*out_resource));
  if (!image || image->image == VK_NULL_HANDLE)
    return VKR_VULKAN_FSR_SDK_SUCCESS;
  return vkr_fsr_resource(image, FFX_RESOURCE_USAGE_READ_ONLY, out_resource);
}

static VkrVulkanFsrSdkResult vkr_fsr_result(FfxErrorCode result) {
  if (result == FFX_OK)
    return VKR_VULKAN_FSR_SDK_SUCCESS;
  if (result == FFX_ERROR_OUT_OF_MEMORY ||
      result == FFX_ERROR_INSUFFICIENT_MEMORY)
    return VKR_VULKAN_FSR_SDK_OUT_OF_MEMORY;
  return VKR_VULKAN_FSR_SDK_ERROR;
}

static void vkr_fsr_destroy_shared_resources(VkrVulkanFsrSdk *sdk) {
  if (!sdk || !sdk->shared_context_created)
    return;
  if (sdk->reconstructed_previous_depth_created) {
    sdk->backend.fpDestroyResource(&sdk->backend,
                                   sdk->reconstructed_previous_depth,
                                   sdk->shared_context_id);
    sdk->reconstructed_previous_depth_created = false_v;
  }
  if (sdk->dilated_motion_created) {
    sdk->backend.fpDestroyResource(&sdk->backend, sdk->dilated_motion,
                                   sdk->shared_context_id);
    sdk->dilated_motion_created = false_v;
  }
  if (sdk->dilated_depth_created) {
    sdk->backend.fpDestroyResource(&sdk->backend, sdk->dilated_depth,
                                   sdk->shared_context_id);
    sdk->dilated_depth_created = false_v;
  }
  sdk->backend.fpDestroyBackendContext(&sdk->backend, sdk->shared_context_id);
  sdk->shared_context_created = false_v;
}

static void vkr_fsr_destroy_scratch(VkrVulkanFsrSdk *sdk) {
  if (!sdk || !sdk->scratch_arena)
    return;
  arena_destroy(sdk->scratch_arena);
  sdk->scratch_arena = NULL;
  sdk->scratch = NULL;
}

extern "C" VkrVulkanFsrSdkResult
vkr_vulkan_fsr_sdk_create(const VkrVulkanFsrSdkConfig *config,
                          VkrVulkanFsrSdk **out_sdk) {
  if (!out_sdk)
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;
  *out_sdk = NULL;
  if (!config || !config->allocator || config->device == VK_NULL_HANDLE ||
      config->physical_device == VK_NULL_HANDLE ||
      !config->get_device_proc_addr ||
      !vkr_fsr_extent_valid(config->max_render_extent) ||
      !vkr_fsr_extent_valid(config->max_output_extent))
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;

  VkrVulkanFsrSdk *sdk = static_cast<VkrVulkanFsrSdk *>(vkr_allocator_alloc(
      config->allocator, sizeof(*sdk), VKR_ALLOCATOR_MEMORY_TAG_VULKAN));
  if (!sdk)
    return VKR_VULKAN_FSR_SDK_OUT_OF_MEMORY;
  MemZero(sdk, sizeof(*sdk));
  sdk->allocator = config->allocator;

  /* One SDK upscaler context plus an owning shared-resource context. */
  sdk->scratch_size = ffxGetScratchMemorySizeVK(config->physical_device, 2u);
  sdk->scratch_arena = arena_create_internal(
      sdk->scratch_size, sdk->scratch_size, ARENA_DEFAULT_FLAGS);
  if (!sdk->scratch_arena) {
    vkr_allocator_free(config->allocator, sdk, sizeof(*sdk),
                       VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
    return VKR_VULKAN_FSR_SDK_OUT_OF_MEMORY;
  }
  sdk->scratch = arena_alloc(sdk->scratch_arena, sdk->scratch_size,
                             ARENA_MEMORY_TAG_VULKAN);
  if (!sdk->scratch) {
    arena_destroy(sdk->scratch_arena);
    vkr_allocator_free(config->allocator, sdk, sizeof(*sdk),
                       VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
    return VKR_VULKAN_FSR_SDK_OUT_OF_MEMORY;
  }

  VkDeviceContext device_context = {};
  device_context.vkDevice = config->device;
  device_context.vkPhysicalDevice = config->physical_device;
  device_context.vkDeviceProcAddr = config->get_device_proc_addr;
  const FfxDevice device = ffxGetDeviceVK(&device_context);
  FfxFsr3UpscalerSharedResourceDescriptions shared = {};
  FfxErrorCode result = ffxGetInterfaceVK(&sdk->backend, device, sdk->scratch,
                                          sdk->scratch_size, 2u);
  if (result != FFX_OK) {
    vkr_fsr_destroy_scratch(sdk);
    vkr_allocator_free(config->allocator, sdk, sizeof(*sdk),
                       VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
    return vkr_fsr_result(result);
  }

  FfxFsr3UpscalerContextDescription description = {};
  description.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                      FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE |
                      FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
  description.maxRenderSize = {config->max_render_extent.width,
                               config->max_render_extent.height};
  description.maxUpscaleSize = {config->max_output_extent.width,
                                config->max_output_extent.height};
  description.fpMessage = NULL;
  description.backendInterface = sdk->backend;
  result = ffxFsr3UpscalerContextCreate(&sdk->context, &description);
  if (result != FFX_OK) {
    vkr_fsr_destroy_scratch(sdk);
    vkr_allocator_free(config->allocator, sdk, sizeof(*sdk),
                       VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
    return vkr_fsr_result(result);
  }

  result = sdk->backend.fpCreateBackendContext(
      &sdk->backend, FFX_EFFECT_SHAREDRESOURCES, NULL, &sdk->shared_context_id);
  if (result != FFX_OK)
    goto cleanup_context;
  sdk->shared_context_created = true_v;

  result = ffxFsr3UpscalerGetSharedResourceDescriptions(&sdk->context, &shared);
  if (result != FFX_OK)
    goto cleanup_shared;
  result = sdk->backend.fpCreateResource(&sdk->backend, &shared.dilatedDepth,
                                         sdk->shared_context_id,
                                         &sdk->dilated_depth);
  if (result != FFX_OK)
    goto cleanup_shared;
  sdk->dilated_depth_created = true_v;
  result = sdk->backend.fpCreateResource(
      &sdk->backend, &shared.dilatedMotionVectors, sdk->shared_context_id,
      &sdk->dilated_motion);
  if (result != FFX_OK)
    goto cleanup_shared;
  sdk->dilated_motion_created = true_v;
  result = sdk->backend.fpCreateResource(
      &sdk->backend, &shared.reconstructedPrevNearestDepth,
      sdk->shared_context_id, &sdk->reconstructed_previous_depth);
  if (result != FFX_OK)
    goto cleanup_shared;
  sdk->reconstructed_previous_depth_created = true_v;

  sdk->max_render_extent = config->max_render_extent;
  sdk->max_output_extent = config->max_output_extent;
  log_info("FSR 3.1.4 SDK scratch arena: request=%zu reserve=%llu commit=%llu",
           sdk->scratch_size, (unsigned long long)sdk->scratch_arena->rsv,
           (unsigned long long)sdk->scratch_arena->cmt);
  *out_sdk = sdk;
  return VKR_VULKAN_FSR_SDK_SUCCESS;

cleanup_shared:
  vkr_fsr_destroy_shared_resources(sdk);
cleanup_context:
  ffxFsr3UpscalerContextDestroy(&sdk->context);
  vkr_fsr_destroy_scratch(sdk);
  vkr_allocator_free(config->allocator, sdk, sizeof(*sdk),
                     VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
  return vkr_fsr_result(result);
}

extern "C" void vkr_vulkan_fsr_sdk_destroy(VkrVulkanFsrSdk *sdk) {
  if (!sdk)
    return;
  ffxFsr3UpscalerContextDestroy(&sdk->context);
  vkr_fsr_destroy_shared_resources(sdk);
  vkr_fsr_destroy_scratch(sdk);
  vkr_allocator_free(sdk->allocator, sdk, sizeof(*sdk),
                     VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
}

extern "C" VkrVulkanFsrSdkResult
vkr_vulkan_fsr_sdk_dispatch(VkrVulkanFsrSdk *sdk,
                            const VkrVulkanFsrSdkDispatch *dispatch) {
  if (!sdk || !dispatch || dispatch->command_buffer == VK_NULL_HANDLE ||
      !vkr_fsr_image_valid(&dispatch->hdr_color) ||
      !vkr_fsr_image_valid(&dispatch->depth) ||
      !vkr_fsr_image_valid(&dispatch->motion) ||
      !vkr_fsr_image_valid(&dispatch->output) ||
      dispatch->frame_time_ms <= 0.0f || dispatch->camera_near <= 0.0f ||
      dispatch->camera_far <= dispatch->camera_near ||
      dispatch->camera_fov_y_radians <= 0.0f || dispatch->sharpness != 0.0f)
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;

  if (!vkr_fsr_extent_within(dispatch->hdr_color.extent,
                             sdk->max_render_extent) ||
      !vkr_fsr_extent_within(dispatch->output.extent, sdk->max_output_extent))
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;

  if (dispatch->hdr_color.extent.width != dispatch->depth.extent.width ||
      dispatch->hdr_color.extent.height != dispatch->depth.extent.height ||
      dispatch->hdr_color.extent.width != dispatch->motion.extent.width ||
      dispatch->hdr_color.extent.height != dispatch->motion.extent.height)
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;
  if ((dispatch->reactive.image != VK_NULL_HANDLE &&
       (dispatch->reactive.extent.width != dispatch->hdr_color.extent.width ||
        dispatch->reactive.extent.height !=
            dispatch->hdr_color.extent.height)) ||
      (dispatch->composition.image != VK_NULL_HANDLE &&
       (dispatch->composition.extent.width !=
            dispatch->hdr_color.extent.width ||
        dispatch->composition.extent.height !=
            dispatch->hdr_color.extent.height)))
    return VKR_VULKAN_FSR_SDK_INVALID_ARGUMENT;

  FfxResource color;
  FfxResource depth;
  FfxResource motion;
  FfxResource reactive;
  FfxResource composition;
  FfxResource output;
  VkrVulkanFsrSdkResult result = vkr_fsr_resource(
      &dispatch->hdr_color, FFX_RESOURCE_USAGE_READ_ONLY, &color);
  if (result != VKR_VULKAN_FSR_SDK_SUCCESS)
    return result;
  result =
      vkr_fsr_resource(&dispatch->depth, FFX_RESOURCE_USAGE_READ_ONLY, &depth);
  if (result != VKR_VULKAN_FSR_SDK_SUCCESS)
    return result;
  result = vkr_fsr_resource(&dispatch->motion, FFX_RESOURCE_USAGE_READ_ONLY,
                            &motion);
  if (result != VKR_VULKAN_FSR_SDK_SUCCESS)
    return result;
  result = vkr_fsr_optional_resource(&dispatch->reactive, &reactive);
  if (result != VKR_VULKAN_FSR_SDK_SUCCESS)
    return result;
  result = vkr_fsr_optional_resource(&dispatch->composition, &composition);
  if (result != VKR_VULKAN_FSR_SDK_SUCCESS)
    return result;
  result = vkr_fsr_resource(&dispatch->output, FFX_RESOURCE_USAGE_UAV, &output);
  if (result != VKR_VULKAN_FSR_SDK_SUCCESS)
    return result;

  FfxFsr3UpscalerDispatchDescription description = {};
  description.commandList = ffxGetCommandListVK(dispatch->command_buffer);
  description.color = color;
  description.depth = depth;
  description.motionVectors = motion;
  description.reactive = reactive;
  description.transparencyAndComposition = composition;
  description.dilatedDepth =
      sdk->backend.fpGetResource(&sdk->backend, sdk->dilated_depth);
  description.dilatedMotionVectors =
      sdk->backend.fpGetResource(&sdk->backend, sdk->dilated_motion);
  description.reconstructedPrevNearestDepth = sdk->backend.fpGetResource(
      &sdk->backend, sdk->reconstructed_previous_depth);
  description.output = output;
  description.jitterOffset = {dispatch->jitter_x, dispatch->jitter_y};
  description.motionVectorScale = {dispatch->motion_scale_x,
                                   dispatch->motion_scale_y};
  description.renderSize = {dispatch->hdr_color.extent.width,
                            dispatch->hdr_color.extent.height};
  description.upscaleSize = {dispatch->output.extent.width,
                             dispatch->output.extent.height};
  description.enableSharpening = false;
  description.sharpness = 0.0f;
  description.frameTimeDelta = dispatch->frame_time_ms;
  description.preExposure = 1.0f;
  description.reset = dispatch->reset != 0;
  description.cameraNear = dispatch->camera_near;
  description.cameraFar = dispatch->camera_far;
  description.cameraFovAngleVertical = dispatch->camera_fov_y_radians;
  description.viewSpaceToMetersFactor = 1.0f;
  return vkr_fsr_result(
      ffxFsr3UpscalerContextDispatch(&sdk->context, &description));
}

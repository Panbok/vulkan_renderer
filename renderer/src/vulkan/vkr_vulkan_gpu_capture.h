#pragma once

#include "defines.h"
#include "platform/vkr_platform.h"

#include <vulkan/vulkan.h>

/*
 * RenderDoc frame captures for debug.gpu_capture (ADR-127). RenderDoc
 * captures Vulkan through its implicit layer, which the loader enables when
 * ENABLE_VULKAN_RENDERDOC_CAPTURE is 1 at instance creation: RenderDoc sets it
 * for a process it launches, and VKR_RENDERDOC_CAPTURE=1 asks the engine to
 * set it. Once the instance exists, the layer's renderdoc library is in the
 * process and its in-application API drives the captures. Without RenderDoc
 * every call here does nothing and reports the tool unavailable.
 */
typedef struct VkrVulkanGpuCaptureTool {
  VkrPlatformLibrary library;
  /* RENDERDOC_API_1_6_0, kept opaque so only the capture unit includes the
     vendor header. */
  void *api;
} VkrVulkanGpuCaptureTool;

/** Before vkCreateInstance: turns RenderDoc's layer on when
 * VKR_RENDERDOC_CAPTURE asks for it. */
void vkr_vulkan_gpu_capture_tool_prepare(void);
/** After vkCreateInstance: binds the RenderDoc API when its layer loaded. */
void vkr_vulkan_gpu_capture_tool_open(VkrVulkanGpuCaptureTool *tool);
void vkr_vulkan_gpu_capture_tool_close(VkrVulkanGpuCaptureTool *tool);
bool8_t
vkr_vulkan_gpu_capture_tool_available(const VkrVulkanGpuCaptureTool *tool);
/** Starts capturing every call on `instance`'s devices; RenderDoc names the
 * file from `path_template` (a path without extension). */
bool8_t vkr_vulkan_gpu_capture_tool_start(const VkrVulkanGpuCaptureTool *tool,
                                          VkInstance instance,
                                          const char *path_template);
/** Ends the capture and copies the written .rdc path; false when RenderDoc
 * wrote nothing. */
bool8_t vkr_vulkan_gpu_capture_tool_end(const VkrVulkanGpuCaptureTool *tool,
                                        VkInstance instance, char *out_path,
                                        uint32_t capacity);

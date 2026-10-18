#include "vulkan/vkr_vulkan_gpu_capture.h"

#include "containers/str.h"
#include "core/logger.h"

#include "renderdoc/renderdoc_app.h"

#if defined(_WIN32)
#define VKR_RENDERDOC_LIBRARY "renderdoc.dll"
#else
#define VKR_RENDERDOC_LIBRARY "librenderdoc.so"
#endif

void vkr_vulkan_gpu_capture_tool_prepare(void) {
  if (!vkr_platform_environment_flag("VKR_RENDERDOC_CAPTURE")) {
    return;
  }
  /* The loader reads the layer's enable variable at instance creation. */
  if (!vkr_platform_environment_set("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1")) {
    log_warn("VKR_RENDERDOC_CAPTURE: could not enable RenderDoc's layer");
  }
}

void vkr_vulkan_gpu_capture_tool_open(VkrVulkanGpuCaptureTool *tool) {
  *tool = (VkrVulkanGpuCaptureTool){0};
  /* Only a process whose loader enabled RenderDoc's layer has the library;
     opening it by name then returns that loaded module instead of loading a
     second, unhooked copy. */
  if (!vkr_platform_environment_flag("ENABLE_VULKAN_RENDERDOC_CAPTURE")) {
    return;
  }
  char error[256];
  if (!vkr_platform_library_open(VKR_RENDERDOC_LIBRARY, &tool->library, error,
                                 sizeof(error))) {
    log_warn("RenderDoc's layer was requested but %s is not loaded: %s",
             VKR_RENDERDOC_LIBRARY, error);
    return;
  }
  const pRENDERDOC_GetAPI get_api =
      (pRENDERDOC_GetAPI)vkr_platform_library_symbol(&tool->library,
                                                     "RENDERDOC_GetAPI");
  RENDERDOC_API_1_6_0 *api = NULL;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_6_0, (void **)&api) ||
      !api) {
    log_warn("RenderDoc's in-application API 1.6 is unavailable");
    vkr_platform_library_close(&tool->library);
    return;
  }
  tool->api = api;
  int major = 0;
  int minor = 0;
  int patch = 0;
  api->GetAPIVersion(&major, &minor, &patch);
  log_info("RenderDoc capture available (API %d.%d.%d)", major, minor, patch);
}

void vkr_vulkan_gpu_capture_tool_close(VkrVulkanGpuCaptureTool *tool) {
  if (!tool) {
    return;
  }
  if (tool->library.handle) {
    vkr_platform_library_close(&tool->library);
  }
  *tool = (VkrVulkanGpuCaptureTool){0};
}

bool8_t
vkr_vulkan_gpu_capture_tool_available(const VkrVulkanGpuCaptureTool *tool) {
  return tool && tool->api;
}

bool8_t vkr_vulkan_gpu_capture_tool_start(const VkrVulkanGpuCaptureTool *tool,
                                          VkInstance instance,
                                          const char *path_template) {
  if (!vkr_vulkan_gpu_capture_tool_available(tool) || !instance ||
      !path_template) {
    return false_v;
  }
  RENDERDOC_API_1_6_0 *api = tool->api;
  api->SetCaptureFilePathTemplate(path_template);
  /* A null window captures every swapchain and offscreen frame of the
     instance's devices, which the headless editor needs. */
  api->StartFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance),
                         NULL);
  return true_v;
}

bool8_t vkr_vulkan_gpu_capture_tool_end(const VkrVulkanGpuCaptureTool *tool,
                                        VkInstance instance, char *out_path,
                                        uint32_t capacity) {
  if (!vkr_vulkan_gpu_capture_tool_available(tool) || !instance || !out_path ||
      capacity == 0u) {
    return false_v;
  }
  out_path[0] = '\0';
  RENDERDOC_API_1_6_0 *api = tool->api;
  if (!api->EndFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance),
                            NULL)) {
    return false_v;
  }
  const uint32_t count = api->GetNumCaptures();
  uint32_t length = 0u;
  if (count == 0u || !api->GetCapture(count - 1u, NULL, &length, NULL) ||
      length == 0u || length > capacity) {
    return false_v;
  }
  return api->GetCapture(count - 1u, out_path, &length, NULL) ? true_v
                                                              : false_v;
}

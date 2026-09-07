#include "core/logger.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "vkr_renderer.h"

/* A custom application can own its loop and assets without linking runtime.
 * Run from the repository root so the authored render graph is available. */
int main(void) {
  int result = 1;
  if (!vkr_platform_init())
    return result;
  Arena *logs = arena_create(MB(1), KB(64));
  if (!logs || !log_init(logs)) {
    arena_destroy(logs);
    vkr_platform_shutdown();
    return result;
  }
  VkrDMemory memory = {0};
  if (!vkr_dmemory_create(MB(1), MB(8), &memory))
    goto cleanup_logs;
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrDeviceRequirements requirements = {
      .supported_stages =
          VKR_SHADER_STAGE_VERTEX_BIT | VKR_SHADER_STAGE_FRAGMENT_BIT,
      .supported_queues =
          VKR_DEVICE_QUEUE_GRAPHICS_BIT | VKR_DEVICE_QUEUE_TRANSFER_BIT,
      .allowed_device_types =
          VKR_DEVICE_TYPE_DISCRETE_BIT | VKR_DEVICE_TYPE_INTEGRATED_BIT,
      .supported_sampler_filters = VKR_SAMPLER_FILTER_ANISOTROPIC_BIT,
  };
  const VkrRendererBackendConfig config = {
      .present_target = {VKR_PRESENT_TARGET_OFFSCREEN, 64u, 64u, 2u},
  };
#if defined(PLATFORM_APPLE)
  const VkrRendererBackendType backend = VKR_RENDERER_BACKEND_TYPE_METAL;
#else
  const VkrRendererBackendType backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
#endif
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrRenderer *renderer = vkr_renderer_create(&allocator, backend, NULL,
                                              &requirements, &config, &error);
  if (renderer) {
    const VkrFrameConfig frame_config = {.shadow_map_size = 256u,
                                         .shadow_cascade_count = 1u};
    VkrFrame frame = {0};
    error = vkr_renderer_begin_frame(renderer, &frame_config, &frame);
    if (error == VKR_RENDERER_ERROR_NONE)
      error = vkr_renderer_cancel_frame(&frame);
    result = error == VKR_RENDERER_ERROR_NONE ? 0 : 1;
    vkr_renderer_release(renderer);
  }
  if (result)
    fprintf(stderr, "Renderer client failed: %d\n", (int)error);
  vkr_allocator_release_global_accounting(&allocator);
  vkr_dmemory_destroy(&memory);
cleanup_logs:
  log_shutdown();
  arena_destroy(logs);
  vkr_platform_shutdown();
  return result;
}

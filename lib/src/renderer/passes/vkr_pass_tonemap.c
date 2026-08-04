#include "renderer/passes/vkr_pass_tonemap.h"

#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_render_packet.h"

vkr_internal void vkr_pass_tonemap_execute(VkrRgPassContext *ctx,
                                           void *user_data) {
  (void)user_data;
  if (!ctx || !ctx->renderer || !ctx->graph) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  const VkrRenderPacket *packet = vkr_rg_pass_get_packet(ctx);
  VkrRgImageHandle source =
      vkr_rg_find_image(ctx->graph, string8_lit("hdr_scene_color"));
  VkrTextureOpaqueHandle source_texture =
      vkr_rg_get_image_texture(ctx->graph, source, ctx->image_index);
  if (!packet || !source_texture) {
    ctx->error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return;
  }

  const VkrRendererError error = vkr_world_resources_record_tonemap(
      rf, &rf->world_resources, source_texture, packet->frame.window_width,
      packet->frame.window_height, packet->globals.exposure);
  if (error != VKR_RENDERER_ERROR_NONE) {
    ctx->error = error;
  }
}

bool8_t vkr_pass_tonemap_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }
  VkrRgPassExecutor entry = {
      .name = string8_lit("pass.tonemap"),
      .execute = vkr_pass_tonemap_execute,
      .user_data = NULL,
  };
  return vkr_rg_executor_registry_register(registry, &entry);
}

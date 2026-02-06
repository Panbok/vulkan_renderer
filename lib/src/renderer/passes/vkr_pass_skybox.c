#include "renderer/passes/vkr_pass_skybox.h"

#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_skybox_system.h"
#include "renderer/vkr_render_packet.h"

vkr_internal void vkr_pass_skybox_execute(VkrRgPassContext *ctx,
                                          void *user_data) {
  (void)user_data;

  if (!ctx || !ctx->renderer) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  const VkrRenderPacket *packet = vkr_rg_pass_get_packet(ctx);
  const VkrSkyboxPassPayload *payload = vkr_rg_pass_get_skybox_payload(ctx);
  if (!packet || !payload) {
    return;
  }

  if (rf->skybox_system.initialized) {
    vkr_skybox_system_render_packet(rf, &rf->skybox_system, payload,
                                    &packet->globals);
  }
}

bool8_t vkr_pass_skybox_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }

  VkrRgPassExecutor entry = {
      .name = string8_lit("pass.skybox"),
      .execute = vkr_pass_skybox_execute,
      .user_data = NULL,
  };

  return vkr_rg_executor_registry_register(registry, &entry);
}

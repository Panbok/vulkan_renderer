#include "renderer/passes/vkr_pass_skybox.h"

#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_skybox_system.h"
#include "renderer/systems/vkr_texture_system.h"
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

  VkrSkyboxPassPayload effective_payload = *payload;
  if (effective_payload.cubemap.id == 0 && rf->active_scene &&
      rf->active_scene->environment.bake_state ==
          VKR_SCENE_ENV_BAKE_STATE_READY &&
      rf->active_scene->environment.source_cubemap.id != 0) {
    VkrTexture *scene_cubemap = vkr_texture_system_get_by_handle(
        &rf->texture_system, rf->active_scene->environment.source_cubemap);
    if (scene_cubemap && scene_cubemap->handle &&
        scene_cubemap->description.type == VKR_TEXTURE_TYPE_CUBE_MAP) {
      effective_payload.cubemap = rf->active_scene->environment.source_cubemap;
    }
  }
  if (effective_payload.cubemap.id == 0 &&
      rf->world_resources.ibl_default_ready &&
      rf->world_resources.ibl_fallback_source_cubemap.id != 0) {
    effective_payload.cubemap = rf->world_resources.ibl_fallback_source_cubemap;
  }

  if (rf->skybox_system.initialized) {
    vkr_skybox_system_render_packet(rf, &rf->skybox_system, &effective_payload,
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

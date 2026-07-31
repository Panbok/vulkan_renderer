#include "renderer/passes/vkr_pass_ibl_bake.h"

#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_world_resources.h"

vkr_internal void vkr_pass_ibl_bake_execute(VkrRgPassContext *ctx,
                                            void *user_data) {
  (void)user_data;

  if (!ctx || !ctx->renderer) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  if (!rf->world_resources.initialized) {
    return;
  }

  if (!vkr_world_resources_ensure_default_ibl_ready(rf, &rf->world_resources)) {
    return;
  }

  if (rf->active_scene) {
    vkr_world_resources_bake_scene_ibl_if_pending(rf, &rf->world_resources,
                                                  rf->active_scene);
    vkr_world_resources_bake_scene_reflection_probes_if_pending(
        rf, &rf->world_resources, rf->active_scene);
  }

  vkr_world_resources_set_active_ibl_from_scene_or_default(
      rf, &rf->world_resources, rf->active_scene);
  vkr_world_resources_apply_active_ibl_to_material_system(
      rf, &rf->world_resources);
}

bool8_t vkr_pass_ibl_bake_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }

  VkrRgPassExecutor entry = {
      .name = string8_lit("pass.ibl_bake"),
      .execute = vkr_pass_ibl_bake_execute,
      .user_data = NULL,
  };

  return vkr_rg_executor_registry_register(registry, &entry);
}

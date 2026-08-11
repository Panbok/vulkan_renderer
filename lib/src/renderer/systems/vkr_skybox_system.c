#include "renderer/systems/vkr_skybox_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_texture_system.h"

bool8_t vkr_skybox_system_init(RendererFrontend *rf, VkrSkyboxSystem *system) {
  if (!rf || !system) {
    return false_v;
  }

  MemZero(system, sizeof(*system));
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_load_cube_map(
          &rf->texture_system, string8_lit("assets/textures/skybox"),
          string8_lit("jpg"), &system->cube_map_texture, &error)) {
    String8 message = vkr_renderer_get_error_string(error);
    log_error("Skybox cubemap load failed: %s", string8_cstr(&message));
    return false_v;
  }
  system->initialized = true_v;
  return true_v;
}

void vkr_skybox_system_shutdown(RendererFrontend *rf, VkrSkyboxSystem *system) {
  if (!rf || !system) {
    return;
  }
  if (system->cube_map_texture.id != 0) {
    vkr_texture_system_release_by_handle(&rf->texture_system,
                                         system->cube_map_texture);
  }
  MemZero(system, sizeof(*system));
}

#include "renderer/systems/vkr_skybox_system.h"

bool8_t vkr_skybox_system_init(struct s_RendererFrontend *rf,
                               VkrSkyboxSystem *system) {
  if (!rf || !system) {
    return false_v;
  }

  MemZero(system, sizeof(*system));
  system->initialized = true_v;
  return true_v;
}

void vkr_skybox_system_shutdown(struct s_RendererFrontend *rf,
                                VkrSkyboxSystem *system) {
  if (!rf || !system) {
    return;
  }
  MemZero(system, sizeof(*system));
}

#pragma once

#include "defines.h"
#include "renderer/resources/vkr_resources.h"

struct s_RendererFrontend;

/** CPU-side skybox state consumed while assembling render packets. */
typedef struct VkrSkyboxSystem {
  VkrTextureHandle cube_map_texture;
  bool8_t initialized;
} VkrSkyboxSystem;

bool8_t vkr_skybox_system_init(struct s_RendererFrontend *rf,
                               VkrSkyboxSystem *system);
void vkr_skybox_system_shutdown(struct s_RendererFrontend *rf,
                                VkrSkyboxSystem *system);

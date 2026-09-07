#pragma once

#include "defines.h"

/** CPU-side skybox state consumed while assembling render packets. */
typedef struct VkrSkyboxSystem {
  bool8_t initialized;
} VkrSkyboxSystem;

bool8_t vkr_skybox_system_init(VkrSkyboxSystem *system);
void vkr_skybox_system_shutdown(VkrSkyboxSystem *system);

#pragma once

#include "defines.h"
#include "math/vec.h"

/** Scene-linear analytic atmosphere. Distances and heights use world units. */
typedef struct VkrFogSettings {
  bool8_t enabled;
  Vec3 color;
  float32_t density;
  float32_t base_height;
  float32_t height_falloff;
  float32_t max_distance;
  float32_t sky_distance;
} VkrFogSettings;

/** Borrowed by shaders through completed frame-slot storage. */
typedef struct VkrFogGpuParams {
  Vec4 color_density;
  Vec4 height_distance;
} VkrFogGpuParams;

_Static_assert(sizeof(VkrFogGpuParams) == 32u, "Fog parameter ABI size drift");

VkrFogSettings vkr_fog_settings_defaults(void);
bool8_t vkr_fog_settings_valid(const VkrFogSettings *settings);
VkrFogGpuParams vkr_fog_prepare(const VkrFogSettings *settings);

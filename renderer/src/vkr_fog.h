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
  /** Gray single-scattering albedo in [0,1] of a medium lit by the published
      atmosphere's sun and sky light. Zero keeps the constant `color`
      in-scatter, which also applies whenever no atmosphere is published. */
  float32_t sky_lighting;
  /** Henyey-Greenstein anisotropy of the sun lobe in [-0.95,0.95]. */
  float32_t anisotropy;
} VkrFogSettings;

/** Borrowed by shaders through completed frame-slot storage. `sky_lighting`
    holds the strength, zero without a published atmosphere, and the sun-lobe
    anisotropy. */
typedef struct VkrFogGpuParams {
  Vec4 color_density;
  Vec4 height_distance;
  Vec4 sky_lighting;
} VkrFogGpuParams;

_Static_assert(sizeof(VkrFogGpuParams) == 48u, "Fog parameter ABI size drift");

VkrFogSettings vkr_fog_settings_defaults(void);
bool8_t vkr_fog_settings_valid(const VkrFogSettings *settings);
VkrFogGpuParams vkr_fog_prepare(const VkrFogSettings *settings);

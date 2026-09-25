#pragma once

#include <vector>

extern "C" {
#include "vkr_atmosphere.h"
}

/* Scene-owned, immutable atmosphere source. The temporary LUTs used to build
   it do not escape this module. RGB intentionally excludes the solar disc;
   `observer_irradiance` is the matching direct-light value. */
struct VkrBakeAtmosphere {
  bool8_t enabled = false_v;
  VkrAtmosphereGpuParams params = {};
  Vec3 observer_irradiance = {};
  std::vector<Vec3> source_rgb;
};

/* Version 2 adds the sunlit ground to below-horizon source radiance. */
constexpr uint32_t VKR_BAKE_ATMOSPHERE_MODEL_VERSION = 2u;

bool vkr_bake_atmosphere_build(VkrBakeAtmosphere *out,
                               const VkrAtmosphereSettings *settings);
/* `atmosphere` is an enabled, completely built scene record and direction is
   a finite unit vector. Callers select this sampler at the scene boundary. */
Vec3 vkr_bake_atmosphere_sample(const VkrBakeAtmosphere *atmosphere,
                                Vec3 unit_direction);
uint64_t vkr_bake_atmosphere_recipe_hash(const VkrBakeAtmosphere *atmosphere);

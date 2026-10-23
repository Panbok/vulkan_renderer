#pragma once

#include <vector>

extern "C" {
#include "vkr_atmosphere.h"
}

/* Scene-owned, immutable atmosphere source. The temporary LUTs used to build
   it do not escape this module. RGB intentionally excludes the solar and
   lunar discs; `observer_irradiance` is the matching direct-light value of the
   key light, the sun while it lights the observer, else the moon (ADR-081),
   toward `key_light_direction`. */
struct VkrBakeAtmosphere {
  bool8_t enabled = false_v;
  VkrAtmosphereGpuParams params = {};
  Vec3 observer_irradiance = {};
  Vec3 key_light_direction = {};
  std::vector<Vec3> source_rgb;
};

/* Version 2 adds the sunlit ground to below-horizon source radiance;
   version 3 shadows directional lights, as the runtime shadows its sun,
   where earlier bakes let their light through walls. */
constexpr uint32_t VKR_BAKE_ATMOSPHERE_MODEL_VERSION = 3u;

bool vkr_bake_atmosphere_build(VkrBakeAtmosphere *out,
                               const VkrAtmosphereSettings *settings);
/* As vkr_bake_atmosphere_build, reading the result from `cache_directory`
   when an earlier build of the same prepared parameters left it there and
   writing it there otherwise; an empty directory builds without a cache.
   A sun key's atmosphere takes about a second on a Level Design Test bake,
   and every lightmap and volume bake builds the same eight. The bakery
   names the directory by the atmosphere code's identity. */
bool vkr_bake_atmosphere_build_cached(VkrBakeAtmosphere *out,
                                      const VkrAtmosphereSettings *settings,
                                      const char *cache_directory);
/* `atmosphere` is an enabled, completely built scene record and direction is
   a finite unit vector. Callers select this sampler at the scene boundary. */
Vec3 vkr_bake_atmosphere_sample(const VkrBakeAtmosphere *atmosphere,
                                Vec3 unit_direction);
uint64_t vkr_bake_atmosphere_recipe_hash(const VkrBakeAtmosphere *atmosphere);

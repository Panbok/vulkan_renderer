#include "bake/vkr_bake_layers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

/* Unit `v` turned by `angle` radians about the unit `axis`, right-handed. */
Vec3 rotate_about(Vec3 v, Vec3 axis, float32_t angle) {
  const float32_t c = std::cos(angle);
  const float32_t s = std::sin(angle);
  return vec3_normalize(
      vec3_add(vec3_add(vec3_scale(v, c), vec3_scale(vec3_cross(axis, v), s)),
               vec3_scale(axis, vec3_dot(axis, v) * (1.0f - c))));
}

} // namespace

std::vector<Vec3> vkr_bake_sun_key_directions(const VkrBakeScene &scene) {
  const Vec3 pole = vec3_normalize(scene.atmosphere_settings.celestial_pole);
  const Vec3 sun = vec3_normalize(scene.atmosphere_settings.sun_direction);
  std::vector<Vec3> directions;
  for (uint32_t k = 0u; k < VKR_BAKE_SUN_KEY_COUNT; ++k) {
    directions.push_back(rotate_about(sun, pole,
                                      6.28318530718f * (float32_t)k /
                                          (float32_t)VKR_BAKE_SUN_KEY_COUNT));
  }
  return directions;
}

bool vkr_bake_plan_layers(const VkrBakeScene &scene,
                          const std::vector<Vec3> &sun_keys,
                          std::vector<VkrBakeLayerPlan> *out_layers,
                          std::string *out_error) {
  const bool sky = scene.environment.enabled &&
                   scene.environment.kind != VkrBakeSceneEnvironmentKind::None;
  std::vector<VkrBakeLayerPlan> layers;
  std::vector<uint32_t> lamp_lights;
  bool has_sun = false;
  if (!sun_keys.empty()) {
    for (uint32_t k = 0u; k < sun_keys.size(); ++k) {
      VkrBakeLayerPlan key = {};
      key.record.kind = VKR_LIGHT_LAYER_SUN_KEY;
      key.record.index = k;
      key.record.sun_direction = sun_keys[k];
      key.lights.push_back(scene.atmosphere_light);
      key.sky = sky;
      key.sun_key = (int32_t)k;
      layers.push_back(key);
    }
    has_sun = true;
    for (uint32_t i = 0u; i < scene.lights.size(); ++i) {
      if (i != scene.atmosphere_light) {
        lamp_lights.push_back(i);
      }
    }
  } else {
    VkrBakeLayerPlan sun = {};
    for (uint32_t i = 0u; i < scene.lights.size(); ++i) {
      const VkrBakeSceneLight &light = scene.lights[i];
      if (light.kind != VkrBakeSceneLightKind::Directional) {
        lamp_lights.push_back(i);
        continue;
      }
      sun.lights.push_back(i);
      if (!has_sun) {
        has_sun = true;
        sun.record.sun_direction = vec3_normalize(vec3_new(
            -light.direction.x, -light.direction.y, -light.direction.z));
      }
    }
    if (has_sun) {
      sun.record.kind = VKR_LIGHT_LAYER_SUN_KEY;
      sun.record.index = 0u;
      sun.sky = sky;
      layers.push_back(sun);
    }
  }

  // A light without a group, such as a directional light, is in the default.
  auto group_of = [&](uint32_t light) {
    const char *group = scene.lights[light].group;
    return std::string(group[0] ? group : VKR_LIGHT_GROUP_DEFAULT);
  };
  std::vector<std::string> groups = {VKR_LIGHT_GROUP_DEFAULT};
  for (uint32_t i : lamp_lights) {
    const std::string group = group_of(i);
    if (scene.lights[i].enabled &&
        std::find(groups.begin(), groups.end(), group) == groups.end()) {
      groups.push_back(group);
    }
  }
  std::sort(groups.begin() + 1, groups.end());
  if (groups.size() > VKR_LIGHT_LAYER_MAX_LAMP_GROUPS) {
    char message[160];
    std::snprintf(message, sizeof(message),
                  "The scene's static lights make %zu light groups, the "
                  "default group included; a bake holds at most %u",
                  groups.size(), VKR_LIGHT_LAYER_MAX_LAMP_GROUPS);
    if (out_error) {
      *out_error = message;
    }
    return false;
  }
  for (uint32_t g = 0u; g < groups.size(); ++g) {
    VkrBakeLayerPlan lamps = {};
    lamps.record.kind = VKR_LIGHT_LAYER_LAMP_GROUP;
    lamps.record.index = g;
    std::snprintf(lamps.record.name, sizeof(lamps.record.name), "%s",
                  groups[g].c_str());
    for (uint32_t i : lamp_lights) {
      if (scene.lights[i].enabled && groups[g] == group_of(i)) {
        lamps.lights.push_back(i);
      }
    }
    lamps.emission = g == 0u;
    lamps.sky = g == 0u && sky && !has_sun;
    layers.push_back(lamps);
  }
  *out_layers = std::move(layers);
  return true;
}

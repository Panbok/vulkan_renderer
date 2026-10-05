#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vkr_bake_scene.h"

/* Sun keys of an atmosphere scene (owner decision 2026-10-05: eight keys on
   the sun's daily circle). */
constexpr uint32_t VKR_BAKE_SUN_KEY_COUNT = 8u;

/*
 * What one baked light layer gathers (ADR-088): its record, the scene lights
 * it holds, whether escaped paths see the sky and surfaces emit, and the sun
 * key whose atmosphere must be current while it bakes (-1 for none).
 * Lightmap and diffuse-volume bakes plan the same layers.
 */
struct VkrBakeLayerPlan {
  VkrLightLayer record = {};
  std::vector<uint32_t> lights;
  bool sky = false;
  bool emission = false;
  int32_t sun_key = -1;
};

/* The keys' sun directions in an atmosphere scene: the authored sun turned
   about the atmosphere's celestial pole in steps of 360 /
   VKR_BAKE_SUN_KEY_COUNT degrees of hour angle, right-handed, so the sun
   keeps its angle to the pole (its season). Key 0 is the authored sun. */
std::vector<Vec3> vkr_bake_sun_key_directions(const VkrBakeScene &scene);

/*
 * The layers this scene's lights make. In an atmosphere scene the sun keys
 * hold the bounce of the atmosphere's key light and the sky for each key's
 * sun. Without an atmosphere, sun key 0 holds the directional lights' bounce
 * and the sky; a scene without a directional light has no sun key, and its
 * sky is static light of the default group.
 *
 * Every other enabled light is static (the scene leaves dynamic ones out)
 * and joins its group's lamp layer. Lamp group 0 is the default group, which
 * also holds surface emission; the other groups follow in name order. More
 * than VKR_LIGHT_LAYER_MAX_LAMP_GROUPS groups fail with `out_error`.
 */
bool vkr_bake_plan_layers(const VkrBakeScene &scene,
                          const std::vector<Vec3> &sun_keys,
                          std::vector<VkrBakeLayerPlan> *out_layers,
                          std::string *out_error);

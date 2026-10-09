#pragma once

#include "renderer/systems/vkr_scene_system.h"

/*
 * Look volumes (ADR-097). A `look_volume` component overrides part of the
 * look inside a box: the values below, which otherwise come from the
 * scene's `post_process`, height `fog` and sky light. Once a frame the CPU
 * weighs every volume at the camera and blends the overridden values in
 * ascending priority, so the volumes add no GPU pass and no pipeline and
 * apply on both pipelines. The module owns no state.
 */

/** The values look volumes override. */
typedef struct VkrLook {
  float32_t exposure_compensation_ev;
  /** The EV range automatic exposure settles in; meaningful only while
      `metering` is set, else the renderer's range applies. */
  bool8_t metering;
  float32_t metering_min_ev;
  float32_t metering_max_ev;
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  float32_t contrast;
  float32_t saturation;
  float32_t bloom_intensity;
  Vec3 fog_color;
  float32_t fog_density;
  float32_t sky_light_intensity;
} VkrLook;

/** A volume placed for blending: its component and its box, the world
    transform of the unit box [-0.5, 0.5]^3. */
typedef struct VkrLookVolume {
  SceneLookVolume value;
  Mat4 box;
} VkrLookVolume;

/** How much `volume` applies at `point`: 1 inside its box, falling linearly
    to 0 at its blend distance outside it, measured in metres from the box's
    nearest point; 0 for a disabled volume. */
float32_t vkr_look_volume_weight(const VkrLookVolume *volume, Vec3 point);

/** Blends `volumes` into `look` at `point`: in ascending priority, equal
    priorities in array order, each overridden value moves toward the
    volume's by its weight. `volumes` is reordered. `metering_min_ev` and
    `metering_max_ev` of `look` must hold the renderer's range on entry. */
void vkr_look_blend(VkrLook *look, VkrLookVolume *volumes, uint32_t count,
                    Vec3 point);

/** The look volumes `scene` applies this frame, with their boxes from the
    entities' current transforms: the scene's own, then its World's. Returns
    the count written, at most `capacity`. */
uint32_t vkr_scene_look_volumes(const VkrScene *scene, VkrLookVolume *out,
                                uint32_t capacity);

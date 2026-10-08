#pragma once

#include "defines.h"
#include "math/vec.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_gpu_abi.h"

#include "vkr_lighting.h"

/** One light's state across calls of the tiled pipeline's dynamic-light
 * limit (ADR-087), matched by render id. */
typedef struct VkrPointLightLimitEntry {
  uint32_t render_id;
  /** Intensity scale in [0, 1]: rises toward one while the light is selected
   * and falls toward zero once it is not, so lights fade instead of popping. */
  float32_t weight;
  /** The light was among the selected lights; it ranks with the incumbent
   * bonus. */
  bool8_t selected;
  /** The light held a shadow; it ranks for shadows with the bonus. */
  bool8_t shadowed;
} VkrPointLightLimitEntry;

/**
 * @brief Lighting system for managing lighting data and applying to shaders.
 *
 * This system caches lighting data from the ECS and applies it to the shader
 * uniforms. It also tracks dirty state to avoid unnecessary updates.
 */
typedef struct VkrLightingSystem {
  // Cached GPU-ready data (updated from ECS each frame)
  struct {
    bool8_t enabled;
    Vec3 direction; // world space
    Vec3 color;
    float32_t intensity;
    float32_t sun_angular_diameter_degrees;
  } directional;

  VkrPointLight point_lights[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t point_light_count;
  uint32_t point_light_dropped_count;
  /* Static lights light only through baked data, the lightmaps and diffuse
     volumes, so the tables keep the dynamic lights (the tiled pipeline,
     ADR-087 and ADR-088). Read by the scene syncs. */
  bool8_t static_lights_baked;
  VkrPointLightGrid point_light_grid;

  VkrRectangleLight rectangle_lights[VKR_MAX_SCENE_RECTANGLE_LIGHTS];
  uint32_t rectangle_light_count;

  /* State of vkr_lighting_system_limit_point_lights from its last call, the
     table's identified lights that were selected or still showing, sorted
     by render id. The scene syncs leave it alone. */
  VkrPointLightLimitEntry point_light_limit[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t point_light_limit_count;
  Vec3 point_light_limit_camera;
  bool8_t point_light_limit_valid;

  // Dirty tracking
  bool8_t dirty;
} VkrLightingSystem;

/**
 * @brief Initializes the lighting system.
 * @param system The lighting system to initialize.
 * @return true_v if the lighting system was initialized successfully, false_v
 * otherwise.
 */
bool8_t vkr_lighting_system_init(VkrLightingSystem *system);

/**
 * @brief Shuts down the lighting system.
 * @param system The lighting system to shut down.
 */
void vkr_lighting_system_shutdown(VkrLightingSystem *system);

/**
 * @brief Syncs from the ECS.
 * The directional light comes from the sun that vkr_scene_sync_sun resolved.
 * @param system The lighting system to sync from the ECS.
 * @param scene The scene to sync from.
 */
void vkr_lighting_system_sync_from_scene(VkrLightingSystem *system,
                                         const VkrScene *scene);

/** Append an additive scene's point and rectangle lights after a sync; its
 * directional light and sun do not replace the rendered scene's (ADR-076). */
void vkr_lighting_system_append_scene(VkrLightingSystem *system,
                                      const VkrScene *scene);

/** A light that the dynamic-light limit selected or shadowed keeps its
 * place until a competitor is this factor nearer (ADR-087). */
#define VKR_POINT_LIGHT_LIMIT_INCUMBENT_BONUS 1.15f
/** Seconds for a light the limit selects or drops to fade in or out, as long
 * as a local shadow takes to fade in. */
#define VKR_POINT_LIGHT_LIMIT_FADE_SECONDS 0.25f

/** A white Lambertian surface lit head-on by a directional light of
 * irradiance E shows E * exposure / pi. Below this fraction of display white,
 * a sixteenth of an 8-bit step, the light is invisible (ADR-081). */
#define VKR_LIGHTING_KEY_LIGHT_VISIBLE_MIN (1.0f / 4096.0f)

/** Lights with the frame's atmosphere key light, the sun or the moon
 * (ADR-081): `toward_light` points from the observer toward it, `irradiance`
 * is its light attenuated to the observer and the diameter sizes the shadow
 * penumbra. With a known `display_exposure`, a light too dim to show is
 * disabled, so neither it nor its cascaded shadows render; zero keeps it. */
void vkr_lighting_system_apply_atmosphere_light(
    VkrLightingSystem *system, Vec3 toward_light, Vec3 irradiance,
    float32_t angular_diameter_degrees, float32_t display_exposure);

/** The tiled pipeline's bounded set of dynamic lights (ADR-087). Selects
 * the `light_max` point lights nearest `camera_position` by the distance to
 * their range, ties by the distance to their centre and then by table order,
 * and lets the `shadow_max` nearest selected casters keep their shadows. A
 * light selected or shadowed by the previous call ranks as if the camera
 * were VKR_POINT_LIGHT_LIMIT_INCUMBENT_BONUS times closer, so membership
 * does not flip while distances cross.
 *
 * A light with a unique nonzero render id fades: its intensity scale rises
 * by `delta_seconds` / VKR_POINT_LIGHT_LIMIT_FADE_SECONDS per call while
 * selected and falls as fast once not. A fading-out light keeps its table
 * slot and loses its shadow, which the local shadow cache fades out; a
 * newly selected light takes a slot once one is free, so at most
 * `light_max` lights draw. The first call, a camera cut and an invalid
 * `delta_seconds` snap every selected light to full intensity. Keeps the
 * table order of the drawn lights and rebuilds the grid when lights drop. */
void vkr_lighting_system_limit_point_lights(VkrLightingSystem *system,
                                            Vec3 camera_position,
                                            float32_t delta_seconds,
                                            uint32_t light_max,
                                            uint32_t shadow_max);

/** Rebuilds the conservative world-space lookup from point_lights. Public for
 * deterministic CPU coverage tests; scene sync calls it automatically. */
void vkr_lighting_system_build_point_light_grid(VkrLightingSystem *system);

/** Returns the global-plus-cell mask used for a world-space fragment. */
VkrPointLightMask
vkr_lighting_system_point_light_mask_at(const VkrLightingSystem *system,
                                        Vec3 world_position);

/** Tests whether one scene-table index is present in a mask. */
bool8_t
vkr_lighting_system_point_light_mask_contains(const VkrPointLightMask *mask,
                                              uint32_t light_index);

/**
 * @brief Applies the lighting system to the shader uniforms.
 * @param system The lighting system to apply to the shader uniforms.
 */

/**
 * @brief Marks the lighting system as dirty.
 * @param system The lighting system to mark as dirty.
 */
void vkr_lighting_system_mark_dirty(VkrLightingSystem *system);

/**
 * @brief Checks if any updates are pending.
 * @param system The lighting system to check if any updates are pending.
 * @return true_v if any updates are pending, false_v otherwise.
 */
bool8_t vkr_lighting_system_is_dirty(const VkrLightingSystem *system);

#pragma once

#include "defines.h"
#include "math/vec.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_gpu_abi.h"

#include "vkr_lighting.h"

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
  } directional;

  VkrPointLight point_lights[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t point_light_count;
  uint32_t point_light_dropped_count;
  VkrPointLightGrid point_light_grid;

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
 * @param system The lighting system to sync from the ECS.
 * @param scene The scene to sync from.
 */
void vkr_lighting_system_sync_from_scene(VkrLightingSystem *system,
                                         const VkrScene *scene);

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

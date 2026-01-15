#pragma once

#include "defines.h"
#include "math/vec.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_shader_system.h"

#define VKR_MAX_POINT_LIGHTS 16

/**
 * @brief Lighting system for managing lighting data and applying to shaders.
 *
 * This system caches lighting data from the ECS and applies it to the shader
 * uniforms. It also tracks dirty state to avoid unnecessary updates.
 */
typedef struct VkrLightingSystem {
  VkrShaderSystem *shader_system;

  // Cached GPU-ready data (updated from ECS each frame)
  struct {
    bool8_t enabled;
    Vec3 direction; // world space
    Vec3 color;
    float32_t intensity;
  } directional;

  struct {
    Vec3 position; // world space
    Vec3 color;
    float32_t intensity;
    float32_t constant;
    float32_t linear;
    float32_t quadratic;
  } point_lights[VKR_MAX_POINT_LIGHTS];
  uint32_t point_light_count;

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

/**
 * @brief Applies the lighting system to the shader uniforms.
 * @param system The lighting system to apply to the shader uniforms.
 */
void vkr_lighting_system_apply_uniforms(VkrLightingSystem *system);

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
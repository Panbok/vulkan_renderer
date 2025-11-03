/*
 * @file vkr_shader_system.h
 * @brief Shader system header file
 * @details Manages shader creation, lookup, and binding
 */
#pragma once

#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// Shader System - Manages shader creation, lookup, and binding
// =============================================================================

/**
 * @brief Shader system configuration
 * @details Configuration for the shader system
 */
typedef struct VkrShaderSystemConfig {
  uint16_t max_shader_count;
  uint8_t max_uniform_count;
  uint8_t max_global_textures;
  uint8_t max_instance_textures;
} VkrShaderSystemConfig;

/**
 * @brief Shader system
 * @details Manages shader creation, lookup, and binding
 */
typedef struct VkrShaderSystem {
  Arena *arena;
  VkrAllocator allocator;
  VkrShaderSystemConfig config;
  uint32_t shader_count;
  VkrHashTable_uint32_t name_to_id;
  Array_VkrShader shaders;
  Array_bool8_t active_shaders;
  // Current bindings (front-end state)
  uint32_t current_shader_id;
  struct VkrShader *current_shader;
  VkrPipelineRegistry *registry;
  struct VkrGeometrySystem *geometry_system; // optional: for layout-stride sync
  // Staging buffers for apply calls (dynamic allocation based on shader)
  // Runtime state for uploads
  VkrShaderStateObject instance_state;
  VkrRendererMaterialState material_state;
  // Raw staging per scope (sized by current shader config)
  uint8_t *global_staging;
  uint64_t global_staging_size;
  uint8_t *instance_staging;
  uint64_t instance_staging_size;
  uint8_t *local_staging; // push constants
  uint64_t local_staging_size;
} VkrShaderSystem;

// =============================================================================
// Shader System - Lifecycle
// =============================================================================

/**
 * @brief Initialize the shader system
 * @param state The shader system state
 * @param cfg The shader system configuration
 * @return true if the shader system was initialized successfully, false
 * otherwise
 */
bool8_t vkr_shader_system_initialize(VkrShaderSystem *state,
                                     VkrShaderSystemConfig cfg);

/**
 * @brief Shutdown the shader system
 * @param state The shader system state
 */
void vkr_shader_system_shutdown(VkrShaderSystem *state);

// =============================================================================
// Shader System - Creation & Lookup
// =============================================================================

/**
 * @brief Create a shader
 * @param state The shader system state
 * @param cfg The shader configuration
 * @return true if the shader was created successfully, false otherwise
 */
bool8_t vkr_shader_system_create(VkrShaderSystem *state,
                                 const VkrShaderConfig *cfg);

/**
 * @brief Get the ID of a shader by name
 * @param state The shader system state
 * @param shader_name The name of the shader
 * @return The ID of the shader, 0 if not found
 */
uint32_t vkr_shader_system_get_id(VkrShaderSystem *state,
                                  const char *shader_name);

/**
 * @brief Get a shader by ID
 * @param state The shader system state
 * @param shader_id The ID of the shader
 * @return The shader, NULL if not found
 */
VkrShader *vkr_shader_system_get_by_id(VkrShaderSystem *state,
                                       uint32_t shader_id);

/**
 * @brief Get a shader by name
 * @param state The shader system state
 * @param shader_name The name of the shader
 * @return The shader, NULL if not found
 */
VkrShader *vkr_shader_system_get(VkrShaderSystem *state,
                                 const char *shader_name);

// =============================================================================
// Shader System - Usage
// =============================================================================

/**
 * @brief Use a shader by name
 * @param state The shader system state
 * @param shader_name The name of the shader
 * @return true if the shader was used successfully, false otherwise
 */
bool8_t vkr_shader_system_use(VkrShaderSystem *state, const char *shader_name);

/**
 * @brief Use a shader by ID
 * @param state The shader system state
 * @param shader_id The ID of the shader
 * @return true if the shader was used successfully, false otherwise
 */
bool8_t vkr_shader_system_use_by_id(VkrShaderSystem *state, uint32_t shader_id);

// =============================================================================
// Shader System - Uniform/Sampler API
// =============================================================================

/**
 * @brief Get the index of a uniform by name
 * @param state The shader system state
 * @param shader The shader
 * @param uniform_name The name of the uniform
 * @return The index of the uniform, VKR_SHADER_INVALID_UNIFORM_INDEX if not
 * found
 */
uint32_t vkr_shader_system_uniform_index(VkrShaderSystem *state,
                                         VkrShader *shader,
                                         const char *uniform_name);

/**
 * @brief Set a uniform by name
 * @param state The shader system state
 * @param uniform_name The name of the uniform
 * @param value The value of the uniform
 * @return true if the uniform was set successfully, false otherwise
 */
bool8_t vkr_shader_system_uniform_set(VkrShaderSystem *state,
                                      const char *uniform_name,
                                      const void *value);

/**
 * @brief Set a sampler by name
 * @param state The shader system state
 * @param sampler_name The name of the sampler
 * @param t The texture handle
 * @return true if the sampler was set successfully, false otherwise
 */
bool8_t vkr_shader_system_sampler_set(VkrShaderSystem *state,
                                      const char *sampler_name,
                                      VkrTextureOpaqueHandle t);

/**
 * @brief Set a uniform by index
 * @param state The shader system state
 * @param index The index of the uniform
 * @param value The value of the uniform
 * @return true if the uniform was set successfully, false otherwise
 */
bool8_t vkr_shader_system_uniform_set_by_index(VkrShaderSystem *state,
                                               uint16_t index,
                                               const void *value);

/**
 * @brief Set a sampler by index
 * @param state The shader system state
 * @param index The index of the sampler
 * @param t The texture handle
 * @return true if the sampler was set successfully, false otherwise
 */
bool8_t vkr_shader_system_sampler_set_by_index(VkrShaderSystem *state,
                                               uint16_t index,
                                               VkrTextureOpaqueHandle t);

// =============================================================================
// Shader System - Apply/Bind
// =============================================================================

/**
 * @brief Apply the global state to the shader
 * @param state The shader system state
 * @return true if the global state was applied successfully, false otherwise
 */
bool8_t vkr_shader_system_apply_global(VkrShaderSystem *state);

/**
 * @brief Apply the instance state to the shader
 * @param state The shader system state
 * @return true if the instance state was applied successfully, false otherwise
 */
bool8_t vkr_shader_system_apply_instance(VkrShaderSystem *state);

/**
 * @brief Bind an instance to the shader
 * @param state The shader system state
 * @param instance_id The ID of the instance
 * @return true if the instance was bound successfully, false otherwise
 */
bool8_t vkr_shader_system_bind_instance(VkrShaderSystem *state,
                                        uint32_t instance_id);

// =============================================================================
// Shader System - Deletion
// =============================================================================

/**
 * @brief Delete a shader by name
 * @param state The shader system state
 * @param shader_name The name of the shader
 * @return true if the shader was deleted successfully, false otherwise
 */
bool8_t vkr_shader_system_delete(VkrShaderSystem *state,
                                 const char *shader_name);

/**
 * @brief Delete a shader by ID
 * @param state The shader system state
 * @param shader_id The ID of the shader
 * @return true if the shader was deleted successfully, false otherwise
 */
bool8_t vkr_shader_system_delete_by_id(VkrShaderSystem *state,
                                       uint32_t shader_id);

// =============================================================================
// Shader System - Instance Resource Lifetime
// =============================================================================

/**
 * @brief Acquire instance resources
 * @param state The shader system state
 * @param shader The shader
 * @param out_instance_id The ID of the instance
 * @return true if the instance resources were acquired successfully, false
 * otherwise
 */
bool8_t vkr_shader_acquire_instance_resources(VkrShaderSystem *state,
                                              VkrShader *shader,
                                              uint32_t *out_instance_id);

/**
 * @brief Release instance resources
 * @param state The shader system state
 * @param shader The shader
 * @param instance_id The ID of the instance
 * @return true if the instance resources were released successfully, false
 * otherwise
 */
bool8_t vkr_shader_release_instance_resources(VkrShaderSystem *state,
                                              VkrShader *shader,
                                              uint32_t instance_id);

// =============================================================================
// Shader System - Integration Hooks
// =============================================================================

/**
 * @brief Set the pipeline registry
 * @param state The shader system state
 * @param registry The pipeline registry
 */
void vkr_shader_system_set_registry(VkrShaderSystem *state,
                                    VkrPipelineRegistry *registry);

/**
 * @brief Set the geometry system for shader-driven layout/stride syncing
 */
void vkr_shader_system_set_geometry_system(
    VkrShaderSystem *state, struct VkrGeometrySystem *geometry_system);

#pragma once

#include "containers/array.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_renderer.h"

#define VKR_MATERIAL_NAME_MAX 128

// =============================================================================
// Material System - Basic materials management with array and hash table
// =============================================================================

typedef struct VkrMaterialSystemConfig {
  uint32_t max_material_count;
} VkrMaterialSystemConfig;

// Lifetime entry stored only in a hash table keyed by material name.
// 'id' is the index into the materials array. This structure manages
// references and auto-release behavior only.
typedef struct VkrMaterialEntry {
  uint32_t id;          // index into materials array
  uint32_t ref_count;   // number of holders
  bool8_t auto_release; // release when ref_count hits 0
  const char *name;     // material name (hash key)
} VkrMaterialEntry;
VkrHashTable(VkrMaterialEntry);

// Default arena sizing for material system internal allocators
#define VKR_MATERIAL_SYSTEM_DEFAULT_ARENA_RSV MB(8)
#define VKR_MATERIAL_SYSTEM_DEFAULT_ARENA_CMT MB(4)

typedef struct VkrMaterialSystem {
  // Internal arenas owned by the material system
  Arena *arena;             // persistent allocations (materials, names, maps)
  VkrAllocator allocator;   // persistent allocator wrapping arena
  VkrDMemory string_memory; // dynamic strings (freed on unload)
  VkrAllocator string_allocator; // allocator wrapper for string_memory
  VkrMaterialSystemConfig config;

  Array_VkrMaterial materials;                    // contiguous array
  VkrHashTable_VkrMaterialEntry material_by_name; // lifetime map

  // ID reuse tracking (stack of free indices)
  Array_uint32_t free_ids;
  uint32_t free_count;

  VkrTextureSystem *texture_system;
  VkrShaderSystem *shader_system;

  // Shadow map bindings for world materials (updated per frame).
  VkrTextureOpaqueHandle shadow_map;
  bool8_t shadow_maps_enabled;

  uint32_t next_free_index;
  uint32_t generation_counter;

  VkrMaterialHandle default_material;
} VkrMaterialSystem;

// =============================================================================
// Initialization / Shutdown
// =============================================================================

/**
 * @brief Initializes the material system
 * @param system The material system to initialize
 * @param arena The arena to use
 * @param texture_system The texture system to use
 * @param shader_system The shader system to use
 * @param config The configuration for the material system
 */
bool8_t vkr_material_system_init(VkrMaterialSystem *system, Arena *arena,
                                 VkrTextureSystem *texture_system,
                                 VkrShaderSystem *shader_system,
                                 const VkrMaterialSystemConfig *config);

/**
 * @brief Shuts down the material system
 * @param system The material system to shutdown
 */
void vkr_material_system_shutdown(VkrMaterialSystem *system);

// =============================================================================
// Material Management
// =============================================================================

/**
 * @brief Creates a default material (white color factor, default texture)
 * @param system The material system to create the default material in
 * @return The handle to the default material
 */
VkrMaterialHandle vkr_material_system_create_default(VkrMaterialSystem *system);

/**
 * @brief Creates a material with a specific diffuse color and default textures.
 * Used for shapes that need custom colors without loading a material file.
 * @param system The material system to create the material in
 * @param name Unique name for the material (will be copied)
 * @param diffuse_color The diffuse color for the material
 * @param out_error Optional error output
 * @return Handle to the created material, or invalid handle on failure
 */
VkrMaterialHandle
vkr_material_system_create_colored(VkrMaterialSystem *system, const char *name,
                                   Vec4 diffuse_color,
                                   VkrRendererError *out_error);

/**
 * @brief Creates or updates built-in gizmo materials (X/Y/Z emissive axes).
 * @param system The material system to create the materials in.
 * @param out_handles Optional array of 3 handles (X/Y/Z).
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_material_system_create_gizmo_materials(
    VkrMaterialSystem *system, VkrMaterialHandle out_handles[3],
    VkrRendererError *out_error);

/**
 * @brief Acquires a material by name; increments refcount if it exists; fails
 * if not loaded.
 * @param system The material system to acquire the material from
 * @param name The name of the material to acquire
 * @param auto_release Whether to auto-release the material when the refcount
 * reaches 0
 * @param out_error Optional; set to a descriptive error on failure (may be
 * NULL).
 * @return The handle to the acquired material; returns
 * VKR_MATERIAL_HANDLE_INVALID if not loaded.
 */
VkrMaterialHandle vkr_material_system_acquire(VkrMaterialSystem *system,
                                              String8 name,
                                              bool8_t auto_release,
                                              VkrRendererError *out_error);

/**
 * @brief Releases a material by handle; will free when ref_count hits 0 and
 * auto_release is set.
 * @param system The material system to release the material from
 * @param handle The handle to the material to release
 */
void vkr_material_system_release(VkrMaterialSystem *system,
                                 VkrMaterialHandle handle);

/**
 * @brief Adds a reference to an already acquired material handle.
 * @param system The material system managing the handle
 * @param handle The handle to retain
 */
void vkr_material_system_add_ref(VkrMaterialSystem *system,
                                 VkrMaterialHandle handle);

/**
 * @brief Applies the global material state to the material system
 * @param system The material system to apply the global material state to
 * @param global_state The global material state to apply
 * @param domain The domain to apply the global material state to
 */
void vkr_material_system_apply_global(VkrMaterialSystem *system,
                                      VkrGlobalMaterialState *global_state,
                                      VkrPipelineDomain domain);

/**
 * @brief Applies the instance material state to the material system
 * @param system The material system to apply the instance material state to
 * @param material The material to apply the instance material state to
 * @param domain The domain to apply the instance material state to
 */
void vkr_material_system_apply_instance(VkrMaterialSystem *system,
                                        const VkrMaterial *material,
                                        VkrPipelineDomain domain);

/**
 * @brief Updates shadow map bindings for world materials.
 *
 * Passing enabled=false clears bindings (default textures will be used).
 */
void vkr_material_system_set_shadow_map(VkrMaterialSystem *system,
                                        VkrTextureOpaqueHandle map,
                                        bool8_t enabled);

/**
 * @brief Applies the local material state to the material system
 * @param system The material system to apply the local material state to
 * @param local_state The local material state to apply
 */
void vkr_material_system_apply_local(VkrMaterialSystem *system,
                                     VkrLocalMaterialState *local_state);

/**
 * @brief Returns a pointer to the material referenced by handle if valid; NULL
 * otherwise.
 * @note Pointer is invalidated if the material is freed or if its slot is
 * reused; existing handles become invalid when generation changes
 * @param system The material system to get the material from
 * @param handle The handle to the material to get
 * @return A pointer to the material if valid; NULL otherwise.
 */
VkrMaterial *vkr_material_system_get_by_handle(VkrMaterialSystem *system,
                                               VkrMaterialHandle handle);

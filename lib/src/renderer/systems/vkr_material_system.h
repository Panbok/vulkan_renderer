#pragma once

#include "containers/array.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/arena.h"
#include "renderer/renderer.h"
#include "renderer/resources/resources.h"
#include "renderer/systems/vkr_texture_system.h"

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

// todo: hash table should count lifetime objects of materials
// with ref counts, but we currently don't do that

typedef struct VkrMaterialSystem {
  // Internal arenas owned by the material system
  Arena *arena;      // persistent allocations (materials, names, maps)
  Arena *temp_arena; // temporary allocations during parsing/loading
  VkrMaterialSystemConfig config;

  Array_VkrMaterial materials;                    // contiguous array
  VkrHashTable_VkrMaterialEntry material_by_name; // lifetime map

  // ID reuse tracking (stack of free indices)
  Array_uint32_t free_ids;
  uint32_t free_count;

  VkrTextureSystem *texture_system; // dependency

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
 * @param config The configuration for the material system
 */
bool8_t vkr_material_system_init(VkrMaterialSystem *system, Arena *arena,
                                 VkrTextureSystem *texture_system,
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
 * @brief Acquires a material by name; increments refcount or creates with
 * defaults if missing.
 * @param system The material system to acquire the material from
 * @param name The name of the material to acquire
 * @param auto_release Whether to auto-release the material when the refcount
 * reaches 0
 * @return The handle to the acquired material
 */
VkrMaterialHandle vkr_material_system_acquire(VkrMaterialSystem *system,
                                              String8 name,
                                              bool8_t auto_release);

/**
 * @brief Releases a material by handle; will free when ref_count hits 0 and
 * auto_release is set.
 * @param system The material system to release the material from
 * @param handle The handle to the material to release
 */
void vkr_material_system_release(VkrMaterialSystem *system,
                                 VkrMaterialHandle handle);

/**
 * @brief Updates a material's properties according to Phong model
 * @param system The material system to update the material in
 * @param handle The handle to the material to update
 * @param base_color The base color of the material
 * @param diffuse_color The diffuse color of the material
 * @param specular_color The specular color of the material
 * @param shininess The shininess of the material
 * @param emission_color The emission color of the material
 */
void vkr_material_system_set(VkrMaterialSystem *system,
                             VkrMaterialHandle handle,
                             VkrTextureHandle base_color, Vec4 diffuse_color,
                             Vec4 specular_color, float32_t shininess,
                             Vec3 emission_color);

// =============================================================================
// Material Loading
// =============================================================================

// Load a material from a .mt file and return its handle. The .mt format is:
// base_color=assets/texture.png
// diffuse_color=R,G,B,A
// specular_color=R,G,B,A
// shininess=FLOAT
// emission_color=R,G,B
// The texture will be acquired via the texture system. The material name is
// derived from the file basename (without extension). Returns true on success.
/**
 * @brief Loads a material from a .mt file and returns its handle
 * @param renderer The renderer to use
 * @param system The material system to load the material into
 * @param path The path to the .mt file
 * @param temp_arena The temporary arena to use
 * @param out_handle The handle to the loaded material
 * @return True on success, false otherwise
 */
bool8_t vkr_material_system_load_from_mt(RendererFrontendHandle renderer,
                                         VkrMaterialSystem *system,
                                         String8 path, Arena *temp_arena,
                                         VkrMaterialHandle *out_handle);

/**
 * @brief Loads default material properties from a .mt file into the existing
 * default material (index 0). Keeps the material id/name mapping stable and
 * only updates base_color and Phong properties. Returns true on success.
 * @param renderer The renderer to use
 * @param system The material system to load the material into
 * @param path The path to the .mt file
 * @param temp_arena The temporary arena to use
 * @return True on success, false otherwise
 */
bool8_t
vkr_material_system_load_default_from_mt(RendererFrontendHandle renderer,
                                         VkrMaterialSystem *system,
                                         String8 path, Arena *temp_arena);

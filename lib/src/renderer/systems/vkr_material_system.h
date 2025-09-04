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
 * @brief Updates a material's diffuse texture and Phong properties
 * @param system The material system to update the material in
 * @param handle The handle to the material to update
 * @param base_color The base color texture handle (slot: diffuse)
 * @param phong Phong lighting properties
 */
void vkr_material_system_set(VkrMaterialSystem *system,
                             VkrMaterialHandle handle,
                             VkrTextureHandle base_color,
                             VkrPhongProperties phong);

/**
 * @brief Sets a single texture map on a material.
 * If texture_handle.id == 0, the slot will be disabled regardless of 'enable'.
 */
void vkr_material_system_set_texture(VkrMaterialSystem *system,
                                     VkrMaterialHandle handle,
                                     VkrTextureSlot slot,
                                     VkrTextureHandle texture_handle,
                                     bool8_t enable);

/**
 * @brief Sets all texture maps on a material at once.
 * 'enabled' may be NULL to infer enable from texture id!=0.
 */
void vkr_material_system_set_textures(
    VkrMaterialSystem *system, VkrMaterialHandle handle,
    const VkrTextureHandle textures[VKR_TEXTURE_SLOT_COUNT],
    const bool8_t enabled[VKR_TEXTURE_SLOT_COUNT]);

// =============================================================================
// Material Loading
// =============================================================================

// =============================================================================
// .mt Material File Format
// =============================================================================
// Key-value pairs, one per line, 'key=value'. Lines starting with '#' are
// comments. Whitespace around keys/values is trimmed.
//
// Supported keys:
//   base_color="assets/texture.png"   // Diffuse/base color texture
//   diffuse_color=R,G,B,A              // Vec4
//   specular_color=R,G,B,A             // Vec4
//   shininess=FLOAT                    // Specular exponent
//   emission_color=R,G,B               // Vec3
//
// Optional texture maps (handled by the material system even if unused by the
// current backend):
//   normal_map="assets/normal.png"
//   specular_map="assets/spec.png"
//   emission_map="assets/emissive.png"
//
// Notes:
// - Paths are relative to the executable working directory.
// - Unknown keys are logged and ignored.
// - The material name is derived from the file basename (without extension).
// - Missing textures fall back to the texture system default for diffuse; other
//   maps remain disabled unless provided.

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

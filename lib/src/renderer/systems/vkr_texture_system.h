#pragma once

#include "containers/array.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/arena.h"
#include "renderer/renderer.h"
#include "renderer/resources/vkr_resources.h"

// =============================================================================
// Texture System - Basic texture management with array and hash table
// =============================================================================

#define VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_RSV MB(16)
#define VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_CMT MB(4)

typedef struct VkrTextureEntry {
  uint32_t index;       // index into textures array
  uint32_t ref_count;   // reference count
  bool8_t auto_release; // release when ref_count hits 0
} VkrTextureEntry;
VkrHashTable(VkrTextureEntry);

typedef struct VkrTextureSystemConfig {
  uint32_t max_texture_count;
} VkrTextureSystemConfig;

typedef struct VkrTextureSystem {
  Arena *arena; // internal arena owned by the system
  VkrTextureSystemConfig config;

  Array_VkrTexture textures; // contiguous array of textures
  VkrHashTable_VkrTextureEntry
      texture_map; // name -> ref (index, refcount, flags)

  uint32_t next_free_index;    // linear probe for free slot
  uint32_t generation_counter; // Monotonic generation counter for texture
                               // description generations

  VkrTextureHandle default_texture; // fallback texture
} VkrTextureSystem;

// =============================================================================
// Initialization / Shutdown
// =============================================================================

/**
 * @brief Initializes the texture system
 * @param renderer The renderer to use
 * @param config The configuration for the texture system
 * @param out_system The initialized texture system (output)
 * @return true on success, false on failure
 */
bool8_t vkr_texture_system_init(RendererFrontendHandle renderer,
                                const VkrTextureSystemConfig *config,
                                VkrTextureSystem *out_system);

/**
 * @brief Shuts down the texture system
 * @param system The texture system to shutdown
 * @param renderer The renderer to use
 */
void vkr_texture_system_shutdown(RendererFrontendHandle renderer,
                                 VkrTextureSystem *system);

// =============================================================================
// Resource operations integrated with the system (use system arena/renderer)
// =============================================================================

/**
 * @brief Creates a default texture
 * @param renderer The renderer to use
 * @param system The texture system to create the default texture in
 * @param out_texture The created default texture (output)
 * @return RendererError indicating success or failure
 */
RendererError vkr_texture_system_create_default(RendererFrontendHandle renderer,
                                                VkrTextureSystem *system,
                                                VkrTexture *out_texture);

/**
 * @brief Loads a texture from a file
 * @param renderer The renderer to use
 * @param system The texture system to load the texture in
 * @param file_path The path to the texture file
 * @param desired_channels The desired number of channels for the texture
 * @param out_texture The texture to load the texture into
 * @return RendererError indicating success or failure
 */
RendererError vkr_texture_system_load(RendererFrontendHandle renderer,
                                      VkrTextureSystem *system,
                                      String8 file_path,
                                      uint32_t desired_channels,
                                      VkrTexture *out_texture);

/**
 * @brief Destroys a texture
 * @param system The texture system to destroy the texture in
 * @param texture The texture to destroy
 */
void vkr_texture_system_destroy(RendererFrontendHandle renderer,
                                VkrTextureSystem *system, VkrTexture *texture);

/**
 * @brief Acquires a texture by name; increments refcount or loads if missing.
 * @param renderer The renderer to use
 * @param system The texture system to acquire the texture from
 * @param texture_name The name of the texture to acquire
 * @param auto_release Whether to auto-release the texture when the refcount
 * reaches 0
 * @param temp_arena The temporary arena to use
 * @param out_error The error output
 * @return The handle to the acquired texture
 */
VkrTextureHandle vkr_texture_system_acquire(RendererFrontendHandle renderer,
                                            VkrTextureSystem *system,
                                            String8 texture_name,
                                            bool8_t auto_release,
                                            Arena *temp_arena,
                                            RendererError *out_error);

/**
 * @brief Releases a texture by name
 * @param renderer The renderer to use
 * @param system The texture system to release the texture from
 * @param texture_name The name of the texture to release
 */
void vkr_texture_system_release(RendererFrontendHandle renderer,
                                VkrTextureSystem *system, String8 texture_name);

// =============================================================================
// Getters
// =============================================================================

/**
 * @brief Gets a texture by handle (validated by generation)
 * @param system The texture system to get the texture from
 * @param handle The handle of the texture to get
 * @return The texture
 */
VkrTexture *vkr_texture_system_get_by_handle(VkrTextureSystem *system,
                                             VkrTextureHandle handle);

/**
 * @brief Gets a texture by index
 * @param system The texture system to get the texture from
 * @param index The index of the texture to get
 * @return The texture
 */
VkrTexture *vkr_texture_system_get_by_index(VkrTextureSystem *system,
                                            uint32_t texture_index);

/**
 * @brief Gets the default texture
 * @param system The texture system to get the default texture from
 * @return The default texture
 */
VkrTexture *vkr_texture_system_get_default(VkrTextureSystem *system);

/**
 * @brief Gets a handle to the default texture (index 0 by convention)
 * @param system The texture system to get the default texture from
 * @return The default texture handle
 */
VkrTextureHandle
vkr_texture_system_get_default_handle(VkrTextureSystem *system);

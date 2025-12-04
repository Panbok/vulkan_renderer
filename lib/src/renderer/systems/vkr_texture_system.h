#pragma once

#include "containers/vkr_hashtable.h"
#include "core/vkr_job_system.h"
#include "defines.h"
#include "memory/arena.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_renderer.h"

// todo: should we merge this with material system, since
// textures are only used by materials?

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
  VkrRendererFrontendHandle renderer;
  Arena *arena;           // internal arena owned by the system
  VkrAllocator allocator; // persistent allocator wrapping arena
  VkrTextureSystemConfig config;

  Array_VkrTexture textures; // contiguous array of textures
  VkrHashTable_VkrTextureEntry
      texture_map; // name -> ref (index, refcount, flags)

  uint32_t next_free_index;    // linear probe for free slot
  uint32_t generation_counter; // Monotonic generation counter for texture
                               // description generations

  VkrTextureHandle default_texture;          // fallback texture
  VkrTextureHandle default_normal_texture;   // flat normal fallback
  VkrTextureHandle default_specular_texture; // flat specular fallback

  VkrJobSystem *job_system; // For async texture loading
} VkrTextureSystem;

// =============================================================================
// Initialization / Shutdown
// =============================================================================

/**
 * @brief Initializes the texture system
 * @param renderer The renderer to use
 * @param config The configuration for the texture system
 * @param job_system The job system for async loading (can be NULL for sync
 * only)
 * @param out_system The initialized texture system (output)
 * @return true on success, false on failure
 */
bool8_t vkr_texture_system_init(VkrRendererFrontendHandle renderer,
                                const VkrTextureSystemConfig *config,
                                VkrJobSystem *job_system,
                                VkrTextureSystem *out_system);

/**
 * @brief Shuts down the texture system
 * @param system The texture system to shutdown
 * @param renderer The renderer to use
 */
void vkr_texture_system_shutdown(VkrRendererFrontendHandle renderer,
                                 VkrTextureSystem *system);

// =============================================================================
// Resource operations integrated with the system (use system arena/renderer)
// =============================================================================

/**
 * @brief Acquires a texture by name; increments refcount if exists, fails if
 * not loaded.
 * @param system The texture system to acquire the texture from
 * @param texture_name The name of the texture to acquire
 * @param auto_release Whether to auto-release the texture when the refcount
 * reaches 0
 * @param out_error The error output
 * @return The handle to the acquired texture, invalid handle if not loaded
 */
VkrTextureHandle vkr_texture_system_acquire(VkrTextureSystem *system,
                                            String8 texture_name,
                                            bool8_t auto_release,
                                            VkrRendererError *out_error);

/**
 * @brief Creates a writable texture owned by the texture system (no
 * auto-release).
 */
bool8_t vkr_texture_system_create_writable(VkrTextureSystem *system,
                                           String8 name,
                                           const VkrTextureDescription *desc,
                                           VkrTextureHandle *out_handle,
                                           VkrRendererError *out_error);

/**
 * @brief Releases a texture by name
 * @param system The texture system to release the texture from
 * @param texture_name The name of the texture to release
 */
void vkr_texture_system_release(VkrTextureSystem *system, String8 texture_name);

/**
 * @brief Releases a texture by handle
 * @param system The texture system to release the texture from
 * @param handle The handle of the texture to release
 */
void vkr_texture_system_release_by_handle(VkrTextureSystem *system,
                                          VkrTextureHandle handle);

/**
 * @brief Updates sampler filtering/wrapping for a texture handle.
 * @param system The texture system to update the sampler for
 * @param handle The handle of the texture to update the sampler for
 * @param min_filter The minimum filter to use
 * @param mag_filter The maximum filter to use
 * @param mip_filter The mip filter to use
 * @param anisotropy_enable Whether to enable anisotropy
 * @param u_repeat_mode The U repeat mode to use
 * @param v_repeat_mode The V repeat mode to use
 * @param w_repeat_mode The W repeat mode to use
 * @return The error
 */
VkrRendererError vkr_texture_system_update_sampler(
    VkrTextureSystem *system, VkrTextureHandle handle, VkrFilter min_filter,
    VkrFilter mag_filter, VkrMipFilter mip_filter, bool8_t anisotropy_enable,
    VkrTextureRepeatMode u_repeat_mode, VkrTextureRepeatMode v_repeat_mode,
    VkrTextureRepeatMode w_repeat_mode);

/**
 * @brief Writes data to a texture handle
 * @param system The texture system to write to
 * @param handle The handle of the texture to write to
 * @param data The data to write
 * @param size The size of the data
 * @return The error
 */
VkrRendererError vkr_texture_system_write(VkrTextureSystem *system,
                                          VkrTextureHandle handle,
                                          const void *data, uint64_t size);

/**
 * @brief Writes data to a region of a texture handle
 * @param system The texture system to write to
 * @param handle The handle of the texture to write to
 * @param region The region to write to
 * @param data The data to write
 * @param size The size of the data
 * @return The error
 */
VkrRendererError vkr_texture_system_write_region(
    VkrTextureSystem *system, VkrTextureHandle handle,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size);

/**
 * @brief Resizes a texture handle
 * @param system The texture system to resize the texture in
 * @param handle The handle of the texture to resize
 * @param new_width The new width of the texture
 * @param new_height The new height of the texture
 * @param preserve_contents Whether to preserve the contents of the texture
 * @param out_handle The output handle
 * @param out_error The output error
 * @return true on success, false on failure
 */
bool8_t vkr_texture_system_resize(VkrTextureSystem *system,
                                  VkrTextureHandle handle, uint32_t new_width,
                                  uint32_t new_height,
                                  bool8_t preserve_contents,
                                  VkrTextureHandle *out_handle,
                                  VkrRendererError *out_error);

/**
 * @brief Registers an external texture handle
 * @param system The texture system to register the external texture in
 * @param name The name of the texture to register
 * @param backend_handle The backend handle of the texture to register
 * @param desc The description of the texture to register
 * @param out_handle The output handle
 * @return true on success, false on failure
 */
bool8_t
vkr_texture_system_register_external(VkrTextureSystem *system, String8 name,
                                     VkrTextureOpaqueHandle backend_handle,
                                     const VkrTextureDescription *desc,
                                     VkrTextureHandle *out_handle);

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

/**
 * @brief Gets a handle to the default flat normal map
 */
VkrTextureHandle
vkr_texture_system_get_default_normal_handle(VkrTextureSystem *system);

/**
 * @brief Gets a handle to the default flat specular map
 * @param system The texture system to get the default specular texture from
 * @return The default specular texture handle
 */
VkrTextureHandle
vkr_texture_system_get_default_specular_handle(VkrTextureSystem *system);

// =============================================================================
// Helpers (used by loaders with direct system access)
// =============================================================================

/**
 * @brief Loads a texture from a file
 * @param self The texture system
 * @param file_path The path to the file
 * @param desired_channels The desired number of channels
 * @param out_texture The output texture
 * @return The error
 */
VkrRendererError vkr_texture_system_load_from_file(VkrTextureSystem *self,
                                                   String8 file_path,
                                                   uint32_t desired_channels,
                                                   VkrTexture *out_texture);

/**
 * @brief Loads a texture from a file
 * @param system The texture system
 * @param name The name of the texture to load
 * @param out_handle The output handle
 * @param out_error The output error
 */
bool8_t vkr_texture_system_load(VkrTextureSystem *system, String8 name,
                                VkrTextureHandle *out_handle,
                                VkrRendererError *out_error);

/**
 * @brief Loads multiple textures in parallel using the job system.
 *
 * Submits all texture decode jobs at once, waits for all to complete,
 * then does GPU uploads. Much faster than loading textures one by one.
 *
 * @param system The texture system
 * @param paths Array of texture file paths to load
 * @param count Number of textures to load
 * @param out_handles Array to receive texture handles (must have 'count'
 * elements)
 * @param out_errors Array to receive per-texture errors (must have 'count'
 * elements)
 * @return Number of textures successfully loaded
 */
uint32_t vkr_texture_system_load_batch(VkrTextureSystem *system,
                                       const String8 *paths, uint32_t count,
                                       VkrTextureHandle *out_handles,
                                       VkrRendererError *out_errors);

/**
 * @brief Finds a free slot in the texture system
 * @param system The texture system to search
 * @return The index of a free slot, or VKR_INVALID_ID if none available
 */
uint32_t vkr_texture_system_find_free_slot(VkrTextureSystem *system);

/**
 * @brief Destroys a texture
 * @param renderer The renderer to use
 * @param texture The texture to destroy
 */
void vkr_texture_destroy(VkrRendererFrontendHandle renderer,
                         VkrTexture *texture);

/**
 * @brief Loads a cube map texture from 6 face images.
 *
 * Face images are expected to have suffixes: _r, _l, _u, _d, _f, _b
 * For example, base_path="skybox" and extension="jpg" would load:
 *   skybox_r.jpg, skybox_l.jpg, skybox_u.jpg, skybox_d.jpg, skybox_f.jpg,
 * skybox_b.jpg
 *
 * @param system The texture system
 * @param base_path Base path without face suffix (e.g.,
 * "assets/textures/skybox")
 * @param extension File extension without dot (e.g., "jpg")
 * @param out_handle Output texture handle
 * @param out_error Output error code
 * @return true on success, false on failure
 */
bool8_t vkr_texture_system_load_cube_map(VkrTextureSystem *system,
                                         String8 base_path, String8 extension,
                                         VkrTextureHandle *out_handle,
                                         VkrRendererError *out_error);

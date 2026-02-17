#pragma once

#include "containers/vkr_hashtable.h"
#include "core/vkr_job_system.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
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
  const char *name;     // stable key for freeing on unload
} VkrTextureEntry;
VkrHashTable(VkrTextureEntry);

typedef struct VkrTextureSystemConfig {
  uint32_t max_texture_count;
} VkrTextureSystemConfig;

typedef enum VkrTextureVktContainerType {
  VKR_TEXTURE_VKT_CONTAINER_UNKNOWN = 0,
  VKR_TEXTURE_VKT_CONTAINER_LEGACY_RAW,
  VKR_TEXTURE_VKT_CONTAINER_KTX2,
} VkrTextureVktContainerType;

/**
 * @brief Semantic texture classes used by transcode target selection.
 *
 * These classes express sampling intent, not source file encoding. Selection
 * logic uses them to keep normals/data linear and to choose class-appropriate
 * compressed families.
 */
typedef enum VkrTextureClass {
  VKR_TEXTURE_CLASS_COLOR_SRGB = 0,
  VKR_TEXTURE_CLASS_COLOR_LINEAR,
  VKR_TEXTURE_CLASS_NORMAL_RG,
  VKR_TEXTURE_CLASS_DATA_MASK,
} VkrTextureClass;

typedef struct VkrTextureSystem {
  VkrRendererFrontendHandle renderer;
  Arena *arena;                  // internal arena owned by the system
  VkrAllocator allocator;        // persistent allocator wrapping arena
  VkrDMemory string_memory;      // dynamic strings (freed on unload)
  VkrAllocator string_allocator; // allocator wrapper for string_memory
  VkrDMemory async_memory;       // freeable async payload allocations
  VkrAllocator async_allocator;  // allocator wrapper for async_memory
  VkrMutex async_mutex;          // guards async allocator across threads
  VkrTextureSystemConfig config;

  Array_VkrTexture textures; // contiguous array of textures
  VkrHashTable_VkrTextureEntry
      texture_map;                    // name -> ref (index, refcount, flags)
  const char **texture_keys_by_index; // slot index -> stable texture-map key

  uint32_t next_free_index;    // linear probe for free slot
  uint32_t generation_counter; // Monotonic generation counter for texture
                               // description generations

  VkrTextureHandle default_texture;          // fallback texture (checkerboard)
  VkrTextureHandle default_diffuse_texture;  // white diffuse fallback
  VkrTextureHandle default_normal_texture;   // flat normal fallback
  VkrTextureHandle default_specular_texture; // flat specular fallback

  VkrJobSystem *job_system;                      // For async texture loading
  struct VkrTextureCacheWriteGuard *cache_guard; // Internal cache write guard

  // Device-dependent transcode policy inputs for KTX2/UASTC decode.
  VkrDeviceTypeFlags device_types;   // Device type bits used as preference hint
  bool8_t supports_texture_astc_4x4; // Whether ASTC 4x4 is supported
  bool8_t supports_texture_bc7;      // Whether BC7 is supported
  bool8_t supports_texture_etc2;     // Whether ETC2 RGBA is supported
  bool8_t supports_texture_bc5;      // Whether BC5 is supported

  // Runtime rollout controls for `.vkt` migration.
  bool8_t strict_vkt_only_mode;     // Disable source-image fallback.
  bool8_t allow_legacy_vkt;         // Allow legacy raw `.vkt` read path.
  bool8_t allow_source_fallback;    // Permit source image decode when `.vkt`
                                    // is missing/invalid.
  bool8_t allow_legacy_cache_write; // Permit writing legacy raw sidecar cache.
} VkrTextureSystem;

/**
 * @brief Returns true when path has a `.vkt` extension (query suffix ignored).
 * @param path The path to check
 * @return true if the path has a `.vkt` extension, false otherwise
 */
bool8_t vkr_texture_is_vkt_path(String8 path);

/**
 * @brief Builds direct/sidecar/source resolution candidates for a texture
 * request.
 *
 * If request path is already `.vkt`, `out_direct_vkt` and `out_source_path` are
 * set to the normalized request path and `out_sidecar_vkt` is empty.
 *
 * @param allocator The allocator to use
 * @param request_path The path to build the resolution candidates for
 * @param out_direct_vkt The direct `.vkt` path (if any)
 * @param out_sidecar_vkt The sidecar `.vkt` path (if any)
 * @param out_source_path The source path (if any)
 */
void vkr_texture_build_resolution_candidates(VkrAllocator *allocator,
                                             String8 request_path,
                                             String8 *out_direct_vkt,
                                             String8 *out_sidecar_vkt,
                                             String8 *out_source_path);

/**
 * @brief Classifies `.vkt` bytes as legacy raw cache, KTX2, or unknown.
 * @param bytes The bytes to classify
 * @param size The size of the bytes
 * @return The container type
 */
VkrTextureVktContainerType
vkr_texture_detect_vkt_container(const uint8_t *bytes, uint64_t size);

/**
 * @brief Parses `?cs=srgb|linear` and returns whether the request is sRGB.
 *
 * Unknown values keep the provided default.
 * @param request_path The path to check
 * @param default_srgb The default sRGB value
 * @return true if the request is sRGB, false otherwise
 */
bool8_t vkr_texture_request_prefers_srgb(String8 request_path,
                                         bool8_t default_srgb);

/**
 * @brief Selects transcode target format with deterministic fallback ordering.
 * @param texture_class Texture data class used to pick a target ladder
 * @param request_srgb Whether the request is sRGB
 * @param device_types Device type bits used for preference ordering
 * @param supports_astc_4x4 Whether ASTC 4x4 is supported
 * @param supports_bc7 Whether BC7 is supported
 * @param supports_etc2 Whether ETC2 RGBA is supported
 * @param supports_bc5 Whether BC5 is supported
 * @return The transcode target format
 */
VkrTextureFormat vkr_texture_select_transcode_target_format(
    VkrTextureClass texture_class, bool8_t request_srgb,
    VkrDeviceTypeFlags device_types, bool8_t supports_astc_4x4,
    bool8_t supports_bc7, bool8_t supports_etc2, bool8_t supports_bc5);

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
 * @brief Adds one reference to a texture by handle.
 *
 * This is useful when a caller already has a resolved handle (for example from
 * async resource resolution) and needs to retain lifetime without relying on
 * name-key lookup.
 *
 * @param system The texture system to update
 * @param handle The texture handle to retain
 */
void vkr_texture_system_add_ref_by_handle(VkrTextureSystem *system,
                                          VkrTextureHandle handle);

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
 * @brief Gets a handle to the default white diffuse texture.
 *
 * Used for materials without diffuse textures to preserve material color.
 */
VkrTextureHandle
vkr_texture_system_get_default_diffuse_handle(VkrTextureSystem *system);

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

typedef struct VkrTexturePreparedLoad {
  VkrTextureDescription description;
  uint8_t *upload_data;
  uint64_t upload_data_size;
  VkrTextureUploadRegion *upload_regions;
  uint32_t upload_region_count;
  uint32_t upload_mip_levels;
  uint32_t upload_array_layers;
  bool8_t upload_is_compressed;
} VkrTexturePreparedLoad;

/**
 * @brief Decode and prepare upload payload without touching GPU state.
 *
 * The output is heap-owned and must be released with
 * `vkr_texture_system_release_prepared_load` on every path.
 *
 * On failure, `out_prepared` is zeroed and does not require cleanup,
 * but calling `vkr_texture_system_release_prepared_load` is safe.
 */
bool8_t vkr_texture_system_prepare_load_from_file(
    VkrTextureSystem *system, String8 file_path, uint32_t desired_channels,
    VkrAllocator *temp_alloc, VkrTexturePreparedLoad *out_prepared,
    VkrRendererError *out_error);

/**
 * @brief Finalize a previously prepared texture on the render thread.
 *
 * This function creates the GPU texture and inserts it into the texture map.
 * On success the returned handle is visible to acquire calls.
 */
bool8_t vkr_texture_system_finalize_prepared_load(
    VkrTextureSystem *system, String8 name,
    const VkrTexturePreparedLoad *prepared, VkrTextureHandle *out_handle,
    VkrRendererError *out_error);

/**
 * @brief Releases CPU memory owned by a prepared texture payload.
 */
void vkr_texture_system_release_prepared_load(VkrTexturePreparedLoad *prepared);

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

#pragma once

#include "containers/array.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_asset_publisher.h"
#include "renderer/vkr_renderer.h"

#define VKR_MATERIAL_NAME_MAX 128

// =============================================================================
// Material System - Basic materials management with array and hash table
// =============================================================================

typedef struct VkrMaterialSystemConfig {
  uint32_t max_material_count;
  const VkrAssetPublisher *asset_publisher;
} VkrMaterialSystemConfig;

typedef struct VkrMaterialIblProbeSlot {
  VkrTextureOpaqueHandle irradiance_map;
  VkrTextureOpaqueHandle prefilter_map;
  Vec3 center;
  Vec3 extents;
  float32_t blend_distance;
  float32_t weight;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  bool8_t box_projection_enabled;
} VkrMaterialIblProbeSlot;

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

#define VKR_MATERIAL_TEXTURE_STREAM_CAPACITY 4096u
#define VKR_MATERIAL_TEXTURE_STREAM_PATH_MAX 512u
#define VKR_MATERIAL_TEXTURE_STREAM_IN_FLIGHT_MAX 8u

typedef enum VkrMaterialTextureResidencyState {
  VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED = 0,
  VKR_MATERIAL_TEXTURE_RESIDENCY_ACTIVE,
  VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT,
  VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED,
} VkrMaterialTextureResidencyState;

typedef struct VkrMaterialTextureStream {
  VkrMaterialHandle material;
  VkrTextureSlot slot;
  char path[VKR_MATERIAL_TEXTURE_STREAM_PATH_MAX];
  VkrMaterialTextureResidencyState state;
  VkrResourceHandleInfo request;
  VkrTextureHandle resident_texture;
  uint64_t resident_bytes;
} VkrMaterialTextureStream;

typedef struct VkrMaterialSystem {
  // Internal arenas owned by the material system
  Arena *arena;             // persistent allocations (materials, names, maps)
  VkrAllocator allocator;   // persistent allocator wrapping arena
  VkrDMemory string_memory; // dynamic strings (freed on unload)
  VkrAllocator string_allocator; // allocator wrapper for string_memory
  VkrDMemory async_memory;       // freeable async payload allocations
  VkrAllocator async_allocator;  // allocator wrapper for async_memory
  VkrMutex async_mutex;          // guards async allocator across threads
  VkrMaterialSystemConfig config;
  const VkrAssetPublisher *asset_publisher;

  Array_VkrMaterial materials;                    // contiguous array
  VkrHashTable_VkrMaterialEntry material_by_name; // lifetime map

  // ID reuse tracking (stack of free indices)
  Array_uint32_t free_ids;
  uint32_t free_count;

  VkrTextureSystem *texture_system;
  VkrMaterialTextureStream *texture_streams;
  uint32_t texture_stream_count;
  uint32_t texture_stream_capacity;
  uint32_t texture_stream_queued_count;
  uint32_t texture_stream_active_count;
  uint32_t texture_stream_resident_count;
  uint32_t texture_stream_evicted_count;
  uint64_t texture_stream_resident_bytes;
  uint64_t texture_stream_budget_bytes;
  bool8_t texture_stream_budget_user_configured;
  uint64_t texture_stream_epoch;
  uint64_t *texture_material_last_used_epochs;
  uint64_t texture_stream_applied_total;
  uint64_t texture_stream_failed_total;
  uint64_t texture_stream_evicted_total;
  uint64_t texture_stream_pressure_stalls_total;

  // Shadow map bindings for world materials (updated per frame).
  VkrTextureOpaqueHandle shadow_map;
  bool8_t shadow_maps_enabled;

  // IBL bindings for PBR world materials (updated per frame).
  VkrTextureOpaqueHandle ibl_irradiance_map;
  VkrTextureOpaqueHandle ibl_prefilter_map;
  bool8_t ibl_enabled;
  float32_t ibl_intensity;
  float32_t ibl_diffuse_intensity;
  float32_t ibl_specular_intensity;
  VkrMaterialIblProbeSlot ibl_probe_slots[3];

  // Graph-owned pre-transmission HDR image; valid only for transmission pass.
  VkrTextureOpaqueHandle transmission_source;
  bool8_t transmission_pass_enabled;

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

/** Queues one material texture path for bounded background residency. */
bool8_t vkr_material_system_stream_texture(VkrMaterialSystem *system,
                                           VkrMaterialHandle material,
                                           VkrTextureSlot slot,
                                           const char *path);

/** Applies up to max_updates ready streamed textures to live material rows. */
void vkr_material_system_pump_texture_streams(VkrMaterialSystem *system,
                                              uint32_t max_updates);

/** Cancels and releases every pending streamed texture for a material. */
void vkr_material_system_cancel_texture_streams(VkrMaterialSystem *system,
                                                VkrMaterialHandle material);

/** Starts one material-usage epoch before packet collection. */
void vkr_material_system_begin_texture_residency_frame(
    VkrMaterialSystem *system);

/** Marks a material as demanded by the current packet. */
void vkr_material_system_touch_texture_residency(VkrMaterialSystem *system,
                                                 VkrMaterialHandle material);

/** Changes the hard logical-byte budget; the next pump evicts to fit. */
void vkr_material_system_set_texture_residency_budget(VkrMaterialSystem *system,
                                                      uint64_t budget_bytes);
void vkr_material_system_set_automatic_texture_residency_budget(
    VkrMaterialSystem *system, uint64_t budget_bytes);

/** Returns the neutral fallback representation for one texture slot. */
VkrMaterialTexture
vkr_material_system_get_default_texture(VkrMaterialSystem *system,
                                        VkrTextureSlot slot);

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
bool8_t
vkr_material_system_create_gizmo_materials(VkrMaterialSystem *system,
                                           VkrMaterialHandle out_handles[3],
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

/** Publish an initialized CPU material under its exact shared handle. */
bool8_t vkr_material_system_publish(VkrMaterialSystem *system,
                                    VkrMaterialHandle handle,
                                    VkrRendererError *out_error);

/** Retire the GPU material associated with an exact shared handle. */
bool8_t vkr_material_system_unpublish(VkrMaterialSystem *system,
                                      VkrMaterialHandle handle);

/**
 * @brief Updates shadow map bindings for world materials.
 *
 * Passing enabled=false clears bindings (default textures will be used).
 */
void vkr_material_system_set_shadow_map(VkrMaterialSystem *system,
                                        VkrTextureOpaqueHandle map,
                                        bool8_t enabled);

/**
 * @brief Updates active IBL maps and scalar controls for PBR materials.
 *
 * Call once per frame before world draws. Passing enabled=false keeps maps
 * bound for descriptor validity while disabling IBL contribution in shader.
 */
void vkr_material_system_set_ibl_maps(VkrMaterialSystem *system,
                                      VkrTextureOpaqueHandle irradiance_map,
                                      VkrTextureOpaqueHandle prefilter_map,
                                      bool8_t enabled, float32_t intensity,
                                      float32_t diffuse_intensity,
                                      float32_t specular_intensity);

/**
 * @brief Updates two per-draw local probe slots for PBR IBL blending.
 */
void vkr_material_system_set_ibl_probe_slots(
    VkrMaterialSystem *system, const VkrMaterialIblProbeSlot slots[3]);

/** Binds the graph-declared pre-transmission HDR source for world materials. */
void vkr_material_system_set_transmission_source(VkrMaterialSystem *system,
                                                 VkrTextureOpaqueHandle source,
                                                 bool8_t enabled);

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

/** Resolve a material handle owned by a live mesh/submesh. */
VkrMaterial *vkr_material_system_get_live(VkrMaterialSystem *system,
                                          VkrMaterialHandle handle);

/**
 * @brief Resolves the effective alpha mode used for draw-list routing.
 *
 * Explicit material modes take precedence. Legacy materials fall back to
 * factor alpha and diffuse-texture alpha metadata. Call once when both blend
 * and cutout decisions are needed so texture state is resolved only once.
 */
VkrMaterialAlphaMode
vkr_material_system_material_alpha_mode(const VkrMaterialSystem *system,
                                        const VkrMaterial *material);

/**
 * @brief Returns whether a material should be treated as transparent.
 *
 * Uses diffuse alpha and texture alpha mode to decide if the material should
 * be blended at draw time. Alpha-cutout materials are not treated as blended.
 *
 * If both transparency and cutout checks are needed, prefer
 * vkr_material_system_material_alpha_mode once and branch on the result.
 */
bool8_t
vkr_material_system_material_has_transparency(const VkrMaterialSystem *system,
                                              const VkrMaterial *material);

/**
 * @brief Returns whether a material should use alpha cutout (discard).
 *
 * If both transparency and cutout checks are needed, prefer
 * vkr_material_system_material_alpha_mode once and branch on the result.
 */
bool8_t
vkr_material_system_material_uses_cutout(const VkrMaterialSystem *system,
                                         const VkrMaterial *material);

/** True when transmission factor or a transmission texture contributes. */
bool8_t
vkr_material_system_material_is_transmissive(const VkrMaterialSystem *system,
                                             const VkrMaterial *material);

/**
 * @brief Returns the effective alpha cutoff for cutout materials.
 *
 * When a material does not explicitly set alpha_cutoff but its diffuse texture
 * is classified as an alpha mask, this returns
 * VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT.
 */
float32_t
vkr_material_system_material_alpha_cutoff(const VkrMaterialSystem *system,
                                          const VkrMaterial *material);

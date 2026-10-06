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
#include "vkr_asset_publisher.h"
#include "vkr_renderer.h"

#define VKR_MATERIAL_NAME_MAX 128

// =============================================================================
// Material System - Basic materials management with array and hash table
// =============================================================================

typedef struct VkrMaterialSystemConfig {
  uint32_t max_material_count;
  const VkrAssetPublisher *asset_publisher;
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

#define VKR_MATERIAL_TEXTURE_STREAM_CAPACITY 4096u
#define VKR_MATERIAL_TEXTURE_STREAM_PATH_MAX 512u
#define VKR_MATERIAL_TEXTURE_STREAM_IN_FLIGHT_MAX 8u
#define VKR_MATERIAL_REPLACEMENT_CAPACITY 512u
/* Reloads toward a changed texture load limit queued or loading at once. */
#define VKR_MATERIAL_TEXTURE_RELOAD_MAX 32u

typedef enum VkrMaterialTextureResidencyState {
  VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED = 0,
  VKR_MATERIAL_TEXTURE_RESIDENCY_ACTIVE,
  VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT,
  VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED,
  VKR_MATERIAL_TEXTURE_RESIDENCY_MEMORY_WAIT,
  /* Loaded and referenced for a replacement, not yet bound. */
  VKR_MATERIAL_TEXTURE_RESIDENCY_STAGED,
} VkrMaterialTextureResidencyState;

typedef struct VkrMaterialTextureStream {
  VkrMaterialHandle material;
  VkrTextureSlot slot;
  /* The material's texture request, its first `path_length` bytes, followed
   * by the load limit's query of the last request, so `path` names that
   * request. */
  char path[VKR_MATERIAL_TEXTURE_STREAM_PATH_MAX];
  uint32_t path_length;
  /* A queued or loading stream that replaces this slot's resident texture
   * with one loaded at the current limit. */
  bool8_t reload;
  /* This resident stream has such a reload in progress. */
  bool8_t reloading;
  VkrMaterialTextureResidencyState state;
  VkrResourceHandleInfo request;
  VkrTextureHandle resident_texture;
  uint64_t resident_bytes;
  uint64_t attempt_relief_generation;
  /* 1-based index of the replacement this texture belongs to; 0 for a texture
   * that binds on its own when it loads. */
  uint32_t replacement;
} VkrMaterialTextureStream;

/* A live material's next definition. Its textures stream as a group, and the
 * definition and every texture publish in one republication once all of them
 * have loaded or failed, so no frame shows new factors with old textures. */
typedef struct VkrMaterialReplacement {
  VkrMaterialHandle material; /* id 0 marks a free entry. */
  VkrMaterial definition;     /* Factors and state; textures are defaults. */
  uint32_t loading;           /* Group textures neither staged nor failed. */
} VkrMaterialReplacement;

typedef struct VkrMaterialTextureStreamStats {
  uint32_t stream_count;
  uint32_t pending_count;
  uint32_t in_flight_count;
  uint32_t resident_count;
  uint32_t evicted_count;
  uint32_t demanded_missing_count;
  uint32_t demanded_evicted_count;
  uint64_t failed_total;
} VkrMaterialTextureStreamStats;

/** Publication bookkeeping of one material slot for the material whose
 * generation it names. */
typedef struct VkrMaterialPublication {
  uint32_t generation;
  VkrPublicationState state;
  /* Across generations of the id, one bit per recorded command awaiting its
     completion, oldest lowest: whether it can change a caster's shadow
     (vkr_material_system_take_shadow_change). */
  uint64_t shadow_commands;
  uint32_t shadow_command_count;
  /* What shadows read of the last published state, and the generation it
     belongs to. */
  uint64_t shadow_key;
  uint32_t shadow_key_generation;
} VkrMaterialPublication;

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

  // Slot table: sized once at init and never grown, so element pointers
  // stay valid until shutdown.
  Array_VkrMaterial materials;
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
  uint32_t texture_stream_memory_wait_count;
  uint32_t texture_stream_demanded_missing_count;
  uint32_t texture_stream_demanded_evicted_count;
  bool8_t texture_stream_memory_recovery_enabled;
  uint64_t texture_stream_relief_generation;
  uint64_t texture_stream_resident_bytes;
  uint64_t texture_stream_budget_bytes;
  uint64_t texture_stream_capacity_retry_high_water;
  bool8_t texture_stream_budget_user_configured;
  /* Load limit (`max_extent=N`) of material texture requests, zero for
   * none. It replaces the texture system's limit for these requests. */
  uint32_t texture_extent_limit;
  /* Resident textures may differ from what the limit loads. */
  bool8_t texture_reload_pending;
  uint64_t texture_stream_epoch;
  uint64_t *texture_material_last_used_epochs;
  VkrMaterialPublication *publications; /* max_material_count */
  uint64_t texture_stream_applied_total;
  uint64_t texture_stream_failed_total;
  uint64_t texture_stream_evicted_total;
  uint64_t texture_stream_pressure_stalls_total;

  VkrMaterialReplacement *replacements; /* VKR_MATERIAL_REPLACEMENT_CAPACITY */
  uint32_t replacement_count;           /* Entries in use. */
  uint64_t replacement_applied_total;

  /* High-water cursor for never-reserved slots. Released slots belong only to
   * free_ids; neither unload nor failed publication rewinds this cursor. */
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

/**
 * Replaces a live material's factors, state and textures together. Each
 * non-empty `texture_paths` entry (a resource path with its texture query, or
 * NULL) streams through the bounded residency path; once every one has loaded
 * or failed, the texture pump binds the definition and those textures in one
 * republication, a failed texture keeping its slot's default. The name,
 * shader and handle stay. A later call for the same material supersedes a
 * pending one. Returns false when the material is not live or the queues are
 * full.
 */
bool8_t vkr_material_system_replace(
    VkrMaterialSystem *system, VkrMaterialHandle material,
    const VkrMaterial *definition,
    const char *const texture_paths[VKR_TEXTURE_SLOT_COUNT]);

/**
 * Sets the load limit of material texture requests: the largest extent of a
 * loaded texture, zero for none. It replaces the texture system's limit for
 * these requests, so callers pass the effective limit. Later requests carry
 * it. Resident textures whose mip chain it changes
 * reload through the bounded stream queue, recently drawn materials first,
 * and keep their texture bound until the reload binds.
 */
void vkr_material_system_set_texture_extent_limit(VkrMaterialSystem *system,
                                                  uint32_t max_extent);

/** Replacements still waiting for their textures or their publication. */
uint32_t
vkr_material_system_pending_replacements(const VkrMaterialSystem *system);

/** Applies up to max_updates ready streamed textures to live material rows. */
void vkr_material_system_pump_texture_streams(VkrMaterialSystem *system,
                                              uint32_t max_updates);

/** Refreshes demand counters once after world demand/publication changes. */
void vkr_material_system_refresh_texture_stream_demand(
    VkrMaterialSystem *system);

/** Commits successful reduced Scene output; retries each waiting request once.
 */
void vkr_material_system_commit_scene_memory_relief(VkrMaterialSystem *system,
                                                    uint64_t generation);

/** Cancels and releases every pending streamed texture for a material. */
void vkr_material_system_cancel_texture_streams(VkrMaterialSystem *system,
                                                VkrMaterialHandle material);

/** Returns one coherent render-thread snapshot of texture-stream progress. */
VkrMaterialTextureStreamStats
vkr_material_system_get_texture_stream_stats(const VkrMaterialSystem *system);

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
/** Capacity retries use a finite high-water value even while budget is
 * unlimited. */
void vkr_material_system_set_texture_capacity_budget(
    VkrMaterialSystem *system, uint64_t budget_bytes,
    uint64_t capacity_allowance);

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
/** Publication state of a published material, NULL when none was recorded
 * for this generation. */
VkrPublicationState *vkr_material_system_publication(VkrMaterialSystem *system,
                                                     VkrMaterialHandle handle);

bool8_t vkr_material_system_unpublish(VkrMaterialSystem *system,
                                      VkrMaterialHandle handle);

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

/** True when the scalar transmission factor selects the transmission path. */
bool8_t
vkr_material_system_material_is_transmissive(const VkrMaterial *material);

/* Whether the oldest outstanding publication command of material `id` could
   change a caster's shadow; call once per completion naming the material.
   Shadows read an opaque material's geometry alone, so republishing one
   with other textures or shading changes none. True when unknown. */
bool8_t vkr_material_system_take_shadow_change(VkrMaterialSystem *system,
                                               uint32_t id);

/* Whether a material whose shadow reads its textures (alpha cutout or
   transmission) samples texture `id`. */
bool8_t
vkr_material_system_shadow_reads_texture(const VkrMaterialSystem *system,
                                         uint32_t id);

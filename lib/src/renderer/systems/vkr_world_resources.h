#pragma once

/**
 * @file vkr_world_resources.h
 * @brief Fallback IBL resources, scene bake preparation, and 3D text slots.
 *
 * Retains fallback IBL handles and persistent 3D text slots.
 */

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "renderer/resources/world/vkr_text_3d.h"
#include "renderer/vkr_renderer.h"

struct VkrRenderAssets;
typedef struct VkrWorldTextCreateData {
  uint32_t text_id;
  String8 content;
  const VkrText3DConfig *config; // Optional; NULL uses defaults
  VkrTransform transform;
} VkrWorldTextCreateData;

typedef struct VkrScene VkrScene;
typedef struct VkrPreparedTextDraw VkrPreparedTextDraw;

/**
 * @brief A single 3D text slot in the world resources.
 *
 * Holds the 3D text resource. Slots are indexed by text_id; inactive slots
 * may be reused for new text.
 */
typedef struct VkrWorldTextSlot {
  VkrText3D text; /**< 3D text resource and GPU state */
  bool8_t active; /**< Slot is in use and should be rendered */
} VkrWorldTextSlot;
Array(VkrWorldTextSlot);

/**
 * @brief World IBL state and 3D text slots.
 *
 * Retains fallback IBL handles and a fixed array of packet-ready 3D text slots.
 */
typedef struct VkrWorldResources {
  Array_VkrWorldTextSlot text_slots; /**< Allocated 3D text slots */

  VkrTextureHandle ibl_fallback_source_cubemap;
  VkrTextureHandle ibl_fallback_prefilter_cubemap;

  bool8_t ibl_default_ready;
  bool8_t hdr_capability_failure_logged;
  uint32_t hdr_ibl_max_cube_extent;
  uint32_t hdr_ibl_max_mip_levels;

  bool8_t initialized; /**< Resources have been initialized */
} VkrWorldResources;

/**
 * @brief Initialize text slots and fallback IBL state.
 * @param assets Published asset owner
 * @param resources World resources to initialize
 * @return true on success, false on failure
 */
bool8_t vkr_world_resources_init(struct VkrRenderAssets *assets,
                                 VkrWorldResources *resources);

/**
 * @brief Release fallback IBL handles and text resources.
 * @param assets Published asset owner
 * @param resources World resources to shutdown
 */
void vkr_world_resources_shutdown(struct VkrRenderAssets *assets,
                                  VkrWorldResources *resources);

/** Prepares scene-owned source and prefilter textures for native baking. */
bool8_t
vkr_world_resources_prepare_scene_environment(struct VkrRenderAssets *assets,
                                              VkrWorldResources *resources,
                                              VkrScene *scene);

bool8_t vkr_world_resources_prepare_scene_reflection_probes(
    struct VkrRenderAssets *assets, VkrWorldResources *resources,
    VkrScene *scene);

/**
 * @brief Produces scene IBL maps when the scene environment bake is pending.
 *
 * Failure does not abort rendering; bake state transitions to FAILED so
 * fallback maps remain active.
 */
void vkr_world_resources_bake_scene_ibl_if_pending(
    struct VkrRenderAssets *assets, VkrWorldResources *resources,
    VkrScene *scene);

/**
 * @brief Bakes all pending local reflection probes for the active scene.
 */
void vkr_world_resources_bake_scene_reflection_probes_if_pending(
    struct VkrRenderAssets *assets, VkrWorldResources *resources,
    VkrScene *scene);

/**
 * @brief Create or replace a 3D text slot.
 *
 * Uses payload->text_id when provided to target a specific slot; otherwise
 * allocates a free slot. Copies content and config from payload.
 * @param assets Published asset owner
 * @param resources World resources
 * @param payload Create data (content, config, transform)
 * @return true on success, false on failure
 */
bool8_t vkr_world_resources_text_create(struct VkrRenderAssets *assets,
                                        VkrWorldResources *resources,
                                        const VkrWorldTextCreateData *payload);

/**
 * @brief Update text content for a 3D text slot.
 * @param resources World resources
 * @param text_id Slot id from vkr_world_resources_text_create
 * @param content New text content (copied)
 * @return false when the slot is missing or content allocation fails
 */
bool8_t vkr_world_resources_text_update(VkrWorldResources *resources,
                                        uint32_t text_id, String8 content);

/**
 * @brief Update the transform for a 3D text slot.
 * @param resources World resources
 * @param text_id Slot id
 * @param transform New world transform (position, rotation, scale)
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_set_transform(VkrWorldResources *resources,
                                               uint32_t text_id,
                                               const VkrTransform *transform);

/**
 * @brief Destroy a 3D text slot.
 *
 * Releases the slot for reuse. Invalidates text_id.
 * @param resources World resources
 * @param text_id Slot id to destroy
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_destroy(VkrWorldResources *resources,
                                         uint32_t text_id);

/**
 * Build world-text draws in frame scratch. Empty output succeeds. Geometry
 * preparation or allocation failure returns false without publishing draws.
 * Draw records borrow retained text geometry until submission returns.
 */
bool8_t vkr_world_resources_prepare_text_draws(VkrWorldResources *resources,
                                               VkrAllocator *scratch,
                                               VkrPreparedTextDraw **out_draws,
                                               uint32_t *out_count);

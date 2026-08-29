#pragma once

/**
 * @file vkr_world_resources.h
 * @brief Shared world pipelines, HDR/IBL state, and 3D text resources.
 *
 * Owns packet-facing HDR/IBL state and persistent 3D text slots.
 */

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "renderer/resources/world/vkr_text_3d.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;
typedef struct VkrScene VkrScene;
typedef struct VkrPreparedTextDraw VkrPreparedTextDraw;

typedef struct VkrWorldIblProbeSlot {
  VkrTextureOpaqueHandle prefilter_map;
  Vec3 center;
  Vec3 extents;
  float32_t blend_distance;
  float32_t weight;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  bool8_t box_projection_enabled;
} VkrWorldIblProbeSlot;

float32_t vkr_world_resources_probe_fragment_influence(Vec3 center,
                                                       Vec3 extents,
                                                       float32_t blend_distance,
                                                       Vec3 world_position);

bool8_t vkr_world_resources_probe_intersects_sphere(Vec3 center, Vec3 extents,
                                                    float32_t blend_distance,
                                                    Vec3 sphere_center,
                                                    float32_t sphere_radius);

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
 * Manages selected IBL handles and a fixed array of packet-ready 3D text slots.
 */
typedef struct VkrWorldResources {
  Array_VkrWorldTextSlot text_slots; /**< Allocated 3D text slots */

  VkrTextureHandle ibl_fallback_source_cubemap;
  VkrTextureHandle ibl_fallback_prefilter_cubemap;

  VkrTextureHandle ibl_active_prefilter_cubemap;
  bool8_t ibl_active_enabled;
  float32_t ibl_active_intensity;
  float32_t ibl_active_diffuse_intensity;
  float32_t ibl_active_specular_intensity;
  bool8_t ibl_default_ready;
  bool8_t hdr_capability_failure_logged;
  uint32_t hdr_ibl_max_cube_extent;
  uint32_t hdr_ibl_max_mip_levels;

  bool8_t initialized; /**< Resources have been initialized */
} VkrWorldResources;

/**
 * @brief Initialize default world pipelines and text slots.
 * @param rf Renderer frontend
 * @param resources World resources to initialize
 * @return true on success, false on failure
 */
bool8_t vkr_world_resources_init(struct s_RendererFrontend *rf,
                                 VkrWorldResources *resources);

/**
 * @brief Release pipelines and text resources.
 * @param rf Renderer frontend
 * @param resources World resources to shutdown
 */
void vkr_world_resources_shutdown(struct s_RendererFrontend *rf,
                                  VkrWorldResources *resources);

/** Prepares all scene-owned bake products and cached face/mip targets. */
bool8_t
vkr_world_resources_prepare_scene_environment(struct s_RendererFrontend *rf,
                                              VkrWorldResources *resources,
                                              VkrScene *scene);

/** Destroys cached target views before their scene-owned textures retire. */
void vkr_world_resources_release_scene_environment_targets(
    struct s_RendererFrontend *rf, VkrScene *scene);

bool8_t vkr_world_resources_prepare_scene_reflection_probes(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    VkrScene *scene);

void vkr_world_resources_release_scene_reflection_probe_targets(
    struct s_RendererFrontend *rf, VkrScene *scene);

/**
 * @brief Produces scene IBL maps when the scene environment bake is pending.
 *
 * Failure does not abort rendering; bake state transitions to FAILED so
 * fallback maps remain active.
 */
void vkr_world_resources_bake_scene_ibl_if_pending(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    VkrScene *scene);

/**
 * @brief Bakes all pending local reflection probes for the active scene.
 */
void vkr_world_resources_bake_scene_reflection_probes_if_pending(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    VkrScene *scene);

/**
 * @brief Selects active IBL maps from scene-ready data or fallback maps.
 */
void vkr_world_resources_set_active_ibl_from_scene_or_default(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    const VkrScene *scene);

/**
 * @brief Selects two local probe candidates plus a global fallback.
 *
 * Slot selection prefers local reflection probes by influence and falls back
 * to active/global IBL maps when no local probe contributes.
 */
void vkr_world_resources_select_probe_slots_for_position(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    const VkrScene *scene, Vec3 world_position,
    VkrWorldIblProbeSlot out_slots[3]);

void vkr_world_resources_select_probe_slots_for_bounds(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    const VkrScene *scene, Vec3 bounds_center, float32_t bounds_radius,
    VkrWorldIblProbeSlot out_slots[3]);

/**
 * @brief Create or replace a 3D text slot.
 *
 * Uses payload->text_id when provided to target a specific slot; otherwise
 * allocates a free slot. Copies content and config from payload.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param payload Create data (content, config, transform)
 * @return true on success, false on failure
 */
bool8_t vkr_world_resources_text_create(struct s_RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        const VkrWorldTextCreateData *payload);

/**
 * @brief Update text content for a 3D text slot.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param text_id Slot id from vkr_world_resources_text_create
 * @param content New text content (copied)
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_update(struct s_RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        uint32_t text_id, String8 content);

/**
 * @brief Update the transform for a 3D text slot.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param text_id Slot id
 * @param transform New world transform (position, rotation, scale)
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_set_transform(struct s_RendererFrontend *rf,
                                               VkrWorldResources *resources,
                                               uint32_t text_id,
                                               const VkrTransform *transform);

/**
 * @brief Destroy a 3D text slot.
 *
 * Releases the slot for reuse. Invalidates text_id.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param text_id Slot id to destroy
 * @return true on success, false if slot not found
 */
bool8_t vkr_world_resources_text_destroy(struct s_RendererFrontend *rf,
                                         VkrWorldResources *resources,
                                         uint32_t text_id);

/** Builds packet-ready world-text descriptors without issuing GPU commands. */
uint32_t vkr_world_resources_prepare_text_draws(struct s_RendererFrontend *rf,
                                                VkrWorldResources *resources,
                                                VkrPreparedTextDraw *out_draws,
                                                uint32_t capacity);

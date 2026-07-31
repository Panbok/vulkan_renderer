#pragma once

/**
 * @file vkr_world_resources.h
 * @brief Stateless world pipelines and 3D text resources.
 *
 * Owns the default world pipelines (opaque, transparent, overlay) and the
 * persistent 3D text slots used by the stateless renderer.
 */

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "renderer/resources/world/vkr_text_3d.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;
typedef struct VkrScene VkrScene;

typedef struct VkrWorldIblProbeSlot {
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
} VkrWorldIblProbeSlot;

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
 * @brief World resources: pipelines and 3D text slots.
 *
 * Manages world pipelines (opaque, transparent, overlay) and a fixed array
 * of 3D text slots for stateless rendering. Uses the current global view
 * and projection from the render packet.
 */
typedef struct VkrWorldResources {
  VkrShaderConfig shader_config;          /**< Base world shader config */
  VkrPipelineHandle pipeline;             /**< Opaque geometry pipeline */
  VkrPipelineHandle transparent_pipeline; /**< Transparent geometry pipeline */
  VkrPipelineHandle overlay_pipeline;     /**< Overlay geometry pipeline */
  VkrShaderConfig pbr_shader_config;      /**< PBR world shader config */
  VkrShaderConfig
      pbr_world_shader_config; /**< PBR world shader config (opaque name) */
  VkrShaderConfig pbr_transparent_shader_config; /**< PBR shader config for
                                                    transparent domain */
  VkrShaderConfig
      pbr_overlay_shader_config;  /**< PBR shader config for overlay domain */
  VkrPipelineHandle pbr_pipeline; /**< PBR opaque pipeline */
  VkrPipelineHandle pbr_transparent_pipeline; /**< PBR transparent pipeline */
  VkrPipelineHandle pbr_overlay_pipeline;     /**< PBR overlay pipeline */

  VkrShaderConfig text_shader_config; /**< 3D text shader config */
  VkrPipelineHandle text_pipeline;    /**< 3D text glyph pipeline */
  Array_VkrWorldTextSlot text_slots;  /**< Allocated 3D text slots */

  VkrTextureHandle ibl_fallback_source_cubemap;
  VkrTextureHandle ibl_fallback_irradiance_cubemap;
  VkrTextureHandle ibl_fallback_prefilter_cubemap;
  VkrTextureHandle ibl_brdf_lut;

  VkrRenderPassHandle ibl_bake_render_pass;
  VkrShaderConfig ibl_diffuse_bake_shader_config;
  VkrShaderConfig ibl_specular_bake_shader_config;
  VkrPipelineHandle ibl_diffuse_bake_pipeline;
  VkrPipelineHandle ibl_specular_bake_pipeline;
  VkrRendererInstanceStateHandle ibl_diffuse_bake_instance_state;
  VkrRendererInstanceStateHandle ibl_specular_bake_instance_state;
  VkrGeometryHandle ibl_bake_cube_geometry;

  VkrTextureHandle ibl_active_irradiance_cubemap;
  VkrTextureHandle ibl_active_prefilter_cubemap;
  bool8_t ibl_active_enabled;
  float32_t ibl_active_intensity;
  float32_t ibl_active_diffuse_intensity;
  float32_t ibl_active_specular_intensity;
  bool8_t ibl_bake_runtime_ready;
  bool8_t ibl_bake_render_pass_owned;
  bool8_t ibl_default_ready;

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

/**
 * @brief Ensures fallback IBL maps and BRDF LUT are ready for binding.
 *
 * This function is safe to call every frame; work is done once and cached.
 * Returns false only when fallback cube acquisition fails.
 */
bool8_t
vkr_world_resources_ensure_default_ibl_ready(struct s_RendererFrontend *rf,
                                             VkrWorldResources *resources);

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
 * @brief Pushes currently active IBL maps and scalar controls to materials.
 */
void vkr_world_resources_apply_active_ibl_to_material_system(
    struct s_RendererFrontend *rf, VkrWorldResources *resources);

/**
 * @brief Selects and blends two probe slots for the given world position.
 *
 * Slot selection prefers local reflection probes by influence and falls back
 * to active/global IBL maps when no local probe contributes.
 */
void vkr_world_resources_select_probe_slots_for_position(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    const VkrScene *scene, Vec3 world_position, VkrWorldIblProbeSlot out_slots[2]);

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

/**
 * @brief Render world text using the current global frame state.
 *
 * Uses view and projection from the render packet.
 * @param rf Renderer frontend
 * @param resources World resources
 */
void vkr_world_resources_render_text(struct s_RendererFrontend *rf,
                                     VkrWorldResources *resources);

/**
 * @brief Render world text into the picking pass.
 *
 * Same geometry as render_text but uses the given picking pipeline for ID
 * output.
 * @param rf Renderer frontend
 * @param resources World resources
 * @param pipeline Picking pipeline to bind
 */
void vkr_world_resources_render_picking_text(struct s_RendererFrontend *rf,
                                             VkrWorldResources *resources,
                                             VkrPipelineHandle pipeline);

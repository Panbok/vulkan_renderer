#pragma once

/**
 * @file vkr_world_resources.h
 * @brief Shared world pipelines, HDR/IBL state, and 3D text resources.
 *
 * Owns world pipelines, prepared HDR/IBL runtime state, tonemapping, and the
 * persistent 3D text slots used by the stateless renderer.
 */

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "math/vkr_transform.h"
#include "renderer/resources/world/vkr_text_3d.h"
#include "renderer/systems/vkr_ibl_bake_types.h"
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
      pbr_overlay_shader_config; /**< PBR shader config for overlay domain */
  VkrShaderConfig pbr_double_sided_shader_config;
  VkrShaderConfig pbr_transparent_double_sided_shader_config;
  VkrShaderConfig pbr_overlay_double_sided_shader_config;
  VkrPipelineHandle pbr_pipeline;             /**< PBR opaque pipeline */
  VkrPipelineHandle pbr_transparent_pipeline; /**< PBR transparent pipeline */
  VkrPipelineHandle pbr_overlay_pipeline;     /**< PBR overlay pipeline */
  VkrPipelineHandle pbr_double_sided_pipeline;
  VkrPipelineHandle pbr_transparent_double_sided_pipeline;
  VkrPipelineHandle pbr_overlay_double_sided_pipeline;

  VkrShaderConfig tonemap_shader_config;
  VkrPipelineHandle tonemap_pipeline;
  VkrRendererInstanceStateHandle tonemap_instance_state;
  uint32_t tonemap_shader_id;

  VkrShaderConfig text_shader_config; /**< 3D text shader config */
  VkrPipelineHandle text_pipeline;    /**< 3D text glyph pipeline */
  Array_VkrWorldTextSlot text_slots;  /**< Allocated 3D text slots */

  VkrTextureHandle ibl_fallback_source_cubemap;
  VkrTextureHandle ibl_legacy_fallback_source_cubemap;
  VkrTextureHandle ibl_default_delivery_equirect;
  VkrTextureHandle ibl_fallback_irradiance_cubemap;
  VkrTextureHandle ibl_fallback_prefilter_cubemap;
  VkrTextureHandle ibl_brdf_lut;

  VkrRenderPassHandle ibl_bake_render_pass;
  VkrShaderConfig ibl_equirect_bake_shader_config;
  VkrShaderConfig ibl_diffuse_bake_shader_config;
  VkrShaderConfig ibl_specular_bake_shader_config;
  VkrShaderConfig ibl_brdf_bake_shader_config;
  VkrPipelineHandle ibl_equirect_bake_pipeline;
  VkrPipelineHandle ibl_diffuse_bake_pipeline;
  VkrPipelineHandle ibl_specular_bake_pipeline;
  VkrPipelineHandle ibl_brdf_bake_pipeline;
  uint32_t ibl_equirect_bake_shader_id;
  uint32_t ibl_diffuse_bake_shader_id;
  uint32_t ibl_specular_bake_shader_id;
  uint32_t ibl_brdf_bake_shader_id;
  VkrGeometryHandle ibl_bake_plane_geometry;
  VkrRenderTargetHandle ibl_brdf_bake_target;
  VkrIblPreparedTargetSet ibl_default_source_targets;
  VkrIblPreparedTargetSet ibl_default_irradiance_targets;
  VkrIblPreparedTargetSet ibl_default_prefilter_targets;

  VkrTextureHandle ibl_active_irradiance_cubemap;
  VkrTextureHandle ibl_active_prefilter_cubemap;
  bool8_t ibl_active_enabled;
  float32_t ibl_active_intensity;
  float32_t ibl_active_diffuse_intensity;
  float32_t ibl_active_specular_intensity;
  bool8_t ibl_bake_runtime_ready;
  bool8_t ibl_bake_render_pass_owned;
  bool8_t ibl_default_ready;
  bool8_t ibl_default_prepared;
  bool8_t ibl_brdf_baked;
  bool8_t ibl_default_cube_baked;
  bool8_t supports_hdr_ibl;
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

/**
 * @brief Ensures fallback IBL maps and BRDF LUT are ready for binding.
 *
 * This function is safe to call every frame; work is done once and cached.
 * Returns false only when fallback cube acquisition fails.
 */
bool8_t
vkr_world_resources_ensure_default_ibl_ready(struct s_RendererFrontend *rf,
                                             VkrWorldResources *resources);

/** Allocates default HDR IBL targets after the skybox system is initialized. */
bool8_t vkr_world_resources_prepare_default_ibl(struct s_RendererFrontend *rf,
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
 * @brief Pushes currently active IBL maps and scalar controls to materials.
 */
void vkr_world_resources_apply_active_ibl_to_material_system(
    struct s_RendererFrontend *rf, VkrWorldResources *resources);

/** Records the fullscreen HDR scene-color to swapchain tonemap draw. */
VkrRendererError vkr_world_resources_record_tonemap(
    struct s_RendererFrontend *rf, VkrWorldResources *resources,
    VkrTextureOpaqueHandle source_hdr, uint32_t width, uint32_t height,
    float32_t exposure);

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

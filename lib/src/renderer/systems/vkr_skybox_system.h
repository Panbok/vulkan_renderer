#pragma once

/**
 * @file vkr_skybox_system.h
 * @brief Stateless skybox resources and rendering helper.
 *
 * Owns the skybox pipeline, cube geometry, and default cubemap. Rendering
 * uses per-pass payload (cubemap, material) and frame globals (view, projection).
 */

#include "defines.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_render_packet.h"

struct s_RendererFrontend;

/**
 * @brief Skybox system state: pipeline, geometry, and cubemap.
 *
 * Provides a skybox render path. The default cubemap is used when the pass
 * payload does not specify one. Call render_packet with payload from the
 * render graph pass context.
 */
typedef struct VkrSkyboxSystem {
  VkrShaderConfig shader_config;           /**< Skybox shader config */
  VkrPipelineHandle pipeline;              /**< Skybox cube pipeline */
  VkrGeometryHandle cube_geometry;        /**< Unit cube geometry */
  VkrTextureHandle cube_map_texture;       /**< Default cubemap (fallback) */
  VkrRendererInstanceStateHandle instance_state; /**< Per-frame instance state */
  bool8_t initialized;                     /**< System has been initialized */
} VkrSkyboxSystem;

/**
 * @brief Initialize skybox shader, pipeline, geometry, and default cubemap.
 * @param rf Renderer frontend
 * @param system Skybox system to initialize
 * @return true on success, false on failure
 */
bool8_t vkr_skybox_system_init(struct s_RendererFrontend *rf,
                               VkrSkyboxSystem *system);

/**
 * @brief Release skybox resources.
 * @param rf Renderer frontend
 * @param system Skybox system to shutdown
 */
void vkr_skybox_system_shutdown(struct s_RendererFrontend *rf,
                                VkrSkyboxSystem *system);

/**
 * @brief Render skybox using packet payload data.
 *
 * Uses payload->cubemap and payload->material when valid; otherwise falls back
 * to system defaults. View and projection come from globals.
 * @param rf Renderer frontend
 * @param system Skybox system
 * @param payload Per-pass cubemap and material (from render graph)
 * @param globals Frame view and projection matrices
 */
void vkr_skybox_system_render_packet(struct s_RendererFrontend *rf,
                                     const VkrSkyboxSystem *system,
                                     const VkrSkyboxPassPayload *payload,
                                     const VkrFrameGlobals *globals);

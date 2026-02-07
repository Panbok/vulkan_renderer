/**
 * @file vkr_editor_viewport.h
 * @brief Editor viewport compositing resources and layout helpers.
 *
 * Owns the persistent GPU resources needed to draw the offscreen scene texture
 * into the editor layout. Layout/mapping is app-owned; helpers are provided to
 * compute the standard editor panel rect and fill a packet payload.
 */
#pragma once

#include "defines.h"
#include "math/vec.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_viewport.h"

struct s_RendererFrontend;

/**
 * @brief Persistent resources for editor viewport compositing.
 *
 * This state owns a mesh (single quad) and the viewport display material/
 * pipeline. It is renderer-owned and reused across frames.
 */
typedef struct VkrEditorViewportResources {
  VkrShaderConfig shader_config;
  VkrPipelineHandle pipeline;
  VkrMaterialHandle material;
  VkrRenderPassHandle renderpass;
  bool8_t owns_renderpass;
  uint32_t mesh_index; /**< Mesh manager index for the viewport quad. */
  Vec2 plane_size;     /**< Base quad size used to compute model scale. */
  bool8_t initialized;
} VkrEditorViewportResources;

/**
 * @brief Initialize editor viewport resources (shader, pipeline, mesh).
 *
 * Non-fatal: returns false if resources failed to create.
 */
bool8_t vkr_editor_viewport_init(struct s_RendererFrontend *rf,
                                 VkrEditorViewportResources *resources);

/**
 * @brief Release editor viewport resources.
 */
void vkr_editor_viewport_shutdown(struct s_RendererFrontend *rf,
                                  VkrEditorViewportResources *resources);

/**
 * @brief Compute editor viewport mapping for the standard editor layout.
 *
 * Uses the same panel proportions as the original editor viewport (top/bottom/left/
 * right gutters). render_scale is clamped to a safe range.
 */
bool8_t vkr_editor_viewport_compute_mapping(uint32_t window_width,
                                            uint32_t window_height,
                                            VkrViewportFitMode fit_mode,
                                            float32_t render_scale,
                                            VkrViewportMapping *out_mapping);

/**
 * @brief Fill a one-draw editor pass payload from a mapping.
 *
 * The caller owns the storage for the draw/instance arrays and payload.
 */
bool8_t vkr_editor_viewport_build_payload(
    const VkrEditorViewportResources *resources,
    const VkrViewportMapping *mapping, VkrDrawItem *out_draw,
    VkrInstanceDataGPU *out_instance, VkrEditorPassPayload *out_payload);

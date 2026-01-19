/**
 * @file vkr_picking_system.h
 * @brief Pixel-perfect 3D object picking system.
 *
 * This module provides GPU-accelerated object picking by rendering the scene
 * with a specialized shader that outputs object IDs to an R32_UINT render
 * target. Clicking in the viewport triggers an async pixel readback to
 * determine which object was selected.
 *
 * Usage workflow:
 * 1. Call vkr_picking_init() during renderer setup
 * 2. Call vkr_picking_resize() when the viewport size changes
 * 3. On mouse click, convert window coords to target coords using
 *    vkr_viewport_mapping_window_to_target_pixel(), then call
 *    vkr_picking_request()
 * 4. Call vkr_picking_render() during the frame (renders only if requested)
 * 5. Call vkr_picking_get_result() to poll for the result
 * 6. Call vkr_picking_shutdown() during cleanup
 *
 * The picking result provides an encoded object_id which can be decoded via
 * vkr_picking_decode_id() and mapped to scene or UI/world text IDs.
 */
#pragma once

#include "defines.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_renderer.h"

// ============================================================================
// Types
// ============================================================================

/**
 * @brief State machine for picking request lifecycle.
 */
typedef enum VkrPickingState {
  VKR_PICKING_STATE_IDLE = 0,         /**< No pick in progress */
  VKR_PICKING_STATE_RENDER_PENDING,   /**< Pick requested, needs render pass */
  VKR_PICKING_STATE_READBACK_PENDING, /**< Rendered, GPU readback in flight */
  VKR_PICKING_STATE_RESULT_READY,     /**< Result available */
} VkrPickingState;

/**
 * @brief Picking system context.
 *
 * Manages the off-screen picking render target, pipeline, and async
 * readback state for pixel-perfect object selection.
 */
typedef struct VkrPickingContext {
  // -------------------------------------------------------------------------
  // Render resources (created on init, recreated on resize)
  // -------------------------------------------------------------------------
  VkrTextureOpaqueHandle picking_texture; /**< R32_UINT color target */
  VkrTextureOpaqueHandle picking_depth;   /**< Depth attachment */
  VkrRenderPassHandle picking_pass;       /**< Picking render pass */
  VkrRenderTargetHandle picking_target;   /**< Render target */
  VkrPipelineHandle picking_pipeline;     /**< Picking mesh pipeline */
  VkrPipelineHandle
      picking_overlay_pipeline; /**< Picking mesh pipeline (no depth test). */
  VkrRendererInstanceStateHandle
      mesh_instance_state; /**< Shared instance state for mesh samplers. */
  VkrRendererInstanceStateHandle
      mesh_overlay_instance_state; /**< Instance state for overlay pipeline. */
  VkrPipelineHandle
      picking_transparent_pipeline; /**< Picking mesh pipeline (no depth write)
                                       for transparent submeshes. */
  VkrRendererInstanceStateHandle mesh_transparent_instance_state; /**< Instance
                                                                     state for
                                                                     transparent
                                                                     pipeline
                                                                     samplers.
                                                                   */
  VkrShaderConfig shader_config;           /**< Cached mesh shader config */
  VkrPipelineHandle picking_text_pipeline; /**< Picking text pipeline */
  VkrPipelineHandle
      picking_world_text_pipeline;    /**< Picking text pipeline for WORLD text
                                         (depth-tested, no depth write). */
  VkrShaderConfig text_shader_config; /**< Cached text shader config */

  // Light gizmo picking resources
  VkrGeometryHandle light_gizmo_cube; /**< Unit cube for light picking gizmos */

  // -------------------------------------------------------------------------
  // Target dimensions
  // -------------------------------------------------------------------------
  uint32_t width;  /**< Current render target width */
  uint32_t height; /**< Current render target height */

  // -------------------------------------------------------------------------
  // Pick request state
  // -------------------------------------------------------------------------
  VkrPickingState state;     /**< Current picking state */
  uint32_t requested_x;      /**< Requested pixel X coordinate */
  uint32_t requested_y;      /**< Requested pixel Y coordinate */
  uint32_t result_object_id; /**< Result object ID (0 = background) */

  // -------------------------------------------------------------------------
  // Initialization flag
  // -------------------------------------------------------------------------
  bool8_t initialized; /**< True if context is initialized */
} VkrPickingContext;

/**
 * @brief Result of a picking operation.
 */
typedef struct VkrPickResult {
  uint32_t object_id; /**< Encoded object ID (0 = no hit) */
  bool8_t hit;        /**< True if an object was hit */
} VkrPickResult;

// ============================================================================
// API
// ============================================================================

/**
 * @brief Initialize the picking system.
 *
 * Creates the picking render target, render pass, and loads the picking
 * pipeline. Must be called after the renderer is initialized.
 *
 * @param renderer The renderer frontend
 * @param ctx Picking context to initialize (caller-owned)
 * @param width Initial render target width
 * @param height Initial render target height
 * @return true on success, false on failure
 */
bool8_t vkr_picking_init(struct s_RendererFrontend *renderer,
                         VkrPickingContext *ctx, uint32_t width,
                         uint32_t height);

/**
 * @brief Resize the picking render target.
 *
 * Call when the viewport dimensions change. Destroys and recreates the
 * picking attachments at the new size.
 *
 * If a pick is in progress, this function waits for the GPU to become idle
 * (completing any pending readback) before destroying attachments, then
 * recreates them at the new size. The picking state is not reset, so callers
 * should check for results via vkr_picking_get_result() before resizing, or
 * call vkr_picking_cancel() to explicitly reset the state if the pending pick
 * should be discarded.
 *
 * @param renderer The renderer frontend
 * @param ctx Picking context
 * @param new_width New render target width
 * @param new_height New render target height
 */
void vkr_picking_resize(struct s_RendererFrontend *renderer,
                        VkrPickingContext *ctx, uint32_t new_width,
                        uint32_t new_height);

/**
 * @brief Request a pick at the specified render-target coordinates.
 *
 * Coordinates should be in render-target pixel space, not window space.
 * Use vkr_viewport_mapping_window_to_target_pixel() to convert window
 * mouse coordinates.
 *
 * Only one pick can be in flight at a time. If a pick is already pending,
 * this call is ignored.
 *
 * Out-of-bounds coordinates (target_x >= width or target_y >= height) are
 * rejected: the request is ignored, a warning is logged, and no pick is
 * initiated. Valid coordinates must be in the range [0, width-1] for target_x
 * and [0, height-1] for target_y.
 *
 * @param ctx Picking context
 * @param target_x X coordinate in render target pixels (must be in [0,
 * width-1])
 * @param target_y Y coordinate in render target pixels (must be in [0,
 * height-1])
 */
void vkr_picking_request(VkrPickingContext *ctx, uint32_t target_x,
                         uint32_t target_y);

/**
 * @brief Render the picking pass.
 *
 * Renders all visible meshes to the picking target with object IDs.
 * Only renders if a pick is requested (state == RENDER_PENDING).
 * After rendering, initiates async pixel readback.
 *
 * Call this during the frame, before or after the main scene render.
 * The picking pass uses its own render target and does not affect
 * the main scene.
 *
 * @param renderer The renderer frontend
 * @param ctx Picking context
 * @param mesh_manager Mesh manager containing meshes to render
 */
void vkr_picking_render(struct s_RendererFrontend *renderer,
                        VkrPickingContext *ctx,
                        struct VkrMeshManager *mesh_manager);

/**
 * @brief Get the result of a picking operation.
 *
 * Polls the async readback status and updates the stored result when a new
 * readback completes.
 *
 * This function always returns the last known pick result. If no pick has
 * completed yet, it returns {object_id=0, hit=false}.
 *
 * @param renderer The renderer frontend
 * @param ctx Picking context
 * @return Last known pick result
 */
VkrPickResult vkr_picking_get_result(struct s_RendererFrontend *renderer,
                                     VkrPickingContext *ctx);

/**
 * @brief Check if a pick is currently in progress.
 *
 * @param ctx Picking context
 * @return true if a pick is pending (render or readback in flight)
 */
bool8_t vkr_picking_is_pending(const VkrPickingContext *ctx);

/**
 * @brief Cancel any pending pick request.
 *
 * Resets the picking state to IDLE, discarding any pending results.
 *
 * @param ctx Picking context
 */
void vkr_picking_cancel(VkrPickingContext *ctx);

/**
 * @brief Invalidate picking instance states.
 *
 * Releases shader instance states but keeps the picking context alive.
 * Call this when scene resources (textures) are being destroyed to ensure
 * descriptor sets don't reference stale textures. New instance states will
 * be acquired automatically on the next picking render.
 *
 * Safe to call at any time, including during an active pick. If a pick is
 * in progress, instance states will be reacquired on the next render cycle.
 *
 * @param renderer The renderer frontend
 * @param ctx Picking context
 */
void vkr_picking_invalidate_instance_states(struct s_RendererFrontend *renderer,
                                            VkrPickingContext *ctx);

/**
 * @brief Render light gizmos for picking.
 *
 * Renders a small cube proxy at each pickable light's world position.
 * Light entities must have SceneRenderId assigned to be pickable.
 * Call this during the picking pass after mesh rendering but before text.
 *
 * @param renderer The renderer frontend
 * @param ctx Picking context
 * @param scene Scene containing light entities (may be NULL)
 */
void vkr_picking_render_light_gizmos(struct s_RendererFrontend *renderer,
                                     VkrPickingContext *ctx,
                                     const struct VkrScene *scene);

/**
 * @brief Shutdown the picking system.
 *
 * Releases all GPU resources and resets the context.
 *
 * @param renderer The renderer frontend
 * @param ctx Picking context to shutdown
 */
void vkr_picking_shutdown(struct s_RendererFrontend *renderer,
                          VkrPickingContext *ctx);

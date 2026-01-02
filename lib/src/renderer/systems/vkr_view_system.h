#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "math/mat.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_renderer.h"

/** Maximum number of layers that can be registered in the view system. */
#define VKR_VIEW_SYSTEM_MAX_LAYERS 16
/** Maximum number of passes that can be configured per layer. */
#define VKR_VIEW_SYSTEM_MAX_LAYER_PASSES 4
/** Maximum number of behaviors that can be attached to a single layer. */
#define VKR_VIEW_SYSTEM_MAX_LAYER_BEHAVIORS 8

/**
 * @brief A layer pass groups render targets and a renderpass together for
 * rendering.
 *
 * A pass defines how a layer renders to one or more render targets. It can use
 * the swapchain color attachment, depth buffer, or custom render targets.
 *
 * @param renderpass_name Name of the renderpass to use (e.g.,
 * "Renderpass.Builtin.World").
 * @param use_swapchain_color If true, uses the swapchain's color attachment as
 * the primary target.
 * @param use_depth If true, uses the depth attachment for depth testing.
 * @param use_custom_render_targets If true, uses custom color attachments
 * instead of render targets.
 * @param renderpass Handle to the renderpass created from renderpass_name.
 * @param render_targets Array of render target handles (used when
 * use_custom_render_targets is false).
 * @param render_target_count Number of render targets in the array.
 * @param custom_color_attachments Array of texture handles for custom color
 * attachments (used when use_custom_render_targets is true).
 * @param custom_color_layouts Array of texture layouts corresponding to
 * custom_color_attachments.
 */
typedef struct VkrLayerPass {
  String8 renderpass_name;
  bool8_t use_swapchain_color;
  bool8_t use_depth;
  bool8_t use_custom_render_targets;

  VkrRenderPassHandle renderpass;
  VkrRenderTargetHandle *render_targets;
  uint32_t render_target_count;
  VkrTextureOpaqueHandle *custom_color_attachments;
  uint32_t custom_color_attachment_count;
  VkrTextureLayout *custom_color_layouts;
} VkrLayerPass;
Array(VkrLayerPass);

/**
 * @brief Internal slot for storing a layer behavior instance.
 *
 * Behaviors are reusable components that can be attached to layers to extend
 * their functionality. Each slot tracks the behavior, its handle, and active
 * state.
 *
 * @param behavior The behavior definition containing callbacks and data.
 * @param handle Unique handle identifying this behavior instance.
 * @param active Whether this behavior slot is currently active and should
 * receive callbacks.
 */
typedef struct VkrLayerBehaviorSlot {
  VkrLayerBehavior behavior;
  VkrLayerBehaviorHandle handle;
  bool8_t active;
} VkrLayerBehaviorSlot;
Array(VkrLayerBehaviorSlot);

/**
 * @brief A layer represents a composable rendering unit with one or more
 * passes.
 *
 * Layers are the primary unit of organization in the view system. Each layer
 * can have multiple passes, camera matrices, and attached behaviors. Layers are
 * sorted by order and rendered sequentially.
 *
 * @param handle Unique handle identifying this layer instance.
 * @param callbacks Lifecycle callbacks for the layer (on_create, on_render,
 * etc.).
 * @param passes Array of passes that define how this layer renders.
 * @param name Human-readable name for debugging and identification.
 * @param order Rendering order (lower values render first). Used to sort
 * layers.
 * @param width Layer width in pixels. If 0, uses window width.
 * @param height Layer height in pixels. If 0, uses window height.
 * @param view View matrix for camera positioning.
 * @param projection Projection matrix for camera perspective/orthographic
 * projection.
 * @param pass_count Number of passes in the passes array.
 * @param active Whether this layer slot is active (used internally for handle
 * validation).
 * @param sync_to_window If true, layer dimensions automatically match window
 * size.
 * @param user_data Opaque pointer for layer-specific data (set by user).
 * @param enabled Whether the layer is currently enabled and should receive
 * updates/rendering.
 * @param flags Layer flags (e.g., VKR_LAYER_FLAG_ALWAYS_UPDATE).
 * @param behaviors Array of behavior slots attached to this layer.
 * @param behavior_count Number of active behaviors in the behaviors array.
 */
typedef struct VkrLayer {
  VkrLayerHandle handle;
  VkrLayerCallbacks callbacks;
  Array_VkrLayerPass passes;
  String8 name;
  uint32_t order;
  uint32_t width;
  uint32_t height;
  Mat4 view;
  Mat4 projection;
  uint8_t pass_count;
  bool8_t active;
  bool8_t sync_to_window;
  void *user_data;
  bool8_t enabled;
  uint32_t flags;
  Array_VkrLayerBehaviorSlot behaviors;
  uint32_t behavior_count;
} VkrLayer;
Array(VkrLayer);

/**
 * @brief The view system manages all layers and coordinates their rendering.
 *
 * The view system maintains a collection of layers, sorts them by order, and
 * coordinates their lifecycle callbacks. It tracks window dimensions, input
 * state, and modal focus for UI layers.
 *
 * @param arena Arena allocator for long-lived allocations.
 * @param allocator General-purpose allocator for temporary and frame
 * allocations.
 * @param renderer Handle to the renderer frontend.
 * @param layers Array of all registered layers (active and inactive).
 * @param layer_capacity Maximum number of layers that can be stored.
 * @param render_target_count Total number of render targets created across all
 * layers.
 * @param sorted_indices Array of layer indices sorted by order (for efficient
 * rendering).
 * @param sorted_count Number of active layers in sorted_indices.
 * @param next_id Next available layer ID (used for handle generation).
 * @param window_width Current window width in pixels.
 * @param window_height Current window height in pixels.
 * @param order_dirty Flag indicating that layers need to be re-sorted.
 * @param initialized Whether the view system has been initialized.
 * @param input_state Pointer to the window's input state (read-only for
 * layers).
 * @param modal_focus_layer Handle to the layer that currently has modal focus
 * (for UI input handling).
 */
typedef struct VkrViewSystem {
  Arena *arena;
  VkrAllocator allocator;
  VkrRendererFrontendHandle renderer;

  Array_VkrLayer layers;
  uint32_t layer_capacity;
  uint32_t render_target_count;
  uint32_t *sorted_indices;
  uint32_t sorted_count;
  uint32_t next_id;
  uint32_t window_width;
  uint32_t window_height;

  bool8_t order_dirty;
  bool8_t initialized;
  InputState *input_state;
  VkrLayerHandle modal_focus_layer;
} VkrViewSystem;

/**
 * @brief Context passed to layer callbacks providing access to layer state and
 * resources.
 *
 * The layer context is the primary interface through which layers interact with
 * the view system. It provides access to the layer's properties, render
 * targets, camera matrices, and the renderer. The pass pointer may be NULL for
 * callbacks that don't operate on a specific pass (e.g., on_update, on_enable).
 *
 * @param view_system Pointer to the view system managing this layer.
 * @param layer Pointer to the layer this context represents.
 * @param pass Pointer to the current pass (NULL if not pass-specific).
 */
typedef struct VkrLayerContext {
  VkrViewSystem *view_system;
  VkrLayer *layer;
  VkrLayerPass *pass;
} VkrLayerContext;

/**
 * @brief Get the renderer frontend handle from a layer context.
 * @param ctx The layer context (must not be NULL).
 * @return The renderer frontend handle.
 */
VkrRendererFrontendHandle vkr_layer_context_get_renderer(VkrLayerContext *ctx);

/**
 * @brief Get the effective width of the layer.
 *
 * Returns the layer's width if set, otherwise returns the window width.
 *
 * @param ctx The layer context (must not be NULL).
 * @return The layer width in pixels.
 */
uint32_t vkr_layer_context_get_width(const VkrLayerContext *ctx);

/**
 * @brief Get the effective height of the layer.
 *
 * Returns the layer's height if set, otherwise returns the window height.
 *
 * @param ctx The layer context (must not be NULL).
 * @return The layer height in pixels.
 */
uint32_t vkr_layer_context_get_height(const VkrLayerContext *ctx);

/**
 * @brief Get a pointer to the layer's view matrix.
 * @param ctx The layer context (must not be NULL).
 * @return Pointer to the view matrix (read-only).
 */
const Mat4 *vkr_layer_context_get_view(const VkrLayerContext *ctx);

/**
 * @brief Get a pointer to the layer's projection matrix.
 * @param ctx The layer context (must not be NULL).
 * @return Pointer to the projection matrix (read-only).
 */
const Mat4 *vkr_layer_context_get_projection(const VkrLayerContext *ctx);

/**
 * @brief Update the layer's camera matrices.
 *
 * Sets both the view and projection matrices for the layer. This affects
 * how the layer's content is rendered.
 *
 * @param ctx The layer context (must not be NULL).
 * @param view The new view matrix (must not be NULL).
 * @param projection The new projection matrix (must not be NULL).
 */
void vkr_layer_context_set_camera(VkrLayerContext *ctx, const Mat4 *view,
                                  const Mat4 *projection);

/**
 * @brief Get the user data pointer associated with the layer.
 *
 * User data is an opaque pointer set during layer creation that can be used
 * to store layer-specific state.
 *
 * @param ctx The layer context (must not be NULL).
 * @return The user data pointer (may be NULL).
 */
void *vkr_layer_context_get_user_data(const VkrLayerContext *ctx);

/**
 * @brief Get the renderpass handle for the current pass.
 *
 * Returns the renderpass handle from the current pass. If ctx->pass is NULL,
 * returns an invalid handle.
 *
 * @param ctx The layer context (must not be NULL).
 * @return The renderpass handle.
 */
VkrRenderPassHandle
vkr_layer_context_get_renderpass(const VkrLayerContext *ctx);

/**
 * @brief Get a render target handle for a specific swapchain image index.
 *
 * Returns the render target handle for the given image index. This is used
 * for triple-buffered rendering where each swapchain image has its own render
 * target.
 *
 * @param ctx The layer context (must not be NULL).
 * @param image_index The swapchain image index (0, 1, or 2 for triple
 * buffering).
 * @return The render target handle for the specified image index.
 */
VkrRenderTargetHandle
vkr_layer_context_get_render_target(const VkrLayerContext *ctx,
                                    uint32_t image_index);

/**
 * @brief Get the number of render targets in the current pass.
 *
 * Returns the number of render targets configured for the current pass.
 * If ctx->pass is NULL, returns 0.
 *
 * @param ctx The layer context (must not be NULL).
 * @return The number of render targets.
 */
uint32_t vkr_layer_context_get_render_target_count(const VkrLayerContext *ctx);

/**
 * @brief Get the index of the current pass within the layer.
 *
 * Returns the index of the pass in the layer's passes array. This is useful
 * for distinguishing between multiple passes in a layer.
 *
 * @param ctx The layer context (must not be NULL).
 * @return The pass index, or UINT32_MAX if ctx->pass is NULL.
 */
uint32_t vkr_layer_context_get_pass_index(const VkrLayerContext *ctx);

/**
 * @brief Get the layer handle from the context.
 * @param ctx The layer context (must not be NULL).
 * @return The layer handle.
 */
VkrLayerHandle vkr_layer_context_get_handle(const VkrLayerContext *ctx);

/**
 * @brief Get the layer flags.
 *
 * Returns the flags set on the layer (e.g., VKR_LAYER_FLAG_ALWAYS_UPDATE).
 *
 * @param ctx The layer context (must not be NULL).
 * @return The layer flags.
 */
uint32_t vkr_layer_context_get_flags(const VkrLayerContext *ctx);

/**
 * @brief Check if the layer currently has modal focus.
 *
 * Modal focus is used for UI layers to determine which layer should receive
 * input events. Only one layer can have modal focus at a time.
 *
 * @param ctx The layer context (must not be NULL).
 * @return true if this layer has modal focus, false otherwise.
 */
bool8_t vkr_layer_context_has_modal_focus(const VkrLayerContext *ctx);

/**
 * @brief Get a pointer to the camera system.
 *
 * Provides access to the camera system for creating and managing cameras.
 *
 * @param ctx The layer context (must not be NULL).
 * @return Pointer to the camera system.
 */
VkrCameraSystem *
vkr_layer_context_get_camera_system(const VkrLayerContext *ctx);

/**
 * @brief Get the handle of the active camera for this layer.
 *
 * Returns the camera handle that is currently active for the layer's rendering.
 *
 * @param ctx The layer context (must not be NULL).
 * @return The active camera handle.
 */
VkrCameraHandle vkr_layer_context_get_active_camera(const VkrLayerContext *ctx);

/**
 * @brief Update all enabled layers in the view system.
 *
 * Calls the on_update callback for each enabled layer, passing delta_time and
 * input state. Layers are updated in sorted order. If a layer consumes input,
 * subsequent layers may not receive input events.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 * @param delta_time Time elapsed since the last update in seconds.
 */
void vkr_view_system_update_all(VkrRendererFrontendHandle renderer,
                                float64_t delta_time);

/**
 * @brief Enable or disable a layer.
 *
 * When a layer is enabled, it receives update and render callbacks. When
 * disabled, it is skipped. The on_enable/on_disable callbacks are invoked when
 * the state changes. Disabling a layer also clears modal focus if it was set on
 * that layer.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 * @param handle The layer handle to modify.
 * @param enabled true to enable the layer, false to disable it.
 */
void vkr_view_system_set_layer_enabled(VkrRendererFrontendHandle renderer,
                                       VkrLayerHandle handle, bool8_t enabled);

/**
 * @brief Check if a layer is currently enabled.
 * @param renderer The renderer frontend handle (must not be NULL).
 * @param handle The layer handle to query.
 * @return true if the layer is enabled, false otherwise.
 */
bool8_t vkr_view_system_is_layer_enabled(VkrRendererFrontendHandle renderer,
                                         VkrLayerHandle handle);

/**
 * @brief Set modal focus to a specific layer.
 *
 * Modal focus determines which layer receives input events. Only one layer
 * can have modal focus at a time. Setting focus to a new layer automatically
 * clears focus from the previous layer.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 * @param handle The layer handle to give modal focus to.
 */
void vkr_view_system_set_modal_focus(VkrRendererFrontendHandle renderer,
                                     VkrLayerHandle handle);

/**
 * @brief Clear modal focus from all layers.
 *
 * After calling this, no layer will have modal focus until set_modal_focus is
 * called.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 */
void vkr_view_system_clear_modal_focus(VkrRendererFrontendHandle renderer);

/**
 * @brief Get the layer handle that currently has modal focus.
 *
 * Returns the handle of the layer with modal focus, or VKR_LAYER_HANDLE_INVALID
 * if no layer has focus.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 * @return The layer handle with modal focus, or VKR_LAYER_HANDLE_INVALID.
 */
VkrLayerHandle
vkr_view_system_get_modal_focus(VkrRendererFrontendHandle renderer);

// Forward declaration for typed message header
typedef struct VkrLayerMsgHeader VkrLayerMsgHeader;

/**
 * @brief Send a typed message to a layer with optional response.
 *
 * Type-safe messaging API for inter-layer communication. Validates message
 * kind, version, and payload size in debug builds.
 *
 * @param renderer The renderer frontend handle.
 * @param target The target layer handle.
 * @param msg Pointer to message (header + payload).
 * @param out_rsp Buffer for typed response (NULL if none expected).
 * @param out_rsp_capacity Size of out_rsp buffer in bytes.
 * @param out_rsp_size Actual response size written (optional).
 * @return true on success, false on failure.
 */
bool32_t vkr_view_system_send_msg(VkrRendererFrontendHandle renderer,
                                  VkrLayerHandle target,
                                  const VkrLayerMsgHeader *msg, void *out_rsp,
                                  uint64_t out_rsp_capacity,
                                  uint64_t *out_rsp_size);

/**
 * @brief Send a typed message without expecting a response.
 *
 * Convenience wrapper for fire-and-forget messages.
 *
 * @param renderer The renderer frontend handle.
 * @param target The target layer handle.
 * @param msg Pointer to message (header + payload).
 * @return true on success, false on failure.
 */
bool32_t vkr_view_system_send_msg_no_rsp(VkrRendererFrontendHandle renderer,
                                         VkrLayerHandle target,
                                         const VkrLayerMsgHeader *msg);

/**
 * @brief Broadcast a typed message to all layers matching flags.
 *
 * @param renderer The renderer frontend handle.
 * @param msg Pointer to message (header + payload).
 * @param flags_filter Only layers with matching flags receive the message.
 */
void vkr_view_system_broadcast_msg(VkrRendererFrontendHandle renderer,
                                   const VkrLayerMsgHeader *msg,
                                   uint32_t flags_filter);

/**
 * @brief Attach a behavior to a layer.
 *
 * Behaviors are reusable components that extend layer functionality. They
 * receive the same lifecycle callbacks as layers (on_attach, on_update,
 * on_render, etc.). Multiple behaviors can be attached to a single layer.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 * @param layer_handle The layer to attach the behavior to.
 * @param behavior Pointer to the behavior definition (must not be NULL).
 * @param out_error Pointer to receive error code on failure (must not be NULL).
 * @return The behavior handle on success, VKR_LAYER_BEHAVIOR_HANDLE_INVALID on
 * failure.
 */
VkrLayerBehaviorHandle vkr_view_system_attach_behavior(
    VkrRendererFrontendHandle renderer, VkrLayerHandle layer_handle,
    const VkrLayerBehavior *behavior, VkrRendererError *out_error);

/**
 * @brief Detach a behavior from a layer.
 *
 * Calls the behavior's on_detach callback and removes it from the layer.
 * The behavior handle becomes invalid after this call.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 * @param layer_handle The layer to detach the behavior from.
 * @param behavior_handle The behavior handle to detach.
 */
void vkr_view_system_detach_behavior(VkrRendererFrontendHandle renderer,
                                     VkrLayerHandle layer_handle,
                                     VkrLayerBehaviorHandle behavior_handle);

/**
 * @brief Get the behavior data pointer for a behavior instance.
 *
 * Returns the behavior_data pointer that was set when the behavior was
 * attached. This allows accessing behavior-specific state.
 *
 * @param renderer The renderer frontend handle (must not be NULL).
 * @param layer_handle The layer the behavior is attached to.
 * @param behavior_handle The behavior handle.
 * @return Pointer to the behavior data, or NULL if the behavior is not found.
 */
void *vkr_view_system_get_behavior_data(VkrRendererFrontendHandle renderer,
                                        VkrLayerHandle layer_handle,
                                        VkrLayerBehaviorHandle behavior_handle);

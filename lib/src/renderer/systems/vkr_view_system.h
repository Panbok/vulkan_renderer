#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "math/mat.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_renderer.h"

#define VKR_VIEW_SYSTEM_MAX_LAYERS 16
#define VKR_VIEW_SYSTEM_MAX_LAYER_PASSES 4

/**
 * @brief A layer pass is a set of render targets and a renderpass.
 * @note This is a convenience struct that is used to group render targets and a
 * renderpass together.
 * @param renderpass_name The name of the renderpass to use.
 * @param use_swapchain_color Whether to use the swapchain color attachment.
 * @param use_depth Whether to use the depth attachment.
 * @param renderpass The renderpass handle.
 * @param render_targets The render targets.
 * @param render_target_count The number of render targets.
 */
typedef struct VkrLayerPass {
  String8 renderpass_name;
  bool8_t use_swapchain_color;
  bool8_t use_depth;

  VkrRenderPassHandle renderpass;
  VkrRenderTargetHandle *render_targets;
  uint32_t render_target_count;
} VkrLayerPass;
Array(VkrLayerPass);

/**
 * @brief A layer is a collection of passes.
 * @note This is a convenience struct that is used to group passes together.
 * @param handle The handle of the layer.
 * @param callbacks The callbacks for the layer.
 * @param passes The passes.
 * @param name The name of the layer.
 * @param order The order of the layer.
 * @param width The width of the layer.
 * @param height The height of the layer.
 * @param view The view matrix of the layer.
 * @param projection The projection matrix of the layer.
 * @param pass_count The number of passes in the layer.
 * @param active Whether the layer is active.
 * @param sync_to_window Whether the layer should sync to the window size.
 * @param user_data The user data for the layer.
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
} VkrLayer;
Array(VkrLayer);

/**
 * @brief A view system is a collection of layers.
 * @note This is a convenience struct that is used to group layers together.
 * @param arena The arena for the view system.
 * @param allocator The allocator for the view system.
 * @param renderer The renderer.
 * @param layers The layers.
 * @param layer_capacity The capacity of the layers.
 * @param render_target_count The number of render targets.
 * @param sorted_indices The sorted indices of the layers.
 * @param sorted_count The number of sorted layers.
 * @param next_id The next ID for the layers.
 * @param window_width The width of the window.
 * @param window_height The height of the window.
 * @param order_dirty Whether the order of the layers is dirty.
 * @param initialized Whether the view system is initialized.
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
} VkrViewSystem;

/**
 * @brief A layer context is a context for a layer.
 * @note This is a convenience struct that is used to group a layer and a pass
 * together.
 * @param view_system The view system.
 * @param layer The layer.
 * @param pass The pass.
 */
typedef struct VkrLayerContext {
  VkrViewSystem *view_system;
  VkrLayer *layer;
  VkrLayerPass *pass;
} VkrLayerContext;

/**
 * @brief Get the renderer from a layer context.
 * @param ctx The layer context.
 * @return The renderer.
 */
VkrRendererFrontendHandle vkr_layer_context_get_renderer(VkrLayerContext *ctx);

/**
 * @brief Get the width of a layer context.
 * @param ctx The layer context.
 * @return The width.
 */
uint32_t vkr_layer_context_get_width(const VkrLayerContext *ctx);

/**
 * @brief Get the height of a layer context.
 * @param ctx The layer context.
 * @return The height.
 */
uint32_t vkr_layer_context_get_height(const VkrLayerContext *ctx);

/**
 * @brief Get the view matrix from a layer context.
 * @param ctx The layer context.
 * @return The view matrix.
 */
const Mat4 *vkr_layer_context_get_view(const VkrLayerContext *ctx);

/**
 * @brief Get the projection matrix from a layer context.
 * @param ctx The layer context.
 * @return The projection matrix.
 */
const Mat4 *vkr_layer_context_get_projection(const VkrLayerContext *ctx);

/**
 * @brief Set the camera for a layer context.
 * @param ctx The layer context.
 * @param view The view matrix.
 * @param projection The projection matrix.
 */
void vkr_layer_context_set_camera(VkrLayerContext *ctx, const Mat4 *view,
                                  const Mat4 *projection);

/**
 * @brief Get the user data from a layer context.
 * @param ctx The layer context.
 * @return The user data.
 */
void *vkr_layer_context_get_user_data(const VkrLayerContext *ctx);

/**
 * @brief Get the renderpass from a layer context.
 * @param ctx The layer context.
 * @return The renderpass.
 */
VkrRenderPassHandle
vkr_layer_context_get_renderpass(const VkrLayerContext *ctx);

/**
 * @brief Get the render target from a layer context.
 * @param ctx The layer context.
 * @param image_index The image index.
 * @return The render target.
 */
VkrRenderTargetHandle
vkr_layer_context_get_render_target(const VkrLayerContext *ctx,
                                    uint32_t image_index);

/**
 * @brief Get the number of render targets from a layer context.
 * @param ctx The layer context.
 * @return The number of render targets.
 */
uint32_t vkr_layer_context_get_render_target_count(const VkrLayerContext *ctx);

/**
 * @brief Get the pass index from a layer context.
 * @param ctx The layer context.
 * @return The pass index.
 */
uint32_t vkr_layer_context_get_pass_index(const VkrLayerContext *ctx);

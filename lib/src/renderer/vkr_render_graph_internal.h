#pragma once

#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_render_graph.h"

// =============================================================================
// Internal Graph Structures
// =============================================================================

/**
 * @brief Internal image resource state; one per declared/imported image in the
 * graph.
 */
typedef struct VkrRgImage {
  String8 name;        /**< Declared name (stable) */
  String8 import_name; /**< Name used when imported (e.g. swapchain/depth) */
  VkrRgImageDesc desc; /**< Image description */
  uint32_t generation; /**< Handle generation; bumped on recompile */
  bool8_t declared_this_frame; /**< True if declared in current frame build */
  bool8_t exported;            /**< True if marked for export */

  bool8_t imported; /**< True if external (swapchain/depth/import_image) */
  VkrTextureOpaqueHandle
      imported_handle; /**< Backend handle when imported (single) */
  VkrRgImageAccessFlags imported_access; /**< Access at import for barriers */
  VkrTextureLayout imported_layout;      /**< Layout at import */
  VkrTextureLayout final_layout;         /**< Final layout of subresource 0 */

  uint32_t first_pass; /**< First pass that uses this image */
  uint32_t last_pass;  /**< Last pass that uses this image */
} VkrRgImage;

Vector(VkrRgImage);

/**
 * @brief Internal buffer resource state; one per declared/imported buffer in
 * the graph.
 */
typedef struct VkrRgBuffer {
  String8 name;                /**< Declared name (stable) */
  VkrRgBufferDesc desc;        /**< Buffer description */
  uint32_t generation;         /**< Handle generation; bumped on recompile */
  bool8_t declared_this_frame; /**< True if declared in current frame build */
  bool8_t exported;            /**< True if marked for export */

  bool8_t imported;                /**< True if external (import_buffer) */
  VkrBufferHandle imported_handle; /**< Backend handle when imported (single) */
  VkrRgBufferAccessFlags imported_access; /**< Access at import for barriers */

  uint32_t first_pass; /**< First pass that uses this buffer */
  uint32_t last_pass;  /**< Last pass that uses this buffer */
} VkrRgBuffer;

Vector(VkrRgBuffer);

/**
 * @brief Image layout/access transition inserted before or after a pass.
 */
typedef struct VkrRgImageBarrier {
  VkrRgImageHandle image;           /**< Image to transition */
  VkrRgImageAccessFlags src_access; /**< Source access mask */
  VkrRgImageAccessFlags dst_access; /**< Destination access mask */
  VkrTextureLayout src_layout;      /**< Source layout */
  VkrTextureLayout dst_layout;      /**< Destination layout */
  VkrGpuDependency dependency;      /**< Canonical execution/visibility */
  /** Subresources this barrier covers; zeroed means the whole image. */
  VkrImageSubresourceRange range;
} VkrRgImageBarrier;

Vector(VkrRgImageBarrier);

/**
 * @brief Access and layout of one image subresource during barrier generation.
 */
typedef struct VkrRgSubresourceState {
  VkrRgImageAccessFlags access;
  VkrGpuStageFlags stages;
  VkrTextureLayout layout;
  VkrRgImageAccessFlags pending_access;
  VkrGpuStageFlags pending_stages;
  VkrTextureLayout pending_layout;
  uint32_t pending_token;
} VkrRgSubresourceState;

/**
 * @brief Access of one buffer during barrier generation.
 */
typedef struct VkrRgBufferState {
  VkrRgBufferAccessFlags access;
  VkrGpuStageFlags stages;
  VkrRgBufferAccessFlags pending_access;
  VkrGpuStageFlags pending_stages;
  uint32_t pending_token;
} VkrRgBufferState;

/**
 * @brief Buffer access transition inserted before or after a pass.
 */
typedef struct VkrRgBufferBarrier {
  VkrRgBufferHandle buffer;          /**< Buffer to transition */
  VkrRgBufferAccessFlags src_access; /**< Source access mask */
  VkrRgBufferAccessFlags dst_access; /**< Destination access mask */
  VkrGpuDependency dependency;       /**< Canonical execution/visibility */
} VkrRgBufferBarrier;

Vector(VkrRgBufferBarrier);

/**
 * @brief Internal pass state; one per pass added to the graph.
 */
typedef struct VkrRgPass {
  VkrRgPassDesc desc; /**< Pass descriptor (name, attachments, uses, execute) */

  Vector_uint32_t out_edges; /**< Indices of passes that depend on this pass */
  Vector_uint32_t in_edges;  /**< Indices of passes this pass depends on */

  Vector_VkrRgImageBarrier
      pre_image_barriers; /**< Image barriers to record before the pass */
  Vector_VkrRgBufferBarrier
      pre_buffer_barriers; /**< Buffer barriers to record before the pass */

  bool8_t culled; /**< True if pass was culled (outputs unused) */
} VkrRgPass;

Vector(VkrRgPass);

/**
 * @brief Render graph state: resources, passes, barriers, and execution order.
 * packet is frame-local and set via vkr_rg_set_packet; must remain valid during
 * execute.
 */
typedef struct VkrRenderGraph {
  VkrAllocator *allocator;            /**< Allocator for graph-owned data */
  VkrAllocator *frame_allocator;      /**< Allocator for frame-owned passes */
  VkrAllocatorScope frame_scope;      /**< Active frame-allocation scope */
  bool8_t frame_scope_active;         /**< True while frame_scope is live */
  VkrRenderGraphFrameInfo frame_info; /**< Frame info from last begin_frame */
  const VkrRenderPacket *packet; /**< Frame-local; set via vkr_rg_set_packet;
                                    valid during execute */

  Vector_VkrRgImage images;   /**< All image resources */
  Vector_VkrRgBuffer buffers; /**< All buffer resources */
  Vector_VkrRgPass passes;    /**< All passes */

  VkrRgImageHandle present_image; /**< Image used for present (swapchain) */
  Vector_VkrRgImageBarrier
      terminal_image_barriers; /**< Graph-owned target completion barriers */
  Vector_VkrRgImageHandle export_images;   /**< Images marked for export */
  Vector_VkrRgBufferHandle export_buffers; /**< Buffers marked for export */

  Vector_uint32_t
      execution_order; /**< Pass indices in execution order (after compile) */

  /**
   * Per-subresource state used by barrier generation. Image i owns
   * mip_levels * array_layers consecutive slots starting at
   * image_state_offsets[i]; buffers own one slot each.
   *
   * Grown on demand and reused across frames. Barrier generation runs every
   * frame and must not allocate in steady state. Freed by vkr_rg_destroy.
   */
  VkrRgSubresourceState *subresource_states;
  uint32_t subresource_state_capacity;
  uint32_t *image_state_offsets;
  uint32_t image_state_offset_capacity;
  uint32_t *image_touch_tokens;
  uint32_t *touched_image_indices;
  uint32_t touched_image_capacity;
  VkrRgBufferState *buffer_states;
  uint32_t buffer_state_capacity;
  uint32_t *touched_buffer_indices;
  uint32_t touched_buffer_capacity;

  /**
   * Retained cross-frame state (ADR-029). The provider is backend-owned; the
   * graph only caches this frame's seed validity so the read-before-write check
   * can consult it, and remembers which instance each retained image compiled
   * against so the commit writes back to the same one.
   *
   * `retained_content_valid` parallels `subresource_states` slot for slot.
   */
  VkrRgRetainedStateProvider retained_provider;
  bool8_t *retained_content_valid;
  uint32_t retained_content_valid_capacity;
  uint32_t *retained_instance_indices;
  uint32_t retained_instance_index_capacity;
} VkrRenderGraph;

/** Mip levels times array layers; the stride of `subresource_states`. */
uint32_t vkr_rg_image_subresource_count(const VkrRgImage *image);

/**
 * @brief Runs the renderer-independent half of compile: validation, dependency
 * edges, culling, topological ordering, lifetimes, and barrier planning.
 *
 * This is the complete shared graph compilation stage. Resource realization
 * and command encoding belong to the selected renderer implementation.
 *
 * @return true when the graph scheduled and its barriers were planned.
 */
VKR_MUST_USE bool8_t vkr_rg_compile_schedule(VkrRenderGraph *graph);

/**
 * @brief Resolves an image handle to the internal image state.
 * @param graph Render graph
 * @param handle Image handle
 * @return Pointer to internal VkrRgImage, or NULL if invalid/stale
 */
VkrRgImage *vkr_rg_image_from_handle(VkrRenderGraph *graph,
                                     VkrRgImageHandle handle);

/**
 * @brief Resolves a buffer handle to the internal buffer state.
 * @param graph Render graph
 * @param handle Buffer handle
 * @return Pointer to internal VkrRgBuffer, or NULL if invalid/stale
 */
VkrRgBuffer *vkr_rg_buffer_from_handle(VkrRenderGraph *graph,
                                       VkrRgBufferHandle handle);

/**
 * @brief Clears all passes and execution order; resources and frame state are
 * unchanged.
 * @param graph Render graph
 */
void vkr_rg_reset_passes(VkrRenderGraph *graph);

/**
 * @brief Clears present_image and export_images/export_buffers lists.
 * @param graph Render graph
 */
void vkr_rg_reset_exports(VkrRenderGraph *graph);

/**
 * @brief Marks the graph as not compiled; does not free resources or passes.
 * @param graph Render graph
 */
void vkr_rg_clear_compiled(VkrRenderGraph *graph);

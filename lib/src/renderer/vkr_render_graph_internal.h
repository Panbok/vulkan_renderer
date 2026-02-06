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
  uint32_t
      allocated_generation; /**< Generation when textures were last allocated */
  uint64_t allocated_bytes_per_texture; /**< Bytes per texture for stats */
  bool8_t declared_this_frame; /**< True if declared in current frame build */
  bool8_t exported;            /**< True if marked for export */

  bool8_t imported; /**< True if external (swapchain/depth/import_image) */
  VkrTextureOpaqueHandle
      imported_handle; /**< Backend handle when imported (single) */
  VkrRgImageAccessFlags imported_access; /**< Access at import for barriers */
  VkrTextureLayout imported_layout;      /**< Layout at import */
  VkrTextureLayout final_layout; /**< Layout after last use (for export) */
  VkrTextureOpaqueHandle
      *textures; /**< Allocated textures (one or per swapchain image) */
  uint32_t texture_count; /**< Number of textures */
  VkrTextureHandle
      *texture_handles;          /**< Public handles for backend (if used) */
  uint32_t texture_handle_count; /**< Number of texture handles */

  uint32_t first_pass; /**< First pass that uses this image */
  uint32_t last_pass;  /**< Last pass that uses this image */
} VkrRgImage;

Vector(VkrRgImage);

/**
 * @brief Internal buffer resource state; one per declared/imported buffer in
 * the graph.
 */
typedef struct VkrRgBuffer {
  String8 name;         /**< Declared name (stable) */
  VkrRgBufferDesc desc; /**< Buffer description */
  uint32_t generation;  /**< Handle generation; bumped on recompile */
  uint32_t
      allocated_generation; /**< Generation when buffers were last allocated */
  uint64_t allocated_size;  /**< Allocated size per buffer for stats */
  bool8_t declared_this_frame; /**< True if declared in current frame build */
  bool8_t exported;            /**< True if marked for export */

  bool8_t imported;                /**< True if external (import_buffer) */
  VkrBufferHandle imported_handle; /**< Backend handle when imported (single) */
  VkrRgBufferAccessFlags imported_access; /**< Access at import for barriers */
  VkrBufferHandle
      *buffers;          /**< Allocated buffers (one or per swapchain image) */
  uint32_t buffer_count; /**< Number of buffers */

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
} VkrRgImageBarrier;

Vector(VkrRgImageBarrier);

/**
 * @brief Buffer access transition inserted before or after a pass.
 */
typedef struct VkrRgBufferBarrier {
  VkrRgBufferHandle buffer;          /**< Buffer to transition */
  VkrRgBufferAccessFlags src_access; /**< Source access mask */
  VkrRgBufferAccessFlags dst_access; /**< Destination access mask */
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

  VkrRenderPassHandle renderpass; /**< Backend render pass (after compile) */
  VkrRenderTargetHandle
      *render_targets;          /**< Backend render targets (color/depth) */
  uint32_t render_target_count; /**< Number of render targets */
} VkrRgPass;

Vector(VkrRgPass);

/**
 * @brief Cache entry for render pass + framebuffer; keyed by pass name and
 * attachment hash.
 */
typedef struct VkrRgRenderTargetCacheEntry {
  String8 pass_name;              /**< Pass name (for lookup) */
  uint64_t renderpass_hash;       /**< Hash of render pass config */
  VkrRenderPassHandle renderpass; /**< Cached render pass handle */
  uint64_t target_hash;           /**< Hash of attachment set */
  VkrRenderTargetHandle
      *targets;          /**< Cached render targets (per image index) */
  uint32_t target_count; /**< Number of targets */
} VkrRgRenderTargetCacheEntry;

Vector(VkrRgRenderTargetCacheEntry);

/**
 * @brief Render graph state: resources, passes, barriers, and execution order.
 * packet is frame-local and set via vkr_rg_set_packet; must remain valid during
 * execute.
 */
typedef struct VkrRenderGraph {
  VkrAllocator *allocator;            /**< Allocator for graph-owned data */
  VkrRenderGraphFrameInfo frame_info; /**< Frame info from last begin_frame */
  struct s_RendererFrontend
      *renderer;                 /**< Renderer frontend (set at execute) */
  const VkrRenderPacket *packet; /**< Frame-local; set via vkr_rg_set_packet;
                                    valid during execute */

  Vector_VkrRgImage images;   /**< All image resources */
  Vector_VkrRgBuffer buffers; /**< All buffer resources */
  Vector_VkrRgPass passes;    /**< All passes */

  Vector_uint64_t renderpass_hashes; /**< Per-pass render pass config hashes */
  Vector_VkrRgRenderTargetCacheEntry
      render_target_cache; /**< Cached render passes + framebuffers */

  VkrRgImageHandle present_image; /**< Image used for present (swapchain) */
  Vector_VkrRgImageHandle export_images;   /**< Images marked for export */
  Vector_VkrRgBufferHandle export_buffers; /**< Buffers marked for export */

  Vector_uint32_t
      execution_order; /**< Pass indices in execution order (after compile) */
  bool8_t compiled;    /**< True after successful vkr_rg_compile */
  VkrRenderGraphResourceStats
      resource_stats; /**< Live/peak resource counts and bytes */
  Vector_VkrRgPassTiming pass_timings; /**< Per-pass timing from last execute */
} VkrRenderGraph;

/**
 * @brief Adds image count and bytes to the graph's resource stats (live and
 * peak).
 * @param graph Render graph
 * @param count Number of image textures to add
 * @param bytes Total bytes to add
 */
vkr_internal inline void
vkr_rg_stats_add_images(VkrRenderGraph *graph, uint32_t count, uint64_t bytes) {
  if (!graph || (count == 0 && bytes == 0)) {
    return;
  }
  if (count > 0) {
    uint32_t live = graph->resource_stats.live_image_textures + count;
    graph->resource_stats.live_image_textures = live;
    if (live > graph->resource_stats.peak_image_textures) {
      graph->resource_stats.peak_image_textures = live;
    }
  }
  if (bytes > 0) {
    uint64_t live_bytes = graph->resource_stats.live_image_bytes + bytes;
    graph->resource_stats.live_image_bytes = live_bytes;
    if (live_bytes > graph->resource_stats.peak_image_bytes) {
      graph->resource_stats.peak_image_bytes = live_bytes;
    }
  }
}

/**
 * @brief Subtracts image count and bytes from the graph's resource stats.
 * @param graph Render graph
 * @param count Number of image textures to remove
 * @param bytes Total bytes to remove
 */
vkr_internal inline void vkr_rg_stats_remove_images(VkrRenderGraph *graph,
                                                    uint32_t count,
                                                    uint64_t bytes) {
  if (!graph || (count == 0 && bytes == 0)) {
    return;
  }
  if (count > 0) {
    if (count >= graph->resource_stats.live_image_textures) {
      graph->resource_stats.live_image_textures = 0;
    } else {
      graph->resource_stats.live_image_textures -= count;
    }
  }
  if (bytes > 0) {
    if (bytes >= graph->resource_stats.live_image_bytes) {
      graph->resource_stats.live_image_bytes = 0;
    } else {
      graph->resource_stats.live_image_bytes -= bytes;
    }
  }
}

/**
 * @brief Adds buffer count and bytes to the graph's resource stats (live and
 * peak).
 * @param graph Render graph
 * @param count Number of buffers to add
 * @param bytes Total bytes to add
 */
vkr_internal inline void vkr_rg_stats_add_buffers(VkrRenderGraph *graph,
                                                  uint32_t count,
                                                  uint64_t bytes) {
  if (!graph || (count == 0 && bytes == 0)) {
    return;
  }

  if (count > 0) {
    uint32_t live = graph->resource_stats.live_buffers + count;
    graph->resource_stats.live_buffers = live;
    if (live > graph->resource_stats.peak_buffers) {
      graph->resource_stats.peak_buffers = live;
    }
  }

  if (bytes > 0) {
    uint64_t live_bytes = graph->resource_stats.live_buffer_bytes + bytes;
    graph->resource_stats.live_buffer_bytes = live_bytes;
    if (live_bytes > graph->resource_stats.peak_buffer_bytes) {
      graph->resource_stats.peak_buffer_bytes = live_bytes;
    }
  }
}

/**
 * @brief Subtracts buffer count and bytes from the graph's resource stats.
 * @param graph Render graph
 * @param count Number of buffers to remove
 * @param bytes Total bytes to remove
 */
vkr_internal inline void vkr_rg_stats_remove_buffers(VkrRenderGraph *graph,
                                                     uint32_t count,
                                                     uint64_t bytes) {
  if (!graph || (count == 0 && bytes == 0)) {
    return;
  }
  if (count >= graph->resource_stats.live_buffers) {
    graph->resource_stats.live_buffers = 0;
  } else {
    graph->resource_stats.live_buffers -= count;
  }
  if (bytes >= graph->resource_stats.live_buffer_bytes) {
    graph->resource_stats.live_buffer_bytes = 0;
  } else {
    graph->resource_stats.live_buffer_bytes -= bytes;
  }
}

/**
 * @brief Releases all allocated textures for an image and updates resource
 * stats. No-op for imported images (only frees the textures array). Frees
 * texture_handles if present.
 * @param graph Render graph
 * @param image Image to release
 */
vkr_internal inline void vkr_rg_release_image_textures(VkrRenderGraph *graph,
                                                       VkrRgImage *image) {
  if (!graph || !image || !image->textures) {
    return;
  }

  uint32_t released = 0;
  uint64_t bytes_per_texture = image->allocated_bytes_per_texture;
  if (!image->imported) {
    for (uint32_t i = 0; i < image->texture_count; ++i) {
      if (image->textures[i]) {
        if (graph->renderer) {
          vkr_renderer_destroy_texture(graph->renderer, image->textures[i]);
        }
        released += 1;
      }
    }
    vkr_rg_stats_remove_images(graph, released,
                               (uint64_t)released * bytes_per_texture);
  }

  vkr_allocator_free(graph->allocator, image->textures,
                     sizeof(VkrTextureOpaqueHandle) *
                         (uint64_t)image->texture_count,
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  image->textures = NULL;
  image->texture_count = 0;
  image->allocated_generation = 0;
  image->allocated_bytes_per_texture = 0;

  if (image->texture_handles) {
    vkr_allocator_free(graph->allocator, image->texture_handles,
                       sizeof(VkrTextureHandle) *
                           (uint64_t)image->texture_handle_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    image->texture_handles = NULL;
    image->texture_handle_count = 0;
  }
}

/**
 * @brief Releases all allocated buffers for a buffer resource and updates
 * resource stats. No-op for imported buffers (only frees the buffers array).
 * @param graph Render graph
 * @param buffer Buffer to release
 */
vkr_internal inline void vkr_rg_release_buffer_handles(VkrRenderGraph *graph,
                                                       VkrRgBuffer *buffer) {
  if (!graph || !buffer || !buffer->buffers) {
    return;
  }

  uint32_t released = 0;
  if (!buffer->imported) {
    for (uint32_t i = 0; i < buffer->buffer_count; ++i) {
      if (buffer->buffers[i]) {
        if (graph->renderer) {
          vkr_renderer_destroy_buffer(graph->renderer, buffer->buffers[i]);
        }
        released += 1;
      }
    }
    uint64_t bytes_per_buffer =
        buffer->allocated_size > 0 ? buffer->allocated_size : buffer->desc.size;
    vkr_rg_stats_remove_buffers(graph, released,
                                (uint64_t)released * bytes_per_buffer);
  }

  vkr_allocator_free(graph->allocator, buffer->buffers,
                     sizeof(VkrBufferHandle) * (uint64_t)buffer->buffer_count,
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  buffer->buffers = NULL;
  buffer->buffer_count = 0;
  buffer->allocated_generation = 0;
  buffer->allocated_size = 0;
}

/**
 * @brief Returns the backend texture for an image at the given swapchain image
 * index. For single-texture or imported images, returns that texture; otherwise
 * indexes into textures array.
 * @param image Internal image state
 * @param image_index Swapchain image index
 * @return Backend texture handle, or imported_handle when no allocations
 */
vkr_internal inline VkrTextureOpaqueHandle
vkr_rg_pick_image_texture(const VkrRgImage *image, uint32_t image_index) {
  if (!image) {
    return NULL;
  }

  if (!image->textures || image->texture_count == 0) {
    return image->imported_handle;
  }

  if (image->texture_count == 1) {
    return image->textures[0];
  }

  if (image_index < image->texture_count) {
    return image->textures[image_index];
  }

  return image->textures[0];
}

/**
 * @brief Returns the backend buffer for a buffer resource at the given
 * swapchain image index. For single-buffer or imported buffers, returns that
 * buffer; otherwise indexes into buffers array.
 * @param buffer Internal buffer state
 * @param image_index Swapchain image index
 * @return Backend buffer handle, or imported_handle when no allocations
 */
vkr_internal inline VkrBufferHandle
vkr_rg_pick_buffer_handle(const VkrRgBuffer *buffer, uint32_t image_index) {
  if (!buffer) {
    return NULL;
  }

  if (!buffer->buffers || buffer->buffer_count == 0) {
    return buffer->imported_handle;
  }

  if (buffer->buffer_count == 1) {
    return buffer->buffers[0];
  }

  if (image_index < buffer->buffer_count) {
    return buffer->buffers[image_index];
  }

  return buffer->buffers[0];
}

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

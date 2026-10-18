/**
 * @file vkr_rg_debug.h
 * @brief Debug helpers for the render graph.
 */
#pragma once

#include "defines.h"
#include "vkr_render_graph.h"

typedef enum VkrRgDotExportFlags {
  VKR_RG_DOT_EXPORT_NONE = 0,
  VKR_RG_DOT_EXPORT_RESOURCES = 1 << 0,
  VKR_RG_DOT_EXPORT_PASS_EDGES = 1 << 1,
} VkrRgDotExportFlags;

#define VKR_RG_DOT_EXPORT_DEFAULT                                              \
  (VKR_RG_DOT_EXPORT_RESOURCES | VKR_RG_DOT_EXPORT_PASS_EDGES)

typedef struct VkrRgDotExportDesc {
  const char *path;
  VkrRgDotExportFlags flags;
} VkrRgDotExportDesc;

/**
 * @brief Export the render graph to a Graphviz DOT file.
 *
 * @param graph Render graph to export.
 * @param path Output path (relative to working directory unless absolute).
 * @return true on success, false on failure.
 */
bool8_t vkr_rg_export_dot(const VkrRenderGraph *graph, const char *path);

/**
 * @brief Export the render graph to DOT with configurable flags.
 *
 * @param graph Render graph to export.
 * @param desc Export options (path + flags).
 * @return true on success, false on failure.
 */
bool8_t vkr_rg_export_dot_ex(const VkrRenderGraph *graph,
                             const VkrRgDotExportDesc *desc);

// =============================================================================
// Compiled graph description
// =============================================================================

/** Uses one described pass reports; a pass with more marks itself truncated.
 */
#define VKR_RG_DESCRIBED_USE_MAX 64u

/** One resource a pass reads or writes. `role` is "color", "depth",
 * "depth_read", "read" or "write"; attachments other than a read-only depth
 * count as writes. */
typedef struct VkrRgDescribedUse {
  String8 resource;
  const char *role;
  bool8_t buffer;
  bool8_t write;
} VkrRgDescribedUse;

/**
 * One pass of the last built graph. Scheduled passes come first in execution
 * order, then culled or disabled passes in declaration order with `order`
 * UINT32_MAX. Names borrow graph storage valid only during the visit.
 */
typedef struct VkrRgDescribedPass {
  String8 name;
  uint32_t index; /**< Declaration index in this frame's build */
  uint32_t order; /**< Execution position; UINT32_MAX when not scheduled */
  VkrRgPassType type;
  const char *type_name; /**< "graphics", "compute" or "transfer" */
  bool8_t culled;        /**< Not scheduled: outputs unused, or disabled */
  bool8_t disabled;      /**< VKR_RG_PASS_FLAG_DISABLED */
  const VkrRgDescribedUse *uses;
  uint32_t use_count;
  uint32_t use_total; /**< Uses the pass has; above use_count when truncated */
} VkrRgDescribedPass;

/**
 * One image or buffer the graph knows. Resources are never removed, so a
 * resource of an earlier frame that this frame did not declare reports
 * `declared` false. `bytes` is one instance: the buffer size, or the image
 * over every mip, layer and sample; zero for a block-compressed or unknown
 * format. Backend instance counts are not graph facts.
 */
typedef struct VkrRgDescribedResource {
  String8 name;
  uint32_t index; /**< Image or buffer index */
  bool8_t buffer;
  bool8_t declared; /**< Declared or imported by this frame's build */
  bool8_t imported;
  bool8_t exported;
  bool8_t present;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t layers;
  uint32_t mip_levels;
  uint32_t samples;
  VkrTextureFormat format;
  const char *format_name; /**< Authored token, or NULL */
  uint64_t bytes;
  VkrRgResourceFlags flags;
  /** "imported", "history", "retained", "per_frame_slot", "persistent",
   * "transient" or "graph" (owned without a lifetime hint). */
  const char *lifetime;
  /** "single", "per_image" or "per_frame_slot" backend instances. */
  const char *instances;
  uint32_t first_order; /**< First execution position; UINT32_MAX unused */
  uint32_t last_order;  /**< Last execution position */
} VkrRgDescribedResource;

/** Callbacks of vkr_rg_describe; either may be NULL. */
typedef struct VkrRgDescribeVisitor {
  void *user;
  void (*pass)(void *user, const VkrRgDescribedPass *pass);
  void (*resource)(void *user, const VkrRgDescribedResource *resource);
} VkrRgDescribeVisitor;

/** Totals of the last built graph. */
typedef struct VkrRgDescription {
  uint32_t frame_index;
  uint32_t viewport_width;
  uint32_t viewport_height;
  float32_t render_scale;
  bool8_t scene_rendering;
  /** The last compile scheduled the graph; false after a failed compile. */
  bool8_t compiled;
  uint32_t pass_count;
  uint32_t scheduled_count;
  uint32_t culled_count;
  uint32_t image_count;
  uint32_t buffer_count;
  uint32_t declared_image_count;
  uint32_t declared_buffer_count;
  /** One instance of every declared graph-owned resource; imports excluded.
   */
  uint64_t declared_image_bytes;
  uint64_t declared_buffer_bytes;
} VkrRgDescription;

/**
 * @brief Visits the passes and resources of the graph's last build and
 * fills its totals. The graph must not change during the call: the caller
 * owns it or has joined the thread that does.
 *
 * @param graph Render graph to describe.
 * @param visitor Optional callbacks.
 * @param out_description Optional totals.
 * @return false for a NULL graph.
 */
bool8_t vkr_rg_describe(const VkrRenderGraph *graph,
                        const VkrRgDescribeVisitor *visitor,
                        VkrRgDescription *out_description);

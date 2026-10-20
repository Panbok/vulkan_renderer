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

// =============================================================================
// Graph snapshot
// =============================================================================

/** Capacities of a VkrRgSnapshot; a larger graph is cut and marked
 * truncated. */
#define VKR_RG_SNAPSHOT_PASS_MAX 256u
#define VKR_RG_SNAPSHOT_RESOURCE_MAX 512u
#define VKR_RG_SNAPSHOT_USE_MAX 4096u
#define VKR_RG_SNAPSHOT_NAME_BYTES 32768u
/** Slots of the name table; a power of two above the distinct names a full
 * snapshot holds (pass, resource, format and lifetime names). */
#define VKR_RG_SNAPSHOT_NAME_SLOTS 2048u

/** How a pass uses a resource, from VkrRgDescribedUse.role. */
typedef enum VkrRgSnapshotRole {
  VKR_RG_SNAPSHOT_ROLE_READ = 0,
  VKR_RG_SNAPSHOT_ROLE_WRITE,
  VKR_RG_SNAPSHOT_ROLE_COLOR,
  VKR_RG_SNAPSHOT_ROLE_DEPTH,
  VKR_RG_SNAPSHOT_ROLE_DEPTH_READ,
  VKR_RG_SNAPSHOT_ROLE_COUNT,
} VkrRgSnapshotRole;

/** Text in a VkrRgSnapshot's name table. */
typedef struct VkrRgSnapshotName {
  uint32_t offset;
  uint32_t length;
} VkrRgSnapshotName;

#define VKR_RG_SNAPSHOT_PASS_CULLED (1u << 0)
#define VKR_RG_SNAPSHOT_PASS_DISABLED (1u << 1)
/* The pass has more uses than VKR_RG_DESCRIBED_USE_MAX. */
#define VKR_RG_SNAPSHOT_PASS_USES_CUT (1u << 2)

/** A pass and its uses, uses[first_use, first_use + use_count). */
typedef struct VkrRgSnapshotPass {
  VkrRgSnapshotName name;
  VkrRgPassType type;
  uint32_t flags;
  /** Execution position; UINT32_MAX when not scheduled. */
  uint32_t order;
  uint32_t first_use;
  uint32_t use_count;
} VkrRgSnapshotPass;

typedef struct VkrRgSnapshotUse {
  /** The resource by name; a pass names an image or buffer the graph
   * knows. */
  VkrRgSnapshotName resource;
  VkrRgSnapshotRole role;
  bool8_t buffer;
  bool8_t write;
} VkrRgSnapshotUse;

#define VKR_RG_SNAPSHOT_RESOURCE_BUFFER (1u << 0)
#define VKR_RG_SNAPSHOT_RESOURCE_IMPORTED (1u << 1)
#define VKR_RG_SNAPSHOT_RESOURCE_EXPORTED (1u << 2)
#define VKR_RG_SNAPSHOT_RESOURCE_PRESENT (1u << 3)

/** A resource this frame's build declared (VkrRgDescribedResource). */
typedef struct VkrRgSnapshotResource {
  VkrRgSnapshotName name;
  /** Authored format token, empty for a buffer or an unknown format. */
  VkrRgSnapshotName format;
  VkrRgSnapshotName lifetime;
  uint32_t flags;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t layers;
  uint32_t mip_levels;
  uint32_t samples;
  uint64_t bytes;
  uint32_t first_order;
  uint32_t last_order;
} VkrRgSnapshotResource;

/**
 * The shape of the graph's last build in caller-owned storage of the fixed
 * capacities above, for a recording (docs/proposals/recordings.md): passes in
 * execution order then the unscheduled ones, their uses, the declared
 * resources, and every name they hold. No pointer into the graph survives.
 */
typedef struct VkrRgSnapshot {
  uint64_t frame_index;
  uint32_t viewport_width;
  uint32_t viewport_height;
  float32_t render_scale;
  VkrRgSnapshotPass passes[VKR_RG_SNAPSHOT_PASS_MAX];
  uint32_t pass_count;
  VkrRgSnapshotUse uses[VKR_RG_SNAPSHOT_USE_MAX];
  uint32_t use_count;
  VkrRgSnapshotResource resources[VKR_RG_SNAPSHOT_RESOURCE_MAX];
  uint32_t resource_count;
  char names[VKR_RG_SNAPSHOT_NAME_BYTES];
  uint32_t name_bytes;
  /** Each distinct name once, open-addressed by its hash; a zero length is
   * an empty slot. A use names its resource with the resource's own text,
   * so the many uses of one resource share it. */
  VkrRgSnapshotName name_slots[VKR_RG_SNAPSHOT_NAME_SLOTS];
  /** Something did not fit: passes, uses, resources or names. */
  bool8_t truncated;
} VkrRgSnapshot;

/**
 * @brief Fills `out` with the shape of the graph's last build. Same
 * threading rule as vkr_rg_describe.
 * @return false for a NULL graph or `out`.
 */
bool8_t vkr_rg_snapshot(const VkrRenderGraph *graph, VkrRgSnapshot *out);

#pragma once

#include "containers/str.h"
#include "containers/vector.h"
#include "defines.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_renderer.h"

struct VkrRgPassContext;

/**
 * @brief Pass execution callback invoked once per pass per frame during vkr_rg_execute().
 * @param ctx Pass context; valid only for the duration of the call
 * @param user_data User data passed when the pass was registered or set
 */
typedef void (*VkrRgPassExecuteFn)(struct VkrRgPassContext *ctx,
                                   void *user_data);

// Forward declarations for stateless render packet access.
typedef struct VkrRenderPacket VkrRenderPacket;
typedef struct VkrFrameInfo VkrFrameInfo;
typedef struct VkrFrameGlobals VkrFrameGlobals;
typedef struct VkrWorldPassPayload VkrWorldPassPayload;
typedef struct VkrShadowPassPayload VkrShadowPassPayload;
typedef struct VkrSkyboxPassPayload VkrSkyboxPassPayload;
typedef struct VkrUiPassPayload VkrUiPassPayload;
typedef struct VkrEditorPassPayload VkrEditorPassPayload;
typedef struct VkrPickingPassPayload VkrPickingPassPayload;

// =============================================================================
// Public Handles
// =============================================================================

/**
 * @brief Opaque handle to a render-graph image resource.
 * @param id Resource id; 0 means invalid
 * @param generation Used to detect stale handles after graph recompile
 */
typedef struct VkrRgImageHandle {
  uint32_t id;         /**< Resource id; 0 means invalid */
  uint32_t generation; /**< Handle generation for validation */
} VkrRgImageHandle;

/**
 * @brief Invalid image handle sentinel.
 */
#define VKR_RG_IMAGE_HANDLE_INVALID ((VkrRgImageHandle){0, 0})

/**
 * @brief Returns true if the image handle is valid (id != 0).
 * @param h Image handle
 * @return true if valid
 */
vkr_internal inline bool8_t vkr_rg_image_handle_valid(VkrRgImageHandle h) {
  return h.id != 0;
}

/**
 * @brief Opaque handle to a render-graph buffer resource.
 * @param id Resource id; 0 means invalid
 * @param generation Used to detect stale handles after graph recompile
 */
typedef struct VkrRgBufferHandle {
  uint32_t id;         /**< Resource id; 0 means invalid */
  uint32_t generation; /**< Handle generation for validation */
} VkrRgBufferHandle;

/**
 * @brief Invalid buffer handle sentinel.
 */
#define VKR_RG_BUFFER_HANDLE_INVALID ((VkrRgBufferHandle){0, 0})

/**
 * @brief Returns true if the buffer handle is valid (id != 0).
 * @param h Buffer handle
 * @return true if valid
 */
vkr_internal inline bool8_t vkr_rg_buffer_handle_valid(VkrRgBufferHandle h) {
  return h.id != 0;
}

Vector(VkrRgImageHandle);
Vector(VkrRgBufferHandle);

// =============================================================================
// Resource Descriptions
// =============================================================================

/**
 * @brief Lifetime and layout hints for render-graph resources.
 */
typedef enum VkrRgResourceFlags {
  VKR_RG_RESOURCE_FLAG_NONE = 0,       /**< No special flags */
  VKR_RG_RESOURCE_FLAG_TRANSIENT = 1 << 0,  /**< Freed after each frame */
  VKR_RG_RESOURCE_FLAG_PERSISTENT = 1 << 1, /**< Kept across frames */
  VKR_RG_RESOURCE_FLAG_EXTERNAL = 1 << 2,    /**< Imported, not owned by graph */
  VKR_RG_RESOURCE_FLAG_PER_IMAGE = 1 << 3,  /**< One resource per swapchain image */
  VKR_RG_RESOURCE_FLAG_RESIZABLE = 1 << 4,  /**< May be recreated on resize */
  VKR_RG_RESOURCE_FLAG_FORCE_ARRAY = 1 << 5, /**< Force array view in descriptors */
} VkrRgResourceFlags;

/**
 * @brief Image resource specification for vkr_rg_create_image.
 * width/height 0 allowed for size-from-attachment or swapchain-derived; otherwise must be positive.
 */
typedef struct VkrRgImageDesc {
  uint32_t width;              /**< Image width; 0 if derived from attachment/swapchain */
  uint32_t height;             /**< Image height; 0 if derived */
  VkrTextureFormat format;      /**< Pixel format */
  VkrTextureUsageFlags usage;   /**< Vulkan usage flags */
  VkrSampleCount samples;       /**< Sample count (MSAA) */
  uint32_t layers;             /**< Array layer count */
  uint32_t mip_levels;          /**< Mip level count */
  VkrTextureType type;         /**< Texture type (2D, cube, etc.) */
  VkrRgResourceFlags flags;    /**< Lifetime and layout hints */
} VkrRgImageDesc;

#define VKR_RG_IMAGE_DESC_DEFAULT                                              \
  ((VkrRgImageDesc){                                                           \
      .width = 0,                                                              \
      .height = 0,                                                             \
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,                              \
      .usage = vkr_texture_usage_flags_create(),                               \
      .samples = VKR_SAMPLE_COUNT_1,                                           \
      .layers = 1,                                                             \
      .mip_levels = 1,                                                         \
      .type = VKR_TEXTURE_TYPE_2D,                                             \
      .flags = VKR_RG_RESOURCE_FLAG_TRANSIENT,                                 \
  })

/**
 * @brief Buffer resource specification for vkr_rg_create_buffer.
 * size must be greater than 0.
 */
typedef struct VkrRgBufferDesc {
  uint64_t size;               /**< Buffer size in bytes; must be > 0 */
  VkrBufferUsageFlags usage;   /**< Vulkan usage flags */
  VkrRgResourceFlags flags;    /**< Lifetime and layout hints */
} VkrRgBufferDesc;

/**
 * @brief Subregion of an image (mip + layer range) for attachment or barrier scope.
 * layer_count must be at least 1.
 */
typedef struct VkrRgImageSlice {
  uint32_t mip_level;   /**< Mip level index */
  uint32_t base_layer;  /**< First layer index */
  uint32_t layer_count; /**< Number of layers; must be >= 1 */
} VkrRgImageSlice;

#define VKR_RG_IMAGE_SLICE_DEFAULT                                             \
  ((VkrRgImageSlice){                                                          \
      .mip_level = 0,                                                          \
      .base_layer = 0,                                                         \
      .layer_count = 1,                                                        \
  })

// =============================================================================
// Pass and Access Descriptions
// =============================================================================

/**
 * @brief Pass type; determines scheduling and pipeline kind.
 */
typedef enum VkrRgPassType {
  VKR_RG_PASS_TYPE_GRAPHICS = 0, /**< Graphics pass (render pass) */
  VKR_RG_PASS_TYPE_COMPUTE = 1,   /**< Compute pass */
  VKR_RG_PASS_TYPE_TRANSFER = 2,  /**< Transfer/copy pass */
} VkrRgPassType;

/**
 * @brief Pass behavior flags.
 */
typedef enum VkrRgPassFlags {
  VKR_RG_PASS_FLAG_NONE = 0,      /**< Default behavior */
  VKR_RG_PASS_FLAG_NO_CULL = 1 << 0, /**< Do not skip pass when outputs are unused */
  VKR_RG_PASS_FLAG_DISABLED = 1 << 1, /**< Do not run the pass */
} VkrRgPassFlags;

/**
 * @brief Image access in a pass; used to infer layout transitions and barriers.
 * Combine flags for read+write where allowed.
 */
typedef enum VkrRgImageAccessFlags {
  VKR_RG_IMAGE_ACCESS_NONE = 0,
  VKR_RG_IMAGE_ACCESS_SAMPLED = 1 << 0,
  VKR_RG_IMAGE_ACCESS_STORAGE_READ = 1 << 1,
  VKR_RG_IMAGE_ACCESS_STORAGE_WRITE = 1 << 2,
  VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT = 1 << 3,
  VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT = 1 << 4,
  VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY = 1 << 5,
  VKR_RG_IMAGE_ACCESS_TRANSFER_SRC = 1 << 6,
  VKR_RG_IMAGE_ACCESS_TRANSFER_DST = 1 << 7,
  VKR_RG_IMAGE_ACCESS_PRESENT = 1 << 8,
} VkrRgImageAccessFlags;

/**
 * @brief Declares one image use in a pass.
 */
typedef struct VkrRgImageUse {
  VkrRgImageHandle image;       /**< Image handle */
  VkrRgImageAccessFlags access; /**< Access type for barriers */
  uint32_t binding;            /**< Descriptor binding index */
  uint32_t array_index;        /**< Descriptor array index */
} VkrRgImageUse;

Vector(VkrRgImageUse);

typedef VkrBufferAccessFlags VkrRgBufferAccessFlags;

#define VKR_RG_BUFFER_ACCESS_NONE VKR_BUFFER_ACCESS_NONE
#define VKR_RG_BUFFER_ACCESS_VERTEX VKR_BUFFER_ACCESS_VERTEX
#define VKR_RG_BUFFER_ACCESS_INDEX VKR_BUFFER_ACCESS_INDEX
#define VKR_RG_BUFFER_ACCESS_UNIFORM VKR_BUFFER_ACCESS_UNIFORM
#define VKR_RG_BUFFER_ACCESS_STORAGE_READ VKR_BUFFER_ACCESS_STORAGE_READ
#define VKR_RG_BUFFER_ACCESS_STORAGE_WRITE VKR_BUFFER_ACCESS_STORAGE_WRITE
#define VKR_RG_BUFFER_ACCESS_TRANSFER_SRC VKR_BUFFER_ACCESS_TRANSFER_SRC
#define VKR_RG_BUFFER_ACCESS_TRANSFER_DST VKR_BUFFER_ACCESS_TRANSFER_DST

/**
 * @brief Declares one buffer use in a pass.
 */
typedef struct VkrRgBufferUse {
  VkrRgBufferHandle buffer;    /**< Buffer handle */
  VkrRgBufferAccessFlags access; /**< Access type for barriers */
  uint32_t binding;          /**< Descriptor binding index */
  uint32_t array_index;       /**< Descriptor array index */
} VkrRgBufferUse;

Vector(VkrRgBufferUse);

/**
 * @brief Load/store and clear for a single attachment.
 * slice defines which mip/layers are used.
 */
typedef struct VkrRgAttachmentDesc {
  VkrRgImageSlice slice;        /**< Image subregion (mip + layers) */
  VkrAttachmentLoadOp load_op;   /**< Load operation */
  VkrAttachmentStoreOp store_op; /**< Store operation */
  VkrClearValue clear_value;     /**< Clear value when load_op is clear */
} VkrRgAttachmentDesc;

/**
 * @brief One attachment (color or depth): image handle plus load/store/clear.
 */
typedef struct VkrRgAttachment {
  VkrRgImageHandle image;  /**< Image handle */
  VkrRgAttachmentDesc desc; /**< Load/store/clear and slice */
  bool8_t read_only;       /**< If true, depth is read-only (e.g. depth prepass) */
} VkrRgAttachment;

Vector(VkrRgAttachment);

/**
 * @brief Full pass specification.
 * Vectors are owned by the graph after add_pass; name and execute_name must be stable for the graph lifetime.
 * execute may be NULL if execute_name is set and resolved later from the executor registry.
 */
typedef struct VkrRgPassDesc {
  String8 name;                   /**< Pass name (stable pointer) */
  VkrRgPassType type;              /**< Pass type */
  VkrRgPassFlags flags;            /**< Pass flags */

  VkrPipelineDomain domain;       /**< Pipeline domain for render pass selection */
  Vector_VkrRgAttachment color_attachments; /**< Color attachments in order */
  bool8_t has_depth_attachment;    /**< True if depth_attachment is used */
  VkrRgAttachment depth_attachment; /**< Depth attachment (valid if has_depth_attachment) */

  Vector_VkrRgImageUse image_reads;  /**< Image read uses */
  Vector_VkrRgImageUse image_writes;  /**< Image write uses */
  Vector_VkrRgBufferUse buffer_reads;  /**< Buffer read uses */
  Vector_VkrRgBufferUse buffer_writes; /**< Buffer write uses */

  String8 execute_name;           /**< Name to resolve execute from registry (optional) */
  VkrRgPassExecuteFn execute;    /**< Execute callback (may be set directly or via execute_name) */
  void *user_data;               /**< User data passed to execute */
} VkrRgPassDesc;

// =============================================================================
// Pass Context
// =============================================================================

/**
 * @brief Read-only context passed to VkrRgPassExecuteFn.
 * Valid only during the execute callback. render_target is the primary target;
 * render_targets[0..render_target_count-1] are the color/depth targets. image_index is the swapchain image index.
 */
typedef struct VkrRgPassContext {
  struct VkrRenderGraph *graph;   /**< Render graph owning this pass */
  const VkrRgPassDesc *pass_desc; /**< Pass descriptor */
  uint32_t pass_index;           /**< Pass index in the graph */

  struct s_RendererFrontend *renderer; /**< Renderer frontend for backend calls */
  VkrRenderPassHandle renderpass;     /**< Current render pass */
  VkrRenderTargetHandle render_target; /**< Primary render target */
  VkrRenderTargetHandle *render_targets; /**< Color/depth targets for this pass */
  uint32_t render_target_count;  /**< Number of elements in render_targets */

  uint32_t frame_index;   /**< Current frame index */
  uint32_t image_index;  /**< Swapchain image index for per-image resources */
  float64_t delta_time;  /**< Frame delta time */
} VkrRgPassContext;

/**
 * @brief Attaches a render packet to the graph for the next vkr_rg_execute().
 * The graph stores the pointer only; the packet must remain valid for the duration of that execute call.
 * @param graph Render graph
 * @param packet Render packet to attach
 */
void vkr_rg_set_packet(struct VkrRenderGraph *graph,
                       const VkrRenderPacket *packet);

/**
 * @brief Gets the world pass payload from the current render packet.
 * @param ctx Pass context
 * @return World pass payload, or NULL if not set or not applicable
 */
const VkrWorldPassPayload *
vkr_rg_pass_get_world_payload(const VkrRgPassContext *ctx);

/**
 * @brief Gets the shadow pass payload from the current render packet.
 * @param ctx Pass context
 * @return Shadow pass payload, or NULL if not set or not applicable
 */
const VkrShadowPassPayload *
vkr_rg_pass_get_shadow_payload(const VkrRgPassContext *ctx);

/**
 * @brief Gets the skybox pass payload from the current render packet.
 * @param ctx Pass context
 * @return Skybox pass payload, or NULL if not set or not applicable
 */
const VkrSkyboxPassPayload *
vkr_rg_pass_get_skybox_payload(const VkrRgPassContext *ctx);

/**
 * @brief Gets the UI pass payload from the current render packet.
 * @param ctx Pass context
 * @return UI pass payload, or NULL if not set or not applicable
 */
const VkrUiPassPayload *vkr_rg_pass_get_ui_payload(const VkrRgPassContext *ctx);

/**
 * @brief Gets the editor pass payload from the current render packet.
 * @param ctx Pass context
 * @return Editor pass payload, or NULL if not set or not applicable
 */
const VkrEditorPassPayload *
vkr_rg_pass_get_editor_payload(const VkrRgPassContext *ctx);

/**
 * @brief Gets the picking pass payload from the current render packet.
 * @param ctx Pass context
 * @return Picking pass payload, or NULL if not set or not applicable
 */
const VkrPickingPassPayload *
vkr_rg_pass_get_picking_payload(const VkrRgPassContext *ctx);

/**
 * @brief Gets the render packet attached for this execute.
 * @param ctx Pass context
 * @return Render packet, or NULL if none was set
 */
const VkrRenderPacket *vkr_rg_pass_get_packet(const VkrRgPassContext *ctx);

/**
 * @brief Gets frame info from the current render packet.
 * @param ctx Pass context
 * @return Frame info
 */
const VkrFrameInfo *vkr_rg_pass_get_frame_info(const VkrRgPassContext *ctx);

/**
 * @brief Gets frame globals from the current render packet.
 * @param ctx Pass context
 * @return Frame globals
 */
const VkrFrameGlobals *
vkr_rg_pass_get_frame_globals(const VkrRgPassContext *ctx);

/**
 * @brief Resolves a render-graph image to a backend texture for a specific swapchain image index.
 * @param graph Render graph
 * @param image Image handle
 * @param image_index Swapchain image index
 * @return Backend texture handle; invalid if image handle is invalid
 */
VkrTextureOpaqueHandle
vkr_rg_get_image_texture(const struct VkrRenderGraph *graph,
                         VkrRgImageHandle image, uint32_t image_index);

/**
 * @brief Finds an image handle by name in the graph.
 * @param graph Render graph
 * @param name Image name
 * @return Image handle, or VKR_RG_IMAGE_HANDLE_INVALID if not found
 */
VkrRgImageHandle vkr_rg_find_image(const struct VkrRenderGraph *graph,
                                   String8 name);

/**
 * @brief Resolves a render-graph buffer to a backend buffer handle for a specific swapchain image index.
 * @param graph Render graph
 * @param buffer Buffer handle
 * @param image_index Swapchain image index
 * @return Backend buffer handle; invalid if buffer handle is invalid
 */
VkrBufferHandle vkr_rg_get_buffer_handle(const struct VkrRenderGraph *graph,
                                         VkrRgBufferHandle buffer,
                                         uint32_t image_index);

/**
 * @brief Resolves a render-graph image for the current pass context (uses ctx->image_index).
 * @param ctx Pass context
 * @param image Image handle
 * @return Backend texture handle
 */
VkrTextureOpaqueHandle
vkr_rg_pass_get_image_texture(const VkrRgPassContext *ctx,
                              VkrRgImageHandle image);

/**
 * @brief Resolves a render-graph buffer for the current pass context (uses ctx->image_index).
 * @param ctx Pass context
 * @param buffer Buffer handle
 * @return Backend buffer handle
 */
VkrBufferHandle vkr_rg_pass_get_buffer_handle(const VkrRgPassContext *ctx,
                                              VkrRgBufferHandle buffer);

/**
 * @brief Named pass executor; name is used to resolve execute_name in pass descriptors.
 * user_data is passed to execute; ownership stays with the caller.
 */
typedef struct VkrRgPassExecutor {
  String8 name;               /**< Executor name (used for lookup) */
  VkrRgPassExecuteFn execute; /**< Execute callback */
  void *user_data;            /**< User data passed to execute */
} VkrRgPassExecutor;

Vector(VkrRgPassExecutor);

/**
 * @brief Registry of named pass executors for resolving execute_name at compile time.
 * allocator is used for entries and must outlive the registry.
 */
typedef struct VkrRgExecutorRegistry {
  VkrAllocator *allocator;         /**< Allocator for entries; must outlive registry */
  Vector_VkrRgPassExecutor entries; /**< Registered executors */
  bool8_t initialized;             /**< True after init */
} VkrRgExecutorRegistry;

/**
 * @brief Initializes the executor registry.
 * @param reg Registry to initialize
 * @param allocator Allocator; must remain valid until destroy
 * @return true on success, false on allocation failure
 */
bool8_t vkr_rg_executor_registry_init(VkrRgExecutorRegistry *reg,
                                      VkrAllocator *allocator);

/**
 * @brief Destroys the executor registry and frees all entries.
 * @param reg Registry to destroy; may be reused after init
 */
void vkr_rg_executor_registry_destroy(VkrRgExecutorRegistry *reg);

/**
 * @brief Registers a pass executor by name.
 * @param reg Registry
 * @param entry Executor to register; duplicate names overwrite
 * @return true on success, false on allocation failure
 */
bool8_t vkr_rg_executor_registry_register(VkrRgExecutorRegistry *reg,
                                          const VkrRgPassExecutor *entry);

/**
 * @brief Finds an executor by name.
 * @param reg Registry
 * @param name Executor name
 * @param out_user_data Optional; receives executor user_data pointer
 * @return Execute function, or NULL if not found
 */
VkrRgPassExecuteFn
vkr_rg_executor_registry_find(const VkrRgExecutorRegistry *reg, String8 name,
                              void **out_user_data);

// =============================================================================
// Builder API
// =============================================================================

typedef struct VkrRenderGraph VkrRenderGraph;

/**
 * @brief Builder for a single pass; valid only until the next vkr_rg_add_pass or vkr_rg_compile.
 * Do not hold across begin_frame/end_frame.
 */
typedef struct VkrRgPassBuilder {
  VkrRenderGraph *graph;  /**< Render graph owning the pass */
  uint32_t pass_index;   /**< Pass index in the graph */
} VkrRgPassBuilder;

/**
 * @brief Per-frame inputs to the graph.
 * Passed to vkr_rg_begin_frame; copied by the graph.
 */
typedef struct VkrRenderGraphFrameInfo {
  uint32_t frame_index;             /**< Current frame index */
  uint32_t image_index;             /**< Swapchain image index */
  float64_t delta_time;             /**< Frame delta time */
  uint32_t window_width;            /**< Window width */
  uint32_t window_height;           /**< Window height */
  uint32_t viewport_width;          /**< Viewport width */
  uint32_t viewport_height;         /**< Viewport height */
  bool8_t editor_enabled;           /**< Whether editor is enabled */
  VkrTextureFormat swapchain_format; /**< Swapchain color format */
  VkrTextureFormat swapchain_depth_format; /**< Swapchain depth format */
  VkrTextureFormat shadow_depth_format; /**< Shadow map depth format */
  uint32_t shadow_map_size;         /**< Shadow map dimension */
  uint32_t shadow_cascade_count;    /**< Number of shadow cascades */
} VkrRenderGraphFrameInfo;

/**
 * @brief Resource lifetime statistics for graph-owned allocations (imports excluded).
 * live_*: current frame; peak_*: maximum since creation or last reset.
 */
typedef struct VkrRenderGraphResourceStats {
  uint32_t live_image_textures;  /**< Current image texture count */
  uint32_t peak_image_textures;  /**< Peak image texture count */
  uint64_t live_image_bytes;    /**< Current image memory bytes */
  uint64_t peak_image_bytes;    /**< Peak image memory bytes */
  uint32_t live_buffers;        /**< Current buffer count */
  uint32_t peak_buffers;        /**< Peak buffer count */
  uint64_t live_buffer_bytes;   /**< Current buffer memory bytes */
  uint64_t peak_buffer_bytes;   /**< Peak buffer memory bytes */
} VkrRenderGraphResourceStats;

/**
 * @brief Per-pass timing from the last execute.
 * name is a view into graph state; valid until next vkr_rg_begin_frame or graph destroy.
 * gpu_ms/gpu_valid reflect the last completed frame if GPU timing is supported.
 */
typedef struct VkrRgPassTiming {
  String8 name;      /**< Pass name */
  float64_t cpu_ms;  /**< CPU time in milliseconds */
  float64_t gpu_ms;  /**< GPU time in milliseconds (if gpu_valid) */
  bool8_t culled;   /**< True if pass was culled */
  bool8_t disabled; /**< True if pass was disabled */
  bool8_t gpu_valid; /**< True if gpu_ms is valid */
} VkrRgPassTiming;

Vector(VkrRgPassTiming);

/**
 * @brief Creates a new render graph.
 * @param allocator Allocator for all graph-owned data; must outlive the graph
 * @return New graph, or NULL on allocation failure
 */
VkrRenderGraph *vkr_rg_create(VkrAllocator *allocator);

/**
 * @brief Destroys the graph and all owned resources.
 * @param graph Graph to destroy; no-op if NULL
 */
void vkr_rg_destroy(VkrRenderGraph *graph);

/**
 * @brief Starts a new frame; updates frame info and may resize/recreate transient resources.
 * Must be paired with vkr_rg_end_frame.
 * @param graph Render graph
 * @param frame Frame info to copy
 */
void vkr_rg_begin_frame(VkrRenderGraph *graph,
                        const VkrRenderGraphFrameInfo *frame);

/**
 * @brief Ends the frame; releases frame-specific state. Call after execute for the frame is done.
 * @param graph Render graph
 */
void vkr_rg_end_frame(VkrRenderGraph *graph);

/**
 * @brief Gets the frame info last passed to begin_frame.
 * @param graph Render graph
 * @param out_frame Output frame info
 * @return true on success, false if no frame is active
 */
bool8_t vkr_rg_get_frame_info(const VkrRenderGraph *graph,
                              VkrRenderGraphFrameInfo *out_frame);

/**
 * @brief Gets resource lifetime statistics (graph-owned only; imports excluded).
 * @param graph Render graph
 * @param out_stats Output statistics
 * @return true on success, false if graph is NULL or stats unavailable
 */
bool8_t vkr_rg_get_resource_stats(const VkrRenderGraph *graph,
                                  VkrRenderGraphResourceStats *out_stats);

/**
 * @brief Gets pass timings from the last execute.
 * @param graph Render graph
 * @param out_timings Receives pointer to timing array; valid until next begin_frame or destroy
 * @param out_count Receives number of timings
 * @return true if timings are available, false otherwise
 */
bool8_t vkr_rg_get_pass_timings(const VkrRenderGraph *graph,
                                const VkrRgPassTiming **out_timings,
                                uint32_t *out_count);

/**
 * @brief Logs current resource stats under the given label.
 * @param graph Render graph
 * @param label Label for the log output (e.g. for debugging)
 */
void vkr_rg_log_resource_stats(const VkrRenderGraph *graph, const char *label);

/**
 * @brief Declares a new graph-owned image.
 * @param graph Render graph
 * @param name Unique image name
 * @param desc Image description
 * @return Image handle, or invalid handle on failure
 */
VkrRgImageHandle vkr_rg_create_image(VkrRenderGraph *graph, String8 name,
                                     const VkrRgImageDesc *desc);

/**
 * @brief Declares an external image (EXTERNAL flag). handle/layout/access describe current state for barrier placement.
 * @param graph Render graph
 * @param name Image name
 * @param handle Backend texture handle
 * @param current_access Current access flags
 * @param current_layout Current layout
 * @param desc Image description (for dimensions/format when needed)
 * @return Image handle
 */
VkrRgImageHandle vkr_rg_import_image(VkrRenderGraph *graph, String8 name,
                                     VkrTextureOpaqueHandle handle,
                                     VkrRgImageAccessFlags current_access,
                                     VkrTextureLayout current_layout,
                                     const VkrRgImageDesc *desc);

/**
 * @brief Imports the swapchain image for the current frame (one image per image_index).
 * @param graph Render graph
 * @return Image handle for the swapchain image
 */
VkrRgImageHandle vkr_rg_import_swapchain(VkrRenderGraph *graph);

/**
 * @brief Imports the shared depth buffer used for the frame.
 * @param graph Render graph
 * @return Image handle for the depth buffer
 */
VkrRgImageHandle vkr_rg_import_depth(VkrRenderGraph *graph);

/**
 * @brief Declares a new graph-owned buffer.
 * @param graph Render graph
 * @param name Unique buffer name
 * @param desc Buffer description
 * @return Buffer handle, or invalid handle on failure
 */
VkrRgBufferHandle vkr_rg_create_buffer(VkrRenderGraph *graph, String8 name,
                                       const VkrRgBufferDesc *desc);

/**
 * @brief Declares an external buffer (EXTERNAL). current_access is used for initial barrier.
 * @param graph Render graph
 * @param name Buffer name
 * @param handle Backend buffer handle
 * @param current_access Current access flags
 * @return Buffer handle
 */
VkrRgBufferHandle vkr_rg_import_buffer(VkrRenderGraph *graph, String8 name,
                                       VkrBufferHandle handle,
                                       VkrRgBufferAccessFlags current_access);

/**
 * @brief Adds a pass and returns a builder for it. Builder is invalid after next add_pass or compile.
 * @param graph Render graph
 * @param type Pass type
 * @param name Pass name
 * @return Pass builder
 */
VkrRgPassBuilder vkr_rg_add_pass(VkrRenderGraph *graph, VkrRgPassType type,
                                 String8 name);

/**
 * @brief Sets the execute callback and user_data for the pass. Overrides execute_name resolution if both set.
 * @param pb Pass builder
 * @param execute Execute callback
 * @param user_data User data passed to execute
 */
void vkr_rg_pass_set_execute(VkrRgPassBuilder *pb, VkrRgPassExecuteFn execute,
                             void *user_data);

/**
 * @brief Sets pass flags.
 * @param pb Pass builder
 * @param flags Pass flags
 */
void vkr_rg_pass_set_flags(VkrRgPassBuilder *pb, VkrRgPassFlags flags);

/**
 * @brief Sets pipeline domain (world/ui/shadow/post) for render pass and pipeline selection.
 * @param pb Pass builder
 * @param domain Pipeline domain
 */
void vkr_rg_pass_set_domain(VkrRgPassBuilder *pb, VkrPipelineDomain domain);

/**
 * @brief Adds one color attachment; order determines layout index.
 * @param pb Pass builder
 * @param image Image handle
 * @param desc Attachment description
 */
void vkr_rg_pass_add_color_attachment(VkrRgPassBuilder *pb,
                                      VkrRgImageHandle image,
                                      const VkrRgAttachmentDesc *desc);

/**
 * @brief Sets the single depth attachment.
 * @param pb Pass builder
 * @param image Image handle
 * @param desc Attachment description
 * @param read_only If true, depth is read-only (e.g. depth prepass)
 */
void vkr_rg_pass_set_depth_attachment(VkrRgPassBuilder *pb,
                                      VkrRgImageHandle image,
                                      const VkrRgAttachmentDesc *desc,
                                      bool8_t read_only);

/**
 * @brief Declares a read use of an image.
 * @param pb Pass builder
 * @param image Image handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
void vkr_rg_pass_read_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                            VkrRgImageAccessFlags access, uint32_t binding,
                            uint32_t array_index);

/**
 * @brief Declares a write use of an image.
 * @param pb Pass builder
 * @param image Image handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
void vkr_rg_pass_write_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                             VkrRgImageAccessFlags access, uint32_t binding,
                             uint32_t array_index);

/**
 * @brief Declares a read use of a buffer.
 * @param pb Pass builder
 * @param buffer Buffer handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
void vkr_rg_pass_read_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                             VkrRgBufferAccessFlags access, uint32_t binding,
                             uint32_t array_index);

/**
 * @brief Declares a write use of a buffer.
 * @param pb Pass builder
 * @param buffer Buffer handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
void vkr_rg_pass_write_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                              VkrRgBufferAccessFlags access, uint32_t binding,
                              uint32_t array_index);

/**
 * @brief Marks the image as the present target for the frame (swapchain).
 * @param graph Render graph
 * @param image Image handle (typically swapchain)
 */
void vkr_rg_set_present_image(VkrRenderGraph *graph, VkrRgImageHandle image);

/**
 * @brief Marks image as exported (retain final layout/access for external use).
 * @param graph Render graph
 * @param image Image handle
 */
void vkr_rg_export_image(VkrRenderGraph *graph, VkrRgImageHandle image);

/**
 * @brief Marks buffer as exported (retain final access for external use).
 * @param graph Render graph
 * @param buffer Buffer handle
 */
void vkr_rg_export_buffer(VkrRenderGraph *graph, VkrRgBufferHandle buffer);

/**
 * @brief Compiles the graph: validates, schedules passes, allocates resources, and prepares barriers.
 * Must be called after all passes are added and before execute.
 * @param graph Render graph
 * @return true on success, false on validation or allocation failure
 */
bool8_t vkr_rg_compile(VkrRenderGraph *graph);

/**
 * @brief Runs the compiled graph for the current frame.
 * Requires a prior begin_frame; set_packet must be called if pass callbacks need the packet.
 * @param graph Render graph
 * @param rf Renderer frontend for backend calls
 */
void vkr_rg_execute(VkrRenderGraph *graph, struct s_RendererFrontend *rf);

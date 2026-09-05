#pragma once

#include "containers/str.h"
#include "containers/vector.h"
#include "defines.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_renderer.h"

// Forward declarations for stateless render packet access.
typedef struct VkrPreparedFrame VkrPreparedFrame;
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
  VKR_RG_RESOURCE_FLAG_NONE = 0, /**< No special flags */
  VKR_RG_RESOURCE_FLAG_TRANSIENT =
      1 << 0, /**< Frame-local contents with overlap-safe backend instances */
  VKR_RG_RESOURCE_FLAG_PERSISTENT = 1 << 1, /**< Kept across frames */
  VKR_RG_RESOURCE_FLAG_EXTERNAL = 1 << 2,   /**< Imported, not owned by graph */
  VKR_RG_RESOURCE_FLAG_PER_IMAGE =
      1 << 3, /**< One resource per swapchain image */
  VKR_RG_RESOURCE_FLAG_RESIZABLE = 1 << 4, /**< May be recreated on resize */
  VKR_RG_RESOURCE_FLAG_FORCE_ARRAY =
      1 << 5, /**< Force array view in descriptors */
  VKR_RG_RESOURCE_FLAG_PER_FRAME_SLOT =
      1 << 6, /**< One resource per completion-gated frame slot */
  VKR_RG_RESOURCE_FLAG_HISTORY =
      1 << 7, /**< Completion-gated history ring owned by the backend */
  /**
   * Contents survive across frames in place, per physical instance and per
   * subresource. See ADR-029.
   *
   * Distinct from HISTORY, which is a ring: history writes a new instance each
   * frame and reads an older one, while a retained subresource keeps whatever
   * was last written to it and may be sampled for many frames with no writer
   * scheduled at all. Distinct from PERSISTENT, which only suppresses the
   * read-before-write diagnostic and preserves nothing.
   *
   * Describes content lifetime, not instance domain, so it composes with
   * PER_IMAGE and RESIZABLE. Mutually exclusive with TRANSIENT, EXTERNAL,
   * HISTORY, and PER_FRAME_SLOT.
   */
  VKR_RG_RESOURCE_FLAG_RETAINED = 1 << 8,
} VkrRgResourceFlags;

/** Lifetime flags that cannot combine with RETAINED, for validation. */
#define VKR_RG_RESOURCE_RETAINED_EXCLUSIONS                                    \
  (VKR_RG_RESOURCE_FLAG_TRANSIENT | VKR_RG_RESOURCE_FLAG_EXTERNAL |            \
   VKR_RG_RESOURCE_FLAG_HISTORY | VKR_RG_RESOURCE_FLAG_PER_FRAME_SLOT)

/** Backend allocation/selection domain implied by resource lifetime flags. */
typedef enum VkrRgResourceInstanceDomain {
  VKR_RG_RESOURCE_INSTANCE_SINGLE = 0,
  VKR_RG_RESOURCE_INSTANCE_PER_IMAGE,
  VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT,
} VkrRgResourceInstanceDomain;

/**
 * Resolves lifetime flags to one backend instance domain. PER_IMAGE takes
 * precedence because TRANSIENT may annotate target-image-local contents.
 */
VkrRgResourceInstanceDomain
vkr_rg_resource_instance_domain(VkrRgResourceFlags flags);

/**
 * @brief Image resource specification for vkr_rg_create_image.
 * width/height 0 allowed for size-from-attachment or swapchain-derived;
 * otherwise must be positive.
 */
typedef struct VkrRgImageDesc {
  uint32_t width;  /**< Image width; 0 if derived from attachment/swapchain */
  uint32_t height; /**< Image height; 0 if derived */
  VkrTextureFormat format;    /**< Pixel format */
  VkrTextureUsageFlags usage; /**< Vulkan usage flags */
  VkrSampleCount samples;     /**< Sample count (MSAA) */
  uint32_t layers;            /**< Array layer count */
  uint32_t mip_levels;        /**< Mip level count */
  VkrTextureType type;        /**< Texture type (2D, cube, etc.) */
  VkrRgResourceFlags flags;   /**< Lifetime and layout hints */
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
  uint64_t size;             /**< Buffer size in bytes; must be > 0 */
  VkrBufferUsageFlags usage; /**< Vulkan usage flags */
  VkrRgResourceFlags flags;  /**< Lifetime and layout hints */
} VkrRgBufferDesc;

/**
 * @brief Subregion of an image (mip + layer range) for attachment or barrier
 * scope. layer_count must be at least 1.
 */
typedef struct VkrRgImageSlice {
  uint32_t mip_level;   /**< First mip level */
  uint32_t mip_count;   /**< Number of mip levels; zero preserves legacy 1 */
  uint32_t base_layer;  /**< First layer index */
  uint32_t layer_count; /**< Number of layers; must be >= 1 */
} VkrRgImageSlice;

#define VKR_RG_IMAGE_SLICE_DEFAULT                                             \
  ((VkrRgImageSlice){                                                          \
      .mip_level = 0,                                                          \
      .mip_count = 1,                                                          \
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
  VKR_RG_PASS_TYPE_COMPUTE = 1,  /**< Compute pass */
  VKR_RG_PASS_TYPE_TRANSFER = 2, /**< Transfer/copy pass */
} VkrRgPassType;

/**
 * @brief Pass behavior flags.
 */
typedef enum VkrRgPassFlags {
  VKR_RG_PASS_FLAG_NONE = 0, /**< Default behavior */
  VKR_RG_PASS_FLAG_NO_CULL =
      1 << 0, /**< Do not skip pass when outputs are unused */
  VKR_RG_PASS_FLAG_DISABLED = 1 << 1, /**< Do not run the pass */
} VkrRgPassFlags;

/** @brief Compute dispatch source carried by an authored compute pass. */
typedef enum VkrRgDispatchKind {
  VKR_RG_DISPATCH_NONE = 0,     /**< Specialized executor owns dispatches */
  VKR_RG_DISPATCH_DIRECT = 1,   /**< Authored workgroup counts */
  VKR_RG_DISPATCH_INDIRECT = 2, /**< Workgroup counts read from a buffer */
} VkrRgDispatchKind;

/**
 * @brief Backend-neutral compute dispatch descriptor.
 *
 * DIRECT requires all three group counts to be non-zero. INDIRECT names a
 * declared buffer binding/array slot and a four-byte-aligned byte offset.
 */
typedef struct VkrRgComputeDispatchDesc {
  VkrRgDispatchKind kind;
  uint32_t group_count_x;
  uint32_t group_count_y;
  uint32_t group_count_z;
  uint32_t indirect_binding;
  uint32_t indirect_array_index;
  uint64_t indirect_offset;
} VkrRgComputeDispatchDesc;

/**
 * @brief Image access in a pass; used to infer layout transitions and barriers.
 *
 * Alias of the renderer-wide VkrImageAccessFlags, exactly as
 * VkrRgBufferAccessFlags aliases VkrBufferAccessFlags below. Sharing the
 * vocabulary is what lets the compiler's access masks reach the backend barrier
 * instead of being reduced to a layout pair.
 */
typedef VkrImageAccessFlags VkrRgImageAccessFlags;

#define VKR_RG_IMAGE_ACCESS_NONE VKR_IMAGE_ACCESS_NONE
#define VKR_RG_IMAGE_ACCESS_SAMPLED VKR_IMAGE_ACCESS_SAMPLED
#define VKR_RG_IMAGE_ACCESS_STORAGE_READ VKR_IMAGE_ACCESS_STORAGE_READ
#define VKR_RG_IMAGE_ACCESS_STORAGE_WRITE VKR_IMAGE_ACCESS_STORAGE_WRITE
#define VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT VKR_IMAGE_ACCESS_COLOR_ATTACHMENT
#define VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT
#define VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY VKR_IMAGE_ACCESS_DEPTH_READ_ONLY
#define VKR_RG_IMAGE_ACCESS_TRANSFER_SRC VKR_IMAGE_ACCESS_TRANSFER_SRC
#define VKR_RG_IMAGE_ACCESS_TRANSFER_DST VKR_IMAGE_ACCESS_TRANSFER_DST
#define VKR_RG_IMAGE_ACCESS_PRESENT VKR_IMAGE_ACCESS_PRESENT

/**
 * @brief Declares one image use in a pass.
 */
typedef struct VkrRgImageUse {
  VkrRgImageHandle image;       /**< Image handle */
  VkrRgImageAccessFlags access; /**< Access type for barriers */
  VkrGpuStageFlags stages;      /**< Exact execution scope */
  uint32_t binding;             /**< Descriptor binding index */
  uint32_t array_index;         /**< Descriptor array index */
  VkrRgImageSlice slice;        /**< Exact transfer subresource when set */
  bool8_t has_slice;
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
#define VKR_RG_BUFFER_ACCESS_INDIRECT_READ VKR_BUFFER_ACCESS_INDIRECT_READ

/**
 * @brief One retained subresource's committed cross-frame state. See ADR-029.
 *
 * `content_valid` is the authority for whether a reader may run with no writer
 * scheduled this frame. Access, stages, and layout describe how the last
 * successful submit left the subresource, so the next frame seeds its barrier
 * planning from there instead of from UNDEFINED.
 */
typedef struct VkrRgRetainedState {
  VkrRgImageAccessFlags access;
  VkrGpuStageFlags stages;
  VkrTextureLayout layout;
  bool8_t content_valid;
} VkrRgRetainedState;

/**
 * @brief Backend hook for retained cross-frame subresource state.
 *
 * The graph does not own retained state: physical instances outlive any single
 * frame's graph, so the selected implementation stores it beside the realized
 * instance and answers these calls.
 *
 * `read` is called during compilation to seed a retained subresource and must
 * report the last *successfully submitted* state. `commit` is called only after
 * a submit is proven to have happened, which is what stops a cancelled frame
 * from advertising contents it never wrote.
 */
typedef struct VkrRgRetainedStateProvider {
  void *context;
  void (*read)(void *context, uint32_t image_index, uint32_t instance_index,
               uint32_t subresource, VkrRgRetainedState *out_state);
  void (*commit)(void *context, uint32_t image_index, uint32_t instance_index,
                 uint32_t subresource, const VkrRgRetainedState *state);
} VkrRgRetainedStateProvider;

struct VkrRenderGraph;

/**
 * @brief Installs the retained-state provider. Pass NULL to clear it.
 *
 * Without a provider, retained resources seed as UNDEFINED with invalid
 * content, so a graph declaring one still compiles but can never reuse.
 */
void vkr_rg_set_retained_state_provider(
    struct VkrRenderGraph *graph, const VkrRgRetainedStateProvider *provider);

/**
 * @brief Commits this frame's retained terminal states.
 *
 * Call only after a submit has provably happened. Not calling it is the
 * rollback path; there is no separate discard entry point, so a cancelled frame
 * rolls back by doing nothing.
 */
void vkr_rg_commit_retained_state(struct VkrRenderGraph *graph);

/**
 * @brief Declares one buffer use in a pass.
 */
typedef struct VkrRgBufferUse {
  VkrRgBufferHandle buffer;      /**< Buffer handle */
  VkrRgBufferAccessFlags access; /**< Access type for barriers */
  VkrGpuStageFlags stages;       /**< Exact execution scope */
  uint32_t binding;              /**< Descriptor binding index */
  uint32_t array_index;          /**< Descriptor array index */
} VkrRgBufferUse;

Vector(VkrRgBufferUse);

/**
 * @brief Load/store and clear for a single attachment.
 * slice defines which mip/layers are used.
 */
typedef struct VkrRgAttachmentDesc {
  VkrRgImageSlice slice;         /**< Image subregion (mip + layers) */
  VkrAttachmentLoadOp load_op;   /**< Load operation */
  VkrAttachmentStoreOp store_op; /**< Store operation */
  VkrClearValue clear_value;     /**< Clear value when load_op is clear */
} VkrRgAttachmentDesc;

/**
 * @brief One attachment (color or depth): image handle plus load/store/clear.
 */
typedef struct VkrRgAttachment {
  VkrRgImageHandle image;   /**< Image handle */
  VkrRgAttachmentDesc desc; /**< Load/store/clear and slice */
  bool8_t read_only; /**< If true, depth is read-only (e.g. depth prepass) */
} VkrRgAttachment;

Vector(VkrRgAttachment);

/**
 * @brief Full pass specification.
 * Vectors and name are owned by the graph after add_pass. Authored passes carry
 * a backend-local executor_id resolved once when their JSON graph is loaded.
 */
typedef struct VkrRgPassDesc {
  String8 name;         /**< Pass name (stable pointer) */
  VkrRgPassType type;   /**< Pass type */
  VkrRgPassFlags flags; /**< Pass flags */

  VkrPipelineDomain domain; /**< Pipeline domain for render pass selection */
  Vector_VkrRgAttachment color_attachments; /**< Color attachments in order */
  bool8_t has_depth_attachment; /**< True if depth_attachment is used */
  VkrRgAttachment
      depth_attachment; /**< Depth attachment (valid if has_depth_attachment) */

  Vector_VkrRgImageUse image_reads;    /**< Image read uses */
  Vector_VkrRgImageUse image_writes;   /**< Image write uses */
  Vector_VkrRgBufferUse buffer_reads;  /**< Buffer read uses */
  Vector_VkrRgBufferUse buffer_writes; /**< Buffer write uses */

  VkrRgComputeDispatchDesc dispatch; /**< Optional compute dispatch */
  uint32_t executor_id;  /**< Backend-local typed executor identity */
  uint32_t repeat_index; /**< Zero-based repetition in the authored pass. */
} VkrRgPassDesc;

/**
 * @brief Finds an image use by its authored descriptor binding and array slot.
 *
 * Reads and writes of the same image may share a binding; conflicting
 * resources at one binding are rejected during graph compilation.
 */
const VkrRgImageUse *vkr_rg_pass_find_image_use(const VkrRgPassDesc *pass,
                                                uint32_t binding,
                                                uint32_t array_index);

/** @brief Buffer counterpart to vkr_rg_pass_find_image_use. */
const VkrRgBufferUse *vkr_rg_pass_find_buffer_use(const VkrRgPassDesc *pass,
                                                  uint32_t binding,
                                                  uint32_t array_index);

/**
 * @brief Attaches a render packet to the graph for the next implementation
 * schedule execution.
 * The graph stores the pointer only; the packet must remain valid for the
 * duration of that execute call.
 * @param graph Render graph
 * @param packet Render packet to attach
 */
void vkr_rg_set_packet(struct VkrRenderGraph *graph,
                       const VkrPreparedFrame *packet);

/**
 * @brief Finds an image handle by name in the graph.
 * @param graph Render graph
 * @param name Image name
 * @return Image handle, or VKR_RG_IMAGE_HANDLE_INVALID if not found
 */
VkrRgImageHandle vkr_rg_find_image(const struct VkrRenderGraph *graph,
                                   String8 name);

/**
 * @brief Named typed pass executor resolved once when a JSON graph is loaded.
 */
typedef struct VkrRgPassExecutor {
  String8 name;       /**< Executor name (used for lookup) */
  uint32_t id;        /**< Non-zero backend-local identity */
  VkrRgPassType type; /**< Only this pass type may select the executor */
} VkrRgPassExecutor;

Vector(VkrRgPassExecutor);

/**
 * @brief Registry of typed named pass executors resolved when JSON is loaded.
 * allocator is used for entries and must outlive the registry.
 */
typedef struct VkrRgExecutorRegistry {
  VkrAllocator *allocator; /**< Allocator for entries; must outlive registry */
  Vector_VkrRgPassExecutor entries; /**< Registered executors */
  bool8_t initialized;              /**< True after init */
} VkrRgExecutorRegistry;

/**
 * @brief Initializes the executor registry.
 * @param reg Registry to initialize
 * @param allocator Allocator; must remain valid until destroy
 * @return true on success, false on allocation failure
 */
VKR_MUST_USE bool8_t vkr_rg_executor_registry_init(VkrRgExecutorRegistry *reg,
                                                   VkrAllocator *allocator);

/**
 * @brief Destroys the executor registry and frees all entries.
 * @param reg Registry to destroy; may be reused after init
 */
void vkr_rg_executor_registry_destroy(VkrRgExecutorRegistry *reg);

/**
 * @brief Registers a pass executor by name.
 * @param reg Registry
 * @param entry Executor to register; ids and names must both be unique
 * @return true on success, false on allocation failure
 */
VKR_MUST_USE bool8_t vkr_rg_executor_registry_register(
    VkrRgExecutorRegistry *reg, const VkrRgPassExecutor *entry);

/**
 * @brief Finds an executor by name.
 * @param reg Registry
 * @param name Executor name
 * @param out_user_data Optional; receives executor user_data pointer
 * @return Registered executor, or NULL if not found
 */
const VkrRgPassExecutor *
vkr_rg_executor_registry_find(const VkrRgExecutorRegistry *reg, String8 name);

// =============================================================================
// Builder API
// =============================================================================

typedef struct VkrRenderGraph VkrRenderGraph;

/**
 * @brief Builder for a single pass; a NULL graph reports creation failure;
 * valid only until the next vkr_rg_add_pass or vkr_rg_compile. Do not hold
 * across begin_frame/end_frame.
 */
typedef struct VkrRgPassBuilder {
  VkrRenderGraph *graph; /**< Render graph owning the pass */
  uint32_t pass_index;   /**< Pass index in the graph */
} VkrRgPassBuilder;

/**
 * @brief Per-frame inputs to the graph.
 * Passed to vkr_rg_begin_frame; copied by the graph.
 */
typedef struct VkrRenderGraphFrameInfo {
  uint32_t frame_index;   /**< Current frame index */
  uint32_t image_index;   /**< Present-target image index */
  float64_t delta_time;   /**< Frame delta time */
  uint32_t target_width;  /**< Present-target width */
  uint32_t target_height; /**< Present-target height */
  uint32_t window_width;  /**< Window width */
  uint32_t window_height; /**< Window height */
  /** Scene presentation extent after reconstruction, before editor composite.
   */
  uint32_t scene_output_width;
  uint32_t scene_output_height;
  uint32_t viewport_width;  /**< Viewport width */
  uint32_t viewport_height; /**< Viewport height */
  float32_t render_scale;   /**< Internal Scene extent / Scene output extent. */
  /** True only for the MetalFX temporal reconstruction topology. */
  bool8_t metalfx_enabled;
  bool8_t editor_enabled; /**< Whether editor is enabled */
  /** True only when a completion-protected HZB history generation is valid. */
  bool8_t hzb_history_valid;
  /** Metal P6 occupied-depth feedback is enabled for this packet. */
  bool8_t sdsm_enabled;
  /** Number of graph-authored reductions after the HZB base mip. */
  uint32_t hzb_reduce_pass_count;
  /** Bounded rough-transmission reductions after the opaque base mip. */
  uint32_t transmission_rough_mip_pass_count;
  /** True when the packet contains transmissive world work. */
  bool8_t transmission_pending;
  /** True only when a focused capture requests the post-layer-3 peel. */
  bool8_t transmission_depth_diagnostic_enabled;
  /**
   * True only for an automatic-exposure packet. Manual frames must not pay for
   * metering, so both exposure passes and both exposure buffers are gated here
   * rather than being executed and discarded.
   */
  bool8_t exposure_automatic;
  /**
   * True only for a frame that both requests bloom and has a viewport large
   * enough for a chain. Gates both bloom images and every bloom pass, so a
   * frame without bloom pays nothing for it.
   */
  bool8_t bloom_enabled;
  /**
   * Chain length, derived from the viewport and the cold bloom configuration.
   * The downsample and upsample repeats are each one shorter than this: the
   * prefilter produces mip 0, and the deepest level has nothing above it to
   * accumulate into.
   */
  uint32_t bloom_mip_count;
  /**
   * True only when the packet requests GTAO and the viewport has a usable
   * current-frame depth chain. Gates every GTAO image and pass.
   */
  bool8_t gtao_enabled;
  /** Current-frame AO depth levels, including full-resolution mip zero. */
  uint32_t gtao_depth_mip_count;
  /** Metal P19 state; false only for the diagnostic full-screen rollback. */
  bool8_t transmission_compact_enabled;
  /** True when this frame requests backend pass timestamps. */
  bool8_t timing_enabled;
  /**
   * Whether this frame's packet requests a pick. Gates the picking resources
   * and passes so a non-picking frame pays nothing for them.
   */
  bool8_t picking_pending;
  VkrTextureFormat target_color_format; /**< Present-target color format */
  VkrTextureFormat target_depth_format; /**< Present-target depth format */
  /** Access/layout each imported target attachment arrives in. */
  VkrPresentTargetImageState target_color_initial_state;
  VkrPresentTargetImageState target_depth_initial_state;
  /** Access/layout the graph must leave the present image in. */
  VkrPresentTargetImageState target_terminal_state;
  VkrTextureFormat shadow_depth_format; /**< Shadow map depth format */
  uint32_t shadow_map_size;             /**< Shadow map dimension */
  /** Configured shadow-map layer capacity; independent of packet readiness. */
  uint32_t shadow_map_layer_count;
  /** Number of active shadow cascades in the submitted packet. */
  uint32_t shadow_cascade_count;
  /** Bits of repeated shadow passes that must be instantiated this frame. */
  uint32_t shadow_cascade_render_mask;
} VkrRenderGraphFrameInfo;

/**
 * @brief Resource lifetime statistics for graph-owned allocations (imports
 * excluded). live_*: current frame; peak_*: maximum since creation or last
 * reset.
 */
typedef struct VkrRenderGraphResourceStats {
  uint32_t live_image_textures; /**< Current image texture count */
  uint32_t peak_image_textures; /**< Peak image texture count */
  uint64_t live_image_bytes;    /**< Current image memory bytes */
  uint64_t peak_image_bytes;    /**< Peak image memory bytes */
  uint32_t live_buffers;        /**< Current buffer count */
  uint32_t peak_buffers;        /**< Peak buffer count */
  uint64_t live_buffer_bytes;   /**< Current buffer memory bytes */
  uint64_t peak_buffer_bytes;   /**< Peak buffer memory bytes */
} VkrRenderGraphResourceStats;

/**
 * @brief Creates a new render graph.
 * @param allocator Allocator for all graph-owned data; must outlive the graph
 * @return New graph, or NULL on allocation failure
 */
VKR_MUST_USE VkrRenderGraph *vkr_rg_create(VkrAllocator *allocator);

/**
 * @brief Assigns a scoped allocator for frame-authored pass data.
 *
 * Persistent resources and backend caches remain on the allocator passed to
 * vkr_rg_create. The frame allocator is reset by the next vkr_rg_begin_frame,
 * after pass/name views from the preceding frame cease to be valid.
 *
 * @param graph Graph with no authored passes
 * @param allocator Scoped allocator that must outlive the graph
 * @return true on success, false if arguments or allocator are invalid
 */
VKR_MUST_USE bool8_t vkr_rg_set_frame_allocator(VkrRenderGraph *graph,
                                                VkrAllocator *allocator);

/**
 * @brief Destroys the graph and all owned resources.
 * @param graph Graph to destroy; no-op if NULL
 */
void vkr_rg_destroy(VkrRenderGraph *graph);

/**
 * @brief Starts a new frame; updates frame info and may resize/recreate
 * transient resources. A successful begin must be paired with vkr_rg_end_frame.
 * Returns false if the frame allocation scope cannot be created.
 * @param graph Render graph
 * @param frame Frame info to copy
 */
VKR_MUST_USE bool8_t vkr_rg_begin_frame(VkrRenderGraph *graph,
                                        const VkrRenderGraphFrameInfo *frame);

/**
 * @brief Ends the frame; releases frame-specific state. Call after execute for
 * the frame is done.
 * @param graph Render graph
 */
void vkr_rg_end_frame(VkrRenderGraph *graph);

/**
 * @brief Gets the frame info last passed to begin_frame.
 * @param graph Render graph
 * @param out_frame Output frame info
 * @return true on success, false for NULL graph or output
 */
bool8_t vkr_rg_get_frame_info(const VkrRenderGraph *graph,
                              VkrRenderGraphFrameInfo *out_frame);

/**
 * @brief Declares a new graph-owned image.
 * @param graph Render graph
 * @param name Unique image name
 * @param desc Image description
 * @return Image handle, or invalid handle on failure
 */
VKR_MUST_USE VkrRgImageHandle vkr_rg_create_image(VkrRenderGraph *graph,
                                                  String8 name,
                                                  const VkrRgImageDesc *desc);

/**
 * @brief Declares an external image (EXTERNAL flag). handle/layout/access
 * describe current state for barrier placement.
 * @param graph Render graph
 * @param name Image name
 * @param handle Backend texture handle
 * @param current_access Current access flags
 * @param current_layout Current layout
 * @param desc Image description (for dimensions/format when needed)
 * @return Image handle
 */
VKR_MUST_USE VkrRgImageHandle vkr_rg_import_image(
    VkrRenderGraph *graph, String8 name, VkrTextureOpaqueHandle handle,
    VkrRgImageAccessFlags current_access, VkrTextureLayout current_layout,
    const VkrRgImageDesc *desc);

/**
 * @brief Adds a usage capability to an imported image for the current graph.
 *
 * The caller must only advertise capabilities that the backing external image
 * was created with. This is used by optional graph overlays whose external
 * image requirements are not part of the authored graph.
 */
bool8_t vkr_rg_imported_image_add_usage(VkrRenderGraph *graph,
                                        VkrRgImageHandle image,
                                        VkrTextureUsageBits usage);

/**
 * @brief Declares a new graph-owned buffer.
 * @param graph Render graph
 * @param name Unique buffer name
 * @param desc Buffer description
 * @return Buffer handle, or invalid handle on failure
 */
VKR_MUST_USE VkrRgBufferHandle vkr_rg_create_buffer(
    VkrRenderGraph *graph, String8 name, const VkrRgBufferDesc *desc);

/**
 * @brief Declares an external buffer (EXTERNAL). current_access is used for
 * initial barrier.
 * @param graph Render graph
 * @param name Buffer name
 * @param handle Backend buffer handle
 * @param current_access Current access flags
 * @return Buffer handle
 */
VKR_MUST_USE VkrRgBufferHandle vkr_rg_import_buffer(
    VkrRenderGraph *graph, String8 name, VkrBufferHandle handle,
    VkrRgBufferAccessFlags current_access);

/** Adds a capability present on an imported buffer's backing allocation. */
bool8_t vkr_rg_imported_buffer_add_usage(VkrRenderGraph *graph,
                                         VkrRgBufferHandle buffer,
                                         VkrBufferUsageBits usage);

/**
 * @brief Adds a pass and returns a builder for it. Builder is invalid after
 * next add_pass or compile.
 * @param graph Render graph
 * @param type Pass type
 * @param name Pass name
 * @return Pass builder
 */
VKR_MUST_USE VkrRgPassBuilder vkr_rg_add_pass(VkrRenderGraph *graph,
                                              VkrRgPassType type, String8 name);

/**
 * @brief Sets pass flags.
 * @param pb Pass builder
 * @param flags Pass flags
 */
void vkr_rg_pass_set_flags(VkrRgPassBuilder *pb, VkrRgPassFlags flags);

/**
 * @brief Sets pipeline domain (world/ui/shadow/post) for render pass and
 * pipeline selection.
 * @param pb Pass builder
 * @param domain Pipeline domain
 */
void vkr_rg_pass_set_domain(VkrRgPassBuilder *pb, VkrPipelineDomain domain);

// Allocation-bearing builder operations return false without publishing the
// requested attachment/use/export. Successful earlier operations remain owned
// by the graph and are released by the next frame or graph destruction.

/**
 * @brief Adds one color attachment; order determines layout index.
 * @param pb Pass builder
 * @param image Image handle
 * @param desc Attachment description
 */
VKR_MUST_USE bool8_t
vkr_rg_pass_add_color_attachment(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                                 const VkrRgAttachmentDesc *desc);

/**
 * @brief Sets the single depth attachment.
 * @param pb Pass builder
 * @param image Image handle
 * @param desc Attachment description
 * @param read_only If true, depth is read-only (e.g. depth prepass)
 */
VKR_MUST_USE bool8_t vkr_rg_pass_set_depth_attachment(
    VkrRgPassBuilder *pb, VkrRgImageHandle image,
    const VkrRgAttachmentDesc *desc, bool8_t read_only);

/**
 * @brief Declares a read use of an image.
 * @param pb Pass builder
 * @param image Image handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
VKR_MUST_USE bool8_t vkr_rg_pass_read_image(VkrRgPassBuilder *pb,
                                            VkrRgImageHandle image,
                                            VkrRgImageAccessFlags access,
                                            uint32_t binding,
                                            uint32_t array_index);
VKR_MUST_USE bool8_t vkr_rg_pass_read_image_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index);
VKR_MUST_USE bool8_t vkr_rg_pass_read_image_slice(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    uint32_t binding, uint32_t array_index, VkrRgImageSlice slice);
VKR_MUST_USE bool8_t vkr_rg_pass_read_image_slice_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index,
    VkrRgImageSlice slice);

/**
 * @brief Declares a write use of an image.
 * @param pb Pass builder
 * @param image Image handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
VKR_MUST_USE bool8_t vkr_rg_pass_write_image(VkrRgPassBuilder *pb,
                                             VkrRgImageHandle image,
                                             VkrRgImageAccessFlags access,
                                             uint32_t binding,
                                             uint32_t array_index);
VKR_MUST_USE bool8_t vkr_rg_pass_write_image_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index);

/**
 * @brief Declares a read use of a buffer.
 * @param pb Pass builder
 * @param buffer Buffer handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
VKR_MUST_USE bool8_t vkr_rg_pass_read_buffer(VkrRgPassBuilder *pb,
                                             VkrRgBufferHandle buffer,
                                             VkrRgBufferAccessFlags access,
                                             uint32_t binding,
                                             uint32_t array_index);

/** @brief Writes an exact image mip/layer range. */
VKR_MUST_USE bool8_t vkr_rg_pass_write_image_slice_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index,
    VkrRgImageSlice slice);
VKR_MUST_USE bool8_t vkr_rg_pass_read_buffer_at_stages(
    VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
    VkrRgBufferAccessFlags access, VkrGpuStageFlags stages, uint32_t binding,
    uint32_t array_index);

/**
 * @brief Declares a write use of a buffer.
 * @param pb Pass builder
 * @param buffer Buffer handle
 * @param access Access flags
 * @param binding Descriptor binding index
 * @param array_index Descriptor array index
 */
VKR_MUST_USE bool8_t vkr_rg_pass_write_buffer(VkrRgPassBuilder *pb,
                                              VkrRgBufferHandle buffer,
                                              VkrRgBufferAccessFlags access,
                                              uint32_t binding,
                                              uint32_t array_index);
VKR_MUST_USE bool8_t vkr_rg_pass_write_buffer_at_stages(
    VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
    VkrRgBufferAccessFlags access, VkrGpuStageFlags stages, uint32_t binding,
    uint32_t array_index);

/**
 * @brief Marks the image as the present target for the frame (swapchain).
 * @param graph Render graph
 * @param image Image handle (typically swapchain)
 */
VKR_MUST_USE bool8_t vkr_rg_set_present_image(VkrRenderGraph *graph,
                                              VkrRgImageHandle image);

/**
 * @brief Marks image as exported (retain final layout/access for external use).
 * @param graph Render graph
 * @param image Image handle
 */
VKR_MUST_USE bool8_t vkr_rg_export_image(VkrRenderGraph *graph,
                                         VkrRgImageHandle image);

/**
 * @brief Marks buffer as exported (retain final access for external use).
 * @param graph Render graph
 * @param buffer Buffer handle
 */
VKR_MUST_USE bool8_t vkr_rg_export_buffer(VkrRenderGraph *graph,
                                          VkrRgBufferHandle buffer);

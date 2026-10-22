#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "defines.h"
#include "memory/vkr_allocator.h"
#include "vkr_gpu_abi.h"
#include "vkr_lighting.h"
#include "vkr_renderer.h"

// Forward declarations for stateless render packet access.
typedef struct VkrPreparedFrame VkrPreparedFrame;
typedef struct VkrFrameInfo VkrFrameInfo;
typedef struct VkrFrameGlobals VkrFrameGlobals;
typedef struct VkrWorldPassPayload VkrWorldPassPayload;
typedef struct VkrShadowPassPayload VkrShadowPassPayload;
typedef struct VkrSkyPassPayload VkrSkyPassPayload;
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
vkr_internal INLINE bool8_t vkr_rg_image_handle_valid(VkrRgImageHandle h) {
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
vkr_internal INLINE bool8_t vkr_rg_buffer_handle_valid(VkrRgBufferHandle h) {
  return h.id != 0;
}

Array(VkrRgImageHandle);
Array(VkrRgBufferHandle);

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
  /** Owned buffers retain completed backing capacity when demand decreases. */
  VKR_RG_RESOURCE_FLAG_GROW_ONLY = 1 << 9,
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
  uint32_t depth;  /**< Image depth; one for non-3D textures */
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
      .depth = 1,                                                              \
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

Array(VkrRgImageUse);

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
 * @brief Layout of one image subresource after the compiled frame.
 *
 * Returns VKR_TEXTURE_LAYOUT_UNDEFINED when no pass touches the subresource or
 * the handle, mip or layer is out of range. A capture of one layer or mip uses
 * this rather than VkrRgImage.final_layout, which follows subresource 0.
 */
VkrTextureLayout
vkr_rg_image_final_subresource_layout(const struct VkrRenderGraph *graph,
                                      VkrRgImageHandle handle, uint32_t mip,
                                      uint32_t layer);

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

Array(VkrRgBufferUse);

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

Array(VkrRgAttachment);

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
  Array_VkrRgAttachment color_attachments; /**< Color attachments in order */
  bool8_t has_depth_attachment; /**< True if depth_attachment is used */
  VkrRgAttachment
      depth_attachment; /**< Depth attachment (valid if has_depth_attachment) */

  Array_VkrRgImageUse image_reads;    /**< Image read uses */
  Array_VkrRgImageUse image_writes;   /**< Image write uses */
  Array_VkrRgBufferUse buffer_reads;  /**< Buffer read uses */
  Array_VkrRgBufferUse buffer_writes; /**< Buffer write uses */

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
/** @brief Buffer counterpart to vkr_rg_find_image. */
VkrRgBufferHandle vkr_rg_find_buffer(const struct VkrRenderGraph *graph,
                                     String8 name);

/**
 * @brief Named typed pass executor resolved once when a JSON graph is loaded.
 */
typedef struct VkrRgPassExecutor {
  String8 name;       /**< Executor name (used for lookup) */
  uint32_t id;        /**< Non-zero backend-local identity */
  VkrRgPassType type; /**< Only this pass type may select the executor */
} VkrRgPassExecutor;

Array(VkrRgPassExecutor);

/**
 * @brief Registry of typed named pass executors resolved when JSON is loaded.
 * allocator is used for entries and must outlive the registry.
 */
typedef struct VkrRgExecutorRegistry {
  VkrAllocator *allocator; /**< Allocator for entries; must outlive registry */
  Array_VkrRgPassExecutor entries; /**< Registered executors */
  bool8_t initialized;             /**< True after init */
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
  /** The viewport at dynamic resolution's largest scale, which the tiled
      temporal upscaler's inputs hold; the viewport without it. */
  uint32_t viewport_capacity_width;
  uint32_t viewport_capacity_height;
  float32_t render_scale; /**< Internal Scene extent / Scene output extent. */
  bool8_t editor_enabled; /**< Whether editor is enabled */
  bool8_t scene_rendering;
  bool8_t skinning_enabled;
  uint32_t skinning_vertex_capacity;
  bool8_t animation_preview_enabled;
  /** Without the editor: the UI pass has draws, or no scene pass writes the
   * swapchain before it. */
  bool8_t ui_fullscreen_enabled;
  bool8_t post_transform_cache_enabled;
  /** SMAA antialiases the display-linear image before the final pass. */
  bool8_t smaa_enabled;
  /** The camera's visibility raster takes two samples a pixel (SMAA S2x);
      a split pass hands sample zero to the single-sample consumers. */
  bool8_t msaa_enabled;
  /** FSR 3.1 native antialiasing replaces SMAA and T2x on this frame. */
  bool8_t fsr31_enabled;
  /** The frame draws a planar reflector's mirrored view (desktop pipeline)
      into an image `planar_reflection_divisor` times smaller than the
      viewport on each axis, inside the half-size reflection target. */
  bool8_t planar_reflection_active;
  uint32_t planar_reflection_divisor;
  /** The frame updates the diffuse volume's dynamic layer (ADR-119). */
  bool8_t dynamic_volume_active;
  /** The frame traces ray-query sun shadows, one ray per 2x2 block, or one
      per pixel at `sun_ray_trace_divisor` 1. */
  bool8_t sun_ray_shadows_active;
  /** The sun-ray trace grid's divisor of the viewport: 2 at half rate, 1 at
      full rate (graph extent divisor source "sun_ray_trace_divisor"). */
  uint32_t sun_ray_trace_divisor;
  /** Deferred lighting marches camera depth toward the sun for contacts
      beyond the rays' cover. */
  bool8_t sun_contact_shadows;
  /** The frame builds its TLAS: for the dynamic layer or sun rays. */
  bool8_t ray_scene_active;
  bool8_t clearcoat_enabled;
  bool8_t sheen_enabled;
  bool8_t anisotropy_enabled;
  /** The scene's lightmaps hold its static lamps in planes the desktop
      pipeline samples (ADR-104): the G-buffer resolve
      writes their irradiance and deferred lighting adds it. */
  bool8_t baked_lamps_enabled;
  /* The frame binds a sparse diffuse volume, which DiffuseVolume.Sample
     looks up per pixel for deferred lighting. */
  bool8_t diffuse_volume_enabled;
  /** A scene material uses a BRDF extension (ADR-113): the frame takes the
   * extension shading variants. */
  bool8_t brdf_extensions_enabled;
  /** Any of clearcoat, sheen, anisotropy, a BRDF extension or
   * `local_shadow_mask_overflow`: deferred lighting splits its tiles between a
   * base kernel and the layered pass, which runs the layered kernel for layer
   * data and the fallback kernel for mask overflows. */
  bool8_t lighting_layers_enabled;
  bool8_t editor_image_available;
  bool8_t editor_overlay_enabled;
  /* The editor draws a selection mask and outlines its edge. */
  bool8_t editor_selection_enabled;
  /* The editor draws its ground grid over the Scene image. */
  bool8_t editor_grid_enabled;
  /* The Scene image goes to the presenter, which composites it with the UI
     on its own queue (ADR-111); no pass of this frame writes the swapchain. */
  bool8_t editor_presenter;
  uint32_t editor_image_width;
  uint32_t editor_image_height;
  /** True only when a completion-protected HZB history generation is valid. */
  bool8_t hzb_history_valid;
  /** Build HZB only when its raster grid can support the reuse policy. */
  bool8_t hzb_build_enabled;
  /** The camera's culling reads an HZB history: one is built this frame, or
      a frozen culling camera keeps the one built where it stood (ADR-127).
      Shared preparation sets it to hzb_build_enabled; a backend that keeps
      a frozen history sets it alone. */
  bool8_t hzb_read_enabled;
  /** The tiled pipeline culls the camera in two phases (ADR-032): draws an
      earlier frame's pyramid hides wait for the late view, which tests them
      against the pyramid of this frame's early depth and draws the ones it
      shows. A backend sets it; shared preparation leaves it false. */
  bool8_t tiled_two_phase_occlusion;
  /** Metal P6 occupied-depth feedback is enabled for this packet. */
  bool8_t sdsm_enabled;
  /** Number of graph-authored reductions after the HZB base mip. */
  uint32_t hzb_reduce_pass_count;
  /** Bounded rough-transmission reductions after the opaque base mip. */
  uint32_t transmission_rough_mip_pass_count;
  /** True when the packet contains transmissive world work. */
  bool8_t transmission_pending;
  /** Current draw-table strides; native GROW_ONLY backing may be larger.
   * The desktop pipeline strides each culling view's rows by the candidate
   * capacity; the tiled pipeline by the visible capacity. */
  uint32_t gpu_draw_candidate_capacity;
  uint32_t gpu_draw_visible_capacity;
  uint32_t transmission_gpu_draw_candidate_capacity;
  /** True only when a focused capture requests the post-layer-3 peel. */
  bool8_t transmission_depth_diagnostic_enabled;
  /** True when the debug payload asks for pass Debug.GpuSpike. */
  bool8_t debug_gpu_spike;
  /** True when the debug payload asks for the draw census of a scene frame
   * with GPU draw candidates (VkrGpuDebugPayload.draw_census): pass
   * Cull.Census and its buffers exist only then. */
  bool8_t gpu_draw_census;
  /** True when pass Editor.PassOutput reads `pass_output_image` whole and
   * shows it over the editor's Scene image (VkrGpuDebugPayload.pass_output);
   * its executor reads the source's layer and mip 0. */
  bool8_t editor_pass_output;
  String8 pass_output_image;
  /** True only when capture requests the resolve emissive diagnostic. */
  bool8_t deferred_emissive_capture_enabled;
  /** True only when capture requests resolve barycentrics and LOD. */
  bool8_t resolve_barycentric_lod_capture_enabled;
  /**
   * True only for an automatic-exposure packet. Manual frames must not pay for
   * metering, so both exposure passes and both exposure buffers are gated here
   * rather than being executed and discarded.
   */
  bool8_t exposure_automatic;
  /**
   * True when deferred lighting measures per-light contribution this frame:
   * a scene frame with point lights whose index is a multiple of
   * VKR_LOCAL_LIGHT_CONTRIBUTION_PERIOD.
   */
  bool8_t light_contribution_enabled;
  /**
   * True only for a frame that both requests bloom and has a viewport large
   * enough for a chain. Gates both bloom images and every bloom pass, so a
   * frame without bloom pays nothing for it.
   */
  bool8_t bloom_enabled;
  bool8_t dof_enabled;
  bool8_t motion_blur_enabled;
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
  bool8_t subsurface_enabled;
  bool8_t fog_enabled;
  bool8_t froxel_fog_enabled;
  /** True when the sky has an enabled atmosphere: the frame builds its
      sky-view lookup and aerial-perspective volume. */
  bool8_t atmosphere_enabled;
  /** True when aerial perspective applies to surfaces this frame. */
  bool8_t aerial_perspective_enabled;
  /** True when the cloud layer renders this frame: it traces the layer and
      its sun-projected shadow map (ADR-074). */
  bool8_t clouds_enabled;
  /** True when the in-place opaque pass applies analytic fog or aerial
      perspective. Froxel application owns the latter under froxel fog. */
  bool8_t fog_apply_enabled;
  /** Metal P19 state; false only for the diagnostic full-screen rollback. */
  bool8_t transmission_compact_enabled;
  /** True when this frame requests backend pass timestamps. */
  bool8_t timing_enabled;
  /** True when this frame requests its whole-frame GPU time. */
  bool8_t submission_timing_enabled;
  /** True when this frame requests each pass's shader invocation counts. */
  bool8_t pipeline_statistics_enabled;
  /**
   * Whether this frame's packet requests a pick. Gates the picking resources
   * and passes so a non-picking frame pays nothing for them.
   */
  bool8_t picking_pending;
  /** The world has ordinary-blend draws, which include the tiled pipeline's
   * glass, or world text. */
  bool8_t blend_pending;
  /** The tiled pipeline's blend has glass with a volume in view, whose
   * refraction samples a reduced copy of the scene (ADR-087). */
  bool8_t tiled_refraction_pending;
  /** The tiled pipeline reconstructs its Halton-jittered scene with
   * MetalFX's temporal scaler at the Scene output extent. */
  bool8_t tiled_temporal_enabled;
  /** The viewport is smaller than its capacity: the upscaler's inputs are
      copied into capacity-sized images so MetalFX keeps one scaler and its
      history across dynamic-resolution steps. */
  bool8_t tiled_temporal_staged;
  /** The tiled upscaler's frame has dynamic draws, whose motion and
      transform history it records. */
  bool8_t tiled_object_motion_pending;
  /** The upscaled frame's camera is still (VkrPreparedFrame). */
  bool8_t tiled_temporal_still;
  /** The tiled opaque pass writes the sun's light and its cascade
      visibility to `tiled_sun_light`, and the atmosphere draw corrects the
      sun's light by sun contact shadows, SDF sun shadows or both
      (VkrPreparedFrame). */
  bool8_t tiled_sun_light;
  /** `Tiled.SunShadow` traces SDF sun shadows into `tiled_sun_shadow`. */
  bool8_t tiled_sun_sdf;
  /** `Tiled.SunShafts` blurs the sky toward the sun into `tiled_sun_shafts`,
      which the atmosphere draw adds as in-scattered sun light
      (VkrPreparedFrame). */
  bool8_t tiled_sun_shafts;
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
  /** The world has dynamic casters: the shared static cascade image exists
   * and cascades may compose their layer from it (ADR-041). */
  bool8_t shadow_cascade_static_active;
  /** The cascade the static image's first layer holds; the image keeps the
   * shadow map's layers from that cascade on. */
  uint32_t shadow_static_first_cascade;
  /** Cascades whose static layer draws, whose static layer is copied into
   * their shadow-map layer, and whose copy takes the dynamic casters. */
  uint32_t shadow_cascade_static_render_mask;
  uint32_t shadow_cascade_copy_mask;
  uint32_t shadow_cascade_dynamic_render_mask;
  /** A far level exists (ADR-041): its pending image is kept; while
   * `shadow_far_slice_active` its cascade's culling view draws one slice of
   * the rebuilt static layer into it, and a bit of `shadow_far_publish_mask`
   * copies the finished image into that cascade's static layer. */
  bool8_t shadow_far_active;
  uint32_t shadow_far_cascade;
  bool8_t shadow_far_slice_active;
  /** The slice is the rebuild's first, whose pass clears the whole image;
   * later slices load it, since the retained image holds the earlier
   * slices. */
  bool8_t shadow_far_slice_first;
  uint32_t shadow_far_publish_mask;
  /** Dynamic-caster culling views after the cascade views: the cascade
   * count while a dynamic draw over a copied layer runs, else zero. */
  uint32_t shadow_cascade_dynamic_view_count;
  /** Far cascades keep EVSM moments, one layer per cascade from
   * VKR_SHADOW_EVSM_FIRST_CASCADE up to the far level, which keeps none;
   * the render mask shifted to those layers selects the moments a redrawn
   * cascade rebuilds. */
  bool8_t shadow_evsm_active;
  uint32_t shadow_moments_layer_count;
  uint32_t shadow_moments_render_mask;
  /** Local shadow views receivers sample. */
  uint32_t local_shadow_view_count;
  /** Transmission array layers receivers sample; zero without refractive
   * casters or before any transmission layer is drawn. */
  uint32_t local_shadow_transmission_layer_count;
  /** Opaque local faces exclude refractive casters. */
  bool8_t local_shadow_refractive_casters;
  /** Shadow.LocalMask runs its contact-shadow variant. */
  bool8_t local_shadow_contact;
  /** Shadowed lights of the table whose range reaches the camera frustum, a
   * bit per light. Only these can be in range of a visible pixel, so
   * Shadow.LocalMask traverses only them; the desktop pipeline only. */
  VkrPointLightMask local_shadow_mask_lights;
  /** Shadow.LocalMask runs: a light of `local_shadow_mask_lights` or a
   * baked lamp of `local_shadow_baked_lamp_count` needs it. */
  bool8_t local_shadow_mask_active;
  /** A light of `local_shadow_mask_lights` can leave a pixel more shadowed
   * lights than mask slots. The fallback or layered lighting kernel then
   * shades the tiles Shadow.LocalMask marks, since only they filter the
   * lights past the last slot inline. */
  bool8_t local_shadow_mask_overflow;
  /** Faces drawn this frame, one opaque culling view and pass each. */
  uint32_t local_shadow_render_count;
  /** The first render slots also draw transmission layers, with one
   * main-stream culling view each selecting only refractive casters. */
  uint32_t local_shadow_transmission_render_count;
  /** Per render slot: the atlas layer and transmission layer it draws. */
  uint32_t
      local_shadow_render_atlas_layers[VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX];
  uint32_t local_shadow_render_transmission_layers
      [VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX];
  /** Render slots that draw static squares, then those that draw dynamic
   * squares over copies of static ones; they sum to the render count. */
  uint32_t local_shadow_static_render_count;
  uint32_t local_shadow_dynamic_render_count;
  /** Baked lamps whose moving casters' shadows Shadow.LocalMask writes and
   * deferred lighting subtracts from their baked light; the desktop
   * pipeline only. */
  uint32_t local_shadow_baked_lamp_count;
  /** Per dynamic slot: the atlas layer of the static square it copies. */
  uint32_t
      local_shadow_copy_source_layers[VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX];
  /** Atlas layers cleared whole before local-shadow faces draw. */
  uint32_t local_shadow_atlas_clear_mask;
  uint32_t local_shadow_atlas_layer_count;
  uint32_t local_shadow_map_size;
  uint32_t local_shadow_transmission_map_size;
  /** Transmission array layers: the configured face budget. */
  uint32_t local_shadow_map_layer_count;
} VkrRenderGraphFrameInfo;

/*
 * GPU culling views, in order: the camera; one view per cascade, which draws
 * the cascade whole or its static layer; one dynamic-caster view per cascade
 * while any cascade draws its dynamic casters over a copy; the drawn local
 * faces; their transmission views; then a planar reflection's mirrored view;
 * the tiled pipeline's motion view; and its late occlusion view.
 */

/** Culling view of the dynamic casters of `cascade`'s composed layer. */
vkr_internal INLINE uint32_t vkr_render_graph_cascade_dynamic_view(
    const VkrRenderGraphFrameInfo *frame, uint32_t cascade) {
  return 1u + frame->shadow_cascade_count + cascade;
}

/** Cascade a directional culling view in [1, local first view) draws. */
vkr_internal INLINE uint32_t vkr_render_graph_view_cascade(
    const VkrRenderGraphFrameInfo *frame, uint32_t view) {
  return view > frame->shadow_cascade_count
             ? view - 1u - frame->shadow_cascade_count
             : view - 1u;
}

vkr_internal INLINE uint32_t
vkr_render_graph_local_shadow_first_view(const VkrRenderGraphFrameInfo *frame) {
  return 1u + frame->shadow_cascade_count +
         frame->shadow_cascade_dynamic_view_count;
}

/** One axis of the mirrored view's drawn extent: the viewport's over the
    reflector's divisor, rounded down as graph extents are, inside the
    half-size target. */
vkr_internal INLINE uint32_t vkr_render_graph_reflection_extent(
    const VkrRenderGraphFrameInfo *frame, uint32_t viewport_extent) {
  return Max(viewport_extent / Max(frame->planar_reflection_divisor, 1u), 1u);
}

/** The planar reflection's culling view, after the transmission views. */
vkr_internal INLINE uint32_t
vkr_render_graph_reflection_view(const VkrRenderGraphFrameInfo *frame) {
  return vkr_render_graph_local_shadow_first_view(frame) +
         frame->local_shadow_render_count +
         frame->local_shadow_transmission_render_count;
}

/** The MetalFX upscaler's camera view of the dynamic draws, after
    every other culling view: the object motion pass redraws them. */
vkr_internal INLINE uint32_t
vkr_render_graph_motion_view(const VkrRenderGraphFrameInfo *frame) {
  return vkr_render_graph_reflection_view(frame) +
         (frame->planar_reflection_active ? 1u : 0u);
}

/** The tiled pipeline's late occlusion view, last of all: the camera's
    draws that an earlier frame's pyramid deferred, confirmed against this
    frame's (ADR-032). */
vkr_internal INLINE uint32_t
vkr_render_graph_late_view(const VkrRenderGraphFrameInfo *frame) {
  return vkr_render_graph_motion_view(frame) +
         (frame->tiled_temporal_enabled ? 1u : 0u);
}

vkr_internal INLINE uint32_t
vkr_render_graph_gpu_draw_view_count(const VkrRenderGraphFrameInfo *frame) {
  return vkr_render_graph_late_view(frame) +
         (frame->tiled_two_phase_occlusion ? 1u : 0u);
}

/**
 * Directional culling views whose pass does not run this frame. They still
 * classify, so a retained cascade keeps counting its casters, but encode no
 * commands (ADR-041). A cascade view draws for a whole cascade, its static
 * layer or a far slice; a dynamic view draws over a copied layer.
 */
vkr_internal INLINE uint64_t
vkr_render_graph_cascade_idle_view_mask(const VkrRenderGraphFrameInfo *frame) {
  const uint32_t drawn =
      frame->shadow_cascade_render_mask |
      frame->shadow_cascade_static_render_mask |
      (frame->shadow_far_slice_active ? UINT32_C(1) << frame->shadow_far_cascade
                                      : 0u);
  uint64_t mask = 0u;
  for (uint32_t cascade = 0u; cascade < frame->shadow_cascade_count;
       ++cascade) {
    const uint32_t bit = UINT32_C(1) << cascade;
    if ((drawn & bit) == 0u)
      mask |= UINT64_C(1) << (1u + cascade);
    if (cascade < frame->shadow_cascade_dynamic_view_count &&
        (frame->shadow_cascade_dynamic_render_mask & bit) == 0u)
      mask |=
          UINT64_C(1) << vkr_render_graph_cascade_dynamic_view(frame, cascade);
  }
  return mask;
}

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
 * @brief Adds live instances of one graph image. Bytes cover every mip, layer
 * and sample; block-compressed or unknown formats are not counted.
 */
void vkr_render_graph_resource_stats_add_image(
    VkrRenderGraphResourceStats *stats, const VkrRgImageDesc *desc,
    uint32_t instance_count);

/** @brief Adds live instances of one graph buffer. */
void vkr_render_graph_resource_stats_add_buffer(
    VkrRenderGraphResourceStats *stats, const VkrRgBufferDesc *desc,
    uint32_t instance_count);

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

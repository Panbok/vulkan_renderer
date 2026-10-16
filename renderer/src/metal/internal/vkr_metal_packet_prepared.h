#pragma once

/* Native pass inputs borrow graph resources and acquired upload bytes until
 * recording ends. The graph frame arena releases the CPU records at end_frame.
 */
typedef struct VkrMetalPacketDispatch {
  id<MTLComputePipelineState> pipeline;
  uint64_t root;
  MTLSize threads;
  MTLSize group_size;
} VkrMetalPacketDispatch;

typedef struct VkrMetalPacketDirectPass {
  id<MTLRenderPipelineState> pipeline;
  /* The tiled blend pass's extension variant for extended draws, nil
     otherwise (ADR-113). */
  id<MTLRenderPipelineState> extended_pipeline;
  id<MTLDepthStencilState> depth;
  const VkrMetalPacketPreparedDraw *draws;
  uint32_t count;
  uint32_t width;
  uint32_t height;
  uint64_t root_gpu;
} VkrMetalPacketDirectPass;

typedef struct VkrMetalPacketTextPass {
  id<MTLRenderPipelineState> pipeline;
  id<MTLDepthStencilState> depth;
  uint32_t count;
  uint32_t width;
  uint32_t height;
  const uint8_t *root_cpu;
  uint64_t root_gpu;
} VkrMetalPacketTextPass;

typedef struct VkrMetalPacketUiPass {
  uint32_t count;
  uint32_t width;
  uint32_t height;
  const uint8_t *root_cpu;
  uint64_t root_gpu;
} VkrMetalPacketUiPass;

typedef struct VkrMetalPacketIndirectPass {
  id<MTLIndirectCommandBuffer> commands;
  id<MTLRenderPipelineState> pipeline;
  id<MTLRenderPipelineState> opaque_pipeline;
  uint64_t arguments;
  MTLViewport viewport;
  VkrShadowConfigOverride depth_bias;
  /* Local shadow faces clear only their atlas square before drawing. */
  MTLScissorRect clear_rect;
  bool8_t clear_depth;
  /* Object motion: the previous frame's history may still be in flight. */
  bool8_t history_in_flight;
  /* A camera pass's Custom graph ranges (the frame's
     VkrMetalCustomCompaction) and the slots that draw them, bit slot - 1;
     zero without Custom graphs (ADR-096). */
  uint64_t custom_arguments;
  uint32_t custom_ready_mask;
  /* Plane 1's slot 0 holds the extended Standard draws this frame, and the
     Custom slots in `brdf_ready_mask` (bit slot - 1) their extended draws
     (ADR-113). */
  bool8_t brdf_ready;
  uint32_t brdf_ready_mask;
} VkrMetalPacketIndirectPass;

typedef struct VkrMetalPacketGpuEncodeGroup {
  id<MTLIndirectCommandBuffer> commands;
  uint64_t root;
  uint64_t arguments;
  uint32_t first_view;
  uint32_t view_count;
} VkrMetalPacketGpuEncodeGroup;

typedef struct VkrMetalPacketBufferCopy {
  id<MTLBuffer> source;
  id<MTLBuffer> destination;
  uint64_t source_offset;
  uint64_t destination_offset;
  uint64_t size;
} VkrMetalPacketBufferCopy;

/* Two candidate ranges, each with its candidate and instance rows, then the
   zeroed compaction state. */
/** A region copy between slices of one texture or of two textures of the
 * same format. */
typedef struct VkrMetalPacketTextureCopy {
  id<MTLTexture> source;
  id<MTLTexture> destination;
  NSUInteger source_slice;
  NSUInteger destination_slice;
  MTLOrigin source_origin;
  MTLOrigin destination_origin;
  MTLSize size;
} VkrMetalPacketTextureCopy;

typedef struct VkrMetalPacketTransferPass {
  VkrMetalPacketBufferCopy buffers[5];
  uint32_t buffer_count;
  /* A shadow layer or atlas square, or the tiled upscaler's three inputs. */
  VkrMetalPacketTextureCopy textures[3];
  uint32_t texture_count;
} VkrMetalPacketTransferPass;

typedef struct VkrMetalPacketPreparedPass {
  const VkrRgPass *source;
  VkrRgExecutorKind kind;
  NSString *label;
  uint32_t index;
  float64_t preparation_cpu_ms;
  bool8_t active;
  bool8_t last_present_writer;
  /** The last pass this frame that writes the shadow map. */
  bool8_t last_shadow_writer;
  VkrMetalDependency *consumers;
  VkrMetalDependency *producers;
  uint32_t consumer_count;
  uint32_t producer_count;
  union {
    VkrMetalPacketDispatch dispatch;
    struct {
      uint64_t roots[VKR_SKINNING_BINDING_CAPACITY];
      uint32_t vertices[VKR_SKINNING_BINDING_CAPACITY];
      uint32_t count;
    } skinning;
    struct {
      uint64_t root;
      MTLSize groups;
    } histogram;
    struct {
      VkrMetalPacketDispatch dispatch;
      /* The history's producer was not known complete at preparation. */
      bool8_t history_in_flight;
    } cloud_trace;
    struct {
      uint64_t root;
      uint32_t extent;
    } cloud_sky_light;
    struct {
      uint64_t root;
      uint32_t probe_count;
    } diffuse_volume_compose;
    struct {
      VkrMetalPacketGpuEncodeGroup
          groups[VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX];
      uint32_t group_count;
      uint32_t candidate_count;
      /** Culling views of retained cascades, a bit each at their view index;
       * their commands are neither reset nor encoded. */
      uint64_t idle_view_mask;
    } gpu_encode;
    struct {
      VkrMetalPacketDirectPass direct;
      VkrMetalPacketTextPass text;
    } world;
    VkrMetalPacketUiPass ui;
    VkrMetalPacketIndirectPass indirect;
    /* The tiled pipeline's opaque pass: the camera's culled opaque draws
       and its clear sky draw's root (ADR-087). */
    struct {
      VkrMetalPacketIndirectPass draws;
      uint64_t sky_root;
      /* The forward variant, which picks each Custom graph's pipelines. */
      uint32_t decals;
      uint32_t probes;
      uint32_t lighting;
    } tiled;
    /* The tiled pipeline's atmosphere draw over the resolved image. */
    struct {
      uint64_t root;
      MTLViewport viewport;
    } tiled_atmosphere;
    /* MetalFX's inputs and output, the viewport the inputs hold and the
       frame's jitter in pixels (y down). */
    struct {
      uint32_t content_width;
      uint32_t content_height;
      id<MTLTexture> color;
      id<MTLTexture> depth;
      id<MTLTexture> motion;
      id<MTLTexture> output;
      float32_t jitter_x;
      float32_t jitter_y;
      float32_t pre_exposure;
      bool8_t reset;
    } tiled_temporal;
    struct {
      uint64_t root;
      MTLViewport viewport;
      MTLScissorRect scissor;
    } tonemap;
    VkrMetalPacketTransferPass transfer;
    /* Tiled.Stabilize: the previous mean may come from a command buffer
       still in flight. */
    struct {
      VkrMetalPacketDispatch dispatch;
      bool8_t history_in_flight;
    } stabilize;
    struct {
      id<MTLTexture> source;
      /* The picked pixel's resolved depth, copied after its object id. */
      id<MTLTexture> depth_source;
      id<MTLBuffer> destination;
      uint64_t destination_offset;
      MTLOrigin origin;
    } picking_readback;
    /* Editor.Present: the Scene image and the presenter image it fills. */
    struct {
      id<MTLTexture> source;
      id<MTLTexture> destination;
    } editor_present;
  };
} VkrMetalPacketPreparedPass;

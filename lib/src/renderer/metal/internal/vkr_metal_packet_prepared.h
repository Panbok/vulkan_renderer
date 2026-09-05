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
  uint64_t draw_root;
  uint64_t peel_root;
  MTLViewport viewport;
  VkrShadowConfigOverride depth_bias;
} VkrMetalPacketIndirectPass;

typedef struct VkrMetalPacketGpuEncodeGroup {
  id<MTLIndirectCommandBuffer> commands;
  uint64_t root;
  uint64_t arguments;
  uint32_t view_count;
} VkrMetalPacketGpuEncodeGroup;

typedef struct VkrMetalPacketBufferCopy {
  id<MTLBuffer> source;
  id<MTLBuffer> destination;
  uint64_t source_offset;
  uint64_t destination_offset;
  uint64_t size;
} VkrMetalPacketBufferCopy;

typedef struct VkrMetalPacketTextureCopy {
  id<MTLTexture> source;
  id<MTLTexture> destination;
  uint32_t source_slice;
  uint32_t source_level;
  uint32_t destination_slice;
  uint32_t destination_level;
  MTLSize extent;
} VkrMetalPacketTextureCopy;

typedef struct VkrMetalPacketTransferPass {
  VkrMetalPacketBufferCopy buffers[6];
  VkrMetalPacketTextureCopy textures[3];
  uint32_t buffer_count;
  uint32_t texture_count;
} VkrMetalPacketTransferPass;

typedef struct VkrMetalPacketPreparedPass {
  const VkrRgPass *source;
  VkrMetalPacketGraphExecutorKind kind;
  NSString *label;
  uint32_t index;
  float64_t preparation_cpu_ms;
  bool8_t active;
  bool8_t last_present_writer;
  VkrMetalDependency *consumers;
  VkrMetalDependency *producers;
  uint32_t consumer_count;
  uint32_t producer_count;
  union {
    VkrMetalPacketDispatch dispatch;
    struct {
      id<MTLComputePipelineState> pipeline;
      uint64_t root;
      uint64_t arguments;
      MTLSize threads;
      bool8_t indirect;
    } transmission;
    struct {
      uint64_t root;
      MTLSize extent;
    } compact;
    struct {
      uint64_t root;
      MTLSize groups;
    } histogram;
    struct {
      id<MTLComputePipelineState> pipeline;
      VkrMetalPacketGpuEncodeGroup
          groups[VKR_METAL_PACKET_GPU_DRAW_ICB_GROUP_COUNT_MAX];
      uint32_t group_count;
      uint32_t candidate_count;
    } gpu_encode;
    struct {
      uint64_t prefilter_roots[VKR_IBL_PREFILTER_MIP_COUNT];
      uint64_t sh_root;
    } ibl;
    struct {
      VkrMetalPacketDirectPass direct;
      VkrMetalPacketTextPass text;
    } world;
    VkrMetalPacketUiPass ui;
    VkrMetalPacketIndirectPass indirect;
    struct {
      uint64_t root;
      MTLViewport viewport;
      MTLScissorRect scissor;
    } tonemap;
    struct {
      id<MTLTexture> color;
      id<MTLTexture> depth;
      id<MTLTexture> motion;
      id<MTLTexture> output;
      uint32_t width;
      uint32_t height;
      Vec2 jitter;
      bool8_t reset;
    } metalfx;
    VkrMetalPacketTransferPass transfer;
    struct {
      id<MTLTexture> source;
      id<MTLBuffer> destination;
      uint64_t destination_offset;
      MTLOrigin origin;
    } picking_readback;
  };
} VkrMetalPacketPreparedPass;

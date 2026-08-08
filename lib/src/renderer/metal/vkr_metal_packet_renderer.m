#include "renderer/metal/vkr_metal_packet_renderer.h"

#if defined(PLATFORM_APPLE)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

#include "core/logger.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/metal/internal/vkr_metal_packet_waits.h"
#include "renderer/metal/vkr_metal_capture_ring.h"
#include "renderer/metal/vkr_metal_dependency.h"
#include "renderer/metal/vkr_metal_packet_abi.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_graph_internal.h"
#include "renderer/vkr_renderer_metrics.h"
#include "renderer/vkr_rg_json.h"

#include <stdio.h>
#include <stdlib.h>

enum {
  VKR_METAL_PACKET_TIMEOUT_MS = 5000,
  VKR_METAL_PACKET_MAX_COLOR_ATTACHMENTS = 8,
  VKR_METAL_PACKET_LOADER_SUBMESH_MAX = 4096,
  VKR_METAL_PACKET_MAX_TEXTURE_MIPS = 15,
  /* Four address modes on three axes, two min/mag filters, one canonical
     non-mipmapped key plus fifteen keys for each mip filter, and anisotropy
     on/off. Samplers remain alive with immutable material rows, so this cache
     covers the complete key domain rather than only simultaneous textures. */
  VKR_METAL_PACKET_SAMPLER_CACHE_CAPACITY =
      VKR_TEXTURE_REPEAT_MODE_COUNT * VKR_TEXTURE_REPEAT_MODE_COUNT *
      VKR_TEXTURE_REPEAT_MODE_COUNT * VKR_FILTER_COUNT * VKR_FILTER_COUNT *
      (1 + (VKR_MIP_FILTER_COUNT - 1) * VKR_METAL_PACKET_MAX_TEXTURE_MIPS) * 2,
};

_Static_assert(VKR_TEXTURE_MAX_DIMENSION ==
                   (1u << (VKR_METAL_PACKET_MAX_TEXTURE_MIPS - 1u)),
               "Metal sampler mip-domain bound must match texture limits");

static uint64_t vkr_metal_packet_align_up(uint64_t value, uint64_t alignment);

/** Converts the renderer's Vulkan-oriented clip-Y convention to Metal. */
static Mat4 vkr_metal_packet_clip_matrix(bool8_t convert, Mat4 matrix) {
  if (!convert)
    return matrix;
  matrix.elements[1] = -matrix.elements[1];
  matrix.elements[5] = -matrix.elements[5];
  matrix.elements[9] = -matrix.elements[9];
  matrix.elements[13] = -matrix.elements[13];
  return matrix;
}

typedef struct VkrMetalPacketImage {
  VkrMetalTextureResource resource;
  VkrRgImageDesc desc;
  uint32_t graph_generation;
  bool8_t live;
  bool8_t owned;
} VkrMetalPacketImage;

typedef struct VkrMetalPacketMesh {
  VkrMetalBufferResource vertices;
  VkrMetalBufferResource indices;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t submesh_count;
  uint32_t generation;
  uint64_t last_use_submit_value;
  bool8_t live;
} VkrMetalPacketMesh;

typedef struct VkrMetalPacketMaterial {
  VkrMetalTextureResource textures[4];
  VkrTextureHandle texture_handles[4];
  VkrMetalMaterialHandle row;
  VkrPbrProperties pbr;
  VkrMaterialAlphaMode alpha_mode;
  float32_t alpha_cutoff;
  uint32_t row_index;
  uint32_t generation;
  uint64_t last_use_submit_value;
  bool8_t owns_textures;
  bool8_t double_sided;
  bool8_t live;
} VkrMetalPacketMaterial;

typedef struct VkrMetalPacketSamplerKey {
  VkrTextureRepeatMode u_repeat_mode;
  VkrTextureRepeatMode v_repeat_mode;
  VkrTextureRepeatMode w_repeat_mode;
  VkrFilter min_filter;
  VkrFilter mag_filter;
  VkrMipFilter mip_filter;
  uint32_t mip_level_count;
  bool8_t anisotropy_enable;
} VkrMetalPacketSamplerKey;

typedef struct VkrMetalPacketSampler {
  VkrMetalPacketSamplerKey key;
  id<MTLSamplerState> state;
} VkrMetalPacketSampler;

typedef struct VkrMetalPacketTexture {
  VkrMetalTextureResource resource;
  uint64_t sampler_resource_id;
  uint32_t generation;
  uint64_t last_use_submit_value;
  bool8_t live;
} VkrMetalPacketTexture;

static VkrMetalPacketTexture *
vkr_metal_packet_resolve_texture(VkrMetalPacketRenderer *renderer,
                                 VkrTextureHandle handle);

typedef struct VkrMetalPacketTextUpload {
  uint64_t vertices_gpu;
  uint64_t indices_gpu;
  uint64_t indices_length;
} VkrMetalPacketTextUpload;

typedef struct VkrMetalPacketFrameUpload {
  VkrMetalRingSlice slice;
  VkrMetalAddressPair addresses;
  id<MTLBuffer> buffer;
  uint8_t *root_cpu;
  uint64_t root_gpu;
  uint64_t world_instances_gpu;
  uint64_t shadow_instances_gpu;
  uint64_t picking_instances_gpu;
  uint64_t editor_instances_gpu;
  uint64_t ui_instances_gpu;
  uint64_t point_light_data_gpu;
  uint64_t point_light_masks_gpu;
  uint64_t shadow_cascades_gpu;
  uint64_t shadow_texture_id;
  uint64_t transmission_texture_id;
  uint64_t ibl_probes_gpu;
  VkrMetalPacketTextUpload *text_uploads;
  uint32_t world_text_count;
  uint32_t ui_text_count;
  uint32_t root_capacity;
  uint32_t root_cursor;
  bool8_t acquired;
} VkrMetalPacketFrameUpload;

typedef struct VkrMetalPacketCapturePlan {
  id<MTLTexture> texture;
  VkrCaptureItemResult result;
  uint64_t buffer_offset;
  uint32_t source_slice;
} VkrMetalPacketCapturePlan;

typedef struct VkrMetalPacketCommandSlot {
  id<MTL4CommandAllocator> allocator;
  id<MTL4CommandBuffer> buffer;
  id<MTL4CounterHeap> timestamp_heap;
  VkrMetalPacketResult timing_result;
  uint64_t submit_value;
  uint32_t timestamp_entry_count;
  bool8_t timing_requested;
  bool8_t timing_collected;
} VkrMetalPacketCommandSlot;

struct VkrMetalPacketRenderer {
  VkrAllocator *allocator;
  Arena *graph_frame_arena;
  VkrAllocator graph_frame_allocator;
  VkrRgJsonGraph json_graph;
  VkrRenderGraph *graph;
  VkrRgExecutorRegistry executors;
  VkrMetalMemoryDevice *memory;
  VkrMetalMaterialTableDevice *materials;
  VkrMetalCaptureRing capture_ring;
  void *capture_storage;
  VkrMetalPacketImage *images;
  VkrMetalPacketMesh *meshes;
  VkrMetalPacketSubmeshCreateInfo *submeshes;
  VkrMetalPacketMaterial *packet_materials;
  VkrMetalPacketTexture *textures;
  VkrMetalPacketSampler *samplers;
  VkrMetalPacketTextUpload *text_uploads;
  MTL4RenderPassDescriptor **render_passes;
  id<MTLDevice> device;
  id<MTL4Compiler> compiler;
  id<MTL4PipelineDataSetSerializer> pipeline_serializer;
  id<MTL4Archive> pipeline_archive;
  MTL4CompilerTaskOptions *compiler_options;
  id<MTL4CommandQueue> queue;
  id<MTLSharedEvent> completion;
  VkrMetalPacketCommandSlot *command_slots;
  VkrMetalPacketResult *completed_timing_results;
  VkrMetalPacketCommandSlot *active_command_slot;
  uint32_t command_slot_count;
  uint32_t next_command_slot;
  uint32_t next_completed_timing;
  VkrMetalPacketWaitCounters wait_counters;
  /* Active-slot aliases keep encoding code independent of slot selection. */
  id<MTL4CommandAllocator> command_allocator;
  id<MTL4CommandBuffer> command_buffer;
  id<MTL4CounterHeap> timestamp_heap;
  uint64_t timestamp_frequency;
  id<MTLRenderPipelineState> shadow_pipeline;
  id<MTLRenderPipelineState> skybox_pipeline;
  id<MTLRenderPipelineState> opaque_pipeline;
  id<MTLRenderPipelineState> blend_pipeline;
  id<MTLRenderPipelineState> overlay_pipeline;
  id<MTLRenderPipelineState> picking_pipeline;
  id<MTLRenderPipelineState> tonemap_pipeline;
  id<MTLRenderPipelineState> world_text_pipeline;
  id<MTLRenderPipelineState> ui_text_pipeline;
  id<MTLRenderPipelineState> picking_text_pipeline;
  id<MTLComputePipelineState> ibl_irradiance_pipeline;
  id<MTLComputePipelineState> ibl_equirect_pipeline;
  id<MTLComputePipelineState> ibl_prefilter_pipeline;
  id<MTLComputePipelineState> ibl_brdf_pipeline;
  id<MTLDepthStencilState> depth_write_state;
  id<MTLDepthStencilState> depth_read_state;
  id<MTL4ArgumentTable> argument_table;
  CAMetalLayer *layer;
  id<CAMetalDrawable> drawable;
  VkrRenderGraphFrameInfo prepared_frame;
  uint64_t submit_value;
  uint32_t max_images;
  uint32_t max_passes;
  uint32_t max_meshes;
  uint32_t max_submeshes_per_mesh;
  uint32_t max_materials;
  uint32_t max_textures;
  uint32_t max_samplers;
  uint32_t sampler_count;
  uint32_t max_draws;
  uint32_t max_instances;
  uint64_t upload_slot_size;
  uint32_t resize_count;
  VkrMetalPacketTargetKind target_kind;
  bool8_t srgb_output;
  bool8_t convert_vulkan_clip_y;
  bool8_t frame_prepared;
  bool8_t pipeline_archive_warm;
  bool8_t pipeline_archive_written;
  VkrMetalTextureResource ibl_irradiance;
  VkrMetalTextureResource ibl_prefilter;
  VkrMetalTextureResource ibl_brdf;
  VkrTextureHandle ibl_source;
  uint64_t ibl_last_use_submit_value;
  bool8_t ibl_live;
  bool8_t ibl_ready;
  bool8_t synchronous_validation_readback;
};

/*
 * Private implementation units remain one Objective-C translation unit. This
 * keeps renderer state and static helpers private while making each lifetime
 * domain independently reviewable.
 */
// clang-format off
#include "renderer/metal/internal/vkr_metal_packet_graph.inc"
#include "renderer/metal/internal/vkr_metal_packet_commands.inc"
#include "renderer/metal/internal/vkr_metal_packet_setup.inc"
#include "renderer/metal/internal/vkr_metal_packet_resources.inc"
#include "renderer/metal/internal/vkr_metal_packet_frame.inc"
#include "renderer/metal/internal/vkr_metal_packet_lifecycle.inc"
// clang-format on

#endif

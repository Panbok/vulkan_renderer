#include "renderer/metal/vkr_metal_packet_renderer.h"

#if defined(PLATFORM_APPLE)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

#include "core/logger.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/metal/internal/vkr_metal_packet_waits.h"
#include "renderer/metal/vkr_metal_dependency.h"
#include "renderer/metal/vkr_metal_packet_abi.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_capture_ring.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_packet_constants.h"
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
  VKR_METAL_PACKET_GRAPH_INSTANCE_MAX = 8,
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

vkr_internal uint64_t vkr_metal_packet_align_up(uint64_t value,
                                                uint64_t alignment);

/** Converts the renderer's Vulkan-oriented clip-Y convention to Metal. */
vkr_internal Mat4 vkr_metal_packet_clip_matrix(bool8_t convert, Mat4 matrix) {
  if (!convert)
    return matrix;
  matrix.elements[1] = -matrix.elements[1];
  matrix.elements[5] = -matrix.elements[5];
  matrix.elements[9] = -matrix.elements[9];
  matrix.elements[13] = -matrix.elements[13];
  return matrix;
}

typedef struct VkrMetalPacketImageInstance {
  VkrMetalTextureResource resource;
  void *mip_views[VKR_METAL_PACKET_MAX_TEXTURE_MIPS];
  uint64_t last_use_submit_value;
  bool8_t live;
  bool8_t owned;
} VkrMetalPacketImageInstance;

typedef struct VkrMetalPacketImage {
  VkrMetalPacketImageInstance instances[VKR_METAL_PACKET_GRAPH_INSTANCE_MAX];
  VkrRgImageDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
  bool8_t external;
} VkrMetalPacketImage;

typedef struct VkrMetalPacketGraphBufferInstance {
  VkrMetalBufferResource resource;
  uint64_t last_use_submit_value;
  bool8_t live;
} VkrMetalPacketGraphBufferInstance;

typedef struct VkrMetalPacketGraphBuffer {
  VkrRgBufferDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
} VkrMetalPacketGraphBuffer;

typedef struct VkrMetalPacketMesh {
  VkrMetalBufferResource vertices;
  VkrMetalBufferResource indices;
  VkrGpuGeometryRow gpu_row;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t submesh_count;
  uint32_t generation;
  uint64_t last_use_submit_value;
  bool8_t live;
} VkrMetalPacketMesh;

typedef struct VkrMetalPacketGeometryMegabuffer {
  VkrMetalBufferResource vertices;
  VkrMetalBufferResource indices;
  uint64_t vertex_cursor;
  uint64_t index_cursor;
  uint64_t vertex_live_bytes;
  uint64_t index_live_bytes;
  uint64_t vertex_high_water;
  uint64_t index_high_water;
  uint64_t rejected_publications;
  uint64_t generation_replacements;
  uint32_t generation;
  bool8_t live;
} VkrMetalPacketGeometryMegabuffer;

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

vkr_internal VkrMetalPacketTexture *
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
  VkrCaptureRing capture_ring;
  void *capture_storage;
  /**
   * The capture ring is sized by capture_max_batch_bytes times the ring
   * capacity and reaches ~96 MiB at the production defaults — far larger than
   * the renderer arena, which is sized for per-frame graph work. It therefore
   * gets its own reservation, exactly as the bindless Vulkan renderer does,
   * instead of displacing every other renderer allocation.
   */
  VkrDMemory capture_storage_memory;
  VkrAllocator capture_storage_allocator;
  /** Backs every fixed-capacity record array; see the helpers below. */
  VkrDMemory record_memory;
  VkrAllocator record_allocator;
  VkrMetalPacketImage *images;
  VkrMetalPacketGraphBuffer *graph_buffers;
  VkrMetalPacketGraphBufferInstance *graph_buffer_instances;
  VkrMetalPacketMesh *meshes;
  VkrMetalPacketGeometryMegabuffer geometry_megabuffer;
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
  VkrTextureHandle ibl_source;
  uint64_t ibl_last_use_submit_value;
  bool8_t ibl_live;
  bool8_t ibl_ready;
  bool8_t synchronous_validation_readback;
};

/*
 * The fixed-capacity record arrays are owned by the engine rather than libc, so
 * their bytes enter the tagging and leak accounting ADR-006 and ADR-015 rely
 * on.
 *
 * They share one dedicated VkrDMemory reservation instead of the renderer
 * arena. The reason is size: max_meshes * max_submeshes_per_mesh is 16384 *
 * 512, so the submesh array alone is 96 MiB at the production defaults — larger
 * than the whole renderer arena, which is sized for per-frame graph work.
 * calloc hid this because the pages stayed untouched and therefore unbacked; a
 * committing allocator cannot. One reservation keeps the previous memory
 * behaviour, keeps these arrays from displacing every other renderer
 * allocation, and collapses teardown to a single destroy.
 *
 * The worst-case submesh product is the real problem and is worth revisiting;
 * that is a capacity decision, not a plumbing one, and is deliberately not made
 * here.
 */
vkr_internal void *vkr_metal_packet_alloc_zeroed(VkrAllocator *allocator,
                                                 uint64_t size) {
  if (!size)
    return NULL;
  void *memory =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (memory)
    MemZero(memory, size);
  return memory;
}

vkr_internal void vkr_metal_packet_free_sized(VkrAllocator *allocator,
                                              void *memory, uint64_t size) {
  if (memory && size)
    vkr_allocator_free(allocator, memory, size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

/** Tagged suballocation from the shared record reservation. */
vkr_internal void *vkr_metal_packet_record_alloc(VkrAllocator *records,
                                                 uint64_t size, bool8_t zero) {
  if (!size)
    return NULL;
  void *memory =
      vkr_allocator_alloc(records, size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (memory && zero)
    MemZero(memory, size);
  return memory;
}

vkr_internal bool8_t vkr_metal_packet_record_size_add(uint64_t *total,
                                                      uint64_t bytes) {
  if (!total || bytes > UINT64_MAX - (KB(4) - 1u))
    return false_v;
  const uint64_t aligned = AlignPow2(bytes, KB(4));
  if (aligned < bytes || aligned > UINT64_MAX - KB(4) ||
      *total > UINT64_MAX - aligned - KB(4))
    return false_v;
  *total += aligned + KB(4);
  return true_v;
}

#define VKR_METAL_PACKET_ARRAY_BYTES(NAME, MEMBER, COUNT_EXPR)                 \
  vkr_internal uint64_t NAME(const VkrMetalPacketRenderer *renderer) {         \
    return (uint64_t)(COUNT_EXPR) * sizeof(*renderer->MEMBER);                 \
  }

VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_images_bytes, images,
                             renderer->max_images)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_graph_buffers_bytes,
                             graph_buffers, renderer->max_images)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_graph_buffer_instances_bytes,
                             graph_buffer_instances,
                             (uint64_t)renderer->max_images *
                                 renderer->command_slot_count)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_meshes_bytes, meshes,
                             renderer->max_meshes)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_submeshes_bytes, submeshes,
                             (uint64_t)renderer->max_meshes *
                                 renderer->max_submeshes_per_mesh)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_materials_bytes, packet_materials,
                             renderer->max_materials)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_textures_bytes, textures,
                             renderer->max_textures)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_samplers_bytes, samplers,
                             renderer->max_samplers)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_text_uploads_bytes, text_uploads,
                             renderer->max_draws)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_render_passes_bytes,
                             render_passes, renderer->max_passes)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_command_slots_bytes,
                             command_slots, renderer->command_slot_count)
VKR_METAL_PACKET_ARRAY_BYTES(vkr_metal_packet_timing_results_bytes,
                             completed_timing_results,
                             renderer->command_slot_count)

#undef VKR_METAL_PACKET_ARRAY_BYTES

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

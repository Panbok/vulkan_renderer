#ifndef VKR_VULKAN_INTERNAL_H
#define VKR_VULKAN_INTERNAL_H

#include "renderer/vulkan/vkr_vulkan_renderer.h"

#include "core/logger.h"
#include "core/vkr_metrics.h"
#include "filesystem/filesystem.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_bloom.h"
#include "renderer/vkr_candidate_residency.h"
#include "renderer/vkr_capture_ring.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_slot_table.h"
#include "renderer/vkr_gpu_submit_ring.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_ibl_sh_pool.h"
#include "renderer/vkr_packet_constants.h"
#include "renderer/vkr_render_graph_internal.h"
#include "renderer/vkr_rg_json.h"
#include "renderer/vulkan/vkr_vulkan_dependency.h"
#include "renderer/vulkan/vkr_vulkan_memory.h"
#include "renderer/vulkan/vkr_vulkan_wsi.h"

#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "platform/vkr_platform.h"

#include <spirv_reflect.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VKR_VULKAN_PACKET_WORLD_VERT_SPV
#define VKR_VULKAN_PACKET_WORLD_VERT_SPV "packet.world.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_WORLD_TEMPORAL_VERT_SPV
#define VKR_VULKAN_PACKET_WORLD_TEMPORAL_VERT_SPV                              \
  "packet.world.temporal.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_WORLD_FRAG_SPV
#define VKR_VULKAN_PACKET_WORLD_FRAG_SPV "packet.world.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_WORLD_TEMPORAL_FRAG_SPV
#define VKR_VULKAN_PACKET_WORLD_TEMPORAL_FRAG_SPV                              \
  "packet.world.temporal.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_PICKING_FRAG_SPV
#define VKR_VULKAN_PACKET_PICKING_FRAG_SPV "packet.picking.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_FULLSCREEN_VERT_SPV
#define VKR_VULKAN_PACKET_FULLSCREEN_VERT_SPV "packet.fullscreen.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_FULLSCREEN_FRAG_SPV
#define VKR_VULKAN_PACKET_FULLSCREEN_FRAG_SPV "packet.fullscreen.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TEXT_VERT_SPV
#define VKR_VULKAN_PACKET_TEXT_VERT_SPV "packet.text.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TEXT_FRAG_SPV
#define VKR_VULKAN_PACKET_TEXT_FRAG_SPV "packet.text.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TEXT_PICKING_FRAG_SPV
#define VKR_VULKAN_PACKET_TEXT_PICKING_FRAG_SPV "packet.text_picking.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_IBL_EQUIRECT_COMP_SPV
#define VKR_VULKAN_PACKET_IBL_EQUIRECT_COMP_SPV "packet.ibl_equirect.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_IBL_PREFILTER_COMP_SPV
#define VKR_VULKAN_PACKET_IBL_PREFILTER_COMP_SPV "packet.ibl_prefilter.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_IBL_SH_COMP_SPV
#define VKR_VULKAN_PACKET_IBL_SH_COMP_SPV "packet.ibl_sh.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_VISIBILITY_VERT_SPV
#define VKR_VULKAN_PACKET_VISIBILITY_VERT_SPV "packet.visibility.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_VISIBILITY_FRAG_SPV
#define VKR_VULKAN_PACKET_VISIBILITY_FRAG_SPV "packet.visibility.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_VISIBILITY_OPAQUE_FRAG_SPV
#define VKR_VULKAN_PACKET_VISIBILITY_OPAQUE_FRAG_SPV                           \
  "packet.visibility_opaque.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_VISIBILITY_SHADOW_FRAG_SPV
#define VKR_VULKAN_PACKET_VISIBILITY_SHADOW_FRAG_SPV                           \
  "packet.visibility_shadow.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GPU_DRAW_CLASSIFY_COMP_SPV
#define VKR_VULKAN_PACKET_GPU_DRAW_CLASSIFY_COMP_SPV                           \
  "packet.gpu_draw_classify.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GPU_DRAW_PREFIX_COMP_SPV
#define VKR_VULKAN_PACKET_GPU_DRAW_PREFIX_COMP_SPV                             \
  "packet.gpu_draw_prefix.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GPU_DRAW_ENCODE_COMP_SPV
#define VKR_VULKAN_PACKET_GPU_DRAW_ENCODE_COMP_SPV                             \
  "packet.gpu_draw_encode.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GBUFFER_RESOLVE_COMP_SPV
#define VKR_VULKAN_PACKET_GBUFFER_RESOLVE_COMP_SPV                             \
  "packet.gbuffer_resolve.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TEMPORAL_TRANSFORM_COMP_SPV
#define VKR_VULKAN_PACKET_TEMPORAL_TRANSFORM_COMP_SPV                          \
  "packet.temporal_transform.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_DEFERRED_LIGHTING_COMP_SPV
#define VKR_VULKAN_PACKET_DEFERRED_LIGHTING_COMP_SPV                           \
  "packet.deferred_lighting.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TEMPORAL_RESOLVE_COMP_SPV
#define VKR_VULKAN_PACKET_TEMPORAL_RESOLVE_COMP_SPV                            \
  "packet.temporal_resolve.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_HZB_BUILD_COMP_SPV
#define VKR_VULKAN_PACKET_HZB_BUILD_COMP_SPV "packet.hzb_build.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_EXPOSURE_CLEAR_COMP_SPV
#define VKR_VULKAN_PACKET_EXPOSURE_CLEAR_COMP_SPV                              \
  "packet.exposure_clear.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_EXPOSURE_HISTOGRAM_COMP_SPV
#define VKR_VULKAN_PACKET_EXPOSURE_HISTOGRAM_COMP_SPV                          \
  "packet.exposure_histogram.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_EXPOSURE_RESOLVE_COMP_SPV
#define VKR_VULKAN_PACKET_EXPOSURE_RESOLVE_COMP_SPV                            \
  "packet.exposure_resolve.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_BLOOM_PREFILTER_COMP_SPV
#define VKR_VULKAN_PACKET_BLOOM_PREFILTER_COMP_SPV                             \
  "packet.bloom_prefilter.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_BLOOM_DOWNSAMPLE_TENT13_COMP_SPV
#define VKR_VULKAN_PACKET_BLOOM_DOWNSAMPLE_TENT13_COMP_SPV                     \
  "packet.bloom_downsample_tent13.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_BLOOM_DOWNSAMPLE_BOX4_COMP_SPV
#define VKR_VULKAN_PACKET_BLOOM_DOWNSAMPLE_BOX4_COMP_SPV                       \
  "packet.bloom_downsample_box4.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_BLOOM_UPSAMPLE_COMP_SPV
#define VKR_VULKAN_PACKET_BLOOM_UPSAMPLE_COMP_SPV                              \
  "packet.bloom_upsample.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_BLOOM_COMBINE_COMP_SPV
#define VKR_VULKAN_PACKET_BLOOM_COMBINE_COMP_SPV "packet.bloom_combine.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GTAO_DEPTH_PREFILTER_COMP_SPV
#define VKR_VULKAN_PACKET_GTAO_DEPTH_PREFILTER_COMP_SPV                        \
  "packet.gtao_depth_prefilter.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GTAO_DEPTH_MIP_COMP_SPV
#define VKR_VULKAN_PACKET_GTAO_DEPTH_MIP_COMP_SPV                              \
  "packet.gtao_depth_mip.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GTAO_EVALUATE_COMP_SPV
#define VKR_VULKAN_PACKET_GTAO_EVALUATE_COMP_SPV "packet.gtao_evaluate.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GTAO_DENOISE_COMP_SPV
#define VKR_VULKAN_PACKET_GTAO_DENOISE_COMP_SPV "packet.gtao_denoise.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SDSM_REDUCE_COMP_SPV
#define VKR_VULKAN_PACKET_SDSM_REDUCE_COMP_SPV "packet.sdsm_reduce.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_PICKING_RESOLVE_COMP_SPV
#define VKR_VULKAN_PACKET_PICKING_RESOLVE_COMP_SPV                             \
  "packet.picking_resolve.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_SHADE_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_SHADE_COMP_SPV                          \
  "packet.transmission_shade.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_COMP_SPV                        \
  "packet.transmission_compact.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_FINALIZE_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_FINALIZE_COMP_SPV               \
  "packet.transmission_compact_finalize.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_COVERAGE_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_COVERAGE_COMP_SPV                       \
  "packet.transmission_coverage.comp.spv"
#endif

enum {
  /**
   * Slot zero of every descriptor heap holds a valid sentinel descriptor: a
   * 1x1 opaque-white image or the default point-clamp sampler. A missing
   * optional resource therefore remains legal and branch-free in shader code.
   * Anything that means "no specific resource" must name this rather than
   * writing a bare 0.
   */
  VKR_VULKAN_SENTINEL_SLOT_INDEX = 0,
  VKR_VULKAN_SENTINEL_UPLOAD_SIZE = 4,
  VKR_VULKAN_SWAPCHAIN_IMAGE_MAX = 8,
  VKR_VULKAN_RETIRED_SWAPCHAIN_MAX = 8,
  VKR_VULKAN_GRAPH_LAYER_MAX = 16,
  VKR_VULKAN_TEXTURE_MIP_MAX = 16,
  VKR_VULKAN_PENDING_IBL_BAKE_MAX = 32,
  VKR_VULKAN_FRAME_UPLOAD_SIZE = 48u * 1024u * 1024u,
};

enum {
  VKR_VULKAN_DEFERRED_VIEW_COUNT_MAX = 1 + VKR_SHADOW_CASCADE_COUNT_MAX,
  VKR_VULKAN_READBACK_COLOR_SIZE = 4,
  VKR_VULKAN_READBACK_DRAW_STATE_OFFSET = 16,
  VKR_VULKAN_READBACK_TRANSMISSION_STATE_OFFSET =
      VKR_VULKAN_READBACK_DRAW_STATE_OFFSET +
      VKR_VULKAN_DEFERRED_VIEW_COUNT_MAX * sizeof(VkrGpuDrawCompactionState),
  VKR_VULKAN_SDSM_STATE_SIZE = 16,
  VKR_VULKAN_READBACK_SDSM_STATE_OFFSET =
      VKR_VULKAN_READBACK_TRANSMISSION_STATE_OFFSET +
      sizeof(VkrGpuTransmissionDiagnostics),
  VKR_VULKAN_READBACK_EXPOSURE_STATE_OFFSET =
      VKR_VULKAN_READBACK_SDSM_STATE_OFFSET + VKR_VULKAN_SDSM_STATE_SIZE,
  VKR_VULKAN_READBACK_EXPOSURE_HISTOGRAM_OFFSET =
      VKR_VULKAN_READBACK_EXPOSURE_STATE_OFFSET + sizeof(VkrExposureGpuState),
  VKR_VULKAN_READBACK_SIZE = VKR_VULKAN_READBACK_EXPOSURE_HISTOGRAM_OFFSET +
                             sizeof(VkrExposureGpuHistogram),
};

typedef enum VkrVulkanPacketPipeline {
  VKR_VULKAN_PACKET_PIPELINE_PICKING = 0,
  VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND,
  VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
  VKR_VULKAN_PACKET_PIPELINE_UI,
  VKR_VULKAN_PACKET_PIPELINE_WORLD_TEXT,
  VKR_VULKAN_PACKET_PIPELINE_PICKING_TEXT,
  VKR_VULKAN_PACKET_PIPELINE_UI_TEXT,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW_OPAQUE,
  VKR_VULKAN_PACKET_PIPELINE_COUNT,
} VkrVulkanPacketPipeline;

typedef enum VkrVulkanFullscreenFlag {
  VKR_VULKAN_FULLSCREEN_TONEMAP = 1u << 1u,
  VKR_VULKAN_FULLSCREEN_FXAA = 1u << 2u,
} VkrVulkanFullscreenFlag;

typedef enum VkrVulkanPacketShader {
  VKR_VULKAN_PACKET_SHADER_WORLD_VERTEX = 0,
  VKR_VULKAN_PACKET_SHADER_WORLD_TEMPORAL_VERTEX,
  VKR_VULKAN_PACKET_SHADER_WORLD_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_WORLD_TEMPORAL_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_PICKING_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_FULLSCREEN_VERTEX,
  VKR_VULKAN_PACKET_SHADER_FULLSCREEN_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_TEXT_VERTEX,
  VKR_VULKAN_PACKET_SHADER_TEXT_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_TEXT_PICKING_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_OPAQUE_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_SHADOW_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_COUNT,
} VkrVulkanPacketShader;

typedef enum VkrVulkanIblPipeline {
  VKR_VULKAN_IBL_PIPELINE_EQUIRECT = 0,
  VKR_VULKAN_IBL_PIPELINE_PREFILTER,
  /** L2 coefficient projection (ADR-038). */
  VKR_VULKAN_IBL_PIPELINE_SH,
  VKR_VULKAN_IBL_PIPELINE_COUNT,
} VkrVulkanIblPipeline;

typedef enum VkrVulkanDeferredPipeline {
  VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY = 0,
  VKR_VULKAN_DEFERRED_PIPELINE_PREFIX,
  VKR_VULKAN_DEFERRED_PIPELINE_ENCODE,
  VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_TRANSFORM,
  VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER,
  VKR_VULKAN_DEFERRED_PIPELINE_LIGHTING,
  VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_RESOLVE,
  VKR_VULKAN_DEFERRED_PIPELINE_HZB,
  VKR_VULKAN_DEFERRED_PIPELINE_SDSM,
  VKR_VULKAN_DEFERRED_PIPELINE_PICKING,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT_FINALIZE,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COVERAGE,
  VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_CLEAR,
  VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_HISTOGRAM,
  VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_RESOLVE,
  VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_PREFILTER,
  /**
   * Both downsample filters are created. Which one a frame binds is cold
   * configuration, so the comparison the design asks for is a config change
   * rather than a rebuild, and neither is a fallback for the other.
   */
  VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_DOWNSAMPLE_TENT13,
  VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_DOWNSAMPLE_BOX4,
  VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_UPSAMPLE,
  VKR_VULKAN_DEFERRED_PIPELINE_BLOOM_COMBINE,
  VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DEPTH_PREFILTER,
  VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DEPTH_MIP,
  VKR_VULKAN_DEFERRED_PIPELINE_GTAO_EVALUATE,
  VKR_VULKAN_DEFERRED_PIPELINE_GTAO_DENOISE,
  VKR_VULKAN_DEFERRED_PIPELINE_COUNT,
} VkrVulkanDeferredPipeline;

typedef enum VkrVulkanMaterialFlag {
  VKR_VULKAN_MATERIAL_TEXTURE_NORMAL = 1u << 0u,
  VKR_VULKAN_MATERIAL_TEXTURE_ORM = 1u << 1u,
  VKR_VULKAN_MATERIAL_TEXTURE_EMISSIVE = 1u << 2u,
} VkrVulkanMaterialFlag;

typedef struct VKR_SIMD_ALIGN VkrVulkanMaterialGpuRow {
  float32_t tint[4];
  uint32_t base_color_texture;
  uint32_t normal_texture;
  uint32_t orm_texture;
  uint32_t emissive_texture;
  uint32_t base_color_sampler;
  uint32_t normal_sampler;
  uint32_t orm_sampler;
  uint32_t emissive_sampler;
  uint32_t material_id;
  uint32_t flags;
  uint32_t alpha_mode;
  float32_t temporal_reactivity;
  Vec4 material_emissive;
  Vec4 material_dielectric_specular;
  Vec4 material_surface;
  Vec4 material_alpha;
  Vec4 material_attenuation_color;
} VkrVulkanMaterialGpuRow;

typedef struct VkrVulkanPushConstants {
  uint64_t root;
  uint32_t material_index;
  uint32_t flags;
} VkrVulkanPushConstants;

typedef struct VKR_SIMD_ALIGN VkrVulkanCullRoot {
  uint64_t candidates;
  uint64_t classifications;
  uint64_t visible;
  uint64_t states;
  uint64_t commands;
  uint64_t instances;
  uint64_t view_projections;
  uint64_t frustum_planes;
  uint32_t candidate_count;
  uint32_t view_count;
  uint32_t candidate_capacity;
  uint32_t command_partition_capacity;
  uint32_t hzb_textures[VKR_VULKAN_TEXTURE_MIP_MAX];
  uint32_t hzb_mip_count;
  uint32_t hzb_extent[2];
  uint32_t hzb_enabled;
  float32_t hzb_depth_epsilon;
  uint32_t camera_required_flags;
  uint32_t shadow_required_flags;
} VkrVulkanCullRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanRasterRoot {
  uint64_t geometry_rows;
  uint64_t visible_rows;
  uint64_t states;
  uint64_t frame;
  uint32_t view_index;
  uint32_t visible_capacity;
  uint32_t previous_depth_texture;
  uint32_t previous_depth_layer;
} VkrVulkanRasterRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanTemporalTransformRoot {
  uint64_t instances;
  uint64_t transforms;
  uint32_t instance_count;
  uint32_t transform_capacity;
  uint32_t frame_index;
  uint32_t reserved;
} VkrVulkanTemporalTransformRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanResolveRoot {
  uint64_t geometry_rows;
  uint64_t visible_rows;
  uint64_t reserved_address;
  uint64_t instances;
  uint64_t materials;
  uint64_t vertices;
  uint64_t indices;
  uint64_t compaction_state;
  Mat4 view_projection;
  Mat4 current_view_projection;
  Mat4 previous_view_projection;
  uint64_t previous_transforms;
  uint32_t vbuffer_texture;
  uint32_t albedo_texture;
  uint32_t specular_texture;
  uint32_t normal_texture;
  uint32_t emissive_texture;
  uint32_t debug_texture;
  uint32_t scene_texture;
  uint32_t motion_texture;
  uint32_t validity_texture;
  uint32_t extent[2];
  uint32_t visible_capacity;
  uint32_t geometry_count;
  uint32_t material_count;
  uint32_t instance_count;
  uint32_t render_mode;
  uint32_t history_valid;
  uint32_t previous_frame_index;
  uint32_t reserved_tail[2];
} VkrVulkanResolveRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanTemporalResolveRoot {
  uint64_t visible_rows;
  uint64_t instances;
  uint32_t scene_texture;
  uint32_t pre_transmission_texture;
  uint32_t motion_texture;
  uint32_t validity_texture;
  uint32_t depth_texture;
  uint32_t vbuffer_texture;
  uint32_t history_color_texture;
  uint32_t history_depth_texture;
  uint32_t history_identity_texture;
  uint32_t history_primitive_texture;
  uint32_t output_color_texture;
  uint32_t output_depth_texture;
  uint32_t output_identity_texture;
  uint32_t output_primitive_texture;
  uint32_t history_sampler;
  uint32_t extent[2];
  uint32_t history_valid;
  uint32_t render_mode;
  uint32_t camera_stationary;
  uint64_t transmission_visible_rows;
  uint64_t transmission_instances;
  uint32_t transmission_vbuffer_texture;
  uint32_t transmission_depth_texture;
  uint32_t transmission_enabled;
  uint32_t transmission_reserved;
} VkrVulkanTemporalResolveRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanLightingRoot {
  uint64_t frame;
  uint64_t frame_padding;
  Mat4 inverse_view_projection;
  uint32_t vbuffer_texture;
  uint32_t depth_texture;
  uint32_t albedo_texture;
  uint32_t specular_texture;
  uint32_t normal_texture;
  uint32_t scene_texture;
  uint32_t extent[2];
  /* Deferred lighting owns the background and samples the packet cubemap. */
  uint32_t sky_texture;
  uint32_t sky_sampler;
  uint32_t sky_enabled;
  uint32_t gtao_visibility_texture;
} VkrVulkanLightingRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanHzbRoot {
  uint32_t source_texture;
  uint32_t destination_texture;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t source_is_depth;
  uint32_t reserved[3];
} VkrVulkanHzbRoot;
typedef struct VKR_SIMD_ALIGN VkrVulkanSdsmRoot {
  uint64_t reduce_state;
  uint32_t depth_texture;
  uint32_t vbuffer_texture;
  uint32_t extent[2];
  uint32_t reserved[2];
} VkrVulkanSdsmRoot;

/** Mirrors VkrVkExposureRoot in shaders/vulkan/slang/post/exposure.slang. */
typedef struct VKR_SIMD_ALIGN VkrVulkanExposureRoot {
  uint64_t histogram;
  uint64_t state;
  /**
   * Newest completed adaptation record, or the current output instance when no
   * completed record exists. Always a real address: the resolve kernel reads it
   * unconditionally and discards the value through `history_valid`, so a null
   * here would be a dereference rather than a select.
   */
  uint64_t previous_state;
  uint32_t source_texture;
  uint32_t extent[2];
  uint32_t reset_reasons;
  VkrExposureGpuMetering metering;
} VkrVulkanExposureRoot;

/** Mirrors VkrVkBloomRoot in shaders/vulkan/slang/post/bloom.slang. */
typedef struct VKR_SIMD_ALIGN VkrVulkanBloomRoot {
  uint32_t source_texture;
  /**
   * Coarser accumulation level, written for the upsample pass only. At the
   * deepest step the accumulation level above has never been written, so the
   * executor points this at the downsample chain instead. Selecting on the CPU
   * is what keeps the kernel free of a bootstrap branch and keeps every
   * sampled texel defined.
   */
  uint32_t coarse_texture;
  uint32_t destination_texture;
  uint32_t source_sampler;
  /** Extent the tap offsets are expressed in; see the shader field comment. */
  uint32_t filter_extent[2];
  uint32_t destination_extent[2];
  VkrBloomGpuParams params;
} VkrVulkanBloomRoot;

/** Mirrors VkrVkGtaoRoot in shaders/vulkan/slang/post/gtao.slang. */
typedef struct VKR_SIMD_ALIGN VkrVulkanGtaoRoot {
  VkrGtaoGpuParams params;
  uint32_t source_texture;
  uint32_t vbuffer_texture;
  uint32_t normal_texture;
  uint32_t destination_texture;
  uint32_t edges_texture;
  uint32_t point_sampler;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t reserved[2];
} VkrVulkanGtaoRoot;

typedef struct VkrVulkanSdsmState {
  uint32_t min_device_z_bits;
  uint32_t max_device_z_bits;
  uint32_t occupied_count;
  uint32_t reserved;
} VkrVulkanSdsmState;

typedef struct VKR_SIMD_ALIGN VkrVulkanPickingRoot {
  uint64_t opaque_visible;
  uint64_t transmission_visible;
  uint64_t opaque_instances;
  uint64_t transmission_instances;
  uint32_t opaque_vbuffer;
  uint32_t transmission_vbuffer;
  uint32_t output_texture;
  uint32_t pixel[2];
  uint32_t transmission_layer;
  uint32_t use_transmission;
} VkrVulkanPickingRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanTransmissionRoot {
  uint64_t visible_rows;
  uint64_t materials;
  /* Offset 16 stays free of physical-buffer addresses; see the resolve root. */
  uint64_t reserved_address;
  uint64_t geometry_rows;
  uint64_t instances;
  uint64_t vertices;
  uint64_t indices;
  uint64_t compaction_state;
  uint64_t pixel_list;
  uint64_t compact_counts;
  uint64_t frame;
  uint32_t frame_address_padding[2];
  Mat4 view_projection;
  Mat4 inverse_view_projection;
  uint32_t vbuffer_texture;
  uint32_t depth_texture;
  uint32_t feedback_texture;
  uint32_t feedback_sampler;
  uint32_t output_texture;
  uint32_t layer;
  uint32_t extent[2];
  uint64_t previous_transforms;
  uint32_t previous_transform_address_padding[2];
  Mat4 current_view_projection;
  Mat4 previous_view_projection;
  uint32_t motion_texture;
  uint32_t validity_texture;
  uint32_t history_valid;
  uint32_t previous_frame_index;
  uint32_t visible_capacity;
  uint32_t geometry_count;
  uint32_t material_count;
  uint32_t instance_count;
  uint32_t pixel_capacity;
  uint32_t compact_layer;
  uint32_t compact_enabled;
  uint32_t reserved;
} VkrVulkanTransmissionRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanTransmissionCompactRoot {
  uint64_t pixel_list;
  uint64_t covered_pixels;
  uint64_t overflow_counts;
  uint64_t indirect_arguments;
  uint32_t vbuffer_texture;
  uint32_t source_texture;
  uint32_t destination_texture;
  uint32_t extent[2];
  uint32_t layer;
  uint32_t capacity;
  uint32_t reserved[5];
} VkrVulkanTransmissionCompactRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanTransmissionCoverageRoot {
  uint64_t covered_pixels;
  uint32_t vbuffer_texture;
  uint32_t layer;
  uint32_t extent[2];
  uint32_t reserved[2];
} VkrVulkanTransmissionCoverageRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanIblRoot {
  uint32_t source_texture;
  uint32_t source_sampler;
  uint32_t target_texture;
  uint32_t target_size;
  uint32_t sample_count;
  uint32_t source_face_size;
  uint32_t source_mip_count;
  float32_t roughness;
} VkrVulkanIblRoot;

/**
 * Root for the L2 coefficient projection dispatch (ADR-038 §2).
 *
 * `source_texture` and `source_sampler` address the published source cubemap;
 * the kernel samples the selected mip at exact texel centers. `destination` is
 * the device address of the single 112-byte slot this dispatch writes. The
 * three window scalars are the already-evaluated deringing factors: the CPU
 * owns `pow(sinc_pi(l/3), sh_deringing)` so the kernel carries no pow().
 */
typedef struct VKR_SIMD_ALIGN VkrVulkanIblShRoot {
  uint64_t destination;
  uint32_t source_texture;
  uint32_t source_sampler;
  uint32_t source_face_size;
  uint32_t source_mip;
  float32_t window_band_0;
  float32_t window_band_1;
  float32_t window_band_2;
  uint32_t reserved;
} VkrVulkanIblShRoot;

/** Mirrors VkrShadowCascadePacketData; see vkr_render_packet.h for units. */
typedef struct VKR_SIMD_ALIGN VkrVulkanPacketShadowCascade {
  Mat4 light_view_projection;
  Vec4 split_near_far_texel_depth;
  Vec4 origin_inv_size_pad;
} VkrVulkanPacketShadowCascade;

typedef struct VKR_SIMD_ALIGN VkrVulkanPacketIblProbe {
  /** ADR-038 final layout: offset 0 carries the coefficient slot, offset 4 is
      reserved, and every field from offset 8 onward keeps its previous offset.
   */
  uint32_t sh_slot;
  uint32_t sh_reserved;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  Vec4 center_blend;
  Vec4 extents_weight;
  Vec4 intensity_box;
} VkrVulkanPacketIblProbe;
typedef struct VKR_SIMD_ALIGN VkrVulkanPacketTemporalDrawState {
  uint64_t previous_transforms;
  uint32_t previous_transform_address_padding[2];
  Mat4 current_view_projection;
  Mat4 previous_view_projection;
  uint32_t history_valid;
  uint32_t previous_frame_index;
  uint32_t reserved[2];
} VkrVulkanPacketTemporalDrawState;

/** Values shared by every indexed draw recorded for one pass. */
typedef struct VKR_SIMD_ALIGN VkrVulkanPacketFrameRoot {
  uint64_t instances;
  uint32_t instance_address_padding[2];
  Mat4 view_projection;
  uint64_t materials;
  /** ADR-038 final layout: the retired diffuse-cubemap pair became the
      coefficient buffer address, and the retired BRDF pair became the global
      slot plus reserved word. Later offsets are unchanged. */
  uint64_t sh_coefficients;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  uint32_t sh_global_slot;
  uint32_t sh_reserved;
  uint32_t shadow_texture;
  uint32_t shadow_sampler;
  uint32_t transmission_texture;
  uint32_t transmission_sampler;
  Vec4 view_position;
  uint32_t prefilter_mip_count;
  uint32_t flags;
  uint32_t reserved_0[2];
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  Vec4 ambient_color;
  uint32_t render_mode;
  uint32_t shadow_debug_mode;
  uint32_t reserved_1[2];
  uint64_t point_light_data;
  uint64_t point_light_masks;
  Vec4 point_light_grid_origin_cell_size;
  uint32_t point_light_grid_dimensions_count[4];
  VkrPointLightMask point_light_global_mask;
  uint32_t point_light_count;
  uint32_t point_light_reserved[3];
  uint64_t shadow_cascades;
  Mat4 view;
  uint32_t shadow_cascade_count;
  /* Receiver quality, mirroring VkrShadowReceiverPacketData. Scalars rather
     than Vec4s so the layout matches the Metal root and the Slang mirror
     without padding holes. */
  uint32_t shadow_pcf_sample_count;
  float32_t shadow_receiver_bias_texels;
  float32_t shadow_slope_bias_texels;
  float32_t shadow_normal_offset_texels;
  float32_t shadow_pcf_radius_texels;
  float32_t shadow_cascade_blend_fraction;
  float32_t shadow_fade_start;
  float32_t shadow_fade_end;
  /** Comparison-sampler heap slot; `shadow_sampler` remains the sentinel. */
  uint32_t shadow_comparison_sampler;
  uint32_t shadow_pcf_uniform_early_out;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t ibl_probe_reserved;
  uint64_t temporal_draw_state;
} VkrVulkanPacketFrameRoot;

/** The only record written per indexed packet draw. */
typedef struct VKR_SIMD_ALIGN VkrVulkanPacketDrawRoot {
  uint64_t geometry_rows;
  uint64_t visible_rows;
  uint64_t vertices;
  uint64_t frame;
  uint32_t visible_row_index;
  uint32_t flags;
  uint32_t reserved[2];
} VkrVulkanPacketDrawRoot;

/** Non-world utility shaders retain a single-draw root because their model,
 * text controls, or source texture genuinely vary with that draw. */
typedef struct VKR_SIMD_ALIGN VkrVulkanPacketUtilityRoot {
  uint64_t vertices;
  uint64_t instances;
  Mat4 view_projection;
  uint64_t materials;
  uint64_t sh_coefficients;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  uint32_t sh_global_slot;
  uint32_t sh_reserved;
  uint32_t shadow_texture;
  uint32_t shadow_sampler;
  uint32_t transmission_texture;
  uint32_t transmission_sampler;
  Vec4 view_position;
  uint32_t material_index;
  uint32_t first_instance;
  uint32_t prefilter_mip_count;
  uint32_t flags;
  Vec4 material_emissive;
  Vec4 material_dielectric_specular;
  Vec4 material_surface;
  Vec4 material_alpha;
  Vec4 material_attenuation_color;
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  Vec4 ambient_color;
  uint32_t render_mode;
  uint32_t alpha_mode;
  uint32_t material_flags;
  uint32_t reserved_0;
  uint64_t point_light_data;
  uint64_t point_light_masks;
  Vec4 point_light_grid_origin_cell_size;
  uint32_t point_light_grid_dimensions_count[4];
  VkrPointLightMask point_light_global_mask;
  uint32_t point_light_count;
  uint32_t point_light_reserved[3];
  uint64_t shadow_cascades;
  Mat4 view;
  uint32_t shadow_cascade_count;
  /* Receiver quality, mirroring VkrShadowReceiverPacketData. Scalars rather
     than Vec4s so the layout matches the Metal root and the Slang mirror
     without padding holes. */
  uint32_t shadow_pcf_sample_count;
  float32_t shadow_receiver_bias_texels;
  float32_t shadow_slope_bias_texels;
  float32_t shadow_normal_offset_texels;
  float32_t shadow_pcf_radius_texels;
  float32_t shadow_cascade_blend_fraction;
  float32_t shadow_fade_start;
  float32_t shadow_fade_end;
  /** Comparison-sampler heap slot; `shadow_sampler` remains the sentinel. */
  uint32_t shadow_comparison_sampler;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t ibl_probe_reserved;
  /** Always valid: graph state in automatic mode, frame-upload fallback in
   * manual. */
  uint64_t exposure_state;
} VkrVulkanPacketUtilityRoot;

_Static_assert(sizeof(VkrVertex3d) == 64u, "Shared vertex ABI drift");
_Static_assert(sizeof(VkrVulkanMaterialGpuRow) == 144u,
               "Vulkan material row ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, base_color_texture) == 16u,
               "Vulkan material texture ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, base_color_sampler) == 32u,
               "Vulkan material sampler ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_id) == 48u,
               "Vulkan material identifier ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_emissive) == 64u,
               "Vulkan material parameter ABI drift");
_Static_assert(sizeof(VkrVulkanPushConstants) == 16u,
               "Push-constant ABI drift");
_Static_assert(sizeof(VkrVulkanCullRoot) == 176u,
               "Deferred cull-root ABI size drift");
_Static_assert(offsetof(VkrVulkanCullRoot, view_projections) == 48u,
               "Deferred cull-root address ABI drift");
_Static_assert(offsetof(VkrVulkanCullRoot, hzb_textures) == 80u,
               "Deferred cull-root HZB ABI drift");
_Static_assert(sizeof(VkrVulkanRasterRoot) == 48u,
               "Deferred raster-root ABI size drift");
_Static_assert(sizeof(VkrVulkanTemporalTransformRoot) == 32u,
               "Temporal transform-root ABI size drift");
_Static_assert(sizeof(VkrVulkanResolveRoot) == 352u,
               "Deferred resolve-root ABI size drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, vertices) == 40u,
               "Deferred resolve-root vertex address ABI drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, view_projection) == 64u,
               "Deferred resolve-root matrix ABI drift");
_Static_assert(sizeof(VkrVulkanTemporalResolveRoot) == 128u,
               "Temporal resolve-root ABI size drift");
_Static_assert(sizeof(VkrVulkanLightingRoot) == 128u,
               "Deferred lighting-root ABI size drift");
_Static_assert(offsetof(VkrVulkanLightingRoot, inverse_view_projection) == 16u,
               "Deferred lighting-root matrix ABI drift");
_Static_assert(sizeof(VkrVulkanHzbRoot) == 48u,
               "Deferred HZB-root ABI size drift");
_Static_assert(sizeof(VkrVulkanExposureRoot) == 112u,
               "Vulkan exposure root ABI size drift");
_Static_assert(sizeof(VkrVulkanBloomRoot) == 64u,
               "Vulkan bloom root ABI size drift");
_Static_assert(sizeof(VkrVulkanGtaoRoot) == 240u,
               "Vulkan GTAO root ABI size drift");
_Static_assert(offsetof(VkrVulkanGtaoRoot, params) == 0u,
               "Vulkan GTAO parameter ABI offset drift");
_Static_assert(offsetof(VkrGtaoGpuParams, projection_m22) == 88u,
               "GTAO projection m22 ABI offset drift");
_Static_assert(offsetof(VkrGtaoGpuParams, projection_m32) == 96u,
               "GTAO projection m32 ABI offset drift");
_Static_assert(offsetof(VkrGtaoGpuParams, projection_m00) == 104u,
               "GTAO projection m00 ABI offset drift");
_Static_assert(offsetof(VkrGtaoGpuParams, projection_m02) == 112u,
               "GTAO projection m02 ABI offset drift");
_Static_assert(offsetof(VkrGtaoGpuParams, effect_radius) == 128u,
               "GTAO radius ABI offset drift");
_Static_assert(offsetof(VkrGtaoGpuParams, final_value_power) == 156u,
               "GTAO final power ABI offset drift");
_Static_assert(offsetof(VkrGtaoGpuParams, slice_count) == 176u,
               "GTAO quality ABI offset drift");
_Static_assert(offsetof(VkrVulkanGtaoRoot, source_texture) == 192u,
               "Vulkan GTAO resource ABI offset drift");
_Static_assert(offsetof(VkrVulkanGtaoRoot, source_extent) == 216u,
               "Vulkan GTAO extent ABI offset drift");
_Static_assert(offsetof(VkrVulkanGtaoRoot, reserved) == 232u,
               "Vulkan GTAO root tail ABI offset drift");
_Static_assert(sizeof(VkrVulkanSdsmRoot) == 32u,
               "Deferred SDSM-root ABI size drift");
_Static_assert(sizeof(VkrVulkanSdsmState) == VKR_VULKAN_SDSM_STATE_SIZE,
               "Deferred SDSM-state ABI size drift");
_Static_assert(sizeof(VkrVulkanPickingRoot) == 64u,
               "Deferred picking-root ABI size drift");
_Static_assert(sizeof(VkrVulkanTransmissionRoot) == 448u,
               "Deferred transmission-root ABI size drift");
_Static_assert(offsetof(VkrVulkanTransmissionRoot, geometry_rows) == 24u,
               "Deferred transmission-root address ABI drift");
_Static_assert(offsetof(VkrVulkanTransmissionRoot, view_projection) == 96u,
               "Deferred transmission-root matrix ABI drift");
_Static_assert(sizeof(VkrVulkanTransmissionCompactRoot) == 80u,
               "Deferred transmission-compact root ABI size drift");
_Static_assert(sizeof(VkrVulkanTransmissionCoverageRoot) == 32u,
               "Deferred transmission-coverage root ABI drift");
_Static_assert(sizeof(VkrVulkanIblRoot) == 32u, "IBL-root ABI drift");
_Static_assert(sizeof(VkrVulkanIblShRoot) == 48u, "IBL SH-root ABI drift");
_Static_assert(sizeof(VkrVulkanPacketShadowCascade) == 96u,
               "Packet shadow-cascade ABI size drift");
_Static_assert(sizeof(VkrVulkanPacketIblProbe) == 64u,
               "Packet IBL-probe ABI size drift");
_Static_assert(offsetof(VkrVulkanPacketIblProbe, sh_slot) == 0u,
               "Packet IBL-probe SH-slot ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketIblProbe, prefilter_texture) == 8u,
               "Packet IBL-probe prefilter ABI offset drift");
_Static_assert(sizeof(VkrVulkanPacketFrameRoot) == 480u,
               "Packet frame-root ABI size drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, sh_coefficients) == 88u,
               "Packet frame-root SH-buffer ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, sh_global_slot) == 104u,
               "Packet frame-root SH-slot ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, shadow_cascade_count) == 400u,
               "Packet frame-root receiver block moved");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, ibl_probes) == 448u,
               "Packet frame-root receiver block changed size");
_Static_assert(sizeof(VkrVulkanPacketDrawRoot) == 48u,
               "Packet draw-root ABI size drift");
_Static_assert(sizeof(VkrVulkanPacketUtilityRoot) == 544u,
               "Packet utility-root ABI size drift");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, sh_coefficients) == 88u,
               "Packet utility-root SH-buffer ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, sh_global_slot) == 104u,
               "Packet utility-root SH-slot ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, ibl_probes) == 520u,
               "Packet utility-root shadow block changed size");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, exposure_state) == 536u,
               "Packet utility-root exposure block moved");

typedef struct VkrVulkanAllocation {
  VkDeviceMemory memory;
  VkDeviceSize memory_size;
  VkDeviceSize offset;
  void *mapped;
  uint32_t memory_type_index;
  VkMemoryPropertyFlags properties;
  VkrVulkanPooledAllocation pooled_allocation;
  VkrVulkanMemoryPoolKey pool_key;
  VkrGpuAllocationOwner owner;
  bool8_t pooled;
  bool8_t dedicated;
  bool8_t retired;
} VkrVulkanAllocation;

typedef struct VkrVulkanBuffer {
  VkBuffer handle;
  VkrVulkanAllocation allocation;
  VkDeviceAddress address;
  VkDeviceSize size;
} VkrVulkanBuffer;

typedef struct VkrVulkanImage {
  VkImage handle;
  VkImageView view;
  VkrVulkanAllocation allocation;
  VkImageLayout layout;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels;
  uint32_t array_layers;
  VkFormat format;
} VkrVulkanImage;

typedef struct VkrVulkanDirtyRange {
  VkDeviceSize offset;
  VkDeviceSize end;
  bool8_t dirty;
} VkrVulkanDirtyRange;

typedef struct VkrVulkanTargetSet {
  VkrVulkanImage images[VKR_VULKAN_TARGET_IMAGE_MAX];
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
} VkrVulkanTargetSet;

typedef struct VkrVulkanGraphImageInstance {
  VkrVulkanImage image;
  VkImageView mip_views[VKR_VULKAN_TEXTURE_MIP_MAX];
  VkImageView mip_layer_views[VKR_VULKAN_TEXTURE_MIP_MAX]
                             [VKR_VULKAN_GRAPH_LAYER_MAX];
  VkrGpuSlotHandle sampled_mip_slots[VKR_VULKAN_TEXTURE_MIP_MAX];
  VkrGpuSlotHandle storage_mip_slots[VKR_VULKAN_TEXTURE_MIP_MAX];
  VkrGpuSlotHandle sampled_slot;
  VkrGpuSlotHandle storage_slot;
  uint64_t last_use_submit_value;
  uint64_t history_producer_submit_value;
  uint64_t history_world_epoch;
  Mat4 history_view_projection;
  uint32_t history_width;
  uint32_t history_height;
  uint64_t history_frame_index;
  uint64_t history_scene_generation;
  bool8_t has_sampled_mip_slot[VKR_VULKAN_TEXTURE_MIP_MAX];
  bool8_t has_storage_mip_slot[VKR_VULKAN_TEXTURE_MIP_MAX];
  bool8_t has_sampled_slot;
  bool8_t has_storage_slot;
  bool8_t history_valid;
  /**
   * Committed cross-frame state for a RETAINED resource (ADR-029), indexed
   * mip * layer_count + layer to match the graph's subresource ordering.
   *
   * Lives on the instance rather than in the graph because the physical image
   * outlives any one frame's graph, and it is written only after a submit is
   * proven, so a cancelled frame leaves the previous frame's contents standing.
   */
  VkrRgRetainedState
      retained_states[VKR_VULKAN_TEXTURE_MIP_MAX * VKR_VULKAN_GRAPH_LAYER_MAX];
} VkrVulkanGraphImageInstance;

typedef struct VkrVulkanGraphImage {
  VkrVulkanGraphImageInstance instances[VKR_VULKAN_TARGET_IMAGE_MAX];
  VkrRgImageDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
  bool8_t external_swapchain;
} VkrVulkanGraphImage;

typedef struct VkrVulkanGraphBufferInstance {
  VkrVulkanBuffer buffer;
  uint64_t last_use_submit_value;
  uint64_t history_producer_submit_value;
  uint64_t history_frame_index;
  uint64_t history_scene_generation;
  bool8_t history_valid;
} VkrVulkanGraphBufferInstance;

typedef struct VkrVulkanGraphBuffer {
  VkrVulkanGraphBufferInstance instances[VKR_VULKAN_TARGET_IMAGE_MAX];
  VkrRgBufferDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
} VkrVulkanGraphBuffer;
typedef struct VkrVulkanCandidateCopyRange {
  uint64_t candidate_source_offset;
  uint64_t instance_source_offset;
  uint32_t destination_first;
  uint32_t count;
} VkrVulkanCandidateCopyRange;

typedef struct VkrVulkanRetiredTargetSet {
  VkrVulkanTargetSet targets;
  uint64_t retire_value;
  bool8_t occupied;
} VkrVulkanRetiredTargetSet;

typedef struct VkrVulkanFrameSlot {
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkQueryPool timestamp_pool;
  VkrVulkanBuffer readback;
  VkrVulkanBuffer capture_readback;
  VkrVulkanBuffer frame_upload;
  VkrCaptureBackendItemPlan capture_plans[VKR_CAPTURE_MAX_ITEMS];
  VkrRgImageHandle capture_images[VKR_CAPTURE_MAX_ITEMS];
  VkrCaptureRequestId capture_request_id;
  uint32_t capture_item_count;
  uint64_t retire_value;
  uint64_t source_frame_index;
  uint32_t image_index;
  uint32_t timestamp_query_count;
  uint32_t pass_timing_count;
  VkrRendererImplPassTiming pass_timings[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS];
  bool8_t timing_requested;
  bool8_t timing_collected;
  bool8_t transmission_coverage_requested;
  uint32_t transmission_coverage_extent[2];
  bool8_t sdsm_requested;
  bool8_t exposure_requested;
  VkrVulkanGraphBufferInstance *sdsm_reduce_state;
  /** Selected completed adaptation record and this frame's output instance. */
  VkrVulkanGraphBufferInstance *exposure_histogram;
  VkrVulkanGraphBuffer *exposure_state_history;
  VkrVulkanGraphBufferInstance *exposure_state_input;
  VkrVulkanGraphBufferInstance *exposure_state_output;
  VkrShadowDepthRangeSample shadow_depth_range;
  bool8_t acquired_window_image;
  bool8_t reacquired_presented_image;
  uint64_t frame_upload_cursor;
  /** Frame-upload allocation failures this frame. Non-zero means the frame was
   *  rejected for want of upload bytes, not for a malformed packet. */
  uint32_t frame_upload_exhaustions;
  uint64_t world_instances;
  uint64_t ui_instances;
  uint64_t editor_instances;
  uint64_t gpu_candidate_instances;
  uint64_t transmission_gpu_candidate_instances;
  uint64_t gpu_geometry_rows;
  VkrVulkanCandidateCopyRange gpu_candidate_copies[2];
  uint32_t gpu_candidate_copy_count;
  uint64_t transmission_gpu_candidate_upload_offset;
  uint64_t transmission_gpu_instance_upload_offset;
  VkrVulkanGraphBufferInstance *gpu_candidate_buffer;
  VkrVulkanGraphBufferInstance *gpu_candidate_instance_buffer;
  VkrCandidateResidencyState candidate_residency;
  VkrCandidateResidencyState pending_candidate_residency;
  bool8_t candidate_residency_pending;
  VkrVulkanGraphBufferInstance *gpu_compaction_state;
  VkrVulkanGraphBufferInstance *transmission_gpu_compaction_state;
  uint32_t gpu_candidate_count;
  uint32_t transmission_gpu_candidate_count;
  uint64_t gpu_world_epoch;
  uint64_t point_light_data;
  uint64_t point_light_masks;
  uint64_t shadow_cascades;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  uint32_t sh_global_slot;
  bool8_t ibl_ready;
  /** True only while this slot's command buffer contains the one-time SH
      sentinel clear. The renderer-wide state commits after queue submission. */
  bool8_t sh_coefficients_clear_recorded;
  /** Slots this frame's packet references, registered against its submit
      serial once submission succeeds. */
  uint32_t sh_referenced_slots[VKR_FRAME_IBL_PROBE_MAX + 1u];
  uint32_t sh_referenced_slot_count;
  uint32_t picking_x;
  uint32_t picking_y;
  bool8_t picking_readback_pending;
  VkrVulkanGraphImageInstance *hzb_history_input;
  VkrVulkanGraphImageInstance *hzb_history_output;
  bool8_t hzb_history_valid;
  VkrVulkanGraphBufferInstance *temporal_transform_input;
  VkrVulkanGraphBufferInstance *temporal_transform_output;
  VkrVulkanGraphImageInstance *temporal_color_input;
  VkrVulkanGraphImageInstance *temporal_color_output;
  VkrVulkanGraphImageInstance *temporal_depth_input;
  VkrVulkanGraphImageInstance *temporal_depth_output;
  VkrVulkanGraphImageInstance *temporal_identity_input;
  VkrVulkanGraphImageInstance *temporal_identity_output;
  VkrVulkanGraphImageInstance *temporal_primitive_input;
  VkrVulkanGraphImageInstance *temporal_primitive_output;
  bool8_t temporal_history_valid;
  uint32_t indexed_draw_count;
  uint32_t blend_draw_count;
  /** Same-frame CPU cost of lowering this packet, reported at submit. */
  VkrPacketBuildMetrics packet_build;
} VkrVulkanFrameSlot;

typedef struct VkrVulkanWindowTarget {
  VkSwapchainKHR swapchain;
  VkImage images[VKR_VULKAN_SWAPCHAIN_IMAGE_MAX];
  VkSemaphore render_complete[VKR_VULKAN_SWAPCHAIN_IMAGE_MAX];
  VkFence present_complete[VKR_VULKAN_SWAPCHAIN_IMAGE_MAX];
  uint64_t image_last_submit_value[VKR_VULKAN_SWAPCHAIN_IMAGE_MAX];
  bool8_t image_presented[VKR_VULKAN_SWAPCHAIN_IMAGE_MAX];
  bool8_t present_fence_pending[VKR_VULKAN_SWAPCHAIN_IMAGE_MAX];
  VkrVulkanReacquireState reacquire_state;
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
  VkFormat format;
  VkColorSpaceKHR color_space;
  VkPresentModeKHR present_mode;
  bool8_t occupied;
} VkrVulkanWindowTarget;

typedef struct VkrVulkanRetiredWindowTarget {
  VkrVulkanWindowTarget target;
  bool8_t occupied;
} VkrVulkanRetiredWindowTarget;

typedef struct VkrVulkanPublishedTexture {
  VkrTextureHandle handle;
  VkrTextureHandle ibl_prefilter;
  /** Published L2 coefficient slot projected from this source cubemap, or
      VKR_SH_SLOT_BLACK before the first successful projection (ADR-038). */
  uint32_t ibl_sh_slot;
  VkrVulkanImage image;
  VkrGpuSlotHandle sampled_slot;
  VkImageView storage_views[VKR_VULKAN_TEXTURE_MIP_MAX];
  VkrGpuSlotHandle storage_slots[VKR_VULKAN_TEXTURE_MIP_MAX];
  uint32_t sampler_record_index;
  uint32_t material_reference_count;
  uint32_t ibl_reference_count;
  uint64_t last_use_submit_value;
  uint32_t storage_slot_count;
  bool8_t initialization_pending;
  bool8_t unpublish_requested;
  bool8_t live;
  bool8_t pending_retire;
} VkrVulkanPublishedTexture;

typedef struct VkrVulkanPendingIblBake {
  VkrTextureHandle equirect;
  VkrTextureHandle source;
  VkrTextureHandle prefilter;
  bool8_t convert_equirect;
  bool8_t recorded;
  /** Candidate coefficient slot this bake projects into, or VKR_SH_SLOT_BLACK
      when no slot could be reserved. Committed only after a successful submit;
      abandoned if recording or submission fails. */
  uint32_t sh_slot;
  /** Authored deringing exponent, validated and normalized at scene load. */
  float32_t sh_deringing;
} VkrVulkanPendingIblBake;

typedef struct VkrVulkanPublishedSampler {
  VkrTextureDescription description;
  VkSampler sampler;
  VkrGpuSlotHandle slot;
  uint64_t last_use_submit_value;
  uint32_t mip_levels;
  uint32_t reference_count;
  bool8_t live;
  bool8_t pending_retire;
} VkrVulkanPublishedSampler;

typedef struct VkrVulkanTextureUploadBatch {
  VkBufferImageCopy2 region;
  uint64_t source_offset;
  uint64_t source_size;
} VkrVulkanTextureUploadBatch;

typedef struct VkrVulkanPendingTextureInitialization {
  VkrVulkanBuffer staging;
  VkrVulkanTextureUploadBatch *batches;
  uint64_t batches_size;
  uint8_t *upload_data;
  uint64_t upload_data_size;
  VkrTextureHandle texture;
  uint32_t batch_count;
  uint32_t next_batch;
  uint32_t staged_batch_count;
  bool8_t writable;
  bool8_t upload_data_accounted;
} VkrVulkanPendingTextureInitialization;

typedef struct VkrVulkanPendingBufferInitialization {
  VkrVulkanBuffer staging;
  VkBuffer destination;
  uint8_t *upload_data;
  VkDeviceSize size;
  VkDeviceSize next_offset;
  VkDeviceSize destination_offset;
  VkPipelineStageFlags2 destination_stage;
  VkAccessFlags2 destination_access;
  uint32_t geometry_record_index;
} VkrVulkanPendingBufferInitialization;

typedef struct VkrVulkanRetiredGeometryMegabuffer {
  VkrVulkanBuffer vertices;
  VkrVulkanBuffer indices;
  uint64_t retire_value;
  bool8_t occupied;
} VkrVulkanRetiredGeometryMegabuffer;

typedef struct VkrVulkanGeometryMegabuffer {
  VkrVulkanBuffer vertices;
  VkrVulkanBuffer indices;
  VkrVulkanBuffer copy_source_vertices;
  VkrVulkanBuffer copy_source_indices;
  uint64_t copy_vertex_size;
  uint64_t copy_index_size;
  VkrVulkanRetiredGeometryMegabuffer retired[4];
  uint64_t vertex_cursor;
  uint64_t index_cursor;
  uint64_t vertex_live_bytes;
  uint64_t index_live_bytes;
  uint64_t vertex_high_water;
  uint64_t index_high_water;
  uint64_t vertex_uploaded_bytes_total;
  uint64_t index_uploaded_bytes_total;
  uint64_t decode_metadata_live_bytes;
  uint64_t decode_metadata_high_water;
  uint64_t decode_metadata_uploaded_bytes_total;
  uint64_t rejected_publications;
  uint64_t generation_replacements;
  uint32_t generation;
  bool8_t live;
  bool8_t copy_pending;
} VkrVulkanGeometryMegabuffer;

typedef struct VkrVulkanRetiredStagingBuffer {
  VkrVulkanBuffer buffer;
  uint64_t retire_value;
  bool8_t occupied;
} VkrVulkanRetiredStagingBuffer;

typedef struct VkrVulkanSubmeshRange {
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
} VkrVulkanSubmeshRange;

typedef struct VkrVulkanPublishedGeometry {
  VkrGeometryHandle handle;
  VkrVulkanBuffer vertices;
  VkrVulkanBuffer indices;
  VkrGpuGeometryRow gpu_row;
  uint32_t vertex_count;
  uint32_t index_count;
  VkIndexType index_type;
  VkrVulkanSubmeshRange *submeshes;
  uint64_t submeshes_size;
  uint32_t submesh_count;
  uint64_t last_use_submit_value;
  uint32_t pending_initialization_count;
  bool8_t live;
  bool8_t pending_retire;
} VkrVulkanPublishedGeometry;

typedef struct VkrVulkanPublishedMaterial {
  VkrMaterialHandle handle;
  VkrGpuSlotHandle slot;
  VkrVulkanMaterialGpuRow row;
  VkrPbrProperties pbr;
  float32_t alpha_cutoff;
  VkrMaterialAlphaMode alpha_mode;
  bool8_t double_sided;
  uint32_t texture_record_indices[4];
  uint8_t pending_texture_count;
  bool8_t live;
} VkrVulkanPublishedMaterial;

typedef struct VkrVulkanRetiredMaterial {
  uint32_t texture_record_indices[4];
  uint64_t retire_value;
  bool8_t occupied;
} VkrVulkanRetiredMaterial;

struct VkrVulkanRenderer {
  VkrAllocator *allocator;
  VkrVulkanRendererConfig config;
  VkrExposureMeteringConfig exposure_metering;
  VkrBloomConfig bloom_config;
  VkrGtaoConfig gtao_config;
  VkrGtaoGpuParams gtao_params;
  VkrDMemory publication_staging_memory;
  VkrDMemory capture_storage_memory;
  Arena *graph_frame_arena;
  VkrAllocator graph_frame_allocator;
  VkrRgJsonGraph json_graph;
  VkrRenderGraph *graph;
  VkrRgExecutorRegistry executors;
  VkrRenderGraphFrameInfo prepared_frame;
  VkrVulkanGraphImage *graph_images;
  uint64_t graph_images_size;
  VkrVulkanGraphBuffer *graph_buffers;
  uint64_t graph_buffers_size;
  VkrRgBufferHandle gpu_candidate_buffer_handle;
  VkrRgBufferHandle gpu_candidate_instance_buffer_handle;
  VkrRgBufferHandle transmission_gpu_candidate_instance_buffer_handle;
  VkrRgBufferHandle temporal_transform_history_handle;
  VkImageMemoryBarrier2 *graph_image_barriers;
  uint64_t graph_image_barriers_size;
  VkBufferMemoryBarrier2 *graph_buffer_barriers;
  uint64_t graph_buffer_barriers_size;
  VkrVulkanDevice *device;
  VkrVulkanTargetSet targets;
  VkrVulkanRetiredTargetSet retired_targets[4];
  VkrVulkanWindowTarget window_target;
  VkrVulkanRetiredWindowTarget
      retired_window_targets[VKR_VULKAN_RETIRED_SWAPCHAIN_MAX];
  VkSemaphore acquire_semaphores[VKR_VULKAN_FRAME_SLOT_COUNT];
  VkrVulkanFrameSlot frame_slots[VKR_VULKAN_FRAME_SLOT_COUNT];
  VkrGpuSubmitRing command_ring;
  VkrGpuSubmitRingSlot command_ring_slots[VKR_VULKAN_FRAME_SLOT_COUNT];
  VkrGpuRingSlice active_command_slice;
  VkrCaptureRing capture_ring;
  void *capture_storage;
  uint64_t capture_storage_size;
  VkrVulkanMemoryPoolManager *memory_pool;
  VkrVulkanBuffer resource_descriptors;
  VkrVulkanBuffer sampler_descriptors;
  VkrGpuSlotTable *sampled_image_slots;
  VkrGpuSlotTable *storage_image_slots;
  VkrGpuSlotTable *sampler_slots;
  VkrGpuSlotTable *material_slots;
  VkrVulkanPublishedGeometry *published_geometries;
  VkrVulkanGeometryMegabuffer geometry_megabuffer;
  VkrVulkanPublishedGeometry *retired_geometries;
  VkrVulkanPublishedTexture *published_textures;
  VkrVulkanPublishedTexture *retired_textures;
  VkrVulkanPublishedSampler *published_samplers;
  VkrVulkanPublishedMaterial *published_materials;
  VkrVulkanRetiredMaterial *retired_materials;
  VkrVulkanPendingTextureInitialization *pending_texture_initializations;
  VkrVulkanPendingBufferInitialization *pending_buffer_initializations;
  VkrVulkanRetiredStagingBuffer *retired_staging_buffers;
  uint64_t published_geometries_size;
  uint64_t retired_geometries_size;
  uint64_t published_textures_size;
  uint64_t retired_textures_size;
  uint64_t published_samplers_size;
  uint64_t published_materials_size;
  uint64_t retired_materials_size;
  uint64_t pending_texture_initializations_size;
  uint64_t pending_buffer_initializations_size;
  uint64_t retired_staging_buffers_size;
  uint64_t pending_texture_upload_bytes;
  uint32_t pending_texture_initialization_count;
  uint32_t pending_buffer_initialization_count;
  uint32_t pending_buffer_initialization_capacity;
  uint32_t retired_staging_buffer_capacity;
  uint32_t staging_buffer_count;
  /* Which publication class claims the single bounded staging chunk next.
     Buffers and textures alternate so neither starves the other. */
  bool8_t stage_textures_first;
  void *sampled_image_slot_storage;
  void *storage_image_slot_storage;
  void *sampler_slot_storage;
  void *material_slot_storage;
  uint64_t sampled_image_slot_storage_size;
  uint64_t storage_image_slot_storage_size;
  uint64_t sampler_slot_storage_size;
  uint64_t material_slot_storage_size;
  uint8_t *descriptor_scratch;
  uint32_t descriptor_scratch_size;
  VkrVulkanDirtyRange resource_descriptor_dirty;
  VkrVulkanDirtyRange sampler_descriptor_dirty;
  VkrVulkanDirtyRange material_dirty;
  VkrVulkanBuffer upload;
  VkrVulkanBuffer materials;
  /** L2 diffuse coefficient slots (ADR-038). Renderer lifetime: the pool
      survives scene reload, and scene reset only retires publications. */
  VkrVulkanBuffer sh_coefficients;
  VkrShSlotPool sh_pool;
  /** Cleared to zero once, so slot 0 is a valid black sentinel and any slot
      referenced before projection reads black rather than stale bytes. */
  bool8_t sh_coefficients_cleared;
  VkrVulkanImage sentinel_image;
  VkSampler sentinel_sampler;
  // Linear clamp sampler for immutable transmission feedback. Its permanent
  // slot avoids per-pass sampler publication and matches Metal filtering.
  VkSampler transmission_sampler;
  uint32_t transmission_sampler_slot;
  /** Linear clamp, LESS_OR_EQUAL comparison sampler for the CSM receiver.
      Published once beside the sentinel and never retired, so its heap slot is
      stable for the device's lifetime. */
  VkSampler shadow_comparison_sampler;
  uint32_t shadow_comparison_sampler_slot;
  VkPipelineLayout pipeline_layout;
  VkPipelineCache pipeline_cache;
  char pipeline_cache_path[1024];
  VkShaderModule packet_shaders[VKR_VULKAN_PACKET_SHADER_COUNT];
  VkPipeline packet_pipelines[VKR_VULKAN_PACKET_PIPELINE_COUNT];
  VkShaderModule ibl_shaders[VKR_VULKAN_IBL_PIPELINE_COUNT];
  VkPipeline ibl_pipelines[VKR_VULKAN_IBL_PIPELINE_COUNT];
  VkShaderModule deferred_shaders[VKR_VULKAN_DEFERRED_PIPELINE_COUNT];
  VkPipeline deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_COUNT];
  VkrVulkanPendingIblBake pending_ibl_bakes[VKR_VULKAN_PENDING_IBL_BAKE_MAX];
  uint32_t pending_ibl_bake_count;
  VkSemaphore timeline;
  uint64_t submit_value;
  uint64_t completed_value;
  uint64_t candidate_publication_generation;
  uint64_t upload_wait_count;
  uint64_t frame_upload_exhaustion_count;
  uint64_t command_slot_wait_count;
  uint32_t active_frame_slot;
  uint32_t history_output_index;
  uint32_t next_image_index;
  bool8_t frame_active;
  bool8_t sentinel_uploaded;
  bool8_t target_dirty;
  bool8_t terminal_failure;
  // One-shot so the bounded publication boundary cannot log per frame.
  bool8_t deferred_candidate_drop_logged;
};

VkDevice vkr_vk_renderer_device(const VkrVulkanRenderer *renderer);
VkFormat vkr_vk_texture_format(VkrTextureFormat format);
VkImageAspectFlags vkr_vk_format_aspects(VkFormat format);
VkImageLayout vkr_vk_texture_layout(VkrTextureLayout layout);
VkrVulkanGraphImageInstance *vkr_vk_graph_image(VkrVulkanRenderer *renderer,
                                                VkrRgImageHandle handle,
                                                uint32_t image_index);
VkrVulkanGraphBufferInstance *vkr_vk_graph_buffer(VkrVulkanRenderer *renderer,
                                                  VkrRgBufferHandle handle);
VkrVulkanPublishedTexture *vkr_vk_published_texture(VkrVulkanRenderer *renderer,
                                                    VkrTextureHandle handle,
                                                    uint32_t *out_index);
VkrVulkanPublishedTexture *
vkr_vk_texture_publication(VkrVulkanRenderer *renderer,
                           VkrTextureHandle handle);
bool8_t vkr_vk_asset_unpublish_texture(void *state, VkrTextureHandle handle);
bool8_t vkr_vk_create_acquire_semaphores(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_create_pipelines(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_create_resources(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_flush(const VkrVulkanRenderer *renderer,
                     const VkrVulkanAllocation *allocation, VkDeviceSize offset,
                     VkDeviceSize size);
bool8_t vkr_vk_flush_publication_ranges(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_invalidate(const VkrVulkanRenderer *renderer,
                          const VkrVulkanAllocation *allocation,
                          VkDeviceSize offset, VkDeviceSize size);
bool8_t vkr_vk_pipeline_cache_initialize(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_realize_graph_images(VkrVulkanRenderer *renderer);
/** Installs this renderer as the graph's retained cross-frame state provider.
 */
void vkr_vk_install_retained_provider(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_realize_graph_buffers(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_select_history_output(VkrVulkanRenderer *renderer);
void vkr_vk_mark_graph_images_submitted(VkrVulkanRenderer *renderer,
                                        uint64_t submit_value);
void vkr_vk_mark_graph_buffers_submitted(VkrVulkanRenderer *renderer,
                                         uint64_t submit_value);
bool8_t vkr_vk_register_graph_executors(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_validate_graph(const VkrVulkanRenderer *renderer);
bool8_t vkr_vk_asset_unpublish_geometry(void *state, VkrGeometryHandle handle);
bool8_t vkr_vk_asset_unpublish_material(void *state, VkrMaterialHandle handle);
bool8_t vkr_vk_collect_captures(VkrVulkanRenderer *renderer,
                                uint64_t completed_value);
bool8_t vkr_vk_commit_buffer_initializations(VkrVulkanRenderer *renderer,
                                             uint64_t retire_value);
bool8_t vkr_vk_commit_texture_initializations(VkrVulkanRenderer *renderer,
                                              uint64_t retire_value);
bool8_t vkr_vk_create_buffer(VkrVulkanRenderer *renderer,
                             VkrVulkanMemoryClass memory_class,
                             VkrGpuAllocationOwner owner, VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             VkrVulkanBuffer *out_buffer);
bool8_t vkr_vk_create_descriptor_slot_tables(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_create_image_ex(VkrVulkanRenderer *renderer, uint32_t width,
                               uint32_t height, uint32_t mip_levels,
                               uint32_t array_layers, VkFormat format,
                               VkImageCreateFlags flags,
                               VkImageViewType view_type,
                               VkImageUsageFlags usage,
                               VkrGpuAllocationOwner owner,
                               VkrVulkanImage *out_image);
bool8_t vkr_vk_create_target_set(VkrVulkanRenderer *renderer, uint32_t width,
                                 uint32_t height, uint32_t image_count,
                                 VkrVulkanTargetSet *out_targets);
bool8_t vkr_vk_create_window_target(VkrVulkanRenderer *renderer,
                                    uint32_t requested_width,
                                    uint32_t requested_height,
                                    uint32_t requested_image_count,
                                    VkSwapchainKHR old_swapchain,
                                    VkrVulkanWindowTarget *out_target);
bool8_t vkr_vk_format_block_info(VkFormat format, uint32_t *out_width,
                                 uint32_t *out_height, uint32_t *out_bytes);
bool8_t vkr_vk_mark_dirty(VkrVulkanDirtyRange *dirty,
                          const VkrVulkanBuffer *buffer, VkDeviceSize offset,
                          VkDeviceSize size);
bool8_t vkr_vk_plan_capture(VkrVulkanRenderer *renderer,
                            const VkrRenderPacket *packet,
                            VkrVulkanFrameSlot *slot);
bool8_t vkr_vk_prepare_packet_uploads(VkrVulkanRenderer *renderer,
                                      VkrVulkanFrameSlot *slot,
                                      const VkrRenderPacket *packet);
VkrVulkanPacketFrameRoot *vkr_vk_packet_frame_root(VkrVulkanFrameSlot *slot,
                                                   uint64_t *out_address);
void vkr_vk_fill_packet_frame_root(
    VkrVulkanRenderer *renderer, VkrVulkanPacketFrameRoot *root,
    const VkrVulkanFrameSlot *slot, const VkrPacketFrameConstants *frame,
    uint64_t instances, Mat4 view_projection, uint32_t shadow_texture,
    uint32_t transmission_texture, bool8_t lighting_pass,
    bool8_t transmission_pass);
bool8_t vkr_vk_publish_sampled_view(VkrVulkanRenderer *renderer,
                                    VkImageView view,
                                    VkImageLayout image_layout,
                                    VkrGpuSlotHandle *out_handle);
bool8_t vkr_vk_publish_sentinel_descriptors(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_publish_storage_view(VkrVulkanRenderer *renderer,
                                    VkImageView view,
                                    VkrGpuSlotHandle *out_handle);
bool8_t vkr_vk_record_capture(VkrVulkanRenderer *renderer,
                              VkCommandBuffer command,
                              VkrVulkanFrameSlot *slot);
bool8_t vkr_vk_record_graph(VkrVulkanRenderer *renderer,
                            VkCommandBuffer command);
bool8_t vkr_vk_record_deferred_upload(VkrVulkanRenderer *renderer,
                                      VkCommandBuffer command,
                                      const VkrRgPass *pass,
                                      bool8_t transmission);
bool8_t vkr_vk_record_deferred_readback(VkrVulkanRenderer *renderer,
                                        VkCommandBuffer command);
bool8_t vkr_vk_record_deferred_cull(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass,
                                    VkrVulkanDeferredPipeline pipeline,
                                    bool8_t transmission);
bool8_t vkr_vk_record_deferred_raster(VkrVulkanRenderer *renderer,
                                      VkCommandBuffer command,
                                      const VkrRgPass *pass, bool8_t shadow,
                                      bool8_t transmission);
bool8_t vkr_vk_record_deferred_gbuffer(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_record_temporal_transform(VkrVulkanRenderer *renderer,
                                         VkCommandBuffer command,
                                         const VkrRgPass *pass);
bool8_t vkr_vk_record_deferred_lighting(VkrVulkanRenderer *renderer,
                                        VkCommandBuffer command,
                                        const VkrRgPass *pass);
bool8_t vkr_vk_record_temporal_resolve(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_record_deferred_hzb(VkrVulkanRenderer *renderer,
                                   VkCommandBuffer command,
                                   const VkrRgPass *pass);
bool8_t vkr_vk_record_exposure_histogram(VkrVulkanRenderer *renderer,
                                         VkCommandBuffer command,
                                         const VkrRgPass *pass);
bool8_t vkr_vk_record_exposure_resolve(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass);
/** Publishes this frame's adaptation record once its submit value is known. */
void vkr_vk_mark_exposure_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value);
bool8_t vkr_vk_record_bloom_prefilter(VkrVulkanRenderer *renderer,
                                      VkCommandBuffer command,
                                      const VkrRgPass *pass);
bool8_t vkr_vk_record_bloom_downsample(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_record_bloom_upsample(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_record_bloom_combine(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass);
bool8_t vkr_vk_record_gtao_depth_prefilter(VkrVulkanRenderer *renderer,
                                           VkCommandBuffer command,
                                           const VkrRgPass *pass);
bool8_t vkr_vk_record_gtao_depth_mip(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_record_gtao_evaluate(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass);
bool8_t vkr_vk_record_gtao_denoise(VkrVulkanRenderer *renderer,
                                   VkCommandBuffer command,
                                   const VkrRgPass *pass);
bool8_t vkr_vk_record_deferred_sdsm(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrRgPass *pass);
bool8_t vkr_vk_record_deferred_picking(VkrVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_record_deferred_transmission(VkrVulkanRenderer *renderer,
                                            VkCommandBuffer command,
                                            const VkrRgPass *pass);
bool8_t vkr_vk_record_deferred_transmission_compact(VkrVulkanRenderer *renderer,
                                                    VkCommandBuffer command,
                                                    const VkrRgPass *pass);
bool8_t
vkr_vk_record_deferred_transmission_coverage(VkrVulkanRenderer *renderer,
                                             VkCommandBuffer command,
                                             const VkrRgPass *pass);
void vkr_vk_mark_hzb_submitted(VkrVulkanRenderer *renderer,
                               uint64_t submit_value);
void vkr_vk_mark_temporal_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value);
bool8_t vkr_vk_record_ibl_bakes(VkrVulkanRenderer *renderer,
                                VkCommandBuffer command);
void vkr_vk_abandon_ibl_bake_recordings(VkrVulkanRenderer *renderer);
void vkr_vk_discard_ibl_bakes(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_record_packet_draws(
    VkrVulkanRenderer *renderer, VkCommandBuffer command,
    VkrVulkanPacketPipeline pipeline, const VkrDrawItem *draws,
    uint32_t draw_count, uint64_t instances, Mat4 view_projection,
    bool8_t alpha_cutout, uint32_t shadow_texture,
    uint32_t transmission_texture, bool8_t transmission_pass);

bool8_t vkr_vk_record_packet_fullscreen(VkrVulkanRenderer *renderer,
                                        VkCommandBuffer command,
                                        VkrVulkanPacketPipeline pipeline,
                                        uint32_t texture_index,
                                        uint64_t exposure_state,
                                        uint32_t flags);
bool8_t vkr_vk_record_text_draws(VkrVulkanRenderer *renderer,
                                 VkCommandBuffer command,
                                 VkrVulkanPacketPipeline pipeline,
                                 const VkrPreparedTextDraw *draws,
                                 uint32_t draw_count, Mat4 view_projection,
                                 uint32_t target_width, uint32_t target_height,
                                 bool8_t ui_domain);
bool8_t vkr_vk_recreate_window_target(VkrVulkanRenderer *renderer,
                                      uint32_t width, uint32_t height,
                                      uint32_t image_count);
bool8_t vkr_vk_retire_allocation(VkrVulkanRenderer *renderer,
                                 VkrVulkanAllocation *allocation,
                                 uint64_t retire_value);
bool8_t vkr_vk_retire_buffer(VkrVulkanRenderer *renderer,
                             VkrVulkanBuffer *buffer, uint64_t retire_value);
bool8_t vkr_vk_stage_next_publication_batch(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_window_presents_complete(VkrVulkanRenderer *renderer,
                                        VkrVulkanWindowTarget *target,
                                        bool8_t wait);
uint64_t vkr_vk_refresh_completed(VkrVulkanRenderer *renderer);
uint64_t vkr_vk_align_up(uint64_t value, uint64_t alignment);
void vkr_vk_collect_asset_publications(VkrVulkanRenderer *renderer,
                                       uint64_t completed);
void vkr_vk_collect_retired_targets(VkrVulkanRenderer *renderer,
                                    uint64_t completed_value);
void vkr_vk_destroy_buffer(VkrVulkanRenderer *renderer,
                           VkrVulkanBuffer *buffer);
void vkr_vk_destroy_frame_slots(VkrVulkanRenderer *renderer);
void vkr_vk_destroy_graph_image(VkrVulkanRenderer *renderer,
                                VkrVulkanGraphImage *slot);
void vkr_vk_destroy_graph_buffer(VkrVulkanRenderer *renderer,
                                 VkrVulkanGraphBuffer *slot);
void vkr_vk_destroy_image(VkrVulkanRenderer *renderer, VkrVulkanImage *image);
void vkr_vk_destroy_target_set(VkrVulkanRenderer *renderer,
                               VkrVulkanTargetSet *targets);
void vkr_vk_destroy_window_target(VkrVulkanRenderer *renderer,
                                  VkrVulkanWindowTarget *target);
void vkr_vk_pipeline_cache_shutdown(VkrVulkanRenderer *renderer);
void *vkr_vk_frame_upload_allocate(VkrVulkanFrameSlot *slot, uint64_t size,
                                   uint64_t alignment, uint64_t *out_address,
                                   uint64_t *out_offset);
void vkr_vk_cmd_image_barrier(VkCommandBuffer command_buffer, VkImage image,
                              VkPipelineStageFlags2 src_stage,
                              VkAccessFlags2 src_access,
                              VkPipelineStageFlags2 dst_stage,
                              VkAccessFlags2 dst_access,
                              VkImageLayout old_layout,
                              VkImageLayout new_layout);
void vkr_vk_collect_retired_window_targets(VkrVulkanRenderer *renderer,
                                           uint64_t completed_submit_value);
void vkr_vk_discard_buffer_initializations(VkrVulkanRenderer *renderer);
void vkr_vk_discard_texture_initializations(VkrVulkanRenderer *renderer);
void vkr_vk_record_buffer_initializations(VkrVulkanRenderer *renderer,
                                          VkCommandBuffer command);
void vkr_vk_record_texture_initializations(VkrVulkanRenderer *renderer,
                                           VkCommandBuffer command);

#endif

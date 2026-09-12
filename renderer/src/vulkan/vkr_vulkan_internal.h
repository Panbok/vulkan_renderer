#include "vkr_packed_geometry.h"
#include "vkr_texture_upload.h"
#ifndef VKR_VULKAN_INTERNAL_H
#define VKR_VULKAN_INTERNAL_H

#include "vulkan/vkr_vulkan_renderer.h"

#include "core/logger.h"
#include "core/vkr_metrics.h"
#include "filesystem/filesystem.h"
#include "vkr_anisotropy_lut.h"
#include "vkr_atmosphere.h"
#include "vkr_bloom.h"
#include "vkr_candidate_residency.h"
#include "vkr_capture_ring.h"
#include "vkr_display_output.h"
#include "vkr_dof.h"
#include "vkr_fog.h"
#include "vkr_froxel_fog.h"
#include "vkr_geometry_ranges.h"
#include "vkr_geometry_upload.h"
#include "vkr_gpu_abi.h"
#include "vkr_gpu_memory.h"
#include "vkr_gpu_slot_table.h"
#include "vkr_gpu_submit_ring.h"
#include "vkr_ibl_math.h"
#include "vkr_ibl_sh_pool.h"
#include "vkr_ltc_lut.h"
#include "vkr_motion_blur.h"
#include "vkr_packet_constants.h"
#include "vkr_render_graph_internal.h"
#include "vkr_rg_json.h"
#include "vkr_sheen_lut.h"
#include "vkr_ssgi.h"
#include "vkr_ssr.h"
#include "vkr_subsurface.h"
#include "vkr_temporal.h"
#include "vulkan/vkr_vulkan_dependency.h"
#include "vulkan/vkr_vulkan_memory.h"
#include "vulkan/vkr_vulkan_wsi.h"

#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "platform/vkr_platform.h"

#include <spirv_reflect.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VKR_VULKAN_PACKET_EDITOR_OVERLAY_VERT_SPV
#define VKR_VULKAN_PACKET_EDITOR_OVERLAY_VERT_SPV                              \
  "packet.editor_overlay.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_EDITOR_OVERLAY_FRAG_SPV
#define VKR_VULKAN_PACKET_EDITOR_OVERLAY_FRAG_SPV                              \
  "packet.editor_overlay.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_EDITOR_OVERLAY_PICKING_FRAG_SPV
#define VKR_VULKAN_PACKET_EDITOR_OVERLAY_PICKING_FRAG_SPV                      \
  "packet.editor_overlay_picking.frag.spv"
#endif
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
#ifndef VKR_VULKAN_PACKET_UI_VERT_SPV
#define VKR_VULKAN_PACKET_UI_VERT_SPV "packet.ui.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_UI_FRAG_SPV
#define VKR_VULKAN_PACKET_UI_FRAG_SPV "packet.ui.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_UI_RECT_VERT_SPV
#define VKR_VULKAN_PACKET_UI_RECT_VERT_SPV "packet.ui_rect.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_UI_RECT_FRAG_SPV
#define VKR_VULKAN_PACKET_UI_RECT_FRAG_SPV "packet.ui_rect.frag.spv"
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
#ifndef VKR_VULKAN_PACKET_ATMOSPHERE_TRANSMITTANCE_COMP_SPV
#define VKR_VULKAN_PACKET_ATMOSPHERE_TRANSMITTANCE_COMP_SPV                    \
  "packet.atmosphere_transmittance.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_ATMOSPHERE_MULTIPLE_SCATTERING_COMP_SPV
#define VKR_VULKAN_PACKET_ATMOSPHERE_MULTIPLE_SCATTERING_COMP_SPV              \
  "packet.atmosphere_multiple_scattering.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_ATMOSPHERE_SOURCE_COMP_SPV
#define VKR_VULKAN_PACKET_ATMOSPHERE_SOURCE_COMP_SPV                           \
  "packet.atmosphere_source.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_ATMOSPHERE_SUN_COMP_SPV
#define VKR_VULKAN_PACKET_ATMOSPHERE_SUN_COMP_SPV                              \
  "packet.atmosphere_sun.comp.spv"
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
#ifndef VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_VERT_SPV
#define VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_VERT_SPV                   \
  "packet.local_shadow_transmission.vert.spv"
#endif
#ifndef VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_FRAG_SPV
#define VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_FRAG_SPV                   \
  "packet.local_shadow_transmission.frag.spv"
#endif
#ifndef VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_OVERFLOW_FRAG_SPV
#define VKR_VULKAN_PACKET_LOCAL_SHADOW_TRANSMISSION_OVERFLOW_FRAG_SPV          \
  "packet.local_shadow_transmission_overflow.frag.spv"
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
#ifndef VKR_VULKAN_PACKET_GBUFFER_RESOLVE_NONE_COMP_SPV
#define VKR_VULKAN_PACKET_GBUFFER_RESOLVE_NONE_COMP_SPV                        \
  "packet.gbuffer_resolve.none.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_COMP_SPV
#define VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_COMP_SPV                    \
  "packet.gbuffer_resolve.emissive.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GBUFFER_RESOLVE_DEBUG_COMP_SPV
#define VKR_VULKAN_PACKET_GBUFFER_RESOLVE_DEBUG_COMP_SPV                       \
  "packet.gbuffer_resolve.debug.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_DEBUG_COMP_SPV
#define VKR_VULKAN_PACKET_GBUFFER_RESOLVE_EMISSIVE_DEBUG_COMP_SPV              \
  "packet.gbuffer_resolve.emissive_debug.comp.spv"
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
#ifndef VKR_VULKAN_PACKET_SSR_DEPTH_BASE_COMP_SPV
#define VKR_VULKAN_PACKET_SSR_DEPTH_BASE_COMP_SPV                              \
  "packet.ssr_depth_base.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSR_DEPTH_MIP_COMP_SPV
#define VKR_VULKAN_PACKET_SSR_DEPTH_MIP_COMP_SPV "packet.ssr_depth_mip.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSR_TRACE_COMP_SPV
#define VKR_VULKAN_PACKET_SSR_TRACE_COMP_SPV "packet.ssr_trace.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSR_TEMPORAL_COMP_SPV
#define VKR_VULKAN_PACKET_SSR_TEMPORAL_COMP_SPV "packet.ssr_temporal.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSR_COMPOSITE_COMP_SPV
#define VKR_VULKAN_PACKET_SSR_COMPOSITE_COMP_SPV "packet.ssr_composite.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSGI_DEPTH_BASE_COMP_SPV
#define VKR_VULKAN_PACKET_SSGI_DEPTH_BASE_COMP_SPV                             \
  "packet.ssgi_depth_base.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSGI_DEPTH_MIP_COMP_SPV
#define VKR_VULKAN_PACKET_SSGI_DEPTH_MIP_COMP_SPV                              \
  "packet.ssgi_depth_mip.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSGI_TRACE_COMP_SPV
#define VKR_VULKAN_PACKET_SSGI_TRACE_COMP_SPV "packet.ssgi_trace.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSGI_TEMPORAL_COMP_SPV
#define VKR_VULKAN_PACKET_SSGI_TEMPORAL_COMP_SPV "packet.ssgi_temporal.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_SSGI_COMPOSITE_COMP_SPV
#define VKR_VULKAN_PACKET_SSGI_COMPOSITE_COMP_SPV                              \
  "packet.ssgi_composite.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_FOG_APPLY_COMP_SPV
#define VKR_VULKAN_PACKET_FOG_APPLY_COMP_SPV "packet.fog_apply.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_FROXEL_INJECT_COMP_SPV
#define VKR_VULKAN_PACKET_FROXEL_INJECT_COMP_SPV "packet.froxel_inject.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_FROXEL_INTEGRATE_COMP_SPV
#define VKR_VULKAN_PACKET_FROXEL_INTEGRATE_COMP_SPV                            \
  "packet.froxel_integrate.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_FROXEL_APPLY_COMP_SPV
#define VKR_VULKAN_PACKET_FROXEL_APPLY_COMP_SPV "packet.froxel_apply.comp.spv"
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
#ifndef VKR_VULKAN_PACKET_SUBSURFACE_GATHER_COMP_SPV
#define VKR_VULKAN_PACKET_SUBSURFACE_GATHER_COMP_SPV "packet.subsurface_gather.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_MOTION_BLUR_TILE_MAX_COMP_SPV
#define VKR_VULKAN_PACKET_MOTION_BLUR_TILE_MAX_COMP_SPV "packet.motion_blur_tile_max.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_MOTION_BLUR_NEIGHBOR_MAX_COMP_SPV
#define VKR_VULKAN_PACKET_MOTION_BLUR_NEIGHBOR_MAX_COMP_SPV "packet.motion_blur_neighbor_max.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_MOTION_BLUR_RECONSTRUCT_COMP_SPV
#define VKR_VULKAN_PACKET_MOTION_BLUR_RECONSTRUCT_COMP_SPV "packet.motion_blur_reconstruct.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_DOF_COC_COMP_SPV
#define VKR_VULKAN_PACKET_DOF_COC_COMP_SPV "packet.dof_coc.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_DOF_DILATE_HORIZONTAL_COMP_SPV
#define VKR_VULKAN_PACKET_DOF_DILATE_HORIZONTAL_COMP_SPV "packet.dof_dilate_horizontal.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_DOF_DILATE_VERTICAL_COMP_SPV
#define VKR_VULKAN_PACKET_DOF_DILATE_VERTICAL_COMP_SPV "packet.dof_dilate_vertical.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_DOF_PREFILTER_COMP_SPV
#define VKR_VULKAN_PACKET_DOF_PREFILTER_COMP_SPV "packet.dof_prefilter.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_DOF_GATHER_COMP_SPV
#define VKR_VULKAN_PACKET_DOF_GATHER_COMP_SPV "packet.dof_gather.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_DOF_COMPOSITE_COMP_SPV
#define VKR_VULKAN_PACKET_DOF_COMPOSITE_COMP_SPV "packet.dof_composite.comp.spv"
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
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_COMP_SPV              \
  "packet.transmission_shade_partitioned.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_COMP_SPV               \
  "packet.transmission_shade_production.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_TEMPORAL_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PRODUCTION_TEMPORAL_COMP_SPV      \
  "packet.transmission_shade_production_temporal.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_COMP_SPV   \
  "packet.transmission_shade_partitioned_production.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_TEMPORAL_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_SHADE_PARTITIONED_PRODUCTION_TEMPORAL_COMP_SPV \
  "packet.transmission_shade_partitioned_production_temporal.comp.spv"
#endif
#ifndef VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_CLEAR_COMP_SPV
#define VKR_VULKAN_PACKET_TRANSMISSION_COMPACT_CLEAR_COMP_SPV                  \
  "packet.transmission_compact_clear.comp.spv"
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
  /* Preserve the former shared-upload ceiling for direct reads. Copy-only
     candidate rows now grow separately in STAGING memory at slot reuse. */
  VKR_VULKAN_FRAME_UPLOAD_SIZE = 75u * 1024u * 1024u,
  VKR_VULKAN_FRAME_UPLOAD_INITIAL_SIZE = 16u * 1024u * 1024u,
  VKR_VULKAN_CANDIDATE_UPLOAD_SIZE =
      2u * VKR_GPU_DRAW_CANDIDATE_CAPACITY *
      (sizeof(VkrGpuCandidateDrawRow) + sizeof(VkrPreparedInstanceGPU)),
};

enum {
  VKR_VULKAN_DEFERRED_VIEW_COUNT_MAX =
      1 + VKR_SHADOW_CASCADE_COUNT_MAX + 2 * VKR_LOCAL_SHADOW_FACE_COUNT_MAX,
  /* The final target can be RGBA8 or RGBA16F. A tightly packed 1x1 image
   * copy therefore occupies eight bytes in the extended-linear case. */
  VKR_VULKAN_READBACK_COLOR_SIZE = 8,
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
  VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_DISPLAY_LINEAR,
  VKR_VULKAN_PACKET_PIPELINE_UI,
  VKR_VULKAN_PACKET_PIPELINE_WORLD_TEXT,
  VKR_VULKAN_PACKET_PIPELINE_PICKING_TEXT,
  VKR_VULKAN_PACKET_PIPELINE_UI_RECT,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_OPAQUE,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW,
  VKR_VULKAN_PACKET_PIPELINE_VISIBILITY_SHADOW_OPAQUE,
  VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW_TRANSMISSION,
  VKR_VULKAN_PACKET_PIPELINE_LOCAL_SHADOW_TRANSMISSION_OVERFLOW,
  VKR_VULKAN_PACKET_PIPELINE_EDITOR_OVERLAY,
  VKR_VULKAN_PACKET_PIPELINE_EDITOR_OVERLAY_PICKING,
  VKR_VULKAN_PACKET_PIPELINE_COUNT,
} VkrVulkanPacketPipeline;

typedef enum VkrVulkanFullscreenFlag {
  /* Editor's retained scene was already lifted for the physical output. */
  VKR_VULKAN_FULLSCREEN_ALREADY_OUTPUT_ENCODED = 1u << 0u,
  VKR_VULKAN_FULLSCREEN_TONEMAP = 1u << 1u,
  VKR_VULKAN_FULLSCREEN_FXAA = 1u << 2u,
  VKR_VULKAN_FULLSCREEN_OPAQUE_ALPHA = 1u << 3u,
  VKR_VULKAN_FULLSCREEN_SOURCE_DISPLAY_LINEAR = 1u << 5u,
  VKR_VULKAN_FULLSCREEN_PREPARE_DISPLAY_LINEAR = 1u << 6u,
  VKR_VULKAN_FULLSCREEN_SCENE_BLUR = 1u << 7u,
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
  VKR_VULKAN_PACKET_SHADER_UI_VERTEX,
  VKR_VULKAN_PACKET_SHADER_UI_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_UI_RECT_VERTEX,
  VKR_VULKAN_PACKET_SHADER_UI_RECT_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_VERTEX,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_OPAQUE_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_VISIBILITY_SHADOW_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_VERTEX,
  VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_LOCAL_SHADOW_TRANSMISSION_OVERFLOW_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_VERTEX,
  VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_EDITOR_OVERLAY_PICKING_FRAGMENT,
  VKR_VULKAN_PACKET_SHADER_COUNT,
} VkrVulkanPacketShader;

typedef enum VkrVulkanIblPipeline {
  VKR_VULKAN_IBL_PIPELINE_EQUIRECT = 0,
  VKR_VULKAN_IBL_PIPELINE_PREFILTER,
  /** L2 coefficient projection (ADR-038). */
  VKR_VULKAN_IBL_PIPELINE_SH,
  VKR_VULKAN_IBL_PIPELINE_COUNT,
} VkrVulkanIblPipeline;

typedef enum VkrVulkanAtmospherePipeline {
  VKR_VULKAN_ATMOSPHERE_PIPELINE_TRANSMITTANCE = 0,
  VKR_VULKAN_ATMOSPHERE_PIPELINE_MULTIPLE_SCATTERING,
  VKR_VULKAN_ATMOSPHERE_PIPELINE_SOURCE,
  VKR_VULKAN_ATMOSPHERE_PIPELINE_SUN,
  VKR_VULKAN_ATMOSPHERE_PIPELINE_COUNT,
} VkrVulkanAtmospherePipeline;

typedef enum VkrVulkanDeferredPipeline {
  VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY = 0,
  VKR_VULKAN_DEFERRED_PIPELINE_PREFIX,
  VKR_VULKAN_DEFERRED_PIPELINE_ENCODE,
  VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_TRANSFORM,
  VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_NONE,
  VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_EMISSIVE,
  VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_DEBUG,
  VKR_VULKAN_DEFERRED_PIPELINE_GBUFFER_EMISSIVE_DEBUG,
  VKR_VULKAN_DEFERRED_PIPELINE_LIGHTING,
  VKR_VULKAN_DEFERRED_PIPELINE_TEMPORAL_RESOLVE,
  VKR_VULKAN_DEFERRED_PIPELINE_FSR31_PREPARE,
  VKR_VULKAN_DEFERRED_PIPELINE_FSR31_STABILIZE,
  VKR_VULKAN_DEFERRED_PIPELINE_HZB,
  VKR_VULKAN_DEFERRED_PIPELINE_SSR_DEPTH_BASE,
  VKR_VULKAN_DEFERRED_PIPELINE_SSR_DEPTH_MIP,
  VKR_VULKAN_DEFERRED_PIPELINE_SSR_TRACE,
  VKR_VULKAN_DEFERRED_PIPELINE_SSR_TEMPORAL,
  VKR_VULKAN_DEFERRED_PIPELINE_SSR_COMPOSITE,
  VKR_VULKAN_DEFERRED_PIPELINE_SSGI_DEPTH_BASE,
  VKR_VULKAN_DEFERRED_PIPELINE_SSGI_DEPTH_MIP,
  VKR_VULKAN_DEFERRED_PIPELINE_SSGI_TRACE,
  VKR_VULKAN_DEFERRED_PIPELINE_SSGI_TEMPORAL,
  VKR_VULKAN_DEFERRED_PIPELINE_SSGI_COMPOSITE,
  VKR_VULKAN_DEFERRED_PIPELINE_FOG_APPLY,
  VKR_VULKAN_DEFERRED_PIPELINE_FROXEL_INJECT,
  VKR_VULKAN_DEFERRED_PIPELINE_FROXEL_INTEGRATE,
  VKR_VULKAN_DEFERRED_PIPELINE_FROXEL_APPLY,
  VKR_VULKAN_DEFERRED_PIPELINE_SDSM,
  VKR_VULKAN_DEFERRED_PIPELINE_PICKING,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PARTITIONED,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PRODUCTION,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PRODUCTION_TEMPORAL,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PARTITIONED_PRODUCTION,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_PARTITIONED_PRODUCTION_TEMPORAL,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT_CLEAR,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COMPACT_FINALIZE,
  VKR_VULKAN_DEFERRED_PIPELINE_TRANSMISSION_COVERAGE,
  VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_CLEAR,
  VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_HISTOGRAM,
  VKR_VULKAN_DEFERRED_PIPELINE_EXPOSURE_RESOLVE,
  VKR_VULKAN_DEFERRED_PIPELINE_SUBSURFACE_GATHER,
  VKR_VULKAN_DEFERRED_PIPELINE_MOTION_BLUR_TILE_MAX,
  VKR_VULKAN_DEFERRED_PIPELINE_MOTION_BLUR_NEIGHBOR_MAX,
  VKR_VULKAN_DEFERRED_PIPELINE_MOTION_BLUR_RECONSTRUCT,
  VKR_VULKAN_DEFERRED_PIPELINE_DOF_COC,
  VKR_VULKAN_DEFERRED_PIPELINE_DOF_DILATE_HORIZONTAL,
  VKR_VULKAN_DEFERRED_PIPELINE_DOF_DILATE_VERTICAL,
  VKR_VULKAN_DEFERRED_PIPELINE_DOF_PREFILTER,
  VKR_VULKAN_DEFERRED_PIPELINE_DOF_GATHER,
  VKR_VULKAN_DEFERRED_PIPELINE_DOF_COMPOSITE,
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
  VKR_VULKAN_MATERIAL_TEXTURE_TRANSMISSION = 1u << 3u,
  VKR_VULKAN_MATERIAL_TEXTURE_THICKNESS = 1u << 4u,
  VKR_VULKAN_MATERIAL_TEXTURE_CLEARCOAT = 1u << 5u,
  VKR_VULKAN_MATERIAL_TEXTURE_CLEARCOAT_ROUGHNESS = 1u << 6u,
  VKR_VULKAN_MATERIAL_TEXTURE_CLEARCOAT_NORMAL = 1u << 7u,
  VKR_VULKAN_MATERIAL_TEXTURE_SHEEN_COLOR = 1u << 8u,
  VKR_VULKAN_MATERIAL_TEXTURE_SHEEN_ROUGHNESS = 1u << 9u,
  VKR_VULKAN_MATERIAL_TEXTURE_ANISOTROPY = 1u << 10u,
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
  Vec4 material_clearcoat;
  uint32_t clearcoat_texture;
  uint32_t clearcoat_roughness_texture;
  uint32_t clearcoat_normal_texture;
  uint32_t clearcoat_sampler;
  uint32_t clearcoat_roughness_sampler;
  uint32_t clearcoat_normal_sampler;
  uint32_t clearcoat_reserved[2];
  /** Linear RGB sheen colour in xyz and Charlie roughness in w. */
  Vec4 material_sheen;
  uint32_t sheen_color_texture;
  uint32_t sheen_roughness_texture;
  uint32_t sheen_color_sampler;
  uint32_t sheen_roughness_sampler;
  /** x strength, y cos(rotation), z sin(rotation), w reserved. */
  Vec4 material_anisotropy;
  uint32_t anisotropy_texture;
  uint32_t anisotropy_sampler;
  /** Linear RGB thin-sheet tint in xyz, direct-light strength in w. */
  Vec4 material_diffuse_transmission;
  /** x strength, y scene profile index, zw reserved. */
  Vec4 material_subsurface;
} VkrVulkanMaterialGpuRow;

typedef struct VKR_SIMD_ALIGN VkrVulkanTransmissionMaterialGpuRow {
  uint32_t transmission_texture;
  uint32_t thickness_texture;
  uint32_t transmission_sampler;
  uint32_t thickness_sampler;
} VkrVulkanTransmissionMaterialGpuRow;

typedef struct VkrVulkanMaterialPublishedRow {
  VkrVulkanMaterialGpuRow material;
  VkrVulkanTransmissionMaterialGpuRow transmission;
} VkrVulkanMaterialPublishedRow;

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
  uint32_t local_shadow_first_view;
  uint32_t transmission_first_view;
  uint32_t transmission_required_flags;
  uint32_t local_shadow_excluded_flags;
  uint32_t reserved;
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

/** One refreshed face borrows the main transmitting view for all three peels.
 */
typedef struct VKR_SIMD_ALIGN VkrVulkanLocalShadowTransmissionRoot {
  VkrVulkanRasterRoot raster;
  uint64_t transmission_materials;
  uint32_t previous_color_texture;
  uint32_t reserved;
  Vec4 light_position;
} VkrVulkanLocalShadowTransmissionRoot;

/** Completion-protected frame upload; graph resources own the five images. */
typedef struct VKR_SIMD_ALIGN VkrVulkanLocalShadowTransmission {
  uint32_t depth0_texture;
  uint32_t color0_texture;
  uint32_t depth1_texture;
  uint32_t color1_texture;
  uint32_t overflow_texture;
  uint32_t extent;
  uint32_t reserved[2];
} VkrVulkanLocalShadowTransmission;

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
  uint32_t clearcoat_texture;
  uint32_t sheen_texture;
  uint32_t anisotropy_texture;
  uint32_t reserved_tail;
  Mat4 sky_reprojection;
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
  uint32_t history_surface_texture;
  uint32_t output_color_texture;
  uint32_t output_depth_texture;
  uint32_t output_identity_texture;
  uint32_t output_surface_texture;
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
  uint32_t scene_history_mode;
  Vec2 current_jitter_pixels;
  Vec2 previous_jitter_pixels;
} VkrVulkanTemporalResolveRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanFsr31PrepareRoot {
  uint32_t scene_texture;
  uint32_t pre_transmission_texture;
  uint32_t validity_texture;
  uint32_t opaque_depth_texture;
  uint32_t transmission_depth_texture;
  uint32_t output_depth_texture;
  uint32_t reactive_texture;
  uint32_t composition_texture;
  uint32_t extent[2];
  uint32_t transmission_enabled;
  uint32_t scene_stationary;
} VkrVulkanFsr31PrepareRoot;
_Static_assert(sizeof(VkrVulkanFsr31PrepareRoot) == 48u,
               "FSR input preparation root must match its shader");

typedef struct VKR_SIMD_ALIGN VkrVulkanFsr31StabilizeRoot {
  uint32_t output_texture;
  uint32_t history_texture;
  uint32_t reactive_texture;
  uint32_t scene_stationary;
  uint32_t output_extent[2];
  uint32_t render_extent[2];
  Vec2 jitter_pixels;
  uint32_t reserved[2];
} VkrVulkanFsr31StabilizeRoot;
_Static_assert(sizeof(VkrVulkanFsr31StabilizeRoot) == 48u,
               "FSR stabilization root must match its shader");

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
  Vec4 solar_disk_radiance;
  uint32_t direct_source_texture;
  uint32_t ssgi_enabled;
  uint32_t clearcoat_texture;
  uint32_t sheen_texture;
  uint32_t anisotropy_texture;
  uint32_t visible_rows_padding[3];
  uint64_t visible_rows;
  uint32_t subsurface_source_texture;
  uint32_t subsurface_profile_count;
} VkrVulkanLightingRoot;
_Static_assert(offsetof(VkrVulkanLightingRoot, subsurface_source_texture) == 184u &&
                   offsetof(VkrVulkanLightingRoot, subsurface_profile_count) == 188u,
               "Subsurface source producer ABI drift");

typedef struct VKR_SIMD_ALIGN VkrVulkanHzbRoot {
  uint32_t source_texture;
  uint32_t destination_texture;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t source_is_depth;
  uint32_t reserved[3];
} VkrVulkanHzbRoot;

/** Mirrors the dedicated current-frame positive view-depth reduction. */
typedef struct VKR_SIMD_ALIGN VkrVulkanSsrDepthBaseRoot {
  VkrSsrGpuParams params;
  uint32_t depth_texture;
  uint32_t vbuffer_texture;
  uint32_t destination_depth_texture;
  uint32_t reserved;
} VkrVulkanSsrDepthBaseRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsrDepthMipRoot {
  uint32_t source_depth_texture;
  uint32_t destination_depth_texture;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t reserved[2];
} VkrVulkanSsrDepthMipRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsrTraceRoot {
  VkrSsrGpuParams params;
  uint32_t depth_texture;
  uint32_t vbuffer_texture;
  uint32_t normal_texture;
  uint32_t specular_texture;
  uint32_t depth_pyramid_texture;
  uint32_t source_texture;
  uint32_t destination_texture;
  uint32_t clearcoat_texture;
  uint32_t source_sampler;
  uint32_t hit_texture;
  uint32_t reserved[2];
} VkrVulkanSsrTraceRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsrTemporalRoot {
  VkrSsrGpuParams params;
  VkrSsrReprojectionGpuParams reprojection;
  uint64_t visible_rows;
  uint64_t instances;
  uint64_t previous_transforms;
  uint32_t previous_frame_index;
  uint32_t raw_texture;
  uint32_t vbuffer_texture;
  uint32_t depth_texture;
  uint32_t normal_texture;
  uint32_t hit_texture;
  uint32_t history_color_texture;
  uint32_t history_depth_texture;
  uint32_t history_identity_texture;
  uint32_t output_color_texture;
  uint32_t output_depth_texture;
  uint32_t output_identity_texture;
  uint32_t specular_texture;
  uint32_t clearcoat_texture;
  uint32_t reserved[4];
} VkrVulkanSsrTemporalRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsrCompositeRoot {
  VkrSsrGpuParams params;
  uint64_t frame;
  uint32_t frame_padding[2];
  Mat4 inverse_view_projection;
  uint32_t scene_texture;
  uint32_t reflection_texture;
  uint32_t vbuffer_texture;
  uint32_t depth_texture;
  uint32_t albedo_texture;
  uint32_t specular_texture;
  uint32_t normal_texture;
  uint32_t gtao_visibility_texture;
  uint32_t linear_sampler;
  uint32_t clearcoat_texture;
  uint32_t sheen_texture;
  uint32_t anisotropy_texture;
} VkrVulkanSsrCompositeRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsgiDepthBaseRoot {
  VkrSsgiGpuParams params;
  uint32_t depth_texture;
  uint32_t vbuffer_texture;
  uint32_t destination_depth_texture;
  uint32_t receiver_texture;
} VkrVulkanSsgiDepthBaseRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsgiDepthMipRoot {
  uint32_t source_depth_texture;
  uint32_t destination_depth_texture;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t reserved[2];
} VkrVulkanSsgiDepthMipRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsgiTraceRoot {
  VkrSsgiGpuParams params;
  uint32_t depth_texture;
  uint32_t vbuffer_texture;
  uint32_t normal_texture;
  uint32_t albedo_texture;
  uint32_t depth_pyramid_texture;
  uint32_t direct_source_texture;
  uint32_t destination_texture;
  uint32_t receiver_texture;
} VkrVulkanSsgiTraceRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsgiTemporalRoot {
  VkrSsgiGpuParams params;
  uint64_t visible_rows;
  uint64_t instances;
  uint32_t raw_texture;
  uint32_t vbuffer_texture;
  uint32_t depth_texture;
  uint32_t normal_texture;
  uint32_t motion_texture;
  uint32_t validity_texture;
  uint32_t history_color_texture;
  uint32_t history_depth_texture;
  uint32_t history_identity_texture;
  uint32_t output_color_texture;
  uint32_t output_depth_texture;
  uint32_t output_identity_texture;
  uint32_t linear_sampler;
  uint32_t receiver_texture;
  uint32_t reserved[2];
} VkrVulkanSsgiTemporalRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanSsgiCompositeRoot {
  VkrSsgiGpuParams params;
  uint64_t frame;
  uint32_t frame_padding[2];
  Mat4 inverse_view_projection;
  uint32_t scene_texture;
  uint32_t reflection_texture;
  uint32_t vbuffer_texture;
  uint32_t depth_texture;
  uint32_t albedo_texture;
  uint32_t normal_texture;
  uint32_t history_depth_texture;
  uint32_t specular_texture;
  uint32_t linear_sampler;
  uint32_t clearcoat_texture;
  uint32_t sheen_texture;
  uint32_t anisotropy_texture;
  uint64_t visible_rows;
  uint32_t subsurface_source_texture;
  uint32_t subsurface_profile_count;
  uint32_t receiver_texture;
  uint32_t reserved[3];
} VkrVulkanSsgiCompositeRoot;
_Static_assert(offsetof(VkrVulkanSsgiCompositeRoot, subsurface_source_texture) == 424u &&
                   offsetof(VkrVulkanSsgiCompositeRoot, subsurface_profile_count) == 428u,
               "Subsurface source producer ABI drift");
typedef struct VKR_SIMD_ALIGN VkrVulkanFogRoot {
  VkrFogGpuParams params;
  Mat4 inverse_view_projection;
  Vec4 camera_position;
  uint32_t depth_texture;
  uint32_t target_texture;
  uint32_t extent[2];
} VkrVulkanFogRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanFroxelInjectRoot {
  uint64_t frame;
  uint64_t params;
  uint32_t history_texture;
  uint32_t history_sampler;
  uint32_t output_texture;
  uint32_t history_valid;
  uint32_t extent[3];
  uint32_t reserved;
} VkrVulkanFroxelInjectRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanFroxelIntegrateRoot {
  uint64_t frame;
  uint64_t params;
  uint32_t scattering_texture;
  uint32_t scattering_sampler;
  uint32_t integrated_texture;
  uint32_t extent[3];
  uint32_t reserved;
} VkrVulkanFroxelIntegrateRoot;

typedef struct VKR_SIMD_ALIGN VkrVulkanFroxelApplyRoot {
  uint64_t frame;
  uint64_t params;
  uint32_t depth_texture;
  uint32_t integrated_texture;
  uint32_t integrated_sampler;
  uint32_t target_texture;
  uint32_t extent[2];
  uint32_t reserved[2];
} VkrVulkanFroxelApplyRoot;

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

typedef struct VKR_SIMD_ALIGN VkrVulkanSubsurfaceRoot {
  VkrSubsurfaceGpuParams params;
  Mat4 inverse_view_projection;
  uint64_t frame;
  uint64_t frame_padding;
  uint64_t visible_rows;
  uint64_t visible_padding;
  uint32_t hdr;
  uint32_t source;
  uint32_t depth;
  uint32_t normal;
  uint32_t vbuffer;
  uint32_t profile_bank;
  uint32_t albedo;
  uint32_t specular;
  uint32_t clearcoat;
  uint32_t sheen;
  uint32_t anisotropy;
  uint32_t destination;
  uint32_t source_sampler;
  uint32_t reserved[3];
} VkrVulkanSubsurfaceRoot;

_Static_assert(sizeof(VkrVulkanSubsurfaceRoot) == 192u,
               "Subsurface gather root ABI drift");

typedef struct VKR_SIMD_ALIGN VkrVulkanMotionBlurRoot {
  VkrMotionBlurGpuParams params;
  uint32_t source0;
  uint32_t source1;
  uint32_t source2;
  uint32_t source3;
  uint32_t source4;
  uint32_t destination0;
  uint32_t source_sampler;
  uint32_t reserved;
} VkrVulkanMotionBlurRoot;

_Static_assert(sizeof(VkrVulkanMotionBlurRoot) == 80u,
               "Motion-blur root ABI size drift");
_Static_assert(offsetof(VkrVulkanMotionBlurRoot, source0) == 48u &&
                   offsetof(VkrVulkanMotionBlurRoot, destination0) == 68u &&
                   offsetof(VkrVulkanMotionBlurRoot, source_sampler) == 72u &&
                   offsetof(VkrVulkanMotionBlurRoot, reserved) == 76u,
               "Motion-blur root ABI offsets drift");

typedef struct VKR_SIMD_ALIGN VkrVulkanDofRoot {
  VkrDofGpuParams params;
  uint32_t source0;
  uint32_t source1;
  uint32_t source2;
  uint32_t source3;
  uint32_t source4;
  uint32_t destination0;
  uint32_t destination1;
  uint32_t source_sampler;
} VkrVulkanDofRoot;

_Static_assert(sizeof(VkrVulkanDofRoot) == 80u,
               "Vulkan DoF root ABI size drift");
_Static_assert(offsetof(VkrVulkanDofRoot, source0) == 48u,
               "Vulkan DoF source0 ABI offset drift");
_Static_assert(offsetof(VkrVulkanDofRoot, source1) == 52u,
               "Vulkan DoF source1 ABI offset drift");
_Static_assert(offsetof(VkrVulkanDofRoot, source2) == 56u,
               "Vulkan DoF source2 ABI offset drift");
_Static_assert(offsetof(VkrVulkanDofRoot, source3) == 60u,
               "Vulkan DoF source3 ABI offset drift");
_Static_assert(offsetof(VkrVulkanDofRoot, source4) == 64u,
               "Vulkan DoF source4 ABI offset drift");
_Static_assert(offsetof(VkrVulkanDofRoot, destination0) == 68u,
               "Vulkan DoF destination0 ABI offset drift");
_Static_assert(offsetof(VkrVulkanDofRoot, destination1) == 72u,
               "Vulkan DoF destination1 ABI offset drift");
_Static_assert(offsetof(VkrVulkanDofRoot, source_sampler) == 76u,
               "Vulkan DoF source_sampler ABI offset drift");

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
  uint64_t transmission_materials;
  uint32_t opaque_texture;
  uint32_t opaque_mip_count;
  uint64_t geometry_rows;
  uint64_t instances;
  uint64_t vertices;
  uint64_t indices;
  uint64_t compaction_state;
  uint64_t pixel_list;
  uint64_t compact_counts;
  uint64_t frame;
  uint32_t frame_address_padding[4];
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
  uint64_t visible_rows;
  uint64_t materials;
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

/** One cold atmosphere dispatch root. Descriptor slots are renderer-owned LUT
 * cache entries plus the candidate source texture's writable cube view. */
typedef struct VKR_SIMD_ALIGN VkrVulkanAtmosphereRoot {
  VkrAtmosphereGpuParams params;
  uint32_t transmittance_sample;
  uint32_t transmittance_storage;
  uint32_t multiple_scattering_sample;
  uint32_t multiple_scattering_storage;
  uint32_t source_storage;
  uint32_t sampler;
  uint64_t sun_output;
  uint32_t extent[2];
  uint32_t face_size;
  uint32_t reserved;
} VkrVulkanAtmosphereRoot;

/**
 * Root for the L2 coefficient projection dispatch (ADR-038).
 *
 * `source_texture` addresses the source cubemap's lazily-published 2D-array
 *
 * view; the kernel loads the selected mip by exact integer face/texel
 *
 * coordinates. `source_sampler` remains in the stable 48-byte root but is not

 * * consulted by this projection. `destination` is the device address of the
 *
 * single 112-byte slot this dispatch writes. The three window scalars are
 *
 * the already-evaluated deringing factors: the CPU owns `pow(sinc_pi(l/3),
 * sh_deringing)` so the kernel carries no pow().
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
  uint32_t reserved[3];
} VkrVulkanIblShRoot;

/** Mirrors VkrShadowCascadePacketData; see vkr_frame_input.h for units. */
typedef struct VKR_SIMD_ALIGN VkrVulkanPacketShadowCascade {
  Mat4 light_view_projection;
  Vec4 split_near_far_texel_depth;
  Vec4 origin_inv_size_sun;
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

typedef struct VKR_SIMD_ALIGN VkrVulkanLtc {
  uint64_t lights;
  uint32_t matrix_texture;
  uint32_t amplitude_texture;
  uint32_t count;
  uint32_t sampler;
  uint32_t reserved[2];
} VkrVulkanLtc;

/** Immutable Charlie split-sum and area-light lookup handles. */
typedef struct VKR_SIMD_ALIGN VkrVulkanSheen {
  uint32_t directional_albedo_texture;
  uint32_t ltc_matrix_texture;
  uint32_t ltc_amplitude_texture;
  uint32_t sampler;
  uint32_t ltc_matrix_texture_b;
  uint32_t ltc_amplitude_texture_b;
  uint32_t reserved[2];
} VkrVulkanSheen;

/** Immutable anisotropic GGX lookup handles shared by every packet draw. */
typedef struct VKR_SIMD_ALIGN VkrVulkanAnisotropy {
  uint32_t table0_texture;
  uint32_t table1_texture;
  uint32_t table2_texture;
  uint32_t sampler_index;
} VkrVulkanAnisotropy;
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
  uint64_t local_shadow_views;
  uint32_t local_shadow_texture;
  uint32_t local_shadow_reserved;
  uint32_t dfg_texture;
  uint32_t dfg_sampler;
  uint32_t diffuse_volume_texture;
  uint32_t diffuse_volume_reserved[3];
  Vec4 diffuse_volume_origin;
  Vec4 diffuse_volume_inverse_spacing;
  uint32_t diffuse_volume_dimensions[4];
  uint64_t ltc;
  uint64_t fog;
  uint64_t froxel_fog;
  uint32_t froxel_integrated_texture;
  uint32_t froxel_sampler;
  uint64_t sheen;
  uint64_t anisotropy;
  uint64_t local_shadow_transmission;
  uint64_t local_shadow_transmission_reserved;
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

/** Compact per-batch root for the retained UI stream. */
typedef struct VKR_SIMD_ALIGN VkrVulkanEditorOverlayRoot {
  uint64_t vertices;
  uint64_t decode;
  Mat4 model_view_projection;
  Vec4 color;
  uint32_t first_vertex;
  uint32_t decode_index;
  uint32_t object_id;
  uint32_t reserved;
  uint64_t display_output;
  uint64_t display_output_reserved;
} VkrVulkanEditorOverlayRoot;
_Static_assert(sizeof(VkrVulkanEditorOverlayRoot) == 128u,
               "Editor overlay root ABI size drift");
_Static_assert(offsetof(VkrVulkanEditorOverlayRoot, model_view_projection) ==
                       16u &&
                   offsetof(VkrVulkanEditorOverlayRoot, color) == 80u &&
                   offsetof(VkrVulkanEditorOverlayRoot, object_id) == 104u,
               "Editor overlay root ABI offset drift");

typedef struct VKR_SIMD_ALIGN VkrVulkanUiRoot {
  uint64_t vertices;
  uint32_t texture;
  uint32_t sampler;
  /** target width/height followed by the normalized MTSDF unit range. */
  Vec4 target_unit_range;
  Vec2 rect_extent;
  uint32_t mode;
  uint32_t flags;
  /** top-left, top-right, bottom-right, bottom-left. */
  Vec4 corner_radii;
  uint64_t display_output;
  uint64_t display_output_reserved;
} VkrVulkanUiRoot;

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
  uint64_t color_grading;
  uint64_t display_output_padding;
  uint64_t display_output;
  uint64_t display_output_reserved;
} VkrVulkanPacketUtilityRoot;

_Static_assert(sizeof(VkrVertex3d) == 64u, "Shared vertex ABI drift");
_Static_assert(sizeof(VkrVulkanUiRoot) == 80u,
               "Vulkan UI root ABI size drift");
_Static_assert(offsetof(VkrVulkanUiRoot, target_unit_range) == 16u,
               "Vulkan UI root target offset drift");
_Static_assert(offsetof(VkrVulkanUiRoot, corner_radii) == 48u,
               "Vulkan UI root radius offset drift");
_Static_assert(offsetof(VkrVulkanUiRoot, display_output) == 64u,
               "Vulkan UI root display-output ABI drift");
_Static_assert(offsetof(VkrVulkanEditorOverlayRoot, display_output) == 112u,
               "Editor overlay display-output ABI drift");
_Static_assert(sizeof(VkrVulkanPacketUtilityRoot) == 576u &&
                   offsetof(VkrVulkanPacketUtilityRoot, display_output) == 560u,
               "Vulkan utility display-output ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_subsurface) == 272u,
               "Subsurface material ABI offset drift");
_Static_assert(sizeof(VkrVulkanMaterialGpuRow) == 288u,
               "Vulkan material row ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, base_color_texture) == 16u,
               "Vulkan material texture ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, base_color_sampler) == 32u,
               "Vulkan material sampler ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_id) == 48u,
               "Vulkan material identifier ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_emissive) == 64u,
               "Vulkan material parameter ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_clearcoat) == 144u &&
                   offsetof(VkrVulkanMaterialGpuRow, clearcoat_texture) ==
                       160u &&
                   offsetof(VkrVulkanMaterialGpuRow,
                            clearcoat_roughness_texture) == 164u &&
                   offsetof(VkrVulkanMaterialGpuRow, clearcoat_normal_texture) ==
                       168u &&
                   offsetof(VkrVulkanMaterialGpuRow, clearcoat_sampler) ==
                       172u &&
                   offsetof(VkrVulkanMaterialGpuRow,
                            clearcoat_roughness_sampler) == 176u &&
                   offsetof(VkrVulkanMaterialGpuRow, clearcoat_normal_sampler) ==
                       180u,
               "Vulkan clearcoat material ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_sheen) == 192u &&
                   offsetof(VkrVulkanMaterialGpuRow, sheen_color_texture) ==
                       208u &&
                   offsetof(VkrVulkanMaterialGpuRow, sheen_roughness_texture) ==
                       212u &&
                   offsetof(VkrVulkanMaterialGpuRow, sheen_color_sampler) ==
                       216u &&
                   offsetof(VkrVulkanMaterialGpuRow, sheen_roughness_sampler) ==
                       220u,
               "Vulkan sheen material ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow, material_anisotropy) == 224u &&
                   offsetof(VkrVulkanMaterialGpuRow, anisotropy_texture) ==
                       240u &&
                   offsetof(VkrVulkanMaterialGpuRow, anisotropy_sampler) ==
                       244u,
               "Vulkan anisotropy material ABI drift");
_Static_assert(offsetof(VkrVulkanMaterialGpuRow,
                        material_diffuse_transmission) == 256u,
               "Vulkan diffuse-transmission material ABI drift");
_Static_assert(sizeof(VkrVulkanTransmissionMaterialGpuRow) == 16u,
               "Vulkan transmission material row ABI drift");
_Static_assert(offsetof(VkrVulkanTransmissionMaterialGpuRow,
                        transmission_sampler) == 8u,
               "Vulkan transmission material sampler ABI drift");
_Static_assert(sizeof(VkrVulkanPushConstants) == 16u,
               "Push-constant ABI drift");
_Static_assert(sizeof(VkrVulkanCullRoot) == 192u,
               "Deferred cull-root ABI size drift");
_Static_assert(offsetof(VkrVulkanCullRoot, view_projections) == 48u,
               "Deferred cull-root address ABI drift");
_Static_assert(offsetof(VkrVulkanCullRoot, hzb_textures) == 80u,
               "Deferred cull-root HZB ABI drift");
_Static_assert(sizeof(VkrVulkanRasterRoot) == 48u,
               "Deferred raster-root ABI size drift");
_Static_assert(sizeof(VkrVulkanLocalShadowTransmissionRoot) == 80u &&
                   offsetof(VkrVulkanLocalShadowTransmissionRoot,
                            transmission_materials) == 48u &&
                   offsetof(VkrVulkanLocalShadowTransmissionRoot,
                            previous_color_texture) == 56u &&
                   offsetof(VkrVulkanLocalShadowTransmissionRoot,
                            light_position) == 64u,
               "Local shadow transmission raster ABI drift");
_Static_assert(
    sizeof(VkrVulkanLocalShadowTransmission) == 32u &&
        offsetof(VkrVulkanLocalShadowTransmission, depth0_texture) == 0u &&
        offsetof(VkrVulkanLocalShadowTransmission, color0_texture) == 4u &&
        offsetof(VkrVulkanLocalShadowTransmission, depth1_texture) == 8u &&
        offsetof(VkrVulkanLocalShadowTransmission, color1_texture) == 12u &&
        offsetof(VkrVulkanLocalShadowTransmission, overflow_texture) == 16u &&
        offsetof(VkrVulkanLocalShadowTransmission, extent) == 20u,
    "Local shadow transmission sampling ABI drift");
_Static_assert(sizeof(VkrVulkanTemporalTransformRoot) == 32u,
               "Temporal transform-root ABI size drift");
_Static_assert(sizeof(VkrVulkanResolveRoot) == 416u,
               "Deferred resolve-root ABI size drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, vertices) == 40u,
               "Deferred resolve-root vertex address ABI drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, view_projection) == 64u,
               "Deferred resolve-root matrix ABI drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, clearcoat_texture) == 336u,
               "Deferred resolve-root clearcoat ABI drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, sheen_texture) == 340u,
               "Deferred resolve-root sheen ABI drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, anisotropy_texture) == 344u,
               "Deferred resolve-root anisotropy ABI drift");
_Static_assert(offsetof(VkrVulkanResolveRoot, sky_reprojection) == 352u,
               "G-buffer sky-reprojection matrix ABI drift");
_Static_assert(sizeof(VkrVulkanTemporalResolveRoot) == 144u,
               "Temporal resolve-root ABI size drift");
_Static_assert(
    offsetof(VkrVulkanTemporalResolveRoot, scene_history_mode) == 124u &&
        offsetof(VkrVulkanTemporalResolveRoot, current_jitter_pixels) == 128u &&
        offsetof(VkrVulkanTemporalResolveRoot, previous_jitter_pixels) == 136u,
    "Temporal resolve-root scene/jitter ABI drift");
_Static_assert(sizeof(VkrVulkanLightingRoot) == 192u,
               "Deferred lighting-root ABI size drift");
_Static_assert(offsetof(VkrVulkanLightingRoot, inverse_view_projection) == 16u,
               "Deferred lighting-root matrix ABI drift");
_Static_assert(offsetof(VkrVulkanLightingRoot, solar_disk_radiance) == 128u,
               "Deferred lighting-root solar ABI drift");
_Static_assert(offsetof(VkrVulkanLightingRoot, direct_source_texture) == 144u &&
                   offsetof(VkrVulkanLightingRoot, ssgi_enabled) == 148u &&
                   offsetof(VkrVulkanLightingRoot, clearcoat_texture) == 152u &&
                   offsetof(VkrVulkanLightingRoot, sheen_texture) == 156u &&
                   offsetof(VkrVulkanLightingRoot, anisotropy_texture) == 160u,
               "Deferred lighting-root SSGI/clearcoat ABI drift");
_Static_assert(offsetof(VkrVulkanLightingRoot, visible_rows) == 176u,
               "Deferred lighting-root visible-row ABI drift");
_Static_assert(sizeof(VkrVulkanHzbRoot) == 48u,
               "Deferred HZB-root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsrDepthBaseRoot) == 304u &&
                   offsetof(VkrVulkanSsrDepthBaseRoot, reserved) == 300u,
               "SSR depth-base root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsrDepthMipRoot) == 32u,
               "SSR depth-mip root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsrTraceRoot) == 336u &&
                   offsetof(VkrVulkanSsrTraceRoot, clearcoat_texture) == 316u &&
                   offsetof(VkrVulkanSsrTraceRoot, source_sampler) == 320u &&
                   offsetof(VkrVulkanSsrTraceRoot, hit_texture) == 324u &&
                   offsetof(VkrVulkanSsrTraceRoot, reserved) == 328u,
               "SSR trace root ABI size drift");
_Static_assert(
    sizeof(VkrVulkanSsrTemporalRoot) == 512u &&
        offsetof(VkrVulkanSsrTemporalRoot, reprojection) == 288u &&
        offsetof(VkrVulkanSsrTemporalRoot, visible_rows) == 416u &&
        offsetof(VkrVulkanSsrTemporalRoot, instances) == 424u &&
        offsetof(VkrVulkanSsrTemporalRoot, previous_transforms) == 432u &&
        offsetof(VkrVulkanSsrTemporalRoot, previous_frame_index) == 440u &&
        offsetof(VkrVulkanSsrTemporalRoot, hit_texture) == 460u &&
        offsetof(VkrVulkanSsrTemporalRoot, clearcoat_texture) == 492u &&
        offsetof(VkrVulkanSsrTemporalRoot, reserved) == 496u,
    "SSR temporal root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsrCompositeRoot) == 416u &&
                   offsetof(VkrVulkanSsrCompositeRoot, clearcoat_texture) ==
                       404u &&
                   offsetof(VkrVulkanSsrCompositeRoot, sheen_texture) == 408u &&
                   offsetof(VkrVulkanSsrCompositeRoot, anisotropy_texture) ==
                       412u,
               "SSR composite root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsgiDepthBaseRoot) == 304u,
               "SSGI depth-base root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsgiDepthMipRoot) == 32u,
               "SSGI depth-mip root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsgiTraceRoot) == 320u,
               "SSGI trace root ABI size drift");
_Static_assert(sizeof(VkrVulkanSsgiTemporalRoot) == 368u,
               "SSGI temporal root ABI size drift");
_Static_assert(
    sizeof(VkrVulkanSsgiCompositeRoot) == 448u &&
        offsetof(VkrVulkanSsgiCompositeRoot, clearcoat_texture) == 404u &&
        offsetof(VkrVulkanSsgiCompositeRoot, sheen_texture) == 408u &&
        offsetof(VkrVulkanSsgiCompositeRoot, anisotropy_texture) == 412u &&
        offsetof(VkrVulkanSsgiCompositeRoot, visible_rows) == 416u,
    "SSGI composite root ABI size drift");
_Static_assert(sizeof(VkrVulkanFogRoot) == 128u,
               "Vulkan fog root ABI size drift");
_Static_assert(offsetof(VkrVulkanFogRoot, params) == 0u &&
                   offsetof(VkrVulkanFogRoot, inverse_view_projection) == 32u &&
                   offsetof(VkrVulkanFogRoot, camera_position) == 96u &&
                   offsetof(VkrVulkanFogRoot, depth_texture) == 112u &&
                   offsetof(VkrVulkanFogRoot, target_texture) == 116u &&
                   offsetof(VkrVulkanFogRoot, extent) == 120u,
               "Vulkan fog root ABI offset drift");
_Static_assert(sizeof(VkrVulkanFroxelInjectRoot) == 48u,
               "Froxel inject-root ABI drift");
_Static_assert(offsetof(VkrVulkanFroxelInjectRoot, frame) == 0u &&
                   offsetof(VkrVulkanFroxelInjectRoot, params) == 8u &&
                   offsetof(VkrVulkanFroxelInjectRoot, history_texture) ==
                       16u &&
                   offsetof(VkrVulkanFroxelInjectRoot, history_sampler) ==
                       20u &&
                   offsetof(VkrVulkanFroxelInjectRoot, output_texture) == 24u &&
                   offsetof(VkrVulkanFroxelInjectRoot, history_valid) == 28u &&
                   offsetof(VkrVulkanFroxelInjectRoot, extent) == 32u &&
                   offsetof(VkrVulkanFroxelInjectRoot, reserved) == 44u,
               "Froxel inject-root ABI offset drift");
_Static_assert(sizeof(VkrVulkanFroxelIntegrateRoot) == 48u,
               "Froxel integrate-root ABI drift");
_Static_assert(
    offsetof(VkrVulkanFroxelIntegrateRoot, frame) == 0u &&
        offsetof(VkrVulkanFroxelIntegrateRoot, params) == 8u &&
        offsetof(VkrVulkanFroxelIntegrateRoot, scattering_texture) == 16u &&
        offsetof(VkrVulkanFroxelIntegrateRoot, scattering_sampler) == 20u &&
        offsetof(VkrVulkanFroxelIntegrateRoot, integrated_texture) == 24u &&
        offsetof(VkrVulkanFroxelIntegrateRoot, extent) == 28u &&
        offsetof(VkrVulkanFroxelIntegrateRoot, reserved) == 40u,
    "Froxel integrate-root ABI offset drift");
_Static_assert(sizeof(VkrVulkanFroxelApplyRoot) == 48u,
               "Froxel apply-root ABI drift");
_Static_assert(offsetof(VkrVulkanFroxelApplyRoot, frame) == 0u &&
                   offsetof(VkrVulkanFroxelApplyRoot, params) == 8u &&
                   offsetof(VkrVulkanFroxelApplyRoot, depth_texture) == 16u &&
                   offsetof(VkrVulkanFroxelApplyRoot, integrated_texture) ==
                       20u &&
                   offsetof(VkrVulkanFroxelApplyRoot, integrated_sampler) ==
                       24u &&
                   offsetof(VkrVulkanFroxelApplyRoot, target_texture) == 28u &&
                   offsetof(VkrVulkanFroxelApplyRoot, extent) == 32u &&
                   offsetof(VkrVulkanFroxelApplyRoot, reserved) == 40u,
               "Froxel apply-root ABI offset drift");
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
_Static_assert(sizeof(VkrVulkanTransmissionRoot) == 464u,
               "Deferred transmission-root ABI size drift");
_Static_assert(offsetof(VkrVulkanTransmissionRoot, geometry_rows) == 32u,
               "Deferred transmission-root address ABI drift");
_Static_assert(offsetof(VkrVulkanTransmissionRoot, view_projection) == 112u,
               "Deferred transmission-root matrix ABI drift");
_Static_assert(sizeof(VkrVulkanTransmissionCompactRoot) == 96u,
               "Deferred transmission-compact root ABI size drift");
_Static_assert(sizeof(VkrVulkanTransmissionCoverageRoot) == 32u,
               "Deferred transmission-coverage root ABI drift");
_Static_assert(sizeof(VkrVulkanIblRoot) == 32u, "IBL-root ABI drift");
_Static_assert(sizeof(VkrVulkanAtmosphereRoot) == 176u,
               "Atmosphere-root ABI drift");
_Static_assert(offsetof(VkrVulkanAtmosphereRoot, transmittance_sample) ==
                       128u &&
                   offsetof(VkrVulkanAtmosphereRoot, source_storage) == 144u &&
                   offsetof(VkrVulkanAtmosphereRoot, sun_output) == 152u &&
                   offsetof(VkrVulkanAtmosphereRoot, extent) == 160u &&
                   offsetof(VkrVulkanAtmosphereRoot, face_size) == 168u,
               "Atmosphere-root field ABI drift");
_Static_assert(sizeof(VkrVulkanIblShRoot) == 48u, "IBL SH-root ABI drift");
_Static_assert(offsetof(VkrVulkanIblShRoot, destination) == 0u,
               "IBL SH-root destination ABI drift");
_Static_assert(offsetof(VkrVulkanIblShRoot, source_texture) == 8u,
               "IBL SH-root source ABI drift");
_Static_assert(offsetof(VkrVulkanIblShRoot, source_face_size) == 16u,
               "IBL SH-root extent ABI drift");
_Static_assert(offsetof(VkrVulkanIblShRoot, window_band_0) == 24u,
               "IBL SH-root window ABI drift");
_Static_assert(sizeof(VkrVulkanPacketShadowCascade) == 96u,
               "Packet shadow-cascade ABI size drift");
_Static_assert(sizeof(VkrVulkanPacketIblProbe) == 64u,
               "Packet IBL-probe ABI size drift");
_Static_assert(offsetof(VkrVulkanPacketIblProbe, sh_slot) == 0u,
               "Packet IBL-probe SH-slot ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketIblProbe, prefilter_texture) == 8u,
               "Packet IBL-probe prefilter ABI offset drift");
_Static_assert(sizeof(VkrVulkanLtc) == 32u, "Vulkan LTC ABI size drift");
_Static_assert(offsetof(VkrVulkanLtc, lights) == 0u,
               "Vulkan LTC lights ABI offset drift");
_Static_assert(offsetof(VkrVulkanLtc, matrix_texture) == 8u,
               "Vulkan LTC matrix ABI offset drift");
_Static_assert(offsetof(VkrVulkanLtc, amplitude_texture) == 12u,
               "Vulkan LTC amplitude ABI offset drift");
_Static_assert(offsetof(VkrVulkanLtc, count) == 16u,
               "Vulkan LTC count ABI offset drift");
_Static_assert(offsetof(VkrVulkanLtc, sampler) == 20u,
               "Vulkan LTC sampler ABI offset drift");
_Static_assert(offsetof(VkrVulkanLtc, reserved) == 24u,
               "Vulkan LTC reserved ABI offset drift");
_Static_assert(sizeof(VkrVulkanSheen) == 32u &&
                   offsetof(VkrVulkanSheen, directional_albedo_texture) == 0u &&
                   offsetof(VkrVulkanSheen, ltc_matrix_texture) == 4u &&
                   offsetof(VkrVulkanSheen, ltc_amplitude_texture) == 8u &&
                   offsetof(VkrVulkanSheen, sampler) == 12u &&
                   offsetof(VkrVulkanSheen, ltc_matrix_texture_b) == 16u &&
                   offsetof(VkrVulkanSheen, ltc_amplitude_texture_b) == 20u &&
                   offsetof(VkrVulkanSheen, reserved) == 24u,
               "Vulkan sheen ABI drift");
_Static_assert(sizeof(VkrVulkanAnisotropy) == 16u &&
                   offsetof(VkrVulkanAnisotropy, table0_texture) == 0u &&
                   offsetof(VkrVulkanAnisotropy, table1_texture) == 4u &&
                   offsetof(VkrVulkanAnisotropy, table2_texture) == 8u &&
                   offsetof(VkrVulkanAnisotropy, sampler_index) == 12u,
               "Vulkan anisotropy ABI drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, local_shadow_views) == 472u,
               "Vulkan local shadow address offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, local_shadow_texture) == 480u,
               "Vulkan local shadow texture offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, dfg_texture) == 488u,
               "Vulkan DFG texture ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, dfg_sampler) == 492u,
               "Vulkan DFG sampler ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, diffuse_volume_texture) ==
                   496u,
               "Vulkan diffuse-volume texture ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, diffuse_volume_origin) ==
                   512u,
               "Vulkan diffuse-volume origin ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot,
                        diffuse_volume_inverse_spacing) == 528u,
               "Vulkan diffuse-volume spacing ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, diffuse_volume_dimensions) ==
                   544u,
               "Vulkan diffuse-volume dimensions ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, ltc) == 560u,
               "Vulkan LTC address ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, fog) == 568u,
               "Vulkan fog address ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, froxel_fog) == 576u,
               "Vulkan froxel parameter address ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, froxel_integrated_texture) ==
                   584u,
               "Vulkan froxel integrated descriptor ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, froxel_sampler) == 588u,
               "Vulkan froxel sampler ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, sheen) == 592u,
               "Vulkan sheen address ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, anisotropy) == 600u,
               "Vulkan anisotropy address ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketFrameRoot, local_shadow_transmission) ==
                   608u,
               "Vulkan local shadow transmission address ABI drift");
_Static_assert(sizeof(VkrVulkanPacketFrameRoot) == 624u,
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
_Static_assert(sizeof(VkrVulkanPacketUtilityRoot) == 576u,
               "Packet utility-root ABI size drift");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, sh_coefficients) == 88u,
               "Packet utility-root SH-buffer ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, sh_global_slot) == 104u,
               "Packet utility-root SH-slot ABI offset drift");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, ibl_probes) == 520u,
               "Packet utility-root shadow block changed size");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, exposure_state) == 536u,
               "Packet utility-root exposure block moved");
_Static_assert(offsetof(VkrVulkanPacketUtilityRoot, color_grading) == 544u &&
                   offsetof(VkrVulkanPacketUtilityRoot, display_output) ==
                       560u,
               "Packet utility-root display block moved");

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
  uint32_t depth;
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

typedef struct VkrVulkanTemporalSceneState {
  VkrTemporalSceneSignature signature;
  uint64_t radiance_revision;
  uint64_t publication_generation;
  uint64_t graph_revision;
  /** Consecutive submitted static frames, capped at the SSR settle limit. */
  uint32_t unchanged_frames;
} VkrVulkanTemporalSceneState;

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
  /** Exact raster grid that produced HZB depth; camera compatibility is
   * separate. */
  Mat4 history_raster_view_projection;
  /** Previous jittered projection for SSR temporal depth linearization. */
  Mat4 history_projection;
  uint32_t history_width;
  uint32_t history_height;
  uint64_t history_frame_index;
  uint64_t history_scene_generation;
  VkrVulkanTemporalSceneState history_scene;
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
  float64_t history_exposure_seconds;
  float64_t history_motion_seconds;
  /** Camera of this transform producer; committed only after submission. */
  Mat4 history_view;
  bool8_t history_motion_valid;
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

typedef struct VkrVulkanPreparedDirectDraw VkrVulkanPreparedDirectDraw;

typedef struct VkrVulkanPreparedCompute {
  uint64_t root_address;
  VkPipeline pipelines[3];
  uint32_t groups[3][3];
  uint32_t dispatch_count;
  VkBuffer indirect_buffer;
  VkDeviceSize indirect_offset;
  VkImageMemoryBarrier2 image_barriers[4];
  uint32_t image_barrier_count;
  VkBufferMemoryBarrier2 buffer_barrier;
  bool8_t has_buffer_barrier;
} VkrVulkanPreparedCompute;

typedef struct VkrVulkanPreparedRaster {
  uint64_t root_address;
  uint32_t command_partition_capacity;
  VkBuffer indices;
  VkBuffer arguments;
  VkBuffer counts;
  VkPipeline pipelines[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  VkCullModeFlags cull_modes[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  VkFrontFace front_faces[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  VkDeviceSize argument_offsets[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  VkDeviceSize count_offsets[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
} VkrVulkanPreparedRaster;

typedef struct VkrVulkanPreparedWorldDraws {
  VkrVulkanPacketPipeline pipeline;
  uint64_t roots_address;
  bool8_t lighting;
  bool8_t enabled;
} VkrVulkanPreparedWorldDraws;

typedef struct VkrVulkanPreparedOverlayDraw VkrVulkanPreparedOverlayDraw;
typedef struct VkrVulkanPreparedOverlay {
  VkrVulkanPacketPipeline pipeline;
  VkrVulkanPreparedOverlayDraw *draws;
  uint64_t roots_address;
  uint32_t count;
} VkrVulkanPreparedOverlay;

typedef struct VkrVulkanPreparedTextDraw VkrVulkanPreparedTextDraw;
typedef struct VkrVulkanPreparedUiDraw VkrVulkanPreparedUiDraw;
typedef struct VkrVulkanPreparedText {
  VkrVulkanPacketPipeline pipeline;
  VkrVulkanPreparedTextDraw *draws;
  uint64_t roots_address;
  uint32_t count;
} VkrVulkanPreparedText;
typedef struct VkrVulkanPreparedUi {
  VkrVulkanPreparedUiDraw *draws;
  uint64_t roots_address;
  uint32_t count;
  uint32_t width;
  uint32_t height;
} VkrVulkanPreparedUi;
typedef struct VkrVulkanPreparedFullscreen {
  VkPipeline pipeline;
  VkrVulkanPushConstants push;
} VkrVulkanPreparedFullscreen;

typedef struct VkrVulkanPreparedUpload {
  VkBuffer source;
  VkBuffer candidates;
  VkBuffer instances;
  VkBuffer state;
  VkBuffer sdsm;
  VkBufferCopy candidate_copies[2];
  VkBufferCopy instance_copies[2];
  uint32_t copy_count;
} VkrVulkanPreparedUpload;

typedef struct VkrVulkanPreparedGraphPass VkrVulkanPreparedGraphPass;

typedef struct VkrVulkanPreparedCaptureCopy {
  VkImage source;
  VkBufferImageCopy2 region;
  VkImageMemoryBarrier2 barrier;
  bool8_t transition;
} VkrVulkanPreparedCaptureCopy;

typedef struct VkrVulkanPreparedReadback {
  VkBufferMemoryBarrier2 barriers[5];
  VkBufferCopy copies[5];
  uint32_t count;
} VkrVulkanPreparedReadback;

typedef struct VkrVulkanFrameSlot {
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkQueryPool timestamp_pool;
  VkrVulkanBuffer readback;
  VkrVulkanBuffer capture_readback;
  VkrVulkanBuffer frame_upload;
  VkrVulkanBuffer candidate_upload;
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
  uint64_t candidate_upload_cursor;
  VkrVulkanTemporalSceneState temporal_scene;
  /** Frame-upload allocation failures this frame. Non-zero means the frame was
   *  rejected for want of upload bytes, not for a malformed packet. */
  uint32_t frame_upload_exhaustions;
  uint64_t world_instances;
  /* CPU rows borrow this completion-protected slot's upload storage. */
  VkrVulkanPreparedDirectDraw *direct_draws;
  uint32_t direct_draw_count;
  uint64_t ui_vertices;
  uint64_t ui_index_offset;
  uint64_t ui_index_size;
  uint64_t gpu_candidate_instances;
  uint64_t transmission_gpu_candidate_instances;
  uint64_t gpu_geometry_rows;
  /** Generation copied into this slot's completion-protected table prefix. */
  uint64_t geometry_table_generation;
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
  uint64_t local_shadow_views;
  uint64_t local_shadow_transmission;
  uint32_t shadow_cascade_count;
  uint32_t local_shadow_view_count;
  uint32_t local_shadow_transmission_view_count;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  uint32_t sh_global_slot;
  bool8_t ibl_ready;
  uint32_t subsurface_texture;
  uint32_t diffuse_volume_texture;
  Vec4 diffuse_volume_origin;
  Vec4 diffuse_volume_inverse_spacing;
  uint32_t diffuse_volume_dimensions[4];
  uint64_t ltc;
  uint64_t sheen;
  uint64_t anisotropy;
  /** Frame-upload display parameters shared by physical presentation roots. */
  uint64_t display_output;
  /** Frame-upload address; valid through this completion-protected slot. */
  uint64_t fog;
  /** Frame-upload params shared by froxel injection/integration/apply. */
  uint64_t froxel_fog;
  VkrFroxelFogGpuParams *froxel_fog_params;
  uint32_t froxel_integrated_texture;
  /** True only while this slot's command buffer contains the one-time SH
      sentinel clear. The renderer-wide state commits after queue submission. */
  bool8_t sh_coefficients_clear_recorded;
  /** True only while this slot's command buffer owns the immutable DFG upload.
   */
  bool8_t dfg_upload_recorded;
  /** True only while this slot's command buffer owns the immutable LTC upload.
   */
  bool8_t ltc_upload_recorded;
  /** True only while this slot's command buffer owns the immutable sheen
   * upload. */
  bool8_t sheen_upload_recorded;
  /** True only while this slot's command buffer owns the anisotropy upload. */
  bool8_t anisotropy_upload_recorded;
  /** Slots this frame's packet references, registered against its submit
      serial once submission succeeds. */
  uint32_t sh_referenced_slots[VKR_FRAME_IBL_PROBE_MAX + 1u];
  uint32_t sh_referenced_slot_count;
  uint64_t picking_request_id;
  uint64_t picking_request_order;
  uint64_t picking_submit_value;
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
  VkrVulkanGraphImageInstance *temporal_surface_input;
  VkrVulkanGraphImageInstance *temporal_surface_output;
  bool8_t temporal_history_valid;
  float32_t motion_blur_interval_scale;
  VkrVulkanGraphImageInstance *ssr_color_input;
  VkrVulkanGraphImageInstance *ssr_color_output;
  VkrVulkanGraphImageInstance *ssr_depth_input;
  VkrVulkanGraphImageInstance *ssr_depth_output;
  VkrVulkanGraphImageInstance *ssr_identity_input;
  VkrVulkanGraphImageInstance *ssr_identity_output;
  bool8_t ssr_history_valid;
  VkrVulkanGraphImageInstance *ssgi_color_input;
  VkrVulkanGraphImageInstance *ssgi_color_output;
  VkrVulkanGraphImageInstance *ssgi_depth_input;
  VkrVulkanGraphImageInstance *ssgi_depth_output;
  VkrVulkanGraphImageInstance *ssgi_identity_input;
  VkrVulkanGraphImageInstance *ssgi_identity_output;
  bool8_t ssgi_history_valid;
  VkrVulkanGraphImageInstance *froxel_history_input;
  VkrVulkanGraphImageInstance *froxel_history_output;
  bool8_t froxel_history_valid;
  /* Lowered once from the selected temporal consumer's predecessor. */
  Mat4 temporal_previous_view_projection;
  uint64_t temporal_previous_frame_index;
  bool8_t fsr31_recorded;
  VkrVulkanPreparedCaptureCopy *capture_copies;
  VkImage picking_readback_image;
  VkBufferImageCopy2 picking_readback_region;
  VkrVulkanPreparedReadback deferred_readback;
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
  /**
   * Lazily-published 2D-array alias used only by exact SH cubemap texel
   * loads.
   * The ordinary sampled slot remains a cube view for filtered IBL
   * sampling.
   */
  VkImageView ibl_sh_texel_view;
  VkrGpuSlotHandle ibl_sh_texel_slot;
  VkImageView storage_views[VKR_VULKAN_TEXTURE_MIP_MAX];
  VkrGpuSlotHandle storage_slots[VKR_VULKAN_TEXTURE_MIP_MAX];
  uint32_t sampler_record_index;
  uint32_t material_reference_count;
  uint32_t ibl_reference_count;
  uint64_t last_use_submit_value;
  VkrAtmosphereBakeResult atmosphere_bake_result;
  uint32_t storage_slot_count;
  bool8_t initialization_pending;
  bool8_t unpublish_requested;
  bool8_t live;
  bool8_t pending_retire;
  bool8_t atmosphere_bake_ready;
} VkrVulkanPublishedTexture;

typedef struct VkrVulkanPendingIblBake {
  VkrTextureHandle equirect;
  VkrTextureHandle source;
  VkrTextureHandle prefilter;
  VkrAtmosphereGpuParams atmosphere_params;
  VkrVulkanBuffer atmosphere_sun_readback;
  uint64_t atmosphere_submit_value;
  bool8_t convert_equirect;
  bool8_t is_atmosphere;
  bool8_t atmosphere_rebuild_luts;
  bool8_t recorded;
  bool8_t submitted;
  bool8_t atmosphere_failed;
  /** Candidate coefficient slot this bake projects into, or VKR_SH_SLOT_BLACK
      when no slot could be reserved. Committed only after a successful submit;
      abandoned if recording or submission fails. */
  uint32_t sh_slot;
  /** Authored deringing exponent, validated and normalized at scene load. */
  float32_t sh_deringing;
} VkrVulkanPendingIblBake;

typedef struct VkrVulkanPreparedIblBake {
  const VkrVulkanPublishedTexture *source;
  const VkrVulkanPublishedTexture *prefilter;
  VkBuffer atmosphere_readback;
  VkrVulkanPreparedCompute atmosphere[4];
  VkrVulkanPreparedCompute conversion;
  VkrVulkanPreparedCompute projection;
  VkrVulkanPreparedCompute mips[VKR_VULKAN_TEXTURE_MIP_MAX];
  uint32_t mip_count;
  uint32_t atmosphere_dispatch_count;
  bool8_t convert;
  bool8_t project;
  bool8_t is_atmosphere;
  bool8_t atmosphere_rebuild_luts;
} VkrVulkanPreparedIblBake;
typedef struct VkrVulkanPreparedIbl {
  VkrVulkanPreparedIblBake *bakes;
  uint32_t count;
  bool8_t clear_coefficients;
  bool8_t projected_any;
} VkrVulkanPreparedIbl;

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

typedef struct VkrVulkanPreparedTextureInitialization {
  const VkrVulkanPublishedTexture *texture;
  const VkrVulkanPendingTextureInitialization *upload;
} VkrVulkanPreparedTextureInitialization;

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
  uint32_t decode_index;
} VkrVulkanSubmeshRange;

typedef struct VkrVulkanPublishedGeometry {
  VkrGeometryHandle handle;
  VkrVulkanBuffer vertices;
  VkrVulkanBuffer indices;
  VkrGpuGeometryRow gpu_row;
  VkrGeometryRangeAllocation ranges;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t decode_count;
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
  VkrVulkanMaterialPublishedRow row;
  VkrPbrProperties pbr;
  float32_t alpha_cutoff;
  VkrMaterialAlphaMode alpha_mode;
  bool8_t double_sided;
  uint32_t texture_record_indices[VKR_TEXTURE_SLOT_COUNT];
  uint8_t pending_texture_count;
  bool8_t live;
} VkrVulkanPublishedMaterial;

struct VkrVulkanPreparedDirectDraw {
  VkrVulkanPublishedGeometry *geometry;
  VkrVulkanPublishedMaterial *material;
  const VkrVulkanSubmeshRange *range;
  uint32_t first_instance;
  uint32_t instance_count;
  VkFrontFace front_face;
  VkCullModeFlags cull_mode;
};

typedef struct VkrVulkanRetiredMaterial {
  uint32_t texture_record_indices[VKR_TEXTURE_SLOT_COUNT];
  uint64_t retire_value;
  bool8_t occupied;
} VkrVulkanRetiredMaterial;

typedef struct VkrVulkanFsrSdk VkrVulkanFsrSdk;

typedef struct VkrVulkanFsr31History {
  Mat4 view_projection;
  VkrVulkanTemporalSceneState scene;
  uint64_t frame_index;
  uint64_t scene_generation;
  uint64_t submit_value;
  uint32_t history_index;
  bool8_t valid;
} VkrVulkanFsr31History;

/**
 * Completion-owned metadata for one physical scattering-history volume. The
 * graph owns its image; this compact record owns only the froxel-specific
 * compatibility proof and the transforms required for reprojection.
 */
typedef struct VkrVulkanFroxelHistory {
  Mat4 view_projection;
  Mat4 view;
  uint64_t signature;
  uint64_t producer_submit_value;
  uint64_t frame_index;
  uint32_t dimensions[3];
  uint32_t scattering_generation;
  uint32_t shadow_generation;
  uint32_t local_shadow_generation;
  uint32_t shadow_valid_layer_mask;
  uint32_t local_shadow_valid_layer_mask;
  uint64_t local_shadow_transmission_generations
      [VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT];
  uint32_t local_shadow_transmission_valid_layer_mask;
  bool8_t valid;
} VkrVulkanFroxelHistory;

/** Completion-owned compatibility proof for one SSGI history tuple. */
typedef struct VkrVulkanSsgiHistory {
  uint64_t producer_submit_value;
  uint64_t frame_index;
  uint32_t dimensions[2];
  uint32_t shadow_generation;
  uint32_t local_shadow_generation;
  uint32_t shadow_valid_layer_mask;
  uint32_t local_shadow_valid_layer_mask;
  uint64_t local_shadow_transmission_generations
      [VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT];
  uint32_t local_shadow_transmission_valid_layer_mask;
  bool8_t valid;
} VkrVulkanSsgiHistory;

struct VkrVulkanRenderer {
  VkrAllocator *allocator;
  VkrVulkanRendererConfig config;
  VkrExposureMeteringConfig exposure_metering;
  /** Bounded simulation time of the last submitted automatic exposure. */
  float64_t exposure_seconds;
  float64_t motion_seconds;
  VkrBloomConfig bloom_config;
  VkrGtaoConfig gtao_config;
  VkrSsrConfig ssr_config;
  VkrSsgiConfig ssgi_config;
  VkrDisplayOutputParams display_output_params;
  VkrDisplayOutputParams display_output_requested;
  uint64_t display_output_snapshot_revision;
  VkrGtaoGpuParams gtao_params;
  VkrVulkanFsrSdk *fsr31;
  uint32_t fsr31_output_width;
  uint32_t fsr31_output_height;
  VkrVulkanFsr31History fsr31_history;
  VkrVulkanFroxelHistory froxel_histories[VKR_VULKAN_HISTORY_INSTANCE_COUNT];
  VkrVulkanSsgiHistory ssgi_histories[VKR_VULKAN_HISTORY_INSTANCE_COUNT];
  /* Independent of command slots: cancelled SDK dispatches advance its rings.
   */
  uint64_t fsr31_dispatch_uses[VKR_VULKAN_FRAME_SLOT_COUNT];
  uint32_t fsr31_dispatch_slot;
  bool8_t fsr31_recreate;
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
  VkrVulkanPreparedGraphPass *prepared_graph_passes;
  VkrVulkanPreparedTextureInitialization *prepared_texture_initializations;
  const VkrVulkanPendingBufferInitialization **prepared_buffer_initializations;
  uint32_t prepared_texture_initialization_count;
  uint32_t prepared_buffer_initialization_count;
  VkDependencyInfo prepared_terminal_barriers;
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
  VkrGpuGeometryRow *geometry_table_rows;
  VkrVulkanGeometryMegabuffer geometry_megabuffer;
  VkrGeometryRanges geometry_ranges;
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
  uint64_t geometry_table_rows_size;
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
  VkrVulkanDirtyRange transmission_material_dirty;
  VkrVulkanBuffer upload;
  VkrVulkanBuffer materials;
  VkDeviceSize transmission_material_offset;
  /** L2 diffuse coefficient slots (ADR-038). Renderer lifetime: the pool
      survives scene reload, and scene reset only retires publications. */
  VkrVulkanBuffer sh_coefficients;
  VkrShSlotPool sh_pool;
  /** Cleared to zero once, so slot 0 is a valid black sentinel and any slot
      referenced before projection reads black rather than stale bytes. */
  bool8_t sh_coefficients_cleared;
  VkrVulkanImage sentinel_image;
  VkSampler sentinel_sampler;
  /** Immutable 256x256 RG16F split-sum lookup. Its upload buffer retires at
      the first successful submit; image and descriptor rows live with the
      renderer. */
  VkrVulkanImage dfg_image;
  VkrVulkanBuffer dfg_upload;
  VkSampler dfg_sampler;
  VkrGpuSlotHandle dfg_texture_slot;
  VkrGpuSlotHandle dfg_sampler_slot;
  uint64_t dfg_upload_retire_value;
  bool8_t dfg_upload_pending;
  /** Immutable matrix/amplitude RGBA16F tables. One shared staging buffer
      retires after their first successful upload; descriptor rows and images
      remain renderer-owned for the device lifetime. */
  VkrVulkanImage ltc_images[VKR_LTC_LUT_TABLE_COUNT];
  VkrVulkanBuffer ltc_upload;
  VkrGpuSlotHandle ltc_texture_slots[VKR_LTC_LUT_TABLE_COUNT];
  uint64_t ltc_upload_retire_value;
  bool8_t ltc_upload_pending;
  /** Immutable Charlie directional-albedo and rectangle-light tables. */
  VkrVulkanImage sheen_directional_albedo_image;
  VkrVulkanImage sheen_ltc_images[VKR_SHEEN_LTC_LUT_TABLE_COUNT];
  VkrVulkanBuffer sheen_upload;
  VkrGpuSlotHandle sheen_texture_slots[1u + VKR_SHEEN_LTC_LUT_TABLE_COUNT];
  uint64_t sheen_upload_retire_value;
  bool8_t sheen_upload_pending;
  /** Three 64-layer RGBA16F anisotropic GGX lookup arrays. The staging buffer
     retires after their first submitted upload; sampled rows persist. */
  VkrVulkanImage anisotropy_images[VKR_ANISOTROPY_LUT_TABLE_COUNT];
  VkrVulkanBuffer anisotropy_upload;
  VkrGpuSlotHandle anisotropy_texture_slots[VKR_ANISOTROPY_LUT_TABLE_COUNT];
  uint64_t anisotropy_upload_retire_value;
  bool8_t anisotropy_upload_pending;
  /** Mutable renderer-lifetime atmosphere lookup cache. Images and descriptor
     rows stay stable; only their contents change with a submitted revision. */
  VkrVulkanImage atmosphere_transmittance;
  VkrVulkanImage atmosphere_multiple_scattering;
  VkImageView atmosphere_storage_views[2];
  VkrGpuSlotHandle atmosphere_sampled_slots[2];
  VkrGpuSlotHandle atmosphere_storage_slots[2];
  VkrAtmosphereGpuParams atmosphere_lut_params;
  uint64_t atmosphere_lut_revision;
  bool8_t atmosphere_lut_valid;
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
  VkShaderModule atmosphere_shaders[VKR_VULKAN_ATMOSPHERE_PIPELINE_COUNT];
  VkPipeline atmosphere_pipelines[VKR_VULKAN_ATMOSPHERE_PIPELINE_COUNT];
  VkShaderModule deferred_shaders[VKR_VULKAN_DEFERRED_PIPELINE_COUNT];
  VkPipeline deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_COUNT];
  VkrVulkanPendingIblBake pending_ibl_bakes[VKR_VULKAN_PENDING_IBL_BAKE_MAX];
  uint32_t pending_ibl_bake_count;
  VkSemaphore timeline;
  uint64_t submit_value;
  uint64_t completed_value;
  VkrPixelReadbackResult picking_completed_result;
  uint64_t picking_request_order;
  uint64_t picking_completed_order;
  uint64_t picking_consumed_order;
  uint64_t candidate_publication_generation;
  uint64_t geometry_table_generation;
  uint64_t radiance_revision;
  uint64_t graph_revision;
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
  VkrRendererError submit_error;
  // One-shot so the bounded publication boundary cannot log per frame.
  bool8_t deferred_candidate_drop_logged;
};

VkDevice vkr_vk_renderer_device(const VkrVulkanRenderer *renderer);
void vkr_vk_record_graph_resource_result(VkrVulkanRenderer *renderer,
                                         VkResult result);
void vkr_vk_advance_radiance_revision(VkrVulkanRenderer *renderer);
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
bool8_t vkr_vk_create_image_ex(
    VkrVulkanRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t depth, uint32_t mip_levels, uint32_t array_layers, VkFormat format,
    VkImageCreateFlags flags, VkImageType image_type, VkImageViewType view_type,
    VkImageUsageFlags usage, VkrGpuAllocationOwner owner,
    VkrVulkanImage *out_image, VkrRendererError *out_error);
bool8_t vkr_vk_create_target_set(VkrVulkanRenderer *renderer, uint32_t width,
                                 uint32_t height, uint32_t image_count,
                                 VkFormat format,
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
                            const VkrPreparedFrame *packet,
                            VkrVulkanFrameSlot *slot);
/* CPU publication admission; recording consumes only these ready rows. */
bool8_t vkr_vk_prepare_direct_draws(VkrVulkanRenderer *renderer,
                                    VkrVulkanFrameSlot *slot,
                                    const VkrWorldPassPayload *world);

bool8_t vkr_vk_prepare_packet_uploads(VkrVulkanRenderer *renderer,
                                      VkrVulkanFrameSlot *slot,
                                      const VkrPreparedFrame *packet);
bool8_t vkr_vk_packet_subsurface_ready(VkrVulkanRenderer *renderer,
                                       const VkrPreparedFrame *packet);
VkrVulkanPacketFrameRoot *vkr_vk_packet_frame_root(VkrVulkanFrameSlot *slot,
                                                   uint64_t *out_address);
void vkr_vk_fill_packet_frame_root(
    VkrVulkanRenderer *renderer, VkrVulkanPacketFrameRoot *root,
    const VkrVulkanFrameSlot *slot, const VkrPacketFrameConstants *frame,
    uint64_t instances, Mat4 view_projection, uint32_t shadow_texture,
    uint32_t transmission_texture, uint32_t local_shadow_texture,
    bool8_t lighting_pass);
bool8_t vkr_vk_publish_sampled_view(VkrVulkanRenderer *renderer,
                                    VkImageView view,
                                    VkImageLayout image_layout,
                                    VkrGpuSlotHandle *out_handle);
void vkr_vk_record_dfg_upload(VkrVulkanRenderer *renderer,
                              VkrVulkanFrameSlot *slot,
                              VkCommandBuffer command);
bool8_t vkr_vk_commit_dfg_upload(VkrVulkanRenderer *renderer,
                                 VkrVulkanFrameSlot *slot,
                                 uint64_t retire_value);
void vkr_vk_collect_dfg_upload(VkrVulkanRenderer *renderer, uint64_t completed);
void vkr_vk_retire_dfg_descriptor_slots(VkrVulkanRenderer *renderer);
void vkr_vk_record_ltc_upload(VkrVulkanRenderer *renderer,
                              VkrVulkanFrameSlot *slot,
                              VkCommandBuffer command);
bool8_t vkr_vk_commit_ltc_upload(VkrVulkanRenderer *renderer,
                                 VkrVulkanFrameSlot *slot,
                                 uint64_t retire_value);
void vkr_vk_collect_ltc_upload(VkrVulkanRenderer *renderer, uint64_t completed);
void vkr_vk_retire_ltc_descriptor_slots(VkrVulkanRenderer *renderer);
void vkr_vk_record_sheen_upload(VkrVulkanRenderer *renderer,
                                VkrVulkanFrameSlot *slot,
                                VkCommandBuffer command);
bool8_t vkr_vk_commit_sheen_upload(VkrVulkanRenderer *renderer,
                                   VkrVulkanFrameSlot *slot,
                                   uint64_t retire_value);
void vkr_vk_collect_sheen_upload(VkrVulkanRenderer *renderer,
                                 uint64_t completed);
void vkr_vk_retire_sheen_descriptor_slots(VkrVulkanRenderer *renderer);
void vkr_vk_record_anisotropy_upload(VkrVulkanRenderer *renderer,
                                     VkrVulkanFrameSlot *slot,
                                     VkCommandBuffer command);
bool8_t vkr_vk_commit_anisotropy_upload(VkrVulkanRenderer *renderer,
                                        VkrVulkanFrameSlot *slot,
                                        uint64_t retire_value);
void vkr_vk_collect_anisotropy_upload(VkrVulkanRenderer *renderer,
                                      uint64_t completed);
void vkr_vk_retire_anisotropy_descriptor_slots(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_create_atmosphere_resources(VkrVulkanRenderer *renderer);
void vkr_vk_destroy_atmosphere_resources(VkrVulkanRenderer *renderer);
void vkr_vk_retire_atmosphere_descriptor_slots(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_publish_sentinel_descriptors(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_publish_storage_view(VkrVulkanRenderer *renderer,
                                    VkImageView view,
                                    VkrGpuSlotHandle *out_handle);
void vkr_vk_record_capture(VkrVulkanRenderer *renderer, VkCommandBuffer command,
                           VkrVulkanFrameSlot *slot);
bool8_t vkr_vk_record_graph(VkrVulkanRenderer *renderer,
                            VkCommandBuffer command);
bool8_t vkr_vk_prepare_fsr31_context(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_prepare_fsr31_inputs(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass);
bool8_t vkr_vk_prepare_fsr31_stabilize(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_prepare_froxel_inject(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_prepare_froxel_integrate(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass);
bool8_t vkr_vk_prepare_froxel_apply(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass);
void vkr_vk_mark_froxel_submitted(VkrVulkanRenderer *renderer,
                                  uint64_t submit_value);
void vkr_vk_cancel_fsr31(VkrVulkanRenderer *renderer);
void vkr_vk_bind_descriptor_buffers(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command);
bool8_t vkr_vk_prepare_deferred_upload(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedUpload *prepared,
                                       const VkrRgPass *pass,
                                       bool8_t transmission);
void vkr_vk_record_deferred_readback(VkrVulkanRenderer *renderer,
                                     VkCommandBuffer command);
bool8_t vkr_vk_prepare_deferred_cull(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass,
                                     VkrVulkanDeferredPipeline pipeline,
                                     bool8_t transmission);
bool8_t vkr_vk_prepare_local_shadow_transmission(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedRaster *prepared,
    const VkrRgPass *pass, bool8_t overflow);
bool8_t vkr_vk_prepare_deferred_raster(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedRaster *prepared,
                                       const VkrRgPass *pass, bool8_t shadow,
                                       bool8_t transmission,
                                       bool8_t local_shadow);
bool8_t vkr_vk_prepare_deferred_gbuffer(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass);
bool8_t vkr_vk_prepare_temporal_transform(VkrVulkanRenderer *renderer,
                                          VkrVulkanPreparedCompute *prepared,
                                          const VkrRgPass *pass);
bool8_t vkr_vk_prepare_deferred_lighting(VkrVulkanRenderer *renderer,
                                         VkrVulkanPreparedCompute *prepared,
                                         const VkrRgPass *pass);
bool8_t vkr_vk_prepare_temporal_resolve(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass);
bool8_t vkr_vk_prepare_deferred_hzb(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssr_depth_base(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssr_depth_mip(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssr_trace(VkrVulkanRenderer *renderer,
                                 VkrVulkanPreparedCompute *prepared,
                                 const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssr_temporal(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssr_composite(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssgi_depth_base(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssgi_depth_mip(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssgi_trace(VkrVulkanRenderer *renderer,
                                  VkrVulkanPreparedCompute *prepared,
                                  const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssgi_temporal(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_prepare_ssgi_composite(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass);
bool8_t vkr_vk_prepare_fog_apply(VkrVulkanRenderer *renderer,
                                 VkrVulkanPreparedCompute *prepared,
                                 const VkrRgPass *pass);
void vkr_vk_mark_ssr_submitted(VkrVulkanRenderer *renderer,
                               uint64_t submit_value);
void vkr_vk_mark_ssgi_submitted(VkrVulkanRenderer *renderer,
                                uint64_t submit_value);
bool8_t vkr_vk_prepare_exposure_histogram(VkrVulkanRenderer *renderer,
                                          VkrVulkanPreparedCompute *prepared,
                                          const VkrRgPass *pass);
bool8_t vkr_vk_prepare_exposure_resolve(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass);
/** Publishes this frame's adaptation record once its submit value is known. */
void vkr_vk_mark_exposure_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value);
bool8_t vkr_vk_prepare_subsurface(VkrVulkanRenderer *renderer,
                                  VkrVulkanPreparedCompute *prepared,
                                  const VkrRgPass *pass);
bool8_t vkr_vk_prepare_motion_blur(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass,
                                    VkrVulkanDeferredPipeline pipeline);
bool8_t vkr_vk_prepare_dof(VkrVulkanRenderer *renderer,
                            VkrVulkanPreparedCompute *prepared,
                            const VkrRgPass *pass,
                            VkrVulkanDeferredPipeline pipeline);
bool8_t vkr_vk_prepare_bloom_prefilter(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_prepare_bloom_downsample(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass);
bool8_t
vkr_vk_prepare_transmission_downsample(VkrVulkanRenderer *renderer,
                                       VkrVulkanPreparedCompute *prepared,
                                       const VkrRgPass *pass);
bool8_t vkr_vk_prepare_bloom_upsample(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass);
bool8_t vkr_vk_prepare_bloom_combine(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_prepare_gtao_depth_prefilter(VkrVulkanRenderer *renderer,
                                            VkrVulkanPreparedCompute *prepared,
                                            const VkrRgPass *pass);
bool8_t vkr_vk_prepare_gtao_depth_mip(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedCompute *prepared,
                                      const VkrRgPass *pass);
bool8_t vkr_vk_prepare_gtao_evaluate(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_prepare_gtao_denoise(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedCompute *prepared,
                                    const VkrRgPass *pass);
bool8_t vkr_vk_prepare_deferred_sdsm(VkrVulkanRenderer *renderer,
                                     VkrVulkanPreparedCompute *prepared,
                                     const VkrRgPass *pass);
bool8_t vkr_vk_prepare_deferred_picking(VkrVulkanRenderer *renderer,
                                        VkrVulkanPreparedCompute *prepared,
                                        const VkrRgPass *pass);
bool8_t vkr_vk_prepare_deferred_transmission(VkrVulkanRenderer *renderer,
                                             VkrVulkanPreparedCompute *prepared,
                                             const VkrRgPass *pass);
bool8_t
vkr_vk_prepare_deferred_transmission_compact(VkrVulkanRenderer *renderer,
                                             VkrVulkanPreparedCompute *prepared,
                                             const VkrRgPass *pass);
bool8_t vkr_vk_prepare_deferred_transmission_coverage(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedCompute *prepared,
    const VkrRgPass *pass);
void vkr_vk_mark_hzb_submitted(VkrVulkanRenderer *renderer,
                               uint64_t submit_value);
void vkr_vk_mark_temporal_submitted(VkrVulkanRenderer *renderer,
                                    uint64_t submit_value);
void vkr_vk_record_ibl_bakes(VkrVulkanRenderer *renderer,
                             VkCommandBuffer command,
                             const VkrVulkanPreparedIbl *prepared);
void vkr_vk_abandon_ibl_bake_recordings(VkrVulkanRenderer *renderer);
void vkr_vk_fail_picking_readback(VkrVulkanRenderer *renderer,
                                  VkrVulkanFrameSlot *slot);
void vkr_vk_discard_unsubmitted_asset_uses(VkrVulkanRenderer *renderer);
void vkr_vk_discard_ibl_bakes(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_prepare_editor_overlay(VkrVulkanRenderer *renderer,
                                      VkrVulkanPreparedOverlay *out,
                                      bool8_t picking);
void vkr_vk_record_editor_overlay(VkrVulkanRenderer *renderer,
                                  VkCommandBuffer command,
                                  const VkrVulkanPreparedOverlay *overlay);

bool8_t vkr_vk_prepare_packet_draws(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedWorldDraws *out,
    VkrVulkanPacketPipeline pipeline, uint64_t instances, Mat4 view_projection,
    uint32_t target_width, uint32_t target_height, uint32_t shadow_texture,
    uint32_t transmission_texture, uint32_t local_shadow_texture);

bool8_t vkr_vk_prepare_packet_fullscreen(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedFullscreen *out,
    VkrVulkanPacketPipeline pipeline, uint32_t texture_index,
    uint64_t exposure_state, uint32_t flags, bool8_t composite,
    uint32_t output_width, uint32_t output_height);
bool8_t vkr_vk_prepare_text_draws(VkrVulkanRenderer *renderer,
                                  VkrVulkanPreparedText *out,
                                  VkrVulkanPacketPipeline pipeline,
                                  const VkrPreparedTextDraw *draws,
                                  uint32_t draw_count, Mat4 view_projection,
                                  uint32_t target_width, uint32_t target_height,
                                  bool8_t ui_domain);
bool8_t vkr_vk_prepare_ui_draw_list(VkrVulkanRenderer *renderer,
                                    VkrVulkanPreparedUi *out,
                                    const VkrPreparedUiDrawList *draw_list,
                                    uint32_t target_width,
                                    uint32_t target_height);
bool8_t vkr_vk_recreate_window_target(VkrVulkanRenderer *renderer,
                                      uint32_t width, uint32_t height,
                                      uint32_t image_count);
bool8_t vkr_vk_recreate_presentation_pipelines(VkrVulkanRenderer *renderer,
                                                VkFormat color_format);
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
bool8_t vkr_vk_reserve_frame_uploads(VkrVulkanRenderer *renderer,
                                     VkrVulkanFrameSlot *slot,
                                     uint64_t direct_bytes,
                                     uint64_t candidate_bytes);
uint64_t vkr_vk_graph_upload_bound(VkrVulkanRenderer *renderer,
                                   uint64_t direct_draw_bytes,
                                   uint64_t text_bytes, uint64_t ui_root_bytes);
void vkr_vk_cmd_image_barrier(VkCommandBuffer command_buffer, VkImage image,
                              VkPipelineStageFlags2 src_stage,
                              VkAccessFlags2 src_access,
                              VkPipelineStageFlags2 dst_stage,
                              VkAccessFlags2 dst_access,
                              VkImageLayout old_layout,
                              VkImageLayout new_layout);
void vkr_vk_cmd_image_barrier_range(
    VkCommandBuffer command_buffer, VkImage image,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
    VkImageLayout old_layout, VkImageLayout new_layout, uint32_t level_count,
    uint32_t layer_count);
void vkr_vk_collect_retired_window_targets(VkrVulkanRenderer *renderer,
                                           uint64_t completed_submit_value);
void vkr_vk_discard_buffer_initializations(VkrVulkanRenderer *renderer);
void vkr_vk_discard_texture_initializations(VkrVulkanRenderer *renderer);
bool8_t vkr_vk_prepare_initializations(VkrVulkanRenderer *renderer);
void vkr_vk_record_buffer_initializations(VkrVulkanRenderer *renderer,
                                          VkCommandBuffer command);
void vkr_vk_record_texture_initializations(VkrVulkanRenderer *renderer,
                                           VkCommandBuffer command);

void vkr_vk_record_world_draws(VkrVulkanRenderer *renderer,
                               VkCommandBuffer command,
                               const VkrVulkanPreparedWorldDraws *draws);

void vkr_vk_record_text(VkrVulkanRenderer *renderer, VkCommandBuffer command,
                        const VkrVulkanPreparedText *text);

void vkr_vk_record_ui(VkrVulkanRenderer *renderer, VkCommandBuffer command,
                      const VkrVulkanPreparedUi *ui);

void vkr_vk_record_fullscreen(VkrVulkanRenderer *renderer,
                              VkCommandBuffer command,
                              const VkrVulkanPreparedFullscreen *fullscreen);

bool8_t vkr_vk_prepare_deferred_readback(VkrVulkanRenderer *renderer);

void vkr_vk_record_prepared_compute(VkrVulkanRenderer *renderer,
                                    VkCommandBuffer command,
                                    const VkrVulkanPreparedCompute *prepared);

void vkr_vk_record_prepared_raster(VkrVulkanRenderer *renderer,
                                   VkCommandBuffer command,
                                   const VkrVulkanPreparedRaster *prepared);

void vkr_vk_record_prepared_upload(VkCommandBuffer command,
                                   const VkrVulkanPreparedUpload *prepared);

bool8_t vkr_vk_prepare_graph(VkrVulkanRenderer *renderer);

bool8_t vkr_vk_prepare_ibl_bakes(VkrVulkanRenderer *renderer,
                                 VkrVulkanPreparedIbl *prepared);

bool8_t vkr_vk_prepare_capture(VkrVulkanRenderer *renderer,

                               VkrVulkanFrameSlot *slot);

#endif

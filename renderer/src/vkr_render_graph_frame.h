#pragma once

#include "vkr_bloom.h"
#include "vkr_gtao.h"
#include "vkr_prepared_frame.h"
#include "vkr_render_graph.h"
#include "vkr_render_graph_internal.h"

/* Derive portable graph conditions and GTAO constants from prepared inputs.
 * Acquisition supplies the target extent. Native formats, resource instances,
 * history validity and completion state remain owned by the caller. */
void vkr_render_graph_prepare_frame(const VkrPreparedFrame *packet,
                                    const VkrBloomConfig *bloom_config,
                                    const VkrGtaoConfig *gtao_config,
                                    VkrRenderGraphFrameInfo *frame,
                                    VkrGtaoGpuParams *gtao_params);

/*
 * Portable executor catalog. Both backends register every entry, so a graph
 * description resolves to the same executor ids on either; a backend rejects
 * the entries it cannot execute when it validates the compiled graph. A
 * registered executor id is its kind plus one.
 */
typedef enum VkrRgExecutorKind {
  VKR_RG_EXECUTOR_SHADOW = 0,
  VKR_RG_EXECUTOR_LOCAL_SHADOW,
  VKR_RG_EXECUTOR_LOCAL_SHADOW_TRANSMISSION0,
  VKR_RG_EXECUTOR_LOCAL_SHADOW_TRANSMISSION1,
  VKR_RG_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW,
  VKR_RG_EXECUTOR_PICKING,
  VKR_RG_EXECUTOR_PICKING_DEPTH_SEED,
  VKR_RG_EXECUTOR_PICKING_RESOLVE,
  VKR_RG_EXECUTOR_PICKING_READBACK,
  VKR_RG_EXECUTOR_IBL_BAKE,
  VKR_RG_EXECUTOR_GPU_DRAW_UPLOAD,
  VKR_RG_EXECUTOR_GPU_DRAW_CLASSIFY,
  VKR_RG_EXECUTOR_GPU_DRAW_PREFIX,
  VKR_RG_EXECUTOR_GPU_DRAW_ENCODE,
  VKR_RG_EXECUTOR_SKINNING,
  VKR_RG_EXECUTOR_TEMPORAL_TRANSFORM,
  VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD,
  VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY,
  VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX,
  VKR_RG_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE,
  VKR_RG_EXECUTOR_TRANSMISSION_DEPTH_SEED,
  VKR_RG_EXECUTOR_VBUFFER_OPAQUE,
  VKR_RG_EXECUTOR_VBUFFER_TRANSMISSION,
  VKR_RG_EXECUTOR_GBUFFER_RESOLVE,
  VKR_RG_EXECUTOR_GTAO_DEPTH_PREFILTER,
  VKR_RG_EXECUTOR_GTAO_DEPTH_MIP,
  VKR_RG_EXECUTOR_GTAO_EVALUATE,
  VKR_RG_EXECUTOR_GTAO_DENOISE,
  VKR_RG_EXECUTOR_LIGHTING_DEFERRED,
  VKR_RG_EXECUTOR_SSGI_DEPTH_BASE,
  VKR_RG_EXECUTOR_SSGI_DEPTH_MIP,
  VKR_RG_EXECUTOR_SSGI_TRACE,
  VKR_RG_EXECUTOR_SSGI_TEMPORAL,
  VKR_RG_EXECUTOR_SSGI_COMPOSITE,
  VKR_RG_EXECUTOR_SSR_DEPTH_BASE,
  VKR_RG_EXECUTOR_SSR_DEPTH_MIP,
  VKR_RG_EXECUTOR_SSR_TRACE,
  VKR_RG_EXECUTOR_SSR_TEMPORAL,
  VKR_RG_EXECUTOR_SSR_COMPOSITE,
  VKR_RG_EXECUTOR_FOG_APPLY,
  VKR_RG_EXECUTOR_FROXEL_INJECT,
  VKR_RG_EXECUTOR_FROXEL_INTEGRATE,
  VKR_RG_EXECUTOR_FROXEL_APPLY,
  VKR_RG_EXECUTOR_SKY_VIEW_LUT,
  VKR_RG_EXECUTOR_AERIAL_PERSPECTIVE,
  VKR_RG_EXECUTOR_CLOUD_SHADOW,
  VKR_RG_EXECUTOR_CLOUD_TRACE,
  VKR_RG_EXECUTOR_TEMPORAL_RESOLVE,
  VKR_RG_EXECUTOR_METALFX_STAGE,
  VKR_RG_EXECUTOR_METALFX_TEMPORAL,
  VKR_RG_EXECUTOR_METALFX_STABILIZE,
  VKR_RG_EXECUTOR_EXPOSURE_HISTOGRAM,
  VKR_RG_EXECUTOR_EXPOSURE_RESOLVE,
  VKR_RG_EXECUTOR_SUBSURFACE_GATHER,
  VKR_RG_EXECUTOR_MOTION_BLUR_TILE_MAX,
  VKR_RG_EXECUTOR_MOTION_BLUR_NEIGHBOR_MAX,
  VKR_RG_EXECUTOR_MOTION_BLUR_RECONSTRUCT,
  VKR_RG_EXECUTOR_DOF_COC,
  VKR_RG_EXECUTOR_DOF_DILATE_HORIZONTAL,
  VKR_RG_EXECUTOR_DOF_DILATE_VERTICAL,
  VKR_RG_EXECUTOR_DOF_PREFILTER,
  VKR_RG_EXECUTOR_DOF_GATHER,
  VKR_RG_EXECUTOR_DOF_COMPOSITE,
  VKR_RG_EXECUTOR_BLOOM_PREFILTER,
  VKR_RG_EXECUTOR_BLOOM_DOWNSAMPLE,
  VKR_RG_EXECUTOR_BLOOM_UPSAMPLE,
  VKR_RG_EXECUTOR_BLOOM_COMBINE,
  VKR_RG_EXECUTOR_TRANSMISSION_DOWNSAMPLE,
  VKR_RG_EXECUTOR_TRANSMISSION_SHADE,
  VKR_RG_EXECUTOR_TRANSMISSION_COVERAGE,
  VKR_RG_EXECUTOR_TRANSMISSION_COMPACT,
  VKR_RG_EXECUTOR_SDSM_REDUCE,
  VKR_RG_EXECUTOR_HZB_BUILD,
  VKR_RG_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN,
  VKR_RG_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR,
  VKR_RG_EXECUTOR_WORLD_BLEND,
  VKR_RG_EXECUTOR_TONEMAP,
  VKR_RG_EXECUTOR_TONEMAP_PREPARE,
  VKR_RG_EXECUTOR_EDITOR,
  VKR_RG_EXECUTOR_EDITOR_CLEAR,
  VKR_RG_EXECUTOR_EDITOR_OVERLAY,
  VKR_RG_EXECUTOR_EDITOR_OVERLAY_PICKING,
  VKR_RG_EXECUTOR_ANIMATION_PREVIEW,
  VKR_RG_EXECUTOR_UI,
  VKR_RG_EXECUTOR_FSR31_PREPARE,
  VKR_RG_EXECUTOR_FSR31_UPSCALE,
  VKR_RG_EXECUTOR_FSR31_STABILIZE,
  VKR_RG_EXECUTOR_COUNT,
} VkrRgExecutorKind;

/** Registers the catalog with ids equal to kind plus one. */
VKR_MUST_USE bool8_t
vkr_render_graph_register_executors(VkrRgExecutorRegistry *registry);

/** Resolves a compiled pass to its catalog kind. Called per pass. */
static INLINE bool8_t vkr_render_graph_executor_kind(
    const VkrRgPass *pass, VkrRgExecutorKind *out_kind) {
  if (!pass || !out_kind || pass->desc.executor_id == 0u ||
      pass->desc.executor_id > VKR_RG_EXECUTOR_COUNT) {
    return false_v;
  }
  *out_kind = (VkrRgExecutorKind)(pass->desc.executor_id - 1u);
  return true_v;
}

/** True when `pass` runs the executor of `kind`. Called per pass. */
static INLINE bool8_t vkr_render_graph_pass_is(const VkrRgPass *pass,
                                               VkrRgExecutorKind kind) {
  return pass && pass->desc.executor_id == (uint32_t)kind + 1u;
}

/** Catalog pass name of `kind`, for diagnostics. */
const char *vkr_render_graph_executor_name(VkrRgExecutorKind kind);

/** Pass type the executor of `kind` requires. */
VkrRgPassType vkr_render_graph_executor_type(VkrRgExecutorKind kind);

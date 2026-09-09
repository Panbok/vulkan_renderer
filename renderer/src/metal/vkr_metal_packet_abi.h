#pragma once

#include <stddef.h>

#include "math/mat.h"
#include "math/vec.h"
#include "metal/vkr_metal_material_table.h"
#include "vkr_bloom.h"
#include "vkr_dof.h"
#include "vkr_subsurface.h"
#include "vkr_motion_blur.h"
#include "vkr_atmosphere.h"
#include "vkr_buffer.h"
#include "vkr_display_output.h"
#include "vkr_exposure.h"
#include "vkr_gpu_abi.h"
#include "vkr_gtao.h"
#include "vkr_lighting.h"
#include "vkr_ssr.h"
#include "vkr_ssgi.h"
#include "vkr_fog.h"
#include "vkr_froxel_fog.h"

enum {
  VKR_METAL_PACKET_ROOT_ALIGNMENT = 256,
  VKR_METAL_PACKET_DRAW_ROOT_STRIDE = 512,
  VKR_METAL_PACKET_TRANSMISSION_LAYER_COUNT = 4,
  VKR_METAL_PACKET_TRANSMISSION_DIAGNOSTIC_LAYER_COUNT = 5,
};

/** Converts VKR's column-major matrix for Slang's row-vector MSL lowering. */
vkr_internal INLINE Mat4 vkr_metal_packet_slang_draw_matrix(Mat4 matrix) {
  return mat4_transpose(matrix);
}

typedef struct VKR_SIMD_ALIGN VkrMetalPacketTemporalDrawState {
  uint64_t previous_transforms;
  uint32_t previous_transform_address_padding[2];
  Mat4 current_view_projection;
  Mat4 previous_view_projection;
  uint32_t history_valid;
  uint32_t previous_frame_index;
  uint32_t reserved[2];
} VkrMetalPacketTemporalDrawState;

/** Values shared by every indexed draw encoded for one pass. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketDiffuseVolume {
  Vec4 origin;
  Vec4 inverse_spacing;
  uint32_t dimensions[4];
} VkrMetalPacketDiffuseVolume;
_Static_assert(sizeof(VkrMetalPacketDiffuseVolume) == 48u, "Metal diffuse volume parameters ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketLtc {
  uint64_t lights;
  uint64_t matrix_texture;
  uint64_t amplitude_texture;
  uint32_t light_count;
  uint32_t reserved;
} VkrMetalPacketLtc;
_Static_assert(sizeof(VkrMetalPacketLtc) == 32u, "Metal LTC ABI drift");
_Static_assert(_Alignof(VkrMetalPacketLtc) == 16u,
               "Metal LTC ABI alignment drift");

/** Fixed native sampler keeps five immutable Charlie texture IDs in 48 B. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSheen {
  uint64_t directional_albedo_texture;
  uint64_t ltc_matrix_texture;
  uint64_t ltc_amplitude_texture;
  uint64_t ltc_matrix_texture_b;
  uint64_t ltc_amplitude_texture_b;
  uint32_t reserved[2];
} VkrMetalPacketSheen;
_Static_assert(sizeof(VkrMetalPacketSheen) == 48u,
               "Metal sheen ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketAnisotropy {
  uint64_t table0;
  uint64_t table1;
  uint64_t table2;
  uint64_t reserved;
} VkrMetalPacketAnisotropy;
_Static_assert(sizeof(VkrMetalPacketAnisotropy) == 32u,
               "Metal anisotropy ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketFrameRoot {
  uint64_t instances;
  uint32_t instance_address_padding[2];
  Mat4 view_projection;
  uint64_t materials;
  /** ADR-038 final layout: the retired diffuse-cubemap reference became the
      coefficient buffer address, and the retired BRDF-LUT slot became the
      global slot plus reserved word. Later offsets are unchanged. */
  uint64_t sh_coefficients_address;
  uint64_t prefilter_texture_id;
  uint32_t sh_global_slot;
  uint32_t sh_reserved;
  Vec4 view_position;
  uint32_t prefilter_mip_count;
  uint32_t flags;
  uint64_t froxel_fog;
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  Vec4 ambient_color;
  uint32_t render_mode;
  uint32_t shadow_debug_mode;
  uint64_t froxel_integrated_texture_id;
  uint64_t point_light_data;
  uint64_t point_light_masks;
  Vec4 point_light_grid_origin_cell_size;
  uint32_t point_light_grid_dimensions_count[4];
  VkrPointLightMask point_light_global_mask;
  uint32_t point_light_count;
  uint32_t point_light_reserved[3];
  uint64_t shadow_texture_id;
  uint64_t shadow_cascades;
  Mat4 view;
  uint32_t shadow_cascade_count;
  /* Receiver quality, mirroring VkrShadowReceiverPacketData. Contiguous and
     scalar rather than packed into Vec4s: Metal aligns a float4 to 16 bytes and
     would open padding holes between the frame root's neighbouring handles. */
  uint32_t shadow_pcf_sample_count;
  float32_t shadow_receiver_bias_texels;
  float32_t shadow_slope_bias_texels;
  float32_t shadow_normal_offset_texels;
  float32_t shadow_pcf_radius_texels;
  float32_t shadow_cascade_blend_fraction;
  float32_t shadow_fade_start;
  float32_t shadow_fade_end;
  uint32_t shadow_pcf_uniform_early_out;
  uint64_t transmission_texture_id;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t ibl_probe_reserved;
  uint64_t temporal_draw_state;
  uint64_t local_shadow_texture_id;
  uint64_t local_shadow_views;
  uint64_t dfg_texture_id;
  uint64_t diffuse_volume_texture_id;
  uint64_t diffuse_volume_params;
  uint64_t ltc;
  uint64_t fog;
  uint64_t sheen;
  uint64_t anisotropy;
} VkrMetalPacketFrameRoot;

_Static_assert(offsetof(VkrMetalPacketFrameRoot, dfg_texture_id) == 472u,
               "Metal DFG texture ID ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, diffuse_volume_params) == 488u,
               "Metal diffuse-volume parameters ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, ltc) == 496u,
               "Metal LTC parameters ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, fog) == 504u,
               "Metal fog parameter ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, sheen) == 512u,
               "Metal sheen parameter ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, anisotropy) == 520u,
               "Metal anisotropy ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, froxel_fog) == 136u,
               "Metal froxel parameters ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, froxel_integrated_texture_id) ==
                   216u,
               "Metal froxel integrated texture ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketFrameRoot) == 528u,
               "Metal frame root ABI size drift");

/* Frame records are cold, shared records. They may span the fixed draw-root
 * cells, while every per-draw record remains one 512-byte cell. */
enum {
  VKR_METAL_PACKET_FRAME_ROOT_CELL_COUNT =
      (sizeof(VkrMetalPacketFrameRoot) + VKR_METAL_PACKET_DRAW_ROOT_STRIDE -
       1u) /
      VKR_METAL_PACKET_DRAW_ROOT_STRIDE,
};
_Static_assert(VKR_METAL_PACKET_FRAME_ROOT_CELL_COUNT == 2u,
               "Metal frame root cell count must be reviewed on ABI growth");
_Static_assert(sizeof(VkrMetalPacketFrameRoot) <=
                   (uint64_t)VKR_METAL_PACKET_FRAME_ROOT_CELL_COUNT *
                       VKR_METAL_PACKET_DRAW_ROOT_STRIDE,
               "Metal frame root cell span is too small");

/** Native unlit editor handle; pointers address packed published geometry. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketEditorOverlayRoot {
  uint64_t vertices;
  uint64_t decode;
  Mat4 model_view_projection;
  Vec4 color;
  uint32_t object_id;
  uint32_t reserved;
  uint64_t display_output;
} VkrMetalPacketEditorOverlayRoot;

_Static_assert(offsetof(VkrMetalPacketEditorOverlayRoot, display_output) == 104u,
               "Metal editor overlay display-output ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketEditorOverlayRoot) == 112,
               "Metal editor overlay root ABI size drift");

/** The only record written per indexed packet draw. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketDrawRoot {
  uint64_t geometry_rows;
  uint64_t visible_rows;
  uint64_t vertices;
  uint64_t frame;
  uint32_t visible_row_index;
  uint32_t flags;
  uint32_t reserved[2];
} VkrMetalPacketDrawRoot;

typedef VkrMetalPacketDrawRoot VkrMetalPacketVertexDrawRoot;

enum {
  VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX = 25u,
};

/** One frustum and routing policy in the bounded multi-view cull set. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketGpuDrawView {
  Vec4 frustum_planes[6];
  uint32_t required_candidate_flags;
  uint32_t hzb_enabled;
  uint32_t reserved[2];
} VkrMetalPacketGpuDrawView;

_Static_assert(sizeof(VkrMetalPacketGpuDrawView) == 112,
               "Metal GPU draw view ABI must remain 112 bytes");

/** Shared root for the classify, prefix, and GPU ICB-encoding kernels. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketGpuDrawRoot {
  uint64_t candidates;
  uint64_t geometry_rows;
  uint64_t instances;
  uint64_t classifications;
  uint64_t compaction_state;
  uint64_t visible_rows;
  uint64_t draw_roots;
  uint64_t views;
  uint32_t candidate_count;
  uint32_t visible_capacity;
  uint32_t view_count;
  uint32_t encode_view_index;
  uint64_t hzb_texture_id;
  uint64_t reserved_2;
  Mat4 history_view_projection;
  uint32_t hzb_extent[2];
  uint32_t hzb_mip_count;
  uint32_t hzb_enabled;
  float32_t hzb_depth_epsilon;
  uint32_t icb_view_group_size;
  uint32_t reserved_3[2];
} VkrMetalPacketGpuDrawRoot;

_Static_assert(sizeof(VkrMetalPacketGpuDrawRoot) == 192,
               "Metal GPU draw root ABI must remain 192 bytes");

/** Per-peel visibility raster state bound outside GPU-encoded draw commands. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketTransmissionPeelRoot {
  uint64_t previous_depth_texture_id;
  float32_t depth_epsilon;
  uint32_t previous_depth_enabled;
} VkrMetalPacketTransmissionPeelRoot;

_Static_assert(sizeof(VkrMetalPacketTransmissionPeelRoot) == 16,
               "Metal transmission-peel root ABI must remain 16 bytes");

/** One HZB base/reduction dispatch over explicit source/destination views. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketHzbBuildRoot {
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t source_is_depth;
  uint32_t reserved[3];
} VkrMetalPacketHzbBuildRoot;

_Static_assert(sizeof(VkrMetalPacketHzbBuildRoot) == 48,
               "Metal HZB build root ABI must remain 48 bytes");

/** Explicit-subresource GTAO depth prefilter/reduction root. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketGtaoDepthRoot {
  VkrGtaoGpuParams params;
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
} VkrMetalPacketGtaoDepthRoot;

_Static_assert(sizeof(VkrMetalPacketGtaoDepthRoot) == 224,
               "Metal GTAO depth root ABI must remain 224 bytes");

/** Full-resolution GTAO horizon evaluation resources. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketGtaoEvaluateRoot {
  VkrGtaoGpuParams params;
  uint64_t vbuffer_texture_id;
  uint64_t view_depth_texture_id;
  uint64_t normal_texture_id;
  uint64_t destination_texture_id;
  uint64_t edges_texture_id;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t reserved[2];
} VkrMetalPacketGtaoEvaluateRoot;

_Static_assert(sizeof(VkrMetalPacketGtaoEvaluateRoot) == 256,
               "Metal GTAO evaluate root ABI must remain 256 bytes");

/** Full-resolution edge-aware GTAO spatial denoise resources. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketGtaoDenoiseRoot {
  VkrGtaoGpuParams params;
  uint64_t source_texture_id;
  uint64_t edges_texture_id;
  uint64_t destination_texture_id;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
  uint32_t reserved[2];
} VkrMetalPacketGtaoDenoiseRoot;

_Static_assert(sizeof(VkrMetalPacketGtaoDenoiseRoot) == 240,
               "Metal GTAO denoise root ABI must remain 240 bytes");

/** Half-resolution current-frame depth and receiver selection for SSR. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketFogRoot {
  VkrFogGpuParams params;
  Mat4 inverse_view_projection;
  Vec4 camera_position;
  uint64_t depth_texture_id;
  uint64_t target_texture_id;
  uint32_t extent[2];
  uint32_t reserved[2];
} VkrMetalPacketFogRoot;

_Static_assert(sizeof(VkrMetalPacketFogRoot) == 144u, "Metal fog root ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketFroxelInjectRoot {
  uint64_t frame;
  uint64_t params;
  uint64_t history_texture_id;
  uint64_t output_texture_id;
  uint32_t history_valid;
  uint32_t extent[3];
} VkrMetalPacketFroxelInjectRoot;

_Static_assert(sizeof(VkrMetalPacketFroxelInjectRoot) == 48u,
               "Metal froxel inject root ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketFroxelIntegrateRoot {
  uint64_t frame;
  uint64_t params;
  uint64_t scattering_texture_id;
  uint64_t integrated_texture_id;
  uint32_t extent[3];
  uint32_t reserved;
} VkrMetalPacketFroxelIntegrateRoot;

_Static_assert(sizeof(VkrMetalPacketFroxelIntegrateRoot) == 48u,
               "Metal froxel integrate root ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketFroxelApplyRoot {
  uint64_t frame;
  uint64_t params;
  uint64_t depth_texture_id;
  uint64_t integrated_texture_id;
  uint64_t target_texture_id;
  uint32_t extent[2];
} VkrMetalPacketFroxelApplyRoot;

_Static_assert(sizeof(VkrMetalPacketFroxelApplyRoot) == 48u,
               "Metal froxel apply root ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsgiDepthBaseRoot {
  VkrSsgiGpuParams params;
  uint64_t depth_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t pyramid_texture_id;
} VkrMetalPacketSsgiDepthBaseRoot;

_Static_assert(sizeof(VkrMetalPacketSsgiDepthBaseRoot) == 320u,
               "Metal SSGI depth-base root ABI must remain 320 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsgiDepthMipRoot {
  VkrSsgiGpuParams params;
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
} VkrMetalPacketSsgiDepthMipRoot;

_Static_assert(sizeof(VkrMetalPacketSsgiDepthMipRoot) == 320u,
               "Metal SSGI depth-mip root ABI must remain 320 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsgiTraceRoot {
  VkrSsgiGpuParams params;
  uint64_t depth_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t normal_texture_id;
  uint64_t albedo_texture_id;
  uint64_t pyramid_texture_id;
  uint64_t direct_source_texture_id;
  uint64_t raw_texture_id;
} VkrMetalPacketSsgiTraceRoot;

_Static_assert(sizeof(VkrMetalPacketSsgiTraceRoot) == 352u,
               "Metal SSGI trace root ABI must remain 352 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsgiTemporalRoot {
  VkrSsgiGpuParams params;
  uint64_t raw_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t depth_texture_id;
  uint64_t normal_texture_id;
  uint64_t motion_texture_id;
  uint64_t validity_texture_id;
  uint64_t history_color_texture_id;
  uint64_t history_depth_texture_id;
  uint64_t history_identity_texture_id;
  uint64_t output_color_texture_id;
  uint64_t output_depth_texture_id;
  uint64_t output_identity_texture_id;
  uint64_t visible_rows;
  uint64_t instances;
} VkrMetalPacketSsgiTemporalRoot;

_Static_assert(sizeof(VkrMetalPacketSsgiTemporalRoot) == 400u,
               "Metal SSGI temporal root ABI must remain 400 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsgiCompositeRoot {
  uint64_t frame;
  VkrSsgiGpuParams params;
  uint64_t hdr_texture_id;
  uint64_t history_color_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t depth_texture_id;
  uint64_t albedo_texture_id;
  uint64_t normal_texture_id;
  uint64_t history_depth_texture_id;
  uint64_t specular_texture_id;
  Mat4 inverse_view_projection;
  uint32_t extent[2];
  uint32_t subsurface_profile_count;
  uint32_t reserved;
  uint64_t clearcoat_texture_id;
  uint64_t sheen_texture_id;
  uint64_t anisotropy_texture_id;
  uint32_t visible_rows_padding[2];
  uint64_t visible_rows;
  uint64_t subsurface_source_texture_id;
} VkrMetalPacketSsgiCompositeRoot;
_Static_assert(offsetof(VkrMetalPacketSsgiCompositeRoot, subsurface_source_texture_id) == 488u &&
                   offsetof(VkrMetalPacketSsgiCompositeRoot, subsurface_profile_count) == 440u,
               "Subsurface source producer ABI drift");

_Static_assert(sizeof(VkrMetalPacketSsgiCompositeRoot) == 496u,
               "Metal SSGI composite root ABI must remain 496 bytes");
_Static_assert(offsetof(VkrMetalPacketSsgiCompositeRoot,
                        clearcoat_texture_id) == 448u,
               "Metal SSGI clearcoat ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketSsgiCompositeRoot, sheen_texture_id) ==
                   456u,
               "Metal SSGI sheen ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketSsgiCompositeRoot,
                        anisotropy_texture_id) == 464u,
               "Metal SSGI anisotropy ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketSsgiCompositeRoot, visible_rows) == 480u,
               "Metal SSGI visible-row ABI offset drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsrDepthBaseRoot {
  VkrSsrGpuParams params;
  uint64_t depth_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t pyramid_texture_id;
  uint64_t receiver_texture_id;
} VkrMetalPacketSsrDepthBaseRoot;

_Static_assert(sizeof(VkrMetalPacketSsrDepthBaseRoot) == 320,
               "Metal SSR depth-base root ABI must remain 320 bytes");

/** One explicit subresource reduction in the current-frame SSR pyramid. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsrDepthMipRoot {
  VkrSsrGpuParams params;
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  uint32_t source_extent[2];
  uint32_t destination_extent[2];
} VkrMetalPacketSsrDepthMipRoot;

_Static_assert(sizeof(VkrMetalPacketSsrDepthMipRoot) == 320,
               "Metal SSR depth-mip root ABI must remain 320 bytes");

/** Half-resolution SSR trace resources. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsrTraceRoot {
  VkrSsrGpuParams params;
  uint64_t depth_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t normal_texture_id;
  uint64_t specular_texture_id;
  uint64_t pyramid_texture_id;
  uint64_t receiver_texture_id;
  uint64_t hdr_texture_id;
  uint64_t raw_texture_id;
  uint64_t clearcoat_texture_id;
} VkrMetalPacketSsrTraceRoot;

_Static_assert(sizeof(VkrMetalPacketSsrTraceRoot) == 368,
               "Metal SSR trace root ABI must remain 368 bytes");
_Static_assert(offsetof(VkrMetalPacketSsrTraceRoot, clearcoat_texture_id) ==
                   352u,
               "Metal SSR trace clearcoat ABI offset drift");

/** SSR temporal filtering owns one coherent color/depth/identity tuple. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsrTemporalRoot {
  VkrSsrGpuParams params;
  uint64_t raw_texture_id;
  uint64_t receiver_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t depth_texture_id;
  uint64_t normal_texture_id;
  uint64_t motion_texture_id;
  uint64_t validity_texture_id;
  uint64_t history_color_texture_id;
  uint64_t history_depth_texture_id;
  uint64_t history_identity_texture_id;
  uint64_t output_color_texture_id;
  uint64_t output_depth_texture_id;
  uint64_t output_identity_texture_id;
  uint64_t visible_rows;
  uint64_t instances;
  uint64_t specular_texture_id;
  uint64_t clearcoat_texture_id;
  uint64_t frame;
  uint64_t albedo_texture_id;
  uint64_t gtao_visibility_texture_id;
  uint64_t sheen_texture_id;
  uint64_t anisotropy_texture_id;
} VkrMetalPacketSsrTemporalRoot;

_Static_assert(sizeof(VkrMetalPacketSsrTemporalRoot) == 464,
               "Metal SSR temporal root ABI must remain 464 bytes");
_Static_assert(offsetof(VkrMetalPacketSsrTemporalRoot,
                        clearcoat_texture_id) == 416u,
               "Metal SSR temporal clearcoat ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketSsrTemporalRoot, frame) == 424u &&
                   offsetof(VkrMetalPacketSsrTemporalRoot,
                            albedo_texture_id) == 432u &&
                   offsetof(VkrMetalPacketSsrTemporalRoot,
                            gtao_visibility_texture_id) == 440u &&
                   offsetof(VkrMetalPacketSsrTemporalRoot,
                            sheen_texture_id) == 448u &&
                   offsetof(VkrMetalPacketSsrTemporalRoot,
                            anisotropy_texture_id) == 456u,
               "Metal SSR temporal shading ABI offset drift");

/** Full-resolution matching-pixel replacement of environment specular. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSsrCompositeRoot {
  uint64_t frame;
  VkrSsrGpuParams params;
  uint64_t hdr_texture_id;
  uint64_t history_color_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t depth_texture_id;
  uint64_t albedo_texture_id;
  uint64_t specular_texture_id;
  uint64_t normal_texture_id;
  uint64_t gtao_visibility_texture_id;
  Mat4 inverse_view_projection;
  uint32_t extent[2];
  uint32_t reserved[2];
  uint64_t clearcoat_texture_id;
  uint64_t sheen_texture_id;
  uint64_t anisotropy_texture_id;
} VkrMetalPacketSsrCompositeRoot;

_Static_assert(sizeof(VkrMetalPacketSsrCompositeRoot) == 480,
               "Metal SSR composite root ABI must remain 480 bytes");
_Static_assert(offsetof(VkrMetalPacketSsrCompositeRoot,
                        clearcoat_texture_id) == 448u,
               "Metal SSR composite clearcoat ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketSsrCompositeRoot, sheen_texture_id) ==
                   456u,
               "Metal SSR composite sheen ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketSsrCompositeRoot,
                        anisotropy_texture_id) == 464u,
               "Metal SSR composite anisotropy ABI offset drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSdsmRoot {
  uint64_t depth_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t reduce_state;
  uint32_t extent[2];
} VkrMetalPacketSdsmRoot;

_Static_assert(sizeof(VkrMetalPacketSdsmRoot) == 32,
               "Metal SDSM root ABI must remain 32 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketExposureRoot {
  uint64_t histogram;
  uint64_t state;
  uint64_t previous_state;
  uint64_t source_texture_id;
  uint32_t extent[2];
  uint32_t reset_reasons;
  uint32_t reserved;
  VkrExposureGpuMetering metering;
} VkrMetalPacketExposureRoot;

_Static_assert(sizeof(VkrMetalPacketExposureRoot) == 112,
               "Metal exposure root ABI must remain 112 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSubsurfaceRoot {
  VkrSubsurfaceGpuParams params;
  Mat4 inverse_view_projection;
  uint64_t frame;
  uint64_t visible_rows;
  uint64_t hdr;
  uint64_t source;
  uint64_t depth;
  uint64_t normal;
  uint64_t vbuffer;
  uint64_t profile_bank;
  uint64_t albedo;
  uint64_t specular;
  uint64_t clearcoat;
  uint64_t sheen;
  uint64_t anisotropy;
  uint64_t destination;
} VkrMetalPacketSubsurfaceRoot;

_Static_assert(sizeof(VkrMetalPacketSubsurfaceRoot) == 208u,
               "Subsurface gather root ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketMotionBlurRoot {
  VkrMotionBlurGpuParams params;
  uint64_t source0;
  uint64_t source1;
  uint64_t source2;
  uint64_t source3;
  uint64_t source4;
  uint64_t destination0;
} VkrMetalPacketMotionBlurRoot;

_Static_assert(sizeof(VkrMetalPacketMotionBlurRoot) == 96u,
               "Motion-blur root ABI size drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketDofRoot {
  VkrDofGpuParams params;
  uint64_t source0;
  uint64_t source1;
  uint64_t source2;
  uint64_t source3;
  uint64_t source4;
  uint64_t destination0;
  uint64_t destination1;
  uint32_t reserved[2];
} VkrMetalPacketDofRoot;

_Static_assert(sizeof(VkrMetalPacketDofRoot) == 112u,
               "Metal DoF root ABI size drift");

/** Mirrors VkrMetalPacketBloomRoot in shaders/metal/msl/post/bloom.metal. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketBloomRoot {
  uint64_t source_texture_id;
  /**
   * Coarser accumulation level, written for the upsample pass only. At the
   * deepest step the accumulation level above has never been written, so the
   * encoder points this at the downsample chain instead. Selecting on the CPU
   * is what keeps the kernel free of a bootstrap branch and keeps every sampled
   * texel defined.
   */
  uint64_t coarse_texture_id;
  uint64_t destination_texture_id;
  /** Extent the tap offsets are expressed in; see the shader field comment. */
  uint32_t filter_extent[2];
  uint32_t destination_extent[2];
  VkrBloomGpuParams params;
  uint32_t reserved[2];
} VkrMetalPacketBloomRoot;

_Static_assert(sizeof(VkrMetalPacketBloomRoot) == 80,
               "Metal bloom root ABI must remain 80 bytes");

typedef struct VkrMetalPacketSdsmState {
  uint32_t min_device_z_bits;
  uint32_t max_device_z_bits;
  uint32_t occupied_count;
  uint32_t reserved;
} VkrMetalPacketSdsmState;

_Static_assert(sizeof(VkrMetalPacketSdsmState) == 16,
               "Metal SDSM state ABI must remain 16 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketTemporalTransformRoot {
  uint64_t instances;
  uint64_t transforms;
  uint32_t instance_count;
  uint32_t transform_capacity;
  uint32_t frame_index;
  uint32_t reserved;
} VkrMetalPacketTemporalTransformRoot;

_Static_assert(sizeof(VkrMetalPacketTemporalTransformRoot) == 32,
               "Metal temporal-transform root ABI must remain 32 bytes");

/** Per-dispatch material-resolve resource table and viewport contract. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketGBufferResolveRoot {
  uint64_t visible_rows;
  uint64_t geometry_rows;
  uint64_t instances;
  uint64_t materials;
  uint64_t compaction_state;
  uint64_t vbuffer_texture_id;
  uint64_t albedo_texture_id;
  uint64_t specular_texture_id;
  uint64_t normal_texture_id;
  uint64_t emissive_texture_id;
  uint64_t debug_texture_id;
  uint64_t hdr_seed_texture_id;
  Mat4 view_projection;
  Mat4 current_view_projection;
  Mat4 previous_view_projection;
  uint64_t previous_transforms;
  uint64_t motion_texture_id;
  uint64_t validity_texture_id;
  uint32_t extent[2];
  uint32_t visible_capacity;
  uint32_t geometry_count;
  uint32_t material_count;
  uint32_t instance_count;
  uint32_t render_mode;
  uint32_t history_valid;
  uint32_t previous_frame_index;
  uint32_t reserved;
  Mat4 sky_reprojection;
  uint64_t clearcoat_texture_id;
  uint64_t sheen_texture_id;
  uint64_t anisotropy_texture_id;
} VkrMetalPacketGBufferResolveRoot;

_Static_assert(sizeof(VkrMetalPacketGBufferResolveRoot) == 448,
               "Metal G-buffer resolve root ABI must remain 448 bytes");
_Static_assert(offsetof(VkrMetalPacketGBufferResolveRoot, sky_reprojection) ==
                   352,
               "Metal G-buffer sky-reprojection matrix ABI drift");
_Static_assert(offsetof(VkrMetalPacketGBufferResolveRoot,
                        clearcoat_texture_id) == 416u,
               "Metal G-buffer clearcoat ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketGBufferResolveRoot, sheen_texture_id) ==
                   424u,
               "Metal G-buffer sheen ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketGBufferResolveRoot,
                        anisotropy_texture_id) == 432u,
               "Metal G-buffer anisotropy ABI offset drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketTemporalResolveRoot {
  uint64_t visible_rows;
  uint64_t instances;
  uint64_t scene_texture_id;
  uint64_t pre_transmission_texture_id;
  uint64_t motion_texture_id;
  uint64_t validity_texture_id;
  uint64_t depth_texture_id;
  uint64_t vbuffer_texture_id;
  uint64_t history_color_texture_id;
  uint64_t history_depth_texture_id;
  uint64_t history_identity_texture_id;
  uint64_t history_surface_texture_id;
  uint64_t output_color_texture_id;
  uint64_t output_depth_texture_id;
  uint64_t output_identity_texture_id;
  uint64_t output_surface_texture_id;
  uint32_t extent[2];
  uint32_t history_valid;
  uint32_t render_mode;
  uint32_t camera_stationary;
  uint64_t transmission_visible_rows;
  uint64_t transmission_instances;
  uint64_t transmission_vbuffer_texture_id;
  uint64_t transmission_depth_texture_id;
  uint32_t transmission_enabled;
  uint32_t transmission_alignment_padding;
  uint32_t transmission_reserved[2];
  Vec2 current_jitter_pixels;
  Vec2 previous_jitter_pixels;
  uint32_t scene_stationary;
} VkrMetalPacketTemporalResolveRoot;

_Static_assert(sizeof(VkrMetalPacketTemporalResolveRoot) == 224,
               "Metal temporal-resolve root ABI must remain 224 bytes");
_Static_assert(offsetof(VkrMetalPacketTemporalResolveRoot,
                        current_jitter_pixels) == 200u &&
                   offsetof(VkrMetalPacketTemporalResolveRoot,
                            previous_jitter_pixels) == 208u,
               "Metal temporal-resolve jitter ABI drift");
_Static_assert(offsetof(VkrMetalPacketTemporalResolveRoot, scene_stationary) ==
                   216u,
               "Metal temporal static-scene ABI drift");

/** Post-MetalFX stationary mean. Output alpha stores private sample age. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketMetalfxStabilizeRoot {
  uint64_t output_texture_id;
  uint64_t history_texture_id;
  uint64_t validity_texture_id;
  uint32_t output_extent[2];
  uint32_t source_extent[2];
  /** Current raster jitter in top-left source pixels, separate from motion. */
  Vec2 jitter_pixels;
  uint32_t history_valid;
  uint32_t scene_stationary;
  uint32_t reserved[2];
} VkrMetalPacketMetalfxStabilizeRoot;

_Static_assert(sizeof(VkrMetalPacketMetalfxStabilizeRoot) == 64u &&
                   _Alignof(VkrMetalPacketMetalfxStabilizeRoot) == 16u,
               "MetalFX stabilize root size/alignment ABI drift");
_Static_assert(
    offsetof(VkrMetalPacketMetalfxStabilizeRoot, output_texture_id) == 0u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, history_texture_id) == 8u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, validity_texture_id) == 16u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, output_extent) == 24u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, source_extent) == 32u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, jitter_pixels) == 40u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, history_valid) == 48u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, scene_stationary) == 52u &&
        offsetof(VkrMetalPacketMetalfxStabilizeRoot, reserved) == 56u,
    "MetalFX stabilize root field ABI drift");

/** Per-dispatch deferred-lighting resources and reconstruction contract. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketDeferredLightingRoot {
  uint64_t frame;
  uint64_t vbuffer_texture_id;
  uint64_t depth_texture_id;
  uint64_t albedo_texture_id;
  uint64_t specular_texture_id;
  uint64_t normal_texture_id;
  uint64_t hdr_texture_id;
  uint64_t sky_texture_id;
  uint64_t gtao_visibility_texture_id;
  Mat4 inverse_view_projection;
  uint32_t extent[2];
  uint32_t sky_enabled;
  uint32_t subsurface_profile_count;
  Vec4 solar_disk_radiance;
  uint64_t direct_source_texture_id;
  uint32_t ssgi_enabled;
  uint32_t ssgi_reserved;
  uint64_t clearcoat_texture_id;
  uint64_t sheen_texture_id;
  uint64_t anisotropy_texture_id;
  uint32_t visible_rows_padding[2];
  uint64_t visible_rows;
  uint64_t subsurface_source_texture_id;
} VkrMetalPacketDeferredLightingRoot;
_Static_assert(offsetof(VkrMetalPacketDeferredLightingRoot, subsurface_source_texture_id) == 232u &&
                   offsetof(VkrMetalPacketDeferredLightingRoot, subsurface_profile_count) == 156u,
               "Subsurface source producer ABI drift");

_Static_assert(offsetof(VkrMetalPacketDeferredLightingRoot,
                        direct_source_texture_id) == 176u,
               "Metal SSGI direct-source ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketDeferredLightingRoot,
                        clearcoat_texture_id) == 192u,
               "Metal deferred clearcoat ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketDeferredLightingRoot, sheen_texture_id) ==
                   200u,
               "Metal deferred sheen ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketDeferredLightingRoot,
                        anisotropy_texture_id) == 208u,
               "Metal deferred anisotropy ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketDeferredLightingRoot, visible_rows) ==
                   224u,
               "Metal deferred visible-row ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketDeferredLightingRoot) == 240u,
               "Metal deferred-lighting root ABI must remain 240 bytes");

/** Per-dispatch frontmost transmission visibility resolve and shading. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketTransmissionShadeRoot {
  uint64_t frame;
  uint64_t visible_rows;
  uint64_t geometry_rows;
  uint64_t instances;
  uint64_t materials;
  uint64_t transmission_materials;
  uint64_t compaction_state;
  uint64_t pixel_list;
  uint64_t compact_counts;
  uint64_t vbuffer_texture_id;
  uint64_t depth_texture_id;
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  Mat4 view_projection;
  Mat4 inverse_view_projection;
  uint32_t extent[2];
  uint32_t visible_capacity;
  uint32_t geometry_count;
  uint32_t material_count;
  uint32_t instance_count;
  uint32_t pixel_capacity;
  uint32_t compact_layer;
  uint32_t compact_enabled;
  uint32_t opaque_mip_count;
  uint64_t opaque_texture_id;
  uint64_t motion_texture_id;
  uint64_t validity_texture_id;
  uint64_t previous_transforms;
  uint32_t previous_transform_address_padding[2];
  Mat4 current_view_projection;
  Mat4 previous_view_projection;
  uint32_t history_valid;
  uint32_t previous_frame_index;
  uint32_t temporal_outputs_enabled;
  uint32_t temporal_reserved;
} VkrMetalPacketTransmissionShadeRoot;
_Static_assert(sizeof(VkrMetalPacketTransmissionShadeRoot) == 464,
               "Metal transmission-shade root ABI must remain 464 bytes");

typedef VkrGpuTransmissionDiagnostics VkrMetalPacketTransmissionDiagnostics;

_Static_assert(sizeof(VkrMetalPacketTransmissionDiagnostics) == 180,
               "Metal transmission diagnostics ABI must remain 180 bytes");
_Static_assert(offsetof(VkrMetalPacketTransmissionDiagnostics,
                        covered_pixels) == sizeof(VkrGpuDrawCompactionState),
               "Metal transmission coverage must follow compaction state");

/** Scans one final transmission layer and finalizes its indirect dispatch. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketTransmissionCompactRoot {
  uint64_t vbuffer_texture_id;
  uint64_t pixel_list;
  uint64_t covered_pixels;
  uint64_t overflow_counts;
  uint64_t indirect_arguments;
  uint64_t visible_rows;
  uint64_t materials;
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  uint32_t extent[2];
  uint32_t layer;
  uint32_t capacity;
  uint32_t reserved[2];
} VkrMetalPacketTransmissionCompactRoot;

_Static_assert(sizeof(VkrMetalPacketTransmissionCompactRoot) == 96,
               "Metal transmission-compact root ABI must remain 96 bytes");

/** Timing-only scan of one transmission visibility layer. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketTransmissionCoverageRoot {
  uint64_t vbuffer_texture_id;
  uint64_t covered_pixels;
  uint32_t extent[2];
  uint32_t layer;
  uint32_t reserved;
} VkrMetalPacketTransmissionCoverageRoot;

_Static_assert(sizeof(VkrMetalPacketTransmissionCoverageRoot) == 32,
               "Metal transmission-coverage root ABI must remain 32 bytes");

/** Resolves the requested deferred visibility pixel to one object ID. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketPickingResolveRoot {
  uint64_t opaque_visible_rows;
  uint64_t opaque_instances;
  uint64_t opaque_compaction_state;
  uint64_t transmission_visible_rows;
  uint64_t transmission_instances;
  uint64_t transmission_compaction_state;
  uint64_t opaque_vbuffer_texture_id;
  uint64_t opaque_depth_texture_id;
  uint64_t transmission_vbuffer_texture_id;
  uint64_t transmission_depth_texture_id;
  uint64_t destination_texture_id;
  uint32_t pixel[2];
  uint32_t extent[2];
  uint32_t opaque_visible_capacity;
  uint32_t opaque_instance_count;
  uint32_t transmission_visible_capacity;
  uint32_t transmission_instance_count;
  uint32_t transmission_enabled;
  uint32_t reserved[1];
} VkrMetalPacketPickingResolveRoot;

_Static_assert(sizeof(VkrMetalPacketPickingResolveRoot) == 128,
               "Metal picking-resolve root ABI must remain 128 bytes");

/** One upload-ring cell used by the retained table-driven forward path. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketTableDrawUpload {
  VkrMetalPacketDrawRoot root;
  VkrGpuGeometryRow geometry;
  VkrGpuVisibleDrawRow visible;
} VkrMetalPacketTableDrawUpload;

_Static_assert(sizeof(VkrMetalPacketTableDrawUpload) <=
                   VKR_METAL_PACKET_DRAW_ROOT_STRIDE,
               "Table draw upload must fit one root-ring cell");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketIblProbe {
  /** ADR-038 final layout: offset 0 carries the coefficient slot, offset 4 is
      reserved, and every field from offset 8 onward keeps its previous offset.
   */
  uint32_t sh_slot;
  uint32_t sh_reserved;
  uint64_t prefilter_texture_id;
  Vec4 center_blend;
  Vec4 extents_weight;
  Vec4 intensity_box;
} VkrMetalPacketIblProbe;

/** Mirrors VkrShadowCascadePacketData; see vkr_frame_input.h for units. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketShadowCascade {
  Mat4 light_view_projection;
  Vec4 split_near_far_texel_depth;
  Vec4 origin_inv_size_sun;
} VkrMetalPacketShadowCascade;

enum { VKR_METAL_PACKET_TONEMAP_FLAG_OPAQUE_ALPHA = 1u << 4u };

typedef struct VKR_SIMD_ALIGN VkrMetalPacketTonemapRoot {
  uint64_t source_texture_id;
  uint32_t flags;
  float32_t image_sharpness;
  uint64_t exposure_state;
  uint32_t output_extent[2];
  uint64_t color_grading;
  uint64_t display_output;
} VkrMetalPacketTonemapRoot;

_Static_assert(offsetof(VkrMetalPacketTonemapRoot, display_output) == 40u,
               "Metal tonemap display-output ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketTonemapRoot) == 48u,
               "Metal tonemap root ABI size drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketEquirectRoot {
  uint64_t source_texture_id;
  uint64_t target_texture_id;
  uint32_t target_size;
  uint32_t reserved[3];
} VkrMetalPacketEquirectRoot;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketAtmosphereRoot {
  VkrAtmosphereGpuParams params;
  uint64_t transmittance_sample_texture_id;
  uint64_t transmittance_storage_texture_id;
  uint64_t multiple_scattering_sample_texture_id;
  uint64_t multiple_scattering_storage_texture_id;
  uint64_t source_storage_texture_id;
  uint64_t sun_output;
  uint32_t extent[2];
  uint32_t face_size;
  uint32_t reserved;
} VkrMetalPacketAtmosphereRoot;

_Static_assert(sizeof(VkrMetalPacketAtmosphereRoot) == 192u,
               "Metal atmosphere root ABI drift");
_Static_assert(_Alignof(VkrMetalPacketAtmosphereRoot) == 16u,
               "Metal atmosphere root alignment drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot,
                        transmittance_sample_texture_id) == 128u,
               "Metal atmosphere transmittance sample offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot,
                        multiple_scattering_sample_texture_id) == 144u,
               "Metal atmosphere multiple-scattering sample offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot, source_storage_texture_id) ==
                   160u,
               "Metal atmosphere source offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot, sun_output) == 168u,
               "Metal atmosphere sun output offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot, extent) == 176u,
               "Metal atmosphere extent offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot, face_size) == 184u,
               "Metal atmosphere face-size offset drift");

/** Mirrors VkrMetalPacketShProjectRoot in metal/msl/ibl/sh_projection.metal.
    `destination` is the device address of the single 112-byte slot this
    dispatch writes; the window scalars are already-evaluated deringing
    factors, so the kernel carries no pow(). */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketShProjectRoot {
  uint64_t source_texture_id;
  uint64_t destination;
  uint32_t source_face_size;
  uint32_t source_mip;
  float32_t window_band_0;
  float32_t window_band_1;
  float32_t window_band_2;
  /* The shader's uint2 reserved is eight-byte aligned, so it lands at 40. */
  uint32_t reserved_padding;
  uint32_t reserved[2];
} VkrMetalPacketShProjectRoot;

_Static_assert(sizeof(VkrMetalPacketShProjectRoot) == 48,
               "Metal SH projection root must remain 48 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketPrefilterRoot {
  uint64_t source_texture_id;
  uint64_t target_texture_id;
  float32_t roughness;
  float32_t source_face_size;
  float32_t source_mip_count;
  uint32_t target_mip;
} VkrMetalPacketPrefilterRoot;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketTextRoot {
  uint64_t vertices;
  uint64_t atlas_texture_id;
  Mat4 model;
  Mat4 view_projection;
  Vec4 controls;
  uint32_t object_id;
  uint32_t flags;
  uint32_t reserved[2];
} VkrMetalPacketTextRoot;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketUiRoot {
  uint64_t vertices;
  uint64_t texture_id;
  /** target width/height followed by the normalized MTSDF unit range. */
  Vec4 target_unit_range;
  Vec2 rect_extent;
  uint32_t mode;
  uint32_t flags;
  /** top-left, top-right, bottom-right, bottom-left. */
  Vec4 corner_radii;
  uint64_t display_output;
} VkrMetalPacketUiRoot;

_Static_assert(offsetof(VkrMetalPacketUiRoot, display_output) == 64u,
               "Metal UI display-output ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketUiRoot) == 80u,
               "Metal UI root ABI size drift");

typedef enum VkrMetalPacketAbiRecordId {
  VKR_METAL_PACKET_ABI_VERTEX = 0,
  VKR_METAL_PACKET_ABI_INSTANCE,
  VKR_METAL_PACKET_ABI_MATERIAL,
  VKR_METAL_PACKET_ABI_TRANSMISSION_MATERIAL,
  VKR_METAL_PACKET_ABI_TEXT_VERTEX,
  VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_TEMPORAL_VERTEX_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_FRAME_ROOT,
  VKR_METAL_PACKET_ABI_IBL_PROBE,
  VKR_METAL_PACKET_ABI_DIFFUSE_VOLUME,
  VKR_METAL_PACKET_ABI_LTC,
  VKR_METAL_PACKET_ABI_SHEEN,
  VKR_METAL_PACKET_ABI_ANISOTROPY,
  VKR_METAL_PACKET_ABI_SHADOW_CASCADE,
  VKR_METAL_PACKET_ABI_DISPLAY_OUTPUT_PARAMS,
  VKR_METAL_PACKET_ABI_TONEMAP_ROOT,
  VKR_METAL_PACKET_ABI_EQUIRECT_ROOT,
  VKR_METAL_PACKET_ABI_ATMOSPHERE_ROOT,
  VKR_METAL_PACKET_ABI_PREFILTER_ROOT,
  VKR_METAL_PACKET_ABI_SH_PROJECT_ROOT,
  VKR_METAL_PACKET_ABI_TEXT_ROOT,
  VKR_METAL_PACKET_ABI_UI_ROOT,
  VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT,
  VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_PEEL_ROOT,
  VKR_METAL_PACKET_ABI_TEMPORAL_TRANSFORM_ROOT,
  VKR_METAL_PACKET_ABI_GBUFFER_RESOLVE_ROOT,
  VKR_METAL_PACKET_ABI_GTAO_PARAMS,
  VKR_METAL_PACKET_ABI_GTAO_DEPTH_ROOT,
  VKR_METAL_PACKET_ABI_GTAO_EVALUATE_ROOT,
  VKR_METAL_PACKET_ABI_GTAO_DENOISE_ROOT,
  VKR_METAL_PACKET_ABI_SSGI_PARAMS,
  VKR_METAL_PACKET_ABI_SSGI_DEPTH_BASE_ROOT,
  VKR_METAL_PACKET_ABI_SSGI_DEPTH_MIP_ROOT,
  VKR_METAL_PACKET_ABI_SSGI_TRACE_ROOT,
  VKR_METAL_PACKET_ABI_SSGI_TEMPORAL_ROOT,
  VKR_METAL_PACKET_ABI_SSGI_COMPOSITE_ROOT,
  VKR_METAL_PACKET_ABI_SSR_PARAMS,
  VKR_METAL_PACKET_ABI_SSR_DEPTH_BASE_ROOT,
  VKR_METAL_PACKET_ABI_SSR_DEPTH_MIP_ROOT,
  VKR_METAL_PACKET_ABI_SSR_TRACE_ROOT,
  VKR_METAL_PACKET_ABI_SSR_TEMPORAL_ROOT,
  VKR_METAL_PACKET_ABI_SSR_COMPOSITE_ROOT,
  VKR_METAL_PACKET_ABI_FOG_ROOT,
  VKR_METAL_PACKET_ABI_FOG_PARAMS,
  VKR_METAL_PACKET_ABI_FROXEL_PARAMS,
  VKR_METAL_PACKET_ABI_FROXEL_INJECT_ROOT,
  VKR_METAL_PACKET_ABI_FROXEL_INTEGRATE_ROOT,
  VKR_METAL_PACKET_ABI_FROXEL_APPLY_ROOT,
  VKR_METAL_PACKET_ABI_DEFERRED_LIGHTING_ROOT,
  VKR_METAL_PACKET_ABI_TEMPORAL_RESOLVE_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_SHADE_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_COVERAGE_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_COMPACT_ROOT,
  VKR_METAL_PACKET_ABI_PICKING_RESOLVE_ROOT,
  VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT,
  VKR_METAL_PACKET_ABI_SDSM_ROOT,
  VKR_METAL_PACKET_ABI_EXPOSURE_ROOT,
  VKR_METAL_PACKET_ABI_SUBSURFACE_PARAMS,
  VKR_METAL_PACKET_ABI_SUBSURFACE_ROOT,
  VKR_METAL_PACKET_ABI_MOTION_BLUR_PARAMS,
  VKR_METAL_PACKET_ABI_MOTION_BLUR_ROOT,
  VKR_METAL_PACKET_ABI_DOF_PARAMS,
  VKR_METAL_PACKET_ABI_DOF_ROOT,
  VKR_METAL_PACKET_ABI_BLOOM_ROOT,
  VKR_METAL_PACKET_ABI_METALFX_STABILIZE_ROOT,
  VKR_METAL_PACKET_ABI_RECORD_COUNT,
} VkrMetalPacketAbiRecordId;

typedef VkrGpuAbiField VkrMetalPacketAbiField;
typedef VkrGpuAbiRecord VkrMetalPacketAbiRecord;

const VkrMetalPacketAbiRecord *
vkr_metal_packet_abi_record(VkrMetalPacketAbiRecordId id);

/** Returns whether the host authority satisfies Metal's reflected minimum
 * alignment for this record. */
bool8_t
vkr_metal_packet_abi_alignment_compatible(VkrMetalPacketAbiRecordId id,
                                          uint32_t shader_min_alignment);

/** CPU gate for the durable expected-size/alignment/offset manifest. */
bool8_t vkr_metal_packet_abi_validate_host(void);

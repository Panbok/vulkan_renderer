#pragma once

#include <stddef.h>

#include "math/mat.h"
#include "math/vec.h"
#include "metal/vkr_metal_material_table.h"
#include "vkr_atmosphere.h"
#include "vkr_bloom.h"
#include "vkr_buffer.h"
#include "vkr_display_output.h"
#include "vkr_exposure.h"
#include "vkr_fog.h"
#include "vkr_froxel_fog.h"
#include "vkr_gpu_abi.h"
#include "vkr_lighting.h"
#include "vkr_render_resources.h"
#include "vkr_shadow.h"

enum {
  VKR_METAL_PACKET_ROOT_ALIGNMENT = 256,
  VKR_METAL_PACKET_DRAW_ROOT_STRIDE = 512,
};

/** Converts VKR's column-major matrix for Slang's row-vector MSL lowering. */
vkr_internal INLINE Mat4 vkr_metal_packet_slang_draw_matrix(Mat4 matrix) {
  return mat4_transpose(matrix);
}

/** Values shared by every indexed draw encoded for one pass. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketDiffuseVolume {
  Vec4 origin;
  Vec4 inverse_spacing;
  uint32_t dimensions[4];
} VkrMetalPacketDiffuseVolume;
_Static_assert(sizeof(VkrMetalPacketDiffuseVolume) == 48u,
               "Metal diffuse volume parameters ABI drift");

/** Frame lightmap record (ADR-088); mirrors VkrMetalPacketLightmap in
    common/draw.metalh and draw.slangh. `texture_id` is a 2D array whose slice
    page * layer_count + layer holds one layer of one page; `rects` holds
    rect_count VkrLightmapRect, selected by an instance's lightmap slot minus
    one. A zero rect_count leaves every draw without lightmaps. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketLightmap {
  uint64_t texture_id;
  uint64_t rects;
  uint32_t rect_count;
  uint32_t layer_count;
  float32_t inverse_page_size;
  uint32_t active_layer_count;
  uint32_t active_layers[8];
  float32_t active_weights[8];
} VkrMetalPacketLightmap;
_Static_assert(sizeof(VkrMetalPacketLightmap) == 96u,
               "Metal lightmap record ABI drift");
_Static_assert(VKR_LIGHTMAP_MAX_ACTIVE_LAYERS <= 8u,
               "Metal lightmap record holds at most eight active layers");

/** The tiled pipeline's sky draws (ADR-087); mirrors VkrMetalTiledSkyRoot in
    world/tiled.metal. `depth_texture_id` is the resolved depth the cloud
    draw reads, zero for the clear sky. */
typedef struct VKR_SIMD_ALIGN VkrMetalTiledSkyRoot {
  uint64_t frame;
  uint64_t sky_view_texture_id;
  Mat4 inverse_view_projection;
  uint32_t extent[2];
  uint32_t sky_mode;
  uint32_t clouds_enabled;
  Vec4 sky_radiance;
  uint64_t depth_texture_id;
} VkrMetalTiledSkyRoot;
_Static_assert(sizeof(VkrMetalTiledSkyRoot) == 128u,
               "Metal tiled sky root ABI drift");

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
_Static_assert(sizeof(VkrMetalPacketSheen) == 48u, "Metal sheen ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketAnisotropy {
  uint64_t table0;
  uint64_t table1;
  uint64_t table2;
  uint64_t reserved;
} VkrMetalPacketAnisotropy;
_Static_assert(sizeof(VkrMetalPacketAnisotropy) == 32u,
               "Metal anisotropy ABI drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketLocalShadowTransmission {
  uint64_t depth0;
  uint64_t color0;
  uint64_t depth1;
  uint64_t color1;
  uint64_t overflow;
  uint64_t reserved;
} VkrMetalPacketLocalShadowTransmission;
_Static_assert(sizeof(VkrMetalPacketLocalShadowTransmission) == 48u,
               "Metal local shadow transmission sampling ABI drift");

/** One light-facing material record shared by all three raster crossings. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketLocalShadowTransmissionDraw {
  uint64_t transmission_materials;
  uint32_t face;
  uint32_t reserved;
  Vec4 light_position;
} VkrMetalPacketLocalShadowTransmissionDraw;
_Static_assert(sizeof(VkrMetalPacketLocalShadowTransmissionDraw) == 32u,
               "Metal local shadow transmission draw ABI drift");

/** Frame-slot sky record; mirrors VkrMetalPacketSky in common/draw.metalh. The
    lookup textures belong to the published atmosphere generation. The cloud
    radiance is this frame's history output and the noise textures are
    renderer-owned (ADR-074). */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSky {
  VkrSkyGpuParams params;
  uint64_t aerial_perspective_texture_id;
  uint64_t transmittance_texture_id;
  uint64_t multiple_scattering_texture_id;
  uint64_t cloud_radiance_texture_id;
  uint64_t cloud_shadow_texture_id;
  uint64_t cloud_base_noise_texture_id;
  uint64_t cloud_detail_noise_texture_id;
  uint64_t cloud_weather_texture_id;
  /** The published sky source's SH slot; the cloud layer lights itself with
      it rather than the cloud-lit slot it feeds (ADR-074). */
  uint32_t clear_sh_slot;
  /** This frame's cloud chain for global reflections and its first face
      extent; zero when the cloud-lit sky light did not run. */
  uint32_t cloud_sky_light_face_size;
  uint64_t cloud_sky_light;
} VkrMetalPacketSky;
_Static_assert(sizeof(VkrMetalPacketSky) == 512u, "Metal sky record ABI drift");
_Static_assert(offsetof(VkrMetalPacketSky, aerial_perspective_texture_id) ==
                   432u,
               "Metal sky aerial-perspective offset drift");

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
  float32_t pre_exposure;
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
  uint64_t local_shadow_transmission;
  uint64_t sky;
  /** The frame's VkrMetalPacketLightmap; always set. */
  uint64_t lightmap;
  /** The material table's terrain rows, which the tiled pipeline's forward
      shading reads by material index (ADR-084). */
  uint64_t terrain_materials;
  /** The material table's transmission rows (transmission and thickness
      textures), which the tiled pipeline's glass reads by material index. */
  uint64_t transmission_materials;
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
_Static_assert(offsetof(VkrMetalPacketFrameRoot,
                        froxel_integrated_texture_id) == 216u,
               "Metal froxel integrated texture ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, local_shadow_transmission) ==
                   528u,
               "Metal local shadow transmission sampling offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, sky) == 536u,
               "Metal sky record offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, lightmap) == 544u,
               "Metal lightmap record offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, terrain_materials) == 552u,
               "Metal terrain material rows offset drift");
_Static_assert(offsetof(VkrMetalPacketFrameRoot, transmission_materials) ==
                   560u,
               "Metal transmission material rows offset drift");
_Static_assert(sizeof(VkrMetalPacketFrameRoot) == 576u,
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

_Static_assert(offsetof(VkrMetalPacketEditorOverlayRoot, display_output) ==
                   104u,
               "Metal editor overlay display-output ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketEditorOverlayRoot) == 112,
               "Metal editor overlay root ABI size drift");

/** Full-screen selection outline over the Scene image; `mask` is the R8
 * selection mask's GPU resource ID at the Scene image extent. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSelectionOutlineRoot {
  uint64_t mask;
  uint64_t padding;
  Vec4 color;
  uint32_t radius_px;
  uint32_t reserved;
  uint64_t display_output;
} VkrMetalPacketSelectionOutlineRoot;

_Static_assert(offsetof(VkrMetalPacketSelectionOutlineRoot, color) == 16u &&
                   offsetof(VkrMetalPacketSelectionOutlineRoot,
                            display_output) == 40u,
               "Metal selection outline ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketSelectionOutlineRoot) == 48,
               "Metal selection outline root ABI size drift");

/** Full-screen editor ground grid over the Scene image; `depth` is the
 * opaque depth's GPU resource ID at the render extent. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketEditorGridRoot {
  Mat4 inverse_view_projection;
  Vec4 camera_position;
  Vec4 params;
  uint32_t extent[2];
  uint32_t plane;
  uint32_t reserved;
  uint64_t depth;
  uint64_t display_output;
} VkrMetalPacketEditorGridRoot;

_Static_assert(offsetof(VkrMetalPacketEditorGridRoot, camera_position) == 64u &&
                   offsetof(VkrMetalPacketEditorGridRoot, extent) == 96u &&
                   offsetof(VkrMetalPacketEditorGridRoot, depth) == 112u &&
                   offsetof(VkrMetalPacketEditorGridRoot, display_output) ==
                       120u,
               "Metal editor grid ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketEditorGridRoot) == 128,
               "Metal editor grid root ABI size drift");

/** The only record written per indexed packet draw. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketDrawRoot {
  uint64_t geometry_rows;
  uint64_t visible_rows;
  uint64_t vertices;
  uint64_t frame;
  uint32_t visible_row_index;
  uint32_t flags;
  uint64_t local_shadow_transmission;
} VkrMetalPacketDrawRoot;

typedef VkrMetalPacketDrawRoot VkrMetalPacketVertexDrawRoot;

/* VkrMetalPacketDrawRoot.flags: the GPU-encoded visible rows carry the draws,
   and the camera view's encode chooses each draw's reflection probe. */
enum {
  VKR_METAL_PACKET_DRAW_ROOT_GPU_DRAW = 1u,
  VKR_METAL_PACKET_DRAW_ROOT_PROBES = 2u,
};

enum {
  /* Camera, directional cascades, then the local render slots. */
  VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX =
      1u + VKR_SHADOW_CASCADE_COUNT_MAX +
      VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX,
};

/** One frustum and routing policy in the bounded multi-view cull set. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketGpuDrawView {
  Vec4 frustum_planes[6];
  uint32_t required_candidate_flags;
  uint32_t hzb_enabled;
  uint32_t excluded_candidate_flags;
  /** Nonzero for a retained cascade: classification still counts its casters,
   * but encoding writes no commands, since its pass does not run. */
  uint32_t encode_idle;
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
  /** One VkrGpuLodView per culling view (ADR-084). */
  uint64_t lod_views;
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

/** Per-frame sky-view lookup and aerial-perspective volume builders. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketSkyBuildRoot {
  uint64_t sky;
  uint64_t output_texture_id;
} VkrMetalPacketSkyBuildRoot;

_Static_assert(sizeof(VkrMetalPacketSkyBuildRoot) == 16u,
               "Metal sky build root ABI drift");

/** One-time cloud noise synthesis (ADR-074); one root drives the base,
    detail and weather kernels. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketCloudNoiseRoot {
  uint64_t base_texture_id;
  uint64_t detail_texture_id;
  uint64_t weather_texture_id;
  uint64_t reserved;
} VkrMetalPacketCloudNoiseRoot;

_Static_assert(sizeof(VkrMetalPacketCloudNoiseRoot) == 32u,
               "Metal cloud noise root ABI drift");

/** Half-resolution cloud trace. `previous_view_projection` is the canonical
    unjittered matrix of the selected history producer; without a valid
    history `history_texture_id` names the output and is never sampled. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketCloudTraceRoot {
  Mat4 previous_view_projection;
  uint64_t sky;
  uint64_t frame;
  uint64_t depth_texture_id;
  uint64_t history_texture_id;
  uint64_t output_texture_id;
  uint32_t extent[2];
  uint32_t depth_extent[2];
  uint32_t frame_index;
  uint32_t history_valid;
  /** Converts history radiance to this frame's pre-exposure. */
  float32_t history_pre_exposure_scale;
  uint32_t reserved[3];
} VkrMetalPacketCloudTraceRoot;

_Static_assert(sizeof(VkrMetalPacketCloudTraceRoot) == 144u,
               "Metal cloud trace root ABI drift");

/** ADR-074 cloud-lit sky light; mirrors VkrMetalPacketCloudSkyLightRoot in
    msl/ibl/clouds.metal. Its march and projection kernels share it. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketCloudSkyLightRoot {
  uint64_t sky;
  uint64_t frame;
  uint64_t radiance;
  uint64_t destination;
  uint64_t source_texture_id;
  uint32_t source_mip;
  uint32_t face_size;
  /** Pre-exposed cloud radiance into the source's radiance_stops scale. */
  float32_t cloud_to_source_scale;
  float32_t window_band_0;
  float32_t window_band_1;
  float32_t window_band_2;
  uint32_t reserved[2];
} VkrMetalPacketCloudSkyLightRoot;

_Static_assert(sizeof(VkrMetalPacketCloudSkyLightRoot) == 80u,
               "Metal cloud sky-light root ABI drift");

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

typedef struct VKR_SIMD_ALIGN VkrMetalPacketAnimationPreviewRoot {
  Mat4 model;
  Mat4 view_projection;
  uint64_t vertices;
  uint64_t decode;
  uint64_t deformation_address;
  uint64_t reserved;
  Vec4 tint;
  uint32_t first_vertex;
  uint32_t decode_index;
  uint64_t reserved_tail;
} VkrMetalPacketAnimationPreviewRoot;

_Static_assert(sizeof(VkrMetalPacketAnimationPreviewRoot) == 192,
               "Metal animation preview root ABI must remain 192 bytes");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSkinningRoot {
  uint64_t bind_vertices;
  uint64_t influences;
  uint64_t palette;
  uint64_t output;
  uint32_t vertex_count;
  uint32_t palette_count;
  uint32_t reserved0;
  uint32_t reserved1;
} VkrMetalPacketSkinningRoot;

_Static_assert(sizeof(VkrMetalPacketSkinningRoot) == 48,
               "Metal skinning root ABI must remain 48 bytes");

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

enum {
  VKR_METAL_PACKET_TONEMAP_FLAG_SOURCE_DISPLAY_LINEAR = 1u << 5u,
  VKR_METAL_PACKET_TONEMAP_FLAG_PREPARE_DISPLAY_LINEAR = 1u << 6u,
  VKR_METAL_PACKET_TONEMAP_FLAG_SCENE_BLUR = 1u << 7u,
  /* The pass adds the bound bloom level to the scene it samples. */
  VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM = 1u << 8u,
};

typedef struct VKR_SIMD_ALIGN VkrMetalPacketTonemapRoot {
  uint64_t source_texture_id;
  uint32_t flags;
  float32_t image_sharpness;
  uint64_t exposure_state;
  uint32_t output_extent[2];
  uint64_t color_grading;
  uint64_t display_output;
  /** Returns pre-exposed radiance to scene-linear before exposure. */
  float32_t inverse_pre_exposure;
  /** With VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM, the bloom chain's first level
   * the pass adds, scaled by this intensity, to the scene it samples. */
  float32_t bloom_intensity;
  uint64_t bloom_texture_id;
} VkrMetalPacketTonemapRoot;

_Static_assert(offsetof(VkrMetalPacketTonemapRoot, display_output) == 40u,
               "Metal tonemap display-output ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketTonemapRoot, inverse_pre_exposure) == 48u,
               "Metal tonemap pre-exposure ABI offset drift");
_Static_assert(offsetof(VkrMetalPacketTonemapRoot, bloom_texture_id) == 56u,
               "Metal tonemap bloom ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketTonemapRoot) == 64u,
               "Metal tonemap root ABI size drift");

typedef struct VKR_SIMD_ALIGN VkrMetalPacketAtmosphereRoot {
  VkrAtmosphereGpuParams params;
  uint64_t transmittance_sample_texture_id;
  uint64_t transmittance_storage_texture_id;
  uint64_t multiple_scattering_sample_texture_id;
  uint64_t multiple_scattering_storage_texture_id;
  uint64_t source_storage_texture_id;
  uint32_t extent[2];
  uint32_t face_size;
  uint32_t reserved;
} VkrMetalPacketAtmosphereRoot;

_Static_assert(sizeof(VkrMetalPacketAtmosphereRoot) == 224u,
               "Metal atmosphere root ABI drift");
_Static_assert(_Alignof(VkrMetalPacketAtmosphereRoot) == 16u,
               "Metal atmosphere root alignment drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot,
                        transmittance_sample_texture_id) == 160u,
               "Metal atmosphere transmittance sample offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot,
                        multiple_scattering_sample_texture_id) == 176u,
               "Metal atmosphere multiple-scattering sample offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot,
                        source_storage_texture_id) == 192u,
               "Metal atmosphere source offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot, extent) == 200u,
               "Metal atmosphere extent offset drift");
_Static_assert(offsetof(VkrMetalPacketAtmosphereRoot, face_size) == 208u,
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
  /** Bit 0: the batch texture is bound. */
  uint32_t flags;
  uint32_t reserved;
  uint64_t display_output;
} VkrMetalPacketUiRoot;

_Static_assert(offsetof(VkrMetalPacketUiRoot, display_output) == 40u,
               "Metal UI display-output ABI offset drift");
_Static_assert(sizeof(VkrMetalPacketUiRoot) == 48u,
               "Metal UI root ABI size drift");

typedef enum VkrMetalPacketAbiRecordId {
  VKR_METAL_PACKET_ABI_VERTEX = 0,
  VKR_METAL_PACKET_ABI_INSTANCE,
  VKR_METAL_PACKET_ABI_MATERIAL,
  VKR_METAL_PACKET_ABI_TRANSMISSION_MATERIAL,
  VKR_METAL_PACKET_ABI_TERRAIN_MATERIAL,
  VKR_METAL_PACKET_ABI_TEXT_VERTEX,
  VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_FRAME_ROOT,
  VKR_METAL_PACKET_ABI_IBL_PROBE,
  VKR_METAL_PACKET_ABI_DIFFUSE_VOLUME,
  VKR_METAL_PACKET_ABI_LIGHTMAP,
  VKR_METAL_PACKET_ABI_LTC,
  VKR_METAL_PACKET_ABI_SHEEN,
  VKR_METAL_PACKET_ABI_ANISOTROPY,
  VKR_METAL_PACKET_ABI_LOCAL_SHADOW_TRANSMISSION,
  VKR_METAL_PACKET_ABI_LOCAL_SHADOW_TRANSMISSION_DRAW,
  VKR_METAL_PACKET_ABI_SHADOW_CASCADE,
  VKR_METAL_PACKET_ABI_DISPLAY_OUTPUT_PARAMS,
  VKR_METAL_PACKET_ABI_TONEMAP_ROOT,
  VKR_METAL_PACKET_ABI_ATMOSPHERE_ROOT,
  VKR_METAL_PACKET_ABI_PREFILTER_ROOT,
  VKR_METAL_PACKET_ABI_SH_PROJECT_ROOT,
  VKR_METAL_PACKET_ABI_TEXT_ROOT,
  VKR_METAL_PACKET_ABI_UI_ROOT,
  VKR_METAL_PACKET_ABI_UI_VERTEX,
  VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT,
  VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_GPU_DRAW_VIEW,
  VKR_METAL_PACKET_ABI_SKINNING_ROOT,
  VKR_METAL_PACKET_ABI_ANIMATION_PREVIEW_ROOT,
  VKR_METAL_PACKET_ABI_FOG_PARAMS,
  VKR_METAL_PACKET_ABI_FROXEL_PARAMS,
  VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT,
  VKR_METAL_PACKET_ABI_EXPOSURE_ROOT,
  VKR_METAL_PACKET_ABI_BLOOM_ROOT,
  VKR_METAL_PACKET_ABI_SKY,
  VKR_METAL_PACKET_ABI_SKY_PARAMS,
  VKR_METAL_PACKET_ABI_SKY_VIEW_ROOT,
  VKR_METAL_PACKET_ABI_AERIAL_PERSPECTIVE_ROOT,
  VKR_METAL_PACKET_ABI_CLOUD_PARAMS,
  VKR_METAL_PACKET_ABI_CLOUD_NOISE_ROOT,
  VKR_METAL_PACKET_ABI_CLOUD_SHADOW_ROOT,
  VKR_METAL_PACKET_ABI_CLOUD_TRACE_ROOT,
  VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT,
  VKR_METAL_PACKET_ABI_SELECTION_OUTLINE_ROOT,
  VKR_METAL_PACKET_ABI_EDITOR_GRID_ROOT,
  VKR_METAL_PACKET_ABI_TILED_SKY_ROOT,
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

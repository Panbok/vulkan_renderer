#pragma once

#include "math/mat.h"
#include "math/vec.h"
#include "renderer/metal/vkr_metal_material_table.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_gpu_abi.h"

enum {
  VKR_METAL_PACKET_ROOT_ALIGNMENT = 256,
  VKR_METAL_PACKET_DRAW_ROOT_STRIDE = 512,
  VKR_METAL_PACKET_TRANSMISSION_LAYER_COUNT = 4,
};

/** Converts VKR's column-major matrix for Slang's row-vector MSL lowering. */
static INLINE Mat4 vkr_metal_packet_slang_draw_matrix(Mat4 matrix) {
  return mat4_transpose(matrix);
}

/** Values shared by every indexed draw encoded for one pass. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketFrameRoot {
  uint64_t instances;
  uint32_t instance_address_padding[2];
  Mat4 view_projection;
  uint64_t materials;
  uint64_t irradiance_texture_id;
  uint64_t prefilter_texture_id;
  /* Retired BRDF-LUT slot. The environment BRDF is analytic on both
     backends; this stays as named padding so no downstream offset moves. */
  uint64_t reserved_brdf_lut;
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
  uint64_t shadow_texture_id;
  uint64_t shadow_cascades;
  Mat4 view;
  uint32_t shadow_cascade_count;
  float32_t shadow_bias;
  uint32_t shadow_reserved[2];
  uint64_t transmission_texture_id;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t ibl_probe_reserved;
} VkrMetalPacketFrameRoot;

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
  VKR_METAL_PACKET_GPU_DRAW_VIEW_COUNT_MAX = 5u,
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
  uint32_t extent[2];
  uint32_t visible_capacity;
  uint32_t geometry_count;
  uint32_t material_count;
  uint32_t instance_count;
  uint32_t render_mode;
  uint32_t reserved;
} VkrMetalPacketGBufferResolveRoot;

_Static_assert(sizeof(VkrMetalPacketGBufferResolveRoot) == 192,
               "Metal G-buffer resolve root ABI must remain 192 bytes");

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
  Mat4 inverse_view_projection;
  uint32_t extent[2];
  uint32_t sky_enabled;
  uint32_t reserved;
} VkrMetalPacketDeferredLightingRoot;

_Static_assert(sizeof(VkrMetalPacketDeferredLightingRoot) == 144,
               "Metal deferred-lighting root ABI must remain 144 bytes");

/** Per-dispatch frontmost transmission visibility resolve and shading. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketTransmissionShadeRoot {
  uint64_t frame;
  uint64_t visible_rows;
  uint64_t geometry_rows;
  uint64_t instances;
  uint64_t materials;
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
  uint32_t reserved[3];
} VkrMetalPacketTransmissionShadeRoot;

_Static_assert(sizeof(VkrMetalPacketTransmissionShadeRoot) == 272,
               "Metal transmission-shade root ABI must remain 272 bytes");

typedef VkrGpuTransmissionDiagnostics VkrMetalPacketTransmissionDiagnostics;

_Static_assert(sizeof(VkrMetalPacketTransmissionDiagnostics) == 112,
               "Metal transmission diagnostics ABI must remain 112 bytes");
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
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  uint32_t extent[2];
  uint32_t layer;
  uint32_t capacity;
  uint32_t reserved[2];
} VkrMetalPacketTransmissionCompactRoot;

_Static_assert(sizeof(VkrMetalPacketTransmissionCompactRoot) == 80,
               "Metal transmission-compact root ABI must remain 80 bytes");

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
  uint64_t irradiance_texture_id;
  uint64_t prefilter_texture_id;
  Vec4 center_blend;
  Vec4 extents_weight;
  Vec4 intensity_box;
} VkrMetalPacketIblProbe;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketShadowCascade {
  Mat4 light_view_projection;
  Vec4 split_depth;
} VkrMetalPacketShadowCascade;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketTonemapRoot {
  uint64_t source_texture_id;
  float32_t exposure;
  uint32_t padding;
  uint32_t reserved[3];
} VkrMetalPacketTonemapRoot;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketSkyboxRoot {
  Mat4 inverse_view_projection;
  uint64_t cubemap_texture_id;
  float32_t target_width;
  float32_t target_height;
} VkrMetalPacketSkyboxRoot;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketEquirectRoot {
  uint64_t source_texture_id;
  uint64_t target_texture_id;
  uint32_t target_size;
  uint32_t reserved[3];
} VkrMetalPacketEquirectRoot;

typedef struct VKR_SIMD_ALIGN VkrMetalPacketIrradianceRoot {
  uint64_t source_texture_id;
  uint64_t target_texture_id;
  uint32_t sample_count;
  uint32_t target_size;
  uint32_t reserved[2];
} VkrMetalPacketIrradianceRoot;

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

typedef enum VkrMetalPacketAbiRecordId {
  VKR_METAL_PACKET_ABI_VERTEX = 0,
  VKR_METAL_PACKET_ABI_INSTANCE,
  VKR_METAL_PACKET_ABI_MATERIAL,
  VKR_METAL_PACKET_ABI_TEXT_VERTEX,
  VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_FRAME_ROOT,
  VKR_METAL_PACKET_ABI_IBL_PROBE,
  VKR_METAL_PACKET_ABI_SHADOW_CASCADE,
  VKR_METAL_PACKET_ABI_TONEMAP_ROOT,
  VKR_METAL_PACKET_ABI_SKYBOX_ROOT,
  VKR_METAL_PACKET_ABI_EQUIRECT_ROOT,
  VKR_METAL_PACKET_ABI_IRRADIANCE_ROOT,
  VKR_METAL_PACKET_ABI_PREFILTER_ROOT,
  VKR_METAL_PACKET_ABI_TEXT_ROOT,
  VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_PEEL_ROOT,
  VKR_METAL_PACKET_ABI_GBUFFER_RESOLVE_ROOT,
  VKR_METAL_PACKET_ABI_DEFERRED_LIGHTING_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_SHADE_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_COVERAGE_ROOT,
  VKR_METAL_PACKET_ABI_TRANSMISSION_COMPACT_ROOT,
  VKR_METAL_PACKET_ABI_PICKING_RESOLVE_ROOT,
  VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT,
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

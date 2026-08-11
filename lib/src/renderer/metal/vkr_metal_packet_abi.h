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
};

/** Converts VKR's column-major matrix for Slang's row-vector MSL lowering. */
static INLINE Mat4 vkr_metal_packet_slang_draw_matrix(Mat4 matrix) {
  return mat4_transpose(matrix);
}

/** Host representations of every record read through a packet GPU address. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketDrawRoot {
  uint64_t vertices;
  uint64_t instances;
  Mat4 view_projection;
  uint64_t materials;
  uint64_t irradiance_texture_id;
  uint64_t prefilter_texture_id;
  uint64_t brdf_texture_id;
  Vec4 view_position;
  uint32_t material_index;
  uint32_t first_instance;
  uint32_t prefilter_mip_count;
  uint32_t flags;
  Vec4 material_emissive;
  Vec4 material_dielectric_specular;
  Vec4 material_surface;
  Vec4 material_alpha;
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  Vec4 ambient_color;
  uint32_t render_mode;
  uint32_t alpha_mode;
  uint32_t material_flags;
  uint32_t reserved;
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
  Vec4 material_attenuation_color;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t ibl_probe_reserved;
} VkrMetalPacketDrawRoot;

/** Slang vertex functions intentionally consume only this draw-root prefix. */
typedef struct VKR_SIMD_ALIGN VkrMetalPacketVertexDrawRoot {
  uint64_t vertices;
  uint64_t instances;
  Mat4 view_projection;
  uint64_t materials;
  uint64_t irradiance_texture_id;
  uint64_t prefilter_texture_id;
  uint64_t brdf_texture_id;
  Vec4 view_position;
  uint32_t material_index;
  uint32_t first_instance;
  uint32_t prefilter_mip_count;
  uint32_t flags;
  Vec4 material_emissive;
  Vec4 material_dielectric_specular;
  Vec4 material_surface;
  Vec4 material_alpha;
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  Vec4 ambient_color;
  uint32_t render_mode;
  uint32_t alpha_mode;
  uint32_t material_flags;
  uint32_t reserved;
} VkrMetalPacketVertexDrawRoot;

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

typedef struct VKR_SIMD_ALIGN VkrMetalPacketBrdfRoot {
  uint64_t target_texture_id;
  uint32_t sample_count;
  uint32_t target_size;
  uint64_t reserved[2];
} VkrMetalPacketBrdfRoot;

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
  VKR_METAL_PACKET_ABI_IBL_PROBE,
  VKR_METAL_PACKET_ABI_SHADOW_CASCADE,
  VKR_METAL_PACKET_ABI_TONEMAP_ROOT,
  VKR_METAL_PACKET_ABI_SKYBOX_ROOT,
  VKR_METAL_PACKET_ABI_EQUIRECT_ROOT,
  VKR_METAL_PACKET_ABI_IRRADIANCE_ROOT,
  VKR_METAL_PACKET_ABI_PREFILTER_ROOT,
  VKR_METAL_PACKET_ABI_BRDF_ROOT,
  VKR_METAL_PACKET_ABI_TEXT_ROOT,
  VKR_METAL_PACKET_ABI_RECORD_COUNT,
} VkrMetalPacketAbiRecordId;

typedef VkrGpuAbiField VkrMetalPacketAbiField;
typedef VkrGpuAbiRecord VkrMetalPacketAbiRecord;

const VkrMetalPacketAbiRecord *
vkr_metal_packet_abi_record(VkrMetalPacketAbiRecordId id);

/** CPU gate for the durable expected-size/alignment/offset manifest. */
bool8_t vkr_metal_packet_abi_validate_host(void);

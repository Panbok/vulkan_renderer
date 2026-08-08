#include "renderer/metal/vkr_metal_packet_abi.h"

#include <stddef.h>

#define VKR_ABI_FIELD(TYPE, HOST, SHADER, OFFSET)                              \
  {#HOST, SHADER, OFFSET, (uint32_t)offsetof(TYPE, HOST)}

static const VkrMetalPacketAbiField vkr_vertex_fields[] = {
    VKR_ABI_FIELD(VkrVertex3d, position.x, "position_x", 0),
    VKR_ABI_FIELD(VkrVertex3d, position.y, "position_y", 4),
    VKR_ABI_FIELD(VkrVertex3d, position.z, "position_z", 8),
    VKR_ABI_FIELD(VkrVertex3d, normal.x, "normal_x", 12),
    VKR_ABI_FIELD(VkrVertex3d, normal.y, "normal_y", 16),
    VKR_ABI_FIELD(VkrVertex3d, normal.z, "normal_z", 20),
    VKR_ABI_FIELD(VkrVertex3d, texcoord, "texcoord", 24),
    VKR_ABI_FIELD(VkrVertex3d, colour, "color", 32),
    VKR_ABI_FIELD(VkrVertex3d, tangent, "tangent", 48),
};

static const VkrMetalPacketAbiField vkr_instance_fields[] = {
    VKR_ABI_FIELD(VkrInstanceDataGPU, model, "model", 0),
    VKR_ABI_FIELD(VkrInstanceDataGPU, object_id, "object_id", 64),
    VKR_ABI_FIELD(VkrInstanceDataGPU, reserved, "reserved_0", 68),
};

static const VkrMetalPacketAbiField vkr_material_fields[] = {
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, tint, "tint", 0),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, base_color_texture_id,
                  "base_color_texture", 16),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, normal_texture_id, "normal_texture",
                  24),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, orm_texture_id, "orm_texture", 32),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, emissive_texture_id,
                  "emissive_texture", 40),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, base_color_sampler_id,
                  "base_color_sampler", 48),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, normal_sampler_id, "normal_sampler",
                  56),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, orm_sampler_id, "orm_sampler", 64),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, emissive_sampler_id,
                  "emissive_sampler", 72),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_id, "material_id", 80),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, flags, "flags", 84),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, reserved, "reserved", 88),
};

static const VkrMetalPacketAbiField vkr_text_vertex_fields[] = {
    VKR_ABI_FIELD(VkrTextVertex, position, "position", 0),
    VKR_ABI_FIELD(VkrTextVertex, texcoord, "texcoord", 8),
    VKR_ABI_FIELD(VkrTextVertex, color, "color", 16),
};

static const VkrMetalPacketAbiField vkr_vertex_draw_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, vertices, "vertices", 0),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, instances, "instances", 8),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, view_projection,
                  "view_projection", 16),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, materials, "materials_address",
                  80),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, irradiance_texture_id,
                  "irradiance", 88),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, prefilter_texture_id,
                  "prefilter", 96),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, brdf_texture_id, "brdf_lut",
                  104),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, view_position, "view_position",
                  112),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, material_index,
                  "material_index", 128),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, first_instance,
                  "first_instance", 132),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, prefilter_mip_count,
                  "prefilter_mip_count", 136),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, flags, "flags", 140),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, material_emissive,
                  "material_emissive", 144),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, material_dielectric_specular,
                  "material_dielectric_specular", 160),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, material_surface,
                  "material_surface", 176),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, material_alpha,
                  "material_alpha", 192),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, ibl_controls, "ibl_controls",
                  208),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, directional_direction_enabled,
                  "directional_direction_enabled", 224),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, directional_color_intensity,
                  "directional_color_intensity", 240),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, ambient_color, "ambient_color",
                  256),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, render_mode, "render_mode",
                  272),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, alpha_mode, "alpha_mode", 276),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, material_flags,
                  "material_flags", 280),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, reserved, "reserved", 284),
};

static const VkrMetalPacketAbiField vkr_draw_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, vertices, "vertices", 0),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, instances, "instances", 8),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, view_projection, "view_projection",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, materials, "materials", 80),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, irradiance_texture_id, "irradiance",
                  88),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, prefilter_texture_id, "prefilter",
                  96),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, brdf_texture_id, "brdf_lut", 104),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, view_position, "view_position", 112),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, material_index, "material_index",
                  128),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, first_instance, "first_instance",
                  132),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, prefilter_mip_count,
                  "prefilter_mip_count", 136),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, flags, "flags", 140),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, material_emissive,
                  "material_emissive", 144),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, material_dielectric_specular,
                  "material_dielectric_specular", 160),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, material_surface, "material_surface",
                  176),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, material_alpha, "material_alpha",
                  192),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, ibl_controls, "ibl_controls", 208),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, directional_direction_enabled,
                  "directional_direction_enabled", 224),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, directional_color_intensity,
                  "directional_color_intensity", 240),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, ambient_color, "ambient_color", 256),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, render_mode, "render_mode", 272),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, alpha_mode, "alpha_mode", 276),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, material_flags, "material_flags",
                  280),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, reserved, "reserved", 284),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, point_light_data, "point_light_data",
                  288),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, point_light_masks,
                  "point_light_masks", 296),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, point_light_grid_origin_cell_size,
                  "point_light_grid_origin_cell_size", 304),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, point_light_grid_dimensions_count,
                  "point_light_grid_dimensions_count", 320),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, point_light_global_mask,
                  "point_light_global_mask", 336),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, point_light_count,
                  "point_light_count", 352),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, point_light_reserved,
                  "point_light_reserved_0", 356),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, shadow_texture_id, "shadow_map", 368),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, shadow_cascades, "shadow_cascades",
                  376),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, view, "view", 384),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, shadow_cascade_count,
                  "shadow_cascade_count", 448),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, shadow_bias, "shadow_bias", 452),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, shadow_reserved, "shadow_reserved_0",
                  456),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, transmission_texture_id,
                  "transmission_source", 464),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, material_attenuation_color,
                  "material_attenuation_color", 480),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, ibl_probes, "ibl_probes", 496),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, ibl_probe_count, "ibl_probe_count",
                  504),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, ibl_probe_reserved,
                  "ibl_probe_reserved", 508),
};

static const VkrMetalPacketAbiField vkr_ibl_probe_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, irradiance_texture_id, "irradiance",
                  0),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, prefilter_texture_id, "prefilter", 8),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, center_blend, "center_blend", 16),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, extents_weight, "extents_weight", 32),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, intensity_box, "intensity_box", 48),
};

static const VkrMetalPacketAbiField vkr_shadow_cascade_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, light_view_projection,
                  "light_view_projection", 0),
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, split_depth, "split_depth", 64),
};

static const VkrMetalPacketAbiField vkr_tonemap_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, exposure, "exposure", 8),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, reserved, "reserved", 16),
};

static const VkrMetalPacketAbiField vkr_skybox_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSkyboxRoot, inverse_view_projection,
                  "inverse_view_projection", 0),
    VKR_ABI_FIELD(VkrMetalPacketSkyboxRoot, cubemap_texture_id, "cubemap", 64),
    VKR_ABI_FIELD(VkrMetalPacketSkyboxRoot, target_width, "target_size", 72),
};

static const VkrMetalPacketAbiField vkr_equirect_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, target_texture_id, "target", 8),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, target_size, "target_size", 16),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, reserved, "reserved_0", 20),
};

static const VkrMetalPacketAbiField vkr_irradiance_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, target_texture_id, "target", 8),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, sample_count, "sample_count",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, target_size, "target_size", 20),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, reserved, "reserved", 24),
};

static const VkrMetalPacketAbiField vkr_prefilter_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, target_texture_id, "target", 8),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, roughness, "roughness", 16),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, source_face_size,
                  "source_face_size", 20),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, source_mip_count,
                  "source_mip_count", 24),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, target_mip, "target_mip", 28),
};

static const VkrMetalPacketAbiField vkr_brdf_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketBrdfRoot, target_texture_id, "target", 0),
    VKR_ABI_FIELD(VkrMetalPacketBrdfRoot, sample_count, "sample_count", 8),
    VKR_ABI_FIELD(VkrMetalPacketBrdfRoot, target_size, "target_size", 12),
    VKR_ABI_FIELD(VkrMetalPacketBrdfRoot, reserved, "reserved", 16),
};

static const VkrMetalPacketAbiField vkr_text_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, vertices, "vertices", 0),
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, atlas_texture_id, "atlas", 8),
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, model, "model", 16),
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, view_projection, "view_projection",
                  80),
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, controls, "controls", 144),
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, object_id, "object_id", 160),
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, flags, "flags", 164),
    VKR_ABI_FIELD(VkrMetalPacketTextRoot, reserved, "reserved", 168),
};

#define VKR_ABI_RECORD(TYPE, SHADER, SIZE, ALIGNMENT, FIELDS)                  \
  {#TYPE,                                                                      \
   SHADER,                                                                     \
   SIZE,                                                                       \
   ALIGNMENT,                                                                  \
   (uint32_t)sizeof(TYPE),                                                     \
   (uint32_t)_Alignof(TYPE),                                                   \
   FIELDS,                                                                     \
   ArrayCount(FIELDS)}

static const VkrMetalPacketAbiRecord
    vkr_metal_packet_abi_records[VKR_METAL_PACKET_ABI_RECORD_COUNT] = {
        [VKR_METAL_PACKET_ABI_VERTEX] = VKR_ABI_RECORD(
            VkrVertex3d, "VkrMetalPacketVertex", 64, 16, vkr_vertex_fields),
        [VKR_METAL_PACKET_ABI_INSTANCE] =
            VKR_ABI_RECORD(VkrInstanceDataGPU, "VkrMetalPacketInstance", 80, 16,
                           vkr_instance_fields),
        [VKR_METAL_PACKET_ABI_MATERIAL] =
            VKR_ABI_RECORD(VkrMetalMaterialGpuRow, "VkrMetalPacketMaterial", 96,
                           16, vkr_material_fields),
        [VKR_METAL_PACKET_ABI_TEXT_VERTEX] =
            VKR_ABI_RECORD(VkrTextVertex, "VkrMetalPacketTextVertex", 32, 16,
                           vkr_text_vertex_fields),
        [VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketVertexDrawRoot, "VkrMetalPacketDrawRoot", 288, 16,
            vkr_vertex_draw_root_fields),
        [VKR_METAL_PACKET_ABI_DRAW_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketDrawRoot, "VkrMetalPacketDrawRoot",
                           512, 16, vkr_draw_root_fields),
        [VKR_METAL_PACKET_ABI_IBL_PROBE] =
            VKR_ABI_RECORD(VkrMetalPacketIblProbe, "VkrMetalPacketIblProbe", 64,
                           16, vkr_ibl_probe_fields),
        [VKR_METAL_PACKET_ABI_SHADOW_CASCADE] = VKR_ABI_RECORD(
            VkrMetalPacketShadowCascade, "VkrMetalPacketShadowCascade", 80, 16,
            vkr_shadow_cascade_fields),
        [VKR_METAL_PACKET_ABI_TONEMAP_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketTonemapRoot, "VkrMetalPacketTonemapRoot", 32, 16,
            vkr_tonemap_root_fields),
        [VKR_METAL_PACKET_ABI_SKYBOX_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketSkyboxRoot, "VkrMetalPacketSkyboxRoot",
                           80, 16, vkr_skybox_root_fields),
        [VKR_METAL_PACKET_ABI_EQUIRECT_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketEquirectRoot, "VkrMetalPacketEquirectRoot", 32, 16,
            vkr_equirect_root_fields),
        [VKR_METAL_PACKET_ABI_IRRADIANCE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketIrradianceRoot, "VkrMetalPacketIrradianceRoot", 32,
            16, vkr_irradiance_root_fields),
        [VKR_METAL_PACKET_ABI_PREFILTER_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketPrefilterRoot, "VkrMetalPacketPrefilterRoot", 32, 16,
            vkr_prefilter_root_fields),
        [VKR_METAL_PACKET_ABI_BRDF_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketBrdfRoot, "VkrMetalPacketBrdfRoot", 32,
                           16, vkr_brdf_root_fields),
        [VKR_METAL_PACKET_ABI_TEXT_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTextRoot, "VkrMetalPacketTextRoot",
                           176, 16, vkr_text_root_fields),
};

const VkrMetalPacketAbiRecord *
vkr_metal_packet_abi_record(VkrMetalPacketAbiRecordId id) {
  return id < VKR_METAL_PACKET_ABI_RECORD_COUNT
             ? &vkr_metal_packet_abi_records[id]
             : NULL;
}

bool8_t vkr_metal_packet_abi_validate_host(void) {
  for (uint32_t record_index = 0;
       record_index < VKR_METAL_PACKET_ABI_RECORD_COUNT; ++record_index) {
    const VkrMetalPacketAbiRecord *record =
        &vkr_metal_packet_abi_records[record_index];
    if (record->host_size != record->expected_size ||
        record->host_alignment != record->expected_alignment)
      return false_v;
    for (uint32_t field_index = 0; field_index < record->field_count;
         ++field_index) {
      const VkrMetalPacketAbiField *field = &record->fields[field_index];
      if (field->host_offset != field->expected_offset)
        return false_v;
    }
  }
  return sizeof(VkrMetalPacketDrawRoot) <= VKR_METAL_PACKET_DRAW_ROOT_STRIDE &&
                 VKR_METAL_PACKET_DRAW_ROOT_STRIDE %
                         VKR_METAL_PACKET_ROOT_ALIGNMENT ==
                     0
             ? true_v
             : false_v;
}

#undef VKR_ABI_RECORD
#undef VKR_ABI_FIELD

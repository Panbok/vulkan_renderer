#include "metal/vkr_metal_packet_abi.h"

#include <stddef.h>

#define VKR_ABI_FIELD(TYPE, HOST, SHADER, OFFSET)                              \
  {#HOST, SHADER, OFFSET, (uint32_t)offsetof(TYPE, HOST)}

vkr_global const VkrMetalPacketAbiField vkr_material_fields[] = {
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
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, alpha_mode, "alpha_mode", 88),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, temporal_reactivity,
                  "temporal_reactivity", 92),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_emissive,
                  "material_emissive", 96),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_dielectric_specular,
                  "material_dielectric_specular", 112),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_surface, "material_surface",
                  128),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_alpha, "material_alpha",
                  144),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_attenuation_color,
                  "material_attenuation_color", 160),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, clearcoat_texture_id,
                  "clearcoat_texture", 176),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, clearcoat_roughness_texture_id,
                  "clearcoat_roughness_texture", 184),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, clearcoat_normal_texture_id,
                  "clearcoat_normal_texture", 192),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, clearcoat_sampler_id,
                  "clearcoat_sampler", 200),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, clearcoat_roughness_sampler_id,
                  "clearcoat_roughness_sampler", 208),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, clearcoat_normal_sampler_id,
                  "clearcoat_normal_sampler", 216),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_clearcoat,
                  "material_clearcoat", 224),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, sheen_color_texture_id,
                  "sheen_color_texture", 240),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, sheen_roughness_texture_id,
                  "sheen_roughness_texture", 248),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, sheen_color_sampler_id,
                  "sheen_color_sampler", 256),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, sheen_roughness_sampler_id,
                  "sheen_roughness_sampler", 264),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_sheen, "material_sheen",
                  272),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_anisotropy,
                  "material_anisotropy", 288),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, anisotropy_texture_id,
                  "anisotropy_texture", 304),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, anisotropy_sampler_id,
                  "anisotropy_sampler", 312),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_diffuse_transmission,
                  "material_diffuse_transmission", 320),
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, material_subsurface,
                  "material_subsurface", 336),
};

vkr_global const VkrMetalPacketAbiField vkr_transmission_material_fields[] = {
    VKR_ABI_FIELD(VkrMetalTransmissionMaterialGpuRow, transmission_texture_id,
                  "transmission_texture", 0),
    VKR_ABI_FIELD(VkrMetalTransmissionMaterialGpuRow, thickness_texture_id,
                  "thickness_texture", 8),
    VKR_ABI_FIELD(VkrMetalTransmissionMaterialGpuRow, transmission_sampler_id,
                  "transmission_sampler", 16),
    VKR_ABI_FIELD(VkrMetalTransmissionMaterialGpuRow, thickness_sampler_id,
                  "thickness_sampler", 24),
};

vkr_global const VkrMetalPacketAbiField vkr_terrain_material_fields[] = {
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer1_base_color_texture_id,
                  "layer1_base_color_texture", 0),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer1_normal_texture_id,
                  "layer1_normal_texture", 8),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer1_orm_texture_id,
                  "layer1_orm_texture", 16),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer1_sampler_id,
                  "layer1_sampler", 24),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer1_tint, "layer1_tint",
                  32),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer1_surface,
                  "layer1_surface", 48),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer2_base_color_texture_id,
                  "layer2_base_color_texture", 64),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer2_normal_texture_id,
                  "layer2_normal_texture", 72),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer2_orm_texture_id,
                  "layer2_orm_texture", 80),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer2_sampler_id,
                  "layer2_sampler", 88),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer2_tint, "layer2_tint",
                  96),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer2_surface,
                  "layer2_surface", 112),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer3_base_color_texture_id,
                  "layer3_base_color_texture", 128),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer3_normal_texture_id,
                  "layer3_normal_texture", 136),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer3_orm_texture_id,
                  "layer3_orm_texture", 144),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer3_sampler_id,
                  "layer3_sampler", 152),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer3_tint, "layer3_tint",
                  160),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, layer3_surface,
                  "layer3_surface", 176),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, mask_texture_id,
                  "mask_texture", 192),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, mask_sampler_id,
                  "mask_sampler", 200),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, mask_range, "mask_range", 208),
    VKR_ABI_FIELD(VkrMetalTerrainMaterialGpuRow, mask_source, "mask_source",
                  224),
};

vkr_global const VkrMetalPacketAbiField vkr_custom_material_fields[] = {
    VKR_ABI_FIELD(VkrMetalCustomMaterialGpuRow, texture_ids, "textures", 0),
    VKR_ABI_FIELD(VkrMetalCustomMaterialGpuRow, sampler_ids, "samplers", 64),
    VKR_ABI_FIELD(VkrMetalCustomMaterialGpuRow, params, "params", 128),
};

vkr_global const VkrMetalPacketAbiField vkr_vertex_draw_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, geometry_rows, "geometry_rows",
                  0),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, visible_rows, "visible_rows",
                  8),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, vertices, "vertices", 16),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, frame, "frame", 24),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, visible_row_index,
                  "visible_row_index", 32),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, flags, "flags", 36),
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, local_shadow_transmission,
                  "local_shadow_transmission", 40),
};

vkr_global const VkrMetalPacketAbiField vkr_draw_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, geometry_rows, "geometry_rows", 0),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, visible_rows, "visible_rows", 8),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, vertices, "vertices", 16),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, frame, "frame", 24),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, visible_row_index,
                  "visible_row_index", 32),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, flags, "flags", 36),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, local_shadow_transmission,
                  "local_shadow_transmission", 40),
};

vkr_global const VkrMetalPacketAbiField vkr_frame_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, instances, "instances", 0),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, instance_address_padding,
                  "instance_address_padding", 8),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, view_projection, "view_projection",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, materials, "materials", 80),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, sh_coefficients_address,
                  "sh_coefficients", 88),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, prefilter_texture_id, "prefilter",
                  96),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, sh_global_slot, "sh_global_slot",
                  104),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, pre_exposure, "pre_exposure", 108),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, view_position, "view_position", 112),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, prefilter_mip_count,
                  "prefilter_mip_count", 128),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, flags, "flags", 132),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, froxel_fog, "froxel_fog", 136),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_controls, "ibl_controls", 144),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, directional_direction_enabled,
                  "directional_direction_enabled", 160),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, directional_color_intensity,
                  "directional_color_intensity", 176),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ambient_color, "ambient_color", 192),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, render_mode, "render_mode", 208),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_debug_mode,
                  "shadow_debug_mode", 212),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, froxel_integrated_texture_id,
                  "froxel_integrated", 216),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, point_light_data, "point_light_data",
                  224),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, point_light_masks,
                  "point_light_masks", 232),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, point_light_grid_origin_cell_size,
                  "point_light_grid_origin_cell_size", 240),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, point_light_grid_dimensions_count,
                  "point_light_grid_dimensions_count", 256),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, point_light_global_mask,
                  "point_light_global_mask", 272),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, point_light_count,
                  "point_light_count", 288),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, custom_time, "custom_time", 292),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_texture_id, "shadow_map",
                  304),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_cascades, "shadow_cascades",
                  312),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, view, "view", 320),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_cascade_count,
                  "shadow_cascade_count", 384),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_pcf_sample_count,
                  "shadow_pcf_sample_count", 388),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_receiver_bias_texels,
                  "shadow_receiver_bias_texels", 392),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_slope_bias_texels,
                  "shadow_slope_bias_texels", 396),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_normal_offset_texels,
                  "shadow_normal_offset_texels", 400),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_pcf_radius_texels,
                  "shadow_pcf_radius_texels", 404),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_cascade_blend_fraction,
                  "shadow_cascade_blend_fraction", 408),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_fade_start,
                  "shadow_fade_start", 412),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_fade_end, "shadow_fade_end",
                  416),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_pcf_uniform_early_out,
                  "shadow_pcf_uniform_early_out", 420),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, transmission_texture_id,
                  "transmission_source", 424),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_probes, "ibl_probes", 432),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_probe_count, "ibl_probe_count",
                  440),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_probe_reserved,
                  "ibl_probe_reserved", 444),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, temporal_draw_state,
                  "temporal_draw_state", 448),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, local_shadow_texture_id,
                  "local_shadow_map", 456),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, local_shadow_views,
                  "local_shadow_views", 464),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, dfg_texture_id, "dfg_lut", 472),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, diffuse_volume_reserved,
                  "diffuse_volume_reserved", 480),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, diffuse_volume_params,
                  "diffuse_volume_params", 488),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ltc, "ltc", 496),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, fog, "fog", 504),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, sheen, "sheen", 512),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, anisotropy, "anisotropy", 520),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, local_shadow_transmission,
                  "local_shadow_transmission", 528),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, sky, "sky", 536),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, lightmap, "lightmap", 544),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, terrain_materials,
                  "terrain_materials", 552),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, transmission_materials,
                  "transmission_materials", 560),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, decals, "decals", 568),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, custom_materials, "custom_materials",
                  576),
};

vkr_global const VkrMetalPacketAbiField vkr_sky_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSky, params, "params", 0),
    VKR_ABI_FIELD(VkrMetalPacketSky, aerial_perspective_texture_id,
                  "aerial_perspective", 432),
    VKR_ABI_FIELD(VkrMetalPacketSky, transmittance_texture_id, "transmittance",
                  440),
    VKR_ABI_FIELD(VkrMetalPacketSky, multiple_scattering_texture_id,
                  "multiple_scattering", 448),
    VKR_ABI_FIELD(VkrMetalPacketSky, cloud_radiance_texture_id,
                  "cloud_radiance", 456),
    VKR_ABI_FIELD(VkrMetalPacketSky, cloud_shadow_texture_id, "cloud_shadow",
                  464),
    VKR_ABI_FIELD(VkrMetalPacketSky, cloud_base_noise_texture_id,
                  "cloud_base_noise", 472),
    VKR_ABI_FIELD(VkrMetalPacketSky, cloud_detail_noise_texture_id,
                  "cloud_detail_noise", 480),
    VKR_ABI_FIELD(VkrMetalPacketSky, cloud_weather_texture_id, "cloud_weather",
                  488),
    VKR_ABI_FIELD(VkrMetalPacketSky, clear_sh_slot, "clear_sh_slot", 496),
    VKR_ABI_FIELD(VkrMetalPacketSky, cloud_sky_light_face_size,
                  "cloud_sky_light_face_size", 500),
    VKR_ABI_FIELD(VkrMetalPacketSky, cloud_sky_light, "cloud_sky_light", 504),
};

vkr_global const VkrMetalPacketAbiField vkr_sky_params_fields[] = {
    VKR_ABI_FIELD(VkrSkyGpuParams, atmosphere, "atmosphere", 0),
    VKR_ABI_FIELD(VkrSkyGpuParams, view_projection, "view_projection", 160),
    VKR_ABI_FIELD(VkrSkyGpuParams, inverse_view_projection,
                  "inverse_view_projection", 224),
    VKR_ABI_FIELD(VkrSkyGpuParams, camera_position, "camera_position", 288),
    VKR_ABI_FIELD(VkrSkyGpuParams, aerial, "aerial", 304),
    VKR_ABI_FIELD(VkrSkyGpuParams, clouds, "clouds", 320),
    VKR_ABI_FIELD(VkrSkyGpuParams, key_light, "key_light", 384),
    VKR_ABI_FIELD(VkrSkyGpuParams, star_pole, "star_pole", 400),
    VKR_ABI_FIELD(VkrSkyGpuParams, star_axis, "star_axis", 416),
};

vkr_global const VkrMetalPacketAbiField vkr_cloud_params_fields[] = {
    VKR_ABI_FIELD(VkrCloudGpuParams, layer, "layer", 0),
    VKR_ABI_FIELD(VkrCloudGpuParams, noise, "noise", 16),
    VKR_ABI_FIELD(VkrCloudGpuParams, wind, "wind", 32),
    VKR_ABI_FIELD(VkrCloudGpuParams, shadow, "shadow", 48),
};

vkr_global const VkrMetalPacketAbiField vkr_cloud_noise_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketCloudNoiseRoot, base_texture_id, "base", 0),
    VKR_ABI_FIELD(VkrMetalPacketCloudNoiseRoot, detail_texture_id, "detail", 8),
    VKR_ABI_FIELD(VkrMetalPacketCloudNoiseRoot, weather_texture_id, "weather",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketCloudNoiseRoot, reserved, "reserved", 24),
};

vkr_global const VkrMetalPacketAbiField vkr_cloud_trace_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, previous_view_projection,
                  "previous_view_projection", 0),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, sky, "sky", 64),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, frame, "frame", 72),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, depth_texture_id, "depth", 80),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, history_texture_id, "history",
                  88),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, output_texture_id, "output",
                  96),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, extent, "extent", 104),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, depth_extent, "depth_extent",
                  112),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, frame_index, "frame_index",
                  120),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, history_valid, "history_valid",
                  124),
    VKR_ABI_FIELD(VkrMetalPacketCloudTraceRoot, history_pre_exposure_scale,
                  "history_pre_exposure_scale", 128),
};

vkr_global const VkrMetalPacketAbiField
    vkr_diffuse_volume_compose_root_fields[] = {
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot, layer_sh,
                      "layer_sh", 0),
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot, sh, "sh", 8),
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot, probe_count,
                      "probe_count", 16),
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot, rows, "rows", 20),
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot,
                      active_layer_count, "active_layer_count", 24),
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot, active_layers,
                      "active_layers", 32),
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot, active_weights,
                      "active_weights", 64),
        VKR_ABI_FIELD(VkrMetalPacketDiffuseVolumeComposeRoot, active_direct,
                      "active_direct", 96),
};

vkr_global const VkrMetalPacketAbiField vkr_cloud_sky_light_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, sky, "sky", 0),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, frame, "frame", 8),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, radiance, "radiance", 16),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, destination, "destination",
                  24),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, source_texture_id, "source",
                  32),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, source_mip, "source_mip",
                  40),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, face_size, "face_size", 44),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, cloud_to_source_scale,
                  "cloud_to_source_scale", 48),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, window_band_0,
                  "window_band_0", 52),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, window_band_1,
                  "window_band_1", 56),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, window_band_2,
                  "window_band_2", 60),
    VKR_ABI_FIELD(VkrMetalPacketCloudSkyLightRoot, reserved, "reserved", 64),
};

vkr_global const VkrMetalPacketAbiField vkr_sky_build_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSkyBuildRoot, sky, "sky", 0),
    VKR_ABI_FIELD(VkrMetalPacketSkyBuildRoot, output_texture_id, "output", 8),
};

vkr_global const VkrMetalPacketAbiField vkr_anisotropy_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketAnisotropy, table0, "table0", 0),
    VKR_ABI_FIELD(VkrMetalPacketAnisotropy, table1, "table1", 8),
    VKR_ABI_FIELD(VkrMetalPacketAnisotropy, table2, "table2", 16),
    VKR_ABI_FIELD(VkrMetalPacketAnisotropy, reserved, "reserved", 24),
};

vkr_global const VkrMetalPacketAbiField vkr_froxel_params_fields[] = {
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, inverse_view_projection,
                  "inverse_view_projection", 0),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, previous_view_projection,
                  "previous_view_projection", 64),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, previous_view, "previous_view", 128),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, color_density, "color_density", 192),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, height_distance_phase,
                  "height_distance_phase", 208),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, depth_mapping, "depth_mapping", 224),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, temporal_clamp, "temporal_clamp", 240),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, grid_dimensions_cell_pixels,
                  "grid_dimensions_cell_pixels", 256),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, selected_local_indices_count,
                  "selected_local_indices_count", 272),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, boxes, "boxes", 288),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, current_view_projection,
                  "current_view_projection", 800),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, inverse_raster_view_projection,
                  "inverse_raster_view_projection", 864),
    VKR_ABI_FIELD(VkrFroxelFogGpuParams, lighting, "lighting", 928),
};

vkr_global const VkrMetalPacketAbiField vkr_ltc_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketLtc, lights, "lights", 0),
    VKR_ABI_FIELD(VkrMetalPacketLtc, matrix_texture, "matrix_texture", 8),
    VKR_ABI_FIELD(VkrMetalPacketLtc, amplitude_texture, "amplitude_texture",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketLtc, light_count, "light_count", 24),
    VKR_ABI_FIELD(VkrMetalPacketLtc, reserved, "reserved", 28),
};

vkr_global const VkrMetalPacketAbiField vkr_sheen_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSheen, directional_albedo_texture,
                  "directional_albedo_texture", 0),
    VKR_ABI_FIELD(VkrMetalPacketSheen, ltc_matrix_texture, "ltc_matrix_texture",
                  8),
    VKR_ABI_FIELD(VkrMetalPacketSheen, ltc_amplitude_texture,
                  "ltc_amplitude_texture", 16),
    VKR_ABI_FIELD(VkrMetalPacketSheen, ltc_matrix_texture_b,
                  "ltc_matrix_texture_b", 24),
    VKR_ABI_FIELD(VkrMetalPacketSheen, ltc_amplitude_texture_b,
                  "ltc_amplitude_texture_b", 32),
    VKR_ABI_FIELD(VkrMetalPacketSheen, reserved, "reserved", 40),
};

vkr_global const VkrMetalPacketAbiField vkr_diffuse_volume_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketDiffuseVolume, indirection, "indirection", 0),
    VKR_ABI_FIELD(VkrMetalPacketDiffuseVolume, probes, "probes", 8),
    VKR_ABI_FIELD(VkrMetalPacketDiffuseVolume, moments, "moments", 16),
    VKR_ABI_FIELD(VkrMetalPacketDiffuseVolume, sh, "sh", 24),
    VKR_ABI_FIELD(VkrMetalPacketDiffuseVolume, origin, "origin", 32),
    VKR_ABI_FIELD(VkrMetalPacketDiffuseVolume, params, "params", 48),
    VKR_ABI_FIELD(VkrMetalPacketDiffuseVolume, dimensions, "dimensions", 64),
};

vkr_global const VkrMetalPacketAbiField vkr_lightmap_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketLightmap, texture_id, "texture", 0),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, rects, "rects", 8),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, rect_count, "rect_count", 16),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, layer_count, "layer_count", 20),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, inverse_page_size,
                  "inverse_page_size", 24),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, active_layer_count,
                  "active_layer_count", 28),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, active_layers, "active_layers", 32),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, active_weights, "active_weights", 64),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, shadow_mask_id, "shadow_mask", 96),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, stationary, "stationary", 104),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, candidates, "candidates", 112),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, stationary_count, "stationary_count",
                  120),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, shadowed_count, "shadowed_count",
                  124),
    VKR_ABI_FIELD(VkrMetalPacketLightmap, shadowed, "shadowed", 128),
};

vkr_global const VkrMetalPacketAbiField vkr_decals_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketDecals, rows, "rows", 0),
    VKR_ABI_FIELD(VkrMetalPacketDecals, masks, "masks", 8),
    VKR_ABI_FIELD(VkrMetalPacketDecals, grid_origin_cell_size,
                  "grid_origin_cell_size", 16),
    VKR_ABI_FIELD(VkrMetalPacketDecals, grid_dimensions_count,
                  "grid_dimensions_count", 32),
};

vkr_global const VkrMetalPacketAbiField vkr_decal_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketDecal, world_to_box, "world_to_box", 0),
    VKR_ABI_FIELD(VkrMetalPacketDecal, fade, "fade", 48),
    VKR_ABI_FIELD(VkrMetalPacketDecal, material_index, "material_index", 64),
};

vkr_global const VkrMetalPacketAbiField vkr_tiled_sky_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, frame, "frame", 0),
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, sky_view_texture_id, "sky_view", 8),
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, inverse_view_projection,
                  "inverse_view_projection", 16),
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, extent, "extent", 80),
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, sky_mode, "sky_mode", 88),
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, clouds_enabled, "clouds_enabled", 92),
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, sky_radiance, "sky_radiance", 96),
    VKR_ABI_FIELD(VkrMetalTiledSkyRoot, depth_texture_id, "depth", 112),
};

vkr_global const VkrMetalPacketAbiField vkr_ibl_probe_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, sh_slot, "sh_slot", 0),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, sh_reserved, "sh_reserved", 4),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, prefilter_texture_id, "prefilter", 8),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, center_blend, "center_blend", 16),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, extents_weight, "extents_weight", 32),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, intensity_box, "intensity_box", 48),
};

vkr_global const VkrMetalPacketAbiField vkr_shadow_cascade_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, light_view_projection,
                  "light_view_projection", 0),
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, split_near_far_texel_depth,
                  "split_near_far_texel_depth", 64),
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, origin_inv_size_sun,
                  "origin_inv_size_sun", 80),
};

vkr_global const VkrMetalPacketAbiField vkr_display_output_params_fields[] = {
    VKR_ABI_FIELD(VkrDisplayOutputParams, headroom, "headroom", 0),
    VKR_ABI_FIELD(VkrDisplayOutputParams, output_scale, "output_scale", 4),
    VKR_ABI_FIELD(VkrDisplayOutputParams, extended_linear, "extended_linear",
                  8),
    VKR_ABI_FIELD(VkrDisplayOutputParams, reserved, "reserved", 12),
};

vkr_global const VkrMetalPacketAbiField vkr_tonemap_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, flags, "flags", 8),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, image_sharpness, "image_sharpness",
                  12),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, exposure_state, "exposure_state",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, output_extent, "output_extent",
                  24),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, color_grading, "color_grading",
                  32),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, display_output, "display_output",
                  40),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, inverse_pre_exposure,
                  "inverse_pre_exposure", 48),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, bloom_intensity, "bloom_intensity",
                  52),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, bloom_texture_id, "bloom", 56),
};

vkr_global const VkrMetalPacketAbiField vkr_atmosphere_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot, params, "params", 0),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot, transmittance_sample_texture_id,
                  "transmittance_sample", 160),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot,
                  transmittance_storage_texture_id, "transmittance_storage",
                  168),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot,
                  multiple_scattering_sample_texture_id,
                  "multiple_scattering_sample", 176),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot,
                  multiple_scattering_storage_texture_id,
                  "multiple_scattering_storage", 184),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot, source_storage_texture_id,
                  "source_storage", 192),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot, extent, "extent", 200),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot, face_size, "face_size", 208),
    VKR_ABI_FIELD(VkrMetalPacketAtmosphereRoot, reserved, "reserved", 212),
};

vkr_global const VkrMetalPacketAbiField vkr_prefilter_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, target_texture_id, "target", 8),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, roughness, "roughness", 16),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, source_face_size,
                  "source_face_size", 20),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, source_mip_count,
                  "source_mip_count", 24),
    VKR_ABI_FIELD(VkrMetalPacketPrefilterRoot, target_mip, "target_mip", 28),
};

vkr_global const VkrMetalPacketAbiField vkr_sh_project_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, destination, "destination", 8),
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, source_face_size,
                  "source_face_size", 16),
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, source_mip, "source_mip", 20),
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, window_band_0, "window_band_0",
                  24),
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, window_band_1, "window_band_1",
                  28),
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, window_band_2, "window_band_2",
                  32),
    VKR_ABI_FIELD(VkrMetalPacketShProjectRoot, reserved, "reserved", 40),
};

vkr_global const VkrMetalPacketAbiField vkr_text_root_fields[] = {
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

vkr_global const VkrMetalPacketAbiField vkr_ui_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, vertices, "vertices", 0),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, texture_id, "texture", 8),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, target_unit_range, "target_unit_range",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, flags, "flags", 32),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, reserved, "reserved", 36),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, display_output, "display_output", 40),
};

vkr_global const VkrMetalPacketAbiField vkr_ui_vertex_fields[] = {
    VKR_ABI_FIELD(VkrUiVertex, position, "position", 0),
    VKR_ABI_FIELD(VkrUiVertex, texcoord, "texcoord", 8),
    VKR_ABI_FIELD(VkrUiVertex, color, "color", 16),
    VKR_ABI_FIELD(VkrUiVertex, border_color, "border_color", 32),
    VKR_ABI_FIELD(VkrUiVertex, corner_radius_px, "corner_radius", 48),
    VKR_ABI_FIELD(VkrUiVertex, local_px, "local", 64),
    VKR_ABI_FIELD(VkrUiVertex, half_extent_px, "half_extent", 72),
    VKR_ABI_FIELD(VkrUiVertex, border_px, "border", 80),
    VKR_ABI_FIELD(VkrUiVertex, softness_px, "softness", 84),
    VKR_ABI_FIELD(VkrUiVertex, mode, "mode", 88),
    VKR_ABI_FIELD(VkrUiVertex, reserved, "reserved", 92),
};

vkr_global const VkrMetalPacketAbiField vkr_gpu_draw_view_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawView, frustum_planes, "frustum_planes",
                  0),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawView, required_candidate_flags,
                  "required_candidate_flags", 96),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawView, hzb_enabled, "hzb_enabled", 100),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawView, excluded_candidate_flags,
                  "excluded_candidate_flags", 104),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawView, encode_idle, "encode_idle", 108),
};

vkr_global const VkrMetalPacketAbiField vkr_gpu_draw_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, candidates, "candidates", 0),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, geometry_rows, "geometry_rows", 8),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, instances, "instances", 16),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, classifications, "classifications",
                  24),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, compaction_state,
                  "compaction_state", 32),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, visible_rows, "visible_rows", 40),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, draw_roots, "draw_roots", 48),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, views, "views", 56),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, candidate_count, "candidate_count",
                  64),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, visible_capacity,
                  "visible_capacity", 68),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, view_count, "view_count", 72),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, encode_view_index,
                  "encode_view_index", 76),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, hzb_texture_id, "hzb", 80),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, lod_views, "lod_views", 88),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, history_view_projection,
                  "history_view_projection", 96),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, hzb_extent, "hzb_extent", 160),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, hzb_mip_count, "hzb_mip_count",
                  168),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, hzb_enabled, "hzb_enabled", 172),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, hzb_depth_epsilon,
                  "hzb_depth_epsilon", 176),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, icb_view_group_size,
                  "icb_view_group_size", 180),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, custom_compaction,
                  "custom_compaction", 184),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, static_candidate_count,
                  "static_candidate_count", 192),
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, reserved_3, "reserved_3", 196),
};

vkr_global const VkrMetalPacketAbiField
    vkr_local_shadow_transmission_fields[] = {
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmission, depth0, "depth0",
                      0),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmission, color0, "color0",
                      8),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmission, depth1, "depth1",
                      16),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmission, color1, "color1",
                      24),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmission, overflow,
                      "overflow", 32),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmission, reserved,
                      "reserved", 40),
};

vkr_global const VkrMetalPacketAbiField
    vkr_local_shadow_transmission_draw_fields[] = {
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmissionDraw,
                      transmission_materials, "transmission_materials", 0),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmissionDraw, face, "face",
                      8),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmissionDraw, reserved,
                      "reserved", 12),
        VKR_ABI_FIELD(VkrMetalPacketLocalShadowTransmissionDraw, light_position,
                      "light_position", 16),
};

vkr_global const VkrMetalPacketAbiField vkr_hzb_build_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketHzbBuildRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketHzbBuildRoot, destination_texture_id,
                  "destination", 8),
    VKR_ABI_FIELD(VkrMetalPacketHzbBuildRoot, source_extent, "source_extent",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketHzbBuildRoot, destination_extent,
                  "destination_extent", 24),
    VKR_ABI_FIELD(VkrMetalPacketHzbBuildRoot, source_is_depth,
                  "source_is_depth", 32),
    VKR_ABI_FIELD(VkrMetalPacketHzbBuildRoot, reserved, "reserved", 36),
};

vkr_global const VkrMetalPacketAbiField vkr_exposure_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, histogram, "histogram", 0),
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, state, "state", 8),
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, previous_state, "previous_state",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, source_texture_id, "source", 24),
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, extent, "extent", 32),
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, reset_reasons, "reset_reasons",
                  40),
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, reserved, "reserved", 44),
    VKR_ABI_FIELD(VkrMetalPacketExposureRoot, metering, "metering", 48),
};

vkr_global const VkrMetalPacketAbiField vkr_bloom_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketBloomRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketBloomRoot, coarse_texture_id, "coarse", 8),
    VKR_ABI_FIELD(VkrMetalPacketBloomRoot, destination_texture_id,
                  "destination", 16),
    VKR_ABI_FIELD(VkrMetalPacketBloomRoot, filter_extent, "filter_extent", 24),
    VKR_ABI_FIELD(VkrMetalPacketBloomRoot, destination_extent,
                  "destination_extent", 32),
    VKR_ABI_FIELD(VkrMetalPacketBloomRoot, params, "params", 40),
    VKR_ABI_FIELD(VkrMetalPacketBloomRoot, reserved, "reserved", 72),
};

vkr_global const VkrMetalPacketAbiField vkr_fog_params_fields[] = {
    VKR_ABI_FIELD(VkrFogGpuParams, color_density, "color_density", 0),
    VKR_ABI_FIELD(VkrFogGpuParams, height_distance, "height_distance", 16),
    VKR_ABI_FIELD(VkrFogGpuParams, sky_lighting, "sky_lighting", 32),
};

vkr_global const VkrMetalPacketAbiField vkr_animation_preview_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, model, "model", 0),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, view_projection,
                  "view_projection", 64),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, vertices, "vertices",
                  128),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, decode, "decode", 136),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, deformation_address,
                  "deformation_address", 144),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, reserved, "reserved",
                  152),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, tint, "tint", 160),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, first_vertex,
                  "first_vertex", 176),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, decode_index,
                  "decode_index", 180),
    VKR_ABI_FIELD(VkrMetalPacketAnimationPreviewRoot, reserved_tail,
                  "reserved_tail", 184),
};

vkr_global const VkrMetalPacketAbiField vkr_skinning_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, bind_vertices, "bind_vertices",
                  0),
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, influences, "influences", 8),
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, palette, "palette", 16),
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, output, "output", 24),
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, vertex_count, "vertex_count", 32),
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, palette_count, "palette_count",
                  36),
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, reserved0, "reserved0", 40),
    VKR_ABI_FIELD(VkrMetalPacketSkinningRoot, reserved1, "reserved1", 44),
};

vkr_global const VkrMetalPacketAbiField vkr_editor_overlay_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketEditorOverlayRoot, vertices, "vertices", 0),
    VKR_ABI_FIELD(VkrMetalPacketEditorOverlayRoot, decode, "decode", 8),
    VKR_ABI_FIELD(VkrMetalPacketEditorOverlayRoot, model_view_projection,
                  "model_view_projection", 16),
    VKR_ABI_FIELD(VkrMetalPacketEditorOverlayRoot, color, "color", 80),
    VKR_ABI_FIELD(VkrMetalPacketEditorOverlayRoot, object_id, "object_id", 96),
    VKR_ABI_FIELD(VkrMetalPacketEditorOverlayRoot, reserved, "reserved", 100),
    VKR_ABI_FIELD(VkrMetalPacketEditorOverlayRoot, display_output,
                  "display_output", 104),
};

vkr_global const VkrMetalPacketAbiField vkr_selection_outline_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSelectionOutlineRoot, mask, "mask", 0),
    VKR_ABI_FIELD(VkrMetalPacketSelectionOutlineRoot, color, "color", 16),
    VKR_ABI_FIELD(VkrMetalPacketSelectionOutlineRoot, radius_px, "radius_px",
                  32),
    VKR_ABI_FIELD(VkrMetalPacketSelectionOutlineRoot, reserved, "reserved", 36),
    VKR_ABI_FIELD(VkrMetalPacketSelectionOutlineRoot, display_output,
                  "display_output", 40),
};

vkr_global const VkrMetalPacketAbiField vkr_editor_grid_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, inverse_view_projection,
                  "inverse_view_projection", 0),
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, camera_position,
                  "camera_position", 64),
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, params, "params", 80),
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, extent, "extent", 96),
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, plane, "plane", 104),
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, reserved, "reserved", 108),
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, depth, "depth", 112),
    VKR_ABI_FIELD(VkrMetalPacketEditorGridRoot, display_output,
                  "display_output", 120),
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

vkr_global const VkrMetalPacketAbiRecord
    vkr_metal_packet_abi_records[VKR_METAL_PACKET_ABI_RECORD_COUNT] = {
        [VKR_METAL_PACKET_ABI_MATERIAL] =
            VKR_ABI_RECORD(VkrMetalMaterialGpuRow, "VkrMetalPacketMaterial",
                           352, 16, vkr_material_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_MATERIAL] =
            VKR_ABI_RECORD(VkrMetalTransmissionMaterialGpuRow,
                           "VkrMetalPacketTransmissionMaterial", 32, 16,
                           vkr_transmission_material_fields),
        [VKR_METAL_PACKET_ABI_TERRAIN_MATERIAL] = VKR_ABI_RECORD(
            VkrMetalTerrainMaterialGpuRow, "VkrMetalPacketTerrainMaterial", 240,
            16, vkr_terrain_material_fields),
        [VKR_METAL_PACKET_ABI_CUSTOM_MATERIAL] = VKR_ABI_RECORD(
            VkrMetalCustomMaterialGpuRow, "VkrMetalPacketCustomMaterial", 256,
            16, vkr_custom_material_fields),
        [VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketVertexDrawRoot, "VkrMetalPacketDrawRoot", 48, 16,
            vkr_vertex_draw_root_fields),
        [VKR_METAL_PACKET_ABI_DRAW_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketDrawRoot, "VkrMetalPacketDrawRoot", 48,
                           16, vkr_draw_root_fields),
        [VKR_METAL_PACKET_ABI_FRAME_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketFrameRoot, "VkrMetalPacketFrameRoot",
                           592, 16, vkr_frame_root_fields),
        [VKR_METAL_PACKET_ABI_LOCAL_SHADOW_TRANSMISSION] =
            VKR_ABI_RECORD(VkrMetalPacketLocalShadowTransmission,
                           "VkrMetalPacketLocalShadowTransmission", 48, 16,
                           vkr_local_shadow_transmission_fields),
        [VKR_METAL_PACKET_ABI_LOCAL_SHADOW_TRANSMISSION_DRAW] =
            VKR_ABI_RECORD(VkrMetalPacketLocalShadowTransmissionDraw,
                           "VkrMetalPacketLocalShadowTransmissionDraw", 32, 16,
                           vkr_local_shadow_transmission_draw_fields),
        [VKR_METAL_PACKET_ABI_LTC] = VKR_ABI_RECORD(
            VkrMetalPacketLtc, "VkrMetalPacketLtc", 32, 16, vkr_ltc_fields),
        [VKR_METAL_PACKET_ABI_SHEEN] =
            VKR_ABI_RECORD(VkrMetalPacketSheen, "VkrMetalPacketSheen", 48, 16,
                           vkr_sheen_fields),
        [VKR_METAL_PACKET_ABI_ANISOTROPY] =
            VKR_ABI_RECORD(VkrMetalPacketAnisotropy, "VkrMetalPacketAnisotropy",
                           32, 16, vkr_anisotropy_fields),
        [VKR_METAL_PACKET_ABI_DIFFUSE_VOLUME] = VKR_ABI_RECORD(
            VkrMetalPacketDiffuseVolume, "VkrMetalPacketDiffuseVolume", 80, 16,
            vkr_diffuse_volume_fields),
        [VKR_METAL_PACKET_ABI_LIGHTMAP] =
            VKR_ABI_RECORD(VkrMetalPacketLightmap, "VkrMetalPacketLightmap",
                           160, 16, vkr_lightmap_fields),
        [VKR_METAL_PACKET_ABI_DECALS] =
            VKR_ABI_RECORD(VkrMetalPacketDecals, "VkrMetalPacketDecals", 48, 16,
                           vkr_decals_fields),
        [VKR_METAL_PACKET_ABI_DECAL] =
            VKR_ABI_RECORD(VkrMetalPacketDecal, "VkrMetalPacketDecal", 80, 16,
                           vkr_decal_fields),
        [VKR_METAL_PACKET_ABI_TILED_SKY_ROOT] =
            VKR_ABI_RECORD(VkrMetalTiledSkyRoot, "VkrMetalTiledSkyRoot", 128,
                           16, vkr_tiled_sky_root_fields),
        [VKR_METAL_PACKET_ABI_IBL_PROBE] =
            VKR_ABI_RECORD(VkrMetalPacketIblProbe, "VkrMetalPacketIblProbe", 64,
                           16, vkr_ibl_probe_fields),
        [VKR_METAL_PACKET_ABI_SHADOW_CASCADE] = VKR_ABI_RECORD(
            VkrMetalPacketShadowCascade, "VkrMetalPacketShadowCascade", 96, 16,
            vkr_shadow_cascade_fields),
        [VKR_METAL_PACKET_ABI_DISPLAY_OUTPUT_PARAMS] =
            VKR_ABI_RECORD(VkrDisplayOutputParams, "VkrDisplayOutputParams", 16,
                           4, vkr_display_output_params_fields),
        [VKR_METAL_PACKET_ABI_TONEMAP_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketTonemapRoot, "VkrMetalPacketTonemapRoot", 64, 16,
            vkr_tonemap_root_fields),
        [VKR_METAL_PACKET_ABI_ATMOSPHERE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketAtmosphereRoot, "VkrMetalPacketAtmosphereRoot", 224,
            16, vkr_atmosphere_root_fields),
        [VKR_METAL_PACKET_ABI_PREFILTER_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketPrefilterRoot, "VkrMetalPacketPrefilterRoot", 32, 16,
            vkr_prefilter_root_fields),
        [VKR_METAL_PACKET_ABI_SH_PROJECT_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketShProjectRoot, "VkrMetalPacketShProjectRoot", 48, 16,
            vkr_sh_project_root_fields),
        [VKR_METAL_PACKET_ABI_TEXT_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTextRoot, "VkrMetalPacketTextRoot",
                           176, 16, vkr_text_root_fields),
        [VKR_METAL_PACKET_ABI_UI_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketUiRoot, "VkrMetalPacketUiRoot", 48, 16,
                           vkr_ui_root_fields),
        [VKR_METAL_PACKET_ABI_UI_VERTEX] =
            VKR_ABI_RECORD(VkrUiVertex, "VkrMetalPacketUiVertex", 96, 16,
                           vkr_ui_vertex_fields),
        [VKR_METAL_PACKET_ABI_EDITOR_OVERLAY_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketEditorOverlayRoot, "VkrMetalPacketEditorOverlayRoot",
            112, 16, vkr_editor_overlay_root_fields),
        [VKR_METAL_PACKET_ABI_SELECTION_OUTLINE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketSelectionOutlineRoot,
                           "VkrMetalPacketSelectionOutlineRoot", 48, 16,
                           vkr_selection_outline_root_fields),
        [VKR_METAL_PACKET_ABI_EDITOR_GRID_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketEditorGridRoot, "VkrMetalPacketEditorGridRoot", 128,
            16, vkr_editor_grid_root_fields),
        [VKR_METAL_PACKET_ABI_GPU_DRAW_VIEW] = VKR_ABI_RECORD(
            VkrMetalPacketGpuDrawView, "VkrMetalPacketGpuDrawView", 112, 16,
            vkr_gpu_draw_view_fields),
        [VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketGpuDrawRoot, "VkrMetalPacketGpuDrawRoot", 208, 16,
            vkr_gpu_draw_root_fields),
        [VKR_METAL_PACKET_ABI_ANIMATION_PREVIEW_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketAnimationPreviewRoot,
                           "VkrMetalPacketAnimationPreviewRoot", 192, 16,
                           vkr_animation_preview_root_fields),
        [VKR_METAL_PACKET_ABI_SKINNING_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketSkinningRoot, "VkrMetalPacketSkinningRoot", 48, 16,
            vkr_skinning_root_fields),
        [VKR_METAL_PACKET_ABI_FOG_PARAMS] = VKR_ABI_RECORD(
            VkrFogGpuParams, "VkrFogParams", 48, 16, vkr_fog_params_fields),
        [VKR_METAL_PACKET_ABI_FROXEL_PARAMS] =
            VKR_ABI_RECORD(VkrFroxelFogGpuParams, "VkrFroxelFogParams", 944, 16,
                           vkr_froxel_params_fields),
        [VKR_METAL_PACKET_ABI_SKY] = VKR_ABI_RECORD(
            VkrMetalPacketSky, "VkrMetalPacketSky", 512, 16, vkr_sky_fields),
        [VKR_METAL_PACKET_ABI_SKY_PARAMS] = VKR_ABI_RECORD(
            VkrSkyGpuParams, "VkrSkyParams", 432, 16, vkr_sky_params_fields),
        [VKR_METAL_PACKET_ABI_SKY_VIEW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketSkyBuildRoot, "VkrMetalPacketSkyViewRoot", 16, 16,
            vkr_sky_build_root_fields),
        [VKR_METAL_PACKET_ABI_AERIAL_PERSPECTIVE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketSkyBuildRoot, "VkrMetalPacketAerialPerspectiveRoot",
            16, 16, vkr_sky_build_root_fields),
        [VKR_METAL_PACKET_ABI_CLOUD_PARAMS] =
            VKR_ABI_RECORD(VkrCloudGpuParams, "VkrCloudParams", 64, 16,
                           vkr_cloud_params_fields),
        [VKR_METAL_PACKET_ABI_CLOUD_NOISE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketCloudNoiseRoot, "VkrMetalPacketCloudNoiseRoot", 32,
            16, vkr_cloud_noise_root_fields),
        [VKR_METAL_PACKET_ABI_CLOUD_SHADOW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketSkyBuildRoot, "VkrMetalPacketCloudShadowRoot", 16, 16,
            vkr_sky_build_root_fields),
        [VKR_METAL_PACKET_ABI_CLOUD_TRACE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketCloudTraceRoot, "VkrMetalPacketCloudTraceRoot", 144,
            16, vkr_cloud_trace_root_fields),
        [VKR_METAL_PACKET_ABI_CLOUD_SKY_LIGHT_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketCloudSkyLightRoot, "VkrMetalPacketCloudSkyLightRoot",
            80, 16, vkr_cloud_sky_light_root_fields),
        [VKR_METAL_PACKET_ABI_DIFFUSE_VOLUME_COMPOSE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketDiffuseVolumeComposeRoot,
                           "VkrMetalPacketDiffuseVolumeComposeRoot", 128, 16,
                           vkr_diffuse_volume_compose_root_fields),
        [VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketHzbBuildRoot, "VkrMetalPacketHzbBuildRoot", 48, 16,
            vkr_hzb_build_root_fields),
        [VKR_METAL_PACKET_ABI_EXPOSURE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketExposureRoot, "VkrMetalPacketExposureRoot", 112, 16,
            vkr_exposure_root_fields),
        [VKR_METAL_PACKET_ABI_BLOOM_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketBloomRoot, "VkrMetalPacketBloomRoot",
                           80, 16, vkr_bloom_root_fields),
};

const VkrMetalPacketAbiRecord *
vkr_metal_packet_abi_record(VkrMetalPacketAbiRecordId id) {
  switch (id) {
  case VKR_METAL_PACKET_ABI_VERTEX:
    return vkr_gpu_abi_record(VKR_GPU_ABI_VERTEX);
  case VKR_METAL_PACKET_ABI_INSTANCE:
    return vkr_gpu_abi_record(VKR_GPU_ABI_INSTANCE);
  case VKR_METAL_PACKET_ABI_TEXT_VERTEX:
    return vkr_gpu_abi_record(VKR_GPU_ABI_TEXT_VERTEX);
  default:
    break;
  }
  return id < VKR_METAL_PACKET_ABI_RECORD_COUNT
             ? &vkr_metal_packet_abi_records[id]
             : NULL;
}

bool8_t
vkr_metal_packet_abi_alignment_compatible(VkrMetalPacketAbiRecordId id,
                                          uint32_t shader_min_alignment) {
  const VkrMetalPacketAbiRecord *record = vkr_metal_packet_abi_record(id);
  return record && shader_min_alignment > 0u &&
                 record->expected_alignment >= shader_min_alignment &&
                 record->expected_alignment % shader_min_alignment == 0u
             ? true_v
             : false_v;
}

bool8_t vkr_metal_packet_abi_validate_host(void) {
  for (uint32_t record_index = 0;
       record_index < VKR_METAL_PACKET_ABI_RECORD_COUNT; ++record_index) {
    const VkrMetalPacketAbiRecord *record =
        vkr_metal_packet_abi_record((VkrMetalPacketAbiRecordId)record_index);
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
                 sizeof(VkrMetalPacketFrameRoot) <=
                     (uint64_t)VKR_METAL_PACKET_FRAME_ROOT_CELL_COUNT *
                         VKR_METAL_PACKET_DRAW_ROOT_STRIDE &&
                 VKR_METAL_PACKET_DRAW_ROOT_STRIDE %
                         VKR_METAL_PACKET_ROOT_ALIGNMENT ==
                     0
             ? true_v
             : false_v;
}

#undef VKR_ABI_RECORD
#undef VKR_ABI_FIELD

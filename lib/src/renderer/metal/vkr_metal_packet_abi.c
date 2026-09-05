#include "renderer/metal/vkr_metal_packet_abi.h"

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
    VKR_ABI_FIELD(VkrMetalPacketVertexDrawRoot, reserved, "reserved", 40),
};

vkr_global const VkrMetalPacketAbiField vkr_draw_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, geometry_rows, "geometry_rows", 0),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, visible_rows, "visible_rows", 8),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, vertices, "vertices", 16),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, frame, "frame", 24),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, visible_row_index,
                  "visible_row_index", 32),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, flags, "flags", 36),
    VKR_ABI_FIELD(VkrMetalPacketDrawRoot, reserved, "reserved", 40),
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
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, sh_reserved, "sh_reserved", 108),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, view_position, "view_position", 112),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, prefilter_mip_count,
                  "prefilter_mip_count", 128),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, flags, "flags", 132),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_controls, "ibl_controls", 144),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, directional_direction_enabled,
                  "directional_direction_enabled", 160),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, directional_color_intensity,
                  "directional_color_intensity", 176),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ambient_color, "ambient_color", 192),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, render_mode, "render_mode", 208),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_debug_mode,
                  "shadow_debug_mode", 212),
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
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, origin_inv_size_pad,
                  "origin_inv_size_pad", 80),
};

vkr_global const VkrMetalPacketAbiField vkr_tonemap_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, reserved, "reserved", 8),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, exposure_state, "exposure_state",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, output_extent, "output_extent",
                  24),
};

vkr_global const VkrMetalPacketAbiField vkr_equirect_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, target_texture_id, "target", 8),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, target_size, "target_size", 16),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, reserved, "reserved_0", 20),
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
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, rect_extent, "rect_extent", 32),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, mode, "mode", 40),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, flags, "flags", 44),
    VKR_ABI_FIELD(VkrMetalPacketUiRoot, corner_radii, "corner_radii", 48),
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
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, reserved_2, "reserved_2", 88),
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
    VKR_ABI_FIELD(VkrMetalPacketGpuDrawRoot, reserved_3, "reserved_3", 184),
};

vkr_global const VkrMetalPacketAbiField vkr_transmission_peel_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTransmissionPeelRoot, previous_depth_texture_id,
                  "previous_depth", 0),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionPeelRoot, depth_epsilon,
                  "depth_epsilon", 8),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionPeelRoot, previous_depth_enabled,
                  "previous_depth_enabled", 12),
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

vkr_global const VkrMetalPacketAbiField vkr_sdsm_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSdsmRoot, depth_texture_id, "depth", 0),
    VKR_ABI_FIELD(VkrMetalPacketSdsmRoot, vbuffer_texture_id, "vbuffer", 8),
    VKR_ABI_FIELD(VkrMetalPacketSdsmRoot, reduce_state, "reduce_state", 16),
    VKR_ABI_FIELD(VkrMetalPacketSdsmRoot, extent, "extent", 24),
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

vkr_global const VkrMetalPacketAbiField vkr_gtao_params_fields[] = {
    VKR_ABI_FIELD(VkrGtaoGpuParams, view, "view", 0),
    VKR_ABI_FIELD(VkrGtaoGpuParams, viewport_width, "viewport_width", 64),
    VKR_ABI_FIELD(VkrGtaoGpuParams, viewport_height, "viewport_height", 68),
    VKR_ABI_FIELD(VkrGtaoGpuParams, depth_mip_count, "depth_mip_count", 72),
    VKR_ABI_FIELD(VkrGtaoGpuParams, reserved_u32_0, "reserved_u32_0", 76),
    VKR_ABI_FIELD(VkrGtaoGpuParams, viewport_pixel_size_x,
                  "viewport_pixel_size_x", 80),
    VKR_ABI_FIELD(VkrGtaoGpuParams, viewport_pixel_size_y,
                  "viewport_pixel_size_y", 84),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m22, "projection_m22", 88),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m23, "projection_m23", 92),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m32, "projection_m32", 96),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m33, "projection_m33", 100),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m00, "projection_m00", 104),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m11, "projection_m11", 108),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m02, "projection_m02", 112),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m12, "projection_m12", 116),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m03, "projection_m03", 120),
    VKR_ABI_FIELD(VkrGtaoGpuParams, projection_m13, "projection_m13", 124),
    VKR_ABI_FIELD(VkrGtaoGpuParams, effect_radius, "effect_radius", 128),
    VKR_ABI_FIELD(VkrGtaoGpuParams, radius_multiplier, "radius_multiplier",
                  132),
    VKR_ABI_FIELD(VkrGtaoGpuParams, falloff_range, "falloff_range", 136),
    VKR_ABI_FIELD(VkrGtaoGpuParams, falloff_mul, "falloff_mul", 140),
    VKR_ABI_FIELD(VkrGtaoGpuParams, falloff_add, "falloff_add", 144),
    VKR_ABI_FIELD(VkrGtaoGpuParams, depth_mip_falloff_mul,
                  "depth_mip_falloff_mul", 148),
    VKR_ABI_FIELD(VkrGtaoGpuParams, sample_distribution_power,
                  "sample_distribution_power", 152),
    VKR_ABI_FIELD(VkrGtaoGpuParams, final_value_power, "final_value_power",
                  156),
    VKR_ABI_FIELD(VkrGtaoGpuParams, depth_mip_sampling_offset,
                  "depth_mip_sampling_offset", 160),
    VKR_ABI_FIELD(VkrGtaoGpuParams, denoise_blur_beta, "denoise_blur_beta",
                  164),
    VKR_ABI_FIELD(VkrGtaoGpuParams, reserved_float0, "reserved_float0", 168),
    VKR_ABI_FIELD(VkrGtaoGpuParams, reserved_float1, "reserved_float1", 172),
    VKR_ABI_FIELD(VkrGtaoGpuParams, slice_count, "slice_count", 176),
    VKR_ABI_FIELD(VkrGtaoGpuParams, steps_per_slice, "steps_per_slice", 180),
    VKR_ABI_FIELD(VkrGtaoGpuParams, noise_index, "noise_index", 184),
    VKR_ABI_FIELD(VkrGtaoGpuParams, reserved0, "reserved0", 188),
};

vkr_global const VkrMetalPacketAbiField vkr_gtao_depth_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketGtaoDepthRoot, params, "params", 0),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDepthRoot, source_texture_id, "source",
                  192),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDepthRoot, destination_texture_id,
                  "destination", 200),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDepthRoot, source_extent, "source_extent",
                  208),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDepthRoot, destination_extent,
                  "destination_extent", 216),
};

vkr_global const VkrMetalPacketAbiField vkr_gtao_evaluate_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, params, "params", 0),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, vbuffer_texture_id, "vbuffer",
                  192),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, view_depth_texture_id,
                  "view_depth", 200),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, normal_texture_id, "normal",
                  208),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, destination_texture_id,
                  "destination", 216),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, edges_texture_id, "edges",
                  224),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, source_extent,
                  "source_extent", 232),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, destination_extent,
                  "destination_extent", 240),
    VKR_ABI_FIELD(VkrMetalPacketGtaoEvaluateRoot, reserved, "reserved", 248),
};

vkr_global const VkrMetalPacketAbiField vkr_gtao_denoise_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketGtaoDenoiseRoot, params, "params", 0),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDenoiseRoot, source_texture_id, "source",
                  192),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDenoiseRoot, edges_texture_id, "edges",
                  200),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDenoiseRoot, destination_texture_id,
                  "destination", 208),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDenoiseRoot, source_extent, "source_extent",
                  216),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDenoiseRoot, destination_extent,
                  "destination_extent", 224),
    VKR_ABI_FIELD(VkrMetalPacketGtaoDenoiseRoot, reserved, "reserved", 232),
};

vkr_global const VkrMetalPacketAbiField vkr_temporal_transform_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTemporalTransformRoot, instances, "instances",
                  0),
    VKR_ABI_FIELD(VkrMetalPacketTemporalTransformRoot, transforms, "transforms",
                  8),
    VKR_ABI_FIELD(VkrMetalPacketTemporalTransformRoot, instance_count,
                  "instance_count", 16),
    VKR_ABI_FIELD(VkrMetalPacketTemporalTransformRoot, transform_capacity,
                  "transform_capacity", 20),
    VKR_ABI_FIELD(VkrMetalPacketTemporalTransformRoot, frame_index,
                  "frame_index", 24),
    VKR_ABI_FIELD(VkrMetalPacketTemporalTransformRoot, reserved, "reserved",
                  28),
};

vkr_global const VkrMetalPacketAbiField vkr_gbuffer_resolve_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, visible_rows,
                  "visible_rows", 0),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, geometry_rows,
                  "geometry_rows", 8),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, instances, "instances", 16),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, materials, "materials", 24),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, compaction_state,
                  "compaction_state", 32),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, vbuffer_texture_id,
                  "vbuffer", 40),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, albedo_texture_id, "albedo",
                  48),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, specular_texture_id,
                  "specular", 56),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, normal_texture_id, "normal",
                  64),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, emissive_texture_id,
                  "emissive", 72),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, debug_texture_id, "debug",
                  80),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, hdr_seed_texture_id,
                  "hdr_seed", 88),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, view_projection,
                  "view_projection", 96),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, current_view_projection,
                  "current_view_projection", 160),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, previous_view_projection,
                  "previous_view_projection", 224),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, previous_transforms,
                  "previous_transforms", 288),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, motion_texture_id, "motion",
                  296),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, validity_texture_id,
                  "validity", 304),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, extent, "extent", 312),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, visible_capacity,
                  "visible_capacity", 320),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, geometry_count,
                  "geometry_count", 324),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, material_count,
                  "material_count", 328),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, instance_count,
                  "instance_count", 332),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, render_mode, "render_mode",
                  336),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, history_valid,
                  "history_valid", 340),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, previous_frame_index,
                  "previous_frame_index", 344),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, reserved, "reserved", 348),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, sky_reprojection,
                  "sky_reprojection", 352),
};

vkr_global const VkrMetalPacketAbiField vkr_deferred_lighting_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, frame, "frame", 0),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, vbuffer_texture_id,
                  "vbuffer", 8),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, depth_texture_id, "depth",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, albedo_texture_id,
                  "albedo", 24),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, specular_texture_id,
                  "specular", 32),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, normal_texture_id,
                  "normal", 40),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, hdr_texture_id, "hdr",
                  48),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, sky_texture_id, "sky",
                  56),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot,
                  gtao_visibility_texture_id, "gtao_visibility", 64),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, inverse_view_projection,
                  "inverse_view_projection", 80),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, extent, "extent", 144),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, sky_enabled,
                  "sky_enabled", 152),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, reserved, "reserved",
                  156),
};

vkr_global const VkrMetalPacketAbiField vkr_temporal_resolve_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, visible_rows,
                  "visible_rows", 0),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, instances, "instances", 8),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, scene_texture_id, "scene",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot,
                  pre_transmission_texture_id, "pre_transmission", 24),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, motion_texture_id,
                  "motion", 32),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, validity_texture_id,
                  "validity", 40),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, depth_texture_id, "depth",
                  48),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, vbuffer_texture_id,
                  "vbuffer", 56),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, history_color_texture_id,
                  "history_color", 64),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, history_depth_texture_id,
                  "history_depth", 72),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot,
                  history_identity_texture_id, "history_identity", 80),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot,
                  history_surface_texture_id, "history_surface", 88),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, output_color_texture_id,
                  "output_color", 96),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, output_depth_texture_id,
                  "output_depth", 104),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, output_identity_texture_id,
                  "output_identity", 112),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot,
                  output_surface_texture_id, "output_surface", 120),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, extent, "extent", 128),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, history_valid,
                  "history_valid", 136),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, render_mode, "render_mode",
                  140),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, camera_stationary,
                  "camera_stationary", 144),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, transmission_visible_rows,
                  "transmission_visible_rows", 152),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, transmission_instances,
                  "transmission_instances", 160),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot,
                  transmission_vbuffer_texture_id, "transmission_vbuffer", 168),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot,
                  transmission_depth_texture_id, "transmission_depth", 176),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, transmission_enabled,
                  "transmission_enabled", 184),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot,
                  transmission_alignment_padding,
                  "transmission_alignment_padding", 188),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, transmission_reserved,
                  "transmission_reserved", 192),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, current_jitter_pixels,
                  "current_jitter_pixels", 200),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, previous_jitter_pixels,
                  "previous_jitter_pixels", 208),
    VKR_ABI_FIELD(VkrMetalPacketTemporalResolveRoot, scene_stationary,
                  "scene_stationary", 216),
};

vkr_global const VkrMetalPacketAbiField vkr_transmission_shade_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, frame, "frame", 0),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, visible_rows,
                  "visible_rows", 8),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, geometry_rows,
                  "geometry_rows", 16),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, instances, "instances",
                  24),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, materials, "materials",
                  32),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, transmission_materials,
                  "transmission_materials", 40),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, compaction_state,
                  "compaction_state", 48),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, pixel_list, "pixel_list",
                  56),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, compact_counts,
                  "compact_counts", 64),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, vbuffer_texture_id,
                  "vbuffer", 72),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, depth_texture_id,
                  "depth", 80),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, source_texture_id,
                  "source", 88),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, destination_texture_id,
                  "destination", 96),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, view_projection,
                  "view_projection", 112),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, inverse_view_projection,
                  "inverse_view_projection", 176),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, extent, "extent", 240),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, visible_capacity,
                  "visible_capacity", 248),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, geometry_count,
                  "geometry_count", 252),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, material_count,
                  "material_count", 256),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, instance_count,
                  "instance_count", 260),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, pixel_capacity,
                  "pixel_capacity", 264),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, compact_layer,
                  "compact_layer", 268),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, compact_enabled,
                  "compact_enabled", 272),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, opaque_mip_count,
                  "opaque_mip_count", 276),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, opaque_texture_id,
                  "opaque_pyramid", 280),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, motion_texture_id,
                  "motion", 288),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, validity_texture_id,
                  "validity", 296),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, previous_transforms,
                  "previous_transforms", 304),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot,
                  previous_transform_address_padding,
                  "previous_transform_address_padding", 312),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, current_view_projection,
                  "current_view_projection", 320),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, previous_view_projection,
                  "previous_view_projection", 384),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, history_valid,
                  "history_valid", 448),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, previous_frame_index,
                  "previous_frame_index", 452),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, temporal_outputs_enabled,
                  "temporal_outputs_enabled", 456),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, temporal_reserved,
                  "temporal_reserved", 460),
};

vkr_global const VkrMetalPacketAbiField
    vkr_transmission_coverage_root_fields[] = {
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCoverageRoot,
                      vbuffer_texture_id, "vbuffer", 0),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCoverageRoot, covered_pixels,
                      "covered_pixels", 8),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCoverageRoot, extent, "extent",
                      16),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCoverageRoot, layer, "layer",
                      24),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCoverageRoot, reserved,
                      "reserved", 28),
};

vkr_global const VkrMetalPacketAbiField
    vkr_transmission_compact_root_fields[] = {
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, vbuffer_texture_id,
                      "vbuffer", 0),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, pixel_list,
                      "pixel_list", 8),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, covered_pixels,
                      "covered_pixels", 16),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, overflow_counts,
                      "overflow_counts", 24),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, indirect_arguments,
                      "indirect_arguments", 32),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, visible_rows,
                      "visible_rows", 40),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, materials,
                      "materials", 48),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, source_texture_id,
                      "source", 56),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot,
                      destination_texture_id, "destination", 64),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, extent, "extent",
                      72),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, layer, "layer",
                      80),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, capacity,
                      "capacity", 84),
        VKR_ABI_FIELD(VkrMetalPacketTransmissionCompactRoot, reserved,
                      "reserved", 88),
};

vkr_global const VkrMetalPacketAbiField vkr_picking_resolve_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, opaque_visible_rows,
                  "opaque_visible_rows", 0),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, opaque_instances,
                  "opaque_instances", 8),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, opaque_compaction_state,
                  "opaque_compaction_state", 16),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, transmission_visible_rows,
                  "transmission_visible_rows", 24),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, transmission_instances,
                  "transmission_instances", 32),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot,
                  transmission_compaction_state,
                  "transmission_compaction_state", 40),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, opaque_vbuffer_texture_id,
                  "opaque_vbuffer", 48),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, opaque_depth_texture_id,
                  "opaque_depth", 56),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot,
                  transmission_vbuffer_texture_id, "transmission_vbuffer", 64),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot,
                  transmission_depth_texture_id, "transmission_depth", 72),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, destination_texture_id,
                  "destination", 80),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, pixel, "pixel", 88),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, extent, "extent", 96),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, opaque_visible_capacity,
                  "opaque_visible_capacity", 104),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, opaque_instance_count,
                  "opaque_instance_count", 108),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot,
                  transmission_visible_capacity,
                  "transmission_visible_capacity", 112),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, transmission_instance_count,
                  "transmission_instance_count", 116),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, transmission_enabled,
                  "transmission_enabled", 120),
    VKR_ABI_FIELD(VkrMetalPacketPickingResolveRoot, reserved, "reserved", 124),
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
                           176, 16, vkr_material_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_MATERIAL] =
            VKR_ABI_RECORD(VkrMetalTransmissionMaterialGpuRow,
                           "VkrMetalPacketTransmissionMaterial", 32, 16,
                           vkr_transmission_material_fields),
        [VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketVertexDrawRoot, "VkrMetalPacketDrawRoot", 48, 16,
            vkr_vertex_draw_root_fields),
        [VKR_METAL_PACKET_ABI_TEMPORAL_VERTEX_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketVertexDrawRoot, "VkrMetalPacketDrawRoot", 48, 16,
            vkr_vertex_draw_root_fields),
        [VKR_METAL_PACKET_ABI_DRAW_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketDrawRoot, "VkrMetalPacketDrawRoot", 48,
                           16, vkr_draw_root_fields),
        [VKR_METAL_PACKET_ABI_FRAME_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketFrameRoot, "VkrMetalPacketFrameRoot",
                           464, 16, vkr_frame_root_fields),
        [VKR_METAL_PACKET_ABI_IBL_PROBE] =
            VKR_ABI_RECORD(VkrMetalPacketIblProbe, "VkrMetalPacketIblProbe", 64,
                           16, vkr_ibl_probe_fields),
        [VKR_METAL_PACKET_ABI_SHADOW_CASCADE] = VKR_ABI_RECORD(
            VkrMetalPacketShadowCascade, "VkrMetalPacketShadowCascade", 96, 16,
            vkr_shadow_cascade_fields),
        [VKR_METAL_PACKET_ABI_TONEMAP_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketTonemapRoot, "VkrMetalPacketTonemapRoot", 32, 16,
            vkr_tonemap_root_fields),
        [VKR_METAL_PACKET_ABI_EQUIRECT_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketEquirectRoot, "VkrMetalPacketEquirectRoot", 32, 16,
            vkr_equirect_root_fields),
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
            VKR_ABI_RECORD(VkrMetalPacketUiRoot, "VkrMetalPacketUiRoot", 64, 16,
                           vkr_ui_root_fields),
        [VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketGpuDrawRoot, "VkrMetalPacketGpuDrawRoot", 192, 16,
            vkr_gpu_draw_root_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_PEEL_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTransmissionPeelRoot,
                           "VkrMetalPacketTransmissionPeelRoot", 16, 16,
                           vkr_transmission_peel_root_fields),
        [VKR_METAL_PACKET_ABI_TEMPORAL_TRANSFORM_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTemporalTransformRoot,
                           "VkrMetalPacketTemporalTransformRoot", 32, 16,
                           vkr_temporal_transform_root_fields),
        [VKR_METAL_PACKET_ABI_GBUFFER_RESOLVE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketGBufferResolveRoot,
                           "VkrMetalPacketGBufferResolveRoot", 416, 16,
                           vkr_gbuffer_resolve_root_fields),
        [VKR_METAL_PACKET_ABI_GTAO_PARAMS] = VKR_ABI_RECORD(
            VkrGtaoGpuParams, "VkrGtaoParams", 192, 16, vkr_gtao_params_fields),
        [VKR_METAL_PACKET_ABI_GTAO_DEPTH_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketGtaoDepthRoot, "VkrMetalPacketGtaoDepthRoot", 224, 16,
            vkr_gtao_depth_root_fields),
        [VKR_METAL_PACKET_ABI_GTAO_EVALUATE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketGtaoEvaluateRoot, "VkrMetalPacketGtaoEvaluateRoot",
            256, 16, vkr_gtao_evaluate_root_fields),
        [VKR_METAL_PACKET_ABI_GTAO_DENOISE_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketGtaoDenoiseRoot, "VkrMetalPacketGtaoDenoiseRoot", 240,
            16, vkr_gtao_denoise_root_fields),
        [VKR_METAL_PACKET_ABI_DEFERRED_LIGHTING_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketDeferredLightingRoot,
                           "VkrMetalPacketDeferredLightingRoot", 160, 16,
                           vkr_deferred_lighting_root_fields),
        [VKR_METAL_PACKET_ABI_TEMPORAL_RESOLVE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTemporalResolveRoot,
                           "VkrMetalPacketTemporalResolveRoot", 224, 16,
                           vkr_temporal_resolve_root_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_SHADE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTransmissionShadeRoot,
                           "VkrMetalPacketTransmissionShadeRoot", 464, 16,
                           vkr_transmission_shade_root_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_COVERAGE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTransmissionCoverageRoot,
                           "VkrMetalPacketTransmissionCoverageRoot", 32, 16,
                           vkr_transmission_coverage_root_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_COMPACT_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTransmissionCompactRoot,
                           "VkrMetalPacketTransmissionCompactRoot", 96, 16,
                           vkr_transmission_compact_root_fields),
        [VKR_METAL_PACKET_ABI_PICKING_RESOLVE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketPickingResolveRoot,
                           "VkrMetalPacketPickingResolveRoot", 128, 16,
                           vkr_picking_resolve_root_fields),
        [VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketHzbBuildRoot, "VkrMetalPacketHzbBuildRoot", 48, 16,
            vkr_hzb_build_root_fields),
        [VKR_METAL_PACKET_ABI_SDSM_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketSdsmRoot, "VkrMetalPacketSdsmRoot", 32,
                           16, vkr_sdsm_root_fields),
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
                 VKR_METAL_PACKET_DRAW_ROOT_STRIDE %
                         VKR_METAL_PACKET_ROOT_ALIGNMENT ==
                     0
             ? true_v
             : false_v;
}

#undef VKR_ABI_RECORD
#undef VKR_ABI_FIELD

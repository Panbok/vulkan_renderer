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
    VKR_ABI_FIELD(VkrMetalMaterialGpuRow, reserved, "reserved", 92),
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
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, irradiance_texture_id, "irradiance",
                  88),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, prefilter_texture_id, "prefilter",
                  96),
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
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, shadow_bias, "shadow_bias", 388),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, transmission_texture_id,
                  "transmission_source", 400),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_probes, "ibl_probes", 408),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_probe_count, "ibl_probe_count",
                  416),
    VKR_ABI_FIELD(VkrMetalPacketFrameRoot, ibl_probe_reserved,
                  "ibl_probe_reserved", 420),
};

vkr_global const VkrMetalPacketAbiField vkr_ibl_probe_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, irradiance_texture_id, "irradiance",
                  0),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, prefilter_texture_id, "prefilter", 8),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, center_blend, "center_blend", 16),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, extents_weight, "extents_weight", 32),
    VKR_ABI_FIELD(VkrMetalPacketIblProbe, intensity_box, "intensity_box", 48),
};

vkr_global const VkrMetalPacketAbiField vkr_shadow_cascade_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, light_view_projection,
                  "light_view_projection", 0),
    VKR_ABI_FIELD(VkrMetalPacketShadowCascade, split_depth, "split_depth", 64),
};

vkr_global const VkrMetalPacketAbiField vkr_tonemap_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, exposure, "exposure", 8),
    VKR_ABI_FIELD(VkrMetalPacketTonemapRoot, reserved, "reserved", 16),
};

vkr_global const VkrMetalPacketAbiField vkr_skybox_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketSkyboxRoot, inverse_view_projection,
                  "inverse_view_projection", 0),
    VKR_ABI_FIELD(VkrMetalPacketSkyboxRoot, cubemap_texture_id, "cubemap", 64),
    VKR_ABI_FIELD(VkrMetalPacketSkyboxRoot, target_width, "target_size", 72),
};

vkr_global const VkrMetalPacketAbiField vkr_equirect_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, target_texture_id, "target", 8),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, target_size, "target_size", 16),
    VKR_ABI_FIELD(VkrMetalPacketEquirectRoot, reserved, "reserved_0", 20),
};

vkr_global const VkrMetalPacketAbiField vkr_irradiance_root_fields[] = {
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, source_texture_id, "source", 0),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, target_texture_id, "target", 8),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, sample_count, "sample_count",
                  16),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, target_size, "target_size", 20),
    VKR_ABI_FIELD(VkrMetalPacketIrradianceRoot, reserved, "reserved", 24),
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
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, extent, "extent", 160),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, visible_capacity,
                  "visible_capacity", 168),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, geometry_count,
                  "geometry_count", 172),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, material_count,
                  "material_count", 176),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, instance_count,
                  "instance_count", 180),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, render_mode, "render_mode",
                  184),
    VKR_ABI_FIELD(VkrMetalPacketGBufferResolveRoot, reserved, "reserved", 188),
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
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, inverse_view_projection,
                  "inverse_view_projection", 64),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, extent, "extent", 128),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, sky_enabled,
                  "sky_enabled", 136),
    VKR_ABI_FIELD(VkrMetalPacketDeferredLightingRoot, reserved, "reserved",
                  140),
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
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, compaction_state,
                  "compaction_state", 40),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, vbuffer_texture_id,
                  "vbuffer", 48),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, depth_texture_id,
                  "depth", 56),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, source_texture_id,
                  "source", 64),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, destination_texture_id,
                  "destination", 72),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, view_projection,
                  "view_projection", 80),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, inverse_view_projection,
                  "inverse_view_projection", 144),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, extent, "extent", 208),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, visible_capacity,
                  "visible_capacity", 216),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, geometry_count,
                  "geometry_count", 220),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, material_count,
                  "material_count", 224),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, instance_count,
                  "instance_count", 228),
    VKR_ABI_FIELD(VkrMetalPacketTransmissionShadeRoot, reserved, "reserved",
                  232),
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
        [VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketVertexDrawRoot, "VkrMetalPacketDrawRoot", 48, 16,
            vkr_vertex_draw_root_fields),
        [VKR_METAL_PACKET_ABI_DRAW_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketDrawRoot, "VkrMetalPacketDrawRoot", 48,
                           16, vkr_draw_root_fields),
        [VKR_METAL_PACKET_ABI_FRAME_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketFrameRoot, "VkrMetalPacketFrameRoot",
                           432, 16, vkr_frame_root_fields),
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
        [VKR_METAL_PACKET_ABI_TEXT_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTextRoot, "VkrMetalPacketTextRoot",
                           176, 16, vkr_text_root_fields),
        [VKR_METAL_PACKET_ABI_GPU_DRAW_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketGpuDrawRoot, "VkrMetalPacketGpuDrawRoot", 192, 16,
            vkr_gpu_draw_root_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_PEEL_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTransmissionPeelRoot,
                           "VkrMetalPacketTransmissionPeelRoot", 16, 16,
                           vkr_transmission_peel_root_fields),
        [VKR_METAL_PACKET_ABI_GBUFFER_RESOLVE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketGBufferResolveRoot,
                           "VkrMetalPacketGBufferResolveRoot", 192, 16,
                           vkr_gbuffer_resolve_root_fields),
        [VKR_METAL_PACKET_ABI_DEFERRED_LIGHTING_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketDeferredLightingRoot,
                           "VkrMetalPacketDeferredLightingRoot", 144, 16,
                           vkr_deferred_lighting_root_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_SHADE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTransmissionShadeRoot,
                           "VkrMetalPacketTransmissionShadeRoot", 240, 16,
                           vkr_transmission_shade_root_fields),
        [VKR_METAL_PACKET_ABI_TRANSMISSION_COVERAGE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketTransmissionCoverageRoot,
                           "VkrMetalPacketTransmissionCoverageRoot", 32, 16,
                           vkr_transmission_coverage_root_fields),
        [VKR_METAL_PACKET_ABI_PICKING_RESOLVE_ROOT] =
            VKR_ABI_RECORD(VkrMetalPacketPickingResolveRoot,
                           "VkrMetalPacketPickingResolveRoot", 128, 16,
                           vkr_picking_resolve_root_fields),
        [VKR_METAL_PACKET_ABI_HZB_BUILD_ROOT] = VKR_ABI_RECORD(
            VkrMetalPacketHzbBuildRoot, "VkrMetalPacketHzbBuildRoot", 48, 16,
            vkr_hzb_build_root_fields),
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

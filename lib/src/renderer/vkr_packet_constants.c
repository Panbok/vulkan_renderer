#include "renderer/vkr_packet_constants.h"

#include "renderer/vkr_ibl_math.h"

/**
 * Shadow depth bias applied by every backend. A single literal so the two
 * cannot drift; the value is empirical and belongs with the lowering rather
 * than in either backend's draw loop.
 */
vkr_global const float32_t vkr_packet_shadow_bias = 0.0001f;

VkrPacketFrameConstants
vkr_packet_derive_frame_constants(const VkrRenderPacket *packet,
                                  uint32_t target_width,
                                  uint32_t target_height) {
  VkrPacketFrameConstants constants = {0};
  if (!packet)
    return constants;

  const VkrFrameLighting *lighting = packet->lighting;
  const float32_t inverse_width =
      1.0f / (float32_t)(target_width ? target_width : 1u);
  const float32_t inverse_height =
      1.0f / (float32_t)(target_height ? target_height : 1u);

  constants.view_position =
      (Vec4){packet->globals.view_position.x, packet->globals.view_position.y,
             packet->globals.view_position.z, 1.0f};
  constants.ibl_controls =
      lighting
          ? (Vec4){lighting->ibl_intensity, lighting->ibl_diffuse_intensity,
                   lighting->ibl_specular_intensity, inverse_width}
          : (Vec4){1.0f, 1.0f, 1.0f, inverse_width};
  constants.directional_direction_enabled =
      lighting ? (Vec4){lighting->directional_direction.x,
                        lighting->directional_direction.y,
                        lighting->directional_direction.z,
                        lighting->directional_enabled ? 1.0f : 0.0f}
               : vec4_zero();
  constants.directional_color_intensity =
      lighting
          ? (Vec4){lighting->directional_color.x, lighting->directional_color.y,
                   lighting->directional_color.z,
                   lighting->directional_intensity}
          : vec4_zero();
  constants.ambient_color =
      (Vec4){packet->globals.ambient_color.x, packet->globals.ambient_color.y,
             packet->globals.ambient_color.z, inverse_height};

  /* The grid block stays zeroed unless finite lights actually populated it;
     an empty grid must not publish a cell size or dimensions. */
  if (lighting && lighting->point_light_count > 0) {
    const VkrPointLightGrid *grid = lighting->point_light_grid;
    constants.point_light_grid_origin_cell_size =
        (Vec4){grid->origin.x, grid->origin.y, grid->origin.z, grid->cell_size};
    constants.point_light_grid_dimensions_count[0] = grid->dimensions[0];
    constants.point_light_grid_dimensions_count[1] = grid->dimensions[1];
    constants.point_light_grid_dimensions_count[2] = grid->dimensions[2];
    constants.point_light_grid_dimensions_count[3] = grid->cell_count;
    constants.point_light_global_mask = grid->global_mask;
  }
  constants.point_light_count = lighting ? lighting->point_light_count : 0u;

  constants.render_mode = packet->globals.render_mode;
  constants.shadow_debug_mode =
      packet->debug ? packet->debug->shadow_debug_mode : 0u;
  constants.prefilter_mip_count = VKR_IBL_PREFILTER_MIP_COUNT;
  constants.shadow_cascade_count =
      packet->shadow ? packet->shadow->cascade_count : 0u;
  constants.shadow_bias = vkr_packet_shadow_bias;
  constants.view = packet->globals.view;
  return constants;
}

uint32_t vkr_packet_derive_frame_flags(const VkrRenderPacket *packet,
                                       bool8_t lighting_pass,
                                       bool8_t ibl_resources_ready,
                                       bool8_t transmission_pass) {
  uint32_t flags = 0u;
  if (lighting_pass)
    flags |= VKR_PACKET_FRAME_FLAG_LIGHTING;
  if (lighting_pass && ibl_resources_ready && packet && packet->lighting &&
      packet->lighting->ibl_enabled)
    flags |= VKR_PACKET_FRAME_FLAG_IBL;
  if (transmission_pass)
    flags |= VKR_PACKET_FRAME_FLAG_TRANSMISSION;
  return flags;
}

VkrPacketMaterialConstants
vkr_packet_derive_material_constants(const VkrPbrProperties *pbr,
                                     float32_t alpha_cutoff,
                                     VkrMaterialAlphaMode alpha_mode) {
  if (!pbr)
    return (VkrPacketMaterialConstants){0};
  return (VkrPacketMaterialConstants){
      .emissive = {pbr->emissive_factor.x, pbr->emissive_factor.y,
                   pbr->emissive_factor.z, 0.0f},
      .dielectric_specular = {pbr->dielectric_specular.x,
                              pbr->dielectric_specular.y,
                              pbr->dielectric_specular.z, 0.0f},
      .surface = {pbr->metallic, pbr->roughness, pbr->normal_scale,
                  pbr->occlusion_strength},
      .alpha = {alpha_cutoff, pbr->transmission_factor, pbr->ior,
                pbr->thickness_factor},
      .attenuation_color = {pbr->attenuation_color.x, pbr->attenuation_color.y,
                            pbr->attenuation_color.z,
                            pbr->attenuation_distance},
      .alpha_mode = (uint32_t)alpha_mode,
  };
}

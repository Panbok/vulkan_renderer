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
  constants.prefilter_mip_count = VKR_IBL_PREFILTER_MIP_COUNT;
  constants.shadow_cascade_count =
      packet->shadow ? packet->shadow->cascade_count : 0u;
  constants.shadow_bias = vkr_packet_shadow_bias;
  constants.view = packet->globals.view;
  return constants;
}

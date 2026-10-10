#include "vkr_packet_constants.h"

#include "vkr_ibl_math.h"
#include "vkr_render_graph.h"

VkrPacketFrameConstants
vkr_packet_derive_frame_constants(const VkrPreparedFrame *packet,
                                  uint32_t target_width,
                                  uint32_t target_height) {
  VkrPacketFrameConstants constants = {0};
  if (!packet)
    return constants;

  const VkrFrameLighting *lighting = packet->input.lighting;
  /* Every radiometric input is lowered pre-exposed (VkrExposureFrame); the
     packet keeps physical values, which history signatures hash. */
  const float32_t pre_exposure = packet->exposure.pre_exposure;
  const float32_t inverse_width =
      1.0f / (float32_t)(target_width ? target_width : 1u);
  const float32_t inverse_height =
      1.0f / (float32_t)(target_height ? target_height : 1u);

  constants.view_position = (Vec4){packet->input.globals.view_position.x,
                                   packet->input.globals.view_position.y,
                                   packet->input.globals.view_position.z, 1.0f};
  constants.ibl_controls =
      lighting ? (Vec4){lighting->ibl_intensity * pre_exposure,
                        lighting->ibl_diffuse_intensity,
                        lighting->ibl_specular_intensity, inverse_width}
               : (Vec4){pre_exposure, 1.0f, 1.0f, inverse_width};
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
                   lighting->directional_intensity * pre_exposure}
          : vec4_zero();
  constants.ambient_color = (Vec4){
      packet->input.globals.ambient_color.x * pre_exposure,
      packet->input.globals.ambient_color.y * pre_exposure,
      packet->input.globals.ambient_color.z * pre_exposure, inverse_height};

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

  constants.render_mode = packet->input.globals.render_mode;
  constants.shadow_debug_mode =
      packet->input.debug ? packet->input.debug->shadow_debug_mode : 0u;
  constants.prefilter_mip_count = VKR_IBL_PREFILTER_MIP_COUNT;
  constants.pre_exposure = pre_exposure;
  constants.shadow_cascade_count =
      packet->input.shadow ? packet->input.shadow->cascade_count : 0u;
  /* Zeroed without a shadow payload: a zero tap count is the receiver's own
     "no cascades" signal, so no separate enable bit is needed. */
  if (packet->input.shadow)
    constants.shadow_receiver = packet->input.shadow->receiver;
  constants.view = packet->input.globals.view;
  return constants;
}

void vkr_packet_pre_expose_text_vertices(VkrTextVertex *vertices,
                                         uint32_t vertex_count,
                                         float32_t pre_exposure) {
  if (pre_exposure == 1.0f)
    return;
  for (uint32_t i = 0u; i < vertex_count; ++i) {
    vertices[i].color.x *= pre_exposure;
    vertices[i].color.y *= pre_exposure;
    vertices[i].color.z *= pre_exposure;
  }
}

uint32_t vkr_packet_derive_frame_flags(const VkrPreparedFrame *packet,
                                       bool8_t lighting_pass,
                                       bool8_t ibl_resources_ready) {
  uint32_t flags = 0u;
  if (lighting_pass)
    flags |= VKR_PACKET_FRAME_FLAG_LIGHTING;
  if (lighting_pass && ibl_resources_ready && packet &&
      packet->input.lighting && packet->input.lighting->ibl_enabled)
    flags |= VKR_PACKET_FRAME_FLAG_IBL;
  return flags;
}

Vec4 vkr_packet_terrain_layer_surface(const VkrMaterialLayer *layer) {
  return vec4_new(layer->metallic, layer->roughness, layer->normal_scale,
                  layer->occlusion_strength);
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

/* LOD policy of each culling view in their order: the camera, cascades, then
   opaque and transmitting local faces (ADR-084). */
void vkr_packet_write_lod_views(const VkrPreparedFrame *packet,
                                const VkrRenderGraphFrameInfo *frame,
                                VkrGpuLodView *out_views, uint32_t view_count) {
  out_views[0] = vkr_gpu_lod_view(
      packet->temporal.current_view_projection,
      (float32_t)frame->scene_output_height * frame->render_scale,
      packet->input.globals.view_position,
      packet->temporal.previous_view_position, true_v);
  const uint32_t cascade_count = frame->shadow_cascade_count;
  for (uint32_t view = 1u; view < view_count; ++view) {
    if (view <= cascade_count) {
      out_views[view] = vkr_gpu_lod_view(
          packet->input.shadow->cascades[view - 1u].light_view_projection,
          (float32_t)frame->shadow_map_size, vec3_zero(), vec3_zero(), false_v);
      continue;
    }
    const uint32_t slot =
        (view - 1u - cascade_count) % Max(frame->local_shadow_render_count, 1u);
    const VkrLocalShadowView *face =
        &packet->input.local_shadow
             ->views[packet->input.local_shadow->render_views[slot]];
    const Vec3 light =
        vec3_new(face->light_position_near.x, face->light_position_near.y,
                 face->light_position_near.z);
    /* projection_params.y is one face texel; faces differ in size. */
    const float32_t face_size = face->projection_params.y > 0.0f
                                    ? 1.0f / face->projection_params.y
                                    : (float32_t)frame->local_shadow_map_size;
    out_views[view] = vkr_gpu_lod_view(face->light_view_projection, face_size,
                                       light, light, false_v);
    /* An opaque face view draws the casters its square holds: the static
       ones into a static square, the dynamic ones over a copied one.
       Transmission views draw every refractive caster. */
    const VkrLocalShadowPassPayload *local = packet->input.local_shadow;
    if (view - 1u - cascade_count >= frame->local_shadow_render_count)
      continue;
    if (slot >= vkr_local_shadow_dynamic_render_first(local))
      out_views[view].flags |= VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY;
    else if ((local->static_render_mask & (UINT64_C(1) << slot)) != 0u)
      out_views[view].flags |= VKR_GPU_LOD_VIEW_STATIC_CASTERS_ONLY;
  }
}

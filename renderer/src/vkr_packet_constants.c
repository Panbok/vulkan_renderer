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
      .brdf = {pbr->diffuse_roughness, pbr->smooth_terminator,
               pbr->smooth_terminator_length, pbr->dual_specular_opacity},
      .specular_tint = {pbr->specular_f82_tint.x, pbr->specular_f82_tint.y,
                        pbr->specular_f82_tint.z,
                        pbr->dual_specular_roughness_scale},
      .alpha_mode = (uint32_t)alpha_mode,
  };
}

Vec4 vkr_packet_material_detail(const VkrMaterial *material) {
  if (!material || !vkr_material_detail_active(material))
    return vec4_zero();
  const VkrPbrProperties *pbr = &material->pbr;
  /* Without an albedo map the overlay's weight is zero, so the detail
     pipelines skip its sample and the slot's stand-in never shows. */
  const float32_t albedo =
      material->textures[VKR_TEXTURE_SLOT_DETAIL_ALBEDO].enabled
          ? pbr->detail_albedo_strength
          : 0.0f;
  return vec4_new(
      pbr->detail_uv_scale, pbr->detail_normal_scale, albedo,
      pbr->detail_mask == VKR_MATERIAL_DETAIL_MASK_BASE_ALPHA ? 1.0f : 0.0f);
}

/* LOD policy of each culling view in their order: the camera, cascades,
   the cascades' dynamic casters, then opaque and transmitting local faces
   (ADR-084). */
void vkr_packet_write_lod_views(const VkrPreparedFrame *packet,
                                const VkrRenderGraphFrameInfo *frame,
                                VkrGpuLodView *out_views, uint32_t view_count) {
  /* A frozen culling camera selects levels and measures distances from
     where it stood, with no morph motion (ADR-127). */
  const VkrCullCamera *frozen =
      packet->input.world ? packet->input.world->frozen_cull : NULL;
  out_views[0] =
      frozen ? vkr_gpu_lod_view(
                   mat4_mul(frozen->projection, frozen->view),
                   (float32_t)frame->scene_output_height * frame->render_scale,
                   frozen->view_position, frozen->view_position, true_v)
             : vkr_gpu_lod_view(
                   packet->temporal.current_view_projection,
                   (float32_t)frame->scene_output_height * frame->render_scale,
                   packet->input.globals.view_position,
                   packet->temporal.previous_view_position, true_v);
  /* The camera alone culls by draw distance and screen size. */
  out_views[0].distance_scale =
      packet->input.world ? packet->input.world->draw_distance_scale : 0.0f;
  const uint32_t local_first = vkr_render_graph_local_shadow_first_view(frame);
  const uint32_t reflection_view = frame->planar_reflection_active
                                       ? vkr_render_graph_reflection_view(frame)
                                       : UINT32_MAX;
  const uint32_t motion_view = frame->tiled_temporal_enabled
                                   ? vkr_render_graph_motion_view(frame)
                                   : UINT32_MAX;
  const uint32_t late_view = frame->tiled_two_phase_occlusion
                                 ? vkr_render_graph_late_view(frame)
                                 : UINT32_MAX;
  for (uint32_t view = 1u; view < view_count; ++view) {
    /* The late occlusion view classifies the camera's deferred draws, with
       the camera's levels and culling (ADR-032). */
    if (view == late_view) {
      out_views[view] = out_views[0];
      continue;
    }
    /* The motion view takes the camera's levels and culling over the
       dynamic draws only, so it redraws the surfaces the camera drew. */
    if (view == motion_view) {
      out_views[view] = out_views[0];
      out_views[view].flags |= VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY;
      continue;
    }
    /* The mirrored view selects levels for its own image rows from the
       mirrored eye, and culls by the camera's draw distances. */
    if (view == reflection_view) {
      const VkrPlanarReflectionPayload *reflection =
          packet->input.planar_reflection;
      out_views[view] = vkr_gpu_lod_view(
          mat4_mul(packet->input.globals.projection, reflection->view),
          (float32_t)frame->viewport_height /
              (float32_t)frame->planar_reflection_divisor,
          reflection->view_position, reflection->view_position, false_v);
      out_views[view].distance_scale = out_views[0].distance_scale;
      continue;
    }
    if (view < local_first) {
      const uint32_t cascade = vkr_render_graph_view_cascade(frame, view);
      out_views[view] = vkr_gpu_lod_view(
          packet->input.shadow->cascades[cascade].light_view_projection,
          (float32_t)frame->shadow_map_size, vec3_zero(), vec3_zero(), false_v);
      /* Casters whose shadow distance ends before the slice skip it, and
         sealed casters every slice; a cascade's start is never zero. */
      out_views[view].shadow_near = Max(
          packet->input.shadow->cascades[cascade].split_near_far_texel_depth.x,
          VKR_GPU_LOD_SHADOW_NEAR_MIN);
      /* A composed cascade draws its static casters into its static layer
         and its dynamic casters over the copy of that layer; a far slice
         draws static casters into the rebuilt layer. Levels and the size
         cut follow the far cascade's whole map, not the slice's matrix. */
      if (view > frame->shadow_cascade_count)
        out_views[view].flags |= VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY;
      else if ((frame->shadow_cascade_static_render_mask &
                (UINT32_C(1) << cascade)) != 0u ||
               (frame->shadow_far_slice_active &&
                cascade == frame->shadow_far_cascade))
        out_views[view].flags |= VKR_GPU_LOD_VIEW_STATIC_CASTERS_ONLY;
      continue;
    }
    const uint32_t slot =
        (view - local_first) % Max(frame->local_shadow_render_count, 1u);
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
    if (view - local_first >= frame->local_shadow_render_count)
      continue;
    if (slot >= vkr_local_shadow_dynamic_render_first(local))
      out_views[view].flags |= VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY;
    else if ((local->static_render_mask & (UINT64_C(1) << slot)) != 0u)
      out_views[view].flags |= VKR_GPU_LOD_VIEW_STATIC_CASTERS_ONLY;
  }
}

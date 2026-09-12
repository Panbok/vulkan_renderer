static float4 vkr_metal_packet_shade(
    VkrMetalPacketVertexOutput input,
    constant VkrMetalPacketDrawRoot *root,
    bool front_facing) {
  constant VkrMetalPacketFrameRoot *frame = root->frame;
  const device VkrGpuVisibleDrawRow &visible =
      root->visible_rows[input.visible_row_index];
  const device VkrMetalPacketMaterial &material =
      frame->materials[visible.material_index];
  float4 base = material.base_color_texture.sample(material.base_color_sampler,
                                                   input.texcoord) *
                material.tint * input.color;
  if (material.alpha_mode == 1u && base.a < material.material_alpha.x)
    discard_fragment();
  if ((frame->flags & 1u) == 0u)
    return base;

  float face_sign = front_facing ? 1.0 : -1.0;
  float3 geometric_normal = normalize(input.world_normal) * face_sign;
  float3 normal = geometric_normal;
  if ((material.flags & 1u) != 0u) {
    float3 sampled = vkr_normal_map_decode(
        material.normal_texture.sample(material.normal_sampler, input.texcoord)
            .xyz,
        material.material_surface.z);
    float3 tangent = normalize(input.world_tangent.xyz);
    tangent = normalize(tangent - dot(tangent, normal) * normal);
    float3 bitangent =
        normalize(cross(normal, tangent)) * input.world_tangent.w;
    normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                       normal * sampled.z);
  }
  if (frame->render_mode == 2u)
    return float4(normal * 0.5 + 0.5, 1.0);
  float3 view = normalize(frame->view_position.xyz - input.world_position);
  float no_v = max(dot(normal, view), 0.0);
  float metallic = saturate(material.material_surface.x);
  float roughness = clamp(material.material_surface.y, 0.04, 1.0);
  float ao = saturate(material.material_surface.w);
  float3 emissive = material.material_emissive.rgb;
  if ((material.flags & 2u) != 0u) {
    float3 orm =
        material.orm_texture.sample(material.orm_sampler, input.texcoord).rgb;
    ao *= orm.r;
    roughness = clamp(roughness * orm.g, 0.04, 1.0);
    metallic = saturate(metallic * orm.b);
  }
  float3 normal_dx = dfdx(normal);
  float3 normal_dy = dfdy(normal);
  float variance =
      0.25 * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
  roughness = vkr_ggx_filter_roughness(roughness, variance);
  if ((material.flags & 4u) != 0u) {
    emissive *= material.emissive_texture
                    .sample(material.emissive_sampler, input.texcoord)
                    .rgb;
  }
  if (frame->render_mode == 3u)
    return float4(base.rgb + emissive,
                  material.alpha_mode == 0u ? 1.0 : base.a);
  float3 f0 = mix(saturate(material.material_dielectric_specular.rgb), base.rgb,
                  metallic);
  if (frame->render_mode == 6u)
    return float4(metallic, roughness, max(f0.x, max(f0.y, f0.z)), 1.0);
  if (frame->shadow_debug_mode != 0u) {
    VkrMetalPacketShadowSample shadow_sample =
        vkr_metal_packet_directional_shadow_sample(frame, input.world_position,
                                                   normal);
    return float4(vkr_metal_packet_shadow_debug_color(frame->shadow_debug_mode,
                                                      shadow_sample),
                  1.0);
  }
  float anisotropy_strength = material.material_anisotropy.x;
  float3 anisotropy_axis = float3(0.0f);
  if (anisotropy_strength > 0.0f) {
    float3 map = float3(1.0f, 0.5f, 1.0f);
    if ((material.flags & 1024u) != 0u)
      map = material.anisotropy_texture.sample(material.anisotropy_sampler, input.texcoord).rgb;
    anisotropy_strength *= map.b;
    anisotropy_axis = vkr_anisotropy_axis(geometric_normal, normal,
        input.world_tangent, map.rg * 2.0f - 1.0f, material.material_anisotropy.yz);
  }
  VkrGgxMaterialEnergy energy =
      vkr_metal_packet_prepare_brdf(frame, no_v, roughness, f0);
  if (anisotropy_strength > 0.0f)
    energy = vkr_metal_prepare_anisotropy(frame, normal, anisotropy_axis, view, roughness, anisotropy_strength, f0);
    energy = vkr_ggx_diffuse_transmission(energy, material.material_diffuse_transmission);
  float clearcoat_factor = saturate(material.material_clearcoat.x);
  if (clearcoat_factor > 0.0 && (material.flags & 32u) != 0u)
    clearcoat_factor *= material.clearcoat_texture
                            .sample(material.clearcoat_sampler, input.texcoord)
                            .r;
  bool clearcoat_active = vkr_clearcoat_active(clearcoat_factor);
  VkrClearcoatLayer clearcoat;
  if (clearcoat_active) {
    float clearcoat_roughness = material.material_clearcoat.y;
    if ((material.flags & 64u) != 0u)
      clearcoat_roughness *= material.clearcoat_roughness_texture
                                 .sample(material.clearcoat_roughness_sampler,
                                         input.texcoord)
                                 .g;
    float3 clearcoat_normal = geometric_normal;
    if ((material.flags & 128u) != 0u) {
      float3 sampled = vkr_normal_map_decode(
          material.clearcoat_normal_texture
              .sample(material.clearcoat_normal_sampler, input.texcoord)
              .xyz,
          material.material_clearcoat.z);
      float3 tangent = normalize(input.world_tangent.xyz);
      tangent = normalize(tangent - dot(tangent, geometric_normal) *
                                      geometric_normal);
      float3 bitangent = normalize(cross(geometric_normal, tangent)) *
                         input.world_tangent.w;
      clearcoat_normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                                   geometric_normal * sampled.z);
    }
    clearcoat = vkr_metal_packet_prepare_clearcoat(
        frame, clearcoat_factor, clearcoat_roughness, clearcoat_normal, view);
  }

  float3 sheen_color = max(material.material_sheen.rgb, float3(0.0f));
  if (vkr_sheen_active(sheen_color) && (material.flags & 256u) != 0u)
    sheen_color *= material.sheen_color_texture
                       .sample(material.sheen_color_sampler, input.texcoord).rgb;
  bool sheen_active = vkr_sheen_active(sheen_color);
  float sheen_roughness = 0.0f;
  VkrSheenLayer sheen;
  float sheen_normalization = 0.0f;
  if (sheen_active) {
    sheen_roughness = material.material_sheen.w;
    if ((material.flags & 512u) != 0u)
      sheen_roughness *= material.sheen_roughness_texture
                             .sample(material.sheen_roughness_sampler,
                                     input.texcoord).a;
    sheen = vkr_metal_packet_prepare_sheen(frame, sheen_color, sheen_roughness,
                                            normal, view);
    sheen_normalization = vkr_metal_packet_sheen_ltc_sample(
        frame, no_v, sheen.roughness).energy_normalization;
  }
  float3 analytic_diffuse = 0.0;
  float3 analytic_specular = 0.0;
  float3 clearcoat_direct = 0.0;
  float3 sheen_direct = 0.0;
  if (frame->directional_direction_enabled.w > 0.5) {
    float3 sun_direction = normalize(-frame->directional_direction_enabled.xyz);
    bool back_lit = energy.diffuse_transmission_strength > 0.0f &&
                    dot(normal, sun_direction) < 0.0f;
    float base_shadow = vkr_metal_packet_directional_shadow_sample(
        frame, input.world_position, back_lit ? -normal : normal).factor;
    float layer_shadow = base_shadow;
    if (back_lit && clearcoat_active)
      layer_shadow = vkr_metal_packet_directional_shadow_sample(
          frame, input.world_position, normal).factor;
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
        normal, view, normalize(-frame->directional_direction_enabled.xyz),
        frame->directional_color_intensity.rgb *
            frame->directional_color_intensity.w * base_shadow,
        base.rgb, metallic, roughness, f0, energy);
    analytic_diffuse = direct.diffuse;
    analytic_specular = direct.specular;
    if (clearcoat_active)
      clearcoat_direct += vkr_metal_packet_clearcoat_direct(
          view, normalize(-frame->directional_direction_enabled.xyz),
          frame->directional_color_intensity.rgb *
              frame->directional_color_intensity.w * layer_shadow,
          clearcoat);
    if (sheen_active)
      sheen_direct += vkr_metal_packet_sheen_direct(
          normal, view, normalize(-frame->directional_direction_enabled.xyz),
          frame->directional_color_intensity.rgb *
              frame->directional_color_intensity.w * layer_shadow,
          sheen, sheen_normalization);
  }
  uint4 point_mask =
      vkr_metal_packet_point_light_mask(frame, input.world_position);
  uint point_count = min(frame->point_light_count, 128u);
  for (uint word = 0u; word < 4u; ++word) {
    /* SIMD-uniform light index; see vkr_metal_packet_deferred_lighting. */
    uint remaining = simd_or(point_mask[word]);
    while (remaining != 0u) {
      uint bit = ctz(remaining);
      remaining &= remaining - 1u;
      uint light_index = word * 32u + bit;
      if (light_index >= point_count)
        continue;
      const device VkrGpuPointLightRow &light =
          frame->point_light_data[light_index];
      float4 p0 = light.p0;
      float4 p1 = light.p1;
      float4 p2 = light.p2;
      float4 p3 = light.p3;
      if ((point_mask[word] & (1u << bit)) == 0u)
        continue;
      uint kind = uint(p2.w + 0.5);
      float3 to_light = p0.xyz - input.world_position;
      float distance_squared = dot(to_light, to_light);
      if (kind != 0u && p2.z > 0.0 && distance_squared >= p2.z * p2.z)
        continue;
      float distance = sqrt(distance_squared);
      float3 light_direction =
          distance > 1e-6 ? to_light / distance : float3(0.0);
      float attenuation = 0.0;
      if (kind == 0u) {
        attenuation = 1.0 / max(max(p0.w, 1.0) + p1.w * distance +
                                    p2.y * distance_squared,
                                1e-6);
      } else {
        float range_attenuation = 1.0;
        if (p2.z > 0.0) {
          float ratio = distance / p2.z;
          range_attenuation = saturate(1.0 - ratio * ratio * ratio * ratio);
          range_attenuation *= range_attenuation;
        }
        attenuation = range_attenuation / max(distance_squared, 1e-4);
        if (kind == 2u) {
          float cone = dot(-light_direction, normalize(p3.xyz));
          float cone_attenuation = smoothstep(p1.w, p0.w, cone);
          if (cone_attenuation <= 0.0)
            continue;
          attenuation *= cone_attenuation;
        }
      }
      bool back_lit = energy.diffuse_transmission_strength > 0.0f &&
                      dot(normal, light_direction) < 0.0f;
      float3 base_attenuation =
          attenuation * vkr_metal_packet_local_shadow_sample(
                            frame, uint(p3.w), kind, input.world_position,
                            back_lit ? -normal : normal);
      float3 layer_attenuation;
      if (back_lit && clearcoat_active)
        layer_attenuation = attenuation * vkr_metal_packet_local_shadow_sample(
                                              frame, uint(p3.w), kind,
                                              input.world_position, normal);
      else
        layer_attenuation = base_attenuation;
      VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
          normal, view, light_direction, p1.rgb * p2.x * base_attenuation, base.rgb,
          metallic, roughness, f0, energy);
      analytic_diffuse += direct.diffuse;
      analytic_specular += direct.specular;
      if (clearcoat_active)
        clearcoat_direct += vkr_metal_packet_clearcoat_direct(
            view, light_direction, p1.rgb * p2.x * layer_attenuation,
            clearcoat);
      if (sheen_active)
        sheen_direct += vkr_metal_packet_sheen_direct(
            normal, view, light_direction, p1.rgb * p2.x * layer_attenuation,
            sheen, sheen_normalization);
    }
  }
  VkrMetalPacketDirectResult rectangles =
      vkr_metal_packet_rectangle_lights<true>(
          frame, input.world_position, normal, view, base.rgb, metallic,
          roughness, f0, energy);
  analytic_diffuse += rectangles.diffuse;
  analytic_specular += rectangles.specular;
  if (clearcoat_active)
    clearcoat_direct += vkr_metal_packet_clearcoat_rectangle_lights(
        frame, input.world_position, view, clearcoat);
  if (sheen_active)
    sheen_direct += vkr_metal_packet_sheen_rectangle_lights(
        frame, input.world_position, normal, view, sheen);
  float3 clearcoat_indirect = 0.0;
  float3 sheen_indirect = 0.0;
  float3 indirect_specular = 0.0;
  float4 volume_response = vkr_metal_packet_diffuse_volume(frame, input.world_position, normal);
  float3 indirect_diffuse = 0.0f;

  if ((frame->flags & 2u) != 0u) {
    constexpr sampler environment_sampler(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::linear, mip_filter::linear);
    roughness = vkr_anisotropy_environment_roughness(roughness, energy);
    float3 reflection = reflect(-view, vkr_anisotropy_environment_normal(normal, view, energy));
    VkrShL2Evaluation sh_evaluation = vkr_sh_l2_prepare_evaluation(normal);
    float horizon = saturate(1.0 + dot(reflection, geometric_normal));
    float specular_visibility =
        horizon * horizon * vkr_metal_packet_specular_ao(ao, no_v, roughness);
    float3 diffuse = 0.0;
    float3 specular = 0.0;
    float local_weight_sum = 0.0;
    uint probe_count = min(frame->ibl_probe_count, 16u);
    if (frame->ibl_probes != nullptr) {
      for (uint i = 0u; i < probe_count; ++i) {
        local_weight_sum += vkr_metal_packet_probe_influence(
            frame->ibl_probes[i], input.world_position);
      }
      float weight_scale =
          local_weight_sum > 1.0 ? 1.0 / local_weight_sum : 1.0;
      for (uint i = 0u; i < probe_count; ++i) {
        const device VkrMetalPacketIblProbe &probe = frame->ibl_probes[i];
        float weight =
            vkr_metal_packet_probe_influence(probe, input.world_position) *
            weight_scale;
        if (weight <= 1e-6)
          continue;
        float3 probe_reflection =
            probe.intensity_box.w > 0.5
                ? vkr_metal_packet_box_project(
                      reflection, input.world_position, probe.center_blend.xyz,
                      max(probe.extents_weight.xyz, 0.0))
                : reflection;
        float3 probe_irradiance = volume_response.w != 0.0f ? float3(0.0f) : vkr_sh_l2_evaluate(
            frame->sh_coefficients[probe.sh_slot], sh_evaluation);
        float3 probe_prefiltered =
            probe.prefilter
                .sample(environment_sampler, probe_reflection,
                        level(roughness *
                              float(max(frame->prefilter_mip_count, 1u) - 1u)))
                .rgb;
        diffuse += energy.diffuse_weight * (1.0 - metallic) * probe_irradiance *
                   base.rgb * ao * probe.intensity_box.x *
                   probe.intensity_box.y * weight;
        specular += probe_prefiltered * energy.reflectance *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
      }
    }
    float global_weight = max(1.0 - min(local_weight_sum, 1.0), 0.0);
    float3 global_irradiance = volume_response.w != 0.0f ? float3(0.0f) : vkr_sh_l2_evaluate(
        frame->sh_coefficients[frame->sh_global_slot], sh_evaluation);
    diffuse += energy.diffuse_weight * (1.0 - metallic) * global_irradiance *
               base.rgb * ao * global_weight;
    float3 global_prefiltered =
        frame->prefilter
            .sample(environment_sampler, reflection,
                    level(roughness *
                          float(max(frame->prefilter_mip_count, 1u) - 1u)))
            .rgb;
    specular += global_prefiltered * energy.reflectance * specular_visibility *
                global_weight;
    indirect_diffuse = diffuse * frame->ibl_controls.y * frame->ibl_controls.x;
    indirect_specular = specular * frame->ibl_controls.z * frame->ibl_controls.x;
    clearcoat_indirect = clearcoat_active
                              ? vkr_metal_packet_clearcoat_environment(
                                    frame, input.world_position, view, ao, 1.0,
                                    clearcoat)
                              : float3(0.0);
    sheen_indirect = sheen_active
                         ? vkr_metal_packet_sheen_environment(
                               frame, input.world_position, normal, view, ao,
                               1.0f, sheen) *
                               (clearcoat_active ? clearcoat.base_transmission : 1.0f)
                         : float3(0.0f);
  } else {
    indirect_diffuse = frame->ambient_color.rgb * energy.diffuse_weight *
             (1.0 - metallic) * base.rgb * ao;
  }
  if (volume_response.w != 0.0f)
    indirect_diffuse = volume_response.rgb * energy.diffuse_weight * (1.0 - metallic) * base.rgb * ao;
  if (sheen_active) {
    analytic_diffuse *= sheen.base_transmission;
    analytic_specular *= sheen.base_transmission;
    indirect_diffuse *= sheen.base_transmission;
    indirect_specular *= sheen.base_transmission;
    emissive *= sheen.base_transmission;
  }
  if (clearcoat_active) {
    analytic_diffuse *= clearcoat.base_transmission;
    analytic_specular *= clearcoat.base_transmission;
    indirect_diffuse *= clearcoat.base_transmission;
    indirect_specular *= clearcoat.base_transmission;
    emissive *= clearcoat.base_transmission;
    sheen_direct *= clearcoat.base_transmission;
  }
  float3 color = analytic_diffuse + analytic_specular + clearcoat_direct + sheen_direct +
                 indirect_diffuse + indirect_specular + clearcoat_indirect +
                 sheen_indirect;
  if (frame->render_mode == 1u)
    return float4(analytic_diffuse + analytic_specular + clearcoat_direct + sheen_direct, 1.0);
  if (frame->render_mode == 4u)
    return float4(analytic_diffuse, 1.0);
  if (frame->render_mode == 5u)
    return float4(analytic_specular + clearcoat_direct + sheen_direct, 1.0);
  color += emissive;
  return float4(color, material.alpha_mode == 0u ? 1.0 : base.a);
}

struct VkrMetalPacketTemporalBlendOutput {
  float4 color [[color(0)]];
  uint2 surface [[color(1)]];
  float2 motion [[color(2)]];
  float2 validity [[color(3)]];
};

fragment float4 vkr_metal_packet_opaque_fragment(
    VkrMetalPacketVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  return vkr_metal_packet_shade(input, root, front_facing);
}

fragment VkrMetalPacketTemporalBlendOutput
vkr_metal_packet_temporal_blend_fragment(
    VkrMetalPacketTemporalVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]],
    uint primitive_id [[primitive_id]]) {
  VkrMetalPacketTemporalBlendOutput output;
  VkrMetalPacketVertexOutput surface;
  surface.position = input.position;
  surface.texcoord = input.texcoord;
  surface.color = input.color;
  surface.object_id = input.object_id;
  surface.world_position = input.world_position;
  surface.world_normal = input.world_normal;
  surface.world_tangent = input.world_tangent;
  surface.visible_row_index = input.visible_row_index;
  output.color = vkr_metal_packet_shade(surface, root, front_facing);
  if (output.color.a <= 1e-4)
    discard_fragment();
  if (vkr_froxel_enabled(*root->frame->froxel_fog)) {
    VkrFroxelSample froxel =
        vkr_metal_packet_froxel_sample(root->frame, input.world_position);
    output.color.rgb = vkr_froxel_apply_integrated(output.color.rgb, froxel).inscatter;
  } else if (root->frame->fog->color_density.w > 0.0f) {
    output.color.rgb = vkr_fog_apply_surface(
        output.color.rgb, *root->frame->fog,
        root->frame->view_position.xyz, input.world_position);
  }

  constexpr uint overlay_bit = 0x80000000u;
  constexpr uint generation_mask = 0x7fffffffu;
  constexpr uint surface_mask = 0x1ffffu;
  uint surface_token = input.temporal_flags >> 1u;
  bool identity_valid =
      input.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY &&
      input.temporal_generation > 0u &&
      input.temporal_generation <= generation_mask &&
      surface_token > 0u && surface_token <= surface_mask;
  output.surface = uint2(overlay_bit, 0u);
  if (identity_valid) {
    output.surface.x = overlay_bit | input.temporal_generation;
    output.surface.y =
        (input.temporal_index << 17u) | surface_token;
  }
  output.motion = 0.0;
  output.validity = 0.0;

  constant VkrMetalPacketTemporalDrawState *temporal =
      root->frame->temporal_draw_state;
  if (identity_valid && temporal->history_valid != 0u) {
    const device VkrTemporalTransform &previous =
        temporal->previous_transforms[input.temporal_index];
    if (previous.valid != 0u &&
        previous.generation == input.temporal_generation &&
        previous.frame_index == temporal->previous_frame_index) {
      float4 current_clip =
          temporal->current_view_projection * float4(input.world_position, 1.0);
      float4 previous_clip =
          temporal->previous_view_projection *
          (previous.model * float4(input.object_position, 1.0));
      if (current_clip.w > 1e-6 && previous_clip.w > 1e-6) {
        float2 current_ndc = current_clip.xy / current_clip.w;
        float2 previous_ndc = previous_clip.xy / previous_clip.w;
        float2 current_uv =
            float2(current_ndc.x * 0.5 + 0.5, 0.5 - current_ndc.y * 0.5);
        float2 previous_uv =
            float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
        const device VkrGpuVisibleDrawRow &visible =
            root->visible_rows[input.visible_row_index];
        float reactivity =
            saturate(root->frame->materials[visible.material_index]
                         .temporal_reactivity);
        output.motion = previous_uv - current_uv;
        output.validity =
            float2(2.0 + reactivity, previous_clip.z / previous_clip.w);
      }
    }
  }
  return output;
}

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
  roughness = sqrt(saturate(roughness * roughness + min(variance, 0.25)));
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

  float3 analytic_diffuse = 0.0;
  float3 analytic_specular = 0.0;
  if (frame->directional_direction_enabled.w > 0.5) {
    VkrMetalPacketShadowSample shadow_sample =
        vkr_metal_packet_directional_shadow_sample(frame, input.world_position,
                                                   normal);
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
        normal, view, normalize(-frame->directional_direction_enabled.xyz),
        frame->directional_color_intensity.rgb *
            frame->directional_color_intensity.w * shadow_sample.factor,
        base.rgb, metallic, roughness, f0);
    analytic_diffuse = direct.diffuse;
    analytic_specular = direct.specular;
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
      VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
          normal, view, light_direction, p1.rgb * p2.x * attenuation, base.rgb,
          metallic, roughness, f0);
      analytic_diffuse += direct.diffuse;
      analytic_specular += direct.specular;
    }
  }
  float3 color = analytic_diffuse + analytic_specular;

  if ((frame->flags & 2u) != 0u) {
    constexpr sampler environment_sampler(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::linear, mip_filter::linear);
    float3 reflection = reflect(-view, normal);
    float3 fresnel = vkr_metal_packet_fresnel_roughness(no_v, f0, roughness);
    float3 kd = (1.0 - fresnel) * (1.0 - metallic);
    VkrShL2Evaluation sh_evaluation = vkr_sh_l2_prepare_evaluation(normal);
    float2 brdf = vkr_metal_packet_brdf_approximation(no_v, roughness);
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0);
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
        float3 probe_irradiance =
            vkr_sh_l2_evaluate(frame->sh_coefficients[probe.sh_slot],
                               sh_evaluation);
        float3 probe_prefiltered =
            probe.prefilter
                .sample(environment_sampler, probe_reflection,
                        level(roughness *
                              float(max(frame->prefilter_mip_count, 1u) - 1u)))
                .rgb;
        diffuse += kd * probe_irradiance * base.rgb * ao *
                   probe.intensity_box.x * probe.intensity_box.y * weight;
        specular += probe_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
      }
    }
    float global_weight = max(1.0 - min(local_weight_sum, 1.0), 0.0);
    float3 global_irradiance = vkr_sh_l2_evaluate(
        frame->sh_coefficients[frame->sh_global_slot], sh_evaluation);
    diffuse += kd * global_irradiance * base.rgb * ao * global_weight;
    float3 global_prefiltered =
        frame->prefilter
            .sample(environment_sampler, reflection,
                    level(roughness *
                          float(max(frame->prefilter_mip_count, 1u) - 1u)))
            .rgb;
    specular += global_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                specular_visibility * global_weight;
    color +=
        (diffuse * frame->ibl_controls.y + specular * frame->ibl_controls.z) *
        frame->ibl_controls.x;
  } else {
    color += frame->ambient_color.rgb * base.rgb * ao;
  }
  if (frame->render_mode == 1u)
    return float4(analytic_diffuse + analytic_specular, 1.0);
  if (frame->render_mode == 4u)
    return float4(analytic_diffuse, 1.0);
  if (frame->render_mode == 5u)
    return float4(analytic_specular, 1.0);
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

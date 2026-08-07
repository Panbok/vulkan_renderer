fragment float4 vkr_metal_packet_opaque_fragment(
    VkrMetalPacketVertexOutput input [[stage_in]],
    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],
    bool front_facing [[front_facing]]) {
  const device VkrMetalPacketMaterial &material =
      root->materials[root->material_index];
  constexpr sampler material_sampler(coord::normalized, address::clamp_to_edge,
                                     filter::linear);
  float4 base =
      material.base_color_texture.sample(material_sampler, input.texcoord) *
      material.tint * input.color;
  if (root->alpha_mode == 1u && base.a < root->material_alpha.x)
    discard_fragment();
  if ((root->flags & 1u) == 0u)
    return base;

  float face_sign = root->reserved != 0u && !front_facing ? -1.0 : 1.0;
  float3 geometric_normal = normalize(input.world_normal) * face_sign;
  float3 normal = geometric_normal;
  if ((material.flags & 1u) != 0u) {
    float3 sampled =
        material.normal_texture.sample(material_sampler, input.texcoord).xyz *
            2.0 -
        1.0;
    sampled.xy *= root->material_surface.z;
    if (root->reserved != 0u)
      sampled.y = -sampled.y;
    float3 tangent = normalize(input.world_tangent.xyz);
    tangent = normalize(tangent - dot(tangent, normal) * normal);
    float3 bitangent =
        normalize(cross(normal, tangent)) * input.world_tangent.w;
    normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                       normal * sampled.z);
  }
  if (root->render_mode == 2u)
    return float4(normal * 0.5 + 0.5, 1.0);
  float3 view = normalize(root->view_position.xyz - input.world_position);
  float no_v = max(dot(normal, view), 0.0);
  float metallic = saturate(root->material_surface.x);
  float roughness = clamp(root->material_surface.y, 0.04, 1.0);
  float ao = saturate(root->material_surface.w);
  float3 emissive = root->material_emissive.rgb;
  if ((material.flags & 2u) != 0u) {
    float3 orm =
        material.orm_texture.sample(material_sampler, input.texcoord).rgb;
    ao *= orm.r;
    roughness = clamp(roughness * orm.g, 0.04, 1.0);
    metallic = saturate(metallic * orm.b);
  }
  if (root->reserved != 0u) {
    float3 normal_dx = dfdx(normal);
    float3 normal_dy = dfdy(normal);
    float variance =
        0.25 * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
    roughness = sqrt(saturate(roughness * roughness + min(variance, 0.25)));
  }
  if ((material.flags & 4u) != 0u) {
    emissive *=
        material.emissive_texture.sample(material_sampler, input.texcoord).rgb;
  }
  if (root->render_mode == 3u)
    return float4(base.rgb + emissive, root->alpha_mode == 0u ? 1.0 : base.a);
  float3 f0 =
      mix(saturate(root->material_dielectric_specular.rgb), base.rgb, metallic);
  if (root->render_mode == 6u)
    return float4(metallic, roughness, max(f0.x, max(f0.y, f0.z)), 1.0);

  float3 analytic_diffuse = 0.0;
  float3 analytic_specular = 0.0;
  if (root->directional_direction_enabled.w > 0.5) {
    float shadow =
        vkr_metal_packet_directional_shadow(root, input.world_position);
    VkrMetalPacketDirectResult direct = vkr_metal_packet_direct(
        normal, view, normalize(-root->directional_direction_enabled.xyz),
        root->directional_color_intensity.rgb *
            root->directional_color_intensity.w * shadow,
        base.rgb, metallic, roughness, f0);
    analytic_diffuse = direct.diffuse;
    analytic_specular = direct.specular;
  }
  uint4 point_mask =
      vkr_metal_packet_point_light_mask(root, input.world_position);
  uint point_count = min(root->point_light_count, 128u);
  for (uint word = 0u; word < 4u; ++word) {
    uint remaining = point_mask[word];
    while (remaining != 0u) {
      uint light_index = word * 32u + ctz(remaining);
      remaining &= remaining - 1u;
      if (light_index >= point_count)
        continue;
      float4 p0 = root->point_light_data[light_index * 4u + 0u];
      float4 p1 = root->point_light_data[light_index * 4u + 1u];
      float4 p2 = root->point_light_data[light_index * 4u + 2u];
      float4 p3 = root->point_light_data[light_index * 4u + 3u];
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

  if ((root->material_flags & 1u) != 0u) {
    constexpr sampler environment_sampler(coord::normalized,
                                          address::clamp_to_edge,
                                          filter::linear, mip_filter::linear);
    float3 reflection = reflect(-view, normal);
    float3 fresnel = vkr_metal_packet_fresnel_roughness(no_v, f0, roughness);
    float3 kd = (1.0 - fresnel) * (1.0 - metallic);
    float3 global_irradiance =
        root->irradiance.sample(environment_sampler, normal).rgb;
    float3 global_prefiltered =
        root->prefilter
            .sample(environment_sampler, reflection,
                    level(roughness *
                          float(max(root->prefilter_mip_count, 1u) - 1u)))
            .rgb;
    float2 brdf =
        root->brdf_lut.sample(environment_sampler, float2(no_v, roughness)).rg;
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0);
    float horizon =
        saturate(1.0 + dot(reflection,
                           root->reserved != 0u ? geometric_normal : normal));
    float specular_visibility =
        horizon * horizon * vkr_metal_packet_specular_ao(ao, no_v, roughness);
    float3 diffuse = 0.0;
    float3 specular = 0.0;
    float local_weight_sum = 0.0;
    uint probe_count = min(root->ibl_probe_count, 16u);
    if (root->ibl_probes != nullptr) {
      for (uint i = 0u; i < probe_count; ++i) {
        local_weight_sum += vkr_metal_packet_probe_influence(
            root->ibl_probes[i], input.world_position);
      }
      float weight_scale =
          local_weight_sum > 1.0 ? 1.0 / local_weight_sum : 1.0;
      for (uint i = 0u; i < probe_count; ++i) {
        const device VkrMetalPacketIblProbe &probe = root->ibl_probes[i];
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
            probe.irradiance.sample(environment_sampler, normal).rgb;
        float3 probe_prefiltered =
            probe.prefilter
                .sample(environment_sampler, probe_reflection,
                        level(roughness *
                              float(max(root->prefilter_mip_count, 1u) - 1u)))
                .rgb;
        diffuse += kd * probe_irradiance * base.rgb * ao *
                   probe.intensity_box.x * probe.intensity_box.y * weight;
        specular += probe_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
      }
    }
    float global_weight = max(1.0 - min(local_weight_sum, 1.0), 0.0);
    diffuse += kd * global_irradiance * base.rgb * ao * global_weight;
    specular += global_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                specular_visibility * global_weight;
    color +=
        (diffuse * root->ibl_controls.y + specular * root->ibl_controls.z) *
        root->ibl_controls.x;
  } else {
    color += root->ambient_color.rgb * base.rgb * ao;
  }
  if (root->render_mode == 1u)
    return float4(analytic_diffuse + analytic_specular, 1.0);
  if (root->render_mode == 4u)
    return float4(analytic_diffuse, 1.0);
  if (root->render_mode == 5u)
    return float4(analytic_specular, 1.0);
  color += emissive;
  if (root->material_dielectric_specular.w > 0.5 &&
      root->material_alpha.y > 0.0) {
    float transmission = saturate(root->material_alpha.y);
    float thickness = max(root->material_alpha.w, 0.0);
    float3 view_normal = normalize((root->view * float4(normal, 0.0)).xyz);
    float3 refracted = refract(float3(0.0, 0.0, -1.0), view_normal,
                               1.0 / max(root->material_alpha.z, 1.0));
    float2 screen_uv =
        input.position.xy * float2(root->ibl_controls.w, root->ambient_color.w);
    float2 offset =
        refracted.xy / max(abs(refracted.z), 0.25) * thickness * 0.02;
    constexpr sampler transmission_sampler(
        coord::normalized, address::clamp_to_edge, filter::linear);
    float3 transmitted =
        root->transmission_source
            .sample(transmission_sampler, clamp(screen_uv + offset, 0.0, 1.0))
            .rgb;
    if (root->material_attenuation_color.w > 1e-4 && thickness > 0.0) {
      transmitted *= pow(clamp(root->material_attenuation_color.rgb, 1e-4, 1.0),
                         thickness / root->material_attenuation_color.w);
    }
    float3 fresnel = vkr_metal_packet_fresnel(no_v, f0);
    float weight = transmission * (1.0 - metallic) *
                   (1.0 - max(fresnel.x, max(fresnel.y, fresnel.z)));
    color = mix(color, transmitted, saturate(weight));
  }
  return float4(color, root->alpha_mode == 0u ? 1.0 : base.a);
}

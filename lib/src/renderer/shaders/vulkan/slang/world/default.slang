[shader("vertex")]
VkrBindlessVertexOutput world_vertex(uint vertex_id : SV_VertexID,
                                     uint instance_id : SV_InstanceID)
{
    return packet_vertex_impl(vertex_id, instance_id,
                              g_push.root.frame.view_projection);
}
float4 packet_base_color(VkrBindlessVertexOutput input)
{
    VkrGpuVisibleDrawRow visible =
        g_push.root.visible_rows[g_push.root.visible_row_index];
    VkrBindlessMaterial material =
        g_push.root.frame.materials[visible.material_index];
    uint texture_index = NonUniformResourceIndex(material.base_color_texture);
    uint sampler_index = NonUniformResourceIndex(material.base_color_sampler);
    return g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv)
           * material.tint * input.color;
}

struct VkrBindlessDirectResult
{
    float3 diffuse;
    float3 specular;
};

float3 packet_fresnel(float cosine, float3 f0)
{
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    return f0 + (f90 - f0) * pow(max(1.0f - cosine, 0.0f), 5.0f);
}

float3 packet_fresnel_roughness(float cosine, float3 f0, float roughness)
{
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float3 rough_f90 = f90 * (1.0f - roughness);
    return f0 + (max(rough_f90, f0) - f0) *
                    pow(max(1.0f - cosine, 0.0f), 5.0f);
}

float packet_geometry_schlick(float no_x, float roughness)
{
    float r = roughness + 1.0f;
    float k = r * r * 0.125f;
    return no_x / max(no_x * (1.0f - k) + k, 1e-6f);
}

VkrBindlessDirectResult packet_direct(float3 normal, float3 view, float3 light,
                                      float3 radiance, float3 albedo,
                                      float metallic, float roughness,
                                      float3 f0)
{
    float3 half_vector = normalize(view + light);
    float no_l = max(dot(normal, light), 0.0f);
    float no_v = max(dot(normal, view), 0.0f);
    float3 fresnel = packet_fresnel(max(dot(half_vector, view), 0.0f), f0);
    float distribution = distribution_ggx(max(dot(normal, half_vector), 0.0f),
                                          roughness);
    float geometry = packet_geometry_schlick(no_v, roughness) *
                     packet_geometry_schlick(no_l, roughness);
    float3 specular = distribution * geometry * fresnel /
                      max(4.0f * no_v * no_l, 1e-6f);
    float3 kd = (1.0f - fresnel) * (1.0f - metallic);
    float3 scale = radiance * no_l;
    VkrBindlessDirectResult result;
    result.diffuse = kd * albedo / vkr_pi * scale;
    result.specular = specular * scale;
    return result;
}

float packet_specular_ao(float ao, float no_v, float roughness)
{
    return saturate(pow(no_v + ao, exp2(-16.0f * roughness - 1.0f)) -
                    1.0f + ao);
}

// Epic's split-sum fit avoids a per-frame dependency on a mutable BRDF LUT.
float2 packet_brdf_approximation(float no_v, float roughness)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * no_v)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

uint4 packet_point_light_mask(VkrBindlessPacketFrameRoot* root,
                              float3 world_position)
{
    uint4 result = root.point_light_global_mask;
    uint4 dimensions = root.point_light_grid_dimensions_count;
    float cell_size = root.point_light_grid_origin_cell_size.w;
    if (dimensions.w == 0u || cell_size <= 0.0f)
        return result;
    int3 cell = int3(floor((world_position -
                            root.point_light_grid_origin_cell_size.xyz) /
                           cell_size));
    if (any(cell < 0) || cell.x >= int(dimensions.x) ||
        cell.y >= int(dimensions.y) || cell.z >= int(dimensions.z))
        return result;
    uint index = uint(cell.x) + dimensions.x *
                 (uint(cell.y) + dimensions.y * uint(cell.z));
    return result | root.point_light_masks[index];
}

float packet_directional_shadow(VkrBindlessPacketFrameRoot* root,
                                float3 world_position)
{
    if (root.shadow_cascade_count == 0u)
        return 1.0f;
    float view_depth = abs(mul(root.view, float4(world_position, 1.0f)).z);
    uint cascade = 0u;
    for (uint i = 0u; i + 1u < root.shadow_cascade_count; ++i)
    {
        if (view_depth > root.shadow_cascades[i].split_depth.x)
            cascade = i + 1u;
    }
    float4 clip = mul(root.shadow_cascades[cascade].light_view_projection,
                      float4(world_position, 1.0f));
    if (abs(clip.w) <= 1e-6f)
        return 1.0f;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || projected.z < 0.0f ||
        projected.z > 1.0f)
        return 1.0f;
    uint texture_index = NonUniformResourceIndex(root.shadow_texture);
    uint sampler_index = NonUniformResourceIndex(root.shadow_sampler);
    float shadow_depth = g_array_textures[texture_index].SampleLevel(
        g_samplers[sampler_index], float3(uv, float(cascade)), 0.0f).r;
    return projected.z - root.shadow_bias <= shadow_depth ? 1.0f : 0.0f;
}

float packet_probe_influence(VkrBindlessIblProbe probe,
                             float3 world_position)
{
    float weight = max(probe.extents_weight.w, 0.0f);
    if (weight <= 1e-6f || probe.intensity_box.w <= 0.5f)
        return weight;
    float3 outside = max(abs(world_position - probe.center_blend.xyz) -
                         max(probe.extents_weight.xyz, 0.0f), 0.0f);
    float distance = length(outside);
    if (distance <= 1e-6f)
        return weight;
    return probe.center_blend.w > 1e-6f
               ? weight * saturate(1.0f - distance / probe.center_blend.w)
               : 0.0f;
}

float3 packet_box_project(float3 direction, float3 world_position,
                          float3 center, float3 extents)
{
    if (any(extents <= 1e-4f))
        return normalize(direction);
    float3 safe_direction = direction;
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        if (abs(safe_direction[axis]) < 1e-4f)
            safe_direction[axis] = safe_direction[axis] < 0.0f
                                           ? -1e-4f
                                           : 1e-4f;
    }
    float3 t_min = (center - extents - world_position) / safe_direction;
    float3 t_max = (center + extents - world_position) / safe_direction;
    float3 t_far = max(t_min, t_max);
    float t_hit = max(min(t_far.x, min(t_far.y, t_far.z)), 0.0f);
    return normalize(world_position + safe_direction * t_hit - center);
}

// Every enabled punctual light for one shaded point. `albedo` and `metallic`
// follow packet_direct: the forward path passes base color with its metallic
// factor, the deferred path passes G-buffer diffuse albedo and zero. Both
// spell the same product, so the two paths shade identically.
VkrBindlessDirectResult packet_punctual(VkrBindlessPacketFrameRoot* frame,
                                        float3 world_position, float3 normal,
                                        float3 view, float3 albedo,
                                        float metallic, float roughness,
                                        float3 f0)
{
    VkrBindlessDirectResult result;
    result.diffuse = 0.0f;
    result.specular = 0.0f;
    uint4 point_mask = packet_point_light_mask(frame, world_position);
    uint point_count = min(frame.point_light_count, 128u);
    for (uint word = 0u; word < 4u; ++word)
    {
        uint remaining = point_mask[word];
        while (remaining != 0u)
        {
            uint light_index = word * 32u + uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            if (light_index >= point_count)
                continue;
            float4 p0 = frame.point_light_data[light_index * 4u + 0u];
            float4 p1 = frame.point_light_data[light_index * 4u + 1u];
            float4 p2 = frame.point_light_data[light_index * 4u + 2u];
            float4 p3 = frame.point_light_data[light_index * 4u + 3u];
            uint kind = uint(p2.w + 0.5f);
            float3 to_light = p0.xyz - world_position;
            float distance_squared = dot(to_light, to_light);
            if (kind != 0u && p2.z > 0.0f &&
                distance_squared >= p2.z * p2.z)
                continue;
            float distance = sqrt(distance_squared);
            float3 light_direction = distance > 1e-6f
                                         ? to_light / distance
                                         : float3(0.0f);
            float attenuation = 0.0f;
            if (kind == 0u)
            {
                attenuation = 1.0f /
                    max(max(p0.w, 1.0f) + p1.w * distance +
                            p2.y * distance_squared,
                        1e-6f);
            }
            else
            {
                float range_attenuation = 1.0f;
                if (p2.z > 0.0f)
                {
                    float ratio = distance / p2.z;
                    range_attenuation = saturate(1.0f - ratio * ratio *
                                                 ratio * ratio);
                    range_attenuation *= range_attenuation;
                }
                attenuation = range_attenuation /
                              max(distance_squared, 1e-4f);
                if (kind == 2u)
                {
                    float cone = dot(-light_direction, normalize(p3.xyz));
                    float cone_attenuation = smoothstep(p1.w, p0.w, cone);
                    if (cone_attenuation <= 0.0f)
                        continue;
                    attenuation *= cone_attenuation;
                }
            }
            VkrBindlessDirectResult direct = packet_direct(
                normal, view, light_direction, p1.rgb * p2.x * attenuation,
                albedo, metallic, roughness, f0);
            result.diffuse += direct.diffuse;
            result.specular += direct.specular;
        }
    }
    return result;
}

// Global environment plus weighted local probes. `horizon_normal` is the
// geometric normal where one survives; the deferred G-buffer keeps only the
// shading normal and passes it for both.
float3 packet_environment(VkrBindlessPacketFrameRoot* frame,
                          float3 world_position, float3 normal,
                          float3 horizon_normal, float3 view,
                          float3 albedo, float metallic, float roughness,
                          float ao, float3 f0)
{
    float no_v = max(dot(normal, view), 0.0f);
    float3 reflection = reflect(-view, normal);
    float3 fresnel = packet_fresnel_roughness(no_v, f0, roughness);
    float3 kd = (1.0f - fresnel) * (1.0f - metallic);
    uint irradiance_texture =
        NonUniformResourceIndex(frame.irradiance_texture);
    uint irradiance_sampler =
        NonUniformResourceIndex(frame.irradiance_sampler);
    uint prefilter_texture =
        NonUniformResourceIndex(frame.prefilter_texture);
    uint prefilter_sampler =
        NonUniformResourceIndex(frame.prefilter_sampler);
    float3 global_irradiance = g_cube_textures[irradiance_texture]
        .SampleLevel(g_samplers[irradiance_sampler], normal, 0.0f).rgb;
    float3 global_prefiltered = g_cube_textures[prefilter_texture]
        .SampleLevel(g_samplers[prefilter_sampler], reflection,
                     roughness * float(max(frame.prefilter_mip_count, 1u) -
                                       1u)).rgb;
    float2 brdf = packet_brdf_approximation(no_v, roughness);
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float horizon = saturate(1.0f + dot(reflection, horizon_normal));
    float specular_visibility = horizon * horizon *
        packet_specular_ao(ao, no_v, roughness);
    float3 diffuse = 0.0f;
    float3 specular = 0.0f;
    float local_weight_sum = 0.0f;
    uint probe_count = min(frame.ibl_probe_count, 16u);
    for (uint i = 0u; i < probe_count; ++i)
        local_weight_sum += packet_probe_influence(frame.ibl_probes[i],
                                                    world_position);
    float weight_scale = local_weight_sum > 1.0f
                             ? 1.0f / local_weight_sum
                             : 1.0f;
    for (uint i = 0u; i < probe_count; ++i)
    {
        VkrBindlessIblProbe probe = frame.ibl_probes[i];
        float weight = packet_probe_influence(probe,
                                              world_position) *
                       weight_scale;
        if (weight <= 1e-6f)
            continue;
        float3 probe_reflection = probe.intensity_box.w > 0.5f
            ? packet_box_project(reflection, world_position,
                                 probe.center_blend.xyz,
                                 max(probe.extents_weight.xyz, 0.0f))
            : reflection;
        uint probe_irradiance_texture =
            NonUniformResourceIndex(probe.irradiance_texture);
        uint probe_irradiance_sampler =
            NonUniformResourceIndex(probe.irradiance_sampler);
        uint probe_prefilter_texture =
            NonUniformResourceIndex(probe.prefilter_texture);
        uint probe_prefilter_sampler =
            NonUniformResourceIndex(probe.prefilter_sampler);
        float3 probe_irradiance =
            g_cube_textures[probe_irradiance_texture].SampleLevel(
                g_samplers[probe_irradiance_sampler], normal, 0.0f).rgb;
        float3 probe_prefiltered =
            g_cube_textures[probe_prefilter_texture].SampleLevel(
                g_samplers[probe_prefilter_sampler], probe_reflection,
                 roughness * float(max(frame.prefilter_mip_count, 1u) -
                                  1u)).rgb;
        diffuse += kd * probe_irradiance * albedo * ao *
                   probe.intensity_box.x * probe.intensity_box.y * weight;
        specular += probe_prefiltered *
                    (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
    }
    float global_weight = max(1.0f - min(local_weight_sum, 1.0f), 0.0f);
    diffuse += kd * global_irradiance * albedo * ao * global_weight;
    specular += global_prefiltered *
                (fresnel * brdf.x + f90 * brdf.y) *
                specular_visibility * global_weight;
    return (diffuse * frame.ibl_controls.y +
            specular * frame.ibl_controls.z) *
           frame.ibl_controls.x;
}

[shader("fragment")]
float4 world_fragment(VkrBindlessVertexOutput input,
                      bool front_facing : SV_IsFrontFace) : SV_Target0
{
    VkrBindlessPacketDrawRoot* draw = g_push.root;
    VkrBindlessPacketFrameRoot* frame = draw.frame;
    VkrGpuVisibleDrawRow visible = draw.visible_rows[draw.visible_row_index];
    VkrBindlessMaterial material = frame.materials[visible.material_index];
    float4 base = packet_base_color(input);
    if (material.alpha_mode == 1u && base.a < material.material_alpha.x)
        discard;
    if ((frame.flags & 1u) == 0u)
        return base;

    float face_sign = front_facing ? 1.0f : -1.0f;
    float3 geometric_normal = normalize(input.world_normal) * face_sign;
    float3 normal = geometric_normal;
    if ((material.flags & 1u) != 0u)
    {
        uint texture_index = NonUniformResourceIndex(material.normal_texture);
        uint sampler_index = NonUniformResourceIndex(material.normal_sampler);
        float3 sampled = g_textures[texture_index].Sample(
            g_samplers[sampler_index], input.uv).xyz * 2.0f - 1.0f;
        sampled.xy *= material.material_surface.z;
        sampled.y = -sampled.y;
        float3 tangent = normalize(input.world_tangent.xyz);
        tangent = normalize(tangent - dot(tangent, normal) * normal);
        float3 bitangent = cross(normal, tangent) * input.world_tangent.w;
        float3x3 tangent_basis = float3x3(tangent, bitangent, normal);
        normal = normalize(mul(tangent_basis, sampled));
    }
    if (frame.render_mode == 2u)
        return float4(normal * 0.5f + 0.5f, 1.0f);

    float3 view = normalize(frame.view_position.xyz - input.world_position);
    float no_v = max(dot(normal, view), 0.0f);
    float metallic = saturate(material.material_surface.x);
    float roughness = clamp(material.material_surface.y, 0.04f, 1.0f);
    float ao = saturate(material.material_surface.w);
    float3 emissive = material.material_emissive.rgb;
    if ((material.flags & 2u) != 0u)
    {
        uint texture_index = NonUniformResourceIndex(material.orm_texture);
        uint sampler_index = NonUniformResourceIndex(material.orm_sampler);
        float3 orm = g_textures[texture_index].Sample(
            g_samplers[sampler_index], input.uv).rgb;
        ao *= orm.r;
        roughness = clamp(roughness * orm.g, 0.04f, 1.0f);
        metallic = saturate(metallic * orm.b);
    }
    float3 normal_dx = ddx(normal);
    float3 normal_dy = ddy(normal);
    float variance = 0.25f * (dot(normal_dx, normal_dx) +
                              dot(normal_dy, normal_dy));
    roughness = sqrt(saturate(roughness * roughness + min(variance, 0.25f)));
    if ((material.flags & 4u) != 0u)
    {
        uint texture_index = NonUniformResourceIndex(material.emissive_texture);
        uint sampler_index = NonUniformResourceIndex(material.emissive_sampler);
        emissive *= g_textures[texture_index].Sample(
            g_samplers[sampler_index], input.uv).rgb;
    }
    if (frame.render_mode == 3u)
        return float4(base.rgb + emissive,
                      material.alpha_mode == 0u ? 1.0f : base.a);

    float3 f0 = lerp(saturate(material.material_dielectric_specular.rgb),
                     base.rgb, metallic);
    if (frame.render_mode == 6u)
        return float4(metallic, roughness,
                      max(f0.x, max(f0.y, f0.z)), 1.0f);

    float3 analytic_diffuse = 0.0f;
    float3 analytic_specular = 0.0f;
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        float shadow = packet_directional_shadow(frame, input.world_position);
        VkrBindlessDirectResult direct = packet_direct(
            normal, view, normalize(-frame.directional_direction_enabled.xyz),
            frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * shadow,
            base.rgb, metallic, roughness, f0);
        analytic_diffuse = direct.diffuse;
        analytic_specular = direct.specular;
    }
    VkrBindlessDirectResult punctual = packet_punctual(
        frame, input.world_position, normal, view, base.rgb, metallic,
        roughness, f0);
    analytic_diffuse += punctual.diffuse;
    analytic_specular += punctual.specular;
    float3 color = analytic_diffuse + analytic_specular;

    if ((frame.flags & 2u) != 0u)
    {
        color += packet_environment(frame, input.world_position, normal,
                                    geometric_normal, view, base.rgb,
                                    metallic, roughness, ao, f0);
    }
    else
    {
        color += frame.ambient_color.rgb * base.rgb * ao;
    }
    if (frame.render_mode == 1u)
        return float4(analytic_diffuse + analytic_specular, 1.0f);
    if (frame.render_mode == 4u)
        return float4(analytic_diffuse, 1.0f);
    if (frame.render_mode == 5u)
        return float4(analytic_specular, 1.0f);
    color += emissive;

    if ((frame.flags & 4u) != 0u && material.material_alpha.y > 0.0f)
    {
        float transmission = saturate(material.material_alpha.y);
        float thickness = max(material.material_alpha.w, 0.0f);
        float3 view_normal = normalize(mul(frame.view,
                                           float4(normal, 0.0f)).xyz);
        float3 refracted = refract(float3(0.0f, 0.0f, -1.0f), view_normal,
                                   1.0f / max(material.material_alpha.z, 1.0f));
        float2 screen_uv = input.position.xy *
                           float2(frame.ibl_controls.w, frame.ambient_color.w);
        float2 offset = refracted.xy / max(abs(refracted.z), 0.25f) *
                        thickness * 0.02f;
        uint texture_index =
            NonUniformResourceIndex(frame.transmission_texture);
        uint sampler_index =
            NonUniformResourceIndex(frame.transmission_sampler);
        float3 transmitted = g_textures[texture_index].SampleLevel(
            g_samplers[sampler_index],
            clamp(screen_uv + offset, 0.0f, 1.0f), 0.0f).rgb;
        if (material.material_attenuation_color.w > 1e-4f && thickness > 0.0f)
        {
            transmitted *= pow(clamp(material.material_attenuation_color.rgb,
                                     1e-4f, 1.0f),
                               thickness /
                                   material.material_attenuation_color.w);
        }
        float3 fresnel = packet_fresnel(no_v, f0);
        float weight = transmission * (1.0f - metallic) *
                       (1.0f - max(fresnel.x, max(fresnel.y, fresnel.z)));
        color = lerp(color, transmitted, saturate(weight));
    }
    return float4(color, material.alpha_mode == 0u ? 1.0f : base.a);
}

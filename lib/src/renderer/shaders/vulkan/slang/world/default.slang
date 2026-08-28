[shader("vertex")]
VkrVulkanVertexOutput world_vertex(uint vertex_id : SV_VertexID,
                                   uint instance_id : SV_InstanceID)
{
    return packet_vertex_impl(vertex_id, instance_id,
                              g_push.root.frame.view_projection);
}

[shader("vertex")]
VkrVulkanTemporalVertexOutput
world_temporal_vertex(uint vertex_id : SV_VertexID,
                      uint instance_id : SV_InstanceID)
{
    return packet_temporal_vertex_impl(vertex_id, instance_id,
                                       g_push.root.frame.view_projection);
}

float4 packet_base_color(VkrVulkanVertexOutput input)
{
    VkrGpuVisibleDrawRow visible =
        g_push.root.visible_rows[g_push.root.visible_row_index];
    VkrVulkanMaterial material =
        g_push.root.frame.materials[visible.material_index];
    uint texture_index = NonUniformResourceIndex(material.base_color_texture);
    uint sampler_index = NonUniformResourceIndex(material.base_color_sampler);
    return g_textures[texture_index].Sample(g_samplers[sampler_index],
                                            input.uv) *
           material.tint * input.color;
}

struct VkrVulkanDirectResult
{
    float3 diffuse;
    float3 specular;
};

// Schlick's fifth power as three multiplies. `pow` lowers to exp2/log2, which
// this runs once per light per pixel inside packet_punctual's inner loop.
float packet_schlick_weight(float cosine)
{
    float m = max(1.0f - cosine, 0.0f);
    float m2 = m * m;
    return m2 * m2 * m;
}

float3 packet_fresnel(float cosine, float3 f0)
{
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    return f0 + (f90 - f0) * packet_schlick_weight(cosine);
}

float3 packet_fresnel_roughness(float cosine, float3 f0, float roughness)
{
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float3 rough_f90 = f90 * (1.0f - roughness);
    return f0 + (max(rough_f90, f0) - f0) * packet_schlick_weight(cosine);
}

float packet_geometry_schlick(float no_x, float roughness)
{
    float r = roughness + 1.0f;
    float k = r * r * 0.125f;
    return no_x / max(no_x * (1.0f - k) + k, 1e-6f);
}

VkrVulkanDirectResult packet_direct(float3 normal, float3 view, float3 light,
                                    float3 radiance, float3 albedo,
                                    float metallic, float roughness, float3 f0)
{
    float3 half_vector = normalize(view + light);
    float no_l = max(dot(normal, light), 0.0f);
    float no_v = max(dot(normal, view), 0.0f);
    float3 fresnel = packet_fresnel(max(dot(half_vector, view), 0.0f), f0);
    float distribution =
        distribution_ggx(max(dot(normal, half_vector), 0.0f), roughness);
    float geometry = packet_geometry_schlick(no_v, roughness) *
                     packet_geometry_schlick(no_l, roughness);
    float3 specular =
        distribution * geometry * fresnel / max(4.0f * no_v * no_l, 1e-6f);
    float3 kd = (1.0f - fresnel) * (1.0f - metallic);
    float3 scale = radiance * no_l;
    VkrVulkanDirectResult result;
    result.diffuse = kd * albedo / vkr_pi * scale;
    result.specular = specular * scale;
    return result;
}

float packet_specular_ao(float ao, float no_v, float roughness)
{
    return saturate(pow(no_v + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f +
                    ao);
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

uint4 packet_point_light_mask(VkrVulkanPacketFrameRoot *root,
                              float3 world_position)
{
    uint4 result = root.point_light_global_mask;
    uint4 dimensions = root.point_light_grid_dimensions_count;
    float cell_size = root.point_light_grid_origin_cell_size.w;
    if (dimensions.w == 0u || cell_size <= 0.0f)
        return result;
    int3 cell = int3(
        floor((world_position - root.point_light_grid_origin_cell_size.xyz) /
              cell_size));
    if (any(cell < 0) || cell.x >= int(dimensions.x) ||
        cell.y >= int(dimensions.y) || cell.z >= int(dimensions.z))
        return result;
    uint index = uint(cell.x) +
                 dimensions.x * (uint(cell.y) + dimensions.y * uint(cell.z));
    return result | root.point_light_masks[index];
}

/**
 * Stable rotation for the Poisson kernel.
 *
 * The cell index lives in the cascade's light-space texel grid, not in screen
 * space, so it does not swim as the camera moves: the fit snaps in whole texels
 * and `light_space_origin` cancels the remaining translation. A screen-space or
 * raw world-position hash would reintroduce the crawling the rotation exists to
 * hide.
 */
float2 packet_shadow_kernel_rotation(float2 uv, float2 origin_texels,
                                     float map_size)
{
    // `origin - uv * map_size`, not the sum. The fit anchor appears in both
    // terms with the same sign, so subtracting cancels it and leaves the world
    // point's own light-basis projection in texels. Adding would double it, and
    // the cell would jump every time the cascade re-anchors -- the exact
    // swimming this rotation exists to prevent.
    int2 cell = int2(floor(origin_texels - uv * map_size));
    uint hash = (uint(cell.x) * 73856093u) ^ (uint(cell.y) * 19349663u);
    float angle = float(hash & 1023u) * (6.28318530718f / 1024.0f);
    return float2(cos(angle), sin(angle));
}

/**
 * Accumulates rotated taps `[first, count)` of the shared progressive table.
 *
 * Frame-root descriptor indices are dynamically uniform: every invocation reads
 * the same push-constant-addressed root. Decorating them NonUniform costs a
 * waterfall loop and blocks scalarization for nothing.
 */
float packet_shadow_kernel(VkrVulkanPacketFrameRoot *root, uint cascade,
                           float2 uv, float reference, float2 rotation,
                           float2 tap_scale, uint first, uint count)
{
    float sum = 0.0f;
    for (uint i = first; i < count; ++i)
    {
        float2 tap = vkr_shadow_poisson[i];
        float2 rotated = float2(tap.x * rotation.x - tap.y * rotation.y,
                                tap.x * rotation.y + tap.y * rotation.x);
        // A comparison sampler makes every tap a hardware bilinear 2x2 PCF, so
        // a distant cascade degrades into softness instead of into crawling
        // staircase noise.
        sum += g_depth_array_textures[root.shadow_texture].SampleCmpLevelZero(
            g_comparison_samplers[root.shadow_comparison_sampler],
            float3(uv + rotated * tap_scale, float(cascade)), reference);
    }
    return sum;
}

/** One cascade's visibility. Returns 1.0 where the receiver misses it. */
float packet_shadow_cascade(VkrVulkanPacketFrameRoot *root, uint cascade,
                            float3 world_position, float3 normal, float n_dot_l)
{
    VkrVulkanShadowCascade data = root.shadow_cascades[cascade];
    float texel = data.split_near_far_texel_depth.z;
    float depth_span = data.split_near_far_texel_depth.w;

    // Normal offset is a world-space push before projection, so it moves the
    // sample off the surface rather than biasing the comparison. It stays
    // effective at grazing angles, where depth bias is weakest.
    float3 offset_position =
        world_position + normal * (root.shadow_normal_offset_texels * texel);
    float4 clip =
        mul(data.light_view_projection, float4(offset_position, 1.0f));
    if (abs(clip.w) <= 1e-6f)
        return 1.0f;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || projected.z < 0.0f ||
        projected.z > 1.0f)
        return 1.0f;

    // Both biases are configured in texels and converted through *this*
    // cascade's own footprint and fitted depth span. That is what makes one
    // configured value mean the same world distance in cascade 0 and cascade 3,
    // whose depth ranges differ by orders of magnitude.
    float slope = saturate(1.0f - n_dot_l);
    float bias_texels = root.shadow_receiver_bias_texels +
                        root.shadow_slope_bias_texels * slope;
    float reference = projected.z - bias_texels * texel / depth_span;

    float inv_size = data.origin_inv_size_pad.z;
    float2 rotation = packet_shadow_kernel_rotation(
        uv, data.origin_inv_size_pad.xy / texel, 1.0f / inv_size);
    float2 tap_scale = root.shadow_pcf_radius_texels * inv_size;
    uint count = root.shadow_pcf_sample_count;

    if (count >= 16u && root.shadow_pcf_uniform_early_out != 0u)
    {
        // Fully lit and fully shadowed regions are the common case and resolve
        // in nine taps. Only the penumbra pays for the full kernel.
        float probe = packet_shadow_kernel(root, cascade, uv, reference,
                                           rotation, tap_scale, 0u, 9u);
        if (probe <= 0.0f || probe >= 9.0f)
            return probe * (1.0f / 9.0f);
        return (probe + packet_shadow_kernel(root, cascade, uv, reference,
                                             rotation, tap_scale, 9u, count)) /
               float(count);
    }
    return packet_shadow_kernel(root, cascade, uv, reference, rotation,
                                tap_scale, 0u, count) /
           float(count);
}

float packet_directional_shadow(VkrVulkanPacketFrameRoot *root,
                                float3 world_position, float3 normal)
{
    if (root.shadow_cascade_count == 0u)
        return 1.0f;
    float view_depth = abs(mul(root.view, float4(world_position, 1.0f)).z);

    // Fade to unshadowed before the last split so the shadowed range ends in a
    // gradient rather than on a hard terminating edge.
    float fade_span = root.shadow_fade_end - root.shadow_fade_start;
    float fade = fade_span > 0.0f
                     ? saturate((root.shadow_fade_end - view_depth) / fade_span)
                     : 1.0f;
    if (fade <= 0.0f)
        return 1.0f;

    uint cascade = 0u;
    for (uint i = 0u; i + 1u < root.shadow_cascade_count; ++i)
    {
        if (view_depth > root.shadow_cascades[i].split_near_far_texel_depth.y)
            cascade = i + 1u;
    }

    float n_dot_l =
        dot(normal, normalize(-root.directional_direction_enabled.xyz));
    float factor =
        packet_shadow_cascade(root, cascade, world_position, normal, n_dot_l);

    // Cross-fade over the last `blend_fraction` of this cascade's span. Outside
    // the band exactly one cascade is sampled, so the seam cost is confined to
    // the band instead of being paid everywhere.
    if (cascade + 1u < root.shadow_cascade_count)
    {
        float4 slice = root.shadow_cascades[cascade].split_near_far_texel_depth;
        float band = (slice.y - slice.x) * root.shadow_cascade_blend_fraction;
        if (band > 0.0f && view_depth > slice.y - band)
        {
            float t = saturate((view_depth - (slice.y - band)) / band);
            float next = packet_shadow_cascade(root, cascade + 1u,
                                               world_position, normal, n_dot_l);
            factor = lerp(factor, next, t);
        }
    }

    return lerp(1.0f, factor, fade);
}

struct VkrVulkanShadowDebugSample
{
    float factor;
    float map_depth;
    uint cascade;
    uint valid;
};

VkrVulkanShadowDebugSample
packet_directional_shadow_debug(VkrVulkanPacketFrameRoot *root,
                                float3 world_position, float3 normal)
{
    VkrVulkanShadowDebugSample sample;
    sample.factor = 1.0f;
    sample.map_depth = 1.0f;
    sample.cascade = 0u;
    sample.valid = 0u;
    if (root.shadow_cascade_count == 0u)
        return sample;

    float view_depth = abs(mul(root.view, float4(world_position, 1.0f)).z);
    float fade_span = root.shadow_fade_end - root.shadow_fade_start;
    float fade = fade_span > 0.0f
                     ? saturate((root.shadow_fade_end - view_depth) / fade_span)
                     : 1.0f;
    if (fade <= 0.0f)
    {
        sample.cascade = root.shadow_cascade_count - 1u;
        return sample;
    }

    for (uint i = 0u; i + 1u < root.shadow_cascade_count; ++i)
    {
        if (view_depth > root.shadow_cascades[i].split_near_far_texel_depth.y)
            sample.cascade = i + 1u;
    }
    float n_dot_l =
        dot(normal, normalize(-root.directional_direction_enabled.xyz));
    sample.factor = packet_shadow_cascade(root, sample.cascade, world_position,
                                          normal, n_dot_l);
    if (sample.cascade + 1u < root.shadow_cascade_count)
    {
        float4 slice =
            root.shadow_cascades[sample.cascade].split_near_far_texel_depth;
        float band = (slice.y - slice.x) * root.shadow_cascade_blend_fraction;
        if (band > 0.0f && view_depth > slice.y - band)
        {
            float t = saturate((view_depth - (slice.y - band)) / band);
            float next = packet_shadow_cascade(root, sample.cascade + 1u,
                                               world_position, normal, n_dot_l);
            sample.factor = lerp(sample.factor, next, t);
        }
    }
    sample.factor = lerp(1.0f, sample.factor, fade);

    VkrVulkanShadowCascade data = root.shadow_cascades[sample.cascade];
    float texel = data.split_near_far_texel_depth.z;
    float3 offset_position =
        world_position + normal * (root.shadow_normal_offset_texels * texel);
    float4 clip =
        mul(data.light_view_projection, float4(offset_position, 1.0f));
    if (abs(clip.w) <= 1e-6f)
        return sample;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || projected.z < 0.0f ||
        projected.z > 1.0f)
        return sample;
    sample.valid = 1u;
    if (root.shadow_debug_mode == 3u)
    {
        sample.map_depth =
            g_depth_array_textures[root.shadow_texture].SampleLevel(
                g_samplers[root.shadow_sampler],
                float3(uv, float(sample.cascade)), 0.0f);
    }
    return sample;
}

float3 packet_shadow_debug_color(uint mode, VkrVulkanShadowDebugSample sample)
{
    if (mode == 1u)
    {
        switch (sample.cascade & 3u)
        {
        case 0u:
            return float3(1.0f, 0.2f, 0.2f);
        case 1u:
            return float3(0.2f, 1.0f, 0.2f);
        case 2u:
            return float3(0.2f, 0.4f, 1.0f);
        default:
            return float3(1.0f, 0.85f, 0.2f);
        }
    }
    if (mode == 2u)
        return sample.factor;
    if (mode == 3u)
        return sample.map_depth;
    return sample.valid != 0u ? sample.factor : 1.0f;
}

float packet_probe_influence(VkrVulkanIblProbe probe, float3 world_position)
{
    float weight = max(probe.extents_weight.w, 0.0f);
    if (weight <= 1e-6f || probe.intensity_box.w <= 0.5f)
        return weight;
    float3 outside = max(abs(world_position - probe.center_blend.xyz) -
                             max(probe.extents_weight.xyz, 0.0f),
                         0.0f);
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
            safe_direction[axis] = safe_direction[axis] < 0.0f ? -1e-4f : 1e-4f;
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
VkrVulkanDirectResult packet_punctual(VkrVulkanPacketFrameRoot *frame,
                                      float3 world_position, float3 normal,
                                      float3 view, float3 albedo,
                                      float metallic, float roughness,
                                      float3 f0)
{
    VkrVulkanDirectResult result;
    result.diffuse = 0.0f;
    result.specular = 0.0f;
    uint4 point_mask = packet_point_light_mask(frame, world_position);
    uint point_count = min(frame.point_light_count, 128u);
    for (uint word = 0u; word < 4u; ++word)
    {
        // The mask comes from a per-pixel cluster cell, so lanes straddling a
        // cell boundary carry different masks and the wave already pays for the
        // union of their set bits. Iterating that union explicitly makes
        // `light_index` wave-uniform, which lets the light rows load as scalars
        // instead of per-lane vectors. Each lane still accumulates only its own
        // bits, so the summation order and result are unchanged.
        uint remaining = WaveActiveBitOr(point_mask[word]);
        while (remaining != 0u)
        {
            uint bit = uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            uint light_index = word * 32u + bit;
            if (light_index >= point_count)
                continue;
            float4 p0 = frame.point_light_data[light_index * 4u + 0u];
            float4 p1 = frame.point_light_data[light_index * 4u + 1u];
            float4 p2 = frame.point_light_data[light_index * 4u + 2u];
            float4 p3 = frame.point_light_data[light_index * 4u + 3u];
            if ((point_mask[word] & (1u << bit)) == 0u)
                continue;
            uint kind = uint(p2.w + 0.5f);
            float3 to_light = p0.xyz - world_position;
            float distance_squared = dot(to_light, to_light);
            if (kind != 0u && p2.z > 0.0f && distance_squared >= p2.z * p2.z)
                continue;
            float distance = sqrt(distance_squared);
            float3 light_direction =
                distance > 1e-6f ? to_light / distance : float3(0.0f);
            float attenuation = 0.0f;
            if (kind == 0u)
            {
                attenuation = 1.0f / max(max(p0.w, 1.0f) + p1.w * distance +
                                             p2.y * distance_squared,
                                         1e-6f);
            }
            else
            {
                float range_attenuation = 1.0f;
                if (p2.z > 0.0f)
                {
                    float ratio = distance / p2.z;
                    range_attenuation =
                        saturate(1.0f - ratio * ratio * ratio * ratio);
                    range_attenuation *= range_attenuation;
                }
                attenuation = range_attenuation / max(distance_squared, 1e-4f);
                if (kind == 2u)
                {
                    float cone = dot(-light_direction, normalize(p3.xyz));
                    float cone_attenuation = smoothstep(p1.w, p0.w, cone);
                    if (cone_attenuation <= 0.0f)
                        continue;
                    attenuation *= cone_attenuation;
                }
            }
            VkrVulkanDirectResult direct = packet_direct(
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
struct VkrVulkanEnvironmentResult
{
    float3 diffuse;
    float3 specular;
};

VkrVulkanEnvironmentResult
packet_environment_terms(VkrVulkanPacketFrameRoot *frame, float3 world_position,
                         float3 normal, float3 horizon_normal, float3 view,
                         float3 albedo, float metallic, float roughness,
                         float ao, float3 f0)
{
    float no_v = max(dot(normal, view), 0.0f);
    float3 reflection = reflect(-view, normal);
    float3 fresnel = packet_fresnel_roughness(no_v, f0, roughness);
    float3 kd = (1.0f - fresnel) * (1.0f - metallic);
    // Frame-root indices are uniform. Each local-probe loop iteration also
    // visits one shared table row across the active lanes.
    float3 global_irradiance =
        g_cube_textures[frame.irradiance_texture]
            .SampleLevel(g_samplers[frame.irradiance_sampler], normal, 0.0f)
            .rgb;
    float3 global_prefiltered =
        g_cube_textures[frame.prefilter_texture]
            .SampleLevel(g_samplers[frame.prefilter_sampler], reflection,
                         roughness *
                             float(max(frame.prefilter_mip_count, 1u) - 1u))
            .rgb;
    float2 brdf = packet_brdf_approximation(no_v, roughness);
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float horizon = saturate(1.0f + dot(reflection, horizon_normal));
    float specular_visibility =
        horizon * horizon * packet_specular_ao(ao, no_v, roughness);
    float3 diffuse = 0.0f;
    float3 specular = 0.0f;
    float local_weight_sum = 0.0f;
    uint probe_count = min(frame.ibl_probe_count, 16u);
    for (uint i = 0u; i < probe_count; ++i)
        local_weight_sum +=
            packet_probe_influence(frame.ibl_probes[i], world_position);
    float weight_scale =
        local_weight_sum > 1.0f ? 1.0f / local_weight_sum : 1.0f;
    for (uint i = 0u; i < probe_count; ++i)
    {
        VkrVulkanIblProbe probe = frame.ibl_probes[i];
        float weight =
            packet_probe_influence(probe, world_position) * weight_scale;
        if (weight <= 1e-6f)
            continue;
        float3 probe_reflection =
            probe.intensity_box.w > 0.5f
                ? packet_box_project(reflection, world_position,
                                     probe.center_blend.xyz,
                                     max(probe.extents_weight.xyz, 0.0f))
                : reflection;
        float3 probe_irradiance =
            g_cube_textures[probe.irradiance_texture]
                .SampleLevel(g_samplers[probe.irradiance_sampler], normal, 0.0f)
                .rgb;
        float3 probe_prefiltered =
            g_cube_textures[probe.prefilter_texture]
                .SampleLevel(
                    g_samplers[probe.prefilter_sampler], probe_reflection,
                    roughness * float(max(frame.prefilter_mip_count, 1u) - 1u))
                .rgb;
        diffuse += kd * probe_irradiance * albedo * ao * probe.intensity_box.x *
                   probe.intensity_box.y * weight;
        specular += probe_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
    }
    float global_weight = max(1.0f - min(local_weight_sum, 1.0f), 0.0f);
    diffuse += kd * global_irradiance * albedo * ao * global_weight;
    specular += global_prefiltered * (fresnel * brdf.x + f90 * brdf.y) *
                specular_visibility * global_weight;
    VkrVulkanEnvironmentResult result;
    result.diffuse = diffuse * frame.ibl_controls.y * frame.ibl_controls.x;
    result.specular = specular * frame.ibl_controls.z * frame.ibl_controls.x;
    return result;
}

float3 packet_environment(VkrVulkanPacketFrameRoot *frame,
                          float3 world_position, float3 normal,
                          float3 horizon_normal, float3 view, float3 albedo,
                          float metallic, float roughness, float ao,
                          float directional_visibility, float3 f0)
{
    VkrVulkanEnvironmentResult result =
        packet_environment_terms(frame, world_position, normal, horizon_normal,
                                 view, albedo, metallic, roughness, ao, f0);
    // Keep the authored IBL floor in shadow, then add its sun-visible diffuse
    // share through the existing CSM result. Specular IBL remains unchanged.
    float sun_visible_diffuse_scale =
        1.0f + saturate(directional_visibility) *
                   max(frame.directional_color_intensity.w, 0.0f);
    return result.diffuse * sun_visible_diffuse_scale + result.specular;
}

float4 packet_world_shade(VkrVulkanVertexOutput input, bool front_facing)
{
    VkrVulkanPacketDrawRoot *draw = g_push.root;
    VkrVulkanPacketFrameRoot *frame = draw.frame;
    VkrGpuVisibleDrawRow visible = draw.visible_rows[draw.visible_row_index];
    VkrVulkanMaterial material = frame.materials[visible.material_index];
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
        float3 sampled = vkr_normal_map_decode(
            g_textures[texture_index]
                .Sample(g_samplers[sampler_index], input.uv)
                .xyz,
            material.material_surface.z);
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
        float3 orm = g_textures[texture_index]
                         .Sample(g_samplers[sampler_index], input.uv)
                         .rgb;
        ao *= orm.r;
        roughness = clamp(roughness * orm.g, 0.04f, 1.0f);
        metallic = saturate(metallic * orm.b);
    }
    float3 normal_dx = ddx(normal);
    float3 normal_dy = ddy(normal);
    float variance =
        0.25f * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
    roughness = sqrt(saturate(roughness * roughness + min(variance, 0.25f)));
    if ((material.flags & 4u) != 0u)
    {
        uint texture_index = NonUniformResourceIndex(material.emissive_texture);
        uint sampler_index = NonUniformResourceIndex(material.emissive_sampler);
        emissive *= g_textures[texture_index]
                        .Sample(g_samplers[sampler_index], input.uv)
                        .rgb;
    }
    if (frame.render_mode == 3u)
        return float4(base.rgb + emissive,
                      material.alpha_mode == 0u ? 1.0f : base.a);

    float3 f0 = lerp(saturate(material.material_dielectric_specular.rgb),
                     base.rgb, metallic);
    if (frame.render_mode == 6u)
        return float4(metallic, roughness, max(f0.x, max(f0.y, f0.z)), 1.0f);
    if (frame.shadow_debug_mode != 0u)
    {
        VkrVulkanShadowDebugSample shadow_sample =
            packet_directional_shadow_debug(frame, input.world_position,
                                            normal);
        return float4(
            packet_shadow_debug_color(frame.shadow_debug_mode, shadow_sample),
            1.0f);
    }

    float3 analytic_diffuse = 0.0f;
    float3 analytic_specular = 0.0f;
    float directional_shadow = 0.0f;
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        directional_shadow =
            packet_directional_shadow(frame, input.world_position, normal);
        VkrVulkanDirectResult direct = packet_direct(
            normal, view, normalize(-frame.directional_direction_enabled.xyz),
            frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * directional_shadow,
            base.rgb, metallic, roughness, f0);
        analytic_diffuse = direct.diffuse;
        analytic_specular = direct.specular;
    }
    VkrVulkanDirectResult punctual =
        packet_punctual(frame, input.world_position, normal, view, base.rgb,
                        metallic, roughness, f0);
    analytic_diffuse += punctual.diffuse;
    analytic_specular += punctual.specular;
    float3 color = analytic_diffuse + analytic_specular;

    if ((frame.flags & 2u) != 0u)
    {
        color += packet_environment(frame, input.world_position, normal,
                                    geometric_normal, view, base.rgb, metallic,
                                    roughness, ao, directional_shadow, f0);
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
        float3 view_normal =
            normalize(mul(frame.view, float4(normal, 0.0f)).xyz);
        float3 refracted = refract(float3(0.0f, 0.0f, -1.0f), view_normal,
                                   1.0f / max(material.material_alpha.z, 1.0f));
        float2 screen_uv = input.position.xy *
                           float2(frame.ibl_controls.w, frame.ambient_color.w);
        float2 offset =
            refracted.xy / max(abs(refracted.z), 0.25f) * thickness * 0.02f;
        uint texture_index =
            NonUniformResourceIndex(frame.transmission_texture);
        uint sampler_index =
            NonUniformResourceIndex(frame.transmission_sampler);
        float3 transmitted =
            g_textures[texture_index]
                .SampleLevel(g_samplers[sampler_index],
                             clamp(screen_uv + offset, 0.0f, 1.0f), 0.0f)
                .rgb;
        if (material.material_attenuation_color.w > 1e-4f && thickness > 0.0f)
        {
            transmitted *=
                pow(clamp(material.material_attenuation_color.rgb, 1e-4f, 1.0f),
                    thickness / material.material_attenuation_color.w);
        }
        float3 fresnel = packet_fresnel(no_v, f0);
        float weight = transmission * (1.0f - metallic) *
                       (1.0f - max(fresnel.x, max(fresnel.y, fresnel.z)));
        color = lerp(color, transmitted, saturate(weight));
    }
    return float4(color, material.alpha_mode == 0u ? 1.0f : base.a);
}

struct VkrVulkanTemporalBlendOutput
{
    float4 color : SV_Target0;
    uint2 surface : SV_Target1;
    float2 motion : SV_Target2;
    float2 validity : SV_Target3;
};

[shader("fragment")]
float4 world_fragment(VkrVulkanVertexOutput input,
                      bool front_facing : SV_IsFrontFace) : SV_Target0
{
    return packet_world_shade(input, front_facing);
}

[shader("fragment")]
VkrVulkanTemporalBlendOutput
world_temporal_fragment(VkrVulkanTemporalVertexOutput input,
                        bool front_facing : SV_IsFrontFace,
                        uint primitive_id : SV_PrimitiveID)
{
    VkrVulkanTemporalBlendOutput output;
    VkrVulkanVertexOutput surface;
    surface.position = input.position;
    surface.uv = input.uv;
    surface.color = input.color;
    surface.world_position = input.world_position;
    surface.world_normal = input.world_normal;
    surface.object_id = input.object_id;
    surface.world_tangent = input.world_tangent;
    output.color = packet_world_shade(surface, front_facing);
    if (output.color.a <= 1e-4f)
        discard;

    const uint overlay_bit = 0x80000000u;
    const uint generation_mask = 0x7fffffffu;
    const uint primitive_mask = 0x1ffffu;
    bool identity_valid =
        (input.temporal_flags & VKR_INSTANCE_TEMPORAL_OWNER) != 0u &&
        input.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY &&
        input.temporal_generation > 0u &&
        input.temporal_generation <= generation_mask &&
        primitive_id < primitive_mask;
    output.surface = uint2(overlay_bit, 0u);
    if (identity_valid)
    {
        output.surface.x = overlay_bit | input.temporal_generation;
        output.surface.y =
            (input.temporal_index << 17u) | (primitive_id + 1u);
    }
    output.motion = 0.0f;
    output.validity = 0.0f;

    VkrVulkanPacketTemporalDrawState *temporal =
        g_push.root.frame.temporal_draw_state;
    if (identity_valid && temporal.history_valid != 0u)
    {
        VkrTemporalTransform previous =
            temporal.previous_transforms[input.temporal_index];
        if (previous.valid != 0u &&
            previous.generation == input.temporal_generation &&
            previous.frame_index == temporal.previous_frame_index)
        {
            float4 current_clip = mul(
                temporal.current_view_projection,
                float4(input.world_position, 1.0f));
            float4 previous_clip = mul(
                temporal.previous_view_projection,
                mul(previous.model, float4(input.object_position, 1.0f)));
            if (current_clip.w > 1e-6f && previous_clip.w > 1e-6f)
            {
                float2 current_uv =
                    current_clip.xy / current_clip.w * 0.5f + 0.5f;
                float2 previous_uv =
                    previous_clip.xy / previous_clip.w * 0.5f + 0.5f;
                VkrGpuVisibleDrawRow visible =
                    g_push.root.visible_rows[g_push.root.visible_row_index];
                float reactivity = saturate(
                    g_push.root.frame.materials[visible.material_index]
                        .temporal_reactivity);
                output.motion = previous_uv - current_uv;
                output.validity =
                    float2(2.0f + reactivity,
                           previous_clip.z / previous_clip.w);
            }
        }
    }
    return output;
}

[shader("vertex")]
VkrVulkanVertexOutput world_vertex(uint vertex_id : SV_VertexID,
                                   uint instance_id : SV_InstanceID)
{
    VkrVulkanPacketDrawRoot* draw = vkr_vulkan_packet_draw_root();
    return packet_vertex_impl(vertex_id, instance_id,
                              draw.frame.view_projection);
}

[shader("vertex")]
VkrVulkanTemporalVertexOutput
world_temporal_vertex(uint vertex_id : SV_VertexID,
                      uint instance_id : SV_InstanceID)
{
    VkrVulkanPacketDrawRoot* draw = vkr_vulkan_packet_draw_root();
    return packet_temporal_vertex_impl(vertex_id, instance_id,
                                       draw.frame.view_projection);
}

float4 packet_base_color(VkrVulkanVertexOutput input)
{
    VkrVulkanPacketDrawRoot* draw = vkr_vulkan_packet_draw_root();
    VkrGpuVisibleDrawRow visible =
        draw.visible_rows[draw.visible_row_index];
    VkrVulkanMaterial material =
        draw.frame.materials[visible.material_index];
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
    float3 irradiance;
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





VkrVulkanDirectResult packet_direct(float3 normal, float3 view, float3 light,
                                    float3 radiance, float3 albedo,
                                    float metallic, float roughness, float3 f0, VkrGgxMaterialEnergy energy)
{
    float3 half_vector = view + light;
    float no_l = max(dot(normal, light), 0.0f);
    float half_inverse_length =
        vkr_direct_half_vector_inverse_length(no_l, half_vector);
    if (half_inverse_length == 0.0f)
    {
        VkrVulkanDirectResult back = (VkrVulkanDirectResult)0;
        back.diffuse = energy.back_diffuse_weight * albedo * (1.0f - metallic) *
            (max(-dot(normal, light), 0.0f) / vkr_pi) * radiance;
        return back;
    }
    half_vector *= half_inverse_length;
    float3 fresnel = packet_fresnel(max(dot(half_vector, view), 0.0f), f0);
    float distribution =
        vkr_ggx_surface_distribution(normal, half_vector, roughness, energy);
    float3 specular = distribution * vkr_ggx_surface_visibility(no_l, light, energy) *
                      fresnel * energy.specular_scale;
    float3 kd = energy.diffuse_weight * (1.0f - metallic);
    float3 scale = radiance * no_l;
    VkrVulkanDirectResult result;
    result.irradiance = scale / vkr_pi;
    result.diffuse = kd * albedo / vkr_pi * scale;
    result.specular = specular * scale;
    return result;
}

float3 packet_direct_specular(float3 normal, float3 view, float3 light,
                              float3 radiance, float roughness, float3 f0, VkrGgxMaterialEnergy energy)
{
    float3 half_vector = view + light;
    float no_l = max(dot(normal, light), 0.0f);
    float half_inverse_length =
        vkr_direct_half_vector_inverse_length(no_l, half_vector);
    if (half_inverse_length == 0.0f)
        return float3(0.0f);
    half_vector *= half_inverse_length;
    float3 fresnel = packet_fresnel(max(dot(half_vector, view), 0.0f), f0);
    float distribution =
        vkr_ggx_surface_distribution(normal, half_vector, roughness, energy);
    float3 specular = distribution * vkr_ggx_surface_visibility(no_l, light, energy) *
                      fresnel * energy.specular_scale;
    float3 scale = radiance * no_l;
    return specular * scale;
}

// Rectangle rows are one-sided, unshadowed radiance emitters. One LUT sample
// and one receiver frame serve every row; the cosine integral already has /pi.
VkrVulkanDirectResult packet_rectangle_lights(
    VkrVulkanPacketFrameRoot *frame, float3 world_position, float3 normal,
    float3 view, float3 diffuse_albedo, float metallic, float roughness,
    float3 f0, VkrGgxMaterialEnergy energy, bool diffuse_enabled)
{
    VkrVulkanDirectResult result = (VkrVulkanDirectResult)0;
    VkrVulkanLtc ltc = frame.ltc[0];
    if (ltc.count == 0u)
        return result;

    float2 uv = vkr_ltc_lut_uv(energy.no_v, roughness);
    VkrLtcSample sample;
    sample.inverse_matrix = g_textures[ltc.matrix_texture]
        .SampleLevel(g_samplers[ltc.sampler], uv, 0.0f);
    sample.amplitude_fresnel = g_textures[ltc.amplitude_texture]
        .SampleLevel(g_samplers[ltc.sampler], uv, 0.0f).rg;
    VkrLtcFrame ltc_frame = vkr_ltc_make_frame(normal, view);
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float3 specular_amplitude = vkr_ltc_specular_amplitude(
        sample, energy.no_v, f0, f90, energy.specular_scale);
    VkrAnisotropyLtc anisotropic_sample;
    bool anisotropic = energy.anisotropy_strength > 0.0f;
    if (anisotropic) {
        VkrAnisotropyLookup lookup = vkr_anisotropy_lookup(
            normal, energy.anisotropy_tangent, view, roughness,
            energy.anisotropy_strength);
        float4 table0 = packet_anisotropy_sample(frame.anisotropy[0].table0_texture, frame.anisotropy[0].sampler_index, lookup);
        float4 table1 = packet_anisotropy_sample(frame.anisotropy[0].table1_texture, frame.anisotropy[0].sampler_index, lookup);
        float4 table2 = float4(energy.anisotropy_support_z,
            energy.anisotropy_rectangle_b, energy.directional_dfg);
        anisotropic_sample = vkr_anisotropy_ltc_decode(
            table0, table1, table2, lookup.signs, f0, energy);
        ltc_frame.tangent = energy.anisotropy_tangent;
        ltc_frame.bitangent = energy.anisotropy_bitangent;
    }
    uint light_count = min(ltc.count, 8u);
    for (uint index = 0u; index < light_count; ++index)
    {
        VkrGpuRectangleLightRow row = ltc.lights[index];
        float3 center = row.center_half_width.xyz;
        float3 right = row.right_half_height.xyz * row.center_half_width.w;
        float3 up = row.up_radiance.xyz * row.right_half_height.w;
        // The input boundary proves `right` and `up` are orthonormal.
        if (dot(world_position - center, -cross(right, up)) <= 0.0f)
            continue;
        VkrLtcRectangle local_rectangle = vkr_ltc_project_rectangle_in_frame(
            center - world_position, right, up, ltc_frame);
        float3 specular = anisotropic
            ? vkr_anisotropy_ltc_rectangle(local_rectangle, anisotropic_sample) *
                  anisotropic_sample.amplitude
            : vkr_ltc_evaluate_specular_rectangle(
                local_rectangle, sample.inverse_matrix, specular_amplitude);
        float3 radiance = row.color.rgb * row.up_radiance.w;
        result.specular += radiance * specular;
        if (diffuse_enabled)
        {
            float diffuse_integral = vkr_ltc_integrate_transformed_rectangle(
                local_rectangle.vertex0, local_rectangle.vertex1,
                local_rectangle.vertex2, local_rectangle.vertex3);
            result.irradiance += radiance * diffuse_integral;
            result.diffuse += radiance * diffuse_integral *
                energy.diffuse_weight * diffuse_albedo * (1.0f - metallic);
      if (energy.diffuse_transmission_strength > 0.0f) {
        float3 flip = float3(1.0f, -1.0f, -1.0f);
        float back_integral = vkr_ltc_integrate_transformed_rectangle(
            local_rectangle.vertex0 * flip, local_rectangle.vertex1 * flip,
            local_rectangle.vertex2 * flip, local_rectangle.vertex3 * flip);
        result.diffuse += radiance * back_integral * energy.back_diffuse_weight *
            diffuse_albedo * (1.0f - metallic);
      }
        }
    }
    return result;
}

float3 packet_sheen_rectangle_lights(VkrVulkanPacketFrameRoot *frame,
                                    float3 world_position, float3 normal,
                                    float3 view, VkrSheenLayer sheen,
                                    VkrSheenLtcSample sample)
{
    VkrVulkanLtc ltc = frame.ltc[0];
    if (ltc.count == 0u)
        return 0.0f;
    VkrVulkanSheen tables = frame.sheen[0];
    float2 uv = vkr_sheen_ltc_lut_uv(max(dot(normal, view), 0.0f),
                                      sheen.roughness);
    VkrSheenLtcTransform transform_b = vkr_sheen_ltc_decode_transform(
        g_textures[tables.ltc_matrix_texture_b]
            .SampleLevel(g_samplers[tables.sampler], uv, 0.0f));
    sample.inverse_matrix_b = transform_b.inverse_matrix;
    sample.clipped_mass_b = transform_b.clipped_mass;
    sample.table_weight_b = g_textures[tables.ltc_amplitude_texture_b]
        .SampleLevel(g_samplers[tables.sampler], uv, 0.0f).r;
    sample = vkr_sheen_ltc_prepare_sample(sample, sheen.directional_albedo);
    VkrLtcFrame ltc_frame = vkr_ltc_make_frame(normal, view);
    float3 result = 0.0f;
    uint light_count = min(ltc.count, 8u);
    for (uint index = 0u; index < light_count; ++index)
    {
        VkrGpuRectangleLightRow row = ltc.lights[index];
        float3 center = row.center_half_width.xyz;
        float3 right = row.right_half_height.xyz * row.center_half_width.w;
        float3 up = row.up_radiance.xyz * row.right_half_height.w;
        if (dot(world_position - center, -cross(right, up)) <= 0.0f)
            continue;
        VkrLtcRectangle rectangle = vkr_ltc_project_rectangle_in_frame(
            center - world_position, right, up, ltc_frame);
        result += row.color.rgb * row.up_radiance.w * sheen.color *
                  vkr_sheen_ltc_rectangle(rectangle, sample);
    }
    return result;
}

float packet_specular_ao(float ao, float no_v, float roughness)
{
    return saturate(pow(no_v + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f +
                    ao);
}

VkrGgxMaterialEnergy packet_prepare_brdf(VkrVulkanPacketFrameRoot *frame,
                                         float no_v, float roughness, float3 f0)
{
    float2 dfg = g_textures[frame.dfg_texture]
        .SampleLevel(g_samplers[frame.dfg_sampler],
                     vkr_ggx_dfg_uv(no_v, roughness), 0.0f).rg;
    return vkr_ggx_prepare_material(no_v, roughness, f0, dfg);
}

float4 packet_anisotropy_sample(uint table, uint sampler_index,
                                  VkrAnisotropyLookup lookup) {

  float4 a = g_array_textures[table].SampleLevel(g_samplers[sampler_index], float3(lookup.uv, lookup.layers.x), 0.0f);
  float4 b = g_array_textures[table].SampleLevel(g_samplers[sampler_index], float3(lookup.uv, lookup.layers.y), 0.0f);
  float4 c = g_array_textures[table].SampleLevel(g_samplers[sampler_index], float3(lookup.uv, lookup.layers.z), 0.0f);
  float4 d = g_array_textures[table].SampleLevel(g_samplers[sampler_index], float3(lookup.uv, lookup.layers.w), 0.0f);
  return lerp(lerp(a, b, lookup.blend.x), lerp(c, d, lookup.blend.x), lookup.blend.y);
}

VkrGgxMaterialEnergy packet_prepare_anisotropy(
    VkrVulkanPacketFrameRoot *frame, float3 normal, float3 tangent,
    float3 view, float roughness, float strength, float3 f0) {
  VkrAnisotropyLookup lookup = vkr_anisotropy_lookup(normal, tangent, view,
                                                    roughness, strength);
  float4 table2 = packet_anisotropy_sample(frame.anisotropy[0].table2_texture, frame.anisotropy[0].sampler_index, lookup);
  VkrGgxMaterialEnergy energy = vkr_anisotropy_prepare(
      normal, tangent, view, roughness, strength, f0, table2.zw);
  energy.anisotropy_support_z = table2.x;
  energy.anisotropy_rectangle_b = table2.y;
  return energy;
}

VkrGgxMaterialEnergy packet_prepare_gbuffer_brdf(
    VkrVulkanPacketFrameRoot *frame, float3 normal, float3 view,
    float roughness, float3 f0, float4 anisotropy) {
  if (anisotropy.b == 0.0f)
    return packet_prepare_brdf(frame, max(dot(normal, view), 0.0f), roughness, f0);
  return packet_prepare_anisotropy(frame, normal,
      vkr_anisotropy_decode_axis(anisotropy.rg, normal), view, roughness,
      anisotropy.b, f0);
}

VkrClearcoatLayer packet_prepare_clearcoat(VkrVulkanPacketFrameRoot *frame,
                                           float factor, float roughness,
                                           float3 normal, float3 view)
{
    float3 facing_normal = vkr_clearcoat_view_facing_normal(normal, view);
    float no_v = max(dot(facing_normal, view), 0.0f);
    float2 dfg = g_textures[frame.dfg_texture]
        .SampleLevel(g_samplers[frame.dfg_sampler],
                     vkr_ggx_dfg_uv(no_v, roughness), 0.0f).rg;
    return vkr_clearcoat_prepare_layer(factor, roughness, facing_normal, view, dfg);
}

float3 packet_clearcoat_direct(float3 view, float3 light, float3 radiance,
                                VkrClearcoatLayer coat)
{
    return coat.factor * packet_direct_specular(
        coat.normal, view, light, radiance, coat.roughness,
        float3(VKR_CLEARCOAT_F0), coat.energy);
}

VkrSheenLayer packet_prepare_sheen(VkrVulkanPacketFrameRoot *frame,
                                   float3 color, float roughness,
                                   float3 normal, float3 view,
                                   float2 allocation_reserve)
{
    VkrVulkanSheen tables = frame.sheen[0];
    float no_v = max(dot(normal, view), 0.0f);
    float directional_albedo = g_textures[tables.directional_albedo_texture]
        .SampleLevel(g_samplers[tables.sampler],
                     vkr_sheen_energy_lut_uv(no_v, roughness), 0.0f).r;
    return vkr_sheen_prepare_layer(color, roughness, directional_albedo,
                                   allocation_reserve);
}

VkrSheenLtcSample packet_sheen_ltc_sample(VkrVulkanPacketFrameRoot *frame,
                                          float no_v, float roughness)
{
    VkrVulkanSheen tables = frame.sheen[0];
    float2 uv = vkr_sheen_ltc_lut_uv(no_v, roughness);
    float4 amplitude = g_textures[tables.ltc_amplitude_texture]
        .SampleLevel(g_samplers[tables.sampler], uv, 0.0f);
    VkrSheenLtcSample result;
    VkrSheenLtcTransform transform = vkr_sheen_ltc_decode_transform(
        g_textures[tables.ltc_matrix_texture]
            .SampleLevel(g_samplers[tables.sampler], uv, 0.0f));
    result.inverse_matrix = transform.inverse_matrix;
    result.clipped_mass = transform.clipped_mass;
    result.amplitude = 0.0f;
    result.allocation_reserve = amplitude.gb;
    result.energy_normalization = amplitude.a;
    result.inverse_matrix_b = 0.0f;
    result.clipped_mass_b = 0.0f;
    result.amplitude_b = 0.0f;
    result.table_weight = amplitude.r;
    result.table_weight_b = 0.0f;
    return result;
}

float2 packet_sheen_allocation_reserve(VkrVulkanPacketFrameRoot *frame,
                                       float no_v, float roughness)
{
    VkrVulkanSheen tables = frame.sheen[0];
    return g_textures[tables.ltc_amplitude_texture]
        .SampleLevel(g_samplers[tables.sampler],
                     vkr_sheen_ltc_lut_uv(no_v, roughness), 0.0f)
        .gb;
}

float3 packet_sheen_direct(float3 normal, float3 view, float3 light,
                           float3 radiance, VkrSheenLayer sheen,
                           float energy_normalization)
{
    return vkr_sheen_direct(view, light, normal, radiance, sheen,
                            energy_normalization);
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

struct VkrVkPcssBlockers
{
    float average_depth;
    uint count;
};

float packet_shadow_raw_depth(VkrVulkanPacketFrameRoot *root, uint cascade,
                              float2 uv, float inv_size)
{
    int map_size = int(1.0f / inv_size + 0.5f);
    int2 pixel = int2(floor(uv * float(map_size)));
    pixel = min(max(pixel, int2(0)), int2(map_size - 1));
    return g_depth_array_textures[root.shadow_texture].Load(
        int4(pixel, int(cascade), 0));
}

VkrVkPcssBlockers packet_shadow_pcss_blockers(
    VkrVulkanPacketFrameRoot *root, uint cascade, float2 uv, float reference,
    float2 rotation, float2 search_tap_scale, float inv_size)
{
    VkrVkPcssBlockers result = (VkrVkPcssBlockers)0;
    for (uint i = 0u; i < VKR_SHADOW_PCSS_BLOCKER_COUNT; ++i)
    {
        float2 tap = vkr_shadow_poisson[i];
        float2 rotated = float2(tap.x * rotation.x - tap.y * rotation.y,
                                tap.x * rotation.y + tap.y * rotation.x);
        float blocker_depth = packet_shadow_raw_depth(
            root, cascade, uv + rotated * search_tap_scale, inv_size);
        if (blocker_depth < reference)
        {
            result.average_depth += blocker_depth;
            ++result.count;
        }
    }
    if (result.count != 0u)
        result.average_depth /= float(result.count);
    return result;
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

    float inv_size = data.origin_inv_size_sun.z;
    float2 rotation = packet_shadow_kernel_rotation(
        uv, data.origin_inv_size_sun.xy / texel, 1.0f / inv_size);
    float2 tap_scale = root.shadow_pcf_radius_texels * inv_size;
    uint count = root.shadow_pcf_sample_count;

    float sun_tan_half_angle = data.origin_inv_size_sun.w;
    if (vkr_shadow_pcss_enabled(cascade, sun_tan_half_angle) != 0u)
    {
        // The near PCSS budget includes its no-blocker fallback. Preserve the
        // PCF radius and prefix order there, while bounding a 32-tap request.
        count = min(count, VKR_SHADOW_PCSS_FILTER_MAX_COUNT);
        float search_radius = vkr_shadow_pcss_search_radius_texels(
            reference, depth_span, sun_tan_half_angle, texel,
            root.shadow_pcf_radius_texels);
        VkrVkPcssBlockers blockers = packet_shadow_pcss_blockers(
            root, cascade, uv, reference, rotation, search_radius * inv_size,
            inv_size);
        // Sparse blockers do not prove an unoccluded receiver. With none,
        // retain the configured PCF radius; otherwise widen the bounded PCSS
        // filter.
        if (blockers.count != 0u)
        {
            float filter_radius = vkr_shadow_pcss_filter_radius_texels(
                reference, blockers.average_depth, depth_span,
                sun_tan_half_angle, texel, root.shadow_pcf_radius_texels);
            tap_scale = filter_radius * inv_size;
        }
    }

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

// Froxel injection is budgeted to one comparison. It selects the same cascade
// and bias as surface shading, but intentionally does not spend PCF/PCSS taps.
float packet_directional_shadow_one_tap(VkrVulkanPacketFrameRoot *root,
                                        float3 world_position)
{
    if (root.shadow_cascade_count == 0u)
        return 1.0f;
    float view_depth = abs(mul(root.view, float4(world_position, 1.0f)).z);
    float fade_span = root.shadow_fade_end - root.shadow_fade_start;
    float fade = fade_span > 0.0f
                     ? saturate((root.shadow_fade_end - view_depth) / fade_span)
                     : 1.0f;
    if (fade <= 0.0f)
        return 1.0f;
    uint cascade = 0u;
    for (uint index = 0u; index + 1u < root.shadow_cascade_count; ++index)
    {
        if (view_depth > root.shadow_cascades[index].split_near_far_texel_depth.y)
            cascade = index + 1u;
    }
    VkrVulkanShadowCascade data = root.shadow_cascades[cascade];
    float4 clip = mul(data.light_view_projection, float4(world_position, 1.0f));
    if (abs(clip.w) <= 1e-6f)
        return 1.0f;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || projected.z < 0.0f ||
        projected.z > 1.0f)
        return 1.0f;
    float bias_texels = root.shadow_receiver_bias_texels +
                        root.shadow_slope_bias_texels;
    float reference = projected.z - bias_texels *
        data.split_near_far_texel_depth.z / data.split_near_far_texel_depth.w;
    float visibility = g_depth_array_textures[root.shadow_texture].SampleCmpLevelZero(
        g_comparison_samplers[root.shadow_comparison_sampler],
        float3(uv, float(cascade)), reference);
    return lerp(1.0f, visibility, fade);
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
    if (weight <= 1e-6f)
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

// Gate discrete crossing depths before selecting their cumulative RGB prefix.
float3 packet_local_shadow_transmission(VkrVulkanPacketFrameRoot *frame,
                                        float2 uv, uint layer, float receiver_depth)
{
    if (frame.local_shadow_transmission == nullptr)
        return 1.0f;
    VkrVulkanLocalShadowTransmission maps = frame.local_shadow_transmission[0];
    uint2 pixel = min(uint2(saturate(uv) * float(maps.extent)), maps.extent - 1u);
    int4 address = int4(pixel, layer, 0u);
    float depth0 = g_depth_array_textures[maps.depth0_texture].Load(address);
    float depth1 = g_depth_array_textures[maps.depth1_texture].Load(address);
    float overflow = g_depth_array_textures[maps.overflow_texture].Load(address);
    uint prefix = vkr_local_shadow_transmission_prefix_index(
        receiver_depth, float2(depth0, depth1), overflow);
    if (prefix == 3u)
        return 0.0f;
    if (prefix == 2u)
        return g_array_textures[maps.color1_texture].Load(address).rgb;
    if (prefix == 1u)
        return g_array_textures[maps.color0_texture].Load(address).rgb;
    return 1.0f;
}

// Local perspective depth arrays share the directional comparison sampler.
float3 packet_local_shadow(VkrVulkanPacketFrameRoot *frame,
                          VkrGpuPointLightRow light, uint kind,
                          float3 world_position, float3 normal)
{
    uint encoded_view = uint(light.p3.w);
    if (encoded_view == 0u)
        return 1.0f;
    uint first_view = encoded_view - 1u;
    float3 from_light = world_position - light.p0.xyz;
    uint face = kind == 2u ? 0u : vkr_local_shadow_face(from_light);
    VkrLocalShadowView data = frame.local_shadow_views[first_view + face];
    float forward_depth = dot(from_light, data.light_direction_far.xyz);
    if (forward_depth <= data.light_position_near.w ||
        forward_depth >= data.light_direction_far.w)
        return 1.0f;
    float world_texel = 2.0f * forward_depth * data.projection_params.x *
                        data.projection_params.y;
    float3 receiver = world_position +
        normal * (data.projection_params.w * world_texel) -
        normalize(from_light) * (data.projection_params.z * world_texel);
    if (kind != 2u)
    {
        face = vkr_local_shadow_face(receiver - light.p0.xyz);
        data = frame.local_shadow_views[first_view + face];
    }
    float4 clip = mul(data.light_view_projection, float4(receiver, 1.0f));
    if (clip.w <= 0.0f || clip.z < 0.0f || clip.z > clip.w)
        return 1.0f;
    float2 uv = clip.xy / clip.w * 0.5f + 0.5f;
    if (kind == 2u && (any(uv < 0.0f) || any(uv > 1.0f)))
        return 1.0f;
    float radial_distance = length(receiver - light.p0.xyz);
    float3 visibility = 0.0f;
    [unroll] for (uint tap = 0u; tap < 9u; ++tap)
    {
        float2 tap_uv = uv + vkr_shadow_poisson[tap] *
                            (1.5f * data.projection_params.y);
        float reference = clip.z / clip.w;
        uint tap_view = first_view;
        if (kind == 2u)
        {
            if (any(tap_uv < 0.0f) || any(tap_uv > 1.0f))
            {
                visibility += 1.0f;
                continue;
            }
        }
        else
        {
            float3 ray = vkr_local_shadow_direction(face, tap_uv);
            tap_view += vkr_local_shadow_face(ray);
            float3 tap_world = light.p0.xyz + normalize(ray) * radial_distance;
            float4 tap_clip = mul(frame.local_shadow_views[tap_view]
                                     .light_view_projection,
                                 float4(tap_world, 1.0f));
            tap_uv = tap_clip.xy / tap_clip.w * 0.5f + 0.5f;
            reference = tap_clip.z / tap_clip.w;
            if (reference < 0.0f || reference > 1.0f)
            {
                visibility += 1.0f;
                continue;
            }
        }
        float opaque = g_depth_array_textures[frame.local_shadow_texture]
            .SampleCmpLevelZero(
                g_comparison_samplers[frame.shadow_comparison_sampler],
                float3(tap_uv, float(tap_view)), reference);
        visibility += opaque * packet_local_shadow_transmission(
            frame, tap_uv, tap_view, reference);
    }
    return visibility / 9.0f;
}

float3 packet_local_shadow_one_tap(VkrVulkanPacketFrameRoot *frame,
                                  VkrGpuPointLightRow light, uint kind,
                                  float3 world_position)
{
    uint encoded_view = uint(light.p3.w);
    if (encoded_view == 0u)
        return 1.0f;
    uint first_view = encoded_view - 1u;
    float3 from_light = world_position - light.p0.xyz;
    bool point = kind != 2u;
    uint face = point ? vkr_local_shadow_face(from_light) : 0u;
    VkrLocalShadowView data = frame.local_shadow_views[first_view + face];
    float forward_depth = dot(from_light, data.light_direction_far.xyz);
    if (forward_depth <= data.light_position_near.w ||
        forward_depth >= data.light_direction_far.w)
        return 1.0f;
    float footprint = 2.0f * forward_depth * data.projection_params.x *
                      data.projection_params.y;
    float3 receiver = world_position - normalize(from_light) *
        (data.projection_params.z * footprint);
    if (point)
    {
        face = vkr_local_shadow_face(receiver - light.p0.xyz);
        data = frame.local_shadow_views[first_view + face];
    }
    float4 clip = mul(data.light_view_projection, float4(receiver, 1.0f));
    if (clip.w <= 0.0f || clip.z < 0.0f || clip.z > clip.w)
        return 1.0f;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (!point && (any(uv < 0.0f) || any(uv > 1.0f)))
        return 1.0f;
    float opaque = g_depth_array_textures[frame.local_shadow_texture].SampleCmpLevelZero(
        g_comparison_samplers[frame.shadow_comparison_sampler],
        float3(uv, float(first_view + face)), projected.z);
    return opaque * packet_local_shadow_transmission(
        frame, uv, first_view + face, projected.z);
}

// Every enabled punctual light for one shaded point. `albedo` and `metallic`
// follow packet_direct: the forward path passes base color with its metallic
// factor, the deferred path passes G-buffer diffuse albedo and zero. Both
// spell the same product, so the two paths shade identically.
VkrVulkanDirectResult packet_punctual(VkrVulkanPacketFrameRoot *frame,
                                      float3 world_position, float3 normal,
                                      float3 view, float3 albedo,
                                      float metallic, float roughness,
                                      float3 f0, VkrGgxMaterialEnergy energy)
{
    VkrVulkanDirectResult result;
    result.irradiance = 0.0f;
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
            VkrGpuPointLightRow light = frame.point_light_data[light_index];
            float4 p0 = light.p0;
            float4 p1 = light.p1;
            float4 p2 = light.p2;
            float4 p3 = light.p3;
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
            float3 visibility = packet_local_shadow(frame, light, kind, world_position,
                vkr_ggx_diffuse_shadow_normal(normal, light_direction, energy));
            VkrVulkanDirectResult direct = packet_direct(
                normal, view, light_direction, p1.rgb * p2.x * attenuation * visibility,
                albedo, metallic, roughness, f0, energy);
            result.irradiance += direct.irradiance;
            result.diffuse += direct.diffuse;
            result.specular += direct.specular;
        }
    }
    return result;
}

float3 packet_punctual_specular(VkrVulkanPacketFrameRoot *frame,
                                float3 world_position, float3 normal,
                                float3 view, float roughness, float3 f0, VkrGgxMaterialEnergy energy)
{
    float3 result = 0.0f;
    uint4 point_mask = packet_point_light_mask(frame, world_position);
    uint point_count = min(frame.point_light_count, 128u);
    for (uint word = 0u; word < 4u; ++word)
    {
        uint remaining = WaveActiveBitOr(point_mask[word]);
        while (remaining != 0u)
        {
            uint bit = uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            uint light_index = word * 32u + bit;
            if (light_index >= point_count)
                continue;
            VkrGpuPointLightRow light = frame.point_light_data[light_index];
            float4 p0 = light.p0;
            float4 p1 = light.p1;
            float4 p2 = light.p2;
            float4 p3 = light.p3;
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
            float3 visibility = packet_local_shadow(frame, light, kind, world_position, normal);
            result += packet_direct_specular(
                normal, view, light_direction, p1.rgb * p2.x * attenuation * visibility,
                roughness, f0, energy);
        }
    }
    return result;
}

float3 packet_punctual_sheen(VkrVulkanPacketFrameRoot *frame,
                             float3 world_position, float3 normal,
                             float3 view, VkrSheenLayer sheen,
                             float energy_normalization)
{
    float3 result = 0.0f;
    uint4 point_mask = packet_point_light_mask(frame, world_position);
    uint point_count = min(frame.point_light_count, 128u);
    for (uint word = 0u; word < 4u; ++word)
    {
        uint remaining = WaveActiveBitOr(point_mask[word]);
        while (remaining != 0u)
        {
            uint bit = uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            uint light_index = word * 32u + bit;
            if (light_index >= point_count ||
                (point_mask[word] & (1u << bit)) == 0u)
                continue;
            VkrGpuPointLightRow light = frame.point_light_data[light_index];
            float4 p0 = light.p0;
            float4 p1 = light.p1;
            float4 p2 = light.p2;
            float4 p3 = light.p3;
            uint kind = uint(p2.w + 0.5f);
            float3 to_light = p0.xyz - world_position;
            float distance_squared = dot(to_light, to_light);
            if (kind != 0u && p2.z > 0.0f && distance_squared >= p2.z * p2.z)
                continue;
            float distance = sqrt(distance_squared);
            float3 light_direction =
                distance > 1e-6f ? to_light / distance : float3(0.0f);
            float attenuation;
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
            float3 visibility = packet_local_shadow(frame, light, kind,
                                                world_position, normal);
            result += packet_sheen_direct(
                normal, view, light_direction, p1.rgb * p2.x * attenuation * visibility,
                sheen, energy_normalization);
        }
    }
    return result;
}

// A valid volume cell replaces only diffuse indirect light. The texture's
// lower-corner cell row proves all eight interpolated probes share one room.
bool packet_diffuse_volume_terms(VkrVulkanPacketFrameRoot *frame,
                           float3 world_position, float3 normal,
                           float3 albedo, float metallic, float ao,
                           VkrGgxMaterialEnergy energy, out float3 diffuse, out float3 irradiance)
{
    diffuse = 0.0f;
    irradiance = 0.0f;
    // Descriptor slot zero is the renderer's permanent sentinel, not a volume.
    if (frame.diffuse_volume_texture == 0u)
        return false;
    uint3 dimensions = frame.diffuse_volume_dimensions.xyz;
    VkrDiffuseVolumeCell cell = vkr_diffuse_volume_cell(
        world_position, frame.diffuse_volume_origin.xyz,
        frame.diffuse_volume_inverse_spacing.xyz, dimensions);
    if (!cell.valid ||
        g_textures[frame.diffuse_volume_texture]
            .Load(int3(7, int(cell.row), 0)).y <= 0.0f)
        return false;
    VkrShL2Packed coefficients;
    [unroll] for (uint term = 0u; term < 7u; ++term)
        coefficients.v[term] = 0.0f;
    [unroll] for (uint corner_index = 0u; corner_index < 8u; ++corner_index)
    {
        uint3 corner = vkr_diffuse_volume_corner(corner_index);
        uint node = vkr_diffuse_volume_node_index(cell.base + corner,
                                                   dimensions);
        float weight = vkr_diffuse_volume_weight(corner, cell.fraction);
        [unroll] for (uint term = 0u; term < 7u; ++term)
            coefficients.v[term] +=
                g_textures[frame.diffuse_volume_texture]
                    .Load(int3(int(term), int(node), 0)) * weight;
    }
    float3 response = vkr_sh_l2_evaluate(
        coefficients, vkr_sh_l2_prepare_evaluation(normal));
    irradiance = response * ao;
    diffuse = energy.diffuse_weight * (1.0f - metallic) * albedo * ao *
              response;
    return true;
}

bool packet_diffuse_volume(VkrVulkanPacketFrameRoot *frame,
                           float3 world_position, float3 normal,
                           float3 albedo, float metallic, float ao,
                           VkrGgxMaterialEnergy energy, out float3 diffuse) {
    float3 unused_irradiance;
    return packet_diffuse_volume_terms(frame,world_position,normal,albedo,metallic,ao,energy,diffuse,unused_irradiance);
}

// Global environment plus weighted local probes. `horizon_normal` is the
// geometric normal where one survives; the deferred G-buffer keeps only the
// shading normal and passes it for both.
struct VkrVulkanEnvironmentResult
{
    float3 diffuse;
    float3 specular;
    float3 irradiance;
};

VkrVulkanEnvironmentResult
packet_environment_terms_with_diffuse_normal(
    VkrVulkanPacketFrameRoot *frame, float3 world_position,
    float3 diffuse_normal, float3 normal, float3 horizon_normal, float3 view,
    float3 albedo, float metallic, float roughness, float ao,
    VkrGgxMaterialEnergy energy)
{
    float no_v = max(dot(normal, view), 0.0f);
    roughness = vkr_anisotropy_environment_roughness(roughness, energy);
    float3 reflection = reflect(-view, vkr_anisotropy_environment_normal(normal, view, energy));
    float3 kd = energy.diffuse_weight * (1.0f - metallic);
    // Frame-root indices are uniform. Each local-probe loop iteration also
    // visits one shared table row across the active lanes.
    VkrShL2Evaluation sh_evaluation =
        vkr_sh_l2_prepare_evaluation(diffuse_normal);
    float horizon = saturate(1.0f + dot(reflection, horizon_normal));
    float specular_visibility =
        horizon * horizon * packet_specular_ao(ao, no_v, roughness);
    float3 irradiance = 0.0f;
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
            vkr_sh_l2_evaluate(frame.sh_coefficients[probe.sh_slot],
                               sh_evaluation);
        float3 probe_prefiltered =
            g_cube_textures[probe.prefilter_texture]
                .SampleLevel(
                    g_samplers[probe.prefilter_sampler], probe_reflection,
                    roughness * float(max(frame.prefilter_mip_count, 1u) - 1u))
                .rgb;
        irradiance += probe_irradiance * ao * probe.intensity_box.x * probe.intensity_box.y * weight;
        diffuse += kd * probe_irradiance * albedo * ao * probe.intensity_box.x *
                   probe.intensity_box.y * weight;
        specular += probe_prefiltered * energy.reflectance *
                    specular_visibility * probe.intensity_box.x *
                    probe.intensity_box.z * weight;
    }
    float global_weight = max(1.0f - min(local_weight_sum, 1.0f), 0.0f);
    // Global results are consumed after the local sum, so do not keep them
    // live across the local-probe sampling and SH evaluation loop.
    float3 global_irradiance = vkr_sh_l2_evaluate(
        frame.sh_coefficients[frame.sh_global_slot], sh_evaluation);
    irradiance += global_irradiance * ao * global_weight;
    diffuse += kd * global_irradiance * albedo * ao * global_weight;
    float3 global_prefiltered =
        g_cube_textures[frame.prefilter_texture]
            .SampleLevel(g_samplers[frame.prefilter_sampler], reflection,
                         roughness *
                             float(max(frame.prefilter_mip_count, 1u) - 1u))
            .rgb;
    specular += global_prefiltered * energy.reflectance *
                specular_visibility * global_weight;
    VkrVulkanEnvironmentResult result;
    result.irradiance = irradiance * frame.ibl_controls.y * frame.ibl_controls.x;
    result.diffuse = diffuse * frame.ibl_controls.y * frame.ibl_controls.x;
    result.specular = specular * frame.ibl_controls.z * frame.ibl_controls.x;
    return result;
}

VkrVulkanEnvironmentResult
packet_environment_terms(VkrVulkanPacketFrameRoot *frame, float3 world_position,
                         float3 normal, float3 horizon_normal, float3 view,
                         float3 albedo, float metallic, float roughness,
                         float ao, VkrGgxMaterialEnergy energy)
{
    return packet_environment_terms_with_diffuse_normal(
        frame, world_position, normal, normal, horizon_normal, view, albedo,
        metallic, roughness, ao, energy);
}

float3 packet_environment_specular_radiance(
    VkrVulkanPacketFrameRoot *frame, float3 world_position, float3 normal,
    float3 view, float roughness)
{
    float3 reflection = reflect(-view, normal);
    float3 radiance = 0.0f;
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
        radiance += g_cube_textures[probe.prefilter_texture]
                        .SampleLevel(
                            g_samplers[probe.prefilter_sampler],
                            probe_reflection,
                            roughness * float(max(frame.prefilter_mip_count, 1u) - 1u))
                        .rgb * probe.intensity_box.x * probe.intensity_box.z *
                    weight;
    }
    float global_weight = max(1.0f - min(local_weight_sum, 1.0f), 0.0f);
    radiance += g_cube_textures[frame.prefilter_texture]
                    .SampleLevel(g_samplers[frame.prefilter_sampler], reflection,
                                 roughness *
                                     float(max(frame.prefilter_mip_count, 1u) - 1u))
                    .rgb * global_weight;
    return radiance;
}

float3 packet_environment_specular_weight(float3 normal, float3 horizon_normal,
                                          float3 view, float roughness, float ao,
                                          VkrGgxMaterialEnergy energy)
{
    float no_v = max(dot(normal, view), 0.0f);
    roughness = vkr_anisotropy_environment_roughness(roughness, energy);
    float3 reflection = reflect(-view, vkr_anisotropy_environment_normal(normal, view, energy));
    float horizon = saturate(1.0f + dot(reflection, horizon_normal));
    return energy.reflectance * horizon * horizon *
           packet_specular_ao(ao, no_v, roughness);
}

float3 packet_environment_specular(VkrVulkanPacketFrameRoot *frame,
                                   float3 world_position, float3 normal,
                                   float3 horizon_normal, float3 view,
                                   float roughness, float ao,
                                   VkrGgxMaterialEnergy energy)
{
    return packet_environment_specular_radiance(frame, world_position,
                vkr_anisotropy_environment_normal(normal, view, energy), view,
                vkr_anisotropy_environment_roughness(roughness, energy)) *
           packet_environment_specular_weight(normal, horizon_normal, view,
                                              roughness, ao, energy) *
           frame.ibl_controls.z * frame.ibl_controls.x;
}

float3 packet_sheen_environment(VkrVulkanPacketFrameRoot *frame,
                                float3 world_position, float3 normal,
                                float3 view, float ao, float gtao_specular_cone,
                                VkrSheenLayer sheen)
{
    float no_v = max(dot(normal, view), 0.0f);
    VkrGgxMaterialEnergy approximation = packet_prepare_brdf(
        frame, no_v, sheen.roughness, float3(1.0f));
    float3 incoming = packet_environment_specular_radiance(
        frame, world_position, normal, view, sheen.roughness);
    float3 receiver = packet_environment_specular_weight(
        normal, normal, view, sheen.roughness, ao, approximation);
    return sheen.color * sheen.directional_albedo * incoming * receiver *
           gtao_specular_cone * frame.ibl_controls.z * frame.ibl_controls.x;
}

float3 packet_environment(VkrVulkanPacketFrameRoot *frame,
                          float3 world_position, float3 normal,
                          float3 horizon_normal, float3 view, float3 albedo,
                          float metallic, float roughness, float ao, VkrGgxMaterialEnergy energy)
{
    VkrVulkanEnvironmentResult result =
        packet_environment_terms(frame, world_position, normal, horizon_normal,
                                 view, albedo, metallic, roughness, ao, energy);
    return result.diffuse + result.specular;
}

VkrFroxelSample packet_froxel_sample(VkrVulkanPacketFrameRoot *frame,
                                      float3 world_position)
{
    VkrFroxelSample identity = {float3(0.0f), 1.0f};
    VkrFroxelFogParams params = frame.froxel_fog[0];
    if (!vkr_froxel_enabled(params))
        return identity;
    VkrFroxelHistoryCoordinate coordinate =
        vkr_froxel_integrated_coordinate(params, world_position);
    if (coordinate.valid == 0u)
        return identity;
    float positive_depth = -mul(frame.view, float4(world_position, 1.0f)).z;
    VkrFroxelIntegratedCoordinate z = vkr_froxel_integrated_sample_coordinate(
        params, positive_depth);
    float4 packed = g_3d_textures[frame.froxel_integrated_texture].SampleLevel(
        g_samplers[frame.froxel_sampler], float3(coordinate.uvw.xy, z.z), 0.0f);
    VkrFroxelSample result = {packed.rgb, saturate(packed.w)};
    if (z.near_identity != 0u)
    {
        result.inscatter *= z.near_blend;
        result.transmittance = lerp(1.0f, result.transmittance, z.near_blend);
    }
    return result;
}

float4 packet_world_shade(VkrVulkanVertexOutput input, bool front_facing)
{
    VkrVulkanPacketDrawRoot *draw = vkr_vulkan_packet_draw_root();
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
        float3 bitangent =
            normalize(cross(normal, tangent)) * input.world_tangent.w;
        normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                           normal * sampled.z);
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
    roughness = vkr_ggx_filter_roughness(roughness, variance);
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

  float anisotropy_strength = material.material_anisotropy.x;
  float3 anisotropy_axis = float3(0.0f);
  if (anisotropy_strength > 0.0f) {
    float3 map = float3(1.0f, 0.5f, 1.0f);
    if ((material.flags & 1024u) != 0u)
      map = g_textures[NonUniformResourceIndex(material.anisotropy_texture)].Sample(g_samplers[NonUniformResourceIndex(material.anisotropy_sampler)], input.uv).rgb;
    anisotropy_strength *= map.b;
    anisotropy_axis = vkr_anisotropy_axis(geometric_normal, normal,
        input.world_tangent, map.rg * 2.0f - 1.0f, material.material_anisotropy.yz);
  }
    VkrGgxMaterialEnergy energy = packet_prepare_brdf(frame, no_v, roughness, f0);
  if (anisotropy_strength > 0.0f)
    energy = packet_prepare_anisotropy(frame, normal, anisotropy_axis, view, roughness, anisotropy_strength, f0);
    energy = vkr_ggx_diffuse_transmission(energy, material.material_diffuse_transmission);
    float clearcoat_factor = saturate(material.material_clearcoat.x);
    if (clearcoat_factor > 0.0f && (material.flags & 32u) != 0u)
        clearcoat_factor *= g_textures[NonUniformResourceIndex(material.clearcoat_texture)]
            .Sample(g_samplers[NonUniformResourceIndex(material.clearcoat_sampler)], input.uv).r;
    bool clearcoat_active = vkr_clearcoat_active(clearcoat_factor);
    VkrClearcoatLayer clearcoat;
    if (clearcoat_active)
    {
        float clearcoat_roughness = material.material_clearcoat.y;
        if ((material.flags & 64u) != 0u)
            clearcoat_roughness *= g_textures[NonUniformResourceIndex(material.clearcoat_roughness_texture)]
                .Sample(g_samplers[NonUniformResourceIndex(material.clearcoat_roughness_sampler)], input.uv).g;
        float3 clearcoat_normal = geometric_normal;
        if ((material.flags & 128u) != 0u)
        {
            float3 sampled = vkr_normal_map_decode(
                g_textures[NonUniformResourceIndex(material.clearcoat_normal_texture)]
                    .Sample(g_samplers[NonUniformResourceIndex(material.clearcoat_normal_sampler)], input.uv).xyz,
                material.material_clearcoat.z);
            float3 tangent = normalize(input.world_tangent.xyz);
            tangent = normalize(tangent - dot(tangent, geometric_normal) * geometric_normal);
            float3 bitangent = normalize(cross(geometric_normal, tangent)) * input.world_tangent.w;
            clearcoat_normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                                         geometric_normal * sampled.z);
        }
        clearcoat = packet_prepare_clearcoat(frame, clearcoat_factor, clearcoat_roughness,
                                              clearcoat_normal, view);
    }
    float3 sheen_color = max(material.material_sheen.rgb, 0.0f);
    if (vkr_sheen_active(sheen_color) && (material.flags & 256u) != 0u)
    {
        sheen_color *= g_textures[NonUniformResourceIndex(material.sheen_color_texture)]
            .Sample(g_samplers[NonUniformResourceIndex(material.sheen_color_sampler)],
                    input.uv)
            .rgb;
    }
    bool sheen_active = vkr_sheen_active(sheen_color);
    VkrSheenLayer sheen;
    VkrSheenLtcSample sheen_ltc;
    if (sheen_active)
    {
        float sheen_roughness = material.material_sheen.w;
        if ((material.flags & 512u) != 0u)
            sheen_roughness *= g_textures[NonUniformResourceIndex(
                                    material.sheen_roughness_texture)]
                                   .Sample(g_samplers[NonUniformResourceIndex(
                                                material.sheen_roughness_sampler)],
                                           input.uv)
                                   .a;
        sheen_ltc = packet_sheen_ltc_sample(frame, no_v, sheen_roughness);
        sheen = packet_prepare_sheen(frame, sheen_color, sheen_roughness,
                                     normal, view,
                                     sheen_ltc.allocation_reserve);
    }
    float base_transmission =
        (clearcoat_active ? clearcoat.base_transmission : 1.0f) *
        (sheen_active ? sheen.base_transmission : 1.0f);
    float sheen_transmission =
        clearcoat_active ? clearcoat.base_transmission : 1.0f;
    float3 analytic_diffuse = 0.0f;
    float3 analytic_specular = 0.0f;
    float3 clearcoat_direct = 0.0f;
    float3 sheen_direct = 0.0f;
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        float3 sun_direction = normalize(-frame.directional_direction_enabled.xyz);
        bool back_lit = energy.diffuse_transmission_strength > 0.0f &&
                        dot(normal, sun_direction) < 0.0f;
        float base_shadow = packet_directional_shadow(
            frame, input.world_position, back_lit ? -normal : normal);
        float directional_shadow = base_shadow;
        if (back_lit && clearcoat_active)
            directional_shadow = packet_directional_shadow(frame, input.world_position, normal);
        VkrVulkanDirectResult direct = packet_direct(
            normal, view, normalize(-frame.directional_direction_enabled.xyz),
            frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * base_shadow,
            base.rgb, metallic, roughness, f0, energy);
        analytic_diffuse = direct.diffuse;
        analytic_specular = direct.specular;
        if (clearcoat_active)
            clearcoat_direct += packet_clearcoat_direct(
                view, normalize(-frame.directional_direction_enabled.xyz),
                frame.directional_color_intensity.rgb * frame.directional_color_intensity.w * directional_shadow,
                clearcoat);
        if (sheen_active)
            sheen_direct += packet_sheen_direct(
                normal, view, normalize(-frame.directional_direction_enabled.xyz),
                frame.directional_color_intensity.rgb *
                    frame.directional_color_intensity.w * directional_shadow,
                sheen, sheen_ltc.energy_normalization);
    }
    VkrVulkanDirectResult punctual =
        packet_punctual(frame, input.world_position, normal, view, base.rgb,
                        metallic, roughness, f0, energy);
    analytic_diffuse += punctual.diffuse;
    analytic_specular += punctual.specular;
    if (clearcoat_active)
    {
        VkrVulkanDirectResult coat_punctual = packet_punctual(
            frame, input.world_position, clearcoat.normal, view, float3(0.0f),
            0.0f, clearcoat.roughness, float3(VKR_CLEARCOAT_F0), clearcoat.energy);
        clearcoat_direct += coat_punctual.specular * clearcoat.factor;
    }
    if (sheen_active)
        sheen_direct += packet_punctual_sheen(
            frame, input.world_position, normal, view, sheen,
            sheen_ltc.energy_normalization);
    VkrVulkanDirectResult rectangles = packet_rectangle_lights(
        frame, input.world_position, normal, view, base.rgb, metallic,
        roughness, f0, energy, true);
    analytic_diffuse += rectangles.diffuse;
    analytic_specular += rectangles.specular;
    if (clearcoat_active)
    {
        VkrVulkanDirectResult coat_rectangles = packet_rectangle_lights(
            frame, input.world_position, clearcoat.normal, view, float3(0.0f),
            0.0f, clearcoat.roughness, float3(VKR_CLEARCOAT_F0), clearcoat.energy, false);
        clearcoat_direct += coat_rectangles.specular * clearcoat.factor;
    }
    if (sheen_active)
        sheen_direct += packet_sheen_rectangle_lights(
            frame, input.world_position, normal, view, sheen, sheen_ltc);
    float3 indirect_diffuse;
    float3 indirect_specular = 0.0f;
    float3 clearcoat_indirect = 0.0f;
    float3 sheen_indirect = 0.0f;
    if ((frame.flags & 2u) != 0u)
    {
        VkrVulkanEnvironmentResult environment = packet_environment_terms(
            frame, input.world_position, normal, geometric_normal, view,
            base.rgb, metallic, roughness, ao, energy);
        indirect_diffuse = environment.diffuse;
        indirect_specular = environment.specular;
        if (clearcoat_active)
            clearcoat_indirect = clearcoat.factor * packet_environment_specular(
                frame, input.world_position, clearcoat.normal, clearcoat.normal, view,
                clearcoat.roughness, ao, clearcoat.energy);
        if (sheen_active)
            sheen_indirect = packet_sheen_environment(
                frame, input.world_position, normal, view, ao, 1.0f, sheen);
    }
    else
    {
        indirect_diffuse = frame.ambient_color.rgb * base.rgb * ao *
                           (1.0f - metallic) * energy.diffuse_weight;
    }
    float3 volume_diffuse;
    if (packet_diffuse_volume(frame, input.world_position, normal, base.rgb,
                              metallic, ao, energy, volume_diffuse))
        indirect_diffuse = volume_diffuse;
    analytic_diffuse *= base_transmission;
    analytic_specular *= base_transmission;
    indirect_diffuse *= base_transmission;
    indirect_specular *= base_transmission;
    emissive *= base_transmission;
    sheen_direct *= sheen_transmission;
    sheen_indirect *= sheen_transmission;
    float3 color = analytic_diffuse + analytic_specular + clearcoat_direct +
                   sheen_direct + indirect_diffuse + indirect_specular +
                   clearcoat_indirect + sheen_indirect;
    if (frame.render_mode == 1u)
        return float4(analytic_diffuse + analytic_specular + clearcoat_direct +
                          sheen_direct,
                      1.0f);
    if (frame.render_mode == 4u)
        return float4(analytic_diffuse, 1.0f);
    if (frame.render_mode == 5u)
        return float4(analytic_specular + clearcoat_direct + sheen_direct, 1.0f);
    color += emissive;

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
    VkrVulkanPacketDrawRoot *draw = vkr_vulkan_packet_draw_root();
    VkrFroxelFogParams froxel_params = draw.frame.froxel_fog[0];
    if (vkr_froxel_enabled(froxel_params))
    {
        VkrFroxelSample froxel = packet_froxel_sample(
            draw.frame, input.world_position);
        output.color.rgb = vkr_froxel_apply_integrated(
            output.color.rgb, froxel).inscatter;
    }
    else if (draw.frame.fog[0].color_density.w > 0.0f)
    {
        output.color.rgb = vkr_fog_apply_surface(
            output.color.rgb, draw.frame.fog[0], draw.frame.view_position.xyz,
            input.world_position);
    }

    const uint overlay_bit = 0x80000000u;
    const uint generation_mask = 0x7fffffffu;
    const uint surface_mask = 0x1ffffu;
    uint surface_token = input.temporal_flags >> 1u;
    bool identity_valid =
        input.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY &&
        input.temporal_generation > 0u &&
        input.temporal_generation <= generation_mask && surface_token > 0u &&
        surface_token <= surface_mask;
    output.surface = uint2(overlay_bit, 0u);
    if (identity_valid)
    {
        output.surface.x = overlay_bit | input.temporal_generation;
        output.surface.y =
            (input.temporal_index << 17u) | surface_token;
    }
    output.motion = 0.0f;
    output.validity = 0.0f;

    VkrVulkanPacketTemporalDrawState *temporal =
        draw.frame.temporal_draw_state;
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
                    draw.visible_rows[draw.visible_row_index];
                float reactivity = saturate(
                    draw.frame.materials[visible.material_index]
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

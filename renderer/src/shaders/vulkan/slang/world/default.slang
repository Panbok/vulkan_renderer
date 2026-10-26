bool packet_is_orthographic(VkrVulkanPacketFrameRoot *frame)
{
    float3 clip_w = frame.view_projection[3].xyz;
    return dot(clip_w, clip_w) < 1e-12f;
}

float3 packet_view_direction(VkrVulkanPacketFrameRoot *frame,
                              float3 world_position)
{
    if (packet_is_orthographic(frame))
    {
        return normalize(frame.view[2].xyz);
    }
    return normalize(frame.view_position.xyz - world_position);
}

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
    float v_o_h = max(dot(half_vector, view), 0.0f);
    float3 fresnel =
        vkr_ggx_f82_fresnel(packet_fresnel(v_o_h, f0), v_o_h, energy);
    float3 specular = vkr_ggx_direct_specular(normal, half_vector, light, no_l,
                                              roughness, energy) *
                      fresnel;
    float3 kd = energy.diffuse_weight * (1.0f - metallic);
    float3 scale = radiance * no_l *
                   vkr_smooth_terminator(no_l, dot(normal, half_vector), v_o_h,
                                         energy);
    VkrVulkanDirectResult result;
    result.irradiance = scale / vkr_pi;
    result.diffuse = kd *
                     vkr_eon_direct_albedo(albedo, no_l, dot(light, view),
                                           energy) /
                     vkr_pi * scale;
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
    float v_o_h = max(dot(half_vector, view), 0.0f);
    float3 fresnel =
        vkr_ggx_f82_fresnel(packet_fresnel(v_o_h, f0), v_o_h, energy);
    float3 specular = vkr_ggx_direct_specular(normal, half_vector, light, no_l,
                                              roughness, energy) *
                      fresnel;
    float3 scale = radiance * no_l *
                   vkr_smooth_terminator(no_l, dot(normal, half_vector), v_o_h,
                                         energy);
    return specular * scale;
}

VkrSheenLtcSample packet_prepare_sheen_rectangle(
    VkrVulkanPacketFrameRoot *frame, float3 normal, float3 view,
    VkrSheenLayer sheen, VkrSheenLtcSample sample)
{
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
    return sample;
}

// Rectangle rows are one-sided, unshadowed radiance emitters. One LUT sample
// and one receiver frame serve every row; the cosine integral already has /pi.
VkrVulkanDirectResult packet_layered_rectangle_lights(
    VkrVulkanPacketFrameRoot *frame, float3 world_position, float3 normal,
    float3 view, float3 diffuse_albedo, float metallic, float roughness,
    float3 f0, VkrGgxMaterialEnergy energy, bool diffuse_enabled,
    bool coat_active, VkrClearcoatLayer coat,
    bool sheen_active, VkrSheenLayer sheen, VkrSheenLtcSample sheen_sample,
    inout float3 coat_result, inout float3 sheen_result)
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
    float3 specular_amplitude = vkr_ggx_ltc_f82_amplitude(
        vkr_ltc_specular_amplitude(sample, energy.no_v, f0, f90,
                                   energy.specular_scale),
        sample.amplitude_fresnel.x, energy.f82_directional,
        energy.specular_scale, energy);
    // The dual lobe (ADR-113) takes its own LTC sample at its roughness.
    VkrLtcSample dual_sample = (VkrLtcSample)0;
    float3 dual_amplitude = 0.0f;
    bool dual = energy.dual_opacity > 0.0f;
    if (dual)
    {
        float2 dual_uv = vkr_ltc_lut_uv(energy.no_v, energy.dual_roughness);
        dual_sample.inverse_matrix = g_textures[ltc.matrix_texture]
            .SampleLevel(g_samplers[ltc.sampler], dual_uv, 0.0f);
        dual_sample.amplitude_fresnel = g_textures[ltc.amplitude_texture]
            .SampleLevel(g_samplers[ltc.sampler], dual_uv, 0.0f).rg;
        dual_amplitude = vkr_ggx_ltc_f82_amplitude(
            vkr_ltc_specular_amplitude(dual_sample, energy.no_v, f0, f90,
                                       energy.dual_specular_scale),
            dual_sample.amplitude_fresnel.x, energy.dual_f82_directional,
            energy.dual_specular_scale, energy);
    }
    VkrAnisotropyLtc anisotropic_sample = (VkrAnisotropyLtc)0;
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
    VkrLtcSample coat_sample = (VkrLtcSample)0;
    VkrLtcFrame coat_frame = (VkrLtcFrame)0;
    float3 coat_amplitude = 0.0f;
    if (coat_active)
    {
        float2 coat_uv = vkr_ltc_lut_uv(coat.energy.no_v, coat.roughness);
        coat_sample.inverse_matrix = g_textures[ltc.matrix_texture]
            .SampleLevel(g_samplers[ltc.sampler], coat_uv, 0.0f);
        coat_sample.amplitude_fresnel = g_textures[ltc.amplitude_texture]
            .SampleLevel(g_samplers[ltc.sampler], coat_uv, 0.0f).rg;
        coat_frame = vkr_ltc_make_frame(coat.normal, view);
        coat_amplitude = vkr_ltc_specular_amplitude(
            coat_sample, coat.energy.no_v, float3(VKR_CLEARCOAT_F0),
            saturate(VKR_CLEARCOAT_F0 * 25.0f), coat.energy.specular_scale);
    }
    VkrLtcFrame sheen_frame = (VkrLtcFrame)0;
    if (sheen_active)
    {
        sheen_sample = packet_prepare_sheen_rectangle(
            frame, normal, view, sheen, sheen_sample);
        sheen_frame = vkr_ltc_make_frame(normal, view);
    }
    float3 coat_sum = 0.0f;
    float3 sheen_sum = 0.0f;
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
        if (dual)
        {
            // The isotropic dual lobe integrates in the view-aligned frame,
            // which an anisotropic primary has rotated.
            VkrLtcRectangle dual_rectangle = anisotropic
                ? vkr_ltc_project_rectangle_in_frame(
                      center - world_position, right, up,
                      vkr_ltc_make_frame(normal, view))
                : local_rectangle;
            float3 second = vkr_ltc_evaluate_specular_rectangle(
                dual_rectangle, dual_sample.inverse_matrix, dual_amplitude);
            specular += (second - specular) * energy.dual_opacity;
        }
        float3 radiance = row.color.rgb * row.up_radiance.w;
        result.specular += radiance * specular;
        if (coat_active)
        {
            VkrLtcRectangle coat_rectangle = vkr_ltc_project_rectangle_in_frame(
                center - world_position, right, up, coat_frame);
            coat_sum += radiance * vkr_ltc_evaluate_specular_rectangle(
                coat_rectangle, coat_sample.inverse_matrix, coat_amplitude);
        }
        if (sheen_active)
        {
            // An anisotropic base rotates its LTC frame; sheen retains its own.
            VkrLtcRectangle sheen_rectangle = vkr_ltc_project_rectangle_in_frame(
                center - world_position, right, up, sheen_frame);
            sheen_sum += radiance * sheen.color *
                vkr_sheen_ltc_rectangle(sheen_rectangle, sheen_sample);
        }
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
    if (coat_active)
    {
        coat_result += coat_sum * coat.factor;
    }
    if (sheen_active)
    {
        sheen_result += sheen_sum;
    }
    return result;
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

VkrBrdfExtension packet_material_brdf_extension(VkrVulkanMaterial material)
{
    return vkr_brdf_extension_unpack(material.material_brdf,
                                     material.material_specular_tint);
}

// The DFG table's A, B and C terms for one lobe.
float3 packet_dfg_sample(VkrVulkanPacketFrameRoot *frame, float no_v,
                         float roughness)
{
    return g_textures[frame.dfg_texture]
        .SampleLevel(g_samplers[frame.dfg_sampler],
                     vkr_ggx_dfg_uv(no_v, roughness), 0.0f).rgb;
}

// ADR-113 specular extensions, after the base (and anisotropic) energy and
// before the diffuse-transmission split. Each adds its table samples only
// when the material enables it.
VkrGgxMaterialEnergy packet_apply_specular_extension(
    VkrVulkanPacketFrameRoot *frame, VkrGgxMaterialEnergy energy,
    VkrBrdfExtension extension, float roughness, float3 f0)
{
    if (vkr_brdf_extension_f82_active(extension))
        energy = vkr_ggx_apply_f82(
            energy, f0, extension.specular_f82_tint,
            packet_dfg_sample(
                frame, energy.no_v,
                vkr_anisotropy_environment_roughness(roughness, energy)));
    if (vkr_brdf_extension_dual_active(extension))
    {
        float dual_roughness = vkr_brdf_dual_roughness(roughness, extension);
        energy = vkr_ggx_apply_dual_specular(
            energy, f0, extension.dual_specular_opacity, dual_roughness,
            packet_dfg_sample(frame, energy.no_v, dual_roughness));
    }
    return energy;
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
    return vkr_sheen_prepare_layer(color, roughness, no_v, directional_albedo,
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

/**
 * Accumulates rotated taps `[first, count)` of the shared progressive table.
 * Each tap compares against the receiver plane's depth at its offset
 * (`depth_gradient`).
 *
 * Frame-root descriptor indices are dynamically uniform: every invocation reads
 * the same push-constant-addressed root. Decorating them NonUniform costs a
 * waterfall loop and blocks scalarization for nothing.
 */
float packet_shadow_kernel(VkrVulkanPacketFrameRoot *root, uint cascade,
                           float2 uv, float reference, float2 depth_gradient,
                           float2 rotation, float2 tap_scale, uint first,
                           uint count)
{
    float sum = 0.0f;
    for (uint i = first; i < count; ++i)
    {
        float2 offset =
            vkr_local_shadow_rotate_tap(vkr_shadow_poisson[i], rotation) *
            tap_scale;
        // A comparison sampler makes every tap a hardware bilinear 2x2 PCF, so
        // a distant cascade degrades into softness instead of into crawling
        // staircase noise.
        sum += g_depth_array_textures[root.shadow_texture].SampleCmpLevelZero(
            g_comparison_samplers[root.shadow_comparison_sampler],
            float3(uv + offset, float(cascade)),
            reference + dot(depth_gradient, offset));
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

// Blockers are stored depths in front of the receiver plane at each tap, so a
// sloped receiver does not count itself.
VkrVkPcssBlockers packet_shadow_pcss_blockers(
    VkrVulkanPacketFrameRoot *root, uint cascade, float2 uv, float reference,
    float2 depth_gradient, float2 rotation, float2 search_tap_scale,
    float inv_size)
{
    VkrVkPcssBlockers result = (VkrVkPcssBlockers)0;
    for (uint i = 0u; i < VKR_SHADOW_PCSS_BLOCKER_COUNT; ++i)
    {
        float2 offset =
            vkr_local_shadow_rotate_tap(vkr_shadow_poisson[i], rotation) *
            search_tap_scale;
        float blocker_depth =
            packet_shadow_raw_depth(root, cascade, uv + offset, inv_size);
        if (blocker_depth < reference + dot(depth_gradient, offset))
        {
            result.average_depth += blocker_depth;
            ++result.count;
        }
    }
    if (result.count != 0u)
        result.average_depth /= float(result.count);
    return result;
}

// No far-cascade EVSM moments: every cascade filters its depth with PCF.
#define VKR_VK_SHADOW_MOMENTS_NONE 0xFFFFFFFFu
// packet_shadow_cascade's result for a receiver its cascade's map misses.
#define VKR_VK_SHADOW_MISSED -1.0f

/**
 * One cascade's visibility, or VKR_VK_SHADOW_MISSED where its map misses the
 * receiver. `rotation` turns the kernel per pixel (vkr_shadow_kernel_rotation).
 * Cascades from VKR_SHADOW_EVSM_FIRST_CASCADE below `moments_cascade_end`
 * sample `moments_texture`; the far level has no moments and filters with PCF.
 */
float packet_shadow_cascade(VkrVulkanPacketFrameRoot *root, uint cascade,
                            float3 world_position, float3 normal, float n_dot_l,
                            float2 rotation,
                            uint moments_texture = VKR_VK_SHADOW_MOMENTS_NONE,
                            uint moments_sampler = 0u,
                            uint moments_cascade_end = 0u)
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
        return VKR_VK_SHADOW_MISSED;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return VKR_VK_SHADOW_MISSED;
    // Nothing the map holds lies nearer the light than its near plane; a
    // receiver past its far plane lies behind every caster stored there, as
    // valley floors below a far level's casters do.
    if (projected.z < 0.0f)
        return 1.0f;
    projected.z = min(projected.z, 1.0f);

    // Both biases are configured in texels and converted through *this*
    // cascade's own footprint and fitted depth span. That is what makes one
    // configured value mean the same world distance in cascade 0 and cascade 3,
    // whose depth ranges differ by orders of magnitude.
    float slope = saturate(1.0f - n_dot_l);
    float bias_texels = root.shadow_receiver_bias_texels +
                        root.shadow_slope_bias_texels * slope;
    float reference = projected.z - bias_texels * texel / depth_span;

    // Filtered far cascades take one bilinear moments fetch instead of PCF.
    if (moments_texture != VKR_VK_SHADOW_MOMENTS_NONE &&
        cascade >= VKR_SHADOW_EVSM_FIRST_CASCADE &&
        cascade < moments_cascade_end)
    {
        float4 moments = g_array_textures[moments_texture].SampleLevel(
            g_samplers[moments_sampler],
            float3(uv, float(cascade - VKR_SHADOW_EVSM_FIRST_CASCADE)), 0.0f);
        return vkr_shadow_evsm_visibility(moments, reference);
    }

    float inv_size = data.origin_inv_size_sun.z;
    float4x4 light = data.light_view_projection;
    float2 depth_gradient = vkr_shadow_receiver_plane_gradient(
        normal, normalize(light[0].xyz), normalize(light[1].xyz),
        normalize(light[2].xyz), texel / (inv_size * depth_span));
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
            root, cascade, uv, reference, depth_gradient, rotation,
            search_radius * inv_size, inv_size);
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
        float probe =
            packet_shadow_kernel(root, cascade, uv, reference, depth_gradient,
                                 rotation, tap_scale, 0u, 9u);
        if (probe <= 0.0f || probe >= 9.0f)
            return probe * (1.0f / 9.0f);
        return (probe + packet_shadow_kernel(root, cascade, uv, reference,
                                             depth_gradient, rotation,
                                             tap_scale, 9u, count)) /
               float(count);
    }
    return packet_shadow_kernel(root, cascade, uv, reference, depth_gradient,
                                rotation, tap_scale, 0u, count) /
           float(count);
}

float packet_directional_shadow(VkrVulkanPacketFrameRoot *root,
                                float3 world_position, float3 normal,
                                float2 rotation,
                                uint moments_texture = VKR_VK_SHADOW_MOMENTS_NONE,
                                uint moments_sampler = 0u,
                                uint moments_cascade_end = 0u)
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
    float factor = packet_shadow_cascade(root, cascade, world_position, normal,
                                         n_dot_l, rotation, moments_texture,
                                         moments_sampler, moments_cascade_end);
    // A receiver its cascade's map misses, pushed past an edge by the normal
    // offset, takes the next cascade's map, which contains it, rather than
    // full light.
    if (factor < 0.0f && cascade + 1u < root.shadow_cascade_count)
    {
        cascade += 1u;
        factor = packet_shadow_cascade(root, cascade, world_position, normal,
                                       n_dot_l, rotation, moments_texture,
                                       moments_sampler, moments_cascade_end);
    }
    if (factor < 0.0f)
        factor = 1.0f;

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
                                               world_position, normal, n_dot_l,
                                               rotation, moments_texture,
                                               moments_sampler,
                                               moments_cascade_end);
            // A next map that misses the receiver keeps this one's value.
            if (next >= 0.0f)
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
    // The debug readout keeps one fixed kernel orientation.
    sample.factor = packet_shadow_cascade(root, sample.cascade, world_position,
                                          normal, n_dot_l, float2(1.0f, 0.0f));
    if (sample.factor < 0.0f && sample.cascade + 1u < root.shadow_cascade_count)
    {
        sample.cascade += 1u;
        sample.factor = packet_shadow_cascade(root, sample.cascade,
                                              world_position, normal, n_dot_l,
                                              float2(1.0f, 0.0f));
    }
    if (sample.factor < 0.0f)
        sample.factor = 1.0f;
    if (sample.cascade + 1u < root.shadow_cascade_count)
    {
        float4 slice =
            root.shadow_cascades[sample.cascade].split_near_far_texel_depth;
        float band = (slice.y - slice.x) * root.shadow_cascade_blend_fraction;
        if (band > 0.0f && view_depth > slice.y - band)
        {
            float t = saturate((view_depth - (slice.y - band)) / band);
            float next = packet_shadow_cascade(
                root, sample.cascade + 1u, world_position, normal, n_dot_l,
                float2(1.0f, 0.0f));
            if (next >= 0.0f)
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
    return vkr_editor_shadow_debug_color(mode, sample.cascade, sample.factor,
                                         sample.map_depth, sample.valid != 0u);
}

// Where a draw takes its baked diffuse light and its reflection probes: its
// mover's lighting origin (ADR-120), else the shaded point. A mover's draws
// keep their own normals, so only the sampled position is shared.
float3 packet_lighting_position(VkrVulkanPacketFrameRoot *frame,
                                uint instance_index, float3 world_position)
{
    float4 origin = frame.instances[instance_index].lighting_origin;
    return origin.w != 0.0f ? origin.xyz : world_position;
}

float packet_probe_influence(VkrVulkanIblProbe probe, float3 world_position)
{
    return max(probe.extents_weight.w, 0.0f) *
           vkr_reflection_probe_influence(world_position,
                                          probe.center_blend.xyz,
                                          max(probe.extents_weight.xyz, 0.0f),
                                          probe.center_blend.w);
}

// The factor on probe row `index`'s brightness for the dynamic layer's
// light at its capture origin (ADR-119); one when no trace relit the rows.
float packet_probe_relight(VkrVulkanPacketFrameRoot *frame, uint index)
{
    return frame.probe_relight != nullptr ? frame.probe_relight[index] : 1.0f;
}

// The cube direction of `dominant`, the lobe's dominant reflection, in one
// probe: box-projected unless the probe turns that off.
float3 packet_probe_direction(VkrVulkanIblProbe probe, float3 dominant,
                              float3 world_position, float alpha)
{
    return probe.intensity_box.w > 0.5f
               ? vkr_reflection_probe_direction(
                     dominant, world_position, probe.center_blend.xyz,
                     max(probe.extents_weight.xyz, 0.0f),
                     probe.capture_origin.xyz, alpha)
               : dominant;
}

// Gate discrete crossing depths before selecting their cumulative RGB prefix.
// `encoded_layer` is the face's transmission layer plus one; zero leaves
// refractive casters unshadowed for the face.
float3 packet_local_shadow_transmission(VkrVulkanPacketFrameRoot *frame,
                                        float2 uv, uint encoded_layer,
                                        float receiver_depth)
{
    if (encoded_layer == 0u)
        return 1.0f;
    // The host names a record every frame; a frame without transmission
    // layers gives it extent zero.
    VkrVulkanLocalShadowTransmission maps = frame.local_shadow_transmission[0];
    if (maps.extent == 0u)
        return 1.0f;
    uint2 pixel = min(uint2(saturate(uv) * float(maps.extent)), maps.extent - 1u);
    int4 address = int4(pixel, encoded_layer - 1u, 0u);
    // Crossings are peeled in order from the light, so a receiver in front of
    // the first one has no transmission prefix.
    float depth0 = g_depth_array_textures[maps.depth0_texture].Load(address);
    if (receiver_depth <= depth0)
        return 1.0f;
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

// Average forward distance of the blockers a contact-hardening search finds
// around `uv`, or zero when the sparse search finds none. Point-light taps
// compare against the receiver's radial distance like the filter taps.
float packet_local_shadow_blocker_forward(
    VkrVulkanPacketFrameRoot *frame, VkrLocalShadowView data, uint kind,
    float2 uv, float center_reference, float4 face_clip_terms,
    float radial_distance, float2 tap_rotation, float search_texels,
    uint blocker_count)
{
    float depth_sum = 0.0f;
    uint found = 0u;
    for (uint tap = 0u; tap < blocker_count; ++tap)
    {
        float2 tap_uv = saturate(uv +
            vkr_local_shadow_rotate_tap(vkr_shadow_poisson[tap],
                                        tap_rotation) *
                (search_texels * data.projection_params.y));
        float reference = kind == 2u
            ? center_reference
            : vkr_local_shadow_in_face_depth(face_clip_terms, radial_distance,
                                             tap_uv);
        float2 texel = vkr_local_shadow_atlas_texel(
            data.atlas_rect, data.projection_params.y, tap_uv);
        float depth = g_depth_array_textures[frame.local_shadow_texture].Load(
            int4(int2(texel), int(data.atlas_rect.w), 0));
        if (depth < reference)
        {
            depth_sum += depth;
            ++found;
        }
    }
    if (found == 0u)
        return 0.0f;
    return max(vkr_local_shadow_forward_distance(face_clip_terms,
                                                 depth_sum / float(found)),
               data.light_position_near.w);
}

// Local perspective depth arrays share the directional comparison sampler.
// Fully filtered lights take `tap_count` Poisson taps rotated by
// `tap_rotation`. A nonzero `blocker_count` widens their disk by the light's
// source radius from that many blocker-search taps (contact-hardening); an
// empty search keeps the fixed radius, since sparse taps do not prove the
// receiver is lit.
float3 packet_local_shadow(VkrVulkanPacketFrameRoot *frame,
                          VkrGpuPointLightRow light, uint kind,
                          float3 world_position, float3 normal,
                          float2 tap_rotation = float2(1.0f, 0.0f),
                          uint tap_count = VKR_LOCAL_SHADOW_FILTER_TAP_COUNT,
                          uint blocker_count = 0u)
{
    uint encoded_view = uint(light.p3.w);
    if (encoded_view == 0u)
        return 1.0f;
    uint first_view = encoded_view - 1u;
    float strength = frame.local_shadow_views[first_view].shadow_params.x;
    if (strength <= 0.0f)
        return 1.0f;
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
    if (frame.local_shadow_views[first_view].shadow_params.z > 0.5f)
    {
        // Lights past the most important few take one hardware-filtered tap.
        float reference = clip.z / clip.w;
        float opaque = g_depth_array_textures[frame.local_shadow_texture]
            .SampleCmpLevelZero(
                g_comparison_samplers[frame.shadow_comparison_sampler],
                float3(vkr_local_shadow_atlas_uv(data.atlas_rect,
                                                 data.projection_params.y, uv),
                       data.atlas_rect.w),
                reference);
        float3 transmission = opaque > 0.0f
            ? packet_local_shadow_transmission(
                  frame, uv, uint(data.shadow_params.y), reference)
            : 1.0f;
        return vkr_local_shadow_apply_strength(opaque * transmission, strength);
    }
    float radial_distance = length(receiver - light.p0.xyz);
    float4 origin_clip =
        mul(data.light_view_projection, float4(light.p0.xyz, 1.0f));
    float4 axis_clip = mul(data.light_view_projection,
                           float4(data.light_direction_far.xyz, 0.0f));
    float4 face_clip_terms =
        float4(origin_clip.z, axis_clip.z, origin_clip.w, axis_clip.w);
    float filter_radius_texels = VKR_LOCAL_SHADOW_FILTER_RADIUS_TEXELS;
    float source_radius_uv = data.shadow_params.w;
    if (blocker_count != 0u && source_radius_uv > 0.0f)
    {
        float receiver_forward =
            dot(receiver - light.p0.xyz, data.light_direction_far.xyz);
        float search_texels = vkr_local_shadow_pcss_search_radius_texels(
            source_radius_uv, data.light_position_near.w, receiver_forward,
            data.projection_params.y);
        float blocker_forward = packet_local_shadow_blocker_forward(
            frame, data, kind, uv, clip.z / clip.w, face_clip_terms,
            radial_distance, tap_rotation, search_texels, blocker_count);
        if (blocker_forward > 0.0f)
            filter_radius_texels = vkr_local_shadow_pcss_filter_radius_texels(
                source_radius_uv, blocker_forward, receiver_forward,
                data.projection_params.y);
    }
    float3 visibility = 0.0f;
    [unroll] for (uint tap = 0u; tap < tap_count; ++tap)
    {
        float2 tap_uv = uv +
            vkr_local_shadow_rotate_tap(vkr_shadow_poisson[tap],
                                        tap_rotation) *
                (filter_radius_texels * data.projection_params.y);
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
        else if (all(tap_uv >= 0.0f) && all(tap_uv <= 1.0f))
        {
            // Most taps stay in the centre face; only their depth changes.
            tap_view += face;
            reference = vkr_local_shadow_in_face_depth(
                face_clip_terms, radial_distance, tap_uv);
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
        float4 tap_rect = frame.local_shadow_views[tap_view].atlas_rect;
        float tap_texel =
            frame.local_shadow_views[tap_view].projection_params.y;
        float opaque = g_depth_array_textures[frame.local_shadow_texture]
            .SampleCmpLevelZero(
                g_comparison_samplers[frame.shadow_comparison_sampler],
                float3(vkr_local_shadow_atlas_uv(tap_rect, tap_texel, tap_uv),
                       tap_rect.w),
                reference);
        // An opaque-occluded tap contributes zero whatever its transmission.
        if (opaque > 0.0f)
            visibility += opaque * packet_local_shadow_transmission(
                frame, tap_uv,
                uint(frame.local_shadow_views[tap_view].shadow_params.y),
                reference);
    }
    return vkr_local_shadow_apply_strength(visibility / float(tap_count),
                                           strength);
}

float3 packet_local_shadow_one_tap(VkrVulkanPacketFrameRoot *frame,
                                  VkrGpuPointLightRow light, uint kind,
                                  float3 world_position)
{
    uint encoded_view = uint(light.p3.w);
    if (encoded_view == 0u)
        return 1.0f;
    uint first_view = encoded_view - 1u;
    float strength = frame.local_shadow_views[first_view].shadow_params.x;
    if (strength <= 0.0f)
        return 1.0f;
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
        float3(vkr_local_shadow_atlas_uv(data.atlas_rect,
                                         data.projection_params.y, uv),
               data.atlas_rect.w),
        projected.z);
    float3 transmission = opaque > 0.0f
        ? packet_local_shadow_transmission(
              frame, uv, uint(data.shadow_params.y), projected.z)
        : 1.0f;
    return vkr_local_shadow_apply_strength(opaque * transmission, strength);
}

// Every enabled punctual light for one shaded point, traversed once for the
// base, coat and sheen lobes. `albedo` and `metallic` follow packet_direct: the
// forward and transmission paths pass base color with its metallic factor, the
// deferred path passes G-buffer diffuse albedo and zero. Both spell the same
// product, so the paths shade identically. Without `diffuse_enabled` the base
// contributes specular only, as for fully transmissive surfaces.
// Visibility Shadow.LocalMask stored for one shadowed light at this pixel.
// `slot` counts the pixel's shadowed, in-range lights in traversal order, as
// the mask pass does. With Fallback, a slot past the mask or tagged for
// another light falls back to inline filtering with `shadow_normal`. Without
// it, which only a pixel the mask did not mark for overflow takes, the light
// takes the slot that carries its tag: a range, cone or cutoff test that
// differed at a boundary shifts the slots. A light no slot holds is one the
// mask found to give the pixel no light, and shades unshadowed.
float3 packet_local_shadow_mask<let Fallback : bool>(
    VkrVulkanPacketFrameRoot *frame, VkrGpuPointLightRow light,
    uint light_index, uint kind, float3 world_position, float3 shadow_normal,
    uint mask_texture, uint2 pixel, inout uint slot)
{
    if (uint(light.p3.w) == 0u)
        return 1.0f;
    uint layer = slot;
    ++slot;
    if (layer < VKR_LOCAL_SHADOW_MASK_SLOT_COUNT)
    {
        float4 stored = g_storage_images[mask_texture][uint3(pixel, layer)];
        if (vkr_local_shadow_mask_tag_matches(stored.a, light_index))
            return stored.rgb;
    }
    if (Fallback)
    {
        return packet_local_shadow(frame, light, kind, world_position,
                                   shadow_normal);
    }
    for (uint other = 0u; other < VKR_LOCAL_SHADOW_MASK_SLOT_COUNT; ++other)
    {
        float4 stored = g_storage_images[mask_texture][uint3(pixel, other)];
        if (vkr_local_shadow_mask_tag_matches(stored.a, light_index))
            return stored.rgb;
    }
    return 1.0f;
}

// UseMask selects the base visibility source at compile time: forward and
// transmission shading filter the shadow maps inline, while deferred lighting
// reads the mask for `pixel` from `mask_texture`, with MaskFallback as
// packet_local_shadow_mask's Fallback. Deferred lighting also adds each
// light's visible contribution to `light_contribution` when it is not null
// (VkrLocalLightContributionSample).
VkrVulkanDirectResult packet_punctual_layered<let UseMask : bool,
                                              let MaskFallback : bool>(
                                      VkrVulkanPacketFrameRoot *frame,
                                      float3 world_position, float3 normal,
                                      float3 view, float3 albedo,
                                      float metallic, float roughness,
                                      float3 f0, VkrGgxMaterialEnergy energy,
                                      bool diffuse_enabled,
                                      bool coat_active, VkrClearcoatLayer coat,
                                      bool sheen_active, VkrSheenLayer sheen,
                                      float sheen_normalization,
                                      inout float3 coat_result, inout float3 sheen_result,
                                      uint mask_texture, uint2 pixel,
                                      uint *light_contribution)
{
    VkrVulkanDirectResult result;
    result.irradiance = 0.0f;
    result.diffuse = 0.0f;
    result.specular = 0.0f;
    float3 coat_sum = 0.0f;
    float3 sheen_sum = 0.0f;
    uint4 point_mask =
        vkr_vk_light_cluster_header(frame.light_clusters, world_position)
            .head_lights;
    uint point_count = min(frame.point_light_count, 128u);
    uint mask_slot = 0u;
    for (uint word = 0u; word < 4u; ++word)
    {
        // The mask comes from the pixel's frustum cluster, which marks the
        // light head's rows (ADR-135), so lanes in different clusters carry
        // different masks and the wave already pays for the union of their
        // set bits. Iterating that union explicitly makes `light_index`
        // wave-uniform, which lets the light rows load as scalars instead of
        // per-lane vectors. Each lane still accumulates only its own bits, so
        // the summation order and result are unchanged.
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
            VkrPunctualLightTerm term =
                vkr_punctual_light_term(p0, p1, p2, p3, world_position);
            if (!term.in_range || term.cone <= 0.0f)
                continue;
            uint kind = term.kind;
            float3 light_direction = term.direction;
            float attenuation = term.attenuation * term.cone;
            // `light_index` is wave-uniform, so the wave adds its pixels'
            // unshadowed contribution with one atomic. Shadowing never
            // changes the measure, so selection cannot feed back on itself.
            if (UseMask && light_contribution != nullptr)
            {
                float wave_lit = WaveActiveSum(vkr_local_light_contribution(
                    p1.rgb, p2.x, attenuation, dot(normal, light_direction),
                    frame.pre_exposure));
                if (WaveIsFirstLane())
                {
                    uint ignored;
                    InterlockedAdd(light_contribution[light_index],
                                   uint(wave_lit *
                                            float(VKR_LOCAL_LIGHT_CONTRIBUTION_SCALE) +
                                        0.5f),
                                   ignored);
                }
            }
            float3 shadow_normal =
                vkr_ggx_diffuse_shadow_normal(normal, light_direction, energy);
            // Deferred lighting skips lights below the contribution cutoff,
            // as Shadow.LocalMask gave them no slot.
            if (UseMask &&
                vkr_local_light_contribution(
                    p1.rgb, p2.x, attenuation,
                    dot(shadow_normal, light_direction), frame.pre_exposure) <
                    VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF)
                continue;
            // The base lobes are zero for a light behind the shadow normal, so
            // its visibility is never looked up; deferred lighting's mask gave
            // it no slot either.
            float3 visibility = 0.0f;
            if (vkr_local_shadow_light_faces(shadow_normal, light_direction))
            {
                if (UseMask)
                    visibility = packet_local_shadow_mask<MaskFallback>(
                        frame, light, light_index, kind, world_position,
                        shadow_normal, mask_texture, pixel, mask_slot);
                else
                    visibility = packet_local_shadow(
                        frame, light, kind, world_position, shadow_normal,
                        float2(1.0f, 0.0f), VKR_LOCAL_SHADOW_INLINE_TAP_COUNT);
            }
            if (diffuse_enabled)
            {
                VkrVulkanDirectResult direct = packet_direct(
                    normal, view, light_direction,
                    p1.rgb * p2.x * attenuation * visibility,
                    albedo, metallic, roughness, f0, energy);
                result.irradiance += direct.irradiance;
                result.diffuse += direct.diffuse;
                result.specular += direct.specular;
            }
            else
            {
                result.specular += packet_direct_specular(
                    normal, view, light_direction,
                    p1.rgb * p2.x * attenuation * visibility,
                    roughness, f0, energy);
            }
            // Sheen shares the front base normal. Backlit sheet visibility is
            // irrelevant to its zero response; an independent coat keeps its bias.
            if (coat_active)
            {
                float3 coat_visibility = visibility;
                if (any(coat.normal != shadow_normal))
                {
                    coat_visibility = packet_local_shadow(
                        frame, light, kind, world_position, coat.normal);
                }
                coat_sum += packet_direct_specular(
                    coat.normal, view, light_direction,
                    p1.rgb * p2.x * attenuation * coat_visibility,
                    coat.roughness, float3(VKR_CLEARCOAT_F0), coat.energy);
            }
            if (sheen_active)
            {
                sheen_sum += packet_sheen_direct(
                    normal, view, light_direction,
                    p1.rgb * p2.x * attenuation * visibility,
                    sheen, sheen_normalization);
            }
        }
    }
    if (coat_active)
    {
        coat_result += coat_sum * coat.factor;
    }
    if (sheen_active)
    {
        sheen_result += sheen_sum;
    }
    return result;
}

// Sparse diffuse volume lookup (DVOL v3): the L1 E/pi of the eight probes
// around the receiver, each weighted by its trilinear position, a smooth
// back-face term and a Chebyshev bound on its distance moments, after the
// receiver moves off its surface by a fraction of the probe spacing. False
// outside the volume and where no probe sees the receiver; the caller then
// keeps the environment path. The composed texture keeps the active lamp
// groups' part apart from the sun keys' (diffuse_volume_compose). A surface
// whose lightmap holds the static lamps (`include_lamps` false) leaves that
// part out. `sky_share` is the luminance share of the response that is not
// that part, the baked sky light environment reflections scale by
// (ADR-054); it is one where the response holds no lamp part.
bool packet_diffuse_volume_lookup(VkrVulkanPacketFrameRoot *frame,
                                  float3 world_position, float3 normal,
                                  bool include_lamps, out float3 response,
                                  out float sky_share)
{
    response = 0.0f;
    sky_share = 1.0f;
    uint4 textures = frame.diffuse_volume_textures;
    // Descriptor slot zero is the renderer's permanent sentinel.
    if (textures.x == 0u)
        return false;
    float3 origin = frame.diffuse_volume_origin.xyz;
    float spacing = frame.diffuse_volume_origin.w;
    uint3 dimensions = frame.diffuse_volume_dimensions.xyz;
    uint3 entry = vkr_diffuse_volume_entry(world_position, origin, spacing,
                                           dimensions);
    if (!vkr_diffuse_volume_entry_inside(entry, dimensions))
        return false;
    uint value = g_uint_textures[textures.x].Load(
        int3(int2(vkr_diffuse_volume_entry_texel(entry, dimensions)), 0));
    if (value == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY)
        return false;
    float bias = VKR_DIFFUSE_VOLUME_BIAS *
                 vkr_diffuse_volume_entry_step(value, spacing);
    float3 to_viewer = normalize(frame.view_position.xyz - world_position);
    float3 biased = world_position + (normal * 0.2f + to_viewer * 0.8f) * bias;
    VkrDiffuseVolumeCell cell = vkr_diffuse_volume_brick_cell(
        value, entry, biased, origin, spacing);
    float tile_rows = frame.diffuse_volume_params.y;
    uint lamp_rows = include_lamps ? uint(frame.diffuse_volume_params.z) : 0u;
    SamplerState moment_sampler = g_samplers[frame.diffuse_volume_dimensions.w];
    float4 red = 0.0f;
    float4 green = 0.0f;
    float4 blue = 0.0f;
    float4 lamp_red = 0.0f;
    float4 lamp_green = 0.0f;
    float4 lamp_blue = 0.0f;
    // The dynamic layer's light the bake lacks (ADR-119), signed: a light
    // changed since the bake takes its baked light away.
    uint dynamic_texture = frame.diffuse_volume_dynamic_texture;
    float4 dynamic_red = 0.0f;
    float4 dynamic_green = 0.0f;
    float4 dynamic_blue = 0.0f;
    float total = 0.0f;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        uint probe = vkr_diffuse_volume_corner_probe(cell, corner);
        float4 record = g_textures[textures.y].Load(
            int3(int2(vkr_diffuse_volume_probe_texel(probe)), 0));
        if (record.w <= 0.0f)
            continue;
        float3 probe_position =
            vkr_diffuse_volume_corner_position(cell, corner) + record.xyz;
        float3 to_probe = probe_position - world_position;
        float weight = vkr_diffuse_volume_backface(
            normal, to_probe * rsqrt(max(dot(to_probe, to_probe), 1.0e-8f)));
        float3 from_probe = biased - probe_position;
        float distance = length(from_probe);
        float2 moments = g_textures[textures.z].SampleLevel(
            moment_sampler,
            vkr_diffuse_volume_moment_uv(
                probe, from_probe / max(distance, 1.0e-6f), tile_rows),
            0.0f).xy;
        weight *= vkr_diffuse_volume_chebyshev(moments, distance);
        weight = vkr_diffuse_volume_crush(weight) *
                 vkr_diffuse_volume_trilinear(corner, cell.fraction);
        red += weight * g_textures[textures.w].Load(
            int3(int2(vkr_diffuse_volume_sh_texel(probe, 0u, 0u)), 0));
        green += weight * g_textures[textures.w].Load(
            int3(int2(vkr_diffuse_volume_sh_texel(probe, 1u, 0u)), 0));
        blue += weight * g_textures[textures.w].Load(
            int3(int2(vkr_diffuse_volume_sh_texel(probe, 2u, 0u)), 0));
        if (lamp_rows != 0u)
        {
            lamp_red += weight * g_textures[textures.w].Load(
                int3(int2(vkr_diffuse_volume_sh_texel(probe, 0u, lamp_rows)), 0));
            lamp_green += weight * g_textures[textures.w].Load(
                int3(int2(vkr_diffuse_volume_sh_texel(probe, 1u, lamp_rows)), 0));
            lamp_blue += weight * g_textures[textures.w].Load(
                int3(int2(vkr_diffuse_volume_sh_texel(probe, 2u, lamp_rows)), 0));
        }
        if (dynamic_texture != 0u)
        {
            dynamic_red += weight * g_textures[dynamic_texture].Load(
                int3(int2(vkr_diffuse_volume_sh_texel(probe, 0u, 0u)), 0));
            dynamic_green += weight * g_textures[dynamic_texture].Load(
                int3(int2(vkr_diffuse_volume_sh_texel(probe, 1u, 0u)), 0));
            dynamic_blue += weight * g_textures[dynamic_texture].Load(
                int3(int2(vkr_diffuse_volume_sh_texel(probe, 2u, 0u)), 0));
        }
        total += weight;
    }
    if (total < VKR_DIFFUSE_VOLUME_MIN_WEIGHT)
        return false;
    float scale = frame.diffuse_volume_params.x / total;
    float3 sky = vkr_diffuse_volume_l1(red, green, blue, normal) * scale;
    // The dynamic layer is stored unscaled; it joins the sum before the
    // clamp, so its negative part takes light away but never below zero.
    float dynamic_scale = 1.0f / max(frame.diffuse_volume_params.x, 1.0e-20f);
    float4 extra_red = lamp_red + dynamic_red * dynamic_scale;
    float4 extra_green = lamp_green + dynamic_green * dynamic_scale;
    float4 extra_blue = lamp_blue + dynamic_blue * dynamic_scale;
    response = sky;
    if (lamp_rows != 0u || dynamic_texture != 0u)
    {
        response = vkr_diffuse_volume_l1(red + extra_red, green + extra_green,
                                         blue + extra_blue, normal) *
                   scale;
        const float3 luminance = float3(0.2126f, 0.7152f, 0.0722f);
        float response_luminance = dot(response, luminance);
        sky_share = response_luminance > 0.0f
                        ? saturate(dot(sky, luminance) / response_luminance)
                        : 1.0f;
    }
    return true;
}

bool packet_diffuse_volume_response(VkrVulkanPacketFrameRoot *frame,
                                    float3 world_position, float3 normal,
                                    bool include_lamps, out float3 response)
{
    float unused_sky_share;
    return packet_diffuse_volume_lookup(frame, world_position, normal,
                                        include_lamps, response,
                                        unused_sky_share);
}

// A surface's baked diffuse light, which scales its environment
// reflections (ADR-054): its pre-exposed response E/pi and the sun-key part
// of it. `valid` is false where the surface has none.
struct VkrVulkanBakedLight
{
    float3 response;
    float3 sky;
    bool valid;
};

VkrVulkanBakedLight packet_no_baked_light()
{
    VkrVulkanBakedLight baked;
    baked.response = 0.0f;
    baked.sky = 0.0f;
    baked.valid = false;
    return baked;
}

// From a volume response, pre-exposed, and its sky share.
VkrVulkanBakedLight packet_volume_baked_light(float3 response, float sky_share)
{
    VkrVulkanBakedLight baked;
    baked.response = response;
    baked.sky = response * sky_share;
    baked.valid = true;
    return baked;
}

// A lightmapped surface's volume light, which leaves out the lamp groups,
// with the pre-exposed irradiance of the lamps its lightmap holds
// (ADR-104): their E/pi joins the surroundings it reflects but not its sky
// light (ADR-054). A surface the volume does not cover keeps none.
VkrVulkanBakedLight packet_baked_light_with_lamps(VkrVulkanBakedLight baked,
                                                  float3 lamp_irradiance)
{
    if (baked.valid)
    {
        baked.response += lamp_irradiance * (1.0f / 3.14159265f);
    }
    return baked;
}

// A covered pixel's volume light replaces only diffuse indirect light;
// `baked` carries it for the environment reflection.
bool packet_diffuse_volume_terms(VkrVulkanPacketFrameRoot *frame,
                                 float3 world_position, float3 normal,
                                 float3 albedo, float metallic, float ao,
                                 bool include_lamps,
                                 VkrGgxMaterialEnergy energy,
                                 out float3 diffuse,
                                 out VkrVulkanBakedLight baked)
{
    diffuse = 0.0f;
    baked = packet_no_baked_light();
    float3 volume;
    float sky_share;
    if (!packet_diffuse_volume_lookup(frame, world_position, normal,
                                      include_lamps, volume, sky_share))
        return false;
    // The baked volume stores physical E/pi; lighting is pre-exposed.
    float3 response = volume * frame.pre_exposure;
    baked = packet_volume_baked_light(response, sky_share);
    diffuse = energy.diffuse_weight * (1.0f - metallic) * albedo * ao *
              response;
    return true;
}

// Global environment plus weighted local probes. `horizon_normal` is the
// geometric normal; deferred lighting reconstructs it from depth because the
// G-buffer keeps only the shading normal.
struct VkrVulkanEnvironmentResult
{
    float3 diffuse;
    float3 specular;
    float3 irradiance;
};

// One level of this frame's cloud chain at a face coordinate, bilinear
// across face seams.
float4 packet_cloud_sky_light_level(VkrVulkanSky *sky,
                                    VkrCloudSkyLightDirection at, uint level)
{
    uint face_size = sky.cloud_sky_light_face_size;
    VkrCloudSkyLightFootprint footprint =
        vkr_cloud_sky_light_footprint(at, max(face_size >> level, 1u));
    float4 *chain = sky.cloud_sky_light +
                    vkr_cloud_sky_light_level_offset(face_size, level);
    float4 first_row = lerp(chain[footprint.first_row_first_column],
                            chain[footprint.first_row_second_column],
                            footprint.blend.x);
    float4 second_row = lerp(chain[footprint.second_row_first_column],
                             chain[footprint.second_row_second_column],
                             footprint.blend.x);
    return lerp(first_row, second_row, footprint.blend.y);
}

// The global prefiltered sky along `reflection` at `lod`, seen through this
// frame's cloud layer when its sky light ran (ADR-074). The cloud chain is
// read at the level whose texels match the prefilter's.
float3 packet_global_prefiltered(VkrVulkanPacketFrameRoot *frame,
                                 float3 reflection, float lod)
{
    float3 clear = g_cube_textures[frame.prefilter_texture]
                       .SampleLevel(g_samplers[frame.prefilter_sampler],
                                    reflection, lod)
                       .rgb;
    VkrVulkanSky *sky = frame.sky;
    if (sky.cloud_sky_light_face_size == 0u)
        return clear;
    uint levels = vkr_cloud_sky_light_levels(sky.cloud_sky_light_face_size);
    float level = vkr_cloud_sky_light_level(
        lod, max(frame.prefilter_mip_count, 1u), levels);
    VkrCloudSkyLightDirection at = vkr_cloud_sky_light_direction(reflection);
    uint fine = uint(level);
    uint coarse = min(fine + 1u, levels - 1u);
    float4 cloud = lerp(packet_cloud_sky_light_level(sky, at, fine),
                        packet_cloud_sky_light_level(sky, at, coarse),
                        level - float(fine));
    return vkr_cloud_sky_light_radiance(clear, cloud);
}

VkrVulkanEnvironmentResult
packet_environment_terms_with_diffuse_normal(
    VkrVulkanPacketFrameRoot *frame, float3 world_position,
    float3 diffuse_normal, float3 normal, float3 horizon_normal, float3 view,
    float3 albedo, float metallic, float roughness, float ao,
    VkrGgxMaterialEnergy energy)
{
    float no_v = max(dot(normal, view), 0.0f);
    roughness = vkr_anisotropy_environment_roughness(roughness, energy);
    float3 environment_normal =
        vkr_anisotropy_environment_normal(normal, view, energy);
    float3 reflection = reflect(-view, environment_normal);
    float alpha = roughness * roughness;
    float3 dominant =
        vkr_specular_dominant_direction(environment_normal, reflection, alpha);
    float lod = roughness * float(max(frame.prefilter_mip_count, 1u) - 1u);
    float3 kd = energy.diffuse_weight * (1.0f - metallic);
    // Frame-root indices are uniform. Each local-probe loop iteration also
    // visits one shared table row across the active lanes.
    VkrShL2Evaluation sh_evaluation =
        vkr_sh_l2_prepare_evaluation(diffuse_normal);
    float horizon = saturate(1.0f + dot(reflection, horizon_normal));
    float specular_visibility =
        horizon * horizon * vkr_specular_occlusion(ao, no_v, alpha);
    // Probes carry their own pre-exposed gains; the environment's gains
    // scale only the share the global environment fills.
    float3 irradiance = 0.0f;
    float3 specular = 0.0f;
    float covered = 0.0f;
    uint probe_count = min(frame.ibl_probe_count, 16u);
    for (uint i = 0u; i < probe_count && covered < 1.0f; ++i)
    {
        VkrVulkanIblProbe probe = frame.ibl_probes[i];
        float weight = vkr_reflection_probe_share(
            packet_probe_influence(probe, world_position), covered);
        if (weight <= 1e-6f)
            continue;
        covered += weight;
        float3 probe_irradiance =
            vkr_sh_l2_evaluate(frame.sh_coefficients[probe.sh_slot],
                               sh_evaluation);
        float3 probe_prefiltered =
            g_cube_textures[probe.prefilter_texture]
                .SampleLevel(g_samplers[probe.prefilter_sampler],
                             packet_probe_direction(probe, dominant,
                                                    world_position, alpha),
                             lod)
                .rgb;
        float intensity = probe.intensity_box.x * packet_probe_relight(frame, i);
        irradiance += probe_irradiance * intensity * probe.intensity_box.y *
                      weight;
        specular += probe_prefiltered * intensity * probe.intensity_box.z *
                    weight;
    }
    float global_weight = saturate(1.0f - covered);
    // Global results are consumed after the local sum, so do not keep them
    // live across the local-probe sampling and SH evaluation loop.
    float3 global_irradiance = vkr_sh_l2_evaluate(
        frame.sh_coefficients[frame.sh_global_slot], sh_evaluation);
    irradiance += global_irradiance * global_weight * frame.ibl_controls.x *
                  frame.ibl_controls.y;
    float3 global_prefiltered = packet_global_prefiltered(frame, dominant, lod);
    specular += global_prefiltered * global_weight * frame.ibl_controls.x *
                frame.ibl_controls.z;
    VkrVulkanEnvironmentResult result;
    result.irradiance = irradiance * ao;
    result.diffuse = kd * albedo * result.irradiance;
    result.specular = specular * energy.primary_reflectance *
                          specular_visibility +
                      packet_environment_dual_specular(
                          frame, world_position, normal, horizon_normal, view,
                          ao, energy, packet_no_baked_light());
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

// The environment's prefiltered radiance along the reflection about
// `normal`, pre-exposed and with its specular gains: the probes in priority
// order, then the global environment in the share they leave. A probe's
// capture already holds what its room sees. The global environment does not:
// with `baked` light the surface reflects it only in the share its baked sky
// light supports and its surroundings in the rest (ADR-054), comparing the
// baked light with the response the global SH gives at `normal`.
float3 packet_environment_specular_radiance(
    VkrVulkanPacketFrameRoot *frame, float3 world_position, float3 normal,
    float3 view, float roughness, VkrVulkanBakedLight baked)
{
    float3 reflection = reflect(-view, normal);
    float alpha = roughness * roughness;
    float3 dominant = vkr_specular_dominant_direction(normal, reflection, alpha);
    float lod = roughness * float(max(frame.prefilter_mip_count, 1u) - 1u);
    float3 radiance = 0.0f;
    float covered = 0.0f;
    uint probe_count = min(frame.ibl_probe_count, 16u);
    for (uint i = 0u; i < probe_count && covered < 1.0f; ++i)
    {
        VkrVulkanIblProbe probe = frame.ibl_probes[i];
        float weight = vkr_reflection_probe_share(
            packet_probe_influence(probe, world_position), covered);
        if (weight <= 1e-6f)
            continue;
        covered += weight;
        radiance += g_cube_textures[probe.prefilter_texture]
                        .SampleLevel(g_samplers[probe.prefilter_sampler],
                                     packet_probe_direction(probe, dominant,
                                                            world_position,
                                                            alpha),
                                     lod)
                        .rgb *
                    probe.intensity_box.x * packet_probe_relight(frame, i) *
                    probe.intensity_box.z * weight;
    }
    float global_weight = saturate(1.0f - covered);
    if (global_weight <= 0.0f)
        return radiance;
    float3 global = packet_global_prefiltered(frame, dominant, lod) *
                    frame.ibl_controls.x;
    if (baked.valid)
    {
        float3 response =
            vkr_sh_l2_evaluate(frame.sh_coefficients[frame.sh_global_slot],
                               vkr_sh_l2_prepare_evaluation(normal)) *
            frame.ibl_controls.x;
        global = vkr_sh_l2_occluded_radiance(
            global, baked.response,
            vkr_sh_l2_sky_visibility(baked.sky, response));
    }
    return radiance + global * (global_weight * frame.ibl_controls.z);
}

// The dual lobe's environment reflection (ADR-113), zero without one. It
// is isotropic about the shading normal.
float3 packet_environment_dual_specular(VkrVulkanPacketFrameRoot *frame,
                                        float3 world_position, float3 normal,
                                        float3 horizon_normal, float3 view,
                                        float ao, VkrGgxMaterialEnergy energy,
                                        VkrVulkanBakedLight baked)
{
    if (energy.dual_opacity <= 0.0f)
        return 0.0f;
    float no_v = max(dot(normal, view), 0.0f);
    float3 reflection = reflect(-view, normal);
    float horizon = saturate(1.0f + dot(reflection, horizon_normal));
    return packet_environment_specular_radiance(frame, world_position, normal,
                                                view, energy.dual_roughness,
                                                baked) *
           energy.dual_reflectance * horizon * horizon *
           vkr_specular_occlusion(ao, no_v,
                                  energy.dual_roughness * energy.dual_roughness);
}

float3 packet_environment_specular_weight(float3 normal, float3 horizon_normal,
                                          float3 view, float roughness, float ao,
                                          VkrGgxMaterialEnergy energy)
{
    float no_v = max(dot(normal, view), 0.0f);
    roughness = vkr_anisotropy_environment_roughness(roughness, energy);
    float3 reflection = reflect(-view, vkr_anisotropy_environment_normal(normal, view, energy));
    float horizon = saturate(1.0f + dot(reflection, horizon_normal));
    return energy.primary_reflectance * horizon * horizon *
           vkr_specular_occlusion(ao, no_v, roughness * roughness);
}

float3 packet_environment_specular(VkrVulkanPacketFrameRoot *frame,
                                   float3 world_position, float3 normal,
                                   float3 horizon_normal, float3 view,
                                   float roughness, float ao,
                                   VkrGgxMaterialEnergy energy,
                                   VkrVulkanBakedLight baked)
{
    return packet_environment_specular_radiance(frame, world_position,
                vkr_anisotropy_environment_normal(normal, view, energy), view,
                vkr_anisotropy_environment_roughness(roughness, energy),
                baked) *
           packet_environment_specular_weight(normal, horizon_normal, view,
                                              roughness, ao, energy) +
           packet_environment_dual_specular(frame, world_position, normal,
                                            horizon_normal, view, ao, energy,
                                            baked);
}

float3 packet_sheen_environment(VkrVulkanPacketFrameRoot *frame,
                                float3 world_position, float3 normal,
                                float3 view, float ao, float gtao_specular_cone,
                                VkrSheenLayer sheen, VkrVulkanBakedLight baked)
{
    float no_v = max(dot(normal, view), 0.0f);
    VkrGgxMaterialEnergy approximation = packet_prepare_brdf(
        frame, no_v, sheen.roughness, float3(1.0f));
    float3 incoming = packet_environment_specular_radiance(
        frame, world_position, normal, view, sheen.roughness, baked);
    float3 receiver = packet_environment_specular_weight(
        normal, normal, view, sheen.roughness, ao, approximation);
    return sheen.color * sheen.directional_albedo * incoming * receiver *
           gtao_specular_cone;
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

struct VkrVkAerialSample
{
    float4 packed;
    float weight;
};

// Aerial in-scatter and mean transmittance at a surface. Zero weight is the
// identity when aerial perspective is off or the point is behind the camera.
VkrVkAerialSample packet_aerial_sample(VkrVulkanSky *sky,
                                       uint aerial_perspective_texture,
                                       float3 world_position)
{
    VkrVkAerialSample result;
    result.packed = float4(0.0f, 0.0f, 0.0f, 1.0f);
    result.weight = 0.0f;
    VkrSkyAerialCoordinate coordinate =
        vkr_sky_aerial_coordinate(sky.params, world_position);
    if (coordinate.valid == 0u)
        return result;
    result.packed = g_3d_textures[aerial_perspective_texture].SampleLevel(
        g_samplers[sky.sampler], coordinate.uvw, 0.0f);
    result.weight = coordinate.weight;
    return result;
}

// Sun transmittance through the cloud layer above a world position (ADR-074).
// One outside the sun-projected map or when the layer does not render.
float packet_cloud_shadow(VkrVulkanPacketFrameRoot *frame,
                          float3 world_position)
{
    VkrVulkanSky *sky = frame.sky;
    if (sky.params.clouds.noise.w <= 0.0f)
        return 1.0f;
    float2 uv = vkr_cloud_shadow_uv(sky.params, world_position);
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;
    return g_textures[sky.cloud_shadow_texture]
        .SampleLevel(g_samplers[sky.sampler], uv, 0.0f)
        .r;
}

// Average radiance of the published sky light, scaled like diffuse IBL. Zero
// when the frame carries no global environment.
float3 packet_sky_ambient(VkrVulkanPacketFrameRoot *frame)
{
    if ((frame.flags & 2u) == 0u)
        return float3(0.0f);
    return max(vkr_sh_l2_average(frame.sh_coefficients[frame.sh_global_slot]),
               float3(0.0f)) *
           frame.ibl_controls.x * frame.ibl_controls.y;
}

// Sun and sky light for sky-lit analytic fog; unused by the constant colour.
VkrFogLighting packet_fog_lighting(VkrVulkanPacketFrameRoot *frame)
{
    VkrFogLighting lighting;
    lighting.sun_direction = float3(0.0f, 1.0f, 0.0f);
    lighting.sun_irradiance = float3(0.0f);
    lighting.sky_ambient = float3(0.0f);
    if (frame.fog[0].sky_lighting.x <= 0.0f)
        return lighting;
    // Clouds shade the analytic medium as they shade the camera.
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        lighting.sun_direction =
            normalize(-frame.directional_direction_enabled.xyz);
        lighting.sun_irradiance =
            frame.directional_color_intensity.rgb *
            frame.directional_color_intensity.w *
            packet_cloud_shadow(frame, frame.view_position.xyz);
    }
    lighting.sky_ambient = packet_sky_ambient(frame);
    return lighting;
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

float3 packet_forward_wire(VkrVulkanVertexOutput input, uint primitive_id)
{
    VkrVulkanPacketDrawRoot *draw = vkr_vulkan_packet_draw_root();
    VkrGpuVisibleDrawRow visible = draw.visible_rows[draw.visible_row_index];
    VkrGpuGeometryRow geometry = draw.geometry_rows[visible.geometry_index];
    VkrVulkanInstance instance = draw.frame.instances[input.instance_index];
    uint *indices = (uint *)geometry.index_address;
    VkrPackedStaticVertex *vertices = (VkrPackedStaticVertex *)geometry.vertex_address;
    VkrGpuGeometryDecodeRecord decode =
        ((VkrGpuGeometryDecodeRecord *)geometry.decode_address)[visible.decode_index];
    float3 world[3];
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        uint vertex_index = uint(int(indices[visible.first_index + primitive_id * 3u + corner]) +
                                 visible.vertex_offset);
        VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
            vertices[geometry.first_vertex + vertex_index], decode);
        vertex = vkr_apply_deformation(vertex, instance.deformation_address, vertex_index);
        world[corner] = mul(instance.model, float4(vertex.position, 1.0f)).xyz;
    }
    float3 barycentric = vkr_editor_triangle_barycentric(
        input.world_position, world[0], world[1], world[2]);
    return vkr_editor_wire_color(barycentric, ddx(barycentric), ddy(barycentric));
}

// Forward shading of one surface of `visible`'s draw with `frame`'s view and
// lights: the blend path's draws and a planar reflection's mirrored view.
float4 packet_world_shade_surface(VkrVulkanPacketFrameRoot *frame,
                                  VkrGpuVisibleDrawRow visible,
                                  VkrVulkanVertexOutput input,
                                  bool front_facing)
{
    VkrVulkanMaterial material = frame.materials[visible.material_index];
    float4 base =
        g_textures[NonUniformResourceIndex(material.base_color_texture)]
            .Sample(g_samplers[NonUniformResourceIndex(
                        material.base_color_sampler)],
                    input.uv) *
        material.tint * input.color;
    if (material.alpha_mode == 1u && base.a < material.material_alpha.x)
        discard;
    bool neutral_lighting = vkr_editor_neutral_lighting(frame.render_mode);
    if ((frame.flags & 1u) == 0u && !neutral_lighting)
        return base;

    float face_sign = front_facing ? 1.0f : -1.0f;
    float3 geometric_normal = normalize(input.world_normal) * face_sign;
    float3 normal = geometric_normal;
    if ((material.flags & 1u) != 0u && !vkr_editor_skip_normal_map(frame.render_mode))
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

    float3 view = packet_view_direction(frame, input.world_position);
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
    if (neutral_lighting)
    {
        base.rgb = float3(0.5f);
        metallic = 0.0f;
        roughness = 0.5f;
        ao = 1.0f;
        emissive = float3(0.0f);
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
    emissive *= frame.pre_exposure;
    if (frame.render_mode == 3u)
        return float4(base.rgb + emissive,
                      material.alpha_mode == 0u ? 1.0f : base.a);

    float3 f0 = lerp(neutral_lighting ? float3(0.04f)
                                      : saturate(material.material_dielectric_specular.rgb),
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

  float anisotropy_strength =
      neutral_lighting ? 0.0f : material.material_anisotropy.x;
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
    VkrBrdfExtension extension =
        neutral_lighting ? vkr_brdf_extension_identity()
                         : packet_material_brdf_extension(material);
    energy = packet_apply_specular_extension(frame, energy, extension,
                                             roughness, f0);
    energy = vkr_ggx_diffuse_transmission(
        energy, neutral_lighting ? float4(0.0f)
                                 : material.material_diffuse_transmission);
    energy = vkr_brdf_apply_diffuse_extension(energy, extension,
                                              base.rgb * (1.0f - metallic));
    float clearcoat_factor =
        neutral_lighting ? 0.0f : saturate(material.material_clearcoat.x);
    if (clearcoat_factor > 0.0f && (material.flags & 32u) != 0u)
        clearcoat_factor *= g_textures[NonUniformResourceIndex(material.clearcoat_texture)]
            .Sample(g_samplers[NonUniformResourceIndex(material.clearcoat_sampler)], input.uv).r;
    bool clearcoat_active = vkr_clearcoat_active(clearcoat_factor);
    VkrClearcoatLayer clearcoat = (VkrClearcoatLayer)0;
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
    float3 sheen_color =
        neutral_lighting ? float3(0.0f) : max(material.material_sheen.rgb, 0.0f);
    if (vkr_sheen_active(sheen_color) && (material.flags & 256u) != 0u)
    {
        sheen_color *= g_textures[NonUniformResourceIndex(material.sheen_color_texture)]
            .Sample(g_samplers[NonUniformResourceIndex(material.sheen_color_sampler)],
                    input.uv)
            .rgb;
    }
    bool sheen_active = vkr_sheen_active(sheen_color);
    VkrSheenLayer sheen = (VkrSheenLayer)0;
    VkrSheenLtcSample sheen_ltc = (VkrSheenLtcSample)0;
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
        float cloud_shadow = packet_cloud_shadow(frame, input.world_position);
        // Blended draws have no temporal resolve, so the per-pixel kernel
        // rotation stays fixed.
        float2 shadow_rotation =
            vkr_shadow_kernel_rotation(uint2(input.position.xy), 0u);
        // The receiver moves off its geometric surface: the shading normal
        // would push it along normal-map relief.
        float base_shadow =
            packet_directional_shadow(
                frame, input.world_position,
                back_lit ? -geometric_normal : geometric_normal,
                shadow_rotation) *
            cloud_shadow;
        float directional_shadow = base_shadow;
        if (back_lit && clearcoat_active)
            directional_shadow =
                packet_directional_shadow(frame, input.world_position,
                                          geometric_normal, shadow_rotation) *
                cloud_shadow;
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
    // Each light list is traversed once for the base, coat and sheen lobes.
    VkrVulkanDirectResult punctual = packet_punctual_layered<false, true>(
        frame, input.world_position, normal, view, base.rgb, metallic,
        roughness, f0, energy, true, clearcoat_active, clearcoat,
        sheen_active, sheen, sheen_ltc.energy_normalization,
        clearcoat_direct, sheen_direct, 0u, uint2(0u, 0u), nullptr);
    analytic_diffuse += punctual.diffuse;
    analytic_specular += punctual.specular;
    VkrVulkanDirectResult rectangles = packet_layered_rectangle_lights(
        frame, input.world_position, normal, view, base.rgb, metallic,
        roughness, f0, energy, true, clearcoat_active, clearcoat,
        sheen_active, sheen, sheen_ltc, clearcoat_direct, sheen_direct);
    analytic_diffuse += rectangles.diffuse;
    analytic_specular += rectangles.specular;
    float3 indirect_diffuse;
    float3 indirect_specular = 0.0f;
    float3 clearcoat_indirect = 0.0f;
    float3 sheen_indirect = 0.0f;
    // A covered pixel takes its diffuse light from the volume, which also
    // scales its environment reflections. A mover's draws take both at its
    // lighting origin.
    float3 lighting_position = packet_lighting_position(
        frame, input.instance_index, input.world_position);
    float3 volume_diffuse;
    VkrVulkanBakedLight baked;
    bool volume_valid = packet_diffuse_volume_terms(
        frame, lighting_position, normal, base.rgb, metallic, ao, true,
        energy, volume_diffuse, baked);
    if ((frame.flags & 2u) != 0u)
    {
        if (volume_valid)
        {
            indirect_diffuse = volume_diffuse;
            indirect_specular = packet_environment_specular(
                frame, lighting_position, normal, geometric_normal, view,
                roughness, ao, energy, baked);
        }
        else
        {
            VkrVulkanEnvironmentResult environment = packet_environment_terms(
                frame, lighting_position, normal, geometric_normal, view,
                base.rgb, metallic, roughness, ao, energy);
            indirect_diffuse = environment.diffuse;
            indirect_specular = environment.specular;
        }
        if (clearcoat_active)
            clearcoat_indirect = clearcoat.factor * packet_environment_specular(
                frame, lighting_position, clearcoat.normal, clearcoat.normal, view,
                clearcoat.roughness, ao, clearcoat.energy, baked);
        if (sheen_active)
            sheen_indirect = packet_sheen_environment(
                frame, lighting_position, normal, view, ao, 1.0f, sheen,
                baked);
    }
    else
    {
        indirect_diffuse = volume_valid
                               ? volume_diffuse
                               : frame.ambient_color.rgb * base.rgb * ao *
                                     (1.0f - metallic) * energy.diffuse_weight;
    }
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

float4 packet_world_shade(VkrVulkanVertexOutput input, bool front_facing,
                          uint primitive_id)
{
    VkrVulkanPacketDrawRoot *draw = vkr_vulkan_packet_draw_root();
    VkrGpuVisibleDrawRow visible = draw.visible_rows[draw.visible_row_index];
    if (draw.frame.render_mode == 12u)
    {
        VkrVulkanMaterial material =
            draw.frame.materials[visible.material_index];
        float4 base = packet_base_color(input);
        if (material.alpha_mode == 1u && base.a < material.material_alpha.x)
            discard;
        return float4(packet_forward_wire(input, primitive_id), base.a);
    }
    return packet_world_shade_surface(draw.frame, visible, input,
                                      front_facing);
}

[shader("fragment")]
float4 world_fragment(VkrVulkanVertexOutput input,
                      bool front_facing : SV_IsFrontFace,
                      uint primitive_id : SV_PrimitiveID) : SV_Target0
{
    return packet_world_shade(input, front_facing, primitive_id);
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
    surface.instance_index = input.instance_index;
    surface.world_tangent = input.world_tangent;
    output.color = packet_world_shade(surface, front_facing, primitive_id);
    if (output.color.a <= 1e-4f)
        discard;
    VkrVulkanPacketDrawRoot *draw = vkr_vulkan_packet_draw_root();
    // Aerial perspective is the farther medium, so it applies before fog.
    VkrVulkanSky *sky = draw.frame.sky;
    if (sky.params.aerial.w > 0.0f)
    {
        VkrVkAerialSample aerial = packet_aerial_sample(
            sky, sky.aerial_perspective_texture, input.world_position);
        output.color.rgb = vkr_sky_apply_aerial(output.color.rgb,
                                                aerial.packed, aerial.weight);
    }
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
            output.color.rgb, draw.frame.fog[0],
            packet_fog_lighting(draw.frame), draw.frame.view_position.xyz,
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

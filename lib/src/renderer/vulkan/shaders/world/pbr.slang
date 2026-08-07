#include "common/transform.slangh"
#include "common/instance.slangh"
#include "common/alpha_cutout.slangh"

static const uint32_t MAX_SCENE_POINT_LIGHTS = 128u;
static const uint32_t POINT_LIGHT_GRID_MASK_WORDS = 4u;
static const uint32_t POINT_LIGHT_GRID_MAX_CELLS = 384u;

struct GlobalUniformBufferObject
{
    column_major float4x4 projection;
    column_major float4x4 view;
    float4 ambient_color;
    float3 view_position;
    uint32_t render_mode;
    float4 screen_params;

    uint32_t dir_enabled;
    float3 dir_direction;
    float4 dir_color;

    uint32_t point_light_count;
    float4 point_light_data[MAX_SCENE_POINT_LIGHTS * 4];
    float4 point_light_grid_origin_cell_size;
    float4 point_light_grid_dimensions_count;
    float4 point_light_grid_global_mask;
    float4 point_light_grid_masks[POINT_LIGHT_GRID_MAX_CELLS];

    uint32_t shadow_enabled;
    uint32_t shadow_cascade_count;
    float4 shadow_map_inv_size[2];
    float shadow_pcf_radius;

    float4 shadow_split_far[2];
    float4 shadow_world_units_per_texel[2];
    float4 shadow_light_space_origin_x[2];
    float4 shadow_light_space_origin_y[2];
    float4 shadow_uv_margin_scale[2];
    float4 shadow_uv_soft_margin_scale[2];
    float4 shadow_uv_kernel_margin_scale[2];
    float shadow_bias;
    float shadow_normal_bias;
    float shadow_slope_bias;
    float shadow_bias_texel_scale;
    float shadow_slope_bias_texel_scale;
    float shadow_distance_fade_range;
    float shadow_cascade_blend_range;
    uint32_t shadow_debug_cascades;
    uint32_t shadow_debug_mode;
    column_major float4x4 shadow_view_projection[8];
    uint32_t ibl_enabled;
    float ibl_intensity;
    float ibl_diffuse_intensity;
    float ibl_specular_intensity;
};

struct LocalUniformObject
{
    float4 base_color;
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float3 emissive_factor;
    float3 dielectric_specular;
    float transmission_factor;
    float ior;
    float thickness_factor;
    float3 attenuation_color;
    float attenuation_distance;
    uint32_t transmission_pass;
    uint32_t alpha_mode;
    float alpha_cutoff;
    uint32_t texture_flags;
    float4 ibl_probe0_center_blend;
    float4 ibl_probe0_extents_weight;
    float4 ibl_probe0_intensity_box;
    float4 ibl_probe1_center_blend;
    float4 ibl_probe1_extents_weight;
    float4 ibl_probe1_intensity_box;
    float4 ibl_probe2_center_blend;
    float4 ibl_probe2_extents_weight;
    float4 ibl_probe2_intensity_box;
};

static const uint32_t TEXTURE_FLAG_HAS_BASE_COLOR = 0x1u;
static const uint32_t TEXTURE_FLAG_HAS_NORMAL = 0x2u;
static const uint32_t TEXTURE_FLAG_HAS_METALLIC_ROUGHNESS = 0x4u;
static const uint32_t TEXTURE_FLAG_HAS_OCCLUSION = 0x8u;
static const uint32_t TEXTURE_FLAG_HAS_EMISSIVE = 0x10u;
static const uint32_t TEXTURE_FLAG_HAS_TRANSMISSION = 0x20u;
static const uint32_t TEXTURE_FLAG_HAS_THICKNESS = 0x40u;
static const uint32_t TEXTURE_FLAG_HAS_DIELECTRIC_SPECULAR = 0x80u;

[[vk::binding(1, 1)]]
Texture2D<float4> base_color_texture;

[[vk::binding(2, 1)]]
Texture2D<float4> metallic_roughness_texture;

[[vk::binding(3, 1)]]
Texture2D<float4> dielectric_specular_texture;

[[vk::binding(4, 1)]]
Texture2D<float4> normal_texture;

[[vk::binding(5, 1)]]
Texture2D<float4> occlusion_texture;

[[vk::binding(6, 1)]]
Texture2D<float4> emissive_texture;

[[vk::binding(7, 1)]]
Texture2DArray<float> shadow_map;

[[vk::binding(8, 1)]]
TextureCube<float4> irradiance_map;

[[vk::binding(9, 1)]]
TextureCube<float4> prefilter_map;

[[vk::binding(10, 1)]]
Texture2D<float4> brdf_lut;

[[vk::binding(11, 1)]]
TextureCube<float4> irradiance_map_b;

[[vk::binding(12, 1)]]
TextureCube<float4> prefilter_map_b;

[[vk::binding(13, 1)]]
Texture2D<float4> transmission_texture;

[[vk::binding(14, 1)]]
Texture2D<float4> thickness_texture;

[[vk::binding(15, 1)]]
Texture2D<float4> transmission_source;

[[vk::binding(16, 1)]]
TextureCube<float4> irradiance_map_c;

[[vk::binding(17, 1)]]
TextureCube<float4> prefilter_map_c;

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUniformBufferObject> g_ubo;

[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> g_instances;

[[vk::binding(0, 1)]]
ConstantBuffer<LocalUniformObject> local_ubo;

[[vk::binding(18, 1)]]
SamplerState base_color_sampler;

[[vk::binding(19, 1)]]
SamplerState metallic_roughness_sampler;

[[vk::binding(20, 1)]]
SamplerState dielectric_specular_sampler;

[[vk::binding(21, 1)]]
SamplerState normal_sampler;

[[vk::binding(22, 1)]]
SamplerState occlusion_sampler;

[[vk::binding(23, 1)]]
SamplerState emissive_sampler;

[[vk::binding(24, 1)]]
SamplerComparisonState shadow_sampler;

[[vk::binding(25, 1)]]
SamplerState irradiance_sampler;

[[vk::binding(26, 1)]]
SamplerState prefilter_sampler;

[[vk::binding(27, 1)]]
SamplerState brdf_lut_sampler;

[[vk::binding(28, 1)]]
SamplerState transmission_sampler;

[[vk::binding(29, 1)]]
SamplerState thickness_sampler;

[[vk::binding(30, 1)]]
SamplerState transmission_source_sampler;

#include "common/csm.slangh"

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 texcoord : TEXCOORD;
    [[vk::location(3)]] float4 color    : COLOR0;
    [[vk::location(4)]] float4 tangent  : TANGENT;
};

struct VertexDto
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    float3 frag_position;
    float3 normal;
    float4 tangent;
    float4 color;
};

[shader("vertex")]
VertexDto vertexMain(VertexInput input, uint instance_id : SV_InstanceID,
                     uint base_instance : SV_StartInstanceLocation)
{
    VertexDto output;
    uint instance_index = instance_id + base_instance;
    InstanceData inst = g_instances[instance_index];

    output.texcoord = input.texcoord;
    output.color = input.color;

    float4 world_pos = vkr_world_from_model_3d(inst.model, input.position);
    output.frag_position = world_pos.xyz;

    float3x3 model_matrix = float3x3(inst.model);
    output.normal = normalize(mul(model_matrix, input.normal));
    output.tangent = float4(normalize(mul(model_matrix, input.tangent.xyz)), input.tangent.w);
    output.position = vkr_clip_from_world(g_ubo.projection, g_ubo.view, world_pos);
    return output;
}

static const float PI = 3.14159265359f;
static const uint32_t RENDER_MODE_DEFAULT = 0u;
static const uint32_t RENDER_MODE_LIGHTING = 1u;
static const uint32_t RENDER_MODE_NORMAL = 2u;
static const uint32_t RENDER_MODE_UNLIT = 3u;
static const uint32_t RENDER_MODE_DIRECT_DIFFUSE = 4u;
static const uint32_t RENDER_MODE_DIRECT_SPECULAR = 5u;
static const uint32_t RENDER_MODE_MATERIAL_PARAMS = 6u;

uint4 point_light_mask_at(float3 world_position)
{
    uint4 mask = asuint(g_ubo.point_light_grid_global_mask);
    float cell_size = g_ubo.point_light_grid_origin_cell_size.w;
    uint3 dimensions = (uint3)(g_ubo.point_light_grid_dimensions_count.xyz + 0.5f);
    uint cell_count = (uint)(g_ubo.point_light_grid_dimensions_count.w + 0.5f);
    if (cell_size <= 0.0f || cell_count == 0u)
    {
        return mask;
    }

    int3 cell = (int3)floor((world_position -
                             g_ubo.point_light_grid_origin_cell_size.xyz) /
                            cell_size);
    if (any(cell < int3(0, 0, 0)) || any(cell >= (int3)dimensions))
    {
        return mask;
    }
    uint index = (uint)cell.x +
                 dimensions.x * ((uint)cell.y + dimensions.y * (uint)cell.z);
    return mask | asuint(g_ubo.point_light_grid_masks[index]);
}

float3 compute_normal(VertexDto input, float face_sign)
{
    float3 n = normalize(input.normal) * face_sign;
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_NORMAL) == 0u)
    {
        return n;
    }

    float3 t = normalize(input.tangent.xyz);
    t = normalize(t - dot(t, n) * n);
    float3 b = cross(n, t) * input.tangent.w;
    float3x3 tbn = float3x3(t, b, n);

    float3 sampled = normal_texture.Sample(normal_sampler, input.texcoord).xyz;
    sampled = sampled * 2.0f - 1.0f;
    sampled.y = -sampled.y;
    sampled.xy *= local_ubo.normal_scale;
    return normalize(mul(tbn, sampled));
}

float distribution_ggx(float3 n, float3 h, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(n, h), 0.0f);
    float ndoth2 = ndoth * ndoth;
    float denom = ndoth2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-6f);
}

float geometry_schlick_ggx(float ndotv, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return ndotv / max(ndotv * (1.0f - k) + k, 1e-6f);
}

float geometry_smith(float3 n, float3 v, float3 l, float roughness)
{
    float ndotv = max(dot(n, v), 0.0f);
    float ndotl = max(dot(n, l), 0.0f);
    float ggx2 = geometry_schlick_ggx(ndotv, roughness);
    float ggx1 = geometry_schlick_ggx(ndotl, roughness);
    return ggx1 * ggx2;
}

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    // KHR_materials_pbrSpecularGlossiness derives grazing reflectance from
    // authored specular color. The retained F0 therefore also controls F90:
    // authored F0=0 must not regain a camera-facing highlight through an
    // implicit white grazing term.
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    return f0 + (f90 - f0) * pow(max(1.0f - cos_theta, 0.0f), 5.0f);
}

float3 fresnel_schlick_roughness(float cos_theta, float3 f0, float roughness)
{
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float3 rough_f90 = float3(f90 * (1.0f - roughness),
                              f90 * (1.0f - roughness),
                              f90 * (1.0f - roughness));
    return f0 + (max(rough_f90, f0) - f0) *
                    pow(max(1.0f - cos_theta, 0.0f), 5.0f);
}

float filter_specular_roughness(float roughness, float3 shading_normal)
{
    // Normal-map frequency above the pixel footprint must widen the GGX lobe;
    // otherwise a distant textured surface turns the environment into a
    // camera-following bright/dark boundary.
    float3 normal_dx = ddx(shading_normal);
    float3 normal_dy = ddy(shading_normal);
    float variance = 0.25f * (dot(normal_dx, normal_dx) +
                              dot(normal_dy, normal_dy));
    return sqrt(saturate(roughness * roughness + min(variance, 0.25f)));
}

float specular_ambient_occlusion(float ao, float ndotv, float roughness)
{
    // Lagarde-style visibility approximation: diffuse AO alone is not enough
    // to keep a global cubemap from leaking through locally occluded material.
    return saturate(pow(ndotv + ao, exp2(-16.0f * roughness - 1.0f)) -
                    1.0f + ao);
}

float3 box_project_direction(float3 direction, float3 world_position, float3 center,
                             float3 extents)
{
    if (extents.x <= 1e-4f || extents.y <= 1e-4f || extents.z <= 1e-4f)
    {
        return normalize(direction);
    }

    float3 box_min = center - extents;
    float3 box_max = center + extents;
    float3 safe_direction = direction;
    safe_direction.x = abs(safe_direction.x) < 1e-4f ? (safe_direction.x < 0.0f ? -1e-4f : 1e-4f) : safe_direction.x;
    safe_direction.y = abs(safe_direction.y) < 1e-4f ? (safe_direction.y < 0.0f ? -1e-4f : 1e-4f) : safe_direction.y;
    safe_direction.z = abs(safe_direction.z) < 1e-4f ? (safe_direction.z < 0.0f ? -1e-4f : 1e-4f) : safe_direction.z;

    float3 t_min = (box_min - world_position) / safe_direction;
    float3 t_max = (box_max - world_position) / safe_direction;
    float3 t_far = max(t_min, t_max);
    float t_hit = min(min(t_far.x, t_far.y), t_far.z);
    t_hit = max(t_hit, 0.0f);

    float3 hit = world_position + safe_direction * t_hit;
    return normalize(hit - center);
}

struct IblSlotResult
{
    float3 diffuse;
    float3 specular;
};

float ibl_probe_influence(float3 world_position, float4 probe_center_blend,
                          float4 probe_extents_weight,
                          float4 probe_intensity_box)
{
    float base_weight = max(probe_extents_weight.w, 0.0f);
    if (base_weight <= 1e-6f)
    {
        return 0.0f;
    }
    if (probe_intensity_box.w <= 0.5f)
    {
        return base_weight;
    }

    float3 extents = max(probe_extents_weight.xyz, 0.0f.xxx);
    float3 outside = max(abs(world_position - probe_center_blend.xyz) - extents,
                         0.0f.xxx);
    float outside_distance = length(outside);
    if (outside_distance <= 1e-6f)
    {
        return base_weight;
    }
    float blend_distance = max(probe_center_blend.w, 0.0f);
    return blend_distance > 1e-6f
               ? base_weight * saturate(1.0f - outside_distance / blend_distance)
               : 0.0f;
}

IblSlotResult evaluate_ibl_slot(TextureCube<float4> slot_irradiance_map,
                                SamplerState slot_irradiance_sampler,
                                TextureCube<float4> slot_prefilter_map,
                                SamplerState slot_prefilter_sampler, float3 n,
                                float3 geometric_normal, float3 v,
                                float3 world_position, float3 albedo,
                                float metallic, float roughness, float ndotv,
                                float ao, float3 f0,
                                float4 probe_center_blend,
                                float4 probe_extents_weight,
                                float4 probe_intensity_box, float weight)
{
    IblSlotResult result = {float3(0.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 0.0f)};
    if (weight <= 1e-6f)
    {
        return result;
    }

    static const float MAX_REFLECTION_LOD = 8.0f;
    float3 center = probe_center_blend.xyz;
    float3 extents = max(probe_extents_weight.xyz, float3(0.0f, 0.0f, 0.0f));
    float slot_intensity = probe_intensity_box.x;
    float slot_diffuse_intensity = probe_intensity_box.y;
    float slot_specular_intensity = probe_intensity_box.z;
    bool box_projection_enabled = probe_intensity_box.w > 0.5f;

    float3 irradiance_dir = n;
    float3 reflection_dir = reflect(-v, n);
    if (box_projection_enabled)
    {
        // Parallax correction applies to a position-dependent reflection ray.
        // Diffuse irradiance is indexed by the surface normal and must remain
        // translation invariant inside a probe volume.
        reflection_dir = box_project_direction(reflection_dir, world_position,
                                               center, extents);
    }

    float3 f = fresnel_schlick_roughness(ndotv, f0, roughness);
    float3 kd = (1.0f - f) * (1.0f - metallic);
    float3 irradiance =
        slot_irradiance_map.Sample(slot_irradiance_sampler, irradiance_dir).rgb;
    float3 diffuse_ibl = irradiance * albedo;
    float3 prefiltered = slot_prefilter_map.SampleLevel(
        slot_prefilter_sampler, reflection_dir, roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf = brdf_lut.Sample(brdf_lut_sampler, float2(ndotv, roughness)).rg;
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float horizon = saturate(1.0f + dot(reflection_dir, geometric_normal));
    horizon *= horizon;
    float specular_visibility =
        horizon * specular_ambient_occlusion(ao, ndotv, roughness);
    float3 specular_ibl =
        prefiltered * (f * brdf.x + f90 * brdf.y) * specular_visibility;

    result.diffuse =
        kd * diffuse_ibl * ao * slot_diffuse_intensity * slot_intensity * weight;
    result.specular =
        specular_ibl * slot_specular_intensity * slot_intensity * weight;
    return result;
}

struct PbrDirectResult
{
    float3 diffuse;
    float3 specular;
};

PbrDirectResult pbr_direct(float3 n, float3 v, float3 l, float3 radiance,
                           float3 albedo, float metallic, float roughness,
                           float3 f0, float shadow)
{
    float3 h = normalize(v + l);
    float ndotl = max(dot(n, l), 0.0f);
    float ndotv = max(dot(n, v), 0.0f);
    float hdotv = max(dot(h, v), 0.0f);

    float3 f = fresnel_schlick(hdotv, f0);
    float d = distribution_ggx(n, h, roughness);
    float g = geometry_smith(n, v, l, roughness);

    float3 numerator = d * g * f;
    float denominator = max(4.0f * ndotv * ndotl, 1e-6f);
    float3 specular = numerator / denominator;

    float3 kd = (1.0f - f) * (1.0f - metallic);
    float3 scale = radiance * ndotl * shadow;
    PbrDirectResult result = {kd * albedo / PI * scale, specular * scale};
    return result;
}

[shader("fragment")]
float4 fragmentMain(VertexDto input,
                    bool is_front_face : SV_IsFrontFace) : SV_Target
{
    float3 v = normalize(g_ubo.view_position - input.frag_position);
    // Winding is stable for the primitive. View-vector faceforward is not: on
    // a grazing plane it changes sign along a camera-dependent line and turns
    // both diffuse and specular response into a moving dark band.
    float face_sign = is_front_face ? 1.0f : -1.0f;
    float3 geometric_normal = normalize(input.normal) * face_sign;
    float3 normal = compute_normal(input, face_sign);
    if (g_ubo.render_mode == RENDER_MODE_NORMAL)
    {
        return float4(normal * 0.5f + 0.5f, 1.0f);
    }

    float4 base_sample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_BASE_COLOR) != 0u)
    {
        base_sample = base_color_texture.Sample(base_color_sampler, input.texcoord);
    }

    float4 base_color = base_sample * local_ubo.base_color;
    float alpha = base_color.a;
    vkr_apply_alpha_cutout_from_alpha(alpha, local_ubo.alpha_cutoff);

    float metallic = saturate(local_ubo.metallic);
    float roughness = saturate(local_ubo.roughness);
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_METALLIC_ROUGHNESS) != 0u)
    {
        float4 mr = metallic_roughness_texture.Sample(metallic_roughness_sampler, input.texcoord);
        metallic *= mr.b;
        roughness *= mr.g;
    }
    roughness = filter_specular_roughness(clamp(roughness, 0.04f, 1.0f), normal);

    float ao = local_ubo.occlusion_strength;
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_OCCLUSION) != 0u)
    {
        float occ = occlusion_texture.Sample(occlusion_sampler, input.texcoord).r;
        ao *= occ;
    }

    float3 emissive = local_ubo.emissive_factor;
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_EMISSIVE) != 0u)
    {
        emissive *= emissive_texture.Sample(emissive_sampler, input.texcoord).rgb;
    }

    if (g_ubo.render_mode == RENDER_MODE_UNLIT)
    {
        return float4(base_color.rgb + emissive,
                      (local_ubo.alpha_mode == 0u) ? 1.0f : alpha);
    }

    float ndotv = max(dot(normal, v), 0.0f);
    float3 dielectric_f0 = saturate(local_ubo.dielectric_specular);
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_DIELECTRIC_SPECULAR) != 0u)
    {
        dielectric_f0 = dielectric_specular_texture.Sample(
            dielectric_specular_sampler, input.texcoord).rgb;
    }
    float3 f0 = lerp(dielectric_f0, base_color.rgb, metallic);
    if (g_ubo.render_mode == RENDER_MODE_MATERIAL_PARAMS)
    {
        return float4(metallic, roughness,
                      max(f0.x, max(f0.y, f0.z)), 1.0f);
    }
    float3 color = g_ubo.ibl_enabled != 0u
                       ? float3(0.0f, 0.0f, 0.0f)
                       : g_ubo.ambient_color.rgb * base_color.rgb * ao;
    float3 analytic_direct = float3(0.0f, 0.0f, 0.0f);
    float3 analytic_diffuse = float3(0.0f, 0.0f, 0.0f);
    float3 analytic_specular = float3(0.0f, 0.0f, 0.0f);

    uint selected_cascade = 0u;
    float4 view_pos = mul(g_ubo.view, float4(input.frag_position, 1.0f));
    if (g_ubo.shadow_enabled != 0u && g_ubo.dir_enabled != 0u)
    {
        selected_cascade = select_cascade(-view_pos.z);
    }

    float dir_shadow = 1.0f;
    uint effective_cascade = selected_cascade;
    if (g_ubo.shadow_enabled != 0u && g_ubo.dir_enabled != 0u)
    {
        float view_depth = -view_pos.z;
        ShadowResult shadow_result =
            calculate_shadow_blended(input.frag_position, normal, view_depth,
                                     selected_cascade);
        dir_shadow = shadow_result.factor;
        effective_cascade = shadow_result.effective_cascade;
    }

    if (g_ubo.dir_enabled != 0u)
    {
        float3 l = normalize(-g_ubo.dir_direction);
        PbrDirectResult direct = pbr_direct(
            normal, v, l, g_ubo.dir_color.rgb, base_color.rgb, metallic,
            roughness, f0, dir_shadow);
        analytic_diffuse += direct.diffuse;
        analytic_specular += direct.specular;
    }

    uint point_count = min(g_ubo.point_light_count, MAX_SCENE_POINT_LIGHTS);
    uint4 point_light_mask = point_light_mask_at(input.frag_position);
    for (uint word = 0u; word < POINT_LIGHT_GRID_MASK_WORDS; ++word)
    {
        uint remaining = point_light_mask[word];
        while (remaining != 0u)
        {
            uint i = word * 32u + (uint)firstbitlow(remaining);
            remaining &= remaining - 1u;
            if (i >= point_count)
            {
                continue;
            }
            float4 p0 = g_ubo.point_light_data[i * 4u + 0u];
            float4 p1 = g_ubo.point_light_data[i * 4u + 1u];
            float4 p2 = g_ubo.point_light_data[i * 4u + 2u];
            float4 p3 = g_ubo.point_light_data[i * 4u + 3u];

            float3 light_pos = p0.xyz;
            float constant = max(p0.w, 1.0f);
            float3 light_color = p1.rgb;
            float linear = p1.w;
            float intensity = p2.x;
            float quadratic = p2.y;
            float range = p2.z;
            uint kind = (uint)(p2.w + 0.5f);

            float3 to_light = light_pos - input.frag_position;
            float distance_squared = dot(to_light, to_light);
            if (kind != 0u && range > 0.0f &&
                distance_squared >= range * range)
            {
                continue;
            }
            float distance = sqrt(distance_squared);
            float3 l = distance > 1e-6f ? to_light / distance : float3(0.0f, 0.0f, 0.0f);
            float attenuation = 0.0f;
            if (kind == 0u)
            {
                attenuation = 1.0f / (constant + linear * distance +
                                      quadratic * distance * distance);
            }
            else
            {
                float distance_squared = max(distance * distance, 1e-4f);
                float range_attenuation = 1.0f;
                if (range > 0.0f)
                {
                    float ratio = distance / range;
                    range_attenuation = saturate(1.0f - ratio * ratio * ratio * ratio);
                    range_attenuation *= range_attenuation;
                }
                attenuation = range_attenuation / distance_squared;
                if (kind == 2u)
                {
                    float cone = dot(-l, normalize(p3.xyz));
                    float cone_attenuation = smoothstep(p1.w, p0.w, cone);
                    if (cone_attenuation <= 0.0f)
                    {
                        continue;
                    }
                    attenuation *= cone_attenuation;
                }
            }
            float3 radiance = light_color * intensity * attenuation;
            PbrDirectResult direct = pbr_direct(
                normal, v, l, radiance, base_color.rgb, metallic, roughness,
                f0, 1.0f);
            analytic_diffuse += direct.diffuse;
            analytic_specular += direct.specular;
        }
    }
    analytic_direct = analytic_diffuse + analytic_specular;
    color += analytic_direct;

    if (g_ubo.ibl_enabled != 0u)
    {
        float probe0_weight = ibl_probe_influence(
            input.frag_position, local_ubo.ibl_probe0_center_blend,
            local_ubo.ibl_probe0_extents_weight,
            local_ubo.ibl_probe0_intensity_box);
        float probe1_weight = ibl_probe_influence(
            input.frag_position, local_ubo.ibl_probe1_center_blend,
            local_ubo.ibl_probe1_extents_weight,
            local_ubo.ibl_probe1_intensity_box);
        float local_weight_sum = probe0_weight + probe1_weight;
        if (local_weight_sum > 1.0f)
        {
            probe0_weight /= local_weight_sum;
            probe1_weight /= local_weight_sum;
            local_weight_sum = 1.0f;
        }
        float probe2_weight = max(1.0f - local_weight_sum, 0.0f) *
                              max(local_ubo.ibl_probe2_extents_weight.w, 0.0f);

        IblSlotResult slot0 = evaluate_ibl_slot(
            irradiance_map, irradiance_sampler, prefilter_map, prefilter_sampler,
            normal, geometric_normal, v, input.frag_position, base_color.rgb,
            metallic, roughness, ndotv, ao, f0, local_ubo.ibl_probe0_center_blend,
            local_ubo.ibl_probe0_extents_weight, local_ubo.ibl_probe0_intensity_box,
            probe0_weight);
        IblSlotResult slot1 = evaluate_ibl_slot(
            irradiance_map_b, irradiance_sampler, prefilter_map_b,
            prefilter_sampler, normal, geometric_normal, v,
            input.frag_position, base_color.rgb, metallic, roughness, ndotv, ao,
            f0, local_ubo.ibl_probe1_center_blend,
            local_ubo.ibl_probe1_extents_weight, local_ubo.ibl_probe1_intensity_box,
            probe1_weight);
        IblSlotResult slot2 = evaluate_ibl_slot(
            irradiance_map_c, irradiance_sampler, prefilter_map_c,
            prefilter_sampler, normal, geometric_normal, v,
            input.frag_position, base_color.rgb, metallic, roughness, ndotv, ao,
            f0, local_ubo.ibl_probe2_center_blend,
            local_ubo.ibl_probe2_extents_weight, local_ubo.ibl_probe2_intensity_box,
            probe2_weight);

        float3 ibl_diffuse = slot0.diffuse + slot1.diffuse + slot2.diffuse;
        float3 ibl_specular = slot0.specular + slot1.specular + slot2.specular;
        float3 ibl = (ibl_diffuse * g_ubo.ibl_diffuse_intensity +
                      ibl_specular * g_ubo.ibl_specular_intensity) *
                     g_ubo.ibl_intensity;
        color += ibl;
    }

    if (g_ubo.render_mode == RENDER_MODE_LIGHTING)
    {
        return float4(analytic_direct, 1.0f);
    }
    if (g_ubo.render_mode == RENDER_MODE_DIRECT_DIFFUSE)
    {
        return float4(analytic_diffuse, 1.0f);
    }
    if (g_ubo.render_mode == RENDER_MODE_DIRECT_SPECULAR)
    {
        return float4(analytic_specular, 1.0f);
    }

    color += emissive;

    if (local_ubo.transmission_pass != 0u)
    {
        float transmission = saturate(local_ubo.transmission_factor);
        if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_TRANSMISSION) != 0u)
        {
            transmission *= transmission_texture.Sample(
                transmission_sampler, input.texcoord).r;
        }

        float thickness = max(local_ubo.thickness_factor, 0.0f);
        if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_THICKNESS) != 0u)
        {
            thickness *= thickness_texture.Sample(thickness_sampler,
                                                  input.texcoord).g;
        }

        float2 screen_uv = input.position.xy * g_ubo.screen_params.xy;

        float safe_ior = max(local_ubo.ior, 1.0f);
        float3 view_normal = normalize(mul((float3x3)g_ubo.view, normal));
        float3 refracted = refract(float3(0.0f, 0.0f, -1.0f), view_normal,
                                   1.0f / safe_ior);
        float2 refract_offset =
            refracted.xy / max(abs(refracted.z), 0.25f) * thickness * 0.02f;
        float3 transmitted = transmission_source.Sample(
            transmission_source_sampler,
            clamp(screen_uv + refract_offset, 0.0f.xx, 1.0f.xx)).rgb;

        if (local_ubo.attenuation_distance > 1e-4f && thickness > 0.0f)
        {
            float3 attenuation_base = clamp(local_ubo.attenuation_color,
                                            1e-4f.xxx, 1.0f.xxx);
            transmitted *= pow(attenuation_base,
                               thickness / local_ubo.attenuation_distance);
        }

        float3 fresnel = fresnel_schlick(ndotv, f0);
        float transmission_weight = transmission * (1.0f - metallic) *
                                    (1.0f - max(fresnel.r,
                                                max(fresnel.g, fresnel.b)));
        color = lerp(color, transmitted, saturate(transmission_weight));
    }
    if (g_ubo.shadow_debug_mode == 2u)
    {
        return float4(dir_shadow.xxx, 1.0f);
    }
    if (g_ubo.shadow_debug_mode == 3u)
    {
        ShadowProjection p = project_shadow(input.frag_position, effective_cascade);
        float shadow_depth = sample_shadow_depth(effective_cascade, p.uv_clamped);
        float delta = saturate(abs(p.z_clamped - shadow_depth) * 50.0f);
        float3 debug_color = float3(p.z_clamped, shadow_depth, delta);
        if (p.uv_out)
        {
            debug_color = lerp(debug_color, float3(1.0f, 0.0f, 0.0f), 0.75f);
        }
        if (p.z_out)
        {
            debug_color = lerp(debug_color, float3(0.0f, 0.0f, 1.0f), 0.75f);
        }
        return float4(debug_color, 1.0f);
    }
    if (g_ubo.shadow_debug_mode == 1u)
    {
        const float3 debug_color = g_ubo.shadow_enabled != 0u
                                       ? SHADOW_CASCADE_DEBUG_COLORS[effective_cascade]
                                       : float3(1.0f, 0.0f, 1.0f);
        return float4(debug_color, 1.0f);
    }
    float output_alpha =
        (local_ubo.alpha_mode == 0u || local_ubo.transmission_pass != 0u)
            ? 1.0f
            : alpha;
    return float4(color, output_alpha);
}

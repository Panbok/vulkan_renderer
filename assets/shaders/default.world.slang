#include "vkr_shader_transform.slangh"
#include "vkr_shader_instance.slangh"

// Maximum number of point lights
static const uint32_t MAX_POINT_LIGHTS = 16u;

// Uniform buffer for MVP matrices
struct GlobalUniformBufferObject
{
    column_major float4x4 projection;
    column_major float4x4 view;
    float4 ambient_color;
    float3 view_position;
    uint32_t render_mode;
    float4 screen_params; // xy = 1/viewport_size, zw = viewport_size

    // Directional light
    uint32_t dir_enabled;
    float3 dir_direction;    // World space, normalized
    float4 dir_color;        // rgb = color * intensity, a = 1

    // Point lights (packed vec4 array)
    uint32_t point_light_count;
    float4 point_light_data[MAX_POINT_LIGHTS * 3];

    // Cascaded shadows (CSM)
    uint32_t shadow_enabled;
    uint32_t shadow_cascade_count;
    float4 shadow_map_inv_size[2];
    float shadow_pcf_radius;

    float4 shadow_split_far[2];
    float4 shadow_world_units_per_texel[2];
    float4 shadow_light_space_origin_x[2];
    float4 shadow_light_space_origin_y[2];
    float shadow_bias;
    float shadow_normal_bias;
    float shadow_slope_bias;
    float shadow_bias_texel_scale;
    float shadow_slope_bias_texel_scale;
    float shadow_distance_fade_range;
    float shadow_cascade_blend_range;
    uint32_t shadow_debug_cascades;
    uint32_t shadow_debug_mode; // 0=off, 1=cascades, 2=factor, 3=depth, 4..11=maps, 12=frustum, 13=camera

    column_major float4x4 shadow_view_projection[8];
};

struct LocalUniformObject
{
    float4 diffuse_color;
    float4 specular_color;
    float3 emission_color;
    float shininess;
    uint32_t texture_flags; // 0x1=has_diffuse, 0x2=has_specular, 0x4=has_normal
};

static const uint32_t TEXTURE_FLAG_HAS_DIFFUSE = 0x1u;
static const uint32_t TEXTURE_FLAG_HAS_SPECULAR = 0x2u;
static const uint32_t TEXTURE_FLAG_HAS_NORMAL = 0x4u;
struct DirectionalLight
{
    float3 direction;
    float4 color;
};

struct PointLight
{
    float3 position;
    float4 color;
    // Usually 1, make sure denominator never gets smaller than 1
    float constant;
    // Reduces light intensity linearly
    float linear;
    // Makes the light fall off slower at longer distances.
    float quadratic;
};

[[vk::binding(1, 1)]]
Texture2D<float4> diffuse_texture;

[[vk::binding(2, 1)]]
Texture2D<float4> specular_texture;

[[vk::binding(3, 1)]]
Texture2D<float4> normal_texture;

[[vk::binding(4, 1)]]
Texture2DArray<float> shadow_map;

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUniformBufferObject> g_ubo;

[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> g_instances;

// Instance UBO (set 1, binding 0)
[[vk::binding(0, 1)]]
ConstantBuffer<LocalUniformObject> local_ubo;

// Samplers in the same instance set: set 1, bindings 1+instance_texture_count (here: 5..8)
[[vk::binding(5, 1)]]
SamplerState diffuse_sampler;

[[vk::binding(6, 1)]]
SamplerState specular_sampler;

[[vk::binding(7, 1)]]
SamplerState normal_sampler;

[[vk::binding(8, 1)]]
SamplerComparisonState shadow_sampler;

#include "vkr_shader_csm.slangh"

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

    float4 worldPos = vkr_world_from_model_3d(inst.model, input.position);
    output.frag_position = worldPos.xyz;

    float3x3 model_matrix = float3x3(inst.model);
    output.normal = normalize(mul(model_matrix, input.normal));
    output.tangent = float4(normalize(mul(model_matrix, input.tangent.xyz)), input.tangent.w);

    output.position = vkr_clip_from_world(g_ubo.projection, g_ubo.view, worldPos);

    return output;
}

static const uint32_t RENDER_MODE_DEFAULT = 0u;
static const uint32_t RENDER_MODE_LIGHTING = 1u;
static const uint32_t RENDER_MODE_NORMAL = 2u;
static const uint32_t RENDER_MODE_UNLIT = 3u;

float3 compute_normal_from_map(VertexDto input)
{
    float3 normal = input.normal;

    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_NORMAL) == 0)
    {
        return normal;
    }

    float3 tangent = input.tangent.xyz;
    
    // Re-orthogonalize tangent (Gram-Schmidt)
    tangent = normalize(tangent - dot(tangent, normal) * normal);
    
    float3 bitangent = cross(normal, tangent) * input.tangent.w;
    float3x3 tbn = float3x3(tangent, bitangent, normal);

    float3 sampled = normal_texture.Sample(normal_sampler, input.texcoord).xyz;
    
    // Invert Green Channel for correct OpenGL->DirectX/Vulkan mapping 
    sampled.y = 1.0 - sampled.y;

    sampled = (sampled * 2.0f) - 1.0f;
    return normalize(mul(tbn, sampled));
}

float4 calculate_directional_light(DirectionalLight light, float3 normal,
                                   float3 view_direction, float4 ambient_color,
                                   uint32_t render_mode, float4 diffuse_sample,
                                   float specular_mask, float shadow)
{
    float diffuse_factor = max(dot(normal, -light.direction), 0.0f);
    float3 half_direction = normalize(view_direction - light.direction);
    float specular_factor =
        pow(max(dot(half_direction, normal), 0.0f), local_ubo.shininess);

    float4 ambient =
        float4(ambient_color.rgb * local_ubo.diffuse_color.rgb, diffuse_sample.a);
    float4 diffuse =
        float4(light.color.rgb * diffuse_factor, diffuse_sample.a) * shadow;
    float4 specular =
        float4(light.color.rgb * specular_factor, diffuse_sample.a) * shadow;
    specular *= local_ubo.specular_color * specular_mask;

    if (render_mode == RENDER_MODE_DEFAULT)
    {
        ambient *= diffuse_sample;
        diffuse *= diffuse_sample;
        specular.a = diffuse_sample.a;
    }
    else
    {
        diffuse *= local_ubo.diffuse_color;
    }

    return ambient + diffuse + specular;
}

float4 calculate_point_light(PointLight light, float3 normal,
                             float3 frag_position, float3 view_direction,
                             uint32_t render_mode,
                             float4 diffuse_sample, float specular_mask)
{
    float3 light_direction = normalize(light.position - frag_position);
    float diff = max(dot(normal, light_direction), 0.0f);
    float3 half_direction = normalize(view_direction + light_direction);
    float spec = pow(max(dot(half_direction, normal), 0.0f), local_ubo.shininess);

    float distance = length(light.position - frag_position);
    float attenuation =
        1.0f /
        (light.constant + light.linear * distance +
         light.quadratic * (distance * distance));

    float4 diffuse = light.color * diff;
    float4 specular = light.color * spec;
    specular *= local_ubo.specular_color;

    if (render_mode == RENDER_MODE_DEFAULT)
    {
        diffuse *= diffuse_sample;
        specular *= specular_mask;
        specular.a = diffuse_sample.a;
    }
    else
    {
        diffuse *= local_ubo.diffuse_color;
    }

    diffuse *= attenuation;
    specular *= attenuation;
    return diffuse + specular;
}

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    float3 normal = compute_normal_from_map(input);

    uint32_t render_mode = g_ubo.render_mode;
    if (render_mode == RENDER_MODE_NORMAL)
    {
        return float4(abs(normal), 1.0f);
    }

    // Use diffuse texture if available, otherwise use material color
    float4 diffuse_sample;
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_DIFFUSE) != 0)
    {
        diffuse_sample = diffuse_texture.Sample(diffuse_sampler, input.texcoord);
    }
    else
    {
        diffuse_sample = local_ubo.diffuse_color;
    }

    float alpha = diffuse_sample.a;

    if (render_mode == RENDER_MODE_UNLIT)
    {
        return float4(local_ubo.emission_color.rgb, alpha);
    }

    float specular_sample;
    if ((local_ubo.texture_flags & TEXTURE_FLAG_HAS_SPECULAR) != 0)
    {
        specular_sample = specular_texture.Sample(specular_sampler, input.texcoord).r;
    }
    else
    {
        specular_sample = 1.0; // Or use local_ubo.specular_color if appropriate
    }

    float3 view_direction = float3(0.0f, 0.0f, 0.0f);
    bool has_lighting = (g_ubo.dir_enabled != 0u) || (g_ubo.point_light_count > 0u);
    if (has_lighting)
    {
        view_direction = normalize(g_ubo.view_position - input.frag_position);
    }

    // Start with ambient contribution
    float4 color = float4(0.0, 0.0, 0.0, diffuse_sample.a);

    // Directional light (if enabled)
    float shadow = 1.0f;
    uint cascade = 0u;           // Initially selected cascade (by view depth)
    uint effective_cascade = 0u; // Actually used cascade (may differ due to fallback)
    if (g_ubo.shadow_enabled != 0u && g_ubo.dir_enabled != 0)
    {
        float4 view_pos = mul(g_ubo.view, float4(input.frag_position, 1.0));
        float view_depth = -view_pos.z;
        cascade = select_cascade(view_depth);
        ShadowResult shadow_result = calculate_shadow_blended(input.frag_position, normal, view_depth, cascade);
        shadow = shadow_result.factor;
        effective_cascade = shadow_result.effective_cascade;

        float fade_range = max(g_ubo.shadow_distance_fade_range, 0.0f);
        if (fade_range > 0.0f)
        {
            uint count = min(max(g_ubo.shadow_cascade_count, 1u), SHADOW_CASCADE_MAX);
            float far_split = shadow_split_far_for(count - 1u);
            float start = far_split - fade_range;
            float t = saturate((view_depth - start) / max(fade_range, 1e-4f));
            shadow = lerp(shadow, 1.0f, t);
        }
    }

    if (g_ubo.dir_enabled != 0)
    {
        DirectionalLight dir_light;
        dir_light.direction = g_ubo.dir_direction;
        dir_light.color = g_ubo.dir_color;

        color += calculate_directional_light(dir_light, normal,
                                             view_direction,
                                             g_ubo.ambient_color,
                                             render_mode, diffuse_sample,
                                             specular_sample, shadow);
    }
    else
    {
        // No directional light - add ambient only
        float4 ambient = float4(g_ubo.ambient_color.rgb * local_ubo.diffuse_color.rgb, diffuse_sample.a);
        if (render_mode == RENDER_MODE_DEFAULT)
        {
            ambient *= diffuse_sample;
        }
        color += ambient;
    }

    // Point lights (loop through active lights)
    for (uint i = 0; i < g_ubo.point_light_count && i < MAX_POINT_LIGHTS; i++)
    {
        // Unpack point light data from vec4 array
        // [i*3+0] = {position.xyz, constant}
        // [i*3+1] = {color.rgb * intensity, linear}
        // [i*3+2] = {intensity, quadratic, 0, 0}
        float4 d0 = g_ubo.point_light_data[i * 3 + 0];
        float4 d1 = g_ubo.point_light_data[i * 3 + 1];
        float4 d2 = g_ubo.point_light_data[i * 3 + 2];

        PointLight point_light;
        point_light.position = d0.xyz;
        point_light.constant = d0.w;
        point_light.color = float4(d1.xyz, 1.0);
        point_light.linear = d1.w;
        point_light.quadratic = d2.y;

        color += calculate_point_light(point_light, normal, input.frag_position,
                                       view_direction,
                                       render_mode, diffuse_sample,
                                       specular_sample);
    }

    color.rgb += local_ubo.emission_color.rgb;

    // Reinhard Tone Mapping
    color.rgb = color.rgb / (color.rgb + 1.0f);
    color.a = alpha;

    if (g_ubo.shadow_debug_mode == 2u)
    {
        color.rgb = shadow.xxx;
        color.a = 1.0f;
        return color;
    }

    if (g_ubo.shadow_debug_mode == 3u)
    {
        // Use effective_cascade to show the cascade actually being sampled
        ShadowProjection p = project_shadow(input.frag_position, effective_cascade);
        float d = sample_shadow_depth(effective_cascade, p.uv_clamped);

        // Debug (mode=3): visualize projection coverage and depth compare inputs.
        // - R: receiver depth (clamped)
        // - G: shadow map depth at clamped UV
        // - B: |receiver - map| (amplified)
        // Out-of-range tint:
        // - UV out-of-range: red
        // - Z out-of-range: blue
        float dz = saturate(abs(p.z_clamped - d) * 50.0f);
        color.rgb = float3(p.z_clamped, d, dz);
        if (p.uv_out)
        {
            color.rgb = lerp(color.rgb, float3(1.0f, 0.0f, 0.0f), 0.75f);
        }
        if (p.z_out)
        {
            color.rgb = lerp(color.rgb, float3(0.0f, 0.0f, 1.0f), 0.75f);
        }
        color.a = 1.0f;
        return color;
    }

    if (g_ubo.shadow_debug_mode == 12u)
    {
        uint count = min(max(g_ubo.shadow_cascade_count, 1u), SHADOW_CASCADE_MAX);

        ShadowProjection sel = project_shadow(input.frag_position, cascade);
        bool sel_in = (!sel.uv_out && !sel.z_out);

        bool any_in = sel_in;
        uint cover = cascade;
        [unroll]
        for (uint i = 0u; i < SHADOW_CASCADE_MAX; ++i)
        {
            if (i < count && !any_in)
            {
                ShadowProjection p = project_shadow(input.frag_position, i);
                if (!p.uv_out && !p.z_out)
                {
                    any_in = true;
                    cover = i;
                }
            }
        }

        float3 c = float3(sel.uv_clamped, sel.z_clamped);
        c = lerp(c, SHADOW_CASCADE_DEBUG_COLORS[cascade], 0.15f);

        if (!sel.uv_out)
        {
            float2 uv = sel.uv_clamped;
            float edge = min(min(uv.x, 1.0f - uv.x), min(uv.y, 1.0f - uv.y));
            float border_width = max(shadow_map_inv_size_for(cascade) * 2.0f, 1e-6f);
            float border = saturate((border_width - edge) / border_width);
            c = lerp(c, float3(1.0f, 1.0f, 1.0f), border);
        }

        if (!sel_in)
        {
            c = lerp(c, float3(1.0f, 0.0f, 0.0f), sel.uv_out ? 0.75f : 0.0f);
            c = lerp(c, float3(0.0f, 0.0f, 1.0f), sel.z_out ? 0.75f : 0.0f);
            if (any_in)
            {
                c = lerp(c, SHADOW_CASCADE_DEBUG_COLORS[cover], 0.4f);
                c = lerp(c, float3(1.0f, 0.0f, 1.0f), 0.6f);
            }
        }

        color.rgb = c;
        color.a = 1.0f;
        return color;
    }

    if (g_ubo.shadow_debug_mode == 13u)
    {
        float4 world_pos = float4(input.frag_position, 1.0f);
        float4 view_pos = mul(g_ubo.view, world_pos);
        float4 clip_pos = mul(g_ubo.projection, view_pos);

        float3 ndc = clip_pos.xyz / clip_pos.w;
        float2 uv = ndc.xy * 0.5f + 0.5f;

        uint count = min(max(g_ubo.shadow_cascade_count, 1u), SHADOW_CASCADE_MAX);

        float far_ref = shadow_split_far_for(count - 1u);
        far_ref = max(far_ref, 1e-4f);

        float view_depth = -view_pos.z;
        float depth_norm = saturate(view_depth / far_ref);

        float3 c = float3(uv, depth_norm);

        // Highlight unexpected clip-space / view-space signs (points in front
        // should have clip.w > 0 and view_pos.z < 0 for the engine's
        // right-handed convention).
        if (clip_pos.w <= 0.0f || view_pos.z > 0.0f)
        {
            c = lerp(c, float3(1.0f, 0.0f, 0.0f), 0.85f);
        }

        bool ndc_out = (ndc.x < -1.0f || ndc.x > 1.0f ||
                        ndc.y < -1.0f || ndc.y > 1.0f ||
                        ndc.z <  0.0f || ndc.z > 1.0f);
        if (ndc_out)
        {
            c = lerp(c, float3(1.0f, 0.5f, 0.0f), 0.75f);
        }

        // Draw split planes in view space. If the shadow cutoff aligns with
        // one of these lines, the issue is likely cascade selection/coverage.
        float thickness = max(far_ref * 0.002f, 0.05f);
        float3 split_c = 0.0.xxx;
        float w = 0.0f;

        for (uint i = 0u; i < count; ++i)
        {
            float d = abs(view_depth - shadow_split_far_for(i));
            float l = saturate((thickness - d) / thickness);
            split_c += l * SHADOW_CASCADE_DEBUG_COLORS[i];
            w += l;
        }

        c = lerp(c, split_c, saturate(w));

        color.rgb = c;
        color.a = 1.0f;
        return color;
    }

    if (g_ubo.shadow_debug_mode >= 4u && g_ubo.shadow_debug_mode <= 11u)
    {
        // Debug (mode=4..11): show raw shadow map contents (cascade 0..7) by
        // mapping the current pixel to UV space.
        //
        // On Vulkan backends, `SV_Position` may be provided either as
        // framebuffer pixel coordinates or as NDC. Detect which it is to avoid
        // sampling a single texel (appears as a uniform black/white screen).
        float2 uv = 0.0.xx;
        float2 pos = input.position.xy;
        bool pos_is_ndc = (abs(pos.x) <= 2.0f) && (abs(pos.y) <= 2.0f);
        if (pos_is_ndc)
        {
            uv = pos * 0.5f + 0.5f;
        }
        else
        {
            float2 inv_viewport = g_ubo.screen_params.xy;
            uv = pos * inv_viewport;
        }

        uint map_index = min(g_ubo.shadow_debug_mode - 4u,
                             SHADOW_CASCADE_MAX - 1u);
        float d = 1.0f;
        d = sample_shadow_depth(map_index, uv);

        // Depth maps often cluster close to 1.0. Provide a few views at once:
        // - R: raw depth (d)
        // - G: amplified inverse depth (casters pop, but can saturate)
        // - B: zoomed view of the 0.98..1.0 range (reveals "near-1" structure)
        float inv = 1.0f - d;
        float inv_amp = saturate(inv * 50.0f);
        float near1 = saturate((d - 0.98f) / 0.02f);
        color.rgb = float3(d, inv_amp, near1);
        color.a = 1.0f;
        return color;
    }

    if (g_ubo.shadow_debug_cascades != 0u || g_ubo.shadow_debug_mode == 1u)
    {
        // Debug: magenta means shadows are disabled but debug is on
        if (g_ubo.shadow_enabled == 0u)
        {
            color.rgb = lerp(color.rgb, float3(1.0f, 0.0f, 1.0f), 0.5f);
        }
        else
        {
            // Use effective_cascade to show the cascade actually sampled (after fallback)
            if (g_ubo.shadow_debug_mode == 1u)
            {
                color.rgb = SHADOW_CASCADE_DEBUG_COLORS[effective_cascade];
            }
            else
            {
                color.rgb = lerp(color.rgb, SHADOW_CASCADE_DEBUG_COLORS[effective_cascade], 0.3f);
            }
        }
    }

    return color;
}

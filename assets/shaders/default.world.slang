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
    float shadow_map_inv_size;
    float shadow_pcf_radius;

    float4 shadow_split_far;
    float4 shadow_world_units_per_texel;
    float shadow_bias;
    float shadow_normal_bias;
    float shadow_cascade_blend_range;
    uint32_t shadow_debug_cascades;
    uint32_t shadow_debug_mode; // 0=off, 1=cascades, 2=factor, 3=depth, 4..7=maps, 8=frustum, 9=camera

    column_major float4x4 shadow_view_projection[4];
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

static const float3 SHADOW_CASCADE_DEBUG_COLORS[4] = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1),
    float3(1, 1, 0),
};

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

struct InstanceData
{
    column_major float4x4 model;
    uint object_id;
    uint material_index;
    uint flags;
    uint _padding;
};

[[vk::binding(1, 1)]]
Texture2D<float4> diffuse_texture;

[[vk::binding(2, 1)]]
Texture2D<float4> specular_texture;

[[vk::binding(3, 1)]]
Texture2D<float4> normal_texture;

[[vk::binding(4, 1)]]
Texture2D<float> shadow_map_0;

[[vk::binding(5, 1)]]
Texture2D<float> shadow_map_1;

[[vk::binding(6, 1)]]
Texture2D<float> shadow_map_2;

[[vk::binding(7, 1)]]
Texture2D<float> shadow_map_3;

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUniformBufferObject> g_ubo;

[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> g_instances;

// Instance UBO (set 1, binding 0)
[[vk::binding(0, 1)]]
ConstantBuffer<LocalUniformObject> local_ubo;

// Samplers in the same instance set: set 1, bindings 1+instance_texture_count (here: 8..14)
[[vk::binding(8, 1)]]
SamplerState diffuse_sampler;

[[vk::binding(9, 1)]]
SamplerState specular_sampler;

[[vk::binding(10, 1)]]
SamplerState normal_sampler;

[[vk::binding(11, 1)]]
SamplerComparisonState shadow_sampler_0;

[[vk::binding(12, 1)]]
SamplerComparisonState shadow_sampler_1;

[[vk::binding(13, 1)]]
SamplerComparisonState shadow_sampler_2;

[[vk::binding(14, 1)]]
SamplerComparisonState shadow_sampler_3;

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
    float4 ambient_color;
    float3 normal;
    float4 tangent;
    float4 color;
    float3 view_position;
    uint32_t render_mode;
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

    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    output.frag_position = worldPos.xyz;

    float3x3 model_matrix = float3x3(inst.model);
    output.normal = normalize(mul(model_matrix, input.normal));
    output.tangent = float4(normalize(mul(model_matrix, input.tangent.xyz)), input.tangent.w);

    output.ambient_color = g_ubo.ambient_color;
    output.view_position = g_ubo.view_position;
    output.render_mode = g_ubo.render_mode;
    
    float4 viewPos = mul(g_ubo.view, worldPos);
    float4 clipPos = mul(g_ubo.projection, viewPos);

    output.position = clipPos;

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

uint select_cascade(float view_depth)
{
    uint count = max(g_ubo.shadow_cascade_count, 1u);
    count = min(count, 4u);

    float4 far_splits = g_ubo.shadow_split_far;

    if (count <= 1u) return 0u;
    if (view_depth < far_splits.x) return 0u;
    if (count == 2u) return 1u;
    if (view_depth < far_splits.y) return 1u;
    if (count == 3u) return 2u;
    if (view_depth < far_splits.z) return 2u;
    return min(count - 1u, 3u);
}

// Maximum UV deviation from [0,1] range before considering the cascade as not
// covering. A larger value makes cascade selection more lenient, reducing
// bright "seam" lines at cascade boundaries at the cost of sampling shadow map
// edges more often (minor quality reduction at boundaries).
float shadow_uv_margin()
{
    // Keep this tight. If we allow fragments outside [0,1] to be considered
    // "covered", we end up clamping to the shadow-map edge which often yields
    // fully-lit seams that track the camera/cascade split.
    float texel_margin = g_ubo.shadow_map_inv_size * 1.0f; // 1 texel
    return max(texel_margin, 0.00025f);
}

struct ShadowProjection
{
    float2 uv;
    float z;
    float2 uv_clamped;
    float z_clamped;
    bool uv_out;
    bool z_out;
    float uv_out_amount; // max distance outside [0,1] (0 = inside)
    float coverage_quality; // 1.0 = fully inside, decreases towards edge
};

float shadow_uv_soft_margin()
{
    // Soft fallback: allow a few texels out-of-range to avoid 1-pixel “holes”
    // near cascade boundaries / snapping.
    return max(g_ubo.shadow_map_inv_size * 8.0f, 0.001f);
}

float shadow_pcf_radius_texels(uint cascade);

float shadow_uv_kernel_margin(uint cascade)
{
    // If the receiver UV is close enough to the shadow map edge that the PCF
    // kernel samples outside [0,1], border sampling can create bright seams.
    // Treat that region as uncovered so we can fall back to a better cascade.
    float r = shadow_pcf_radius_texels(cascade);
    return g_ubo.shadow_map_inv_size * (r + 0.5f);
}

ShadowProjection project_shadow(float3 world_pos, uint cascade)
{
    ShadowProjection p;
    float4 clip = mul(g_ubo.shadow_view_projection[cascade], float4(world_pos, 1.0f));
    clip.xyz /= clip.w;

    p.uv = clip.xy * 0.5f + 0.5f;
    p.z = clip.z;

    float margin = shadow_uv_margin();
    float kernel_margin = shadow_uv_kernel_margin(cascade);
    // Treat the PCF kernel as part of the coverage test: if the kernel would
    // sample outside [0,1], edge/border sampling can create bright seams.
    float2 uv = p.uv;
    float edge = min(min(uv.x, 1.0f - uv.x), min(uv.y, 1.0f - uv.y));
    bool kernel_hits_edge = (edge < kernel_margin);

    p.uv_out = (p.uv.x < -margin || p.uv.x > 1.0f + margin ||
                p.uv.y < -margin || p.uv.y > 1.0f + margin ||
                kernel_hits_edge);

    // Z check: allow small deviation for depth precision issues
    float z_margin = 0.002f;
    p.z_out = (p.z < -z_margin || p.z > 1.0f + z_margin);

    p.uv_clamped = clamp(p.uv, 0.0f, 1.0f);
    p.z_clamped = saturate(p.z);

    float2 out_lo = max(0.0f.xx, 0.0f.xx - p.uv);
    float2 out_hi = max(0.0f.xx, p.uv - 1.0f.xx);
    p.uv_out_amount = max(max(out_lo.x, out_lo.y), max(out_hi.x, out_hi.y));

    // Compute coverage quality - how well this cascade covers the fragment.
    // 1.0 = UV is well inside [0,1], decreases towards the edge.
    float dist_from_edge = min(
        min(p.uv.x, 1.0f - p.uv.x),
        min(p.uv.y, 1.0f - p.uv.y)
    );
    float cover_band = max(margin + kernel_margin, 1e-6f);
    p.coverage_quality = saturate(dist_from_edge / cover_band);

    return p;
}

static const float2 SHADOW_POISSON_16[16] = {
    float2(-0.94201624, -0.39906216),
    float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870),
    float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432),
    float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845),
    float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554),
    float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023),
    float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507),
    float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367),
    float2( 0.14383161, -0.14100790),
};

float shadow_pcf_radius_texels(uint cascade)
{
    float r0 = max(g_ubo.shadow_pcf_radius, 0.0f);
    if (r0 <= 0.0f) return 0.0f;

    // Keep the filter roughly constant in world-space across cascades by
    // scaling the radius in texels according to world-units-per-texel.
    float base = max(g_ubo.shadow_world_units_per_texel.x, 1e-6f);
    float cur = (cascade == 0u) ? g_ubo.shadow_world_units_per_texel.x :
                (cascade == 1u) ? g_ubo.shadow_world_units_per_texel.y :
                (cascade == 2u) ? g_ubo.shadow_world_units_per_texel.z :
                                  g_ubo.shadow_world_units_per_texel.w;
    float scale = clamp(cur / base, 0.5f, 8.0f);
    return clamp(r0 * scale, 0.0f, 4.0f);
}

float sample_shadow_pcf(Texture2D<float> map, SamplerComparisonState samp, float2 uv,
                        float compare_depth, float bias, uint cascade)
{
    float2 inv_size =
        float2(g_ubo.shadow_map_inv_size, g_ubo.shadow_map_inv_size);

    // Avoid clamping to 1.0: ref=1 with LESS/LEQUAL tends to produce fully-lit
    // seams for fragments that drift slightly past the cascade far plane.
    float cmp = clamp(compare_depth - bias, 0.0f, 1.0f - 1e-5f);

    float r_tex = shadow_pcf_radius_texels(cascade);
    if (r_tex <= 0.0f)
    {
        return map.SampleCmpLevelZero(samp, uv, cmp);
    }

    float2 radius_uv = inv_size * r_tex;
    float sum = 0.0f;

    [unroll]
    for (uint i = 0u; i < 16u; ++i)
    {
        float2 uv_tap = clamp(uv + SHADOW_POISSON_16[i] * radius_uv, 0.0f, 1.0f);
        sum += map.SampleCmpLevelZero(samp, uv_tap, cmp);
    }

    return sum * (1.0f / 16.0f);
}

// Try to find a cascade that covers the fragment. Returns the cascade index
// and whether coverage was found. Prioritizes the initially selected cascade,
// then searches neighbors to handle coverage gaps from stabilization snapping.
struct CascadeLookup
{
    uint cascade;
    bool covered;
    ShadowProjection proj;
};

uint clamp_cascade_index(uint idx)
{
    uint count = min(max(g_ubo.shadow_cascade_count, 1u), 4u);
    if (count == 0u) return 0u;
    return min(idx, count - 1u);
}

CascadeLookup find_covering_cascade(float3 world_pos, uint initial_cascade)
{
    CascadeLookup result;
    initial_cascade = clamp_cascade_index(initial_cascade);
    result.cascade = initial_cascade;
    result.covered = false;
    result.proj = project_shadow(world_pos, initial_cascade);

    uint count = min(max(g_ubo.shadow_cascade_count, 1u), 4u);

    // If the initially selected cascade covers, keep it. This avoids temporal
    // instability from switching cascades based on marginal coverage heuristics.
    if (!result.proj.uv_out && !result.proj.z_out)
    {
        result.covered = true;
        return result;
    }

    // Otherwise, search nearest cascades first. This produces stable results
    // when two cascades overlap (guard band, snapping, or frustum fit changes).
    //
    // Order example for initial=1: 1,0,2,3
    // Order example for initial=2: 2,1,3,0
    [unroll]
    for (uint step = 1u; step < 4u; ++step)
    {
        uint left = (initial_cascade >= step) ? (initial_cascade - step) : 0u;
        if (left < count)
        {
            ShadowProjection p = project_shadow(world_pos, left);
            if (!p.uv_out && !p.z_out)
            {
                result.cascade = left;
                result.covered = true;
                result.proj = p;
                return result;
            }
        }

        uint right = initial_cascade + step;
        if (right < count)
        {
            ShadowProjection p = project_shadow(world_pos, right);
            if (!p.uv_out && !p.z_out)
            {
                result.cascade = right;
                result.covered = true;
                result.proj = p;
                return result;
            }
        }
    }

    // Soft fallback: if we failed strict coverage due to tiny numerical drift,
    // pick the cascade that is only slightly out-of-range in UV and sample it
    // clamped. This prevents straight “lit seams” that track the camera.
    float soft_uv = shadow_uv_soft_margin();
    float z_soft = 0.02f;
    uint best = initial_cascade;
    float best_out = 1e9f;
    ShadowProjection best_proj = result.proj;

    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        if (i >= count) continue;
        ShadowProjection p = (i == initial_cascade) ? result.proj : project_shadow(world_pos, i);
        bool z_ok = (p.z >= -z_soft && p.z <= 1.0f + z_soft);
        if (!z_ok) continue;
        if (p.uv_out_amount <= soft_uv)
        {
            float out_amt = p.uv_out_amount + (float)i * 1e-6f; // stable tie-break
            if (out_amt < best_out)
            {
                best_out = out_amt;
                best = i;
                best_proj = p;
            }
        }
    }

    if (best_out < 1e8f)
    {
        result.cascade = best;
        result.covered = true;
        result.proj = best_proj;
        return result;
    }

    return result;
}

struct ShadowResult
{
    float factor;
    uint effective_cascade;
    bool covered;
};

ShadowResult calculate_shadow_ex(float3 world_pos, float3 normal, uint initial_cascade)
{
    ShadowResult result;
    result.factor = 1.0f;
    result.effective_cascade = initial_cascade;
    result.covered = false;

    if (g_ubo.shadow_enabled == 0u || g_ubo.dir_enabled == 0u)
    {
        return result;
    }

    // Find a cascade that actually covers this fragment
    CascadeLookup lookup = find_covering_cascade(world_pos, initial_cascade);
    if (!lookup.covered)
    {
        // No cascade covers this fragment - treat as lit
        return result;
    }

    result.effective_cascade = lookup.cascade;
    result.covered = true;
    float2 uv = lookup.proj.uv_clamped;
    float z = lookup.proj.z_clamped;

    float3 light_dir = normalize(-g_ubo.dir_direction);
    float ndotl = saturate(dot(normal, light_dir));
    float base_bias = g_ubo.shadow_bias + g_ubo.shadow_normal_bias * (1.0f - ndotl);

    // Bias is in shadow-depth (0..1) units. Scaling it by world-space texel size
    // can easily over-bias far cascades and wipe out shadows. Keep bias stable
    // across cascades; tune per-scene via the config values.
    float bias = base_bias;

    if (result.effective_cascade == 0u)
        result.factor = sample_shadow_pcf(shadow_map_0, shadow_sampler_0, uv, z, bias, 0u);
    else if (result.effective_cascade == 1u)
        result.factor = sample_shadow_pcf(shadow_map_1, shadow_sampler_1, uv, z, bias, 1u);
    else if (result.effective_cascade == 2u)
        result.factor = sample_shadow_pcf(shadow_map_2, shadow_sampler_2, uv, z, bias, 2u);
    else
        result.factor = sample_shadow_pcf(shadow_map_3, shadow_sampler_3, uv, z, bias, 3u);

    return result;
}

float calculate_shadow(float3 world_pos, float3 normal, uint cascade)
{
    return calculate_shadow_ex(world_pos, normal, cascade).factor;
}

ShadowResult calculate_shadow_blended(float3 world_pos, float3 normal, float view_depth, uint initial_cascade)
{
    uint selected = clamp_cascade_index(initial_cascade);
    ShadowResult base = calculate_shadow_ex(world_pos, normal, selected);
    if (!base.covered)
    {
        return base;
    }

    uint count = min(max(g_ubo.shadow_cascade_count, 1u), 4u);
    if (count <= 1u)
    {
        return base;
    }

    // Blend near the selected cascade's far split to smooth transitions.
    // Range is expressed in view-space units (positive forward).
    float blend_range = max(g_ubo.shadow_cascade_blend_range, 0.0f);
    if (blend_range <= 0.0f)
    {
        return base;
    }

    uint next = min(selected + 1u, count - 1u);
    if (next == selected)
    {
        return base;
    }

    float split_far =
        (selected == 0u) ? g_ubo.shadow_split_far.x :
        (selected == 1u) ? g_ubo.shadow_split_far.y :
        (selected == 2u) ? g_ubo.shadow_split_far.z :
                           g_ubo.shadow_split_far.w;

    float start = split_far - blend_range;
    float t = saturate((view_depth - start) / max(blend_range, 1e-4f));
    if (t <= 0.0f)
    {
        return base;
    }

    ShadowResult alt = calculate_shadow_ex(world_pos, normal, next);
    if (!alt.covered)
    {
        return base;
    }

    ShadowResult out_r = base;
    out_r.factor = lerp(base.factor, alt.factor, t);
    // Keep effective_cascade as the base cascade so debug modes remain readable.
    return out_r;
}

float sample_shadow_depth(uint cascade, float2 uv)
{
    // SampleCmp samplers do not reliably return raw depth via Sample().
    // Use Load() for debug visualization (nearest, no sampler).
    uint dim_x = 1u;
    uint dim_y = 1u;
    if (cascade == 0u) shadow_map_0.GetDimensions(dim_x, dim_y);
    else if (cascade == 1u) shadow_map_1.GetDimensions(dim_x, dim_y);
    else if (cascade == 2u) shadow_map_2.GetDimensions(dim_x, dim_y);
    else shadow_map_3.GetDimensions(dim_x, dim_y);

    int2 p = int2(clamp(uv, 0.0f, 1.0f) * float2((float)max(dim_x - 1u, 1u),
                                                 (float)max(dim_y - 1u, 1u)));

    if (cascade == 0u) return shadow_map_0.Load(int3(p, 0)).r;
    if (cascade == 1u) return shadow_map_1.Load(int3(p, 0)).r;
    if (cascade == 2u) return shadow_map_2.Load(int3(p, 0)).r;
    return shadow_map_3.Load(int3(p, 0)).r;
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
                             float4 ambient_color, uint32_t render_mode,
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

    float4 ambient = float4(0.0, 0.0, 0.0, 0.0); 
    float4 diffuse = light.color * diff;
    float4 specular = light.color * spec;
    specular *= local_ubo.specular_color;

    if (render_mode == RENDER_MODE_DEFAULT)
    {
        diffuse *= diffuse_sample;
        specular *= specular_mask;
        specular *= local_ubo.specular_color;
        specular.a = diffuse_sample.a;
    }
    else
    {
        diffuse *= local_ubo.diffuse_color;
    }

    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    return ambient + diffuse + specular;
}

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    float3 normal = compute_normal_from_map(input);

    if (input.render_mode == RENDER_MODE_NORMAL)
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

    if (input.render_mode == RENDER_MODE_UNLIT)
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

    float3 view_direction = normalize(input.view_position - input.frag_position);

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
    }

    if (g_ubo.dir_enabled != 0)
    {
        DirectionalLight dir_light;
        dir_light.direction = g_ubo.dir_direction;
        dir_light.color = g_ubo.dir_color;

        color += calculate_directional_light(dir_light, normal,
                                             view_direction,
                                             input.ambient_color,
                                             input.render_mode, diffuse_sample,
                                             specular_sample, shadow);
    }
    else
    {
        // No directional light - add ambient only
        float4 ambient = float4(input.ambient_color.rgb * local_ubo.diffuse_color.rgb, diffuse_sample.a);
        if (input.render_mode == RENDER_MODE_DEFAULT)
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
                                       input.ambient_color,
                                       input.render_mode, diffuse_sample,
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

    if (g_ubo.shadow_debug_mode == 8u)
    {
        uint count = max(g_ubo.shadow_cascade_count, 1u);
        count = min(count, 4u);

        ShadowProjection sel = project_shadow(input.frag_position, cascade);
        bool sel_in = (!sel.uv_out && !sel.z_out);

        bool any_in = sel_in;
        uint cover = cascade;
        [unroll]
        for (uint i = 0u; i < 4u; ++i)
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
            float border_width = max(g_ubo.shadow_map_inv_size * 2.0f, 1e-6f);
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

    if (g_ubo.shadow_debug_mode == 9u)
    {
        float4 world_pos = float4(input.frag_position, 1.0f);
        float4 view_pos = mul(g_ubo.view, world_pos);
        float4 clip_pos = mul(g_ubo.projection, view_pos);

        float3 ndc = clip_pos.xyz / clip_pos.w;
        float2 uv = ndc.xy * 0.5f + 0.5f;

        uint count = max(g_ubo.shadow_cascade_count, 1u);
        count = min(count, 4u);

        float4 far_splits = g_ubo.shadow_split_far;
        float far_ref = (count == 1u) ? far_splits.x :
                        (count == 2u) ? far_splits.y :
                        (count == 3u) ? far_splits.z :
                                        far_splits.w;
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

        float d0 = abs(view_depth - far_splits.x);
        float l0 = saturate((thickness - d0) / thickness);
        split_c += l0 * float3(1.0f, 0.0f, 0.0f);
        w += l0;

        if (count >= 2u)
        {
            float d1 = abs(view_depth - far_splits.y);
            float l1 = saturate((thickness - d1) / thickness);
            split_c += l1 * float3(0.0f, 1.0f, 0.0f);
            w += l1;
        }

        if (count >= 3u)
        {
            float d2 = abs(view_depth - far_splits.z);
            float l2 = saturate((thickness - d2) / thickness);
            split_c += l2 * float3(0.0f, 0.0f, 1.0f);
            w += l2;
        }

        if (count >= 4u)
        {
            float d3 = abs(view_depth - far_splits.w);
            float l3 = saturate((thickness - d3) / thickness);
            split_c += l3 * float3(1.0f, 1.0f, 0.0f);
            w += l3;
        }

        c = lerp(c, split_c, saturate(w));

        color.rgb = c;
        color.a = 1.0f;
        return color;
    }

    if (g_ubo.shadow_debug_mode >= 4u && g_ubo.shadow_debug_mode <= 7u)
    {
        // Debug (mode=4..7): show raw shadow map contents (cascade 0..3) by
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

        uint map_index = g_ubo.shadow_debug_mode - 4u;
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

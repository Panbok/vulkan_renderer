struct VertexDto
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
VertexDto vertexMain(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f)
    };
    VertexDto output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    output.uv = positions[vertex_id] * 0.5f + 0.5f;
    return output;
}

static const float PI = 3.14159265359f;

float radical_inverse_vdc(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), radical_inverse_vdc(i));
}

float3 importance_sample_ggx(float2 xi, float roughness, float3 n)
{
    float alpha = roughness * roughness;
    float phi = 2.0f * PI * xi.x;
    float cos_theta = sqrt((1.0f - xi.y) /
                           (1.0f + (alpha * alpha - 1.0f) * xi.y));
    float sin_theta = sqrt(max(1.0f - cos_theta * cos_theta, 0.0f));
    float3 h = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
    float3 up = abs(n.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f)
                                  : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, n));
    return normalize(tangent * h.x + cross(n, tangent) * h.y + n * h.z);
}

float geometry_schlick_ggx(float no_v, float roughness)
{
    float k = roughness * roughness * 0.5f;
    return no_v / max(no_v * (1.0f - k) + k, 1e-5f);
}

float2 integrate_brdf(float no_v, float roughness)
{
    float3 v = float3(sqrt(max(1.0f - no_v * no_v, 0.0f)), 0.0f, no_v);
    float a = 0.0f;
    float b = 0.0f;
    const uint sample_count = 512u;
    for (uint i = 0u; i < sample_count; ++i)
    {
        float3 h = importance_sample_ggx(hammersley(i, sample_count),
                                        roughness, float3(0.0f, 0.0f, 1.0f));
        float3 l = normalize(2.0f * dot(v, h) * h - v);
        float no_l = max(l.z, 0.0f);
        float no_h = max(h.z, 0.0f);
        float vo_h = max(dot(v, h), 0.0f);
        if (no_l > 0.0f)
        {
            float visibility = geometry_schlick_ggx(no_v, roughness) *
                               geometry_schlick_ggx(no_l, roughness);
            float g_vis = visibility * vo_h / max(no_h * no_v, 1e-5f);
            float fc = pow(1.0f - vo_h, 5.0f);
            a += (1.0f - fc) * g_vis;
            b += fc * g_vis;
        }
    }
    return float2(a, b) / float(sample_count);
}

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    float2 dfg = integrate_brdf(clamp(input.uv.x, 1e-4f, 1.0f),
                                clamp(input.uv.y, 0.0f, 1.0f));
    return float4(dfg, 0.0f, 1.0f);
}

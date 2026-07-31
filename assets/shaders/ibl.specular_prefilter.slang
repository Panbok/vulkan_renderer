#include "vkr_shader_transform.slangh"

struct GlobalUniformBufferObject
{
    column_major float4x4 projection;
    column_major float4x4 view;
};

struct InstanceUniformBufferObject
{
    float roughness;
    float3 _padding;
};

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
    float3 direction : TEXCOORD;
};

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUniformBufferObject> g_ubo;

[[vk::binding(0, 1)]]
ConstantBuffer<InstanceUniformBufferObject> i_ubo;

[[vk::binding(1, 1)]]
TextureCube<float4> source_cubemap;

[[vk::binding(2, 1)]]
SamplerState source_sampler;

[shader("vertex")]
VertexDto vertexMain(VertexInput input)
{
    VertexDto output;
    output.direction = input.position;

    float4 world_pos = float4(input.position, 1.0f);
    output.position = vkr_clip_from_world(g_ubo.projection, g_ubo.view, world_pos);
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

float3 importance_sample_ggx(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0f * PI * xi.x;
    float cos_theta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sin_theta = sqrt(max(1.0f - cos_theta * cos_theta, 0.0f));

    float3 h;
    h.x = cos(phi) * sin_theta;
    h.y = sin(phi) * sin_theta;
    h.z = cos_theta;

    float3 up = abs(n.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f)
                                  : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);

    float3 sample_vec = tangent * h.x + bitangent * h.y + n * h.z;
    return normalize(sample_vec);
}

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    float3 n = normalize(input.direction);
    float3 r = n;
    float3 v = r;

    const uint sample_count = 256u;
    float3 prefiltered_color = float3(0.0f, 0.0f, 0.0f);
    float total_weight = 0.0f;

    for (uint i = 0u; i < sample_count; ++i)
    {
        float2 xi = hammersley(i, sample_count);
        float3 h = importance_sample_ggx(xi, n, i_ubo.roughness);
        float3 l = normalize(2.0f * dot(v, h) * h - v);

        float ndotl = max(dot(n, l), 0.0f);
        if (ndotl > 0.0f)
        {
            prefiltered_color += source_cubemap.Sample(source_sampler, l).rgb * ndotl;
            total_weight += ndotl;
        }
    }

    prefiltered_color /= max(total_weight, 1e-4f);
    return float4(prefiltered_color, 1.0f);
}

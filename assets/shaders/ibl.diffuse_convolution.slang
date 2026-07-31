#include "vkr_shader_transform.slangh"

struct GlobalUniformBufferObject
{
    column_major float4x4 projection;
    column_major float4x4 view;
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

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    float3 normal = normalize(input.direction);

    float3 up = abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f)
                                        : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, normal));
    up = normalize(cross(normal, right));

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    float sample_count = 0.0f;

    const float sample_delta = 0.025f;
    for (float phi = 0.0f; phi < 2.0f * PI; phi += sample_delta)
    {
        for (float theta = 0.0f; theta < 0.5f * PI; theta += sample_delta)
        {
            float3 tangent_sample =
                float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sample_dir = tangent_sample.x * right + tangent_sample.y * up +
                                tangent_sample.z * normal;

            irradiance +=
                source_cubemap.Sample(source_sampler, sample_dir).rgb *
                cos(theta) * sin(theta);
            sample_count += 1.0f;
        }
    }

    irradiance = PI * irradiance * (1.0f / max(sample_count, 1.0f));
    return float4(irradiance, 1.0f);
}

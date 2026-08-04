#include "vkr_shader_cube.slangh"

struct PushConstants
{
    float4 face_params;
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
    float2 face_uv : TEXCOORD;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push_constants;

[[vk::binding(1, 1)]]
Texture2D<float4> source_equirect;

[[vk::binding(2, 1)]]
SamplerState source_sampler;

[shader("vertex")]
VertexDto vertexMain(VertexInput input)
{
    VertexDto output;
    output.face_uv = input.texcoord;
    output.position = float4(input.texcoord * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

static const float PI = 3.14159265359f;

float2 direction_to_equirect_uv(float3 direction)
{
    float3 d = normalize(direction);
    return float2(atan2(d.z, d.x) * (0.5f / PI) + 0.5f,
                  acos(clamp(d.y, -1.0f, 1.0f)) / PI);
}

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    uint face = uint(push_constants.face_params.x + 0.5f);
    float3 direction = normalize(vkr_cube_face_uv_to_direction(face, input.face_uv));
    float3 direction_dx = ddx(direction);
    float3 direction_dy = ddy(direction);
    float3 radiance = float3(0.0f);

    [unroll]
    for (uint y = 0u; y < 4u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 4u; ++x)
        {
            float2 offset = (float2(float(x), float(y)) + 0.5f) / 4.0f - 0.5f;
            float3 sample_direction = normalize(
                direction + direction_dx * offset.x + direction_dy * offset.y);
            radiance += source_equirect.SampleLevel(
                source_sampler, direction_to_equirect_uv(sample_direction), 0.0f).rgb;
        }
    }

    return float4(radiance * (1.0f / 16.0f), 1.0f);
}

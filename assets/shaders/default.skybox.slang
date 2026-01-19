#include "vkr_shader_transform.slangh"

struct GlobalUniformBufferObject 
{
    column_major float4x4 projection;  // Must match C struct order!
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
    float3 texcoord : TEXCOORD;
};

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUniformBufferObject> g_ubo;

[shader("vertex")]
VertexDto vertexMain(VertexInput input)
{
    VertexDto output;
    
    // Pass position as cube map lookup direction (matches kohi)
    output.texcoord = input.position;
    
    // Simple projection * view * position (view has translation zeroed in C)
    float4 world_pos = float4(input.position, 1.0);
    output.position = vkr_clip_from_world(g_ubo.projection, g_ubo.view, world_pos);

    return output;
}

// Combined image sampler for cube map
[[vk::binding(1, 1)]]
TextureCube<float4> cube_texture;

[[vk::binding(2, 1)]]
SamplerState cube_sampler;

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    // Sample cube map using the interpolated direction
    return cube_texture.Sample(cube_sampler, input.texcoord);
}

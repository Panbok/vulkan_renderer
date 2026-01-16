struct InstanceUniformBufferObject
{
    float alpha_cutoff;
};

[[vk::binding(0, 1)]]
ConstantBuffer<InstanceUniformBufferObject> i_ubo;

struct PushConstantsObject
{
    column_major float4x4 light_view_projection;
    column_major float4x4 model;
};

[[vk::push_constant]]
ConstantBuffer<PushConstantsObject> push_constants;

[[vk::binding(1, 1)]]
Texture2D<float4> diffuse_texture;

[[vk::binding(2, 1)]]
SamplerState diffuse_sampler;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 texcoord : TEXCOORD;
    [[vk::location(3)]] float4 color    : COLOR0;
    [[vk::location(4)]] float4 tangent  : TANGENT;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    float4 world_pos = mul(push_constants.model, float4(input.position, 1.0));
    output.position = mul(push_constants.light_view_projection, world_pos);
    output.texcoord = input.texcoord;
    return output;
}

// todo: we need to remove this, it causes major performance issues due to early z testing
[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    float cutoff = i_ubo.alpha_cutoff;
    if (cutoff > 0.0f)
    {
        float alpha = diffuse_texture.Sample(diffuse_sampler, input.texcoord).a;
        if (alpha < cutoff)
        {
            discard;
        }
    }
}

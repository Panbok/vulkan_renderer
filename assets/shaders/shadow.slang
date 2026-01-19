struct InstanceUniformBufferObject
{
    float alpha_cutoff;
};

struct InstanceData
{
    column_major float4x4 model;
    uint object_id;
    uint material_index;
    uint flags;
    uint _padding;
};

[[vk::binding(0, 1)]]
ConstantBuffer<InstanceUniformBufferObject> i_ubo;

[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> g_instances;

struct PushConstantsObject
{
    column_major float4x4 light_view_projection;
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
    [[vk::location(2)]] float2 texcoord : TEXCOORD;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instance_id : SV_InstanceID,
                        uint base_instance : SV_StartInstanceLocation)
{
    VertexOutput output;
    uint instance_index = instance_id + base_instance;
    InstanceData inst = g_instances[instance_index];
    float4 world_pos = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(push_constants.light_view_projection, world_pos);
    output.texcoord = input.texcoord;
    return output;
}

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

struct InstanceData
{
    column_major float4x4 model;
    uint object_id;
    uint material_index;
    uint flags;
    uint _padding;
};

[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> g_instances;

struct PushConstantsObject
{
    column_major float4x4 light_view_projection;
};

[[vk::push_constant]]
ConstantBuffer<PushConstantsObject> push_constants;

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
    return output;
}

[shader("fragment")]
void fragmentMain()
{
}

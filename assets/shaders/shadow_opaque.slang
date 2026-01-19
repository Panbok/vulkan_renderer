#include "vkr_shader_transform.slangh"
#include "vkr_shader_instance.slangh"

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
    float4 world_pos = vkr_world_from_model_3d(inst.model, input.position);
    output.position = mul(push_constants.light_view_projection, world_pos);
    return output;
}

[shader("fragment")]
void fragmentMain()
{
    // No-op: shadow pass only writes depth, no color output needed
}

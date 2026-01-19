#include "vkr_shader_transform.slangh"
#include "vkr_shader_instance.slangh"
#include "vkr_shader_alpha_cutout.slangh"

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
    float3 world_pos : TEXCOORD1;
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
    output.texcoord = input.texcoord;
    output.world_pos = world_pos.xyz;
    return output;
}

[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    vkr_apply_alpha_cutout_shadow(diffuse_texture, diffuse_sampler,
                                  input.texcoord, input.world_pos,
                                  i_ubo.alpha_cutoff);
}

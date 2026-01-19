#include "vkr_shader_transform.slangh"
#include "vkr_shader_instance.slangh"
#include "vkr_shader_alpha_cutout.slangh"

// Picking Shader
// Outputs object_id as R32_UINT for pixel-perfect object selection.
// Per-instance data is sourced from the instance buffer.

struct GlobalUBO {
    column_major float4x4 projection;
    column_major float4x4 view;
};

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUBO> g_ubo;

[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> g_instances;

[[vk::binding(0, 1)]]
ConstantBuffer<InstanceUniformBufferObject> i_ubo;

[[vk::binding(1, 1)]]
Texture2D<float4> diffuse_texture;

[[vk::binding(2, 1)]]
SamplerState diffuse_sampler;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 texcoord : TEXCOORD;
    [[vk::location(3)]] float4 color    : COLOR0;
    [[vk::location(4)]] float4 tangent  : TANGENT;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    nointerpolation uint32_t object_id : OBJECT_ID;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instance_id : SV_InstanceID,
                        uint base_instance : SV_StartInstanceLocation) {
    VertexOutput output;

    uint instance_index = instance_id + base_instance;
    InstanceData inst = g_instances[instance_index];
    float4 world_pos = vkr_world_from_model_3d(inst.model, input.position);
    output.position = vkr_clip_from_world(g_ubo.projection, g_ubo.view, world_pos);
    output.texcoord = input.texcoord;
    output.object_id = inst.object_id;

    return output;
}

[shader("fragment")]
uint32_t fragmentMain(VertexOutput input) : SV_Target {
    vkr_apply_alpha_cutout(diffuse_texture, diffuse_sampler, input.texcoord,
                           i_ubo.alpha_cutoff);

    return input.object_id;
}

// Picking Shader
// Outputs object_id as R32_UINT for pixel-perfect object selection.
// Per-instance data is sourced from the instance buffer.

struct GlobalUBO {
    column_major float4x4 projection;
    column_major float4x4 view;
};

struct InstanceUniformBufferObject {
    float alpha_cutoff;
};

struct InstanceData {
    column_major float4x4 model;
    uint object_id;
    uint material_index;
    uint flags;
    uint _padding;
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
    nointerpolation float alpha_cutoff : ALPHA_CUTOFF;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instance_id : SV_InstanceID,
                        uint base_instance : SV_StartInstanceLocation) {
    VertexOutput output;

    uint instance_index = instance_id + base_instance;
    InstanceData inst = g_instances[instance_index];
    float4 world_pos = mul(inst.model, float4(input.position, 1.0));
    float4 view_pos = mul(g_ubo.view, world_pos);
    output.position = mul(g_ubo.projection, view_pos);
    output.texcoord = input.texcoord;
    output.object_id = inst.object_id;
    output.alpha_cutoff = i_ubo.alpha_cutoff;

    return output;
}

[shader("fragment")]
uint32_t fragmentMain(VertexOutput input) : SV_Target {
    if (input.alpha_cutoff > 0.0f) {
        float alpha = diffuse_texture.Sample(diffuse_sampler, input.texcoord).a;
        if (alpha < input.alpha_cutoff) {
            discard;
        }
    }

    return input.object_id;
}

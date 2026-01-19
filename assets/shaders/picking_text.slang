#include "vkr_shader_transform.slangh"

// Picking Text Shader
// Outputs object_id as R32_UINT for pixel-perfect object selection.

struct GlobalUBO {
    column_major float4x4 projection;
    column_major float4x4 view;
};

struct PushConstants {
    column_major float4x4 model;
    uint32_t object_id;
};

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUBO> g_ubo;

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 texcoord : TEXCOORD;
    [[vk::location(2)]] float4 color    : COLOR;
};

struct VertexOutput {
    float4 position : SV_Position;
    nointerpolation uint32_t object_id : OBJECT_ID;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;

    float4 world_pos = vkr_world_from_model_2d(push.model, input.position);
    output.position = vkr_clip_from_world(g_ubo.projection, g_ubo.view, world_pos);
    output.object_id = push.object_id;

    return output;
}

[shader("fragment")]
uint32_t fragmentMain(VertexOutput input) : SV_Target {
    return input.object_id;
}

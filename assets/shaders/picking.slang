// Picking Shader
// Outputs object_id as R32_UINT for pixel-perfect object selection.
// The object_id is passed via push constants and output directly to the
// integer render target.

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
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 texcoord : TEXCOORD;
    [[vk::location(3)]] float4 color    : COLOR0;
    [[vk::location(4)]] float4 tangent  : TANGENT;
};

struct VertexOutput {
    float4 position : SV_Position;
    nointerpolation uint32_t object_id : OBJECT_ID;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;

    float4 world_pos = mul(push.model, float4(input.position, 1.0));
    float4 view_pos = mul(g_ubo.view, world_pos);
    output.position = mul(g_ubo.projection, view_pos);
    output.object_id = push.object_id;

    return output;
}

[shader("fragment")]
uint32_t fragmentMain(VertexOutput input) : SV_Target {
    return input.object_id;
}

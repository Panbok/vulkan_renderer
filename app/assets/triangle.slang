static const float2 positions[3] = {
    float2(0.0, -0.5),
    float2(0.5, 0.5),
    float2(-0.5, 0.5)
};

static const float3 colors[3] = {
    float3(1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(0.0, 0.0, 1.0)
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : VertexColor;
};

// Vertex shader entry point
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.color = colors[vertexId];
    return output;
}

// Fragment shader entry point
[shader("fragment")]
float4 fragmentMain(float3 color : VertexColor) : SV_Target
{
    return float4(color.rgb, 1.0);
}
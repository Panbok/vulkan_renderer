// Vertex shader data
static const float2 positions[3] = {
    float2(0.0, -0.5),
    float2(0.5, 0.5),
    float2(-0.5, 0.5)
};

// Vertex shader entry point
[shader("vertex")]
float4 vertexMain(uint vertexId : SV_VertexID) : SV_Position
{
    return float4(positions[vertexId], 0.0, 1.0);
}

// Fragment shader entry point
[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0); // Red color
}
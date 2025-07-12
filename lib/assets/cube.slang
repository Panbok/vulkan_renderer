// 3D Cube Shader - Vertex and Fragment stages
// Simple shader to test basic cube visibility

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR;
};

// Vertex shader entry point
[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    
    // Simple transformation to make sure cube is visible
    float3 pos = input.position;
    
    // Scale down the cube significantly 
    pos *= 0.3f;
    
    // Just pass through to clip space with small Z offset to see it's 3D
    output.position = float4(pos.x, pos.y, pos.z * 0.5f, 1.0);
    
    // Use bright magenta color to ensure visibility
    output.color = float3(1.0, 0.0, 1.0); // Bright magenta - should be very visible
    
    return output;
}

// Fragment shader entry point
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // Use the interpolated vertex color
    return float4(input.color, 1.0);
} 
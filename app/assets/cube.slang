// Uniform buffer for MVP matrices
struct UniformBufferObject 
{
    float4x4 model;
    float4x4 view;
    float4x4 projection;
};

[[vk::binding(0, 0)]]
ConstantBuffer<UniformBufferObject> ubo;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    
    // Transform vertex position using MVP matrices
    float4 worldPos = mul(ubo.model, float4(input.position, 1.0));
    float4 viewPos = mul(ubo.view, worldPos);
    output.position = mul(ubo.projection, viewPos);
    
    // Pass through color (or use position-based coloring)
    output.color = input.color;
    
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
} 
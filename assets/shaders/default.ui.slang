// Uniform buffer for MVP matrices
struct UniformBufferObject 
{
    column_major float4x4 view;
    column_major float4x4 projection;
};

struct LocalUniformObject
{
    float4 diffuse_color;
};

struct PushConstantsObject
{
    column_major float4x4 model;
};

[[vk::binding(1, 1)]]
Texture2D<float4> texture;

[[vk::binding(0, 0)]]
ConstantBuffer<UniformBufferObject> ubo;

// Instance UBO (set 1, binding 0)
[[vk::binding(0, 1)]]
ConstantBuffer<LocalUniformObject> local_ubo;

// Sampler in the same instance set: set 1, binding 2 (after image at binding 1)
[[vk::binding(2, 1)]]
SamplerState sampler;

[[vk::push_constant]]
ConstantBuffer<PushConstantsObject> push_constants;

struct VertexInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 texcoord : TEXCOORD;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    
    float4 worldPos = mul(push_constants.model, float4(input.position, 0.0, 1.0));
    float4 viewPos = mul(ubo.view, worldPos);
    float4 clipPos = mul(ubo.projection, viewPos);

    output.position = clipPos;
    output.texcoord = input.texcoord;

    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return texture.Sample(sampler, input.texcoord) * local_ubo.diffuse_color;
} 


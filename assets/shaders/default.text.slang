// Uniform buffer for MVP matrices
struct UniformBufferObject
{
    column_major float4x4 projection;
    column_major float4x4 view;
};

struct LocalUniformObject
{
    float4 diffuse_color;
    float  screen_px_range;
    float  font_mode;
    float2 _padding;
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
    [[vk::location(2)]] float4 color    : COLOR;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    float4 color    : COLOR;
};

float median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    float4 worldPos = mul(push_constants.model, float4(input.position, 0.0, 1.0));
    float4 viewPos = mul(ubo.view, worldPos);
    float4 clipPos = mul(ubo.projection, viewPos);

    output.position = clipPos;
    output.texcoord = input.texcoord;
    output.color = input.color;

    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float4 tint = input.color * local_ubo.diffuse_color;
    float alpha;

    if (local_ubo.font_mode > 0.5) // MSDF / MTSDF Mode
    {
        float4 sample = texture.Sample(sampler, input.texcoord);
        float sigDist = median(sample.r, sample.g, sample.b) - 0.5;
        alpha = clamp(sigDist / fwidth(sigDist) + 0.5, 0.0, 1.0);
    }
    else // Bitmap Mode
    {
        alpha = texture.Sample(sampler, input.texcoord).a;
    }

    return float4(tint.rgb, tint.a * alpha);
}

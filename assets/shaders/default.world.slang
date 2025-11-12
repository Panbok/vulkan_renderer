// Uniform buffer for MVP matrices
struct GlobalUniformBufferObject 
{
    column_major float4x4 view;
    column_major float4x4 projection;
    float4 ambient_color;
    float3 view_position;
    uint32_t render_mode;
};

struct LocalUniformObject
{
    float4 diffuse_color;
    float4 specular_color;
    float shininess;
};

struct DirectionalLight
{
    float3 direction;
    float4 color;
};

struct PointLight
{
    float3 position;
    float4 color;
    // Usually 1, make sure denominator never gets smaller than 1
    float constant;
    // Reduces light intensity linearly
    float linear;
    // Makes the light fall off slower at longer distances.
    float quadratic;
};

static const DirectionalLight directional_light = {
    float3(-0.57735, -0.57735, -0.57735),
    float4(0.4, 0.4, 0.4, 1.0)
};

// Green light
static const PointLight point_light_0 = {
    float3(-1.5, 0.0, -5.5),
    float4(0.0, 1.0, 0.0, 1.0),
    1.0, // Constant
    0.35, // Linear
    0.44  // Quadratic
};

// Red light
static const PointLight point_light_1 = {
    float3(3.5, 0.0, -5.5),
    float4(1.0, 0.0, 0.0, 1.0),
    1.0, // Constant
    0.35, // Linear
    0.44  // Quadratic
};

struct PushConstantsObject
{
    column_major float4x4 model;
};

[[vk::binding(1, 1)]]
Texture2D<float4> diffuse_texture;

[[vk::binding(2, 1)]]
Texture2D<float4> specular_texture;

[[vk::binding(3, 1)]]
Texture2D<float4> normal_texture;

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUniformBufferObject> g_ubo;

// Instance UBO (set 1, binding 0)
[[vk::binding(0, 1)]]
ConstantBuffer<LocalUniformObject> local_ubo;

// Samplers in the same instance set: set 1, bindings 1+instance_texture_count (here: 4..6)
[[vk::binding(4, 1)]]
SamplerState diffuse_sampler;

[[vk::binding(5, 1)]]
SamplerState specular_sampler;

[[vk::binding(6, 1)]]
SamplerState normal_sampler;

[[vk::push_constant]]
ConstantBuffer<PushConstantsObject> push_constants;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 texcoord : TEXCOORD;
    [[vk::location(3)]] float4 color    : COLOR0;
    [[vk::location(4)]] float4 tangent  : TANGENT;
};

struct VertexDto
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    float3 frag_position;
    float4 ambient_color;
    float3 normal;
    float4 tangent;
    float4 color;
    float3 view_position;
    uint32_t render_mode;
};

[shader("vertex")]
VertexDto vertexMain(VertexInput input)
{
    VertexDto output;

    output.texcoord = input.texcoord;
    output.color = input.color;

    output.frag_position = float3(mul(push_constants.model, float4(input.position, 1.0)).xyz);

    float3x3 model_matrix = float3x3(push_constants.model);
    output.normal = mul(model_matrix, input.normal);
    output.tangent = float4(normalize(mul(model_matrix, input.tangent.xyz)), input.tangent.w);

    output.ambient_color = g_ubo.ambient_color;
    output.view_position = g_ubo.view_position;
    output.render_mode = g_ubo.render_mode;
    
    float4 worldPos = mul(push_constants.model, float4(input.position, 1.0));
    float4 viewPos = mul(g_ubo.view, worldPos);
    float4 clipPos = mul(g_ubo.projection, viewPos);

    output.position = clipPos;

    return output;
}

static const uint32_t RENDER_MODE_DEFAULT = 0u;
static const uint32_t RENDER_MODE_LIGHTING = 1u;
static const uint32_t RENDER_MODE_NORMAL = 2u;

float3 compute_normal_from_map(VertexDto input)
{
    float3 normal = input.normal;
    float3 tangent = input.tangent.xyz;
    tangent = tangent - dot(tangent, normal) * normal;
    float3 bitangent = cross(normal, tangent) * input.tangent.w;
    float3x3 tbn = float3x3(tangent, bitangent, normal);

    float3 sampled = normal_texture.Sample(normal_sampler, input.texcoord).xyz;
    sampled = (sampled * 2.0f) - 1.0f;
    return normalize(mul(tbn, sampled));
}

float4 calculate_directional_light(DirectionalLight light, float3 normal,
                                   float3 view_direction, float4 ambient_color,
                                   uint32_t render_mode, float4 diffuse_sample,
                                   float4 specular_sample)
{
    float diffuse_factor = max(dot(normal, -light.direction), 0.0f);
    float3 half_direction = normalize(view_direction - light.direction);
    float specular_factor =
        pow(max(dot(half_direction, normal), 0.0f), local_ubo.shininess);

    float4 ambient =
        float4(ambient_color.rgb * local_ubo.diffuse_color.rgb, diffuse_sample.a);
    float4 diffuse =
        float4(light.color.rgb * diffuse_factor, diffuse_sample.a);
    float4 specular =
        float4(light.color.rgb * specular_factor, diffuse_sample.a);
    specular *= local_ubo.specular_color;

    if (render_mode == RENDER_MODE_DEFAULT)
    {
        ambient *= diffuse_sample;
        diffuse *= diffuse_sample;
        specular *= float4(specular_sample.rgb, diffuse_sample.a);
    }
    else
    {
        diffuse *= local_ubo.diffuse_color;
    }

    return ambient + diffuse + specular;
}

float4 calculate_point_light(PointLight light, float3 normal,
                             float3 frag_position, float3 view_direction,
                             float4 ambient_color, uint32_t render_mode,
                             float4 diffuse_sample, float4 specular_sample)
{
    float3 light_direction = normalize(light.position - frag_position);
    float diff = max(dot(normal, light_direction), 0.0f);
    float3 half_direction = normalize(view_direction + light_direction);
    float spec = pow(max(dot(half_direction, normal), 0.0f), local_ubo.shininess);

    float distance = length(light.position - frag_position);
    float attenuation =
        1.0f /
        (light.constant + light.linear * distance +
         light.quadratic * (distance * distance));

    float4 ambient = ambient_color.rgba;
    float4 diffuse = light.color * diff;
    float4 specular = light.color * spec;
    specular *= local_ubo.specular_color;

    if (render_mode == RENDER_MODE_DEFAULT)
    {
        ambient *= diffuse_sample;
        diffuse *= diffuse_sample;
        specular *= float4(specular_sample.rgb, diffuse_sample.a);
    }
    else
    {
        diffuse *= local_ubo.diffuse_color;
    }

    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    return ambient + diffuse + specular;
}

[shader("fragment")]
float4 fragmentMain(VertexDto input) : SV_Target
{
    float3 normal = compute_normal_from_map(input);

    if (input.render_mode == RENDER_MODE_NORMAL)
    {
        return float4(abs(normal), 1.0f);
    }

    float4 diffuse_sample = diffuse_texture.Sample(diffuse_sampler, input.texcoord);
    float4 specular_sample = specular_texture.Sample(specular_sampler, input.texcoord);

    float3 view_direction = normalize(input.view_position - input.frag_position);

    float4 color = calculate_directional_light(directional_light, normal,
                                               view_direction, input.ambient_color,
                                               input.render_mode, diffuse_sample,
                                               specular_sample);

    color += calculate_point_light(point_light_0, normal, input.frag_position,
                                   view_direction, input.ambient_color,
                                   input.render_mode, diffuse_sample,
                                   specular_sample);
    color += calculate_point_light(point_light_1, normal, input.frag_position,
                                   view_direction, input.ambient_color,
                                   input.render_mode, diffuse_sample,
                                   specular_sample);
    return color;
} 

struct VkrMetalPacketVertex
{
    float3 position;
    float3 normal;
    float2 texcoord;
    float4 color;
    float4 tangent;
};

struct VkrBindlessMaterial
{
    float4 tint;
    uint base_color_texture;
    uint normal_texture;
    uint orm_texture;
    uint emissive_texture;
    uint base_color_sampler;
    uint normal_sampler;
    uint orm_sampler;
    uint emissive_sampler;
    uint material_id;
    uint flags;
    uint2 reserved;
};

struct VkrBindlessDrawRoot
{
    VkrMetalPacketVertex* vertices;
    float4 tint;
    float4x4 transform;
    uint texture_index;
    uint sampler_index;
    VkrBindlessMaterial* materials;
};

struct VkrBindlessPushConstants
{
    VkrBindlessDrawRoot* root;
    uint material_index;
    uint flags;
};

struct VkrBindlessVertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(0, 1)]] SamplerState g_samplers[];

[[vk::push_constant]] ConstantBuffer<VkrBindlessPushConstants> g_push;

[shader("vertex")]
VkrBindlessVertexOutput vert_main(uint vertex_id : SV_VulkanVertexID)
{
    VkrMetalPacketVertex vertex = g_push.root.vertices[vertex_id];

    VkrBindlessVertexOutput output;
    output.position = mul(float4(vertex.position, 1.0f),
                          g_push.root.transform);
    output.uv = vertex.texcoord;
    return output;
}

[shader("fragment")]
float4 frag_main(VkrBindlessVertexOutput input) : SV_Target0
{
    VkrBindlessMaterial material = g_push.root.materials[g_push.material_index];
    uint texture_index = NonUniformResourceIndex(material.base_color_texture);
    uint sampler_index = NonUniformResourceIndex(material.base_color_sampler);
    return g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv) *
           material.tint;
}

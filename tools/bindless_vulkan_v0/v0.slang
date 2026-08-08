struct V0Vertex
{
    float2 position;
    float2 uv;
};

struct V0DrawRoot
{
    V0Vertex* vertices;
    float4 tint;
    float4x4 transform;
    uint texture_index;
    uint sampler_index;
};

struct V0PushConstants
{
    V0DrawRoot* root;
    uint material_index;
    uint flags;
};

struct V0VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(0, 1)]] SamplerState g_samplers[];

[[vk::push_constant]] ConstantBuffer<V0PushConstants> g_push;

[shader("vertex")]
V0VertexOutput vert_main(uint vertex_id : SV_VulkanVertexID)
{
    V0Vertex vertex = g_push.root.vertices[vertex_id];

    V0VertexOutput output;
    output.position = mul(float4(vertex.position, 0.0f, 1.0f),
                          g_push.root.transform);
    output.uv = vertex.uv;
    return output;
}

[shader("fragment")]
float4 frag_main(V0VertexOutput input) : SV_Target0
{
    uint texture_index = NonUniformResourceIndex(g_push.root.texture_index);
    uint sampler_index = NonUniformResourceIndex(g_push.root.sampler_index);
    return g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv) *
           g_push.root.tint;
}

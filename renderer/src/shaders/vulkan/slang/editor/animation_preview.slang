struct VkrAnimationPreviewRoot
{
    column_major float4x4 model;
    column_major float4x4 view_projection;
    VkrPackedStaticVertex *vertices;
    VkrGpuGeometryDecodeRecord *decode;
    uint64_t deformation_address;
    uint64_t reserved;
    float4 tint;
    uint first_vertex;
    uint decode_index;
    uint64_t reserved_tail;
};

struct VkrAnimationPreviewPush
{
    VkrAnimationPreviewRoot *root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrAnimationPreviewPush> g_animation_preview_push;

struct VkrAnimationPreviewOutput
{
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VkrAnimationPreviewOutput animation_preview_vertex(uint vertex_id : SV_VertexID)
{
    VkrAnimationPreviewRoot *root = g_animation_preview_push.root;
    VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
        root.vertices[root.first_vertex + vertex_id], root.decode[root.decode_index]);
    vertex = vkr_apply_deformation(vertex, root.deformation_address, vertex_id);
    VkrSkinningFrame transformed = vkr_skinning_frame(
        vertex.position, vertex.normal, vertex.tangent,
        mul(root.model, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz,
        mul(root.model, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz,
        mul(root.model, float4(0.0f, 0.0f, 1.0f, 0.0f)).xyz,
        mul(root.model, float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz);
    VkrAnimationPreviewOutput output;
    output.position = mul(root.view_projection, float4(transformed.position, 1.0f));
    output.normal = transformed.normal;
    output.color = vertex.color * root.tint;
    return output;
}

[shader("fragment")]
float4 animation_preview_fragment(VkrAnimationPreviewOutput input,
                                   bool front_facing : SV_IsFrontFace) : SV_Target0
{
    float3 normal = normalize(input.normal) * (front_facing ? 1.0f : -1.0f);
    float diffuse = max(dot(normal, normalize(float3(-0.5f, 0.8f, 0.6f))), 0.0f);
    return float4(input.color.rgb * (0.22f + 0.7f * diffuse), 1.0f);
}

struct VkrMetalPacketAnimationPreviewRoot
{
    float4x4 model;
    float4x4 view_projection;
    device const VkrPackedStaticVertex *vertices;
    device const VkrGpuGeometryDecodeRecord *decode;
    ulong deformation_address;
    ulong reserved;
    float4 tint;
    uint first_vertex;
    uint decode_index;
    ulong reserved_tail;
};

struct VkrAnimationPreviewOutput
{
    float4 position [[position]];
    float3 normal [[user(NORMAL)]];
    float4 color [[user(COLOR)]];
};

vertex VkrAnimationPreviewOutput vkr_metal_packet_animation_preview_vertex(
    uint vertex_id [[vertex_id]],
    constant VkrMetalPacketAnimationPreviewRoot &root [[buffer(1)]])
{
    VkrGpuDecodedVertex decoded = vkr_decode_packed_vertex(
        root.vertices[root.first_vertex + vertex_id], root.decode[root.decode_index]);
    decoded = vkr_apply_deformation(decoded, root.deformation_address, vertex_id);
    VkrSkinningFrame transformed = vkr_skinning_frame(
        decoded.position, decoded.normal, decoded.tangent,
        (root.model * float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz,
        (root.model * float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz,
        (root.model * float4(0.0f, 0.0f, 1.0f, 0.0f)).xyz,
        (root.model * float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz);
    VkrAnimationPreviewOutput output;
    output.position = (root.view_projection * float4(transformed.position, 1.0f));
    output.normal = transformed.normal;
    output.color = decoded.color * root.tint;
    return output;
}

fragment float4 vkr_metal_packet_animation_preview_fragment(
    VkrAnimationPreviewOutput input [[stage_in]],
    bool front_facing [[front_facing]])
{
    float3 normal = normalize(input.normal) * (front_facing ? 1.0f : -1.0f);
    float diffuse = max(dot(normal, normalize(float3(-0.5f, 0.8f, 0.6f))), 0.0f);
    return float4(input.color.rgb * (0.22f + 0.7f * diffuse), 1.0f);
}

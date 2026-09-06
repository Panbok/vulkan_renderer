struct VkrVulkanEditorOverlayRoot
{
    VkrPackedStaticVertex* vertices;
    VkrGpuGeometryDecodeRecord* decode;
    float4x4 model_view_projection;
    float4 color;
    uint first_vertex;
    uint decode_index;
    uint object_id;
    uint reserved;
};

struct VkrVulkanEditorOverlayPush
{
    VkrVulkanEditorOverlayRoot* root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanEditorOverlayPush> g_editor_overlay_push;

struct VkrVulkanEditorOverlayOutput
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
    nointerpolation uint object_id : TEXCOORD0;
};

[shader("vertex")]
VkrVulkanEditorOverlayOutput editor_overlay_vertex(uint vertex_id : SV_VertexID)
{
    VkrVulkanEditorOverlayRoot* root = g_editor_overlay_push.root;
    VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
        root.vertices[root.first_vertex + vertex_id], root.decode[root.decode_index]);
    VkrVulkanEditorOverlayOutput output;
    output.position = mul(root.model_view_projection, float4(vertex.position, 1.0f));
    output.color = root.color;
    output.object_id = root.object_id;
    return output;
}

[shader("fragment")]
float4 editor_overlay_fragment(VkrVulkanEditorOverlayOutput input) : SV_Target0
{
    return input.color;
}

[shader("fragment")]
uint editor_overlay_picking_fragment(VkrVulkanEditorOverlayOutput input) : SV_Target0
{
    return input.object_id;
}

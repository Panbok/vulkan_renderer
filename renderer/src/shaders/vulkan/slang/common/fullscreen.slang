[shader("vertex")]
VkrVulkanVertexOutput fullscreen_vertex(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(3.0f, -1.0f),
        float2(-1.0f, 3.0f),
    };
    VkrVulkanVertexOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    output.uv = positions[vertex_id] * 0.5f + 0.5f;
    output.color = 1.0f;
    output.world_position = 0.0f;
    output.world_normal = float3(0.0f, 0.0f, 1.0f);
    output.world_tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
    output.object_id = 0u;
    return output;
}

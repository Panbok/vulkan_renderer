[shader("vertex")]
VkrMetalPacketVertexOutput
vkr_metal_packet_vertex(uint vertex_id : SV_VertexID,
                        uint instance_id : SV_InstanceID,
                        ParameterBlock<VkrMetalPacketDrawRoot> root
                            : register(b0))
{
    VkrMetalPacketFrameRoot frame = root.frame;
    VkrGpuVisibleDrawRow visible = root.visible_rows[root.visible_row_index];
    VkrGpuGeometryRow geometry = root.geometry_rows[visible.geometry_index];
    VkrMetalPacketVertex vertex =
        root.vertices[geometry.first_vertex + vertex_id];
    VkrMetalPacketInstance instance =
        frame.instances[visible.instance_index + instance_id];
    VkrMetalPacketVertexOutput output;
    float3 position =
        float3(vertex.position_x, vertex.position_y, vertex.position_z);
    float4 world_position = mul(instance.model, float4(position, 1.0));
    output.position = mul(frame.view_projection, world_position);
    output.texcoord = vertex.texcoord;
    output.color = vertex.color;
    output.object_id = instance.object_id;
    output.world_position = world_position.xyz;
    output.world_normal = normalize(mul(instance.model,
                                        float4(vertex.normal_x,
                                               vertex.normal_y,
                                               vertex.normal_z, 0.0)).xyz);
    output.world_tangent =
        float4(normalize(mul(instance.model,
                             float4(vertex.tangent.xyz, 0.0)).xyz),
               vertex.tangent.w);
    return output;
}

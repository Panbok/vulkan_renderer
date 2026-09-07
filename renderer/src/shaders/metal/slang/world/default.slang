struct VkrMetalPacketVertexData
{
    float4 position;
    float2 texcoord;
    float4 color;
    uint object_id;
    float3 world_position;
    float3 world_normal;
    float4 world_tangent;
    uint visible_row_index;
    float3 object_position;
    uint temporal_index;
    uint temporal_generation;
    uint temporal_flags;
};

VkrMetalPacketVertexData
vkr_metal_packet_vertex_data(uint vertex_id, uint instance_id,
                             ParameterBlock<VkrMetalPacketDrawRoot> root)
{
    VkrMetalPacketFrameRoot frame = root.frame;
    bool gpu_draw = (root.flags & 1u) != 0u;
    uint visible_row_index = gpu_draw ? instance_id : root.visible_row_index;
    VkrGpuVisibleDrawRow visible = root.visible_rows[visible_row_index];
    VkrGpuGeometryRow geometry = root.geometry_rows[visible.geometry_index];
    VkrPackedStaticVertex* vertices =
        (VkrPackedStaticVertex*)geometry.vertex_address;
    VkrGpuGeometryDecodeRecord* decode =
        (VkrGpuGeometryDecodeRecord*)geometry.decode_address;
    VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
        vertices[geometry.first_vertex + vertex_id],
        decode[visible.decode_index]);
    VkrMetalPacketInstance instance =
        frame.instances[visible.instance_index + (gpu_draw ? 0u : instance_id)];
    VkrMetalPacketVertexData output;
    float4 world_position = mul(instance.model, float4(vertex.position, 1.0));
    output.position = mul(frame.view_projection, world_position);
    output.texcoord = vertex.texcoord;
    output.color = vertex.color;
    output.object_id = instance.object_id;
    output.world_position = world_position.xyz;
    output.world_normal = normalize(vkr_instance_transform_normal(
        instance.normal_column0.xyz, instance.normal_column1.xyz,
        instance.normal_column2.xyz, vertex.normal));
    output.world_tangent =
        float4(normalize(mul(instance.model,
                             float4(vertex.tangent.xyz, 0.0)).xyz),
               vertex.tangent.w * instance.normal_column0.w);
    output.visible_row_index = visible_row_index;
    output.object_position = vertex.position;
    output.temporal_index = instance.temporal_index;
    output.temporal_generation = instance.temporal_generation;
    output.temporal_flags = instance.temporal_flags;
    return output;
}

[shader("vertex")]
VkrMetalPacketVertexOutput
vkr_metal_packet_vertex(uint vertex_id : SV_VertexID,
                        uint instance_id : SV_InstanceID,
                        ParameterBlock<VkrMetalPacketDrawRoot> root
                            : register(b0))
{
    VkrMetalPacketVertexData data =
        vkr_metal_packet_vertex_data(vertex_id, instance_id, root);
    VkrMetalPacketVertexOutput output;
    output.position = data.position;
    output.texcoord = data.texcoord;
    output.color = data.color;
    output.object_id = data.object_id;
    output.world_position = data.world_position;
    output.world_normal = data.world_normal;
    output.world_tangent = data.world_tangent;
    output.visible_row_index = data.visible_row_index;
    return output;
}

[shader("vertex")]
VkrMetalPacketTemporalVertexOutput
vkr_metal_packet_temporal_vertex(
    uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID,
    ParameterBlock<VkrMetalPacketDrawRoot> root : register(b0))
{
    VkrMetalPacketVertexData data =
        vkr_metal_packet_vertex_data(vertex_id, instance_id, root);
    VkrMetalPacketTemporalVertexOutput output;
    output.position = data.position;
    output.texcoord = data.texcoord;
    output.color = data.color;
    output.object_id = data.object_id;
    output.world_position = data.world_position;
    output.world_normal = data.world_normal;
    output.world_tangent = data.world_tangent;
    output.visible_row_index = data.visible_row_index;
    output.object_position = data.object_position;
    output.temporal_index = data.temporal_index;
    output.temporal_generation = data.temporal_generation;
    output.temporal_flags = data.temporal_flags;
    return output;
}

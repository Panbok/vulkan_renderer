[shader("vertex")]
VkrMetalPacketVertexOutput
vkr_metal_packet_vertex(uint vertex_id : SV_VertexID,
                        uint instance_id : SV_InstanceID,
                        ParameterBlock<VkrMetalPacketDrawRoot> root
                            : register(b0))
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
    vertex = vkr_apply_deformation(vertex, instance.deformation_address, vertex_id);
    // Terrain tiles morph toward their next level (ADR-084). The tiled
    // pipeline keeps no motion vectors, so the previous height goes unused.
    float previous_height;
    vertex = vkr_gpu_terrain_morph(vertex, vertices + geometry.first_vertex,
                                   decode[visible.decode_index], vertex_id,
                                   visible.state_flags, previous_height);
    VkrMetalPacketVertexOutput output;
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
    output.instance_index = visible.instance_index + (gpu_draw ? 0u : instance_id);
    return output;
}

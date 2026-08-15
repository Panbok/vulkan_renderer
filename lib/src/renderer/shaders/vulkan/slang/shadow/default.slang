[shader("vertex")]
VkrBindlessVertexOutput shadow_vertex(uint vertex_id : SV_VertexID,
                                      uint instance_id : SV_InstanceID)
{
    return packet_vertex_impl(vertex_id, instance_id,
                              g_push.root.frame.view_projection);
}
[shader("fragment")]
void shadow_fragment(VkrBindlessVertexOutput input)
{
    VkrGpuVisibleDrawRow visible =
        g_push.root.visible_rows[g_push.root.visible_row_index];
    VkrBindlessMaterial material =
        g_push.root.frame.materials[visible.material_index];
    if ((g_push.flags & 1u) != 0u &&
        packet_base_color(input).a < material.material_alpha.x)
        discard;
}

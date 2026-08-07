struct VkrMetalPacketPickingOutput
{
    uint object_id : SV_Target0;
};

[shader("fragment")]
VkrMetalPacketPickingOutput
vkr_metal_packet_picking_fragment(VkrMetalPacketVertexOutput input)
{
    VkrMetalPacketPickingOutput output;
    output.object_id = input.object_id;
    return output;
}

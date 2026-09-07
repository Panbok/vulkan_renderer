[shader("fragment")]
uint picking_fragment(VkrVulkanVertexOutput input) : SV_Target0
{
    return input.object_id;
}

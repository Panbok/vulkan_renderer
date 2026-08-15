[shader("fragment")]
uint picking_fragment(VkrBindlessVertexOutput input) : SV_Target0
{
    return input.object_id;
}

[shader("fragment")]
float4 fullscreen_fragment(VkrVulkanVertexOutput input) : SV_Target0
{
    VkrVulkanPacketUtilityRoot* root =
        (VkrVulkanPacketUtilityRoot*)g_push.root;
    uint texture_index =
        NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index =
        NonUniformResourceIndex(root.transmission_sampler);
    float4 source =
        g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv);
    if ((g_push.flags & 2u) != 0u)
    {
        source.rgb = vkr_aces_fitted(
            max(source.rgb * root.ibl_controls.x, 0.0f));
    }
    return source;
}

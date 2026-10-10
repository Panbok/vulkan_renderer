// Sparse diffuse volume composition (DVOL v3): the active light layers'
// weighted SH, written in place into the volume's SH texture whenever the
// sun turns or a light group factor changes (ADR-090).

struct VkrVulkanDiffuseVolumeComposeRoot
{
    uint layer_sh_texture;
    uint sh_storage;
    uint probe_count;
    uint rows;
    uint active_layer_count;
    // Row offset of the lamp groups' part, zero when they join the sum; bit
    // i of lamp_mask marks active layer i a lamp group.
    uint lamp_rows;
    uint lamp_mask;
    uint reserved;
    uint4 active_layers[2];
    float4 active_weights[2];
    // Band of an active lamp group's direct light, 0xffffffff without; it
    // joins only the part kept apart.
    uint4 active_direct[2];
};

struct VkrVulkanDiffuseVolumeComposePush
{
    VkrVulkanDiffuseVolumeComposeRoot *root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanDiffuseVolumeComposePush>
    g_diffuse_volume_compose_push;

// One thread per probe.
[shader("compute")]
[numthreads(64, 1, 1)]
void diffuse_volume_compose(uint3 id : SV_DispatchThreadID)
{
    VkrVulkanDiffuseVolumeComposeRoot root =
        vkr_vk_uniform_root(*g_diffuse_volume_compose_push.root);
    uint probe = id.x;
    if (probe >= root.probe_count)
        return;
    float4 sum[3] = { float4(0.0f), float4(0.0f), float4(0.0f) };
    float4 lamps[3] = { float4(0.0f), float4(0.0f), float4(0.0f) };
    for (uint a = 0u; a < root.active_layer_count; ++a)
    {
        uint layer = root.active_layers[a / 4u][a % 4u];
        float weight = root.active_weights[a / 4u][a % 4u];
        bool apart = root.lamp_rows != 0u && (root.lamp_mask & (1u << a)) != 0u;
        uint direct = root.active_direct[a / 4u][a % 4u];
        [unroll] for (uint channel = 0u; channel < 3u; ++channel)
        {
            uint2 texel = vkr_diffuse_volume_sh_texel(probe, channel,
                                                      layer * root.rows);
            float4 value = weight *
                g_textures[root.layer_sh_texture].Load(int3(int2(texel), 0));
            if (apart)
            {
                if (direct != 0xffffffffu)
                {
                    uint2 direct_texel = vkr_diffuse_volume_sh_texel(
                        probe, channel, direct * root.rows);
                    value += weight * g_textures[root.layer_sh_texture].Load(
                        int3(int2(direct_texel), 0));
                }
                lamps[channel] += value;
            }
            else
            {
                sum[channel] += value;
            }
        }
    }
    [unroll] for (uint channel = 0u; channel < 3u; ++channel)
    {
        uint2 texel = vkr_diffuse_volume_sh_texel(probe, channel, 0u);
        g_storage_images[root.sh_storage][uint3(texel, 0u)] = sum[channel];
        if (root.lamp_rows != 0u)
        {
            uint2 lamp_texel =
                vkr_diffuse_volume_sh_texel(probe, channel, root.lamp_rows);
            g_storage_images[root.sh_storage][uint3(lamp_texel, 0u)] =
                lamps[channel];
        }
    }
}

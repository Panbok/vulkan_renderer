struct VkrVulkanFogRoot
{
    VkrFogParams params;
    float4x4 inverse_view_projection;
    float4 camera_position;
    uint depth_texture;
    uint target_texture;
    uint2 extent;
};

struct VkrVulkanFogPush
{
    VkrVulkanFogRoot* root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVulkanFogPush> g_fog_push;

float3 vkr_vk_fog_homogeneous(float4 value)
{
    return value.xyz / max(abs(value.w), 1e-7f) * sign(value.w);
}

float3 vkr_vk_fog_world_position(VkrVulkanFogRoot *root, uint2 pixel,
                                 float device_depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
    return vkr_vk_fog_homogeneous(
        mul(root.inverse_view_projection, float4(ndc, device_depth, 1.0f)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void fog_apply_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVulkanFogRoot *root = g_fog_push.root;
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.extent))
        return;
    float4 current = g_storage_2d[root.target_texture][pixel];
    float device_depth =
        g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    float3 fogged;
    if (device_depth >= 1.0f)
    {
        float3 far_world = vkr_vk_fog_world_position(root, pixel, 1.0f);
        fogged = vkr_fog_apply_sky(current.rgb, root.params,
                                   root.camera_position.xyz,
                                   far_world - root.camera_position.xyz);
    }
    else
    {
        fogged = vkr_fog_apply_surface(
            current.rgb, root.params, root.camera_position.xyz,
            vkr_vk_fog_world_position(root, pixel, device_depth));
    }
    g_storage_2d[root.target_texture][pixel] = float4(fogged, current.a);
}

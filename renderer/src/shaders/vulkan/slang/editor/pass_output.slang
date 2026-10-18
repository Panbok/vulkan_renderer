// Editor.PassOutput: one graph image over the Scene image, read texel by texel
// and coloured by the shared pass output kernel
// (VkrGpuDebugPayload.pass_output). A source without the output's aspect,
// such as a square shadow cascade, shows between dark bars, and one the
// frame's graph lacks as dark stripes.
struct VkrVulkanPassOutputRoot
{
    // x unused (the exposure comes from exposure_state), y and z the camera's
    // near and far planes in metres, w a vector source's display scale.
    float4 params;
    uint2 extent;
    // VkrPassOutputKind.
    uint kind;
    // The shadow map layer a SHADOW_DEPTH source shows.
    uint layer;
    VkrExposureState* exposure_state;
    VkrDisplayOutputParams* display_output;
    // The source's sampled slot over the whole image.
    uint source_texture;
    // Returns pre-exposed radiance to scene-linear before exposure.
    float inverse_pre_exposure;
    uint2 reserved;
};

struct VkrVulkanPassOutputPush
{
    VkrVulkanPassOutputRoot* root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanPassOutputPush> g_pass_output_push;

[shader("fragment")]
float4 pass_output_fragment(VkrVulkanEditorGridOutput input) : SV_Target0
{
    VkrVulkanPassOutputRoot* root = g_pass_output_push.root;
    uint2 pixel = uint2(input.position.xy);
    uint kind = root.kind;
    if (kind == VKR_PASS_OUTPUT_KIND_MISSING)
    {
        return float4(vkr_display_output_scale(
                          vkr_pass_output_missing_color(pixel),
                          root.display_output[0]),
                      1.0f);
    }
    uint index = NonUniformResourceIndex(root.source_texture);
    uint2 source_extent;
    if (kind == VKR_PASS_OUTPUT_KIND_SHADOW_DEPTH)
    {
        uint layers = 0u;
        g_depth_array_textures[index].GetDimensions(source_extent.x,
                                                    source_extent.y, layers);
    }
    else
    {
        g_textures[index].GetDimensions(source_extent.x, source_extent.y);
    }
    int2 texel = vkr_pass_output_texel(pixel, root.extent, source_extent);
    float3 color = float3(0.03f);
    if (texel.x >= 0)
    {
        float4 value;
        if (kind == VKR_PASS_OUTPUT_KIND_SHADOW_DEPTH)
        {
            value = float4(g_depth_array_textures[index].Load(
                int4(texel, int(root.layer), 0)));
        }
        else if (kind == VKR_PASS_OUTPUT_KIND_DEPTH)
        {
            value = float4(g_depth_textures[index].Load(int3(texel, 0)));
        }
        else
        {
            value = g_textures[index].Load(int3(texel, 0));
        }
        float4 params = root.params;
        params.x = root.exposure_state->exposure_multiplier *
                   root.inverse_pre_exposure;
        color = vkr_pass_output_color(kind, value, params);
    }
    return float4(vkr_display_output_scale(color, root.display_output[0]),
                  1.0f);
}

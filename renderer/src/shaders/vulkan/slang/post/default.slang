struct VkrVulkanFullscreenPush
{
    VkrVulkanPacketUtilityRoot* root;
    uint material_index;
    uint flags;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanFullscreenPush> g_fullscreen_push;

// Set only by the editor Scene composite, whose sampled image has already
// passed the final output scale.
static const uint VKR_FULLSCREEN_ALREADY_OUTPUT_ENCODED = 1u << 0u;
static const uint VKR_FULLSCREEN_TONEMAP = 1u << 1u;
// The unset default is GT7. Bit 4 selects the retained ACES comparison path.
static const uint VKR_FULLSCREEN_ACES_FITTED = 1u << 4u;
static const uint VKR_FULLSCREEN_SOURCE_DISPLAY_LINEAR = 1u << 5u;
static const uint VKR_FULLSCREEN_PREPARE_DISPLAY_LINEAR = 1u << 6u;
static const uint VKR_FULLSCREEN_SCENE_BLUR = 1u << 7u;

float4 vkr_post_sample(uint texture_index, uint sampler_index, float2 uv,
                       bool tonemap, bool aces_fitted, float exposure,
                       VkrColorGrading grading,
                       VkrDisplayOutputParams display_output)
{
    float4 source = g_textures[texture_index].SampleLevel(
        g_samplers[sampler_index], uv, 0.0f);
    // The editor Scene composite samples presentation-linear pixels from the
    // retained scene image. They already carry the physical-output scale, so a
    // second grade or SDR clamp would destroy extended-linear highlights.
    if ((g_fullscreen_push.flags & (VKR_FULLSCREEN_ALREADY_OUTPUT_ENCODED |
                                    VKR_FULLSCREEN_SOURCE_DISPLAY_LINEAR)) != 0u)
    {
        return source;
    }
    source.rgb = vkr_color_grade(max(source.rgb * exposure, 0.0f), grading);
    if (!tonemap)
    {
        source.rgb = saturate(source.rgb);
    }
    else if (aces_fitted)
    {
        source.rgb = vkr_display_output_scene_relative(
            source.rgb, vkr_aces_fitted(source.rgb), display_output);
    }
    else
    {
        source.rgb = vkr_gt7_tonemap(
            source.rgb, vkr_display_output_headroom(display_output));
    }
    return source;
}

float4 vkr_finish_output(float4 color, uint flags,
                         VkrDisplayOutputParams display_output)
{
    if ((flags & (VKR_FULLSCREEN_ALREADY_OUTPUT_ENCODED |
                  VKR_FULLSCREEN_PREPARE_DISPLAY_LINEAR)) == 0u)
    {
        color.rgb = vkr_display_output_scale(color.rgb, display_output);
    }
    return color;
}

[shader("fragment")]
float4 fullscreen_fragment(VkrVulkanVertexOutput input) : SV_Target0
{
    VkrVulkanPacketUtilityRoot* root =
        g_fullscreen_push.root;
    uint texture_index =
        NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index =
        NonUniformResourceIndex(root.transmission_sampler);
    bool tonemap = (g_fullscreen_push.flags & VKR_FULLSCREEN_TONEMAP) != 0u;
    bool aces_fitted = (g_fullscreen_push.flags & VKR_FULLSCREEN_ACES_FITTED) != 0u;
    // The scene is stored pre-exposed; exposure applies to scene-linear.
    float exposure =
        root.exposure_state->exposure_multiplier / root.pre_exposure;
    VkrDisplayOutputParams display_output = root.display_output[0];
    uint2 extent = uint2(root.point_light_grid_origin_cell_size.xy);
    float sharpness = root.point_light_grid_origin_cell_size.z;
    if ((g_fullscreen_push.flags & VKR_FULLSCREEN_SCENE_BLUR) != 0u)
    {
        float4 blurred = float4(0.0f);
        for (int y = -2; y <= 2; ++y)
        {
            for (int x = -2; x <= 2; ++x)
            {
                blurred += g_textures[texture_index].SampleLevel(
                    g_samplers[sampler_index],
                    input.uv + vkr_scene_blur_offset(x, y, extent), 0.0f) *
                    vkr_scene_blur_weight(x) * vkr_scene_blur_weight(y);
            }
        }
        return vkr_finish_output(blurred, g_fullscreen_push.flags, display_output);
    }
    float4 result = vkr_post_sample(texture_index, sampler_index, input.uv,
                                    tonemap, aces_fitted, exposure,
                                    *root.color_grading, display_output);
    if (sharpness > 0.0f)
    {
        float2 step = 1.0f / float2(extent);
        float3 north = vkr_post_sample(texture_index, sampler_index,
            input.uv - float2(0.0f, step.y), tonemap, aces_fitted,
            exposure, *root.color_grading, display_output).rgb;
        float3 south = vkr_post_sample(texture_index, sampler_index,
            input.uv + float2(0.0f, step.y), tonemap, aces_fitted,
            exposure, *root.color_grading, display_output).rgb;
        float3 west = vkr_post_sample(texture_index, sampler_index,
            input.uv - float2(step.x, 0.0f), tonemap, aces_fitted,
            exposure, *root.color_grading, display_output).rgb;
        float3 east = vkr_post_sample(texture_index, sampler_index,
            input.uv + float2(step.x, 0.0f), tonemap, aces_fitted,
            exposure, *root.color_grading, display_output).rgb;
        result.rgb = vkr_sharpen_color(result.rgb,
            0.25f * (north + south + west + east),
            min(min(north, south), min(west, east)),
            max(max(north, south), max(west, east)), sharpness);
    }
    return vkr_finish_output(result, g_fullscreen_push.flags, display_output);
}

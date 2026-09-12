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
static const uint VKR_FULLSCREEN_FXAA = 1u << 2u;
static const uint VKR_FULLSCREEN_OPAQUE_ALPHA = 1u << 3u;
// The unset default is AgX. Bit 4 selects the retained ACES comparison path.
static const uint VKR_FULLSCREEN_ACES_FITTED = 1u << 4u;
static const uint VKR_FULLSCREEN_SOURCE_DISPLAY_LINEAR = 1u << 5u;
static const uint VKR_FULLSCREEN_PREPARE_DISPLAY_LINEAR = 1u << 6u;

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
    float3 display_linear = tonemap
                                ? (aces_fitted ? vkr_aces_fitted(source.rgb)
                                               : vkr_agx_tonemap(source.rgb))
                                : saturate(source.rgb);
    source.rgb = tonemap
                     ? vkr_display_output_scene_relative(source.rgb,
                                                         display_linear,
                                                         display_output)
                     : display_linear;
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

float vkr_fxaa_luminance(float3 color)
{
    return dot(sqrt(max(color, 0.0f)),
               float3(0.2126f, 0.7152f, 0.0722f));
}

float4 vkr_fxaa(uint texture_index, uint sampler_index, float2 uv,
                float2 inverse_extent, bool tonemap, bool aces_fitted,
                float exposure, float sharpness, VkrColorGrading grading,
                VkrDisplayOutputParams display_output)
{
    float4 center = vkr_post_sample(texture_index, sampler_index, uv, tonemap,
                                    aces_fitted, exposure, grading, display_output);
    float4 north = vkr_post_sample(
        texture_index, sampler_index, uv - float2(0.0f, inverse_extent.y),
        tonemap, aces_fitted, exposure, grading, display_output);
    float4 south = vkr_post_sample(
        texture_index, sampler_index, uv + float2(0.0f, inverse_extent.y),
        tonemap, aces_fitted, exposure, grading, display_output);
    float4 west = vkr_post_sample(
        texture_index, sampler_index, uv - float2(inverse_extent.x, 0.0f),
        tonemap, aces_fitted, exposure, grading, display_output);
    float4 east = vkr_post_sample(
        texture_index, sampler_index, uv + float2(inverse_extent.x, 0.0f),
        tonemap, aces_fitted, exposure, grading, display_output);
    float4 northwest = vkr_post_sample(
        texture_index, sampler_index, uv + float2(-1.0f, -1.0f) * inverse_extent,
        tonemap, aces_fitted, exposure, grading, display_output);
    float4 northeast = vkr_post_sample(
        texture_index, sampler_index, uv + float2(1.0f, -1.0f) * inverse_extent,
        tonemap, aces_fitted, exposure, grading, display_output);
    float4 southwest = vkr_post_sample(
        texture_index, sampler_index, uv + float2(-1.0f, 1.0f) * inverse_extent,
        tonemap, aces_fitted, exposure, grading, display_output);
    float4 southeast = vkr_post_sample(
        texture_index, sampler_index, uv + inverse_extent, tonemap, aces_fitted,
        exposure, grading, display_output);
    float luma_center = vkr_fxaa_luminance(center.rgb);
    float luma_northwest = vkr_fxaa_luminance(northwest.rgb);
    float luma_northeast = vkr_fxaa_luminance(northeast.rgb);
    float luma_southwest = vkr_fxaa_luminance(southwest.rgb);
    float luma_southeast = vkr_fxaa_luminance(southeast.rgb);
    float luma_north = vkr_fxaa_luminance(north.rgb);
    float luma_south = vkr_fxaa_luminance(south.rgb);
    float luma_west = vkr_fxaa_luminance(west.rgb);
    float luma_east = vkr_fxaa_luminance(east.rgb);
    float luma_min =
        min(luma_center,
            min(min(min(luma_north, luma_south),
                    min(luma_west, luma_east)),
                min(min(luma_northwest, luma_northeast),
                    min(luma_southwest, luma_southeast))));
    float luma_max =
        max(luma_center,
            max(max(max(luma_north, luma_south),
                    max(luma_west, luma_east)),
                max(max(luma_northwest, luma_northeast),
                    max(luma_southwest, luma_southeast))));
    if (luma_max - luma_min <
        max(0.0312f, luma_max * 0.125f))
    {
        if (sharpness > 0.0f)
            center.rgb = vkr_sharpen_color(
                center.rgb, 0.25f * (north.rgb + south.rgb + west.rgb + east.rgb),
                min(min(min(north.rgb, south.rgb), min(west.rgb, east.rgb)),
                    min(min(northwest.rgb, northeast.rgb), min(southwest.rgb, southeast.rgb))),
                max(max(max(north.rgb, south.rgb), max(west.rgb, east.rgb)),
                    max(max(northwest.rgb, northeast.rgb), max(southwest.rgb, southeast.rgb))),
                sharpness);
        return center;
    }

    float2 direction;
    direction.x = -((luma_northwest + luma_northeast) -
                    (luma_southwest + luma_southeast));
    direction.y = (luma_northwest + luma_southwest) -
                  (luma_northeast + luma_southeast);
    float direction_reduce = max(
        (luma_northwest + luma_northeast + luma_southwest + luma_southeast) *
            0.03125f,
        0.0078125f);
    float inverse_direction_min =
        1.0f / (min(abs(direction.x), abs(direction.y)) + direction_reduce);
    direction =
        clamp(direction * inverse_direction_min, -8.0f, 8.0f) * inverse_extent;
    float4 result_a =
        0.5f *
        (vkr_post_sample(texture_index, sampler_index,
                         uv + direction * (1.0f / 3.0f - 0.5f), tonemap,
                         aces_fitted, exposure, grading, display_output) +
         vkr_post_sample(texture_index, sampler_index,
                         uv + direction * (2.0f / 3.0f - 0.5f), tonemap,
                         aces_fitted, exposure, grading, display_output));
    float4 result_b =
        result_a * 0.5f +
        0.25f *
            (vkr_post_sample(texture_index, sampler_index,
                             uv + direction * -0.5f, tonemap, aces_fitted,
                             exposure, grading, display_output) +
             vkr_post_sample(texture_index, sampler_index,
                             uv + direction * 0.5f, tonemap, aces_fitted,
                             exposure, grading, display_output));
    float result_b_luma = vkr_fxaa_luminance(result_b.rgb);
    float4 result =
        result_b_luma < luma_min || result_b_luma > luma_max ? result_a
                                                             : result_b;
    float luma_range = luma_max - luma_min;
    float luma_average =
        (2.0f * (luma_north + luma_south + luma_west + luma_east) +
         luma_northwest + luma_northeast + luma_southwest + luma_southeast) /
        12.0f;
    float subpixel = saturate(abs(luma_average - luma_center) / luma_range);
    subpixel = subpixel * subpixel * (3.0f - 2.0f * subpixel);
    subpixel = subpixel * subpixel * 0.75f;
    result = lerp(result, 0.25f * (north + south + west + east), subpixel);
    if (sharpness > 0.0f)
        result.rgb = vkr_sharpen_color(
            result.rgb, 0.25f * (north.rgb + south.rgb + west.rgb + east.rgb),
            min(min(min(north.rgb, south.rgb), min(west.rgb, east.rgb)),
                min(min(northwest.rgb, northeast.rgb), min(southwest.rgb, southeast.rgb))),
            max(max(max(north.rgb, south.rgb), max(west.rgb, east.rgb)),
                max(max(northwest.rgb, northeast.rgb), max(southwest.rgb, southeast.rgb))),
            sharpness * (1.0f - subpixel));
    result.a = center.a;
    return result;
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
    float exposure = root.exposure_state->exposure_multiplier;
    VkrDisplayOutputParams display_output = root.display_output[0];
    uint2 extent = uint2(root.point_light_grid_origin_cell_size.xy);
    float sharpness = root.point_light_grid_origin_cell_size.z;
    float4 result;
    if ((g_fullscreen_push.flags & VKR_FULLSCREEN_FXAA) == 0u)
    {
        result = vkr_post_sample(texture_index, sampler_index, input.uv, tonemap,
                                 aces_fitted, exposure, *root.color_grading,
                                 display_output);
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
    }
    else
        result = vkr_fxaa(texture_index, sampler_index, input.uv,
                          1.0f / float2(extent), tonemap, aces_fitted,
                          exposure, sharpness, *root.color_grading,
                          display_output);
    // FSR output alpha carries a private static-history sample count.
    if ((g_fullscreen_push.flags & VKR_FULLSCREEN_OPAQUE_ALPHA) != 0u)
        result.a = 1.0f;
    return vkr_finish_output(result, g_fullscreen_push.flags, display_output);
}

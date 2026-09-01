static const uint VKR_FULLSCREEN_TONEMAP = 1u << 1u;
static const uint VKR_FULLSCREEN_FXAA = 1u << 2u;

float4 vkr_post_sample(uint texture_index, uint sampler_index, float2 uv,
                       bool tonemap, float exposure)
{
    float4 source = g_textures[texture_index].SampleLevel(
        g_samplers[sampler_index], uv, 0.0f);
    source.rgb = max(source.rgb * exposure, 0.0f);
    source.rgb = tonemap ? vkr_aces_fitted(source.rgb) : saturate(source.rgb);
    return source;
}

float vkr_fxaa_luminance(float3 color)
{
    return dot(sqrt(max(color, 0.0f)),
               float3(0.2126f, 0.7152f, 0.0722f));
}

float4 vkr_fxaa(uint texture_index, uint sampler_index, float2 uv,
                float2 inverse_extent, bool tonemap, float exposure)
{
    float4 center = vkr_post_sample(texture_index, sampler_index, uv, tonemap,
                                    exposure);
    float4 north = vkr_post_sample(
        texture_index, sampler_index, uv - float2(0.0f, inverse_extent.y),
        tonemap, exposure);
    float4 south = vkr_post_sample(
        texture_index, sampler_index, uv + float2(0.0f, inverse_extent.y),
        tonemap, exposure);
    float4 west = vkr_post_sample(
        texture_index, sampler_index, uv - float2(inverse_extent.x, 0.0f),
        tonemap, exposure);
    float4 east = vkr_post_sample(
        texture_index, sampler_index, uv + float2(inverse_extent.x, 0.0f),
        tonemap, exposure);
    float4 northwest = vkr_post_sample(
        texture_index, sampler_index, uv + float2(-1.0f, -1.0f) * inverse_extent,
        tonemap, exposure);
    float4 northeast = vkr_post_sample(
        texture_index, sampler_index, uv + float2(1.0f, -1.0f) * inverse_extent,
        tonemap, exposure);
    float4 southwest = vkr_post_sample(
        texture_index, sampler_index, uv + float2(-1.0f, 1.0f) * inverse_extent,
        tonemap, exposure);
    float4 southeast = vkr_post_sample(
        texture_index, sampler_index, uv + inverse_extent, tonemap, exposure);
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
        return center;

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
                         exposure) +
         vkr_post_sample(texture_index, sampler_index,
                         uv + direction * (2.0f / 3.0f - 0.5f), tonemap,
                         exposure));
    float4 result_b =
        result_a * 0.5f +
        0.25f *
            (vkr_post_sample(texture_index, sampler_index,
                             uv + direction * -0.5f, tonemap, exposure) +
             vkr_post_sample(texture_index, sampler_index,
                             uv + direction * 0.5f, tonemap, exposure));
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
    result.a = center.a;
    return result;
}

[shader("fragment")]
float4 fullscreen_fragment(VkrVulkanVertexOutput input) : SV_Target0
{
    VkrVulkanPacketUtilityRoot* root =
        (VkrVulkanPacketUtilityRoot*)g_push.root;
    uint texture_index =
        NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index =
        NonUniformResourceIndex(root.transmission_sampler);
    bool tonemap = (g_push.flags & VKR_FULLSCREEN_TONEMAP) != 0u;
    float exposure = root.exposure_state->exposure_multiplier;
    if ((g_push.flags & VKR_FULLSCREEN_FXAA) == 0u)
        return vkr_post_sample(texture_index, sampler_index, input.uv, tonemap,
                               exposure);
    uint2 extent = uint2(root.point_light_grid_origin_cell_size.xy);
    return vkr_fxaa(texture_index, sampler_index, input.uv,
                    1.0f / float2(extent), tonemap, exposure);
}

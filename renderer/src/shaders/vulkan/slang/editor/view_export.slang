// View streaming (ADR-131): resamples the frame's finished Scene image into
// the NV12 picture that Vulkan Video encodes as H.264. The tiled pipeline's
// kernel (shaders/metal/msl/editor/view_export.metal) writes a BGRA surface
// that VideoToolbox converts; Vulkan Video takes only 4:2:0 YCbCr, so this
// kernel converts as well, with the BT.709 video-range matrix the stream's
// VUI names. Each pixel filters the Scene bilinearly at its centre, clamped
// to [0, 1] and sRGB-encoded unless the Scene image already holds
// display-encoded values, as the Metal kernel does. One thread writes a
// 2x2 block: four luma samples and the block's mean chroma. Pixels past the
// picture's extent, out to the coded extent the encoder reads, repeat its
// last row and column.

struct VkrVkViewExportRoot
{
    uint source_texture;
    uint source_sampler;
    uint luma_image;
    uint chroma_image;
    uint2 extent;
    uint2 coded_extent;
    // 1 when the Scene image stores display-encoded values.
    uint source_encoded;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

static const float3 vkr_vk_view_export_luma_weights =
    float3(0.2126f, 0.7152f, 0.0722f);

float vkr_vk_view_export_encode_srgb(float linear)
{
    return linear <= 0.0031308f
               ? linear * 12.92f
               : 1.055f * pow(linear, 1.0f / 2.4f) - 0.055f;
}

float3 vkr_vk_view_export_encode_srgb(float3 linear)
{
    return float3(vkr_vk_view_export_encode_srgb(linear.r),
                  vkr_vk_view_export_encode_srgb(linear.g),
                  vkr_vk_view_export_encode_srgb(linear.b));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_view_export(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkViewExportRoot root =
        vkr_vk_uniform_root(*(VkrVkViewExportRoot*)g_push.root);
    const uint2 block = thread_id.xy;
    if (any(block * 2u >= root.coded_extent))
        return;
    float3 sum = 0.0f;
    [ForceUnroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        const uint2 pixel = block * 2u + uint2(i & 1u, i >> 1u);
        const uint2 source_pixel = min(pixel, root.extent - 1u);
        const float2 uv = (float2(source_pixel) + 0.5f) / float2(root.extent);
        float3 color = saturate(
            g_textures[root.source_texture]
                .SampleLevel(g_samplers[root.source_sampler], uv, 0.0f)
                .rgb);
        if (root.source_encoded == 0u)
            color = vkr_vk_view_export_encode_srgb(color);
        const float luma = dot(color, vkr_vk_view_export_luma_weights);
        // Video range: Y' in [16, 235] of 255.
        g_storage_2d[root.luma_image][pixel] =
            float4((16.0f + 219.0f * luma) / 255.0f, 0.0f, 0.0f, 0.0f);
        sum += color;
    }
    const float3 mean = sum * 0.25f;
    const float mean_luma = dot(mean, vkr_vk_view_export_luma_weights);
    // BT.709: Cb = (B' - Y') / 1.8556, Cr = (R' - Y') / 1.5748, in
    // [-0.5, 0.5], to [16, 240] of 255.
    const float cb = (mean.b - mean_luma) / 1.8556f;
    const float cr = (mean.r - mean_luma) / 1.5748f;
    g_storage_2d[root.chroma_image][block] =
        float4((128.0f + 224.0f * cb) / 255.0f,
               (128.0f + 224.0f * cr) / 255.0f, 0.0f, 0.0f);
}

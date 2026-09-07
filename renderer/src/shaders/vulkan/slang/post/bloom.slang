// Bloom prefilter, downsample, upsample, and combine entry points. The threshold
// curve,
// the non-finite/firefly sanitizer, and the Karis weight live in
// ../../../shared/bloom_kernel.slangh so the Metal path uses the same
// definitions. Tap patterns stay here because they are inseparable from the
// backend's sampling call.

struct VkrVkBloomRoot
{
    uint source_texture;
    // Coarser accumulation level. Written by the executor for the upsample
    // pass only; the deepest step is pointed at the downsample chain instead,
    // because the accumulation level above it has never been written.
    uint coarse_texture;
    uint destination_texture;
    uint source_sampler;
    // Extent the tap offsets are expressed in: the reduction input for the
    // prefilter and downsample, the coarse level for the upsample. The
    // upsample's fine source shares the destination extent, so it needs none.
    uint2 filter_extent;
    uint2 destination_extent;
    VkrBloomParams params;
};

struct VkrVkBloomPush
{
    VkrVkBloomRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkBloomPush> g_bloom_push;

float3 vkr_bloom_tap(VkrVkBloomRoot *root, uint texture_index, float2 uv)
{
    return vkr_bloom_sanitize(
        root.params,
        g_textures[NonUniformResourceIndex(texture_index)]
            .SampleLevel(g_samplers[NonUniformResourceIndex(root.source_sampler)],
                         uv, 0.0f)
            .rgb);
}

// Thirteen-tap Call of Duty downsample. The five overlapping 2x2 boxes are what
// make the reduction stable under motion; a plain box filter at this ratio
// aliases into the chain and the aliasing is then blurred across the screen.
struct VkrBloomTaps13
{
    float3 a, b, c, d, e, f, g, h, i, j, k, l, m;
};

VkrBloomTaps13 vkr_bloom_gather13(VkrVkBloomRoot *root, uint texture_index,
                                  float2 uv, float2 texel)
{
    VkrBloomTaps13 taps;
    taps.a = vkr_bloom_tap(root, texture_index, uv + float2(-2.0f, -2.0f) * texel);
    taps.b = vkr_bloom_tap(root, texture_index, uv + float2(0.0f, -2.0f) * texel);
    taps.c = vkr_bloom_tap(root, texture_index, uv + float2(2.0f, -2.0f) * texel);
    taps.d = vkr_bloom_tap(root, texture_index, uv + float2(-2.0f, 0.0f) * texel);
    taps.e = vkr_bloom_tap(root, texture_index, uv);
    taps.f = vkr_bloom_tap(root, texture_index, uv + float2(2.0f, 0.0f) * texel);
    taps.g = vkr_bloom_tap(root, texture_index, uv + float2(-2.0f, 2.0f) * texel);
    taps.h = vkr_bloom_tap(root, texture_index, uv + float2(0.0f, 2.0f) * texel);
    taps.i = vkr_bloom_tap(root, texture_index, uv + float2(2.0f, 2.0f) * texel);
    taps.j = vkr_bloom_tap(root, texture_index, uv + float2(-1.0f, -1.0f) * texel);
    taps.k = vkr_bloom_tap(root, texture_index, uv + float2(1.0f, -1.0f) * texel);
    taps.l = vkr_bloom_tap(root, texture_index, uv + float2(-1.0f, 1.0f) * texel);
    taps.m = vkr_bloom_tap(root, texture_index, uv + float2(1.0f, 1.0f) * texel);
    return taps;
}

float3 vkr_bloom_combine13(VkrBloomTaps13 taps)
{
    return taps.e * 0.125f + (taps.a + taps.c + taps.g + taps.i) * 0.03125f +
           (taps.b + taps.d + taps.f + taps.h) * 0.0625f +
           (taps.j + taps.k + taps.l + taps.m) * 0.125f;
}

float3 vkr_bloom_karis_box(float3 a, float3 b, float3 c, float3 d)
{
    float wa = vkr_bloom_karis_weight(a);
    float wb = vkr_bloom_karis_weight(b);
    float wc = vkr_bloom_karis_weight(c);
    float wd = vkr_bloom_karis_weight(d);
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

// Firefly-suppressed variant of the same thirteen taps. Each 2x2 box is
// averaged by perceived intensity before the boxes are combined, so a single
// hot texel cannot own the reduction. Only the prefilter needs this: once the
// chain is bounded, later levels have no isolated outlier left to suppress.
float3 vkr_bloom_combine13_karis(VkrBloomTaps13 taps)
{
    return vkr_bloom_karis_box(taps.j, taps.k, taps.l, taps.m) * 0.5f +
           vkr_bloom_karis_box(taps.a, taps.b, taps.d, taps.e) * 0.125f +
           vkr_bloom_karis_box(taps.b, taps.c, taps.e, taps.f) * 0.125f +
           vkr_bloom_karis_box(taps.d, taps.e, taps.g, taps.h) * 0.125f +
           vkr_bloom_karis_box(taps.e, taps.f, taps.h, taps.i) * 0.125f;
}

// Four bilinear taps, each covering a 2x2 source footprint, so the filter is a
// 4x4 box for a quarter of the sample cost. Retained as a measured alternative
// to the thirteen-tap form rather than as a fallback.
float3 vkr_bloom_combine_box4(VkrVkBloomRoot *root, uint texture_index,
                              float2 uv, float2 texel)
{
    return (vkr_bloom_tap(root, texture_index, uv + float2(-1.0f, -1.0f) * texel) +
            vkr_bloom_tap(root, texture_index, uv + float2(1.0f, -1.0f) * texel) +
            vkr_bloom_tap(root, texture_index, uv + float2(-1.0f, 1.0f) * texel) +
            vkr_bloom_tap(root, texture_index, uv + float2(1.0f, 1.0f) * texel)) *
           0.25f;
}

// Nine-tap tent. The upsample filter must be wider than the destination texel
// or the chain reconstructs as visible squares at every level boundary.
float3 vkr_bloom_tent9(VkrVkBloomRoot *root, uint texture_index, float2 uv,
                       float2 texel)
{
    float3 sum =
        vkr_bloom_tap(root, texture_index, uv + float2(-1.0f, -1.0f) * texel) *
        1.0f;
    sum += vkr_bloom_tap(root, texture_index, uv + float2(0.0f, -1.0f) * texel) * 2.0f;
    sum += vkr_bloom_tap(root, texture_index, uv + float2(1.0f, -1.0f) * texel) * 1.0f;
    sum += vkr_bloom_tap(root, texture_index, uv + float2(-1.0f, 0.0f) * texel) * 2.0f;
    sum += vkr_bloom_tap(root, texture_index, uv) * 4.0f;
    sum += vkr_bloom_tap(root, texture_index, uv + float2(1.0f, 0.0f) * texel) * 2.0f;
    sum += vkr_bloom_tap(root, texture_index, uv + float2(-1.0f, 1.0f) * texel) * 1.0f;
    sum += vkr_bloom_tap(root, texture_index, uv + float2(0.0f, 1.0f) * texel) * 2.0f;
    sum += vkr_bloom_tap(root, texture_index, uv + float2(1.0f, 1.0f) * texel) * 1.0f;
    return sum * (1.0f / 16.0f);
}

// True for the destination texels this dispatch actually owns. Extents are odd
// as often as not once the chain has halved a few times, so the tail group of
// every dispatch is partially out of range.
bool vkr_bloom_in_range(VkrVkBloomRoot *root, uint2 pixel)
{
    return all(pixel < root.destination_extent);
}

float2 vkr_bloom_destination_uv(VkrVkBloomRoot *root, uint2 pixel)
{
    return (float2(pixel) + 0.5f) / float2(root.destination_extent);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_bloom_prefilter(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkBloomRoot *root = g_bloom_push.root;
    if (!vkr_bloom_in_range(root, thread_id.xy))
        return;
    float2 uv = vkr_bloom_destination_uv(root, thread_id.xy);
    float2 texel = 1.0f / float2(root.filter_extent);
    VkrBloomTaps13 taps =
        vkr_bloom_gather13(root, root.source_texture, uv, texel);
    // Threshold after the firefly-weighted reduction. Thresholding each tap
    // first would let the knee reintroduce the outlier the weighting removed.
    float3 reduced = vkr_bloom_combine13_karis(taps);
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] =
            float4(vkr_bloom_soft_threshold(root.params, reduced), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_bloom_downsample_tent13(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkBloomRoot *root = g_bloom_push.root;
    if (!vkr_bloom_in_range(root, thread_id.xy))
        return;
    float2 uv = vkr_bloom_destination_uv(root, thread_id.xy);
    float2 texel = 1.0f / float2(root.filter_extent);
    float3 reduced = vkr_bloom_combine13(
        vkr_bloom_gather13(root, root.source_texture, uv, texel));
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = float4(reduced, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_bloom_downsample_box4(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkBloomRoot *root = g_bloom_push.root;
    if (!vkr_bloom_in_range(root, thread_id.xy))
        return;
    float2 uv = vkr_bloom_destination_uv(root, thread_id.xy);
    float2 texel = 1.0f / float2(root.filter_extent);
    float3 reduced =
        vkr_bloom_combine_box4(root, root.source_texture, uv, texel);
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = float4(reduced, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_bloom_upsample(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkBloomRoot *root = g_bloom_push.root;
    if (!vkr_bloom_in_range(root, thread_id.xy))
        return;
    float2 uv = vkr_bloom_destination_uv(root, thread_id.xy);
    // The tent radius is expressed in coarse texels, which is what makes the
    // filter wider than the destination grid at every level.
    float2 coarse_texel = 1.0f / float2(root.filter_extent);
    float3 coarse = vkr_bloom_tent9(root, root.coarse_texture, uv, coarse_texel);
    float3 fine = vkr_bloom_tap(root, root.source_texture, uv);
    // Sum rather than lerp: each level contributes its own band once, so the
    // resolved chain is the sum of the scales rather than a weighted average
    // that drops energy at every step.
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = float4(fine + coarse, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_bloom_combine(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkBloomRoot *root = g_bloom_push.root;
    if (!vkr_bloom_in_range(root, thread_id.xy))
        return;
    float2 uv = vkr_bloom_destination_uv(root, thread_id.xy);
    float3 hdr =
        g_textures[NonUniformResourceIndex(root.source_texture)]
            .SampleLevel(g_samplers[NonUniformResourceIndex(root.source_sampler)],
                         uv, 0.0f)
            .rgb;
    float3 bloom =
        g_textures[NonUniformResourceIndex(root.coarse_texture)]
            .SampleLevel(g_samplers[NonUniformResourceIndex(root.source_sampler)],
                         uv, 0.0f)
            .rgb;
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = float4(hdr + bloom * root.params.intensity, 1.0f);
}

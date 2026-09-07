// Automatic-exposure metering entry points. Shared arithmetic lives in
// ../../../shared/exposure_kernel.slangh so the backend-native entry points use
// the same luminance, binning, percentile, and adaptation definitions.

struct VkrVkExposureHistogram
{
    uint bins[VKR_EXPOSURE_HISTOGRAM_BINS];
};

struct VkrVkExposureRoot
{
    VkrVkExposureHistogram *histogram;
    VkrExposureState *state;
    VkrExposureState *previous_state;
    uint source_texture;
    uint2 extent;
    uint reset_reasons;
    VkrExposureMetering metering;
};

struct VkrVkExposurePush
{
    VkrVkExposureRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkExposurePush> g_exposure_push;

groupshared uint g_exposure_bins[VKR_EXPOSURE_HISTOGRAM_BINS];

// The bounded histogram is cleared inside its own pass so the first frame does
// not depend on device memory arriving zeroed.
[shader("compute")]
[numthreads(VKR_EXPOSURE_HISTOGRAM_BINS, 1, 1)]
void vk_exposure_clear(uint3 thread_id : SV_DispatchThreadID)
{
    g_exposure_push.root.histogram->bins[thread_id.x] = 0u;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void vk_exposure_histogram(uint3 thread_id : SV_DispatchThreadID,
                           uint group_index : SV_GroupIndex)
{
    VkrVkExposureRoot *root = g_exposure_push.root;
    g_exposure_bins[group_index] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (all(thread_id.xy < root.extent))
    {
        float3 color =
            g_textures[NonUniformResourceIndex(root.source_texture)]
                .Load(int3(thread_id.xy, 0))
                .rgb;
        float luminance = vkr_exposure_luminance(color);
        if (vkr_exposure_luminance_accepted(root.metering, luminance))
        {
            uint bin = vkr_exposure_bin_index(root.metering, luminance);
            InterlockedAdd(g_exposure_bins[bin], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // One bounded global merge per workgroup. Whether a subgroup pre-reduction
    // would help depends on measured bin contention for this scene, not on a
    // same-predicate ballot, so it is not assumed here.
    uint local = g_exposure_bins[group_index];
    if (local != 0u)
    {
        uint ignored;
        InterlockedAdd(root.histogram->bins[group_index], local, ignored);
    }
}

groupshared float g_exposure_prefix[VKR_EXPOSURE_HISTOGRAM_BINS];
groupshared float g_exposure_weight[VKR_EXPOSURE_HISTOGRAM_BINS];
groupshared float g_exposure_weighted_log[VKR_EXPOSURE_HISTOGRAM_BINS];
groupshared uint g_exposure_low_bin[VKR_EXPOSURE_HISTOGRAM_BINS];
groupshared uint g_exposure_high_bin[VKR_EXPOSURE_HISTOGRAM_BINS];

[shader("compute")]
[numthreads(VKR_EXPOSURE_HISTOGRAM_BINS, 1, 1)]
void vk_exposure_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkExposureRoot *root = g_exposure_push.root;
    uint bin = thread_id.x;
    float count = float(root.histogram->bins[bin]);
    g_exposure_prefix[bin] = count;
    GroupMemoryBarrierWithGroupSync();

    // Inclusive Hillis-Steele scan over a fixed 256-wide array. The bin count
    // is a compile-time constant, so the step count is too.
    for (uint offset = 1u; offset < VKR_EXPOSURE_HISTOGRAM_BINS; offset <<= 1u)
    {
        float addend = bin >= offset ? g_exposure_prefix[bin - offset] : 0.0f;
        GroupMemoryBarrierWithGroupSync();
        g_exposure_prefix[bin] += addend;
        GroupMemoryBarrierWithGroupSync();
    }

    float total = g_exposure_prefix[VKR_EXPOSURE_HISTOGRAM_BINS - 1u];
    float low_count = root.metering.low_percentile * total;
    float high_count = root.metering.high_percentile * total;
    float inclusive = g_exposure_prefix[bin];
    float retained = vkr_exposure_retained(inclusive - count, inclusive,
                                           low_count, high_count);
    g_exposure_weight[bin] = retained;
    g_exposure_weighted_log[bin] =
        retained * vkr_exposure_bin_log_luminance(root.metering, bin);
    // The clipped percentile range is a debug output, so the edges are reduced
    // alongside the weight rather than re-derived from a capture.
    g_exposure_low_bin[bin] =
        retained > 0.0f ? bin : VKR_EXPOSURE_HISTOGRAM_BINS;
    g_exposure_high_bin[bin] = retained > 0.0f ? bin : 0u;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = VKR_EXPOSURE_HISTOGRAM_BINS / 2u; stride > 0u;
         stride >>= 1u)
    {
        if (bin < stride)
        {
            g_exposure_weight[bin] += g_exposure_weight[bin + stride];
            g_exposure_weighted_log[bin] +=
                g_exposure_weighted_log[bin + stride];
            g_exposure_low_bin[bin] =
                min(g_exposure_low_bin[bin], g_exposure_low_bin[bin + stride]);
            g_exposure_high_bin[bin] =
                max(g_exposure_high_bin[bin], g_exposure_high_bin[bin + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (bin != 0u)
        return;

    float weight = g_exposure_weight[0];
    float average_log_luminance =
        weight > 0.0f ? g_exposure_weighted_log[0] / weight : 0.0f;
    float previous_ev = vkr_exposure_previous_ev(
        root.metering, root.previous_state->adapted_ev);
    float target_ev = vkr_exposure_target_ev(root.metering, weight,
                                             average_log_luminance, previous_ev);
    float adapted_ev =
        vkr_exposure_adapt(root.metering, target_ev, previous_ev);
    bool retained_any = weight > 0.0f;

    VkrExposureState published;
    published.exposure_multiplier = exp2(adapted_ev);
    published.adapted_ev = adapted_ev;
    published.target_ev = target_ev;
    published.average_log_luminance = average_log_luminance;
    published.retained_low_bin = retained_any ? float(g_exposure_low_bin[0]) : 0.0f;
    published.retained_high_bin =
        retained_any ? float(g_exposure_high_bin[0]) : 0.0f;
    published.accepted_texel_count = uint(total);
    published.reset_reasons = root.reset_reasons;
    *root.state = published;
}

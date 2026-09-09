// Execute production SSR filtering and cell traversal with analytic inputs.
#include "ssr_kernel.slangh"
#include "ssgi_kernel.slangh"
RWStructuredBuffer<float> g_results;
[shader("compute")]
[numthreads(1, 1, 1)]
void ssr_stability_execute(uint3 id : SV_DispatchThreadID)
{
    float4 hit = vkr_ssr_spatial_sample(float4(8, 4, 2, 1), 1);
    float4 sparse = vkr_ssr_spatial_resolve(hit, 9);
    float4 dense = vkr_ssr_spatial_resolve(hit * 9, 9);
    float4 empty = vkr_ssr_spatial_resolve(float4(0), 9);
    float4 mixed = vkr_ssr_spatial_resolve(
        vkr_ssr_spatial_sample(float4(8, 4, 2, 0.5), 2) +
        vkr_ssr_spatial_sample(float4(2, 1, 8, 1), 1), 3);
    VkrSsrParams params = (VkrSsrParams)0;
    VkrSsrHistoryDecision decision = (VkrSsrHistoryDecision)0;
    decision.weight = 0.8f;
    decision.accepted = 1u;
    float4 stable = vkr_ssr_temporal_filter(0.25f, 3u,
        float4(8, 4, 2, 0.5), float4(4, 2, 1, 0.5), 0, 16, decision);
    float4 intermittent = vkr_ssr_temporal_filter(0.25f, 3u,
        0, float4(8, 4, 2, 0.5), 0, 16, decision);
    float4 unsupported = vkr_ssr_temporal_filter(0.25f, 0u,
        0, float4(8, 4, 2, 0.5), 1, -1, decision);
    float4 black = vkr_ssr_temporal_filter(VKR_SSR_MIRROR_ROUGHNESS, 3u,
        float4(0, 0, 0, 0.5), float4(8, 4, 2, 0.5), 0, 0, decision);
    decision.weight = 0.0f;
    decision.accepted = 0u;
    float4 rejected = vkr_ssr_temporal_filter(0.25f, 3u,
        float4(8, 4, 2, 0.5), float4(4, 2, 1, 0.5), 0, 16, decision);
    params.history_jitter_uv_x = 0.0025f;
    params.history_jitter_uv_y = -0.005f;
    float2 previous_uv = vkr_ssr_previous_uv(params, float2(0.4, 0.6), float2(0.1, -0.2));
    float4 interpolated = 0.0f;
    float support = 0.0f;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        float weight = vkr_ssr_history_tap_weight(float2(0.25, 0.75), int2(x, y));
        // An affine radiance field must interpolate exactly between samples.
        float value = 2.0f + 3.0f * float(x) + 5.0f * float(y);
        interpolated += vkr_ssr_spatial_sample(float4(value, value, value, 0.5), weight);
        support += weight;
    }
    interpolated = vkr_ssr_spatial_resolve(interpolated, support);
    params.history_valid = 1u;
    params.trace_texel_size_x = 0.1f;
    params.trace_texel_size_y = 0.1f;
    params.previous_projection_m22 = -1.0f;
    params.previous_projection_m33 = 1.0f;
    params.temporal_depth_absolute = 0.01f;
    uint2 identity = uint2(1u, 2u);
    VkrSsrHistoryDecision matching = vkr_ssr_temporal_accept(params, identity,
        identity, 0.5f, 0.5f, float2(0.5f), 1.0f, uint2(10u));
    VkrSsrHistoryDecision foreign = vkr_ssr_temporal_accept(params, identity,
        uint2(2u, 2u), 0.5f, 0.5f, float2(0.5f), 1.0f, uint2(10u));
    VkrSsrHistoryDecision disoccluded = vkr_ssr_temporal_accept(params, identity,
        identity, 0.75f, 0.5f, float2(0.5f), 1.0f, uint2(10u));
    g_results[36] = previous_uv.x;
    g_results[37] = previous_uv.y;
    g_results[38] = float(matching.accepted);
    g_results[39] = float(foreign.accepted);
    g_results[40] = float(disoccluded.accepted);
    g_results[41] = support;
    for (uint i = 0; i < 4; ++i) {
        g_results[42 + i] = interpolated[i];
    }
    params.history_jitter_uv_x = 0.0f;
    params.history_jitter_uv_y = 0.0f;
    uint2 grid = uint2(320, 241);
    float2 identity_uv = vkr_ssr_previous_uv(params,
        vkr_ssr_uv_from_pixel(uint2(100, 120), grid), float2(0));
    float2 identity_coordinate = identity_uv * float2(grid) - 0.5f;
    g_results[46] = identity_coordinate.x;
    g_results[47] = identity_coordinate.y;
    float4 partial = vkr_ssr_spatial_resolve(
        vkr_ssr_spatial_sample(float4(2, 2, 2, 1), 0.25f), 0.25f);
    decision.weight = 0.85f;
    decision.accepted = 1u;
    float4 partial_filtered = vkr_ssr_temporal_filter(0.25f, 3u,
        float4(10, 10, 10, 1), partial, 0, 10, decision);
    g_results[48] = partial_filtered.x;
    // Sparse neighborhoods must accumulate despite collapsed current bounds.
    decision.weight = 0.85f;
    decision.accepted = 1u;
    float4 one_hit = vkr_ssr_temporal_filter(0.25f, 1u,
        float4(10, 10, 10, 1), float4(2, 2, 2, 1), 10, 10, decision);
    float4 two_hits = vkr_ssr_temporal_filter(0.25f, 2u,
        float4(10, 10, 10, 1), float4(2, 2, 2, 1), 10, 10, decision);
    float4 three_hits = vkr_ssr_temporal_filter(0.25f, 3u,
        float4(10, 10, 10, 1), float4(2, 2, 2, 1), 10, 10, decision);
    float4 sparse_coverage = vkr_ssr_temporal_filter(0.25f, 1u,
        float4(10, 10, 10, 0.25), float4(2, 2, 2, 1), 10, 10, decision);
    float4 empty_support = vkr_ssr_temporal_filter(0.25f, 0u,
        0, float4(2, 2, 2, 1), 1, -1, decision);
    float4 in_bounds_history = vkr_ssr_temporal_filter(0.25f, 1u,
        float4(6, 6, 6, 1), float4(4, 4, 4, 1), 2, 10, decision);
    decision.weight = 0.25f;
    float4 lower_weight = vkr_ssr_temporal_filter(0.25f, 1u,
        float4(10, 10, 10, 1), float4(2, 2, 2, 1), 10, 10, decision);
    decision.weight = 0.0f;
    decision.accepted = 0u;
    float4 sparse_rejected = vkr_ssr_temporal_filter(0.25f, 1u,
        float4(10, 10, 10, 1), float4(2, 2, 2, 1), 10, 10, decision);
    g_results[49] = one_hit.x;
    g_results[50] = two_hits.x;
    g_results[51] = three_hits.x;
    g_results[52] = sparse_coverage.x;
    g_results[53] = sparse_coverage.w;
    g_results[54] = empty_support.w;
    g_results[55] = lower_weight.x;
    g_results[56] = sparse_rejected.x;
    g_results[57] = in_bounds_history.x;
    // Empty support must decay coverage while preserving radiance; invalid
    // receiver history must disappear immediately, independent of raw bounds.
    float4 invalid_empty = vkr_ssr_temporal_filter(0.25f, 0u,
        0, float4(8, 4, 2, 1), 1, -1, decision);
    decision.weight = 0.85f;
    decision.accepted = 1u;
    float4 faded = float4(8, 4, 2, 1);
    for (uint frame = 0u; frame < 16u; ++frame)
        faded = vkr_ssr_temporal_filter(0.25f, 0u, 0, faded, 1, -1, decision);
    for (uint component = 0u; component < 4u; ++component) {
        g_results[98u + component] = faded[component];
        g_results[102u + component] = invalid_empty[component];
    }

    g_results[106] = vkr_ssr_temporal_weight(.85f, .36f, float2(0));
    g_results[107] = vkr_ssr_temporal_weight(.85f, .36f, float2(1, 0));
    g_results[108] = vkr_ssr_temporal_weight(.85f, .36f, float2(.5f, 0));
    g_results[109] = vkr_ssr_temporal_weight(.85f, VKR_SSR_MIRROR_ROUGHNESS, float2(0));
    g_results[110] = vkr_ssr_temporal_weight(0, .36f, float2(0));
    g_results[111] = vkr_ssr_temporal_stability((.25f + VKR_SSR_MIRROR_ROUGHNESS) * .5f);
    // Chrome and trim separated by 8cm must not cross the SSR surface filter.
    // SSGI retains its previous 10 cm minimum, independently of SSR ray thickness.
    float3 up = float3(0, 1, 0);
    g_results[112] = vkr_ssr_receiver_bilateral_weight(float2(1, 0), .82f, .74f,
        up, up, VKR_SSR_RECEIVER_DEPTH_ABSOLUTE);
    g_results[113] = vkr_ssr_receiver_bilateral_weight(float2(1, 0), .82f, .83f,
        up, up, VKR_SSR_RECEIVER_DEPTH_ABSOLUTE);
    params.thickness = .1f;
    g_results[114] = vkr_ssgi_spatial_weight(float2(1, 0), .82f, .74f, up, up, params);
    g_results[115] = vkr_ssr_receiver_bilateral_weight(float2(0), .01f, .02f,
        up, up, VKR_SSR_RECEIVER_DEPTH_ABSOLUTE);
    // Constant incoming light with alternating receiver response must accumulate
    // the shaded contribution. Multiplying after history would alternate 2/8.
    float4 shaded_a = vkr_ssr_shade_reflection(float4(10, 10, 10, .6f), float3(.2f));
    float4 shaded_b = vkr_ssr_shade_reflection(float4(10, 10, 10, .6f), float3(.8f));
    decision.weight = .5f;
    decision.accepted = 1u;
    float4 shaded_history = vkr_ssr_temporal_filter(.3f, 3u, shaded_b,
        shaded_a, float3(8), float3(8), decision);
    for (uint component = 0u; component < 4u; ++component)
        g_results[116u + component] = shaded_history[component];
    g_results[120] = vkr_ssr_replace_environment_specular(
        float3(20), float3(3), shaded_history.rgb, shaded_history.a).x;
    g_results[121] = vkr_ssr_replace_environment_specular(
        float3(20), float3(3), shaded_history.rgb, 0).x;
    g_results[122] = vkr_ssr_replace_environment_specular(
        float3(20), float3(3), shaded_history.rgb, 1).x;
    g_results[123] = vkr_ssr_shade_reflection(float4(10, 10, 10, .6f), float3(0)).a;
    params.source_width = 2u;
    params.source_height = 1u;
    params.thickness = 0.1f;
    VkrSsrTrace trace = vkr_ssr_empty_trace();
    trace.start_uv = float2(0, 0.5f);
    trace.end_uv = float2(1, 0.5f);
    trace.active = 1u;
    // A ray wholly in front of depth 10 crosses empty space. Thickness is
    // behind the recorded surface and must not force hierarchy descent here.
    trace.start_depth = 9.95f;
    trace.end_depth = 9.9f;
    trace.start_clip_w = 1.0f;
    trace.end_clip_w = 1.0f;
    trace.mip = 2u;
    VkrSsrTraceResolve front_coarse = vkr_ssr_trace_resolve(trace, 10, 1, params);
    trace.mip = 0u;
    VkrSsrTraceResolve front_leaf = vkr_ssr_trace_resolve(trace, 10, 1, params);
    trace.start_depth = 9.95f;
    trace.end_depth = 10.05f;
    VkrSsrTraceResolve crossing = vkr_ssr_trace_resolve(trace, 10, 1, params);
    trace.start_depth = 10.02f;
    trace.end_depth = 10.08f;
    VkrSsrTraceResolve behind = vkr_ssr_trace_resolve(trace, 10, 1, params);
    trace.start_depth = 10.2f;
    trace.end_depth = 10.3f;
    VkrSsrTraceResolve beyond = vkr_ssr_trace_resolve(trace, 10, 1, params);
    g_results[58] = float(front_coarse.advance);
    g_results[59] = float(front_coarse.descend);
    g_results[60] = float(front_leaf.hit);
    g_results[61] = float(crossing.hit);
    g_results[62] = crossing.hit_t;
    g_results[63] = float(behind.hit);
    g_results[64] = float(beyond.hit);
    g_results[65] = float(vkr_ssr_depth_hit_matches(9.95f, 10, params));
    g_results[66] = float(vkr_ssr_depth_hit_matches(10, 10, params));
    g_results[67] = float(vkr_ssr_depth_hit_matches(10.05f, 10, params));
    g_results[68] = float(vkr_ssr_depth_hit_matches(10.2f, 10, params));
    g_results[69] = float(vkr_ssr_depth_hit_matches(0, 10, params));
    g_results[70] = float(vkr_ssr_depth_hit_matches(10.0f - 1e-6f, 10, params));
    trace.start_depth = 9.95f;
    trace.end_depth = 9.9f;
    trace.mip = 2u;
    VkrSsrTraceResolve diffuse_coarse = vkr_ssgi_trace_resolve(trace, 10, 1, params);
    trace.mip = 0u;
    VkrSsrTraceResolve diffuse_leaf = vkr_ssgi_trace_resolve(trace, 10, 1, params);
    g_results[71] = float(diffuse_coarse.descend);
    g_results[72] = float(diffuse_leaf.hit);
    g_results[73] = diffuse_leaf.hit_t;
    // The first supported point is the cell entry when the ray already lies
    // inside the rear slab, including when depth decreases toward the camera.
    trace.start_depth = 10.08f;
    trace.end_depth = 10.02f;
    VkrSsrTraceResolve rear_entry = vkr_ssr_trace_resolve(trace, 10, 1, params);
    trace.start_depth = 10.25f;
    trace.end_depth = 9.75f;
    params.thickness = 0.125f;
    VkrSsrTraceResolve rear_crossing = vkr_ssr_trace_resolve(trace, 10, 1, params);
    g_results[74] = rear_entry.hit_t;
    g_results[75] = rear_crossing.hit_t;
    // Floor-sized pyramid cells absorb the odd source tail. The full-resolution
    // leaf owns one pixel, while the final reduced cells own three or five.
    uint2 source_extent = uint2(641, 481);
    float4 full_tail = vkr_ssr_cell_bounds(uint2(640, 480), 0u,
        source_extent, source_extent);
    float4 half_tail = vkr_ssr_cell_bounds(uint2(319, 239), 1u,
        source_extent, uint2(320, 240));
    float4 quarter_tail = vkr_ssr_cell_bounds(uint2(159, 119), 2u,
        source_extent, uint2(160, 120));
    float4 half_interior = vkr_ssr_cell_bounds(uint2(318, 238), 1u,
        source_extent, uint2(320, 240));
    for (uint i = 0; i < 4; ++i) {
        g_results[76 + i] = full_tail[i];
        g_results[80 + i] = half_tail[i];
        g_results[84 + i] = quarter_tail[i];
        g_results[88 + i] = half_interior[i];
    }
    // Both entries lie in pixel 252 on the positive ray, and pixel 388 on the
    // negative ray. Their next boundary is independent of how traversal arrived.
    params.source_width = source_extent.x;
    params.source_height = source_extent.y;
    trace.mip = 0u;
    trace.start_uv = float2(0.1f, 0.5f);
    trace.end_uv = float2(0.9f, 0.5f);
    trace.t = 0.368f;
    g_results[92] = vkr_ssr_trace_cell_exit(trace, source_extent, params);
    trace.t = 0.368002f;
    g_results[93] = vkr_ssr_trace_cell_exit(trace, source_extent, params);
    trace.start_uv = float2(0.9f, 0.5f);
    trace.end_uv = float2(0.1f, 0.5f);
    trace.t = 0.368f;
    g_results[94] = vkr_ssr_trace_cell_exit(trace, source_extent, params);
    trace.t = 0.368001f;
    g_results[95] = vkr_ssr_trace_cell_exit(trace, source_extent, params);
    // SSGI level zero still spans two source pixels: its next boundary is 254,
    // while SSR level zero exits at 253. SSR level one also exits at 254.
    trace.start_uv = float2(0.1f, 0.5f);
    trace.end_uv = float2(0.9f, 0.5f);
    trace.t = 0.368f;
    g_results[96] = vkr_ssgi_trace_cell_exit(trace, uint2(320, 240), params);
    trace.mip = 1u;
    g_results[97] = vkr_ssr_trace_cell_exit(trace, uint2(320, 240), params);
    for (uint i = 0; i < 4; ++i) {
        g_results[i] = sparse[i];
        g_results[4 + i] = dense[i];
        g_results[8 + i] = empty[i];
        g_results[12 + i] = mixed[i];
        g_results[16 + i] = stable[i];
        g_results[20 + i] = intermittent[i];
        g_results[24 + i] = unsupported[i];
        g_results[28 + i] = black[i];
        g_results[32 + i] = rejected[i];
    }
    // Source-resolution history accepts its first texel without widening
    // the independently half-resolution SSGI history footprint.
    g_results[124] = float(vkr_ssr_temporal_accept(params, identity, identity,
        0.5f, 0.5f, float2(.025f), 1.0f, uint2(20u)).accepted);
    g_results[125] = float(vkr_ssr_temporal_accept(params, identity, identity,
        0.5f, 0.5f, float2(.025f), 1.0f, uint2(10u)).accepted);
    g_results[126] = vkr_ssr_reconstruction_grid_weight(float2(1.5f, 0), .3f);
    g_results[127] = vkr_ssr_reconstruction_grid_weight(float2(.5f), .04f);
    params.source_width = 100u;
    params.source_height = 50u;
    params.roughness_cutoff = .6f;
    float2 fractional_hit = float2(.40625f, .3125f);
    float2 cone_right = vkr_ssr_hit_filter_uv(fractional_hit, .6f, 1u, params);
    float2 cone_up = vkr_ssr_hit_filter_uv(fractional_hit, .6f, 4u, params);
    g_results[128] = cone_right.x;
    g_results[129] = cone_up.y;
    float2 mirror_hit = vkr_ssr_hit_filter_uv(fractional_hit, .04f, 0u, params);
    g_results[130] = mirror_hit.x * 100.0f - .5f;
    float2 moved_hit = vkr_ssr_hit_filter_uv(fractional_hit + float2(.001953125f, 0),
                                           .04f, 0u, params);
    g_results[131] = (moved_hit.x - mirror_hit.x) * 100.0f;
    // A vanished bright reflection must lose half its unsupported residual per
    // frame even on a stationary rough receiver; current geometry still exists.
    decision.weight = .95f;
    decision.accepted = 1u;
    float4 step_history = float4(10, 10, 10, 1);
    for (uint frame = 0u; frame < 5u; ++frame) {
        step_history = vkr_ssr_temporal_filter(.3f, 3u,
            float4(2, 2, 2, 1), step_history, 2, 2, decision);
        g_results[132u + frame] = step_history.x;
    }
    float4 empty_history = float4(8, 4, 2, 1);
    for (uint frame = 0u; frame < 5u; ++frame)
        empty_history = vkr_ssr_temporal_filter(.3f, 0u, 0,
            empty_history, 1, -1, decision);
    g_results[137] = empty_history.w;
    g_results[138] = empty_history.x;
    g_results[139] = vkr_ssr_temporal_filter(VKR_SSR_MIRROR_ROUGHNESS, 3u,
        float4(2, 2, 2, 1), float4(10, 10, 10, 1), 2, 2, decision).x;

}

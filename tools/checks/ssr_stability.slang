// Execute production SSR filtering and loaded-depth refinement with analytic inputs.
#include "ssr_kernel.slangh"
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
    float4 stable = vkr_ssr_temporal_filter(params,
        float4(8, 4, 2, 0.5), float4(4, 2, 1, 0.5), 0, 16, decision);
    float4 intermittent = vkr_ssr_temporal_filter(params,
        0, float4(8, 4, 2, 0.5), 0, 16, decision);
    float4 unsupported = vkr_ssr_temporal_filter(params,
        0, float4(8, 4, 2, 0.5), 1, -1, decision);
    float4 black = vkr_ssr_temporal_filter(params,
        float4(0, 0, 0, 0.5), float4(8, 4, 2, 0.5), 0, 0, decision);
    decision.weight = 0.0f;
    decision.accepted = 0u;
    float4 rejected = vkr_ssr_temporal_filter(params,
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
        identity, 0.5f, 0.5f, float2(0.5f), 1.0f);
    VkrSsrHistoryDecision foreign = vkr_ssr_temporal_accept(params, identity,
        uint2(2u, 2u), 0.5f, 0.5f, float2(0.5f), 1.0f);
    VkrSsrHistoryDecision disoccluded = vkr_ssr_temporal_accept(params, identity,
        identity, 0.75f, 0.5f, float2(0.5f), 1.0f);
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
    float4 partial_filtered = vkr_ssr_temporal_filter(params,
        float4(10, 10, 10, 1), partial, 0, 10, decision);
    g_results[48] = partial_filtered.x;
    // Two full-resolution pixel slabs occupy [0, 0.5) and [0.5, 1).
    // Their minimum is 10, but pixel zero has depth 11 or 13. The coarse
    // candidate lands in pixel zero; only depth 11 actually intersects it.
    params.source_width = 2u;
    params.source_height = 1u;
    params.thickness = 0.1f;
    VkrSsrTrace trace = vkr_ssr_empty_trace();
    trace.start_uv = float2(0, 0.5f);
    trace.end_uv = float2(1, 0.5f);
    trace.start_depth = 9.0f;
    trace.end_depth = 15.0f;
    trace.start_clip_w = 1.0f;
    trace.end_clip_w = 1.0f;
    trace.active = 1u;
    VkrSsrTraceResolve ortho_hit = vkr_ssr_trace_refine_loaded_depth(
        trace, 11.0f, 1.0f, uint2(0), params);
    VkrSsrTraceResolve ortho_reject = vkr_ssr_trace_refine_loaded_depth(
        trace, 13.0f, 1.0f, uint2(0), params);
    trace.start_clip_w = 9.0f;
    trace.end_clip_w = 15.0f;
    VkrSsrTraceResolve perspective_hit = vkr_ssr_trace_refine_loaded_depth(
        trace, 11.0f, 1.0f, uint2(0), params);
    VkrSsrTraceResolve perspective_reject = vkr_ssr_trace_refine_loaded_depth(
        trace, 13.0f, 1.0f, uint2(0), params);
    g_results[49] = float(ortho_hit.hit);
    g_results[50] = ortho_hit.hit_t;
    g_results[51] = float(ortho_reject.hit);
    g_results[52] = ortho_reject.hit_t;
    g_results[53] = float(perspective_hit.hit);
    g_results[54] = perspective_hit.hit_t;
    g_results[55] = float(perspective_reject.hit);
    g_results[56] = perspective_reject.hit_t;
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
}

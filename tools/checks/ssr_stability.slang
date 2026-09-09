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
    float4 intermittent = vkr_ssr_temporal_filter(0.25f, 0u,
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
    // Reflection history follows a feature across receiver points. Constant
    // incoming light stays 10; only the current receiver's response shades it.
    decision.weight = .5f;
    decision.accepted = 1u;
    float4 incoming_history = vkr_ssr_temporal_filter(.3f, 3u,
        float4(10, 10, 10, .6f), float4(10, 10, 10, .6f), 10, 10, decision);
    float4 shaded_history = vkr_ssr_shade_reflection(incoming_history, float3(.8f));
    for (uint component = 0u; component < 4u; ++component)
        g_results[116u + component] = incoming_history[component];
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
    // Broad bounds admit the old image, but sparse fresh coverage must still
    // replace at least 15% of its conditional radiance on every supported frame.
    decision.weight = .85f;
    float4 sparse_step = float4(8, 8, 8, 1);
    for (uint frame = 0u; frame < 5u; ++frame) {
        sparse_step = vkr_ssr_temporal_filter(.3f, 1u,
            float4(2, 2, 2, .05f), sparse_step, 0, 10, decision);
        g_results[140u + frame] = sparse_step.x;
        if (frame == 0u) g_results[145] = sparse_step.w;
    }
    float4 constant_sparse = vkr_ssr_temporal_filter(.3f, 1u,
        float4(8, 4, 2, .05f), float4(8, 4, 2, 1), 0, 10, decision);
    for (uint i = 0u; i < 4u; ++i) g_results[146u + i] = constant_sparse[i];
    // Weak old coverage must keep its lower contribution; the cap cannot
    // promote it to the configured temporal weight.
    float4 weak_history = vkr_ssr_temporal_filter(.3f, 1u,
        float4(2, 2, 2, 1), float4(8, 8, 8, .05f), 0, 10, decision);
    g_results[150] = weak_history.x;
    g_results[151] = weak_history.w;
    decision.weight = 0.0f;
    float4 no_coverage = vkr_ssr_temporal_filter(.3f, 0u,
        0, 0, 1, -1, decision);
    for (uint i = 0u; i < 4u; ++i) g_results[152u + i] = no_coverage[i];

    // Unit-focal perspective: u=.5+.5*x/-z, v=.5-.5*y/-z.
    // The independently known mirror image of H=(.6,0,-1) in z=-2 is
    // Q=(.6,0,-3). A previous camera at x=-.2 sees Q at u=19/30,
    // while motion of the current receiver P=(.4,0,-2) gives u=.65.
    VkrSsrParams reflected = (VkrSsrParams)0;
    reflected.projection = float4x4(
        1, 0, 0, 0,
        0, -1, 0, 0,
        0, 0, -1, -1,
        0, 0, -1, 0);
    reflected.inverse_projection = float4x4(
        1, 0, 0, 0,
        0, -1, 0, 0,
        0, 0, 0, -1,
        0, 0, -1, 1);
    reflected.history_valid = 1u;
    reflected.temporal_weight = .85f;
    reflected.temporal_depth_absolute = .02f;
    reflected.temporal_depth_relative = .05f;
    float3 receiver_view = float3(.4f, 0, -2);
    float3 receiver_normal = float3(0, 0, 1);
    float3 hit_view = float3(.6f, 0, -1);
    float3 camera_delta = float3(.2f, 0, 0);
    VkrSsrReflectedHit translated = vkr_ssr_reproject_hit(reflected, float2(.6f, .5f),
        receiver_view, receiver_normal, hit_view,
        receiver_view + camera_delta, receiver_normal, hit_view + camera_delta,
        receiver_view + camera_delta, receiver_normal);
    g_results[156] = translated.previous_uv.x;
    g_results[157] = translated.previous_uv.y;
    g_results[158] = translated.expected_receiver_depth;
    g_results[159] = translated.expected_virtual_depth;
    g_results[160] = translated.current_virtual_depth;
    g_results[161] = float(translated.valid);
    float old_receiver_uv = vkr_ssr_project_view_position(
        reflected, receiver_view + camera_delta).uv.x;
    g_results[162] = old_receiver_uv;
    g_results[163] = old_receiver_uv - translated.previous_uv.x;

    // The trace receiver differs from this full-resolution receiver. Zero
    // motion must preserve the latter's offset from the half-resolution ray.
    float2 full_uv = float2(.6078125f, .49375f);
    float3 full_receiver = float3(.43125f, .025f, -2);
    VkrSsrReflectedHit fixed_offset = vkr_ssr_reproject_hit(reflected, full_uv,
        receiver_view, receiver_normal, hit_view,
        receiver_view, receiver_normal, hit_view, full_receiver, receiver_normal);
    VkrSsrReflectedHit translated_offset = vkr_ssr_reproject_hit(reflected, full_uv,
        receiver_view, receiver_normal, hit_view,
        receiver_view + camera_delta, receiver_normal, hit_view + camera_delta,
        full_receiver + camera_delta, receiver_normal);
    g_results[164] = fixed_offset.previous_uv.x;
    g_results[165] = fixed_offset.previous_uv.y;
    g_results[166] = fixed_offset.expected_receiver_depth;
    g_results[167] = fixed_offset.expected_virtual_depth;
    g_results[168] = fixed_offset.current_virtual_depth;
    g_results[169] = float(fixed_offset.valid);
    g_results[170] = translated_offset.previous_uv.x;
    g_results[171] = translated_offset.previous_uv.y;
    g_results[172] = translated_offset.expected_receiver_depth;
    g_results[173] = translated_offset.expected_virtual_depth;
    g_results[174] = translated_offset.current_virtual_depth;
    g_results[175] = float(translated_offset.valid);

    // For the plane y+z=-2, H=(0,1,-2) mirrors to Q=(0,0,-3).
    // Raster jitter changes lookup UV, never the plane-intersection depth.
    reflected.history_jitter_uv_x = .0025f;
    reflected.history_jitter_uv_y = -.005f;
    float3 tilted_receiver = float3(0, 0, -2);
    float3 tilted_normal = normalize(float3(0, 1, 1));
    float3 tilted_hit = float3(0, 1, -2);
    VkrSsrReflectedHit jittered = vkr_ssr_reproject_hit(reflected, float2(.5f),
        tilted_receiver, tilted_normal, tilted_hit,
        tilted_receiver, tilted_normal, tilted_hit, tilted_receiver, tilted_normal);
    g_results[176] = jittered.previous_uv.x;
    g_results[177] = jittered.previous_uv.y;
    g_results[178] = jittered.expected_receiver_depth;
    g_results[179] = jittered.expected_virtual_depth;
    g_results[180] = jittered.current_virtual_depth;
    g_results[181] = float(jittered.valid);
    reflected.history_jitter_uv_x = 0;
    reflected.history_jitter_uv_y = 0;

    float4x4 identity_model = float4x4(
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    float4x4 previous_hit_model = float4x4(
        1, 0, 0, -.2f, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    float4 transported_hit = vkr_ssr_transport_point(
        hit_view, identity_model, previous_hit_model);
    VkrSsrReflectedHit moving_hit = vkr_ssr_reproject_hit(reflected, float2(.6f, .5f),
        receiver_view, receiver_normal, hit_view,
        receiver_view, receiver_normal, transported_hit.xyz, receiver_view, receiver_normal);
    g_results[182] = moving_hit.previous_uv.x;
    g_results[183] = moving_hit.previous_uv.y;
    g_results[184] = moving_hit.expected_receiver_depth;
    g_results[185] = moving_hit.expected_virtual_depth;
    g_results[186] = moving_hit.current_virtual_depth;
    g_results[187] = float(moving_hit.valid);

    // Moving the receiver plane from z=-2 to z=-2.5 puts the virtual image
    // at z=-4 while its real emitter remains at z=-1.
    float4x4 previous_receiver_model = float4x4(
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -.5f, 0, 0, 0, 1);
    VkrSsrSurface transported_receiver = vkr_ssr_transport_surface(
        receiver_view, receiver_normal, identity_model, previous_receiver_model);
    VkrSsrReflectedHit moving_receiver = vkr_ssr_reproject_hit(reflected, float2(.6f, .5f),
        receiver_view, receiver_normal, hit_view,
        transported_receiver.position, transported_receiver.normal, hit_view,
        transported_receiver.position, transported_receiver.normal);
    g_results[188] = moving_receiver.previous_uv.x;
    g_results[189] = moving_receiver.previous_uv.y;
    g_results[190] = moving_receiver.expected_receiver_depth;
    g_results[191] = moving_receiver.expected_virtual_depth;
    g_results[192] = moving_receiver.current_virtual_depth;
    g_results[193] = float(moving_receiver.valid);

    // Object point (1,2,-1) maps independently to (3,4,1) and (-7,7,6.5).
    // Object normal (1,1,1) maps to (6,4,3)/sqrt(61) and (-3,2,12)/sqrt(157).
    // Unequal scales and a 90-degree rotation expose transpose/inverse mistakes.
    float4x4 current_affine = float4x4(
        2, 0, 0, 1, 0, 3, 0, -2, 0, 0, 4, 5, 0, 0, 0, 1);
    float4x4 previous_affine = float4x4(
        0, -2, 0, -3, 3, 0, 0, 4, 0, 0, .5f, 7, 0, 0, 0, 1);
    float4 affine_point = vkr_ssr_transport_point(
        float3(3, 4, 1), current_affine, previous_affine);
    VkrSsrSurface affine_surface = vkr_ssr_transport_surface(
        float3(3, 4, 1), normalize(float3(6, 4, 3)), current_affine, previous_affine);
    for (uint i = 0u; i < 4u; ++i) g_results[194u + i] = affine_point[i];
    for (uint i = 0u; i < 3u; ++i) {
        g_results[198u + i] = affine_surface.position[i];
        g_results[202u + i] = affine_surface.normal[i];
    }
    g_results[201] = float(affine_surface.valid);
    float4x4 singular = float4x4(
        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    g_results[205] = vkr_ssr_transport_point(receiver_view, singular, identity_model).w;
    g_results[206] = float(vkr_ssr_transport_surface(
        receiver_view, receiver_normal, singular, identity_model).valid);
    g_results[207] = float(vkr_ssr_transport_surface(
        receiver_view, receiver_normal, identity_model, singular).valid);
    g_results[208] = vkr_ssr_transport_point(
        float3(asfloat(0x7fc00000u), 0, 0), identity_model, identity_model).w;
    g_results[209] = float(vkr_ssr_transport_surface(
        receiver_view, 0, identity_model, identity_model).valid);
    g_results[210] = float(vkr_ssr_reproject_hit(reflected, float2(.5f),
        receiver_view, receiver_normal, hit_view,
        receiver_view, receiver_normal, hit_view,
        float3(0, 0, -2), float3(1, 0, 0)).valid);
    g_results[211] = float(vkr_ssr_reproject_hit(reflected, float2(.6f, .5f),
        receiver_view, receiver_normal, hit_view,
        float3(.4f, 0, 2), receiver_normal, hit_view,
        receiver_view, receiver_normal).valid);
    g_results[212] = float(vkr_ssr_reproject_hit(reflected, float2(.6f, .5f),
        receiver_view, receiver_normal, hit_view,
        receiver_view, receiver_normal, hit_view,
        float3(.4f, 0, 2), receiver_normal).valid);

    uint4 reflected_identity = uint4(1u, 2u, 3u, 4u);
    float4 geometry = float4(2, 3, 0, 0); // Front-facing +Z encodes to (0,0).
    uint2 history_extent = uint2(1280, 720);
    VkrSsrHistoryDecision reflected_matching = vkr_ssr_reflected_history_accept(
        reflected, translated, reflected_identity, reflected_identity,
        geometry, receiver_normal, history_extent);
    g_results[213] = reflected_matching.weight;
    g_results[214] = float(reflected_matching.accepted);
    for (uint i = 0u; i < 4u; ++i) {
        uint4 foreign_identity = reflected_identity;
        foreign_identity[i] += 1u;
        g_results[215u + i] = float(vkr_ssr_reflected_history_accept(
            reflected, translated, reflected_identity, foreign_identity,
            geometry, receiver_normal, history_extent).accepted);
    }
    g_results[219] = float(vkr_ssr_reflected_history_accept(
        reflected, translated, reflected_identity, reflected_identity,
        float4(3, 3, 0, 0), receiver_normal, history_extent).accepted);
    g_results[220] = float(vkr_ssr_reflected_history_accept(
        reflected, translated, reflected_identity, reflected_identity,
        float4(2, 4, 0, 0), receiver_normal, history_extent).accepted);
    g_results[221] = float(vkr_ssr_reflected_history_accept(
        reflected, translated, reflected_identity, reflected_identity,
        float4(2, 3, 1, 1), receiver_normal, history_extent).accepted);
    reflected.history_valid = 0u;
    g_results[222] = float(vkr_ssr_reflected_history_accept(
        reflected, translated, reflected_identity, reflected_identity,
        geometry, receiver_normal, history_extent).accepted);
    reflected.history_valid = 1u;
    VkrSsrReflectedHit invalid_hit = translated;
    invalid_hit.valid = 0u;
    g_results[223] = float(vkr_ssr_reflected_history_accept(
        reflected, invalid_hit, reflected_identity, reflected_identity,
        geometry, receiver_normal, history_extent).accepted);
    invalid_hit = translated;
    invalid_hit.previous_uv = float2(1.5f, .5f);
    g_results[224] = float(vkr_ssr_reflected_history_accept(
        reflected, invalid_hit, reflected_identity, reflected_identity,
        geometry, receiver_normal, history_extent).accepted);
    uint4 no_hit_identity = uint4(1u, 2u, 0u, 0u);
    g_results[225] = float(vkr_ssr_reflected_history_accept(
        reflected, translated, no_hit_identity, no_hit_identity,
        geometry, receiver_normal, history_extent).accepted);
    g_results[226] = float(vkr_ssr_reflected_history_accept(
        reflected, translated, reflected_identity, reflected_identity,
        float4(2, 0, 0, 0), receiver_normal, history_extent).accepted);
    uint4 no_receiver_identity = uint4(0u, 0u, 3u, 4u);
    g_results[227] = float(vkr_ssr_reflected_history_accept(
        reflected, translated, no_receiver_identity, no_receiver_identity,
        geometry, receiver_normal, history_extent).accepted);

    float2 encoded_front = vkr_ssr_encode_normal(normalize(float3(6, 4, 3)));
    float3 decoded_front = vkr_ssr_decode_normal(float2(6.0f / 13.0f, 4.0f / 13.0f));
    float2 encoded_back = vkr_ssr_encode_normal(normalize(float3(-6, 4, -3)));
    float3 decoded_back = vkr_ssr_decode_normal(float2(-9.0f / 13.0f, 7.0f / 13.0f));
    for (uint i = 0u; i < 2u; ++i) {
        g_results[228u + i] = encoded_front[i];
        g_results[233u + i] = encoded_back[i];
    }
    for (uint i = 0u; i < 3u; ++i) {
        g_results[230u + i] = decoded_front[i];
        g_results[235u + i] = decoded_back[i];
    }
    float3 perspective_position = vkr_ssr_view_from_positive_depth(
        reflected, float2(.6f, .45f), 2);
    VkrSsrParams orthographic = reflected;
    orthographic.projection = float4x4(
        1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -.1f, -.1f, 0, 0, 0, 1);
    orthographic.inverse_projection = float4x4(
        1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -10, -1, 0, 0, 0, 1);
    float3 orthographic_position = vkr_ssr_view_from_positive_depth(
        orthographic, float2(.6f, .45f), 2);
    for (uint i = 0u; i < 3u; ++i) {
        g_results[238u + i] = perspective_position[i];
        g_results[241u + i] = orthographic_position[i];
    }
    VkrSsrReflectedHit orthographic_hit = vkr_ssr_reproject_hit(
        orthographic, float2(.8f, .5f), float3(.6f, 0, -2), receiver_normal, hit_view,
        float3(.8f, 0, -2), receiver_normal, hit_view + camera_delta,
        float3(.8f, 0, -2), receiver_normal);
    g_results[244] = orthographic_hit.previous_uv.x;
    g_results[245] = orthographic_hit.previous_uv.y;
    g_results[246] = orthographic_hit.expected_receiver_depth;
    g_results[247] = orthographic_hit.expected_virtual_depth;
    g_results[248] = orthographic_hit.current_virtual_depth;
    g_results[249] = float(orthographic_hit.valid);
    float4 current_response = vkr_ssr_shade_reflection(incoming_history, float3(.2f, .4f, .8f));
    for (uint i = 0u; i < 4u; ++i) g_results[250u + i] = current_response[i];

    float4x4 mirrored_current = current_affine;
    mirrored_current[0][0] = -2;
    float4 mirrored_point = vkr_ssr_transport_point(
        float3(-1, 4, 1), mirrored_current, previous_affine);
    VkrSsrSurface mirrored_surface = vkr_ssr_transport_surface(
        float3(-1, 4, 1), normalize(float3(-6, 4, 3)), mirrored_current, previous_affine);
    for (uint i = 0u; i < 4u; ++i) g_results[254u + i] = mirrored_point[i];
    for (uint i = 0u; i < 3u; ++i) g_results[258u + i] = mirrored_surface.normal[i];
    g_results[261] = float(mirrored_surface.valid);

    // Current raster jitter is (.0025,-.005), previous is (.00125,.0025).
    // Matrix projection already contains current jitter; only their delta is added.
    reflected.projection[0][2] = -.005f;
    reflected.projection[1][2] = .01f;
    reflected.inverse_projection[0][3] = -.005f;
    reflected.inverse_projection[1][3] = -.01f;
    reflected.history_jitter_uv_x = -.00125f;
    reflected.history_jitter_uv_y = .0075f;
    VkrSsrReflectedHit both_jittered = vkr_ssr_reproject_hit(
        reflected, full_uv + float2(.0025f, -.005f),
        receiver_view, receiver_normal, hit_view,
        receiver_view, receiver_normal, hit_view, full_receiver, receiver_normal);
    g_results[262] = both_jittered.previous_uv.x;
    g_results[263] = both_jittered.previous_uv.y;
    g_results[264] = both_jittered.expected_receiver_depth;
    g_results[265] = both_jittered.expected_virtual_depth;
    g_results[266] = both_jittered.current_virtual_depth;
    g_results[267] = float(both_jittered.valid);
}

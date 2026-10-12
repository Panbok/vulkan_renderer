// Execute production SSGI sampling, filtering and history math with analytic
// inputs. check_ssgi_kernel.py states each expected value independently.
#include "exposure_kernel.slangh"
#include "ssr_kernel.slangh"
#include "ssgi_kernel.slangh"
RWStructuredBuffer<float> g_results;

// One when `count` consecutive phases from `start` put exactly one sample in
// each of `count` equal strata of the chosen dimension.
float ssgi_check_strata(uint start, uint count, uint dimension)
{
    uint seen = 0u;
    for (uint i = 0u; i < count; ++i)
    {
        float2 point = vkr_ssgi_sequence_2d(start + i);
        uint stratum = min(uint(point[dimension] * float(count)), count - 1u);
        if ((seen & (1u << stratum)) != 0u)
            return 0.0f;
        seen |= 1u << stratum;
    }
    return 1.0f;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ssgi_kernel_execute(uint3 id : SV_DispatchThreadID)
{
    // Short windows of the 256-phase sequence are stratified in both
    // dimensions, including base two across the period's end.
    uint starts[4] = {0u, 37u, 100u, 245u};
    float elevation = 1.0f;
    for (uint i = 0u; i < 4u; ++i)
        elevation = min(elevation, ssgi_check_strata(starts[i], 16u, 0u));
    g_results[0] = elevation;
    float azimuth = 1.0f;
    for (uint i = 0u; i < 3u; ++i)
        azimuth = min(azimuth, ssgi_check_strata(starts[i], 9u, 1u));
    g_results[1] = azimuth;
    uint cells = 0u;
    for (uint i = 0u; i < 6u; ++i)
    {
        float2 point = vkr_ssgi_sequence_2d(37u + i);
        cells |= 1u << (uint(point.x * 2.0f) * 3u + uint(point.y * 3.0f));
    }
    g_results[2] = float(cells == 63u);
    g_results[3] = float(all(vkr_ssgi_sequence_2d(5u) ==
                             vkr_ssgi_sequence_2d(5u + VKR_SSGI_SEQUENCE_LENGTH)));

    // Firefly bound: the larger of four times the reference luminance and
    // the radiance whose share of the estimate adds a quarter of displayed
    // luminance. Nine samples of weight one (share 1/9) at display scale
    // .853: a lone bulb hit among dim cobblestones, a dense bright
    // neighbourhood, a dim lone hit; then a red hit alone (share 1) at
    // display scale 1 against reference .2.
    float bulb_sum = 1830.0f + 8.0f * 0.04f;
    g_results[4] = vkr_ssgi_firefly_clamp(float3(1830.0f),
        vkr_ssgi_other_mean_luminance(bulb_sum, 9.0f, 1830.0f, 1.0f),
        1.0f / 9.0f, .853f).x;
    g_results[5] = vkr_ssgi_firefly_clamp(float3(10.0f),
        vkr_ssgi_other_mean_luminance(90.0f, 9.0f, 10.0f, 1.0f),
        1.0f / 9.0f, .853f).x;
    g_results[6] = vkr_ssgi_firefly_clamp(float3(0.5f),
        vkr_ssgi_other_mean_luminance(0.5f, 9.0f, 0.5f, 1.0f),
        1.0f / 9.0f, .853f).x;
    float3 red = vkr_ssgi_firefly_clamp(float3(9.0f, 0.0f, 0.0f), 0.2f,
                                        1.0f, 1.0f);
    g_results[7] = red.x;

    // Plane weights in view space.
    float3 up = float3(0, 1, 0);
    float3 facing = float3(0, 0, 1);
    g_results[8] = vkr_ssgi_plane_weight(float2(1, 0), float3(0, -1.7f, -50),
        up, float3(.05f, -1.7f, -55.4f), up, .1f);
    g_results[9] = vkr_ssgi_plane_weight(float2(1, 0), float3(0, 0, -10),
        facing, float3(.02f, 0, -5), facing, .1f);
    g_results[10] = vkr_ssgi_plane_weight(float2(1, 0), float3(0, 0, -2),
        facing, float3(.02f, 0, -2), float3(0, .8660254f, .5f), .1f);

    // Unit-focal perspective with u = .5 + .5 x / -z and v = .5 - .5 y / -z.
    VkrSsrParams screen = (VkrSsrParams)0;
    screen.projection = float4x4(
        1, 0, 0, 0,
        0, -1, 0, 0,
        0, 0, -1, -1,
        0, 0, -1, 0);
    screen.inverse_projection = float4x4(
        1, 0, 0, 0,
        0, -1, 0, 0,
        0, 0, 0, -1,
        0, 0, -1, 1);
    screen.history_valid = 1u;
    screen.temporal_depth_absolute = .02f;
    screen.temporal_depth_relative = .05f;
    g_results[11] = vkr_ssgi_view_at_depth(screen, float2(.75f, .5f), 4.0f).x;
    // Floor y = -1 below the camera; v = .6 looks down by .2 per unit depth.
    float3 floor_point = float3(0, -1, -5);
    g_results[12] = vkr_ssgi_plane_depth(screen, floor_point, up, float2(.5f, .6f));
    g_results[13] = vkr_ssgi_history_tap_depth(screen, floor_point, up, 5.5f,
                                               float2(.5f, .55f));
    uint2 identity = uint2(3u, 7u);
    g_results[14] = float(vkr_ssgi_history_tap_accepted(
        screen, identity, identity, 11.2f, 11.0f, 0.0f, true, 1.0f));
    g_results[15] = float(vkr_ssgi_history_tap_accepted(
        screen, identity, uint2(4u, 7u), 11.0f, 11.0f, 0.0f, true, 1.0f));
    g_results[16] = float(vkr_ssgi_history_tap_accepted(
        screen, identity, identity, 13.0f, 11.0f, 0.0f, true, 1.0f));
    g_results[17] = float(vkr_ssgi_history_tap_accepted(
        screen, identity, identity, 13.0f, 11.0f, 2.0f, true, 1.0f));

    // Distance-aware thickness at 1080 rows and a 90 degree vertical field.
    VkrSsgiParams params = (VkrSsgiParams)0;
    params.thickness = .1f;
    params.pixel_footprint = 2.0f / 1080.0f;
    g_results[18] = vkr_ssgi_thickness(params, 2.0f);
    g_results[19] = vkr_ssgi_thickness(params, 100.0f);

    // Non-finite radiance never reaches the half-float images.
    float nan = asfloat(0x7fc00000u);
    float infinity = asfloat(0x7f800000u);
    g_results[20] = vkr_ssgi_bare_diffuse_estimate(float3(nan, 1, 1)).y;
    g_results[21] = vkr_ssgi_storable(float3(1, infinity, 1)).x;
    g_results[22] = vkr_ssgi_storable(float3(1e6f, 2, 3)).x;

    // All-miss neighbourhoods: fully supported history (a static receiver,
    // or a camera pan over static geometry) keeps full retention, a half
    // disoccluded footprint decays faster, and a receiver with no answered
    // ray keeps its history at .95 per frame.
    float4 supported_miss = vkr_ssgi_temporal_filter(float4(0, 0, 0, 1),
        float3(8, 4, 2), 0, 0, .9f, vkr_ssgi_empty_retention(1.0f));
    float4 disoccluded_miss = vkr_ssgi_temporal_filter(float4(0, 0, 0, 1),
        float3(8, 4, 2), 0, 0, .9f, vkr_ssgi_empty_retention(.5f));
    float4 unanswered = vkr_ssgi_temporal_filter(float4(0, 0, 0, 0),
        float3(8, 4, 2), 0, 0, .9f, 1.0f);
    g_results[23] = supported_miss.x;
    g_results[24] = disoccluded_miss.x;
    g_results[25] = unanswered.x;
    g_results[26] = unanswered.w;

    // The trace starts at the receiver's leaf and still ascends to the top.
    screen.source_width = 64u;
    screen.source_height = 64u;
    screen.depth_mip_count = 6u;
    screen.max_distance = 20.0f;
    VkrSsrTrace trace = vkr_ssgi_trace_begin(screen, float3(0, 0, -2),
                                             float3(.3f, 0, -1));
    g_results[27] = float(trace.active);
    g_results[28] = float(trace.mip);
    g_results[29] = float(trace.max_mip);

    // A floor 1 m below a counter top, seen 8.6 m past its edge from .7 m
    // above it: the counter plane predicts a 1.2 cm separation there, so the
    // floor is another surface (the measured separation gave .54 * .5).
    g_results[30] = vkr_ssgi_plane_weight(float2(1, 0), float3(0, -.7f, -6),
        up, float3(0, -1.7f, -14.6f), up, .1f);

    // History placement range of the floor y = -1 over one trace texel of
    // .01. At v = .505 (depth 100) the probe toward the horizon leaves the
    // plane and takes the cap, the receiver's depth; the far probe alone gave
    // 66.7. At v = .6 (depth 5) the receding side changes .5556, the near
    // side .4545.
    float2 texel = float2(.01f, .01f);
    g_results[31] = vkr_ssgi_texel_depth_change(screen,
        vkr_ssgi_view_at_depth(screen, float2(.5f, .505f), 100.0f), up,
        float2(.5f, .505f), texel, 100.0f);
    g_results[32] = vkr_ssgi_texel_depth_change(screen,
        vkr_ssgi_view_at_depth(screen, float2(.5f, .6f), 5.0f), up,
        float2(.5f, .6f), texel, 5.0f);

    // Firefly bound, continued: the red hit keeps its hue; a sample alone in
    // its neighbourhood has no other mean. A receiver whose neighbours lie on
    // other surfaces (share 1) keeps a bulb hit to a quarter of displayed
    // luminance over its history of .04, but bounce of 1 that its history
    // already holds at .3 passes; at display scale 8 the quarter is 1/32.
    g_results[33] = red.y;
    g_results[34] = vkr_ssgi_other_mean_luminance(5.0f, 1.0f, 5.0f, 1.0f);
    g_results[35] = vkr_ssgi_firefly_clamp(float3(1830.0f), 0.04f, 1.0f, .853f).x;
    g_results[36] = vkr_ssgi_firefly_clamp(float3(1.0f), 0.3f, 1.0f, .853f).x;
    g_results[37] = vkr_ssgi_firefly_clamp(float3(1830.0f), 0.0f, 1.0f, 8.0f).x;
}

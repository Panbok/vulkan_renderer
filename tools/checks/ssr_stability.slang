// Execute the production radiance/coverage reduction with analytic inputs.
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

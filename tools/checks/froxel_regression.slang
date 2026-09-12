#include "froxel_fog_kernel.slangh"

RWStructuredBuffer<float> g_results;

[shader("compute")]
[numthreads(1, 1, 1)]
void froxel_regression_execute(uint3 dispatch_id : SV_DispatchThreadID)
{
    uint cursor = 0u;
    float3 radiance = float3(1000.0f, 10000.0f, 60000.0f);
    VkrFroxelSample empty = {float3(0.0f), 1.0f};
    float3 opaque = vkr_froxel_apply_integrated(radiance, empty).inscatter;
    float3 local = vkr_froxel_apply_local_over_feedback(
        radiance, float3(0.2f, 0.5f, 0.9f), empty);
    VkrFroxelSample thin = {float3(0.25f, 0.5f, 1.0f), 0.999f};
    float3 thin_opaque = vkr_froxel_apply_integrated(radiance, thin).inscatter;
    float3 thin_local = vkr_froxel_apply_local_over_feedback(
        radiance, float3(0.2f, 0.5f, 0.9f), thin);
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        g_results[cursor++] = opaque[channel];
        g_results[cursor++] = local[channel];
        g_results[cursor++] = thin_opaque[channel];
        g_results[cursor++] = thin_local[channel];
    }

    float4x4 view = float4x4(1, 0, 0, 0,
                             0, 1, 0, 0,
                             0, 0, 1, 0,
                             0, 0, 0, 1);
    // Unit focal length, near .1; far 50, 500 and infinity. The inverse
    // perspective matrices are specified directly from the projection law.
    for (uint projection = 0u; projection < 3u; ++projection)
    {
        float inverse_far = projection == 0u ? 1.0f / 50.0f :
                            projection == 1u ? 1.0f / 500.0f : 0.0f;
        VkrFroxelFogParams params = (VkrFroxelFogParams)0;
        params.inverse_view_projection = float4x4(
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 0, -1,
            0, 0, inverse_far - 10.0f, 10.0f);
        params.height_distance_phase.z = 200.0f;
        params.depth_mapping = float4(0.1f, log(2000.0f), 1.0f / log(2000.0f), 0);
        params.grid_dimensions_cell_pixels = uint4(2u, 2u, 64u, 16u);
        params.color_density.w = 0.01f;
        params.selected_local_indices_count.w = 1u;
        params.boxes[0].min_density = float4(90, -110, -210, 2);
        params.boxes[0].max_reserved = float4(110, -90, -190, 0);
        float3 sky = vkr_froxel_world_at_view_depth(
            params, view, float2(0.5f, -0.5f), 200.0f);
        float3 cell = vkr_froxel_world_center(params, view, uint3(1u, 0u, 63u));
        g_results[cursor++] = sky.x;
        g_results[cursor++] = sky.y;
        g_results[cursor++] = sky.z;
        g_results[cursor++] = cell.x;
        g_results[cursor++] = cell.y;
        g_results[cursor++] = cell.z;
        g_results[cursor++] = vkr_froxel_height_density(params, sky);
        VkrFroxelSample absorption = vkr_froxel_integrate_step(
            empty, float3(0.0f), 0.01f, -sky.z - 0.1f);
        g_results[cursor++] = absorption.transmittance;
    }
}

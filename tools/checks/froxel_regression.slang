#include "froxel_fog_kernel.slangh"
#include "sh_l2_kernel.slangh"

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

    // Henyey-Greenstein integrated over the sphere by the midpoint rule in
    // cos(theta): its total and mean cosine, then its isotropic and forward
    // values.
    const float anisotropies[4] = {-0.6f, 0.0f, 0.5f, 0.9f};
    const uint phase_steps = 200000u;
    for (uint index = 0u; index < 4u; ++index)
    {
        // Double accumulators keep 200,000 float terms from drifting.
        double total = 0.0l;
        double mean_cosine = 0.0l;
        for (uint step = 0u; step < phase_steps; ++step)
        {
            float cosine = -1.0f + 2.0f * (float(step) + 0.5f) / float(phase_steps);
            double weight = 4.0l * 3.14159265358979323846l / double(phase_steps);
            double phase = double(vkr_fog_phase_hg(anisotropies[index], cosine));
            total += phase * weight;
            mean_cosine += phase * double(cosine) * weight;
        }
        g_results[cursor++] = float(total);
        g_results[cursor++] = float(mean_cosine);
        g_results[cursor++] = vkr_fog_phase_hg(anisotropies[index], 1.0f);
    }
    g_results[cursor++] = vkr_fog_phase_hg(0.0f, 0.3f);

    // Analytic fog keeps its constant colour until a sky-lit albedo is set.
    VkrFogParams fog = (VkrFogParams)0;
    fog.color_density = float4(0.5f, 0.6f, 0.7f, 0.01f);
    VkrFogLighting lighting;
    lighting.sun_direction = float3(0.0f, 0.6f, 0.8f);
    lighting.sun_irradiance = float3(2.0f, 1.5f, 1.0f);
    lighting.sky_ambient = float3(0.1f, 0.2f, 0.3f);
    float3 constant_colour = vkr_fog_inscatter(fog, lighting,
                                               float3(0.0f, 0.6f, 0.8f));
    fog.sky_lighting = float4(0.8f, 0.5f, 0.0f, 0.0f);
    float3 toward_sun = vkr_fog_inscatter(fog, lighting,
                                          float3(0.0f, 0.6f, 0.8f));
    float3 across_sun = vkr_fog_inscatter(fog, lighting,
                                          float3(1.0f, 0.0f, 0.0f));
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        g_results[cursor++] = constant_colour[channel];
        g_results[cursor++] = toward_sun[channel];
        g_results[cursor++] = across_sun[channel];
    }

    // The packed SH average against the production evaluator's mean over a
    // Fibonacci sphere.
    VkrShL2Packed sh;
    sh.v[0] = float4(0.10f, -0.05f, 0.20f, 1.00f);
    sh.v[1] = float4(-0.08f, 0.12f, 0.03f, 0.80f);
    sh.v[2] = float4(0.02f, 0.04f, -0.10f, 0.60f);
    sh.v[3] = float4(0.05f, -0.03f, 0.30f, 0.02f);
    sh.v[4] = float4(-0.02f, 0.06f, 0.24f, -0.04f);
    sh.v[5] = float4(0.03f, 0.01f, -0.15f, 0.05f);
    sh.v[6] = float4(0.07f, -0.05f, 0.04f, 0.0f);
    float3 average = vkr_sh_l2_average(sh);
    const uint sphere_points = 65536u;
    double3 integrated = double3(0.0l);
    for (uint point = 0u; point < sphere_points; ++point)
    {
        float z = 1.0f - 2.0f * (float(point) + 0.5f) / float(sphere_points);
        float radius = sqrt(max(1.0f - z * z, 0.0f));
        float azimuth = float(point) * 2.39996322972865332f;
        float3 normal = float3(radius * cos(azimuth), radius * sin(azimuth), z);
        integrated += double3(
            vkr_sh_l2_evaluate(sh, vkr_sh_l2_prepare_evaluation(normal)));
    }
    integrated /= double(sphere_points);
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        g_results[cursor++] = average[channel];
        g_results[cursor++] = float(integrated[channel]);
    }

    // Froxel source: extinction times albedo times the scattered radiance,
    // then the hue-preserving 5,000 cap.
    VkrFroxelFogParams froxel = (VkrFroxelFogParams)0;
    froxel.color_density = float4(0.9f, 0.8f, 0.7f, 0.02f);
    float3 source = vkr_froxel_local_source(0.02f, float3(1.0f, 2.0f, 3.0f),
                                            froxel);
    float3 capped = vkr_froxel_local_source(100.0f,
                                            float3(1.0e4f, 2.0e4f, 4.0e4f),
                                            froxel);
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        g_results[cursor++] = source[channel];
        g_results[cursor++] = capped[channel];
    }
}

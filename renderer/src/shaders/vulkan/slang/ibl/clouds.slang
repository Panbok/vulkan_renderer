// Volumetric cloud layer (ADR-074): one-time noise synthesis, the
// sun-projected shadow map and the half-resolution trace with temporal
// accumulation. Shared math lives in shared/cloud_kernel.slangh.

struct VkrVulkanCloudNoiseRoot
{
    uint base_texture;
    uint detail_texture;
    uint weather_texture;
    uint reserved;
};

struct VkrVulkanCloudNoisePush
{
    VkrVulkanCloudNoiseRoot *root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanCloudNoisePush> g_cloud_noise_push;

[shader("compute")]
[numthreads(4, 4, 4)]
void cloud_base_noise_compute(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= uint3(VKR_CLOUD_BASE_NOISE_SIZE)))
        return;
    float3 uvw = (float3(id) + 0.5f) / float(VKR_CLOUD_BASE_NOISE_SIZE);
    g_storage_3d[g_cloud_noise_push.root.base_texture][id] =
        float4(vkr_cloud_base_noise(uvw), 0.0f, 0.0f, 1.0f);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void cloud_detail_noise_compute(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= uint3(VKR_CLOUD_DETAIL_NOISE_SIZE)))
        return;
    float3 uvw = (float3(id) + 0.5f) / float(VKR_CLOUD_DETAIL_NOISE_SIZE);
    g_storage_3d[g_cloud_noise_push.root.detail_texture][id] =
        float4(vkr_cloud_detail_noise(uvw), 0.0f, 0.0f, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cloud_weather_compute(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(VKR_CLOUD_WEATHER_SIZE)))
        return;
    float2 uv = (float2(id.xy) + 0.5f) / float(VKR_CLOUD_WEATHER_SIZE);
    g_storage_2d[g_cloud_noise_push.root.weather_texture][id.xy] =
        float4(vkr_cloud_weather_noise(uv), 0.0f, 0.0f, 1.0f);
}

// Coverage-shaped density without detail erosion. The weather map is read
// first: where it leaves no coverage the base volume is not sampled.
float vk_cloud_shape(VkrVulkanSky *sky, VkrCloudParams clouds,
                     VkrCloudSample sample)
{
    float weather = g_textures[sky.cloud_weather_texture]
                        .SampleLevel(g_samplers[sky.cloud_noise_sampler],
                                     sample.weather_uv, 0.0f)
                        .r;
    if (vkr_cloud_shape_density(clouds, 1.0f, weather,
                                sample.height_fraction) <= 0.0f)
        return 0.0f;
    float base = g_3d_textures[sky.cloud_base_noise_texture]
                     .SampleLevel(g_samplers[sky.cloud_noise_sampler],
                                  sample.base_uvw, 0.0f)
                     .r;
    return vkr_cloud_shape_density(clouds, base, weather,
                                   sample.height_fraction);
}

// Transmittance of the whole layer along the sun ray that enters its base at
// the texel's world position. The planar march ignores curvature over the
// map's few kilometres.
[shader("compute")]
[numthreads(8, 8, 1)]
void cloud_shadow_compute(uint3 id : SV_DispatchThreadID)
{
    VkrVulkanSkyBuildRoot *root = g_sky_build_push.root;
    if (any(id.xy >= uint2(VKR_CLOUD_SHADOW_SIZE)))
        return;
    VkrVulkanSky *sky = root.sky;
    VkrSkyParams params = sky.params;
    VkrCloudParams clouds = params.clouds;
    float2 uv = (float2(id.xy) + 0.5f) / float(VKR_CLOUD_SHADOW_SIZE);
    float2 entry_km = vkr_cloud_shadow_entry_km(clouds, uv);
    float2 camera_km =
        float2(params.camera_position.x, params.camera_position.z) *
        params.camera_position.w;
    float2 slope = vkr_cloud_shadow_slope(params);
    float climb_step =
        (clouds.layer.y - clouds.layer.x) / float(VKR_CLOUD_SHADOW_STEPS);
    float path_step = climb_step * sqrt(1.0f + dot(slope, slope));
    float optical_depth = 0.0f;
    for (uint step = 0u; step < VKR_CLOUD_SHADOW_STEPS; ++step)
    {
        float climb = (float(step) + 0.5f) * climb_step;
        VkrCloudSample sample =
            vkr_cloud_sample(params, entry_km + slope * climb - camera_km,
                             clouds.layer.x + climb);
        optical_depth +=
            vk_cloud_shape(sky, clouds, sample) * clouds.layer.z * path_step;
    }
    g_storage_2d[root.output_texture][id.xy] = float4(exp(-optical_depth));
}

struct VkrVulkanCloudTraceRoot
{
    float4x4 previous_view_projection;
    VkrVulkanSky *sky;
    VkrVulkanPacketFrameRoot *frame;
    uint depth_texture;
    uint history_texture;
    uint history_sampler;
    uint output_texture;
    uint2 extent;
    uint2 depth_extent;
    uint frame_index;
    uint history_valid;
    uint2 reserved;
};

struct VkrVulkanCloudTracePush
{
    VkrVulkanCloudTraceRoot *root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanCloudTracePush> g_cloud_trace_push;

// Optical depth toward the sun through the base shape, with doubling steps.
float vk_cloud_light_depth(VkrVulkanSky *sky, VkrSkyParams params,
                           float3 position, float3 sun)
{
    VkrCloudParams clouds = params.clouds;
    float optical_depth = 0.0f;
    float travelled = 0.0f;
    float step = VKR_CLOUD_LIGHT_FIRST_STEP_KM;
    for (uint index = 0u; index < VKR_CLOUD_LIGHT_STEPS; ++index)
    {
        float3 sample_position = position + sun * (travelled + 0.5f * step);
        float radius = length(sample_position);
        if (radius > clouds.layer.y)
            break;
        VkrCloudSample sample =
            vkr_cloud_sample(params, sample_position.xz, radius);
        optical_depth +=
            vk_cloud_shape(sky, clouds, sample) * clouds.layer.z * step;
        travelled += step;
        step *= 2.0f;
    }
    return optical_depth;
}

// Cloud in-scatter with aerial perspective at the layer's apparent depth, and
// the layer's gray transmittance.
float4 vk_cloud_march(VkrVulkanCloudTraceRoot *root, float3 direction,
                      uint2 texel)
{
    VkrVulkanSky *sky = root.sky;
    VkrSkyParams params = sky.params;
    VkrCloudParams clouds = params.clouds;
    VkrCloudSegment segment = vkr_cloud_segment(params, direction);
    if (segment.end_km <= segment.start_km)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float camera_radius =
        params.atmosphere.planet.x + params.atmosphere.planet.z;
    float3 origin = float3(0.0f, camera_radius, 0.0f);
    float3 sun = params.atmosphere.sun.xyz;
    float cosine = dot(direction, sun);
    float3 ambient = packet_sky_ambient(root.frame);
    float step_km = (segment.end_km - segment.start_km) /
                    float(VKR_CLOUD_PRIMARY_STEPS);
    float offset = vkr_cloud_march_offset(texel, root.frame_index);
    VkrCloudIntegration state = vkr_cloud_integration_begin();
    for (uint index = 0u; index < VKR_CLOUD_PRIMARY_STEPS; ++index)
    {
        float distance = segment.start_km + (float(index) + offset) * step_km;
        float3 position = origin + direction * distance;
        float radius = length(position);
        VkrCloudSample sample = vkr_cloud_sample(params, position.xz, radius);
        float shape = vk_cloud_shape(sky, clouds, sample);
        if (shape <= 0.0f)
            continue;
        float detail = g_3d_textures[sky.cloud_detail_noise_texture]
                           .SampleLevel(g_samplers[sky.cloud_noise_sampler],
                                        sample.detail_uvw, 0.0f)
                           .r;
        float density =
            vkr_cloud_eroded_density(shape, detail, sample.height_fraction) *
            vkr_cloud_distance_fade(clouds, distance);
        if (density <= 0.0f)
            continue;
        float3 sun_irradiance = float3(0.0f);
        if (!vkr_atmosphere_sun_occluded(params.atmosphere, position, sun))
        {
            sun_irradiance =
                params.atmosphere.solar.rgb *
                vk_atmosphere_lookup_transmittance(sky.transmittance_texture,
                                                   sky.sampler,
                                                   params.atmosphere, position,
                                                   sun);
        }
        float3 source =
            vkr_cloud_sun_scattering(
                sun_irradiance, cosine,
                vk_cloud_light_depth(sky, params, position, sun)) +
            vkr_cloud_ambient(ambient, sample.height_fraction);
        state = vkr_cloud_integrate_step(state, density * clouds.layer.z,
                                         step_km, source, distance);
        if (state.transmittance < VKR_CLOUD_TRANSMITTANCE_CUTOFF)
            break;
    }

    float3 radiance = state.radiance;
    if (params.aerial.w > 0.0f && state.transmittance < 1.0f)
    {
        float depth_km = vkr_cloud_integration_depth(state, segment.start_km);
        float3 world = params.camera_position.xyz +
                       direction * (depth_km / params.camera_position.w);
        VkrVkAerialSample aerial =
            packet_aerial_sample(sky, sky.aerial_perspective_texture, world);
        radiance = vkr_sky_apply_aerial_over_feedback(
            radiance, float3(state.transmittance), aerial.packed,
            aerial.weight);
    }
    return float4(radiance, state.transmittance);
}

// One thread per half-resolution texel. Composition bilinearly samples texels
// up to one full-resolution pixel beyond each 2x2 footprint, so a texel traces
// when any pixel of its 4x4 neighbourhood shows sky; others write the
// untraced marker.
[shader("compute")]
[numthreads(8, 8, 1)]
void cloud_trace_compute(uint3 id : SV_DispatchThreadID)
{
    VkrVulkanCloudTraceRoot *root = g_cloud_trace_push.root;
    uint2 texel = id.xy;
    if (any(texel >= root.extent))
        return;
    int2 origin = int2(texel) * 2 - 1;
    int2 limit = int2(root.depth_extent) - 1;
    bool sky_visible = false;
    for (int y = 0; y < 4 && !sky_visible; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            int2 pixel = clamp(origin + int2(x, y), int2(0), limit);
            if (g_depth_textures[root.depth_texture].Load(int3(pixel, 0)) >=
                1.0f)
            {
                sky_visible = true;
                break;
            }
        }
    }
    if (!sky_visible)
    {
        g_storage_2d[root.output_texture][texel] =
            float4(0.0f, 0.0f, 0.0f, VKR_CLOUD_UNTRACED_ALPHA);
        return;
    }

    VkrSkyParams params = root.sky.params;
    float2 texel_size = 1.0f / float2(root.extent);
    float2 centre_uv = (float2(texel) + 0.5f) * texel_size;
    float3 direction = vkr_cloud_view_direction(
        params, centre_uv + vkr_cloud_ray_jitter(root.frame_index) * texel_size);
    float4 current = vk_cloud_march(root, direction, texel);

    // Clouds lie kilometres away, so the previous camera saw this texel's
    // centre along the same direction.
    float3 centre_direction = vkr_cloud_view_direction(params, centre_uv);
    float4 previous_clip =
        mul(root.previous_view_projection, float4(centre_direction, 0.0f));
    float2 previous_uv =
        previous_clip.xy / max(previous_clip.w, 1e-6f) * 0.5f + 0.5f;
    bool history_valid = root.history_valid != 0u &&
                         previous_clip.w > 0.0f && all(previous_uv >= 0.0f) &&
                         all(previous_uv <= 1.0f);
    float4 history =
        history_valid
            ? g_textures[root.history_texture].SampleLevel(
                  g_samplers[root.history_sampler], previous_uv, 0.0f)
            : current;
    g_storage_2d[root.output_texture][texel] =
        vkr_cloud_history_blend(current, history, history_valid);
}

struct VkrVkFroxelInjectRoot
{
    VkrVulkanPacketFrameRoot* frame;
    VkrFroxelFogParams* params;
    uint history_texture;
    uint history_sampler;
    uint output_texture;
    uint history_valid;
    uint3 extent;
    uint reserved;
};

struct VkrVkFroxelIntegrateRoot
{
    VkrVulkanPacketFrameRoot* frame;
    VkrFroxelFogParams* params;
    uint scattering_texture;
    uint scattering_sampler;
    uint integrated_texture;
    uint3 extent;
    uint reserved;
};

struct VkrVkFroxelApplyRoot
{
    VkrVulkanPacketFrameRoot* frame;
    VkrFroxelFogParams* params;
    uint depth_texture;
    uint integrated_texture;
    uint integrated_sampler;
    uint target_texture;
    uint2 extent;
    uint2 reserved;
};

struct VkrVkFroxelInjectPush
{
    VkrVkFroxelInjectRoot* root;
    uint2 reserved;
};

struct VkrVkFroxelIntegratePush
{
    VkrVkFroxelIntegrateRoot* root;
    uint2 reserved;
};

struct VkrVkFroxelApplyPush
{
    VkrVkFroxelApplyRoot* root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkFroxelInjectPush> g_froxel_inject_push;
[[vk::push_constant]] ConstantBuffer<VkrVkFroxelIntegratePush>
    g_froxel_integrate_push;
[[vk::push_constant]] ConstantBuffer<VkrVkFroxelApplyPush> g_froxel_apply_push;

float3 vkr_vk_froxel_local_radiance(VkrVulkanPacketFrameRoot* frame,
                                    VkrGpuPointLightRow light,
                                    float3 world_position)
{
    float4 p0 = light.p0;
    float4 p1 = light.p1;
    float4 p2 = light.p2;
    float4 p3 = light.p3;
    uint kind = uint(p2.w + 0.5f);
    float3 to_light = p0.xyz - world_position;
    float distance_squared = dot(to_light, to_light);
    if (kind != 0u && p2.z > 0.0f && distance_squared >= p2.z * p2.z)
        return float3(0.0f);
    float distance = sqrt(distance_squared);
    float3 light_direction = distance > 1e-6f ? to_light / distance : float3(0.0f);
    float attenuation;
    if (kind == 0u)
    {
        attenuation = 1.0f / max(max(p0.w, 1.0f) + p1.w * distance +
                                  p2.y * distance_squared, 1e-6f);
    }
    else
    {
        float range_attenuation = 1.0f;
        if (p2.z > 0.0f)
        {
            float ratio = distance / p2.z;
            range_attenuation = saturate(1.0f - ratio * ratio * ratio * ratio);
            range_attenuation *= range_attenuation;
        }
        attenuation = range_attenuation / max(distance_squared, 1e-4f);
        if (kind == 2u)
        {
            float cone = dot(-light_direction, normalize(p3.xyz));
            attenuation *= smoothstep(p1.w, p0.w, cone);
        }
    }
    float3 visibility = packet_local_shadow_one_tap(frame, light, kind,
                                                    world_position);
    return max(p1.rgb * p2.x * attenuation * visibility, float3(0.0f));
}

float3 vkr_vk_froxel_incident_radiance(VkrVulkanPacketFrameRoot* frame,
                                       VkrFroxelFogParams params,
                                       float3 world_position)
{
    float3 incident = float3(0.0f);
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        float visibility = packet_directional_shadow_one_tap(frame,
                                                              world_position);
        incident += frame.directional_color_intensity.rgb *
                    frame.directional_color_intensity.w * visibility;
    }
    uint count = params.selected_local_indices_count.z;
    for (uint slot = 0u; slot < count; ++slot)
    {
        uint source = params.selected_local_indices_count[slot];
        incident += vkr_vk_froxel_local_radiance(
            frame, frame.point_light_data[source], world_position);
    }
    return incident;
}

float3 vkr_vk_froxel_world_position(float4x4 inverse_view_projection,
                                    uint2 pixel, uint2 extent,
                                    float device_depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / float2(extent) * 2.0f - 1.0f;
    return vkr_froxel_homogeneous(
        mul(inverse_view_projection, float4(ndc, device_depth, 1.0f)));
}

VkrFroxelSample vkr_vk_froxel_sample_integrated(
    uint texture_index, uint sampler_index, VkrFroxelFogParams params,
    float4x4 view, float3 world_position)
{
    VkrFroxelSample identity = {float3(0.0f), 1.0f};
    VkrFroxelHistoryCoordinate coordinate =
        vkr_froxel_integrated_coordinate(params, world_position);
    if (coordinate.valid == 0u)
        return identity;
    float positive_depth = -mul(view, float4(world_position, 1.0f)).z;
    VkrFroxelIntegratedCoordinate z = vkr_froxel_integrated_sample_coordinate(
        params, positive_depth);
    float4 packed = g_3d_textures[texture_index].SampleLevel(
        g_samplers[sampler_index], float3(coordinate.uvw.xy, z.z), 0.0f);
    VkrFroxelSample result = {packed.rgb, saturate(packed.w)};
    if (z.near_identity != 0u)
    {
        result.inscatter *= z.near_blend;
        result.transmittance = lerp(1.0f, result.transmittance, z.near_blend);
    }
    return result;
}

[shader("compute")]
[numthreads(8, 8, 4)]
void froxel_inject_compute(uint3 cell : SV_DispatchThreadID)
{
    VkrVkFroxelInjectRoot* root = g_froxel_inject_push.root;
    VkrFroxelFogParams params = root.params[0];
    if (any(cell >= root.extent) || !vkr_froxel_enabled(params))
        return;
    float3 world = vkr_froxel_world_center(params, root.frame.view, cell);
    float extinction = vkr_froxel_height_density(params, world);
    float3 source = vkr_froxel_local_source(
        extinction, vkr_vk_froxel_incident_radiance(root.frame, params, world),
        params);
    float4 current = float4(source, extinction);
    VkrFroxelHistoryCoordinate history_coordinate =
        vkr_froxel_history_coordinate(params, world);
    uint valid = root.history_valid != 0u && history_coordinate.valid != 0u ? 1u : 0u;
    float4 history = valid != 0u
        ? g_3d_textures[root.history_texture].SampleLevel(
              g_samplers[root.history_sampler], history_coordinate.uvw, 0.0f)
        : float4(0.0f);
    g_storage_3d[root.output_texture][cell] =
        vkr_froxel_temporal_filter(params, current, history, valid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void froxel_integrate_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkFroxelIntegrateRoot* root = g_froxel_integrate_push.root;
    VkrFroxelFogParams params = root.params[0];
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.extent.xy) || !vkr_froxel_enabled(params))
        return;
    float2 uv = (float2(pixel) + 0.5f) / float2(root.extent.xy);
    float2 ndc = uv * 2.0f - 1.0f;
    float3 near_world = vkr_froxel_homogeneous(
        mul(params.inverse_view_projection, float4(ndc, 0.0f, 1.0f)));
    float3 far_world = vkr_froxel_homogeneous(
        mul(params.inverse_view_projection, float4(ndc, 1.0f, 1.0f)));
    float ray_scale = vkr_froxel_ray_length_scale(root.frame.view, near_world,
                                                  far_world);
    VkrFroxelSample accumulated = {float3(0.0f), 1.0f};
    for (uint z = 0u; z < root.extent.z; ++z)
    {
        VkrFroxelSliceInterval interval = vkr_froxel_slice_interval(params, z);
        float4 local = g_3d_textures[root.scattering_texture].Load(
            int4(pixel, z, 0));
        accumulated = vkr_froxel_integrate_step(
            accumulated, local.rgb, local.w,
            (interval.far_distance - interval.near_distance) * ray_scale);
        g_storage_3d[root.integrated_texture][uint3(pixel, z)] =
            float4(accumulated.inscatter, accumulated.transmittance);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void froxel_apply_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkFroxelApplyRoot* root = g_froxel_apply_push.root;
    VkrFroxelFogParams params = root.params[0];
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.extent) || !vkr_froxel_enabled(params))
        return;
    float4 current = g_storage_2d[root.target_texture][pixel];
    float device_depth = g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    float3 world;
    if (device_depth >= 1.0f)
    {
        float2 ndc = (float2(pixel) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
        world = vkr_froxel_world_at_view_depth(
            params, root.frame.view, ndc, params.height_distance_phase.z);
    }
    else
    {
        world = vkr_vk_froxel_world_position(params.inverse_raster_view_projection,
                                              pixel, root.extent, device_depth);
    }
    VkrFroxelSample sample = vkr_vk_froxel_sample_integrated(
        root.integrated_texture, root.integrated_sampler, params,
        root.frame.view, world);
    g_storage_2d[root.target_texture][pixel] =
        float4(vkr_froxel_apply_integrated(current.rgb, sample).inscatter,
               current.a);
}

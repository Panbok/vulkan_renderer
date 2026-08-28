[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_equirect(uint3 position : SV_DispatchThreadID)
{
    VkrVulkanIblRoot* root = (VkrVulkanIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 face_uv =
        (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 direction = normalize(cube_direction(position.z, face_uv));
    float2 environment_uv = float2(
        atan2(direction.z, direction.x) * (0.5f / vkr_pi) + 0.5f,
        acos(clamp(direction.y, -1.0f, 1.0f)) / vkr_pi);
    // IBL-root descriptor indices are dynamically uniform; see
    // packet_directional_shadow.
    g_storage_images[root.target_texture][position] =
        g_textures[root.source_texture].SampleLevel(
            g_samplers[root.source_sampler], environment_uv, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_irradiance(uint3 position : SV_DispatchThreadID)
{
    static const uint sample_count = 128u;
    VkrVulkanIblRoot* root = (VkrVulkanIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 uv = (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 normal = normalize(cube_direction(position.z, uv));
    float3 irradiance = 0.0f;
    for (uint i = 0u; i < sample_count; ++i)
    {
        float2 xi = hammersley(i, sample_count);
        float phi = 2.0f * vkr_pi * xi.x;
        float cos_theta = sqrt(1.0f - xi.y);
        float sin_theta = sqrt(xi.y);
        float3 direction = tangent_sample(
            normal, float3(cos(phi) * sin_theta, sin(phi) * sin_theta,
                           cos_theta));
        irradiance += g_cube_textures[root.source_texture].SampleLevel(
            g_samplers[root.source_sampler], direction, 0.0f).rgb;
    }
    g_storage_images[root.target_texture][position] =
        float4(irradiance / float(sample_count), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_prefilter(uint3 position : SV_DispatchThreadID)
{
    static const uint sample_count = 256u;
    VkrVulkanIblRoot* root = (VkrVulkanIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 uv = (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 normal = normalize(cube_direction(position.z, uv));
    float3 view = normal;
    float3 color = 0.0f;
    float total_weight = 0.0f;
    for (uint i = 0u; i < sample_count; ++i)
    {
        float3 half_vector = importance_ggx(hammersley(i, sample_count), normal,
                                            root.roughness);
        float3 light = normalize(2.0f * dot(view, half_vector) * half_vector -
                                 view);
        float no_l = max(dot(normal, light), 0.0f);
        if (no_l <= 0.0f)
            continue;
        float no_h = max(dot(normal, half_vector), 1e-6f);
        float vo_h = max(dot(view, half_vector), 1e-6f);
        float pdf = distribution_ggx(no_h, root.roughness) * no_h /
                    max(4.0f * vo_h, 1e-6f);
        float sample_solid_angle =
            1.0f / (float(sample_count) * max(pdf, 1e-6f));
        float texel_solid_angle = 4.0f * vkr_pi /
            max(6.0f * float(root.source_face_size * root.source_face_size),
                1.0f);
        float lod = root.roughness <= 0.001f
                        ? 0.0f
                        : 0.5f * log2(4.0f * sample_solid_angle /
                                      texel_solid_angle);
        lod = clamp(lod, 0.0f, float(max(root.source_mip_count, 1u) - 1u));
        color += g_cube_textures[root.source_texture].SampleLevel(
            g_samplers[root.source_sampler], light, lod).rgb * no_l;
        total_weight += no_l;
    }
    g_storage_images[root.target_texture][position] =
        float4(color / max(total_weight, 1e-4f), 1.0f);
}

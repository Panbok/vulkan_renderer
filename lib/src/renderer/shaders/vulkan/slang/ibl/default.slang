[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_equirect(uint3 position : SV_DispatchThreadID)
{
    VkrBindlessIblRoot* root = (VkrBindlessIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 face_uv =
        (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 direction = normalize(cube_direction(position.z, face_uv));
    float2 environment_uv = float2(
        atan2(direction.z, direction.x) * (0.5f / vkr_pi) + 0.5f,
        acos(clamp(direction.y, -1.0f, 1.0f)) / vkr_pi);
    uint source = NonUniformResourceIndex(root.source_texture);
    uint sampler_index = NonUniformResourceIndex(root.source_sampler);
    uint target = NonUniformResourceIndex(root.target_texture);
    g_storage_images[target][position] = g_textures[source].SampleLevel(
        g_samplers[sampler_index], environment_uv, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_irradiance(uint3 position : SV_DispatchThreadID)
{
    VkrBindlessIblRoot* root = (VkrBindlessIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 uv = (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 normal = normalize(cube_direction(position.z, uv));
    float3 irradiance = 0.0f;
    uint count = max(root.sample_count, 1u);
    uint source = NonUniformResourceIndex(root.source_texture);
    uint sampler_index = NonUniformResourceIndex(root.source_sampler);
    for (uint i = 0u; i < count; ++i)
    {
        float2 xi = hammersley(i, count);
        float phi = 2.0f * vkr_pi * xi.x;
        float cos_theta = sqrt(1.0f - xi.y);
        float sin_theta = sqrt(xi.y);
        float3 direction = tangent_sample(
            normal, float3(cos(phi) * sin_theta, sin(phi) * sin_theta,
                           cos_theta));
        irradiance += g_cube_textures[source].SampleLevel(
            g_samplers[sampler_index], direction, 0.0f).rgb;
    }
    uint target = NonUniformResourceIndex(root.target_texture);
    g_storage_images[target][position] =
        float4(irradiance / float(count), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_prefilter(uint3 position : SV_DispatchThreadID)
{
    VkrBindlessIblRoot* root = (VkrBindlessIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 uv = (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 normal = normalize(cube_direction(position.z, uv));
    float3 view = normal;
    float3 color = 0.0f;
    float total_weight = 0.0f;
    uint count = max(root.sample_count, 1u);
    uint source = NonUniformResourceIndex(root.source_texture);
    uint sampler_index = NonUniformResourceIndex(root.source_sampler);
    for (uint i = 0u; i < count; ++i)
    {
        float3 half_vector = importance_ggx(hammersley(i, count), normal,
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
        float sample_solid_angle = 1.0f / (float(count) * max(pdf, 1e-6f));
        float texel_solid_angle = 4.0f * vkr_pi /
            max(6.0f * float(root.source_face_size * root.source_face_size),
                1.0f);
        float lod = root.roughness <= 0.001f
                        ? 0.0f
                        : 0.5f * log2(4.0f * sample_solid_angle /
                                      texel_solid_angle);
        lod = clamp(lod, 0.0f, float(max(root.source_mip_count, 1u) - 1u));
        color += g_cube_textures[source].SampleLevel(
            g_samplers[sampler_index], light, lod).rgb * no_l;
        total_weight += no_l;
    }
    uint target = NonUniformResourceIndex(root.target_texture);
    g_storage_images[target][position] =
        float4(color / max(total_weight, 1e-4f), 1.0f);
}

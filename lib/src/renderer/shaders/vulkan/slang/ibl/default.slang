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

// One workgroup per destination slot. A fixed lane assignment and a fixed
// reduction order make repetitions bit-deterministic without floating-point
// atomics; the bounded source mip limits the work, but determinism comes from
// the summation order, not the mip.
#define VKR_SH_PROJECT_LANES 64u

groupshared float3 g_sh_partial[VKR_SH_PROJECT_LANES][9];
groupshared float g_sh_partial_weight[VKR_SH_PROJECT_LANES];

[shader("compute")]
[numthreads(64, 1, 1)]
void ibl_sh(uint3 group_thread : SV_GroupThreadID)
{
    VkrVulkanIblShRoot* root = (VkrVulkanIblShRoot*)g_push.root;
    uint lane = group_thread.x;
    uint extent = max(root.source_face_size, 1u);
    uint face_texels = extent * extent;
    uint texel_count = face_texels * 6u;
    float texel_step = 2.0f / float(extent);

    float3 accumulated[9];
    for (uint i = 0u; i < 9u; ++i)
        accumulated[i] = float3(0.0f, 0.0f, 0.0f);
    float accumulated_weight = 0.0f;

    // Strided so every lane walks the same number of texels in the same order
    // regardless of how the domain divides.
    for (uint index = lane; index < texel_count; index += VKR_SH_PROJECT_LANES)
    {
        uint face = index / face_texels;
        uint local = index - face * face_texels;
        uint y = local / extent;
        uint x = local - y * extent;

        float s0 = float(x) * texel_step - 1.0f;
        float t0 = float(y) * texel_step - 1.0f;
        float solid_angle = vkr_sh_cube_texel_solid_angle(
            s0, s0 + texel_step, t0, t0 + texel_step);

        float2 uv = (float2(float(x), float(y)) + 0.5f) / float(extent);
        float3 direction = normalize(cube_direction(face, uv));
        // Exact texel load through the selected mip's storage view: a sampled
        // fetch would filter and would not reproduce the CPU reference.
        float3 radiance =
            g_storage_images[root.source_storage_texture][uint3(x, y, face)]
                .rgb;

        VkrShL2Basis basis = vkr_sh_l2_basis(direction);
        accumulated_weight += solid_angle;
        for (uint i = 0u; i < 9u; ++i)
            accumulated[i] += radiance * (basis.y[i] * solid_angle);
    }

    for (uint i = 0u; i < 9u; ++i)
        g_sh_partial[lane][i] = accumulated[i];
    g_sh_partial_weight[lane] = accumulated_weight;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = VKR_SH_PROJECT_LANES / 2u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
        {
            for (uint i = 0u; i < 9u; ++i)
                g_sh_partial[lane][i] += g_sh_partial[lane + stride][i];
            g_sh_partial_weight[lane] += g_sh_partial_weight[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane != 0u)
        return;

    // Correct the accumulated weight back to the exact sphere measure so a
    // partially cancelled texel sum cannot bias every coefficient.
    float total_weight = g_sh_partial_weight[0];
    float normalization =
        total_weight > 0.0f ? (4.0f * vkr_pi) / total_weight : 0.0f;

    float3 coefficients[9];
    for (uint i = 0u; i < 9u; ++i)
        coefficients[i] = g_sh_partial[0][i] * normalization;

    // Normalized clamped-cosine transfer (pi, 2pi/3, pi/4 divided by pi) times
    // the authored deringing window, folded in at projection time so the
    // per-pixel path stays fixed.
    float band[3];
    band[0] = 1.0f * root.window_band_0;
    band[1] = (2.0f / 3.0f) * root.window_band_1;
    band[2] = 0.25f * root.window_band_2;
    coefficients[0] *= band[0];
    for (uint i = 1u; i < 4u; ++i)
        coefficients[i] *= band[1];
    for (uint i = 4u; i < 9u; ++i)
        coefficients[i] *= band[2];

    VkrShL2Packed* destination = (VkrShL2Packed*)root.destination;
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        destination.v[channel] = float4(
            VKR_SH_K1 * coefficients[3][channel],
            VKR_SH_K1 * coefficients[1][channel],
            VKR_SH_K1 * coefficients[2][channel],
            VKR_SH_K0 * coefficients[0][channel] -
                VKR_SH_K3 * coefficients[6][channel]);
        destination.v[3u + channel] = float4(
            VKR_SH_K2 * coefficients[4][channel],
            VKR_SH_K2 * coefficients[5][channel],
            3.0f * VKR_SH_K3 * coefficients[6][channel],
            VKR_SH_K2 * coefficients[7][channel]);
    }
    destination.v[6] = float4(VKR_SH_K4 * coefficients[8][0],
                              VKR_SH_K4 * coefficients[8][1],
                              VKR_SH_K4 * coefficients[8][2], 0.0f);
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

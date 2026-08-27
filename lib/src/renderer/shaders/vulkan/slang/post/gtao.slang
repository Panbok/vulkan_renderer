struct VkrVkGtaoRoot
{
    VkrGtaoParams params;
    uint source_texture;
    uint vbuffer_texture;
    uint normal_texture;
    uint destination_texture;
    uint edges_texture;
    uint point_sampler;
    uint2 source_extent;
    uint2 destination_extent;
    uint2 reserved;
};

struct VkrVkGtaoPush
{
    VkrVkGtaoRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkGtaoPush> g_gtao_push;

float2 vkr_vk_gtao_uv(float2 pixel, uint2 extent)
{
    return (pixel + 0.5f) / float2(extent);
}

float vkr_vk_gtao_sample(VkrVkGtaoRoot *root, uint texture_index, float2 uv,
                         float mip)
{
    return g_textures[NonUniformResourceIndex(texture_index)]
        .SampleLevel(g_samplers[NonUniformResourceIndex(root.point_sampler)],
                     uv, mip)
        .r;
}

void vkr_vk_gtao_store(VkrVkGtaoRoot *root, uint texture_index, uint2 pixel,
                       float value)
{
    g_storage_2d[NonUniformResourceIndex(texture_index)][pixel] =
        float4(value, 0.0f, 0.0f, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_gtao_depth_prefilter(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkGtaoRoot *root = g_gtao_push.root;
    if (any(thread_id.xy >= root.destination_extent))
        return;
    float2 uv = vkr_vk_gtao_uv(float2(thread_id.xy), root.destination_extent);
    float device_depth =
        g_depth_textures[root.source_texture]
            .SampleLevel(g_samplers[root.point_sampler], uv, 0.0f);
    vkr_vk_gtao_store(root, root.destination_texture, thread_id.xy,
                      vkr_gtao_linearize_depth(root.params, device_depth));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_gtao_depth_mip(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkGtaoRoot *root = g_gtao_push.root;
    if (any(thread_id.xy >= root.destination_extent))
        return;
    float2 source_pixel = float2(thread_id.xy * 2u);
    float d0 = vkr_vk_gtao_sample(
        root, root.source_texture,
        vkr_vk_gtao_uv(source_pixel + float2(0.0f, 0.0f), root.source_extent),
        0.0f);
    float d1 = vkr_vk_gtao_sample(
        root, root.source_texture,
        vkr_vk_gtao_uv(source_pixel + float2(1.0f, 0.0f), root.source_extent),
        0.0f);
    float d2 = vkr_vk_gtao_sample(
        root, root.source_texture,
        vkr_vk_gtao_uv(source_pixel + float2(0.0f, 1.0f), root.source_extent),
        0.0f);
    float d3 = vkr_vk_gtao_sample(
        root, root.source_texture,
        vkr_vk_gtao_uv(source_pixel + float2(1.0f, 1.0f), root.source_extent),
        0.0f);
    vkr_vk_gtao_store(root, root.destination_texture, thread_id.xy,
                      vkr_gtao_depth_mip_filter(root.params, d0, d1, d2, d3));
}

float vkr_vk_gtao_view_depth(VkrVkGtaoRoot *root, float2 uv, float mip)
{
    return vkr_vk_gtao_sample(root, root.source_texture, uv, mip);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_gtao_evaluate(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkGtaoRoot *root = g_gtao_push.root;
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.destination_extent))
        return;

    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][pixel];
    if (encoded.x == 0u)
    {
        vkr_vk_gtao_store(root, root.destination_texture, pixel, 1.0f);
        vkr_vk_gtao_store(root, root.edges_texture, pixel,
                          vkr_gtao_pack_edges(float4(1.0f)));
        return;
    }

    float2 uv =
        (float2(pixel) + 0.5f) *
        float2(root.params.viewport_pixel_size_x,
               root.params.viewport_pixel_size_y);
    float2 texel = float2(root.params.viewport_pixel_size_x,
                          root.params.viewport_pixel_size_y);
    float center_depth = vkr_vk_gtao_view_depth(root, uv, 0.0f);
    float left_depth =
        vkr_vk_gtao_view_depth(root, uv - float2(texel.x, 0.0f), 0.0f);
    float right_depth =
        vkr_vk_gtao_view_depth(root, uv + float2(texel.x, 0.0f), 0.0f);
    float top_depth =
        vkr_vk_gtao_view_depth(root, uv - float2(0.0f, texel.y), 0.0f);
    float bottom_depth =
        vkr_vk_gtao_view_depth(root, uv + float2(0.0f, texel.y), 0.0f);
    float4 edges = vkr_gtao_calculate_edges(
        center_depth, left_depth, right_depth, top_depth, bottom_depth);
    vkr_vk_gtao_store(root, root.edges_texture, pixel,
                      vkr_gtao_pack_edges(edges));

    float3 world_normal = vkr_vk_octahedral_decode(
        g_storage_float2_2d[root.normal_texture][pixel]);
    float3 view_normal = normalize(
        mul(root.params.view, float4(world_normal, 0.0f)).xyz);
    float3 center_position =
        vkr_gtao_view_position(root.params, uv, center_depth);
    float3 view_direction = normalize(-center_position);
    float pixel_view_size =
        vkr_gtao_pixel_view_size(root.params, uv, center_depth);
    float projected_radius =
        root.params.effect_radius * root.params.radius_multiplier /
        pixel_view_size;
    float2 noise = vkr_gtao_spatiotemporal_noise(pixel,
                                                  root.params.noise_index);
    float visibility = 0.0f;

    for (uint slice = 0u; slice < root.params.slice_count; ++slice)
    {
        float phi =
            (float(slice) + noise.x) * VKR_GTAO_PI /
            float(root.params.slice_count);
        float2 slice_direction = float2(cos(phi), sin(phi));
        float3 view_slice_direction = float3(slice_direction, 0.0f);
        float3 slice_orthogonal =
            normalize(cross(view_slice_direction, view_direction));
        float3 projected_normal =
            view_normal - slice_orthogonal *
                              dot(view_normal, slice_orthogonal);
        float projected_normal_length = max(length(projected_normal), 1e-6f);
        float cos_normal = clamp(
            dot(projected_normal, view_direction) / projected_normal_length,
            0.0f, 1.0f);
        float normal_sign =
            dot(view_slice_direction, projected_normal) >= 0.0f ? 1.0f
                                                                : -1.0f;
        float normal_angle = normal_sign * acos(cos_normal);
        float low_horizon_cos0 = cos(normal_angle + VKR_GTAO_HALF_PI);
        float low_horizon_cos1 = cos(normal_angle - VKR_GTAO_HALF_PI);
        float horizon_cos0 = low_horizon_cos0;
        float horizon_cos1 = low_horizon_cos1;

        for (uint step = 0u; step < root.params.steps_per_slice; ++step)
        {
            float sample_distance =
                max(vkr_gtao_sample_distance(root.params, float(step), noise.y) *
                        projected_radius,
                    1.0f);
            float mip = clamp(log2(sample_distance) -
                                  root.params.depth_mip_sampling_offset,
                              0.0f,
                              float(max(root.params.depth_mip_count, 1u) - 1u));
            [unroll]
            for (uint side = 0u; side < 2u; ++side)
            {
                float side_sign = side == 0u ? -1.0f : 1.0f;
                float2 sample_uv = clamp(
                    uv + slice_direction * sample_distance * texel * side_sign,
                    texel * 0.5f, 1.0f - texel * 0.5f);
                float sample_depth =
                    vkr_vk_gtao_view_depth(root, sample_uv, mip);
                float3 sample_position =
                    vkr_gtao_view_position(root.params, sample_uv,
                                           sample_depth);
                float3 horizon_vector = sample_position - center_position;
                float horizon_distance = max(length(horizon_vector), 1e-6f);
                float sample_horizon_cos =
                    dot(horizon_vector / horizon_distance, view_direction);
                float falloff =
                    vkr_gtao_falloff_weight(root.params, horizon_distance);
                if (side == 0u)
                    horizon_cos0 = max(
                        horizon_cos0,
                        lerp(low_horizon_cos0, sample_horizon_cos, falloff));
                else
                    horizon_cos1 = max(
                        horizon_cos1,
                        lerp(low_horizon_cos1, sample_horizon_cos, falloff));
            }
        }
        visibility += vkr_gtao_integrate_slice(
            projected_normal_length, cos_normal, normal_angle, horizon_cos0,
            horizon_cos1);
    }

    vkr_vk_gtao_store(root, root.destination_texture, pixel,
                      vkr_gtao_finalize_visibility(root.params, visibility));
}

float vkr_vk_gtao_denoise_sample(VkrVkGtaoRoot *root, uint texture_index,
                                 float2 pixel)
{
    return vkr_vk_gtao_sample(
        root, texture_index,
        vkr_vk_gtao_uv(pixel, root.destination_extent), 0.0f);
}

float vkr_vk_gtao_edge_weight(float edge0, float edge1, float beta)
{
    return pow(saturate(edge0 * edge1), beta);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_gtao_denoise(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkGtaoRoot *root = g_gtao_push.root;
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.destination_extent))
        return;

    float2 p = float2(pixel);
    float beta = root.params.denoise_blur_beta;
    float4 center_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture, p));
    float4 left_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(-1.0f, 0.0f)));
    float4 right_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(1.0f, 0.0f)));
    float4 top_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(0.0f, -1.0f)));
    float4 bottom_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(0.0f, 1.0f)));
    float4 top_left_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(-1.0f, -1.0f)));
    float4 top_right_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(1.0f, -1.0f)));
    float4 bottom_left_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(-1.0f, 1.0f)));
    float4 bottom_right_edges = vkr_gtao_unpack_edges(
        vkr_vk_gtao_denoise_sample(root, root.edges_texture,
                                   p + float2(1.0f, 1.0f)));

    float weights[9];
    weights[0] = pow(saturate(vkr_gtao_diagonal_edge_weight(
                         center_edges.x, top_left_edges.y, center_edges.z,
                         top_left_edges.w)),
                     beta);
    weights[1] = vkr_vk_gtao_edge_weight(center_edges.z, top_edges.w, beta);
    weights[2] = pow(saturate(vkr_gtao_diagonal_edge_weight(
                         center_edges.y, top_right_edges.x, center_edges.z,
                         top_right_edges.w)),
                     beta);
    weights[3] = vkr_vk_gtao_edge_weight(center_edges.x, left_edges.y, beta);
    weights[4] = 1.0f;
    weights[5] = vkr_vk_gtao_edge_weight(center_edges.y, right_edges.x, beta);
    weights[6] = pow(saturate(vkr_gtao_diagonal_edge_weight(
                         center_edges.x, bottom_left_edges.y, center_edges.w,
                         bottom_left_edges.z)),
                     beta);
    weights[7] =
        vkr_vk_gtao_edge_weight(center_edges.w, bottom_edges.z, beta);
    weights[8] = pow(saturate(vkr_gtao_diagonal_edge_weight(
                         center_edges.y, bottom_right_edges.x, center_edges.w,
                         bottom_right_edges.z)),
                     beta);

    float visibility = 0.0f;
    float weight_sum = 0.0f;
    uint index = 0u;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            visibility += weights[index] * vkr_vk_gtao_denoise_sample(
                                               root, root.source_texture,
                                               p + float2(float(x), float(y)));
            weight_sum += weights[index];
            ++index;
        }
    }
    vkr_vk_gtao_store(root, root.destination_texture, pixel,
                      visibility / max(weight_sum, 1e-6f));
}

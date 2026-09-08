// Vulkan SSGI compute passes. Shared traversal/temporal math lives in
// ssgi_kernel.slangh after ssr_kernel.slangh; this file owns descriptor access.

struct VkrVkSsgiDepthBaseRoot
{
    VkrSsgiParams params;
    uint depth_texture;
    uint vbuffer_texture;
    uint destination_depth_texture;
    uint reserved;
};

struct VkrVkSsgiDepthMipRoot
{
    uint source_depth_texture;
    uint destination_depth_texture;
    uint2 source_extent;
    uint2 destination_extent;
    uint2 reserved;
};

struct VkrVkSsgiTraceRoot
{
    VkrSsgiParams params;
    uint depth_texture;
    uint vbuffer_texture;
    uint normal_texture;
    uint albedo_texture;
    uint depth_pyramid_texture;
    uint direct_source_texture;
    uint destination_texture;
    uint reserved;
};

struct VkrVkSsgiTemporalRoot
{
    VkrSsgiParams params;
    VkrGpuVisibleDrawRow* visible_rows;
    VkrVulkanInstance* instances;
    uint raw_texture;
    uint vbuffer_texture;
    uint depth_texture;
    uint normal_texture;
    uint motion_texture;
    uint validity_texture;
    uint history_color_texture;
    uint history_depth_texture;
    uint history_identity_texture;
    uint output_color_texture;
    uint output_depth_texture;
    uint output_identity_texture;
    uint linear_sampler;
    uint3 reserved;
};

struct VkrVkSsgiCompositeRoot
{
    VkrSsgiParams params;
    VkrVulkanPacketFrameRoot* frame;
    uint2 frame_address_padding;
    float4x4 inverse_view_projection;
    uint scene_texture;
    uint reflection_texture;
    uint vbuffer_texture;
    uint depth_texture;
    uint albedo_texture;
    uint normal_texture;
    uint history_depth_texture;
    uint specular_texture;
    uint linear_sampler;
    uint clearcoat_texture;
    uint sheen_texture;
    uint anisotropy_texture;
    VkrGpuVisibleDrawRow* visible_rows;
    uint subsurface_source_texture;
    uint subsurface_profile_count;
};

struct VkrVkSsgiDepthBasePush { VkrVkSsgiDepthBaseRoot* root; };
struct VkrVkSsgiDepthMipPush { VkrVkSsgiDepthMipRoot* root; };
struct VkrVkSsgiTracePush { VkrVkSsgiTraceRoot* root; };
struct VkrVkSsgiTemporalPush { VkrVkSsgiTemporalRoot* root; };
struct VkrVkSsgiCompositePush { VkrVkSsgiCompositeRoot* root; };

[[vk::push_constant]] VkrVkSsgiDepthBasePush g_ssgi_depth_base_push;
[[vk::push_constant]] VkrVkSsgiDepthMipPush g_ssgi_depth_mip_push;
[[vk::push_constant]] VkrVkSsgiTracePush g_ssgi_trace_push;
[[vk::push_constant]] VkrVkSsgiTemporalPush g_ssgi_temporal_push;
[[vk::push_constant]] VkrVkSsgiCompositePush g_ssgi_composite_push;

float vkr_vk_ssgi_positive_depth(VkrSsrParams screen, uint2 pixel,
                                  float device_depth, uint2 extent)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(extent);
    float3 view = vkr_ssr_reconstruct_view_position(screen, uv, device_depth);
    return vkr_ssr_valid_depth(-view.z) ? -view.z : 0.0f;
}

VkrSsgiReceiver vkr_vk_ssgi_receiver(VkrSsrParams screen, uint2 trace_pixel,
                                      uint2 trace_extent, uint2 source_extent,
                                      uint vbuffer_texture, uint depth_texture)
{
    VkrSsgiReceiver receiver = vkr_ssgi_empty_receiver();
    uint begin_x = vkr_ssr_reduction_child_begin(trace_pixel.x);
    uint begin_y = vkr_ssr_reduction_child_begin(trace_pixel.y);
    uint end_x = vkr_ssr_reduction_child_end(trace_pixel.x, trace_extent.x,
                                              source_extent.x);
    uint end_y = vkr_ssr_reduction_child_end(trace_pixel.y, trace_extent.y,
                                              source_extent.y);
    for (uint y = begin_y; y < end_y; ++y)
        for (uint x = begin_x; x < end_x; ++x)
        {
            uint2 child = uint2(x, y);
            uint covered = g_storage_uint2_2d[vbuffer_texture][child].x != 0u ? 1u : 0u;
            float depth = covered != 0u
                ? vkr_vk_ssgi_positive_depth(screen, child,
                    g_depth_textures[depth_texture].Load(int3(child, 0)), source_extent)
                : 0.0f;
            receiver = vkr_ssgi_consider_receiver(receiver, child, depth, covered);
        }
    return receiver;
}

uint2 vkr_vk_ssgi_identity(VkrVkSsgiTemporalRoot* root, uint2 receiver_pixel)
{
    uint encoded = g_storage_uint2_2d[root.vbuffer_texture][receiver_pixel].x;
    if (encoded == 0u)
        return uint2(0u);
    VkrGpuVisibleDrawRow visible = root.visible_rows[encoded - 1u];
    VkrVulkanInstance instance = root.instances[visible.instance_index];
    return uint2(instance.temporal_index + 1u, instance.temporal_generation);
}

float3 vkr_vk_ssgi_view_normal(VkrSsgiParams params, uint normal_texture,
                               uint2 pixel, float3 origin)
{
    float3 world_normal = vkr_vk_octahedral_decode(
        g_storage_float2_2d[normal_texture][pixel]);
    float3 view_normal = normalize(mul(params.view, float4(world_normal, 0.0f)).xyz);
    return vkr_ssgi_faceforward_view_normal(view_normal, origin);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssgi_depth_base_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsgiDepthBaseRoot* root = g_ssgi_depth_base_push.root;
    uint2 pixel = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    if (any(pixel >= trace_extent))
        return;
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
    uint begin_x = vkr_ssr_reduction_child_begin(pixel.x);
    uint begin_y = vkr_ssr_reduction_child_begin(pixel.y);
    uint end_x = vkr_ssr_reduction_child_end(pixel.x, trace_extent.x, source_extent.x);
    uint end_y = vkr_ssr_reduction_child_end(pixel.y, trace_extent.y, source_extent.y);
    float values[9] = {0.0f};
    uint count = 0u;
    for (uint y = begin_y; y < end_y; ++y)
        for (uint x = begin_x; x < end_x; ++x)
        {
            uint2 child = uint2(x, y);
            if (g_storage_uint2_2d[root.vbuffer_texture][child].x != 0u)
                values[count] = vkr_vk_ssgi_positive_depth(
                    screen, child, g_depth_textures[root.depth_texture].Load(int3(child, 0)),
                    source_extent);
            ++count;
        }
    g_storage_2d[root.destination_depth_texture][pixel] =
        float4(vkr_ssr_depth_reduce(values[0], values[1], values[2], values[3],
                                    values[4], values[5], values[6], values[7],
                                    values[8], count), 0.0f, 0.0f, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssgi_depth_mip_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsgiDepthMipRoot* root = g_ssgi_depth_mip_push.root;
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.destination_extent))
        return;
    uint begin_x = vkr_ssr_reduction_child_begin(pixel.x);
    uint begin_y = vkr_ssr_reduction_child_begin(pixel.y);
    uint end_x = vkr_ssr_reduction_child_end(pixel.x, root.destination_extent.x,
                                              root.source_extent.x);
    uint end_y = vkr_ssr_reduction_child_end(pixel.y, root.destination_extent.y,
                                              root.source_extent.y);
    float values[9] = {0.0f};
    uint count = 0u;
    for (uint y = begin_y; y < end_y; ++y)
        for (uint x = begin_x; x < end_x; ++x)
            values[count++] = g_textures[root.source_depth_texture].Load(int3(x, y, 0)).x;
    g_storage_2d[root.destination_depth_texture][pixel] =
        float4(vkr_ssr_depth_reduce(values[0], values[1], values[2], values[3],
                                    values[4], values[5], values[6], values[7],
                                    values[8], count), 0.0f, 0.0f, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssgi_trace_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsgiTraceRoot* root = g_ssgi_trace_push.root;
    uint2 pixel = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    if (any(pixel >= trace_extent))
        return;
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
    VkrSsgiReceiver receiver = vkr_vk_ssgi_receiver(
        screen, pixel, trace_extent, source_extent, root.vbuffer_texture, root.depth_texture);
    if (receiver.valid == 0u ||
        saturate(g_textures[root.albedo_texture].Load(int3(receiver.pixel, 0)).a) <= 0.0f)
    {
        g_storage_2d[root.destination_texture][pixel] = float4(0.0f);
        return;
    }
    float device_depth = g_depth_textures[root.depth_texture].Load(int3(receiver.pixel, 0));
    float3 origin = vkr_ssr_reconstruct_view_position(
        screen, (float2(receiver.pixel) + 0.5f) / float2(source_extent), device_depth);
    float3 normal = vkr_vk_ssgi_view_normal(root.params, root.normal_texture,
                                             receiver.pixel, origin);
    float3 direction = vkr_ssgi_cosine_direction(
        vkr_ssgi_sample_2d(receiver.pixel, root.params.sample_phase), normal);
    VkrSsrTrace trace = vkr_ssr_trace_begin(screen,
        origin + normal * root.params.normal_bias, direction,
        max(root.params.depth_mip_count, 1u) - 1u);
    float3 estimate = 0.0f;
    float confidence = 1.0f; // A screen miss is a valid zero Monte-Carlo sample.
    for (uint step = 0u; step < root.params.max_steps && trace.active != 0u; ++step)
    {
        uint2 mip_extent = max(trace_extent >> trace.mip, uint2(1u));
        float cell_exit = vkr_ssr_trace_cell_exit(trace, mip_extent, screen);
        float2 trace_uv = vkr_ssr_trace_uv(trace);
        uint2 source_pixel = min(uint2(floor(trace_uv * float2(source_extent))), source_extent - 1u);
        uint2 cell = min(source_pixel >> (trace.mip + 1u), mip_extent - 1u);
        VkrSsrTraceResolve resolve = vkr_ssr_trace_resolve(
            trace, g_textures[root.depth_pyramid_texture].Load(int3(cell, trace.mip)).x,
            cell_exit, screen);
        if (resolve.hit == 0u)
        {
            trace = resolve.descend != 0u ? vkr_ssr_trace_descend(trace)
                                           : vkr_ssr_trace_advance(trace, cell_exit);
            continue;
        }
        float2 hit_uv = vkr_ssr_lerp(trace.start_uv, trace.end_uv, resolve.hit_t);
        uint2 hit_pixel = min(uint2(floor(hit_uv * float2(source_extent))), source_extent - 1u);
        float hit_depth = vkr_vk_ssgi_positive_depth(
            screen, hit_pixel, g_depth_textures[root.depth_texture].Load(int3(hit_pixel, 0)),
            source_extent);
        float ray_depth = vkr_ssr_trace_depth_at(trace, resolve.hit_t);
        float3 hit_view = vkr_ssr_reconstruct_view_position(
            screen, (float2(hit_pixel) + 0.5f) / float2(source_extent),
            g_depth_textures[root.depth_texture].Load(int3(hit_pixel, 0)));
        float3 hit_normal = vkr_vk_ssgi_view_normal(root.params, root.normal_texture,
                                                     hit_pixel, hit_view);
        uint covered = g_storage_uint2_2d[root.vbuffer_texture][hit_pixel].x != 0u ? 1u : 0u;
        if (vkr_ssgi_hit_valid(covered, hit_depth, ray_depth, hit_normal,
                               direction, root.params))
        {
            estimate = vkr_ssgi_bare_diffuse_estimate(
                g_textures[root.direct_source_texture].Load(int3(hit_pixel, 0)).rgb);
            estimate *= vkr_ssgi_trace_confidence(hit_uv, screen);
            break;
        }
        trace = vkr_ssr_trace_advance(trace, cell_exit);
    }
    g_storage_2d[root.destination_texture][pixel] = float4(estimate, confidence);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssgi_temporal_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsgiTemporalRoot* root = g_ssgi_temporal_push.root;
    uint2 pixel = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    if (any(pixel >= trace_extent))
        return;
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
    VkrSsgiReceiver receiver = vkr_vk_ssgi_receiver(
        screen, pixel, trace_extent, source_extent, root.vbuffer_texture, root.depth_texture);
    if (receiver.valid == 0u)
    {
        g_storage_2d[root.output_color_texture][pixel] = float4(0.0f);
        g_storage_2d[root.output_depth_texture][pixel] = float4(0.0f);
        g_storage_uint2_2d[root.output_identity_texture][pixel] = uint2(0u);
        return;
    }
    float4 raw = g_textures[root.raw_texture].Load(int3(pixel, 0));
    uint2 identity = vkr_vk_ssgi_identity(root, receiver.pixel);
    float center_device_depth =
        g_depth_textures[root.depth_texture].Load(int3(receiver.pixel, 0));
    float3 center_origin = vkr_ssr_reconstruct_view_position(
        screen, (float2(receiver.pixel) + 0.5f) / float2(source_extent),
        center_device_depth);
    float3 center_normal = vkr_vk_ssgi_view_normal(
        root.params, root.normal_texture, receiver.pixel, center_origin);
    float3 neighborhood_sum = 0.0f;
    float3 neighborhood_square_sum = 0.0f;
    float neighborhood_weight = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            int2 neighbor = int2(pixel) + int2(x, y);
            if (all(neighbor >= 0) && all(uint2(neighbor) < trace_extent))
            {
                uint2 neighbor_pixel = uint2(neighbor);
                VkrSsgiReceiver neighbor_receiver = vkr_vk_ssgi_receiver(
                    screen, neighbor_pixel, trace_extent, source_extent,
                    root.vbuffer_texture, root.depth_texture);
                if (neighbor_receiver.valid == 0u)
                    continue;
                float4 sample =
                    g_textures[root.raw_texture].Load(int3(neighbor_pixel, 0));
                if (sample.w <= 0.0f)
                    continue;
                float neighbor_device_depth = g_depth_textures[root.depth_texture]
                    .Load(int3(neighbor_receiver.pixel, 0));
                float3 neighbor_origin = vkr_ssr_reconstruct_view_position(
                    screen, (float2(neighbor_receiver.pixel) + 0.5f) /
                                float2(source_extent),
                    neighbor_device_depth);
                float3 neighbor_normal = vkr_vk_ssgi_view_normal(
                    root.params, root.normal_texture, neighbor_receiver.pixel,
                    neighbor_origin);
                float weight = vkr_ssgi_spatial_weight(
                    float2(x, y), receiver.positive_depth,
                    neighbor_receiver.positive_depth, center_normal,
                    neighbor_normal, screen);
                if (weight <= 0.0f)
                    continue;
                // A trace miss is sample.rgb == 0 with sample.w == 1 and must
                // participate in this unbiased local Monte-Carlo average.
                neighborhood_sum += sample.rgb * weight;
                neighborhood_square_sum += sample.rgb * sample.rgb * weight;
                neighborhood_weight += weight;
            }
        }
    if (neighborhood_weight > 0.0f)
        raw.rgb = neighborhood_sum / neighborhood_weight;
    VkrSsrHistoryDecision decision;
    decision.accepted = 0u;
    decision.weight = 0.0f;
    decision.expected_previous_depth = 0.0f;
    decision.reserved_float_0 = 0.0f;
    if (root.params.history_valid != 0u)
    {
        float3 neighborhood_mean = neighborhood_sum / neighborhood_weight;
        float3 neighborhood_variance = max(
            neighborhood_square_sum / neighborhood_weight -
                neighborhood_mean * neighborhood_mean, float3(0.0f));
        float2 current_uv = (float2(receiver.pixel) + 0.5f) / float2(source_extent);
        float2 previous_uv = current_uv + g_textures[root.motion_texture].Load(int3(receiver.pixel, 0)).xy;
        float2 half_texel = float2(root.params.trace_texel_size_x,
                                   root.params.trace_texel_size_y) * 0.5f;
        float2 history_uv = clamp(previous_uv, half_texel, 1.0f - half_texel);
        uint2 history_pixel = min(uint2(floor(history_uv * float2(trace_extent))),
                                  trace_extent - 1u);
        decision = vkr_ssr_temporal_accept(
            screen, identity, g_storage_uint2_2d[root.history_identity_texture][history_pixel],
            g_textures[root.history_depth_texture].Load(int3(history_pixel, 0)).x,
            g_textures[root.validity_texture].Load(int3(receiver.pixel, 0)).y,
            previous_uv, g_textures[root.validity_texture].Load(int3(receiver.pixel, 0)).x);
        raw = vkr_ssgi_temporal_filter(raw,
            g_textures[root.history_color_texture].Load(int3(history_pixel, 0)),
            neighborhood_mean, neighborhood_variance, decision);
    }
    g_storage_2d[root.output_color_texture][pixel] = raw;
    g_storage_2d[root.output_depth_texture][pixel] =
        float4(receiver.positive_depth, 0.0f, 0.0f, 1.0f);
    g_storage_uint2_2d[root.output_identity_texture][pixel] = identity;
}

bool vkr_vk_ssgi_volume_valid(VkrVulkanPacketFrameRoot* frame, float3 world)
{
    if (frame.diffuse_volume_texture == 0u)
        return false;
    VkrDiffuseVolumeCell cell = vkr_diffuse_volume_cell(
        world, frame.diffuse_volume_origin.xyz, frame.diffuse_volume_inverse_spacing.xyz,
        frame.diffuse_volume_dimensions.xyz);
    return cell.valid &&
        g_textures[frame.diffuse_volume_texture].Load(int3(7, int(cell.row), 0)).y > 0.0f;
}

float vkr_vk_ssgi_filtered_roughness(VkrVkSsgiCompositeRoot* root, uint2 pixel,
                                     uint visible, float3 normal, float roughness,
                                     uint2 extent)
{
    uint2 limit = extent - 1u;
    uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
    uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
    float3 normal_x = vkr_vk_octahedral_decode(g_storage_float2_2d[root.normal_texture][pixel_x]);
    float3 normal_y = vkr_vk_octahedral_decode(g_storage_float2_2d[root.normal_texture][pixel_y]);
    float same_x = g_storage_uint2_2d[root.vbuffer_texture][pixel_x].x == visible ? 1.0f : 0.0f;
    float same_y = g_storage_uint2_2d[root.vbuffer_texture][pixel_y].x == visible ? 1.0f : 0.0f;
    float3 dx = (normal_x - normal) * same_x;
    float3 dy = (normal_y - normal) * same_y;
    return vkr_ggx_filter_roughness(roughness, 0.25f * (dot(dx, dx) + dot(dy, dy)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssgi_composite_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsgiCompositeRoot* root = g_ssgi_composite_push.root;
    uint2 pixel = thread_id.xy;
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    if (any(pixel >= source_extent) || g_storage_uint2_2d[root.vbuffer_texture][pixel].x == 0u)
        return;
    float device_depth = g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    float2 ndc = (float2(pixel) + 0.5f) / float2(source_extent) * 2.0f - 1.0f;
    float4 world_h = mul(root.inverse_view_projection, float4(ndc, device_depth, 1.0f));
    float3 world = world_h.xyz / (max(abs(world_h.w), 1e-7f) * (world_h.w < 0.0f ? -1.0f : 1.0f));
    if (vkr_vk_ssgi_volume_valid(root.frame, world))
        return;
    VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
    float current_depth = vkr_vk_ssgi_positive_depth(screen, pixel, device_depth, source_extent);
    if (!vkr_ssr_valid_depth(current_depth))
        return;
    float3 normal = vkr_vk_octahedral_decode(g_storage_float2_2d[root.normal_texture][pixel]);
    float2 trace_coordinate = (float2(pixel) + 0.5f) / float2(source_extent) *
                              float2(root.params.trace_width, root.params.trace_height) - 0.5f;
    int2 first = int2(floor(trace_coordinate));
    float3 radiance = 0.0f;
    float accepted_weight = 0.0f;
    float support_weight = 0.0f;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
        {
            int2 signed_trace = first + int2(x, y);
            if (any(signed_trace < 0) || any(uint2(signed_trace) >= trace_extent))
                continue;
            uint2 trace_pixel = uint2(signed_trace);
            VkrSsgiReceiver receiver = vkr_vk_ssgi_receiver(
                screen, trace_pixel, trace_extent, source_extent,
                root.vbuffer_texture, root.depth_texture);
            if (receiver.valid == 0u)
                continue;
            float3 receiver_normal = vkr_vk_octahedral_decode(
                g_storage_float2_2d[root.normal_texture][receiver.pixel]);
            float2 offset = float2(trace_pixel) - trace_coordinate;
            float bilinear = max(1.0f - abs(offset.x), 0.0f) *
                              max(1.0f - abs(offset.y), 0.0f);
            float weight = bilinear * vkr_ssr_receiver_bilateral_weight(
                offset, current_depth,
                g_textures[root.history_depth_texture].Load(int3(trace_pixel, 0)).x,
                normal, receiver_normal, screen);
            if (weight <= 0.0f)
                continue;
            support_weight += weight;
            float4 sample = g_textures[root.reflection_texture].Load(int3(trace_pixel, 0));
            radiance += sample.rgb * weight;
            accepted_weight += weight * saturate(sample.a);
        }
    if (accepted_weight <= 0.0f || support_weight <= 0.0f)
        return;
    radiance /= accepted_weight;
    float confidence = saturate(accepted_weight / support_weight);
    uint visible = g_storage_uint2_2d[root.vbuffer_texture][pixel].x;
    float4 albedo = g_storage_2d[root.albedo_texture][pixel];
    float4 specular = g_storage_2d[root.specular_texture][pixel];
    float roughness = vkr_vk_ssgi_filtered_roughness(root, pixel, visible, normal,
                                                      clamp(specular.w, 0.04f, 1.0f),
                                                      source_extent);
    float3 view = normalize(root.frame.view_position.xyz - world);
    VkrGgxMaterialEnergy energy = packet_prepare_gbuffer_brdf(
        root.frame, normal, view, roughness, saturate(specular.rgb), g_textures[root.anisotropy_texture].Load(int3(pixel, 0)));
    energy = vkr_ggx_diffuse_transmission(energy,
        root.frame.materials[root.visible_rows[visible - 1u].material_index].material_diffuse_transmission);
    float3 diffuse_weight = energy.diffuse_weight;
    float4 sheen_packed = g_textures[root.sheen_texture].Load(int3(pixel, 0));
    if (vkr_sheen_active(sheen_packed.rgb))
    {
        VkrSheenLayer sheen = packet_prepare_sheen(
            root.frame, sheen_packed.rgb, sheen_packed.a, normal, view,
            packet_sheen_allocation_reserve(
                root.frame, max(dot(normal, view), 0.0f), sheen_packed.a));
        diffuse_weight *= sheen.base_transmission;
    }
    float4 clearcoat_packed = g_textures[root.clearcoat_texture].Load(int3(pixel, 0));
    if (vkr_clearcoat_active(clearcoat_packed.x))
    {
        VkrClearcoatLayer clearcoat = packet_prepare_clearcoat(
            root.frame, clearcoat_packed.x, clearcoat_packed.y,
            vkr_vk_octahedral_decode(clearcoat_packed.zw), view);
        diffuse_weight *= clearcoat.base_transmission;
    }
    float4 scene = g_storage_2d[root.scene_texture][pixel];
    scene.rgb = vkr_ssgi_add_diffuse(scene.rgb, radiance, diffuse_weight,
                                     albedo.rgb, albedo.a, confidence);
    if (root.subsurface_profile_count != 0u) {
        float4 subsurface = root.frame.materials[root.visible_rows[visible-1u].material_index].material_subsurface;
        if (subsurface.x > 0.0f && uint(subsurface.y) < root.subsurface_profile_count) {
            float4 source = g_storage_2d[root.subsurface_source_texture][pixel];
            source.rgb += vkr_subsurface_root_amplitude(albedo.rgb) * radiance * saturate(albedo.a) * saturate(confidence);
            source.rgb = vkr_subsurface_source_storage(source.rgb);
            g_storage_2d[root.subsurface_source_texture][pixel] = source;
        }
    }
    g_storage_2d[root.scene_texture][pixel] = scene;
}

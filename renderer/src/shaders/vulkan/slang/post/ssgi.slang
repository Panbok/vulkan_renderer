// Vulkan SSGI compute passes. Shared traversal/temporal math lives in
// ssgi_kernel.slangh after ssr_kernel.slangh; this file owns descriptor access.

struct VkrVkSsgiDepthBaseRoot
{
    VkrSsgiParams params;
    uint depth_texture;
    uint vbuffer_texture;
    uint destination_depth_texture;
    uint receiver_texture;
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
    uint receiver_texture;
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
    uint receiver_texture;
    float history_pre_exposure_scale;
    // Displayed radiance per pre-exposed radiance unit; finite and positive.
    float display_exposure_scale;
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
    uint receiver_texture;
    // DiffuseVolume's full-resolution image, zero without a volume.
    uint diffuse_volume_texture;
    uint2 reserved;
};

struct VkrVkSsgiDepthBasePush { VkrVkSsgiDepthBaseRoot* root; uint2 reserved; };
struct VkrVkSsgiDepthMipPush { VkrVkSsgiDepthMipRoot* root; uint2 reserved; };
struct VkrVkSsgiTracePush { VkrVkSsgiTraceRoot* root; uint2 reserved; };
struct VkrVkSsgiTemporalPush { VkrVkSsgiTemporalRoot* root; uint2 reserved; };
struct VkrVkSsgiCompositePush { VkrVkSsgiCompositeRoot* root; uint2 reserved; };

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

uint2 vkr_vk_ssgi_identity(VkrVkSsgiTemporalRoot root, uint2 receiver_pixel)
{
    uint encoded = g_uint2_textures[root.vbuffer_texture]
                       .Load(int3(receiver_pixel, 0)).x;
    if (encoded == 0u)
        return uint2(0u);
    VkrGpuVisibleDrawRow visible = root.visible_rows[encoded - 1u];
    VkrVulkanInstance instance = root.instances[visible.instance_index];
    return instance.deformation_address == 0
      ? uint2(instance.temporal_index + 1u, instance.temporal_generation)
      : uint2(0u);
}

float3 vkr_vk_ssgi_view_normal(VkrSsgiParams params, uint normal_texture,
                               uint2 pixel, float3 origin)
{
    float3 world_normal = vkr_vk_octahedral_decode(
        g_textures[normal_texture].Load(int3(pixel, 0)).xy);
    float3 view_normal = normalize(mul(params.view, float4(world_normal, 0.0f)).xyz);
    return vkr_ssgi_faceforward_view_normal(view_normal, origin);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssgi_depth_base_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsgiDepthBaseRoot root = vkr_vk_uniform_root(*g_ssgi_depth_base_push.root);
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
    VkrSsgiReceiver receiver = vkr_ssgi_empty_receiver();
    for (uint y = begin_y; y < end_y; ++y)
        for (uint x = begin_x; x < end_x; ++x)
        {
            uint2 child = uint2(x, y);
            if (g_uint2_textures[root.vbuffer_texture].Load(int3(child, 0)).x != 0u)
                values[count] = vkr_vk_ssgi_positive_depth(
                    screen, child, g_depth_textures[root.depth_texture].Load(int3(child, 0)),
                    source_extent);
            receiver = vkr_ssgi_consider_receiver(receiver, child, values[count],
                vkr_ssr_valid_depth(values[count]) ? 1u : 0u);
            ++count;
        }
    g_storage_uint2_2d[root.receiver_texture][pixel] = asuint(vkr_ssgi_pack_receiver(receiver, pixel));
    g_storage_2d[root.destination_depth_texture][pixel] =
        float4(vkr_ssr_depth_reduce(values[0], values[1], values[2], values[3],
                                    values[4], values[5], values[6], values[7],
                                    values[8], count), 0.0f, 0.0f, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssgi_depth_mip_compute(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsgiDepthMipRoot root = vkr_vk_uniform_root(*g_ssgi_depth_mip_push.root);
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
    VkrVkSsgiTraceRoot root = vkr_vk_uniform_root(*g_ssgi_trace_push.root);
    uint2 pixel = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    if (any(pixel >= trace_extent))
        return;
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
    VkrSsgiReceiver receiver = vkr_ssgi_unpack_receiver(asfloat(g_uint2_textures[root.receiver_texture].Load(int3(pixel, 0))), pixel);
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
    VkrSsrTrace trace = vkr_ssgi_trace_begin(screen,
        origin + normal * root.params.normal_bias, direction);
    float3 estimate = 0.0f;
    bool hit = false;
    [loop]
    for (uint step = 0u; step < root.params.max_steps && trace.active != 0u; ++step)
    {
        uint2 mip_extent = max(trace_extent >> trace.mip, uint2(1u));
        VkrSsrCellExit cell_exit =
            vkr_ssgi_trace_cell_exit(trace, mip_extent, screen);
        float2 trace_uv = vkr_ssr_trace_uv(trace);
        uint2 source_pixel = min(uint2(floor(trace_uv * float2(source_extent))), source_extent - 1u);
        uint2 cell = min(source_pixel >> (trace.mip + 1u), mip_extent - 1u);
        float hierarchy_depth =
            g_textures[root.depth_pyramid_texture].Load(int3(cell, trace.mip)).x;
        VkrSsrTraceResolve resolve = vkr_ssgi_trace_resolve(
            trace, hierarchy_depth, cell_exit.t,
            vkr_ssgi_thickness(root.params, hierarchy_depth));
        if (resolve.hit == 0u)
        {
            trace = resolve.descend != 0u
                        ? vkr_ssr_trace_descend(trace)
                        : vkr_ssr_trace_advance(trace, cell_exit.restart_t);
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
        uint covered = g_uint2_textures[root.vbuffer_texture]
                           .Load(int3(hit_pixel, 0)).x != 0u ? 1u : 0u;
        if (vkr_ssgi_hit_valid(covered, hit_depth, ray_depth, hit_normal,
                               direction,
                               vkr_ssgi_thickness(root.params, hit_depth)))
        {
            estimate = vkr_ssgi_bare_diffuse_estimate(
                g_textures[root.direct_source_texture].Load(int3(hit_pixel, 0)).rgb);
            estimate *= vkr_ssgi_trace_confidence(hit_uv, screen);
            hit = true;
            break;
        }
        trace = vkr_ssr_trace_advance(trace, cell_exit.restart_t);
    }
    // A screen miss is a valid zero Monte-Carlo sample (alpha 1). A ray still
    // active after its decision budget has no answer: alpha 0 keeps it out of
    // the estimator instead of counting it as a dark miss.
    float sample_weight = hit || trace.active == 0u ? 1.0f : 0.0f;
    g_storage_2d[root.destination_texture][pixel] =
        float4(vkr_ssgi_storable(estimate), sample_weight);
}

// The temporal 3x3 filter of a group's 8x8 trace texels reads a 10x10 tile.
// Each tile texel's receiver-only data is formed once per group instead of
// once per tap of each of up to nine receivers: whether the texel holds a raw
// sample from a valid receiver, the sample, the receiver's camera ray
// and depth scale, whose product each tap forms so that it rounds as when
// evaluated in place, and the receiver's view normal facing that position.
#define VKR_VK_SSGI_TEMPORAL_GROUP 8
#define VKR_VK_SSGI_TEMPORAL_TILE (VKR_VK_SSGI_TEMPORAL_GROUP + 2)
#define VKR_VK_SSGI_TEMPORAL_TILE_TEXELS \
    (VKR_VK_SSGI_TEMPORAL_TILE * VKR_VK_SSGI_TEMPORAL_TILE)

groupshared uint g_ssgi_temporal_valid[VKR_VK_SSGI_TEMPORAL_TILE_TEXELS];
groupshared float3 g_ssgi_temporal_sample[VKR_VK_SSGI_TEMPORAL_TILE_TEXELS];
groupshared float3 g_ssgi_temporal_on_ray[VKR_VK_SSGI_TEMPORAL_TILE_TEXELS];
groupshared float g_ssgi_temporal_depth_scale[VKR_VK_SSGI_TEMPORAL_TILE_TEXELS];
groupshared float3 g_ssgi_temporal_normal[VKR_VK_SSGI_TEMPORAL_TILE_TEXELS];

void vkr_vk_ssgi_temporal_fill(VkrVkSsgiTemporalRoot root, VkrSsrParams screen,
                               int2 tile_origin, uint thread_index,
                               uint2 trace_extent, uint2 source_extent)
{
    for (uint texel = thread_index; texel < VKR_VK_SSGI_TEMPORAL_TILE_TEXELS;
         texel += VKR_VK_SSGI_TEMPORAL_GROUP * VKR_VK_SSGI_TEMPORAL_GROUP)
    {
        int2 trace_pixel = tile_origin + int2(int(texel % VKR_VK_SSGI_TEMPORAL_TILE),
                                              int(texel / VKR_VK_SSGI_TEMPORAL_TILE));
        uint valid = 0u;
        if (all(trace_pixel >= 0) && all(uint2(trace_pixel) < trace_extent))
        {
            uint2 neighbor_pixel = uint2(trace_pixel);
            VkrSsgiReceiver receiver = vkr_ssgi_unpack_receiver(asfloat(g_uint2_textures[root.receiver_texture].Load(int3(neighbor_pixel, 0))), neighbor_pixel);
            // Alpha zero is an exhausted ray: no sample. A trace miss is rgb 0
            // with alpha 1 and stays in this unbiased local Monte-Carlo average.
            float4 sample = g_textures[root.raw_texture].Load(int3(neighbor_pixel, 0));
            if (receiver.valid != 0u && !(sample.w <= 0.0f) &&
                vkr_ssgi_finite(sample.rgb))
            {
                float3 on_ray = vkr_ssgi_camera_ray(
                    screen, (float2(receiver.pixel) + 0.5f) / float2(source_extent));
                float depth_scale =
                    vkr_ssgi_ray_depth_scale(on_ray, receiver.positive_depth);
                g_ssgi_temporal_sample[texel] = sample.rgb;
                g_ssgi_temporal_on_ray[texel] = on_ray;
                g_ssgi_temporal_depth_scale[texel] = depth_scale;
                g_ssgi_temporal_normal[texel] = vkr_vk_ssgi_view_normal(
                    root.params, root.normal_texture, receiver.pixel,
                    on_ray * depth_scale);
                valid = 1u;
            }
        }
        g_ssgi_temporal_valid[texel] = valid;
    }
}

[shader("compute")]
[numthreads(VKR_VK_SSGI_TEMPORAL_GROUP, VKR_VK_SSGI_TEMPORAL_GROUP, 1)]
void ssgi_temporal_compute(uint3 thread_id : SV_DispatchThreadID,
                           uint3 group_thread : SV_GroupThreadID)
{
    VkrVkSsgiTemporalRoot root = vkr_vk_uniform_root(*g_ssgi_temporal_push.root);
    uint2 pixel = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
    // Every thread, past the image edge too, fills the tile before any
    // receiver of the group reads it.
    int2 tile_origin = int2(pixel - group_thread.xy) - 1;
    vkr_vk_ssgi_temporal_fill(root, screen, tile_origin,
                              group_thread.y * VKR_VK_SSGI_TEMPORAL_GROUP + group_thread.x,
                              trace_extent, source_extent);
    GroupMemoryBarrierWithGroupSync();
    if (any(pixel >= trace_extent))
        return;
    VkrSsgiReceiver receiver = vkr_ssgi_unpack_receiver(asfloat(g_uint2_textures[root.receiver_texture].Load(int3(pixel, 0))), pixel);
    if (receiver.valid == 0u)
    {
        g_storage_2d[root.output_color_texture][pixel] = float4(0.0f);
        g_storage_2d[root.output_depth_texture][pixel] = float4(0.0f);
        g_storage_uint2_2d[root.output_identity_texture][pixel] = uint2(0u);
        return;
    }
    uint2 identity = vkr_vk_ssgi_identity(root, receiver.pixel);
    float2 current_uv = (float2(receiver.pixel) + 0.5f) / float2(source_extent);
    float center_device_depth =
        g_depth_textures[root.depth_texture].Load(int3(receiver.pixel, 0));
    float3 center_origin = vkr_ssr_reconstruct_view_position(
        screen, current_uv, center_device_depth);
    float3 center_normal = vkr_vk_ssgi_view_normal(
        root.params, root.normal_texture, receiver.pixel, center_origin);
    float minimum_tolerance =
        vkr_ssgi_thickness(root.params, receiver.positive_depth);

    // Accepted history first: its luminance is one reference of the raw
    // samples' firefly bound.
    float3 history = 0.0f;
    float history_support = 0.0f;
    if (root.params.history_valid != 0u)
    {
        float2 motion = g_textures[root.motion_texture].Load(int3(receiver.pixel, 0)).xy;
        float2 previous_uv = vkr_ssr_previous_uv(screen, current_uv, motion);
        float2 trace_texel = float2(root.params.trace_texel_size_x,
                                    root.params.trace_texel_size_y);
        float2 half_texel = trace_texel * 0.5f;
        bool in_bounds = all(previous_uv >= half_texel) &&
                         all(previous_uv <= 1.0f - half_texel);
        float2 coordinate = clamp(previous_uv, half_texel, 1.0f - half_texel) *
                                float2(trace_extent) - 0.5f;
        int2 first = int2(floor(coordinate));
        float2 validity =
            g_textures[root.validity_texture].Load(int3(receiver.pixel, 0)).xy;
        float previous_depth = vkr_ssr_linearize_previous_depth(screen, validity.y);
        // The stored receiver lies anywhere in its 2x2 cell, up to half a
        // trace texel from the tap centre: allow the plane's depth change
        // over that distance, at the previous depth's scale.
        float texel_depth_change = vkr_ssgi_texel_depth_change(
            screen, center_origin, center_normal, current_uv, trace_texel,
            receiver.positive_depth);
        float placement_tolerance = 0.5f * texel_depth_change *
            previous_depth / max(receiver.positive_depth, VKR_SSGI_EPSILON);
        float3 history_sum = 0.0f;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
            {
                int2 p = first + int2(x, y);
                if (any(p < 0) || any(p >= int2(trace_extent)))
                    continue;
                float weight = vkr_ssr_history_tap_weight(coordinate, p);
                if (weight <= 0.0f)
                    continue;
                // The tap centre moves back into the current raster by the
                // receiver's own motion, then meets the receiver's plane.
                float2 tap_uv = (float2(p) + 0.5f) * trace_texel -
                                (previous_uv - current_uv);
                float expected_depth = vkr_ssgi_history_tap_depth(
                    screen, center_origin, center_normal, previous_depth, tap_uv);
                if (!vkr_ssgi_history_tap_accepted(
                        screen, identity,
                        g_storage_uint2_2d[root.history_identity_texture][p],
                        g_textures[root.history_depth_texture].Load(int3(p, 0)).x,
                        expected_depth, placement_tolerance, in_bounds,
                        validity.x))
                    continue;
                float4 sample =
                    g_textures[root.history_color_texture].Load(int3(p, 0));
                if (!vkr_ssgi_finite(sample.rgb))
                    continue;
                float confidence_weight = weight * saturate(sample.w);
                history_sum += sample.rgb * confidence_weight;
                history_support += confidence_weight;
            }
        if (history_support > 0.0f)
            history = history_sum / history_support * root.history_pre_exposure_scale;
    }

    // Gather the 3x3 raw samples next: each sample's firefly bound needs the
    // weighted mean luminance of the other samples before any sample is used.
    float3 samples[9];
    float weights[9];
    float weight_sum = 0.0f;
    float luminance_sum = 0.0f;
    [unroll]
    for (uint tap = 0u; tap < 9u; ++tap)
    {
        samples[tap] = 0.0f;
        weights[tap] = 0.0f;
        int2 offset = int2(int(tap % 3u) - 1, int(tap / 3u) - 1);
        int2 neighbor = int2(pixel) + offset;
        if (any(neighbor < 0) || any(uint2(neighbor) >= trace_extent))
            continue;
        int2 local = int2(group_thread.xy) + 1 + offset;
        uint texel = uint(local.y * VKR_VK_SSGI_TEMPORAL_TILE + local.x);
        if (g_ssgi_temporal_valid[texel] == 0u)
            continue;
        float3 sample = g_ssgi_temporal_sample[texel];
        float weight = vkr_ssgi_plane_weight(
            float2(offset), center_origin, center_normal,
            g_ssgi_temporal_on_ray[texel] * g_ssgi_temporal_depth_scale[texel],
            g_ssgi_temporal_normal[texel], minimum_tolerance);
        if (weight <= 0.0f)
            continue;
        samples[tap] = sample;
        weights[tap] = weight;
        weight_sum += weight;
        luminance_sum += weight * vkr_exposure_luminance(sample);
    }
    float4 current = 0.0f;
    float3 neighborhood_variance = 0.0f;
    if (weight_sum > 0.0f)
    {
        float history_luminance = vkr_exposure_luminance(history);
        float3 sum = 0.0f;
        float3 square_sum = 0.0f;
        [unroll]
        for (uint tap = 0u; tap < 9u; ++tap)
        {
            if (weights[tap] <= 0.0f)
                continue;
            float reference = max(
                vkr_ssgi_other_mean_luminance(
                    luminance_sum, weight_sum,
                    vkr_exposure_luminance(samples[tap]), weights[tap]),
                history_luminance);
            float3 clamped = vkr_ssgi_firefly_clamp(
                samples[tap], reference, weights[tap] / weight_sum,
                root.display_exposure_scale);
            sum += clamped * weights[tap];
            square_sum += clamped * clamped * weights[tap];
        }
        current = float4(sum / weight_sum, 1.0f);
        neighborhood_variance =
            max(square_sum / weight_sum - current.rgb * current.rgb, float3(0.0f));
    }
    if (history_support > 0.0f)
    {
        // The bilinear weights sum to one, so the support is the accepted
        // share of the footprint: one weak surviving tap keeps a weak
        // history weight instead of the full retention.
        current = vkr_ssgi_temporal_filter(
            current, history, current.rgb, neighborhood_variance,
            root.params.temporal_weight * saturate(history_support),
            vkr_ssgi_empty_retention(history_support));
    }
    g_storage_2d[root.output_color_texture][pixel] =
        float4(vkr_ssgi_storable(current.rgb), current.w);
    g_storage_2d[root.output_depth_texture][pixel] =
        float4(receiver.positive_depth, 0.0f, 0.0f, 1.0f);
    g_storage_uint2_2d[root.output_identity_texture][pixel] = identity;
}

// One trace texel as seen from a full-resolution pixel: the plane weight of the
// texel's receiver against the pixel's surface, and the texel's temporal
// output. A zero weight means the texel cannot contribute.
struct VkrVkSsgiCompositeTap
{
    float weight;
    float4 history;
};

// The part of a composite tap that depends only on the trace texel: its
// receiver's camera ray and depth scale (vkr_ssgi_view_at_depth, whose
// product the tap forms so that it rounds as when evaluated in place), its
// view normal and its temporal output. `state` is
// VKR_VK_SSGI_TEXEL_NONE when the texel cannot contribute, and
// VKR_VK_SSGI_TEXEL_PIXEL_NORMAL when its normal is degenerate, so the reading
// pixel's own normal stands in (vkr_ssgi_normalize_or).
#define VKR_VK_SSGI_TEXEL_NONE 0u
#define VKR_VK_SSGI_TEXEL_VALID 1u
#define VKR_VK_SSGI_TEXEL_PIXEL_NORMAL 2u

struct VkrVkSsgiCompositeTexel
{
    uint state;
    float3 on_ray;
    float depth_scale;
    float3 normal;
    float4 history;
};

VkrVkSsgiCompositeTexel vkr_vk_ssgi_composite_texel(VkrVkSsgiCompositeRoot root,
                                                    VkrSsrParams screen,
                                                    uint2 trace_pixel)
{
    VkrVkSsgiCompositeTexel texel;
    texel.state = VKR_VK_SSGI_TEXEL_NONE;
    texel.on_ray = 0.0f;
    texel.depth_scale = 0.0f;
    texel.normal = 0.0f;
    texel.history = 0.0f;
    VkrSsgiReceiver receiver = vkr_ssgi_unpack_receiver(asfloat(g_uint2_textures[root.receiver_texture].Load(int3(trace_pixel, 0))), trace_pixel);
    float receiver_depth =
        g_textures[root.history_depth_texture].Load(int3(trace_pixel, 0)).x;
    if (receiver.valid == 0u || !vkr_ssr_valid_depth(receiver_depth))
        return texel;
    float4 sample = g_textures[root.reflection_texture].Load(int3(trace_pixel, 0));
    if (!vkr_ssgi_finite(sample.rgb))
        return texel;
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    texel.on_ray = vkr_ssgi_camera_ray(
        screen, (float2(receiver.pixel) + 0.5f) / float2(source_extent));
    texel.depth_scale = vkr_ssgi_ray_depth_scale(texel.on_ray, receiver_depth);
    float3 normal = mul(root.params.view,
                        float4(vkr_vk_octahedral_decode(
                                   g_textures[root.normal_texture]
                                       .Load(int3(receiver.pixel, 0)).xy),
                               0.0f)).xyz;
    float length_squared = dot(normal, normal);
    texel.state = length_squared > 1e-12f ? VKR_VK_SSGI_TEXEL_VALID
                                          : VKR_VK_SSGI_TEXEL_PIXEL_NORMAL;
    texel.normal = normal * rsqrt(length_squared);
    texel.history = sample;
    return texel;
}

VkrVkSsgiCompositeTap vkr_vk_ssgi_composite_tap(
    VkrVkSsgiCompositeTexel texel, uint2 trace_pixel, float2 trace_coordinate,
    float3 position, float3 normal_view, float minimum_tolerance)
{
    VkrVkSsgiCompositeTap tap;
    tap.weight = 0.0f;
    tap.history = 0.0f;
    if (texel.state == VKR_VK_SSGI_TEXEL_NONE)
        return tap;
    float3 receiver_normal =
        texel.state == VKR_VK_SSGI_TEXEL_VALID ? texel.normal : normal_view;
    tap.weight = vkr_ssgi_plane_weight(
        float2(trace_pixel) - trace_coordinate, position, normal_view,
        texel.on_ray * texel.depth_scale, receiver_normal, minimum_tolerance);
    tap.history = texel.history;
    return tap;
}

// A group's 8x8 pixels read the trace texels of their 2x2 bilinear footprints
// and, at a silhouette, the 3x3 around their half-resolution cells. With the
// trace extent half the source extent (vkr_ssgi_reduced_extent) these are
// texels P/2 - 1 to P/2 + 4 of a group whose first pixel is P. Each texel's
// receiver-only work runs once per group; a tap outside the tile, which
// another extent ratio could ask for, evaluates its texel directly.
#define VKR_VK_SSGI_COMPOSITE_GROUP 8
#define VKR_VK_SSGI_COMPOSITE_TILE 6
#define VKR_VK_SSGI_COMPOSITE_TILE_TEXELS \
    (VKR_VK_SSGI_COMPOSITE_TILE * VKR_VK_SSGI_COMPOSITE_TILE)

groupshared uint g_ssgi_composite_state[VKR_VK_SSGI_COMPOSITE_TILE_TEXELS];
groupshared float3 g_ssgi_composite_on_ray[VKR_VK_SSGI_COMPOSITE_TILE_TEXELS];
groupshared float g_ssgi_composite_depth_scale[VKR_VK_SSGI_COMPOSITE_TILE_TEXELS];
groupshared float3 g_ssgi_composite_normal[VKR_VK_SSGI_COMPOSITE_TILE_TEXELS];
groupshared float4 g_ssgi_composite_history[VKR_VK_SSGI_COMPOSITE_TILE_TEXELS];

VkrVkSsgiCompositeTexel vkr_vk_ssgi_composite_tile_texel(
    VkrVkSsgiCompositeRoot root, VkrSsrParams screen, int2 tile_origin,
    uint2 trace_pixel)
{
    int2 local = int2(trace_pixel) - tile_origin;
    if (any(local < 0) || any(local >= VKR_VK_SSGI_COMPOSITE_TILE))
        return vkr_vk_ssgi_composite_texel(root, screen, trace_pixel);
    uint index = uint(local.y * VKR_VK_SSGI_COMPOSITE_TILE + local.x);
    VkrVkSsgiCompositeTexel texel;
    texel.state = g_ssgi_composite_state[index];
    texel.on_ray = g_ssgi_composite_on_ray[index];
    texel.depth_scale = g_ssgi_composite_depth_scale[index];
    texel.normal = g_ssgi_composite_normal[index];
    texel.history = g_ssgi_composite_history[index];
    return texel;
}

float vkr_vk_ssgi_filtered_roughness(VkrVkSsgiCompositeRoot root, uint2 pixel,
                                     uint visible, float3 normal, float roughness,
                                     uint2 extent)
{
    uint2 limit = extent - 1u;
    uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
    uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
    float3 normal_x = vkr_vk_octahedral_decode(
        g_textures[root.normal_texture].Load(int3(pixel_x, 0)).xy);
    float3 normal_y = vkr_vk_octahedral_decode(
        g_textures[root.normal_texture].Load(int3(pixel_y, 0)).xy);
    float same_x = g_uint2_textures[root.vbuffer_texture].Load(int3(pixel_x, 0)).x == visible ? 1.0f : 0.0f;
    float same_y = g_uint2_textures[root.vbuffer_texture].Load(int3(pixel_y, 0)).x == visible ? 1.0f : 0.0f;
    float3 dx = (normal_x - normal) * same_x;
    float3 dy = (normal_y - normal) * same_y;
    return vkr_ggx_filter_roughness(roughness, 0.25f * (dot(dx, dx) + dot(dy, dy)));
}

[shader("compute")]
[numthreads(VKR_VK_SSGI_COMPOSITE_GROUP, VKR_VK_SSGI_COMPOSITE_GROUP, 1)]
void ssgi_composite_compute(uint3 thread_id : SV_DispatchThreadID,
                            uint3 group_thread : SV_GroupThreadID)
{
    VkrVkSsgiCompositeRoot root = vkr_vk_uniform_root(*g_ssgi_composite_push.root);
    uint2 pixel = thread_id.xy;
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    VkrSsrParams screen = vkr_ssgi_screen_trace_params(root.params);
    // Every thread, past the image edge too, takes part before any pixel of
    // the group reads the tile.
    int2 tile_origin = int2((pixel - group_thread.xy) / 2u) - 1;
    uint tile_index = group_thread.y * VKR_VK_SSGI_COMPOSITE_GROUP + group_thread.x;
    if (tile_index < VKR_VK_SSGI_COMPOSITE_TILE_TEXELS)
    {
        int2 trace_pixel = tile_origin + int2(int(tile_index % VKR_VK_SSGI_COMPOSITE_TILE),
                                              int(tile_index / VKR_VK_SSGI_COMPOSITE_TILE));
        VkrVkSsgiCompositeTexel texel;
        texel.state = VKR_VK_SSGI_TEXEL_NONE;
        texel.on_ray = 0.0f;
        texel.depth_scale = 0.0f;
        texel.normal = 0.0f;
        texel.history = 0.0f;
        if (all(trace_pixel >= 0) && all(uint2(trace_pixel) < trace_extent))
            texel = vkr_vk_ssgi_composite_texel(root, screen, uint2(trace_pixel));
        g_ssgi_composite_state[tile_index] = texel.state;
        g_ssgi_composite_on_ray[tile_index] = texel.on_ray;
        g_ssgi_composite_depth_scale[tile_index] = texel.depth_scale;
        g_ssgi_composite_normal[tile_index] = texel.normal;
        g_ssgi_composite_history[tile_index] = texel.history;
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(pixel >= source_extent) ||
        g_uint2_textures[root.vbuffer_texture].Load(int3(pixel, 0)).x == 0u)
        return;
    // SSGI adds nothing where deferred lighting used the baked volume: the bake
    // already holds that bounce (ADR-060). Reading deferred lighting's own
    // per-pixel coverage keeps the two decisions identical at the volume edge.
    if (root.diffuse_volume_texture != 0u &&
        g_storage_2d[root.diffuse_volume_texture][pixel].a > 0.0f)
        return;
    float device_depth = g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    float2 ndc = (float2(pixel) + 0.5f) / float2(source_extent) * 2.0f - 1.0f;
    float4 world_h = mul(root.inverse_view_projection, float4(ndc, device_depth, 1.0f));
    float3 world = world_h.xyz / (max(abs(world_h.w), 1e-7f) * (world_h.w < 0.0f ? -1.0f : 1.0f));
    float3 position = vkr_ssr_reconstruct_view_position(
        screen, (float2(pixel) + 0.5f) / float2(source_extent), device_depth);
    float current_depth = -position.z;
    if (!vkr_ssr_valid_depth(current_depth))
        return;
    float3 normal = vkr_vk_octahedral_decode(
        g_textures[root.normal_texture].Load(int3(pixel, 0)).xy);
    float3 normal_view = vkr_ssgi_normalize_or(
        mul(root.params.view, float4(normal, 0.0f)).xyz, float3(0.0f, 0.0f, 1.0f));
    float minimum_tolerance = vkr_ssgi_thickness(root.params, current_depth);
    float2 trace_coordinate = (float2(pixel) + 0.5f) / float2(source_extent) *
                              float2(root.params.trace_width, root.params.trace_height) - 0.5f;
    int2 first = int2(floor(trace_coordinate));
    float3 radiance = 0.0f;
    float accepted_weight = 0.0f;
    float support_weight = 0.0f;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
        {
            int2 signed_trace = first + int2(x, y);
            if (any(signed_trace < 0) || any(uint2(signed_trace) >= trace_extent))
                continue;
            uint2 trace_pixel = uint2(signed_trace);
            float2 offset = float2(trace_pixel) - trace_coordinate;
            float bilinear = max(1.0f - abs(offset.x), 0.0f) *
                              max(1.0f - abs(offset.y), 0.0f);
            if (bilinear <= 0.0f)
                continue;
            VkrVkSsgiCompositeTap tap = vkr_vk_ssgi_composite_tap(
                vkr_vk_ssgi_composite_tile_texel(root, screen, tile_origin,
                                                 trace_pixel),
                trace_pixel, trace_coordinate, position, normal_view,
                minimum_tolerance);
            float weight = bilinear * tap.weight;
            if (weight <= 0.0f)
                continue;
            float accepted = weight * saturate(tap.history.a);
            support_weight += weight;
            radiance += tap.history.rgb * accepted;
            accepted_weight += accepted;
        }
    if (support_weight <= 0.0f)
    {
        // Silhouette rim: each bilinear texel chose a receiver on another
        // surface, as nearest-receiver selection favours the foreground. Take
        // the best plane match in the 3x3 trace footprint of the pixel's cell.
        int2 cell = int2(min(pixel / 2u, trace_extent - 1u));
        float best_weight = 0.0f;
        float4 best_sample = 0.0f;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 signed_trace = cell + int2(x, y);
                if (any(signed_trace < 0) || any(uint2(signed_trace) >= trace_extent))
                    continue;
                VkrVkSsgiCompositeTap tap = vkr_vk_ssgi_composite_tap(
                    vkr_vk_ssgi_composite_tile_texel(root, screen, tile_origin,
                                                     uint2(signed_trace)),
                    uint2(signed_trace), trace_coordinate, position,
                    normal_view, minimum_tolerance);
                if (tap.weight > best_weight)
                {
                    best_weight = tap.weight;
                    best_sample = tap.history;
                }
            }
        support_weight = best_weight;
        accepted_weight = best_weight * saturate(best_sample.a);
        radiance = best_sample.rgb * accepted_weight;
    }
    if (accepted_weight <= 0.0f || support_weight <= 0.0f)
        return;
    radiance /= accepted_weight;
    float confidence = saturate(accepted_weight / support_weight);
    uint visible = g_uint2_textures[root.vbuffer_texture].Load(int3(pixel, 0)).x;
    float4 albedo = g_textures[root.albedo_texture].Load(int3(pixel, 0));
    float4 specular = g_textures[root.specular_texture].Load(int3(pixel, 0));
    float roughness = vkr_vk_ssgi_filtered_roughness(root, pixel, visible, normal,
                                                      clamp(specular.w, 0.04f, 1.0f),
                                                      source_extent);
    float3 view = normalize(root.frame.view_position.xyz - world);
    VkrGgxMaterialEnergy energy = packet_prepare_gbuffer_brdf(
        root.frame, normal, view, roughness, saturate(specular.rgb), (root.anisotropy_texture == 0u ? float4(0.0f) : g_textures[root.anisotropy_texture].Load(int3(pixel, 0))));
    VkrGpuVisibleDrawRow visible_row = root.visible_rows[visible - 1u];
    VkrVulkanMaterial material =
        root.frame.materials[visible_row.material_index];
    energy = vkr_ggx_diffuse_transmission(
        energy, material.material_diffuse_transmission);
    float3 diffuse_weight = energy.diffuse_weight;
    float4 sheen_packed = (root.sheen_texture == 0u ? float4(0.0f) : g_textures[root.sheen_texture].Load(int3(pixel, 0)));
    if (vkr_sheen_active(sheen_packed.rgb))
    {
        VkrSheenLayer sheen = packet_prepare_sheen(
            root.frame, sheen_packed.rgb, sheen_packed.a, normal, view,
            packet_sheen_allocation_reserve(
                root.frame, max(dot(normal, view), 0.0f), sheen_packed.a));
        diffuse_weight *= sheen.base_transmission;
    }
    float4 clearcoat_packed = (root.clearcoat_texture == 0u ? float4(0.0f) : g_textures[root.clearcoat_texture].Load(int3(pixel, 0)));
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

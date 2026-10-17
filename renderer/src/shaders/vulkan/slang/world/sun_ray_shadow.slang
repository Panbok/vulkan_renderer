// Ray-query sun shadows on the desktop pipeline (prototype for the
// shadow-quality proposal's D2). Shadow.SunRay traces one first-hit ray per
// traced pixel toward a point on the sun's disk against the frame slot's TLAS
// (ADR-119): one pixel of each 2x2 block, whose position turns with the
// frame. Shadow.SunRayResolve filters the samples with depth and
// normal weights at full resolution. Shadow.SunRayTemporal accumulates the
// filtered visibility over frames along the camera motion into the mask that
// deferred lighting takes in place of the cascades within the ray range.

// Bit of VkrVkSunRayShadowRoot::flags: the frame slot's TLAS is built.
#define VKR_SUN_RAY_TLAS 1u
// Texture level of an alpha-tested caster's coverage. Shadows show cut-out
// shapes near the camera, so they take a finer level than the probe trace.
#define VKR_SUN_RAY_ALPHA_LEVEL 1.0f
// Trace-texture depth of a pixel that took no ray.
#define VKR_SUN_RAY_NO_SAMPLE -1.0f

struct VkrVkSunRayShadowRoot
{
    VkrVulkanPacketFrameRoot *frame;
    uint2 frame_address_padding;
    float4x4 inverse_view_projection;
    VkrGpuVisibleDrawRow *visible_rows;
    VkrGpuCandidateDrawRow *candidates;
    VkrVkGeometryRow *geometry_rows;
    // The geometry megabuffer's bases: a row's own index pointer
    // miscompiles on the RX 6700 XT (see VkrVkVolumeTraceRoot).
    uint *indices;
    VkrPackedStaticVertex *vertices;
    uint vbuffer_texture;
    uint depth_texture;
    uint normal_texture;
    // RGBA16F, one texel per 2x2 block: the traced visibility, the traced
    // pixel's view depth, or VKR_SUN_RAY_NO_SAMPLE, and its octahedral
    // normal, which the resolve weights by.
    uint trace_texture;
    // RG8: the filtered visibility and the weight lighting gives it over
    // the cascades: Shadow.SunRayResolve's output, Shadow.SunRayTemporal's
    // input.
    uint mask_texture;
    // The frame slot's acceleration structure in g_volume_tlas.
    uint tlas_slot;
    uint2 extent;
    uint2 trace_extent;
    uint flags;
    uint frame_index;
    // View depths, in metres, where the rays' weight starts to fall and
    // where it reaches zero.
    float range_fade_start;
    float range_end;
    float ray_length;
    // Shadow.SunRayTemporal: the camera's motion to the previous frame in UV
    // and its validity (x: motion valid, y: the surface's previous device
    // depth), from the G-buffer resolve.
    uint motion_texture;
    uint validity_texture;
    // RG16F: the accumulated visibility and its surface's device depth, or
    // VKR_SUN_RAY_NO_SAMPLE; last frame's (read) and this frame's (written,
    // then copied over it).
    uint history_texture;
    uint history_next_texture;
    // RG8: the accumulated mask lighting reads.
    uint temporal_texture;
    // Nonzero when the history holds the previous frame's result.
    uint history_valid;
    uint reserved;
};

struct VkrVkSunRayShadowPush
{
    VkrVkSunRayShadowRoot *root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVkSunRayShadowPush> g_sun_ray_shadow_push;

// The full-resolution pixel a trace texel samples this frame: one corner of
// its 2x2 block in a four-frame cycle.
uint2 vk_sun_ray_trace_pixel(VkrVkSunRayShadowRoot root, uint2 texel)
{
    const uint2 corners[4] = {uint2(0u, 0u), uint2(1u, 1u), uint2(1u, 0u),
                              uint2(0u, 1u)};
    return texel * 2u + corners[root.frame_index & 3u];
}

VkrVkDepthView vk_sun_ray_depth_view(VkrVkSunRayShadowRoot root)
{
    VkrVkDepthView view;
    view.vbuffer_texture = root.vbuffer_texture;
    view.depth_texture = root.depth_texture;
    view.extent = root.extent;
    view.inverse_view_projection = root.inverse_view_projection;
    view.sample_position = 0.5f;
    return view;
}

// Whether a ray from `origin` along `direction` leaves the scene within
// `length`. Alpha-tested casters block where their coverage passes the
// material's cutoff.
bool vk_sun_ray_visible(VkrVkSunRayShadowRoot root,
                        RaytracingAccelerationStructure scene, float3 origin,
                        float3 direction, float length)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = length;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>
        query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFFu, ray);
    VkrVulkanPacketFrameRoot *frame = root.frame;
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        VkrGpuCandidateDrawRow candidate =
            root.candidates[query.CandidateInstanceID()];
        VkrVulkanMaterial material = frame.materials[candidate.material_index];
        float2 uv = vk_ray_triangle_uv(
            root.indices, root.vertices, candidate,
            root.geometry_rows[candidate.geometry_index],
            query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics());
        float alpha =
            g_textures[NonUniformResourceIndex(material.base_color_texture)]
                .SampleLevel(g_samplers[NonUniformResourceIndex(
                                 material.base_color_sampler)],
                             uv, VKR_SUN_RAY_ALPHA_LEVEL)
                .a *
            material.tint.a;
        if (alpha >= material.material_alpha.x)
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_NOTHING;
}

// A direction toward a point on the sun's disk: the disk's angular radius
// gives the penumbra the cascades' PCSS estimates. The pixel's noise turns
// with the frame so temporal antialiasing averages the samples.
float3 vk_sun_ray_direction(float3 sun, float tan_half_angle, uint2 pixel,
                            uint frame_index)
{
    float2 rotation = vkr_shadow_kernel_rotation(pixel, frame_index);
    float2 radius_noise = vkr_shadow_kernel_rotation(pixel.yx + 17u,
                                                     frame_index + 7u);
    float radius = sqrt(radius_noise.x * 0.5f + 0.5f) * tan_half_angle;
    float3 tangent = abs(sun.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f)
                                        : float3(1.0f, 0.0f, 0.0f);
    tangent = normalize(cross(tangent, sun));
    float3 bitangent = cross(sun, tangent);
    return normalize(sun + (tangent * rotation.x + bitangent * rotation.y) *
                               radius);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_sun_ray_shadow_trace(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSunRayShadowRoot root = vkr_vk_uniform_root(*g_sun_ray_shadow_push.root);
    if (any(thread_id.xy >= root.trace_extent))
        return;
    uint2 pixel = min(vk_sun_ray_trace_pixel(root, thread_id.xy),
                      root.extent - 1u);
    float4 result = float4(1.0f, VKR_SUN_RAY_NO_SAMPLE, 0.0f, 0.0f);
    VkrVulkanPacketFrameRoot *frame = root.frame;
    uint visible = g_storage_uint2_2d[root.vbuffer_texture][pixel].x;
    if (visible != 0u && (root.flags & VKR_SUN_RAY_TLAS) != 0u &&
        frame.directional_direction_enabled.w > 0.5f &&
        frame.shadow_cascade_count != 0u)
    {
        float depth = g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
        float3 world =
            vkr_vk_depth_view_world(vk_sun_ray_depth_view(root), pixel, depth);
        float view_depth = abs(mul(frame.view, float4(world, 1.0f)).z);
        if (view_depth < root.range_end)
        {
            float2 encoded_normal =
                g_storage_float2_2d[root.normal_texture][pixel];
            float3 normal = vkr_vk_octahedral_decode(encoded_normal);
            float3 sun = normalize(-frame.directional_direction_enabled.xyz);
            // A surface facing away from the sun takes no direct light
            // unless it transmits some, which lighting shadows from the
            // other side (packet_directional_shadow with the flipped
            // normal).
            bool facing = dot(normal, sun) > 0.0f;
            bool transmits =
                !facing &&
                frame.materials[root.visible_rows[visible - 1u].material_index]
                        .material_diffuse_transmission.w > 0.0f;
            float visibility = 0.0f;
            if (facing || transmits)
            {
                // The offset grows with distance, as the reconstructed
                // position's error does.
                float camera_distance =
                    distance(world, frame.view_position.xyz);
                // The origin leaves the geometric surface: along the
                // shading normal it could start inside normal-map relief.
                float3 geometric_normal = vkr_vk_depth_view_geometric_normal(
                    vk_sun_ray_depth_view(root), pixel, visible, depth, world, normal,
                    packet_view_direction(frame, world));
                float3 origin =
                    world + (facing ? geometric_normal : -geometric_normal) *
                                (0.01f + 0.002f * camera_distance);
                float3 direction = vk_sun_ray_direction(
                    sun, frame.shadow_cascades[0].origin_inv_size_sun.w,
                    pixel, root.frame_index);
                RaytracingAccelerationStructure scene =
                    g_volume_tlas[root.tlas_slot];
                visibility = vk_sun_ray_visible(root, scene, origin, direction,
                                                root.ray_length)
                                 ? 1.0f
                                 : 0.0f;
            }
            result = float4(visibility, view_depth, encoded_normal);
        }
    }
    g_storage_2d[root.trace_texture][thread_id.xy] = result;
}

// Samples within this many trace texels of a pixel's own texel filter it.
#define VKR_SUN_RAY_RESOLVE_RADIUS 1
// Full-resolution pixels of one standard deviation of the spatial weight.
#define VKR_SUN_RAY_RESOLVE_SIGMA 1.75f

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_sun_ray_shadow_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSunRayShadowRoot root = vkr_vk_uniform_root(*g_sun_ray_shadow_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    uint2 pixel = thread_id.xy;
    float2 result = float2(1.0f, 0.0f);
    VkrVulkanPacketFrameRoot *frame = root.frame;
    if (g_storage_uint2_2d[root.vbuffer_texture][pixel].x != 0u)
    {
        float depth = g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
        float3 world =
            vkr_vk_depth_view_world(vk_sun_ray_depth_view(root), pixel, depth);
        float view_depth = abs(mul(frame.view, float4(world, 1.0f)).z);
        float range_weight = saturate(
            (root.range_end - view_depth) /
            max(root.range_end - root.range_fade_start, 1.0e-3f));
        if (range_weight > 0.0f)
        {
            float3 normal = vkr_vk_octahedral_decode(
                g_storage_float2_2d[root.normal_texture][pixel]);
            int2 center = int2(pixel / 2u);
            // Depths within this relative distance of the pixel's count as
            // its surface.
            float depth_tolerance = 0.02f * view_depth + 0.02f;
            float total = 0.0f;
            float weight_sum = 0.0f;
            for (int y = -VKR_SUN_RAY_RESOLVE_RADIUS;
                 y <= VKR_SUN_RAY_RESOLVE_RADIUS; ++y)
            {
                for (int x = -VKR_SUN_RAY_RESOLVE_RADIUS;
                     x <= VKR_SUN_RAY_RESOLVE_RADIUS; ++x)
                {
                    int2 texel = center + int2(x, y);
                    if (any(texel < 0) || any(texel >= int2(root.trace_extent)))
                        continue;
                    float4 traced =
                        g_storage_2d[root.trace_texture][uint2(texel)];
                    if (traced.y < 0.0f)
                        continue;
                    uint2 source = min(
                        vk_sun_ray_trace_pixel(root, uint2(texel)),
                        root.extent - 1u);
                    float2 offset = float2(int2(source) - int2(pixel));
                    float spatial = exp(-dot(offset, offset) /
                                        (2.0f * VKR_SUN_RAY_RESOLVE_SIGMA *
                                         VKR_SUN_RAY_RESOLVE_SIGMA));
                    float depth_weight = saturate(
                        1.0f - abs(traced.y - view_depth) / depth_tolerance);
                    float3 source_normal =
                        vkr_vk_octahedral_decode(traced.zw);
                    float normal_weight =
                        pow(saturate(dot(normal, source_normal)), 8.0f);
                    float weight = spatial * depth_weight * normal_weight;
                    total += traced.x * weight;
                    weight_sum += weight;
                }
            }
            // Without a sample on its surface the pixel keeps the cascades.
            if (weight_sum > 1.0e-3f)
                result = float2(total / weight_sum, range_weight);
        }
    }
    g_storage_float2_2d[root.mask_texture][pixel] = result;
}

// Weight of the current frame's filtered visibility against the history: an
// exponential average over about ten frames.
#define VKR_SUN_RAY_TEMPORAL_ALPHA 0.1f
// The history may lie this many standard deviations of the current
// neighbourhood from its mean before it is clipped.
#define VKR_SUN_RAY_TEMPORAL_CLIP 1.5f
// A history texel lies on the same surface when its device depth is within
// this fraction of the depth the surface had: reversed depth falls as one
// over view depth, so this is about the same fraction of view depth.
#define VKR_SUN_RAY_TEMPORAL_DEPTH_TOLERANCE 0.02f

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_sun_ray_shadow_temporal(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSunRayShadowRoot root = vkr_vk_uniform_root(*g_sun_ray_shadow_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    uint2 pixel = thread_id.xy;
    float2 current = g_storage_float2_2d[root.mask_texture][pixel];
    float stored_depth = VKR_SUN_RAY_NO_SAMPLE;
    float visibility = current.x;
    if (current.y > 0.0f)
    {
        stored_depth =
            g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
        // The current neighbourhood's mean and spread bound the history,
        // so a moved shadow edge does not trail.
        float sum = 0.0f;
        float sum_squared = 0.0f;
        float count = 0.0f;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                int2 neighbour = int2(pixel) + int2(x, y);
                if (any(neighbour < 0) || any(neighbour >= int2(root.extent)))
                    continue;
                float2 value =
                    g_storage_float2_2d[root.mask_texture][uint2(neighbour)];
                if (value.y <= 0.0f)
                    continue;
                sum += value.x;
                sum_squared += value.x * value.x;
                count += 1.0f;
            }
        }
        float mean = sum / max(count, 1.0f);
        float deviation =
            sqrt(max(sum_squared / max(count, 1.0f) - mean * mean, 0.0f));
        float2 validity = g_storage_float2_2d[root.validity_texture][pixel];
        if (root.history_valid != 0u && validity.x > 0.5f)
        {
            // Bilinear history at the previous position, from the taps on
            // the same surface: their depth matches the one this surface had
            // there.
            float2 motion = g_storage_float2_2d[root.motion_texture][pixel];
            float2 previous =
                (float2(pixel) + 0.5f) + motion * float2(root.extent) - 0.5f;
            int2 base = int2(floor(previous));
            float2 fraction = previous - float2(base);
            float expected = validity.y;
            float tolerance = VKR_SUN_RAY_TEMPORAL_DEPTH_TOLERANCE * expected;
            float history = 0.0f;
            float history_weight = 0.0f;
            [unroll] for (uint tap = 0u; tap < 4u; ++tap)
            {
                int2 offset = int2(int(tap & 1u), int(tap >> 1u));
                int2 texel = base + offset;
                if (any(texel < 0) || any(texel >= int2(root.extent)))
                    continue;
                float2 stored =
                    g_storage_float2_2d[root.history_texture][uint2(texel)];
                if (stored.y < 0.0f || abs(stored.y - expected) > tolerance)
                    continue;
                float2 bilinear = lerp(1.0f - fraction, fraction,
                                       float2(offset));
                float weight = bilinear.x * bilinear.y;
                history += stored.x * weight;
                history_weight += weight;
            }
            if (history_weight > 1.0e-3f)
            {
                history /= history_weight;
                history = clamp(history,
                                mean - VKR_SUN_RAY_TEMPORAL_CLIP * deviation,
                                mean + VKR_SUN_RAY_TEMPORAL_CLIP * deviation);
                visibility =
                    lerp(history, current.x, VKR_SUN_RAY_TEMPORAL_ALPHA);
            }
        }
    }
    g_storage_float2_2d[root.temporal_texture][pixel] =
        float2(visibility, current.y);
    g_storage_float2_2d[root.history_next_texture][pixel] =
        float2(visibility, stored_depth);
}

// Dynamic diffuse volume layer (ADR-119): ray-query updates of the sparse
// volume's probes on the desktop pipeline. A selected probe traces a rotated
// spherical Fibonacci set; a hit reflects the light the bake lacks (signed
// light sources, emissive movers and the layer's own bounce) and the probe
// blends the result, as L1 SH of E/pi, into the layer's texture. The bricks
// and visibility stay the bake's; history lives per probe, so nothing
// reprojects.

// Each frame slot's top-level acceleration structure; an instance's custom
// index is its packed candidate row.
[[vk::binding(2, 0)]] RaytracingAccelerationStructure g_volume_tlas[4];

struct VkrVkVolumeTraceRoot
{
    // The frame slot's acceleration structure in g_volume_tlas.
    uint tlas_slot;
    uint tlas_reserved;
    VkrGpuCandidateDrawRow *candidates;
    VkrVulkanInstance *instances;
    VkrVkGeometryRow *geometry_rows;
    VkrVulkanMaterial *materials;
    uint *trace_probes;
    uint *clear_probes;
    // Signed, physical light rows: a negative intensity takes a bake-time
    // light away.
    VkrGpuPointLightRow *lights;
    VkrGpuRectangleLightRow *rectangle_lights;
    // The geometry megabuffer's bases: a row's own index pointer
    // miscompiles on the RX 6700 XT, so surfaces read through these, as the
    // G-buffer resolve does.
    uint *indices;
    VkrPackedStaticVertex *vertices;
    uint indirection_texture;
    uint probe_texture;
    uint moment_texture;
    uint moment_sampler;
    uint bricks_texture;
    uint dynamic_storage;
    uint trace_count;
    uint clear_count;
    uint light_count;
    uint rectangle_light_count;
    // Packed rows from here on are dynamic; hits on them add their emission.
    uint static_candidate_count;
    uint sequence;
    uint2 reserved0;
    float4 origin_spacing;
    // xyz: indirection dimensions; w: the moment atlas's tile rows.
    uint4 dimensions;
    float blend;
    float ray_length;
    // VKR_VOLUME_TRACE_GRID for a runtime grid, which traces all of its
    // light and owns its probe records and moments;
    // VKR_VOLUME_TRACE_DIRECT_BANDS for a bake with lamp direct bands.
    uint flags;
    uint probe_storage;
    uint moment_storage;
    // The environment's diffuse response missed grid rays see:
    // sky_sh[sky_slot] times sky_scale.
    uint sky_slot;
    float sky_scale;
    // A grid's moments clamp at, and count a miss as, this distance.
    float moment_clamp;
    // xyz: unit direction toward the sun; w: one when it shines on a grid.
    float4 sun_direction;
    float4 sun_radiance;
    VkrShL2Packed *sky_sh;
    // The frame's IBL probe rows, whose capture origins the relight reads,
    // and the factors it writes, one per row.
    VkrVulkanIblProbe *ibl_probes;
    float *probe_relight;
    uint ibl_probe_count;
    uint reserved1;
};
#define VKR_VOLUME_TRACE_GRID 1u
// The bake's lamp groups keep their direct light in bands, which a probe's
// baked light holds (ADR-054).
#define VKR_VOLUME_TRACE_DIRECT_BANDS 2u

struct VkrVkVolumeTracePush
{
    VkrVkVolumeTraceRoot *root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVkVolumeTracePush> g_volume_trace_push;

// One BLAS's positions: a submesh's level-0 triangles, unindexed, in the
// geometry's local space.
struct VkrVkVolumeDecodeRoot
{
    VkrVkGeometryRow *geometry_rows;
    float *positions;
    // The geometry megabuffer's bases, as in VkrVkVolumeTraceRoot.
    uint *indices;
    VkrPackedStaticVertex *vertices;
    uint geometry_index;
    uint first_index;
    uint index_count;
    int vertex_offset;
    uint decode_index;
    uint reserved;
};

struct VkrVkVolumeDecodePush
{
    VkrVkVolumeDecodeRoot *root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVkVolumeDecodePush> g_volume_decode_push;

#define VKR_VOLUME_TRACE_RAYS 128u
#define VKR_VOLUME_TRACE_WAVES (VKR_VOLUME_TRACE_RAYS / 32u)
// Rays leave the probe and shadow rays the surface this far, in metres.
#define VKR_VOLUME_TRACE_OFFSET 0.02f
// Texture level of the hit's albedo and alpha: probes see surfaces from a
// metre or more, so the coarse level suffices.
#define VKR_VOLUME_TRACE_TEXTURE_LEVEL 4.0f
// Upper bound of one ray's radiance, so a ray grazing an emitter cannot
// flash a probe.
#define VKR_VOLUME_TRACE_RADIANCE_MAX 64.0f

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_volume_blas_decode(uint3 id : SV_DispatchThreadID)
{
    VkrVkVolumeDecodeRoot *root = g_volume_decode_push.root;
    if (id.x >= root.index_count)
        return;
    VkrVkGeometryRow geometry = root.geometry_rows[root.geometry_index];
    uint index = uint(int(root.indices[root.first_index + id.x]) +
                      root.vertex_offset);
    VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
        root.vertices[geometry.first_vertex + index],
        geometry.decode[root.decode_index]);
    root.positions[id.x * 3u + 0u] = vertex.position.x;
    root.positions[id.x * 3u + 1u] = vertex.position.y;
    root.positions[id.x * 3u + 2u] = vertex.position.z;
}

// The lattice position of probe `probe`, with its relocation, and whether
// the bake kept it.
bool vk_volume_probe_position(VkrVkVolumeTraceRoot *root, uint probe,
                              out float3 position)
{
    uint brick = probe / 64u;
    uint local = probe % 64u;
    uint packed = g_uint_textures[root.bricks_texture].Load(
        int3(int(brick % 1024u), int(brick / 1024u), 0));
    uint3 entry = uint3(packed & 1023u, (packed >> 10u) & 1023u,
                        (packed >> 20u) & 1023u);
    uint level = packed >> 30u;
    uint scale = level == 0u ? 1u : (level == 1u ? 3u : 9u);
    uint3 lattice = uint3(local % 4u, (local / 4u) % 4u, local / 16u);
    float4 record = g_textures[root.probe_texture].Load(
        int3(int2(vkr_diffuse_volume_probe_texel(probe)), 0));
    position = root.origin_spacing.xyz +
               root.origin_spacing.w *
                   (3.0f * float3(entry) + float(scale) * float3(lattice)) +
               record.xyz;
    return record.w > 0.0f;
}

// Direction `index` of `count` on a spherical Fibonacci spiral.
float3 vk_volume_fibonacci(uint index, uint count)
{
    float phi = 6.28318531f * frac(float(index) * 0.618033989f);
    float cos_theta = 1.0f - (2.0f * float(index) + 1.0f) / float(count);
    float sin_theta = sqrt(saturate(1.0f - cos_theta * cos_theta));
    return float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

// A rotation that changes every frame, so successive updates of a probe
// cover the sphere between the set's directions.
float3x3 vk_volume_rotation(uint sequence)
{
    float a = 6.28318531f * frac(float(sequence) * 0.618033989f);
    float b = 6.28318531f * frac(float(sequence) * 0.754877666f);
    float c = acos(1.0f - 2.0f * frac(float(sequence) * 0.569840291f));
    float ca = cos(a);
    float sa = sin(a);
    float cb = cos(b);
    float sb = sin(b);
    float cc = cos(c);
    float sc = sin(c);
    float3x3 z0 = float3x3(ca, -sa, 0.0f, sa, ca, 0.0f, 0.0f, 0.0f, 1.0f);
    float3x3 y = float3x3(cc, 0.0f, sc, 0.0f, 1.0f, 0.0f, -sc, 0.0f, cc);
    float3x3 z1 = float3x3(cb, -sb, 0.0f, sb, cb, 0.0f, 0.0f, 0.0f, 1.0f);
    return mul(z1, mul(y, z0));
}

// The candidate row's surface at a triangle's barycentrics: world position,
// geometric normal facing `incoming`'s origin, and texture coordinate.
struct VkrVkVolumeSurface
{
    float3 position;
    float3 normal;
    float2 uv;
};

// One corner of a traced triangle of a packed candidate row, read through
// the geometry megabuffer's bases.
VkrGpuDecodedVertex vk_ray_triangle_vertex(uint *indices,
                                           VkrPackedStaticVertex *vertices,
                                           VkrGpuCandidateDrawRow candidate,
                                           VkrVkGeometryRow geometry,
                                           VkrGpuGeometryDecodeRecord decode,
                                           uint primitive, uint corner)
{
    uint index = uint(int(indices[candidate.first_index + primitive * 3u +
                                  corner]) +
                      candidate.vertex_offset);
    return vkr_decode_packed_vertex(vertices[geometry.first_vertex + index],
                                    decode);
}

// The texture coordinate at a traced triangle's barycentrics.
float2 vk_ray_triangle_uv(uint *indices, VkrPackedStaticVertex *vertices,
                          VkrGpuCandidateDrawRow candidate,
                          VkrVkGeometryRow geometry, uint primitive,
                          float2 barycentrics)
{
    VkrGpuGeometryDecodeRecord decode = geometry.decode[candidate.decode_index];
    float2 uvs[3];
    [unroll] for (uint corner = 0u; corner < 3u; ++corner)
    {
        uvs[corner] = vk_ray_triangle_vertex(indices, vertices, candidate,
                                             geometry, decode, primitive,
                                             corner)
                          .texcoord;
    }
    return uvs[0] * (1.0f - barycentrics.x - barycentrics.y) +
           uvs[1] * barycentrics.x + uvs[2] * barycentrics.y;
}

VkrVkVolumeSurface vk_volume_surface(VkrVkVolumeTraceRoot *root, uint row,
                                     uint primitive, float2 barycentrics,
                                     float3 incoming)
{
    VkrGpuCandidateDrawRow candidate = root.candidates[row];
    VkrVkGeometryRow geometry = root.geometry_rows[candidate.geometry_index];
    VkrVulkanInstance instance = root.instances[candidate.instance_index];
    VkrGpuGeometryDecodeRecord decode = geometry.decode[candidate.decode_index];
    float3 positions[3];
    float2 uvs[3];
    [unroll] for (uint corner = 0u; corner < 3u; ++corner)
    {
        VkrGpuDecodedVertex vertex = vk_ray_triangle_vertex(
            root.indices, root.vertices, candidate, geometry, decode,
            primitive, corner);
        positions[corner] =
            mul(instance.model, float4(vertex.position, 1.0f)).xyz;
        uvs[corner] = vertex.texcoord;
    }
    float3 weights = float3(1.0f - barycentrics.x - barycentrics.y,
                            barycentrics.x, barycentrics.y);
    VkrVkVolumeSurface surface;
    surface.position = positions[0] * weights.x + positions[1] * weights.y +
                       positions[2] * weights.z;
    surface.uv = uvs[0] * weights.x + uvs[1] * weights.y + uvs[2] * weights.z;
    float3 normal = cross(positions[1] - positions[0],
                          positions[2] - positions[0]);
    normal *= rsqrt(max(dot(normal, normal), 1.0e-20f));
    surface.normal = dot(normal, incoming) > 0.0f ? -normal : normal;
    return surface;
}

float4 vk_volume_base_color(VkrVulkanMaterial material, float2 uv)
{
    return g_textures[NonUniformResourceIndex(material.base_color_texture)]
               .SampleLevel(g_samplers[NonUniformResourceIndex(
                                material.base_color_sampler)],
                            uv, VKR_VOLUME_TRACE_TEXTURE_LEVEL) *
           material.tint;
}

// Whether a segment from `origin` along `direction` reaches `length`
// unblocked. Shadows treat alpha-tested surfaces as solid.
bool vk_volume_visible(RaytracingAccelerationStructure scene, float3 origin,
                       float3 direction, float length)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = max(length - VKR_VOLUME_TRACE_OFFSET, 0.0f);
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>
        query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFFu, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_NOTHING;
}

// The layer's own E/pi at a surface, for its next bounce: the eight corner
// probes weighed as the lighting lookup weighs them.
float3 vk_volume_dynamic_response(VkrVkVolumeTraceRoot *root, float3 position,
                                  float3 normal)
{
    float3 origin = root.origin_spacing.xyz;
    float spacing = root.origin_spacing.w;
    uint3 dimensions = root.dimensions.xyz;
    uint3 entry = vkr_diffuse_volume_entry(position, origin, spacing,
                                           dimensions);
    if (!vkr_diffuse_volume_entry_inside(entry, dimensions))
        return 0.0f;
    uint value = g_uint_textures[root.indirection_texture].Load(
        int3(int2(vkr_diffuse_volume_entry_texel(entry, dimensions)), 0));
    if (value == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY)
        return 0.0f;
    float bias = VKR_DIFFUSE_VOLUME_BIAS *
                 vkr_diffuse_volume_entry_step(value, spacing);
    float3 biased = position + normal * bias;
    VkrDiffuseVolumeCell cell = vkr_diffuse_volume_brick_cell(
        value, entry, biased, origin, spacing);
    SamplerState moment_sampler = g_samplers[root.moment_sampler];
    float4 red = 0.0f;
    float4 green = 0.0f;
    float4 blue = 0.0f;
    float total = 0.0f;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        uint probe = vkr_diffuse_volume_corner_probe(cell, corner);
        float4 record = g_textures[root.probe_texture].Load(
            int3(int2(vkr_diffuse_volume_probe_texel(probe)), 0));
        if (record.w <= 0.0f)
            continue;
        float3 probe_position =
            vkr_diffuse_volume_corner_position(cell, corner) + record.xyz;
        float3 to_probe = probe_position - position;
        float weight = vkr_diffuse_volume_backface(
            normal, to_probe * rsqrt(max(dot(to_probe, to_probe), 1.0e-8f)));
        float3 from_probe = biased - probe_position;
        float distance = length(from_probe);
        float2 moments = g_textures[root.moment_texture].SampleLevel(
            moment_sampler,
            vkr_diffuse_volume_moment_uv(probe, from_probe / max(distance, 1.0e-6f),
                                         float(root.dimensions.w)),
            0.0f).xy;
        weight *= vkr_diffuse_volume_chebyshev(moments, distance);
        weight = vkr_diffuse_volume_crush(weight) *
                 vkr_diffuse_volume_trilinear(corner, cell.fraction);
        red += weight * g_storage_2d[root.dynamic_storage][
            vkr_diffuse_volume_sh_texel(probe, 0u, 0u)];
        green += weight * g_storage_2d[root.dynamic_storage][
            vkr_diffuse_volume_sh_texel(probe, 1u, 0u)];
        blue += weight * g_storage_2d[root.dynamic_storage][
            vkr_diffuse_volume_sh_texel(probe, 2u, 0u)];
        total += weight;
    }
    if (total < VKR_DIFFUSE_VOLUME_MIN_WEIGHT)
        return 0.0f;
    return vkr_diffuse_volume_l1(red, green, blue, normal) / total;
}

// Irradiance the light sources give a surface, each signed and shadowed.
float3 vk_volume_direct_irradiance(VkrVkVolumeTraceRoot *root,
                                   RaytracingAccelerationStructure scene,
                                   float3 position, float3 normal)
{
    float3 irradiance = 0.0f;
    float3 origin = position + normal * VKR_VOLUME_TRACE_OFFSET;
    for (uint i = 0u; i < root.light_count; ++i)
    {
        VkrGpuPointLightRow light = root.lights[i];
        VkrPunctualLightTerm term = vkr_punctual_light_term(
            light.p0, light.p1, light.p2, light.p3, position);
        float n_dot_l = dot(normal, term.direction);
        if (!term.in_range || term.cone <= 0.0f || n_dot_l <= 0.0f)
            continue;
        float distance = length(light.p0.xyz - position);
        if (!vk_volume_visible(scene, origin, term.direction, distance))
            continue;
        irradiance += light.p1.rgb * light.p2.x * term.attenuation *
                      term.cone * n_dot_l;
    }
    for (uint i = 0u; i < root.rectangle_light_count; ++i)
    {
        // The rectangle as a point at its centre with its area's form
        // factor: a probe's coarse light needs no exact integral.
        VkrGpuRectangleLightRow light = root.rectangle_lights[i];
        float3 to_light = light.center_half_width.xyz - position;
        float distance_squared = max(dot(to_light, to_light), 1.0e-4f);
        float distance = sqrt(distance_squared);
        float3 direction = to_light / distance;
        float n_dot_l = dot(normal, direction);
        float3 emitter_normal = normalize(cross(light.right_half_height.xyz,
                                                light.up_radiance.xyz));
        float emitter_cosine = abs(dot(emitter_normal, direction));
        if (n_dot_l <= 0.0f || emitter_cosine <= 0.0f)
            continue;
        float area = 4.0f * light.center_half_width.w *
                     light.right_half_height.w;
        if (!vk_volume_visible(scene, origin, direction, distance))
            continue;
        irradiance += light.color.rgb * light.up_radiance.w * area *
                      emitter_cosine * n_dot_l /
                      max(distance_squared, area * 0.31830989f);
    }
    return irradiance;
}

// A runtime grid's probe update (ADR-119), the bake's rules at run time:
// moments are the weighted mean and mean square of each ray's distance,
// clamped, toward each interior texel; a probe seeing more back faces than
// the fraction moves through the closest one, one closer to a front face
// than the clearance moves away from it, within the limit.
#define VKR_VOLUME_GRID_MOMENT_SHARPNESS 50.0f
#define VKR_VOLUME_GRID_INSIDE_FRACTION 0.25f
#define VKR_VOLUME_GRID_CLEARANCE 0.1f
#define VKR_VOLUME_GRID_OFFSET_LIMIT 0.45f
// The farthest occluder a grid's sun shadow ray finds, in metres.
#define VKR_VOLUME_GRID_SUN_LENGTH 1000.0f

groupshared float4 g_volume_sums[VKR_VOLUME_TRACE_WAVES][3];
// A grid probe's rays: direction and clamped distance, and whether each hit
// a back face.
groupshared float4 g_volume_rays[VKR_VOLUME_TRACE_RAYS];
groupshared uint g_volume_backfaces[VKR_VOLUME_TRACE_RAYS];
groupshared float2 g_volume_moments[VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR *
                                    VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR];

// The direction of octahedral coordinate `e`, the inverse of
// vkr_diffuse_volume_octahedral.
float3 vk_volume_octahedral_direction(float2 e)
{
    float3 direction = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (direction.z < 0.0f)
        direction.xy = float2((1.0f - abs(e.y)) * (e.x >= 0.0f ? 1.0f : -1.0f),
                              (1.0f - abs(e.x)) * (e.y >= 0.0f ? 1.0f : -1.0f));
    return normalize(direction);
}

// The atlas texel of interior texel `local` (0 to 9 with the border) of a
// probe's moment tile.
uint2 vk_volume_moment_texel(uint probe, uint2 local)
{
    return uint2(probe % VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES,
                 probe / VKR_DIFFUSE_VOLUME_MOMENT_ROW_PROBES) *
               VKR_DIFFUSE_VOLUME_MOMENT_SIZE +
           local;
}

// The interior texel a border texel of a moment tile repeats: across each
// octahedral seam, as the bake lays it out.
uint2 vk_volume_moment_border_source(uint2 border)
{
    const uint last = VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR + 1u;
    uint2 source = border;
    if ((border.x == 0u || border.x == last) &&
        (border.y == 0u || border.y == last))
        return uint2(border.x == 0u ? last - 1u : 1u,
                     border.y == 0u ? last - 1u : 1u);
    if (border.y == 0u || border.y == last)
    {
        source.x = last - border.x;
        source.y = border.y == 0u ? 1u : last - 1u;
    }
    else
    {
        source.y = last - border.y;
        source.x = border.x == 0u ? 1u : last - 1u;
    }
    return source;
}

// Updates a grid probe's moments and relocation from its rays in
// g_volume_rays; every thread of the group calls it.
void vk_volume_grid_update(VkrVkVolumeTraceRoot *root, uint probe, uint thread)
{
    const uint interior = VKR_DIFFUSE_VOLUME_MOMENT_INTERIOR;
    if (thread < interior * interior)
    {
        uint2 local = uint2(thread % interior, thread / interior);
        float2 e = (float2(local) + 0.5f) / float(interior) * 2.0f - 1.0f;
        float3 texel_direction = vk_volume_octahedral_direction(e);
        float2 sum = 0.0f;
        float total = 0.0f;
        for (uint ray = 0u; ray < VKR_VOLUME_TRACE_RAYS; ++ray)
        {
            float4 traced = g_volume_rays[ray];
            float weight = pow(saturate(dot(texel_direction, traced.xyz)),
                               VKR_VOLUME_GRID_MOMENT_SHARPNESS);
            sum += weight * float2(traced.w, traced.w * traced.w);
            total += weight;
        }
        uint2 texel = vk_volume_moment_texel(probe, local + 1u);
        float2 stored = g_storage_float2_2d[root.moment_storage][texel];
        float2 moments = total > 1.0e-6f
                             ? lerp(stored, sum / total, root.blend)
                             : stored;
        g_volume_moments[thread] = moments;
        g_storage_float2_2d[root.moment_storage][texel] = moments;
    }
    GroupMemoryBarrierWithGroupSync();
    // The tile's 36 border texels repeat the interior across its seams.
    const uint border_count = 4u * (interior + 1u);
    if (thread < border_count)
    {
        uint side = thread / (interior + 1u);
        uint step = thread % (interior + 1u);
        const uint last = interior + 1u;
        uint2 border = side == 0u ? uint2(step, 0u)
                     : side == 1u ? uint2(last, step)
                     : side == 2u ? uint2(last - step, last)
                                  : uint2(0u, last - step);
        uint2 source = vk_volume_moment_border_source(border) - 1u;
        g_storage_float2_2d[root.moment_storage][vk_volume_moment_texel(
            probe, border)] = g_volume_moments[source.x + source.y * interior];
    }
    if (thread != 0u)
        return;
    uint backfaces = 0u;
    float closest_back = 1.0e30f;
    float3 closest_back_direction = 0.0f;
    float closest_front = 1.0e30f;
    float3 closest_front_direction = 0.0f;
    for (uint ray = 0u; ray < VKR_VOLUME_TRACE_RAYS; ++ray)
    {
        float4 traced = g_volume_rays[ray];
        if (g_volume_backfaces[ray] != 0u)
        {
            backfaces += 1u;
            if (traced.w < closest_back)
            {
                closest_back = traced.w;
                closest_back_direction = traced.xyz;
            }
        }
        else if (traced.w < closest_front)
        {
            closest_front = traced.w;
            closest_front_direction = traced.xyz;
        }
    }
    float spacing = root.origin_spacing.w;
    float clearance = VKR_VOLUME_GRID_CLEARANCE * spacing;
    uint2 probe_texel = uint2(int2(vkr_diffuse_volume_probe_texel(probe)));
    float4 record = g_storage_2d[root.probe_storage][probe_texel];
    bool inside = float(backfaces) >
                  VKR_VOLUME_GRID_INSIDE_FRACTION * float(VKR_VOLUME_TRACE_RAYS);
    float3 offset = record.xyz;
    if (inside)
        offset += closest_back_direction * (closest_back + clearance);
    else if (closest_front < clearance)
        offset -= closest_front_direction * (clearance - closest_front);
    float limit = VKR_VOLUME_GRID_OFFSET_LIMIT * spacing;
    offset = clamp(offset, -limit, limit);
    g_storage_2d[root.probe_storage][probe_texel] =
        float4(offset, inside ? 0.0f : 1.0f);
}

// The environment's diffuse response toward `direction`: a grid's missed
// rays see the sky through it, blurred like a cosine lobe.
float3 vk_volume_sky(VkrVkVolumeTraceRoot *root, float3 direction)
{
    if (root.sky_sh == nullptr)
        return 0.0f;
    return vkr_sh_l2_evaluate(root.sky_sh[root.sky_slot],
                              vkr_sh_l2_prepare_evaluation(direction)) *
           root.sky_scale;
}

// One group per probe, one ray per thread.
[shader("compute")]
[numthreads(128, 1, 1)]
void vk_volume_trace(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    VkrVkVolumeTraceRoot *root = g_volume_trace_push.root;
    if (group.x >= root.trace_count)
        return;
    uint probe = root.trace_probes[group.x];
    float3 probe_position;
    bool valid = vk_volume_probe_position(root, probe, probe_position);
    // A grid traces its invalid probes too, so that they can move out.
    bool grid = (root.flags & VKR_VOLUME_TRACE_GRID) != 0u;
    bool traced = valid || grid;
    RaytracingAccelerationStructure scene = g_volume_tlas[root.tlas_slot];

    float3 direction = mul(vk_volume_rotation(root.sequence),
                           vk_volume_fibonacci(thread.x, VKR_VOLUME_TRACE_RAYS));
    float3 radiance = 0.0f;
    float distance = root.moment_clamp;
    bool backface = false;
    if (traced)
    {
        RayDesc ray;
        ray.Origin = probe_position;
        ray.Direction = direction;
        ray.TMin = VKR_VOLUME_TRACE_OFFSET;
        ray.TMax = root.ray_length;
        RayQuery<RAY_FLAG_NONE> query;
        query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFFu, ray);
        while (query.Proceed())
        {
            // Alpha-tested surfaces let rays through where they are cut out.
            if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
                continue;
            uint row = query.CandidateInstanceID();
            VkrVulkanMaterial material =
                root.materials[root.candidates[row].material_index];
            VkrVkVolumeSurface surface = vk_volume_surface(
                root, row, query.CandidatePrimitiveIndex(),
                query.CandidateTriangleBarycentrics(), direction);
            if (vk_volume_base_color(material, surface.uv).a >=
                material.material_alpha.x)
                query.CommitNonOpaqueTriangleHit();
        }
        if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            uint row = query.CommittedInstanceID();
            distance = min(query.CommittedRayT(), root.moment_clamp);
            // The instance's binding-table offset marks a double-sided
            // material, whose back is no inside.
            backface = !query.CommittedTriangleFrontFace() &&
                       query.CommittedInstanceContributionToHitGroupIndex() == 0u;
            VkrVulkanMaterial material =
                root.materials[root.candidates[row].material_index];
            VkrVkVolumeSurface surface = vk_volume_surface(
                root, row, query.CommittedPrimitiveIndex(),
                query.CommittedTriangleBarycentrics(), direction);
            float3 albedo = vk_volume_base_color(material, surface.uv).rgb;
            float3 irradiance = vk_volume_direct_irradiance(
                root, scene, surface.position, surface.normal);
            // A bake holds the sun; a grid traces it.
            if (grid && root.sun_direction.w > 0.0f)
            {
                float n_dot_l = dot(surface.normal, root.sun_direction.xyz);
                if (n_dot_l > 0.0f &&
                    vk_volume_visible(scene,
                                      surface.position +
                                          surface.normal * VKR_VOLUME_TRACE_OFFSET,
                                      root.sun_direction.xyz,
                                      VKR_VOLUME_GRID_SUN_LENGTH))
                    irradiance += root.sun_radiance.rgb * n_dot_l;
            }
            radiance = albedo * (irradiance * 0.31830989f +
                                 vk_volume_dynamic_response(
                                     root, surface.position, surface.normal));
            // A moving emitter's light is what the bake lacks; a static
            // one's is baked. A grid sets no static rows.
            if (row >= root.static_candidate_count)
            {
                float3 emission = material.material_emissive.rgb;
                if ((material.flags & 4u) != 0u)
                    emission *= g_textures[NonUniformResourceIndex(
                                               material.emissive_texture)]
                                    .SampleLevel(g_samplers[NonUniformResourceIndex(
                                                     material.emissive_sampler)],
                                                 surface.uv,
                                                 VKR_VOLUME_TRACE_TEXTURE_LEVEL)
                                    .rgb;
                radiance += emission;
            }
            radiance = clamp(radiance, -VKR_VOLUME_TRACE_RADIANCE_MAX,
                             VKR_VOLUME_TRACE_RADIANCE_MAX);
        }
        else if (grid)
        {
            radiance = min(vk_volume_sky(root, direction),
                           VKR_VOLUME_TRACE_RADIANCE_MAX);
        }
    }
    if (grid)
    {
        g_volume_rays[thread.x] = float4(direction, distance);
        g_volume_backfaces[thread.x] = backface ? 1u : 0u;
    }

    // L1 SH of E/pi from N uniform rays: the mean radiance, and 2/N times
    // the sum of radiance along each direction (vkr_bake_sh_project_l1).
    float4 red = WaveActiveSum(float4(direction * radiance.r, radiance.r));
    float4 green = WaveActiveSum(float4(direction * radiance.g, radiance.g));
    float4 blue = WaveActiveSum(float4(direction * radiance.b, radiance.b));
    uint wave = thread.x / WaveGetLaneCount();
    if (WaveIsFirstLane())
    {
        g_volume_sums[wave][0] = red;
        g_volume_sums[wave][1] = green;
        g_volume_sums[wave][2] = blue;
    }
    GroupMemoryBarrierWithGroupSync();
    if (grid)
        vk_volume_grid_update(root, probe, thread.x);
    if (thread.x >= 3u)
        return;
    float4 sum = 0.0f;
    uint wave_count = (VKR_VOLUME_TRACE_RAYS + WaveGetLaneCount() - 1u) /
                      WaveGetLaneCount();
    for (uint i = 0u; i < wave_count; ++i)
        sum += g_volume_sums[i][thread.x];
    float inverse = 1.0f / float(VKR_VOLUME_TRACE_RAYS);
    float4 traced_sh = float4(sum.xyz * (2.0f * inverse), sum.w * inverse);
    uint2 texel = vkr_diffuse_volume_sh_texel(probe, thread.x, 0u);
    float4 stored = g_storage_2d[root.dynamic_storage][texel];
    g_storage_2d[root.dynamic_storage][texel] =
        traced ? lerp(stored, traced_sh, root.blend) : float4(0.0f);
}

// One thread per probe whose dynamic light leaves.
[shader("compute")]
[numthreads(64, 1, 1)]
void vk_volume_clear(uint3 id : SV_DispatchThreadID)
{
    VkrVkVolumeTraceRoot *root = g_volume_trace_push.root;
    if (id.x >= root.clear_count)
        return;
    uint probe = root.clear_probes[id.x];
    [unroll] for (uint channel = 0u; channel < 3u; ++channel)
        g_storage_2d[root.dynamic_storage][
            vkr_diffuse_volume_sh_texel(probe, channel, 0u)] = float4(0.0f);
}

// Bounds of a probe's relight factor, as VKR_PROBE_CAPTURE_RATIO_MIN and
// VKR_PROBE_CAPTURE_RATIO_MAX bound its time-of-day ratio.
#define VKR_VOLUME_RELIGHT_MIN (1.0f / 64.0f)
#define VKR_VOLUME_RELIGHT_MAX 16.0f

// The layer's luminance of E/pi at a point, physical: the constant terms of
// the eight corner probes, weighed by validity and trilinear position as
// vkr_diffuse_volume_layer_luminance weighs the bake's layers for a probe's
// capture record.
float vk_volume_dynamic_luminance(VkrVkVolumeTraceRoot *root, float3 position)
{
    float3 origin = root.origin_spacing.xyz;
    float spacing = root.origin_spacing.w;
    uint3 dimensions = root.dimensions.xyz;
    uint3 entry = vkr_diffuse_volume_entry(position, origin, spacing,
                                           dimensions);
    if (!vkr_diffuse_volume_entry_inside(entry, dimensions))
        return 0.0f;
    uint value = g_uint_textures[root.indirection_texture].Load(
        int3(int2(vkr_diffuse_volume_entry_texel(entry, dimensions)), 0));
    if (value == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY)
        return 0.0f;
    VkrDiffuseVolumeCell cell = vkr_diffuse_volume_brick_cell(
        value, entry, position, origin, spacing);
    float3 constant = 0.0f;
    float total = 0.0f;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        uint probe = vkr_diffuse_volume_corner_probe(cell, corner);
        float4 record = g_textures[root.probe_texture].Load(
            int3(int2(vkr_diffuse_volume_probe_texel(probe)), 0));
        float weight = max(record.w, 0.0f) *
                       vkr_diffuse_volume_trilinear(corner, cell.fraction);
        if (weight <= 0.0f)
            continue;
        constant += weight * float3(
            g_storage_2d[root.dynamic_storage][
                vkr_diffuse_volume_sh_texel(probe, 0u, 0u)].w,
            g_storage_2d[root.dynamic_storage][
                vkr_diffuse_volume_sh_texel(probe, 1u, 0u)].w,
            g_storage_2d[root.dynamic_storage][
                vkr_diffuse_volume_sh_texel(probe, 2u, 0u)].w);
        total += weight;
    }
    if (total <= 0.0f)
        return 0.0f;
    return dot(constant / total, float3(0.2126f, 0.7152f, 0.0722f));
}

// The signed light sources' direct E/pi at a point, shadowed, averaged over
// normals as a probe's constant term holds it: a clamped cosine averages to
// a quarter, so a source adds its irradiance at normal incidence over 4 pi.
// The bake's lamp direct bands hold the same measure (ADR-054), which the
// layer, holding only bounce, does not; without them the baked light is
// bounce alone, and so is the layer's.
float vk_volume_direct_luminance(VkrVkVolumeTraceRoot *root,
                                 RaytracingAccelerationStructure scene,
                                 float3 position)
{
    float3 irradiance = 0.0f;
    for (uint i = 0u; i < root.light_count; ++i)
    {
        VkrGpuPointLightRow light = root.lights[i];
        VkrPunctualLightTerm term = vkr_punctual_light_term(
            light.p0, light.p1, light.p2, light.p3, position);
        if (!term.in_range || term.cone <= 0.0f)
            continue;
        float distance = length(light.p0.xyz - position);
        if (!vk_volume_visible(scene, position, term.direction, distance))
            continue;
        irradiance += light.p1.rgb * light.p2.x * term.attenuation * term.cone;
    }
    for (uint i = 0u; i < root.rectangle_light_count; ++i)
    {
        // A point at the rectangle's centre with its area's form factor, as
        // vk_volume_direct_irradiance takes it.
        VkrGpuRectangleLightRow light = root.rectangle_lights[i];
        float3 to_light = light.center_half_width.xyz - position;
        float distance_squared = max(dot(to_light, to_light), 1.0e-4f);
        float distance = sqrt(distance_squared);
        float3 direction = to_light / distance;
        float3 emitter_normal = normalize(cross(light.right_half_height.xyz,
                                                light.up_radiance.xyz));
        float emitter_cosine = abs(dot(emitter_normal, direction));
        if (emitter_cosine <= 0.0f)
            continue;
        float area = 4.0f * light.center_half_width.w *
                     light.right_half_height.w;
        if (!vk_volume_visible(scene, position, direction, distance))
            continue;
        irradiance += light.color.rgb * light.up_radiance.w * area *
                      emitter_cosine /
                      max(distance_squared, area * 0.31830989f);
    }
    return dot(irradiance, float3(0.2126f, 0.7152f, 0.0722f)) *
           (0.25f * 0.31830989f);
}

// One thread per IBL probe row: the factor on a baked probe's brightness
// that adds the layer's light at its capture origin, its bounce and its
// sources' direct light, to the baked light there, so a reflection follows
// a light the bake lacks or lost. A row without baked light keeps its
// brightness.
[shader("compute")]
[numthreads(64, 1, 1)]
void vk_volume_probe_relight(uint3 id : SV_DispatchThreadID)
{
    VkrVkVolumeTraceRoot *root = g_volume_trace_push.root;
    if (id.x >= root.ibl_probe_count)
        return;
    float4 origin_light = root.ibl_probes[id.x].capture_origin;
    float relight = 1.0f;
    if (origin_light.w > 0.0f)
    {
        RaytracingAccelerationStructure scene = g_volume_tlas[root.tlas_slot];
        float dynamic = vk_volume_dynamic_luminance(root, origin_light.xyz);
        if ((root.flags & VKR_VOLUME_TRACE_DIRECT_BANDS) != 0u)
            dynamic += vk_volume_direct_luminance(root, scene,
                                                  origin_light.xyz);
        relight = clamp((origin_light.w + dynamic) / origin_light.w,
                        VKR_VOLUME_RELIGHT_MIN, VKR_VOLUME_RELIGHT_MAX);
    }
    root.probe_relight[id.x] = relight;
}

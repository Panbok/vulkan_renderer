// Lighting.Clusters: the desktop pipeline's frustum clusters of punctual
// lights and decals (ADR-135). The view divides into screen tiles and
// exponential depth slices of clip w. Each cluster marks the light head's
// rows whose bounds reach it in a 128-bit mask, which a wave ORs and walks
// as scalars, and lists the other light rows, then the decal rows, in
// ascending order, so a wave can merge its lanes' lists with WaveActiveMin
// and load each row once.
//
// The prepare dispatch bounds each light and decal by a sphere and a box of
// clusters and clears the counters; the bin dispatch then gives each tile
// one workgroup, which marks the items whose box covers the tile and whose
// sphere reaches its side planes, and lets one thread per slice test them
// against its depth range and write that cluster's mask and list.

// Threads of a bin workgroup; at least the slice count, which the host
// keeps at most 64 (VKR_VULKAN_LIGHT_CLUSTER_SLICE_COUNT).
#define VKR_LIGHT_CLUSTER_BIN_THREADS 64u
// Words of the bin workgroup's item mask:
// VKR_VULKAN_LIGHT_CLUSTER_ITEM_MAX / 32.
#define VKR_LIGHT_CLUSTER_ITEM_WORDS 64u
// The boxes buffer holds VKR_LIGHT_CLUSTER_ITEM_MAX uint2 boxes, then each
// item's world bounding sphere as two uint2 (asuint), radius < 0 for an item
// that reaches every cluster.
#define VKR_LIGHT_CLUSTER_ITEM_MAX 2048u
// A box of clusters packs its minimum and maximum corners in one uint each:
// tile x in bits 0-11, tile y in 12-23, slice in 24-31.
#define VKR_LIGHT_CLUSTER_EMPTY_BOX uint2(0xFFFFFFFFu, 0u)

struct VkrVkLightClusterBuildRoot
{
    VkrVulkanLightClusters *clusters;
    VkrGpuPointLightRow *lights;
    VkrVulkanDecalRow *decals;
    // Per light, then per decal: the packed box of clusters it reaches.
    uint2 *boxes;
    // x: indices allocated, y: references dropped past the pool, z: the
    // longest list; one uint4 that the prepare dispatch clears.
    uint4 *counters;
    uint light_count;
    uint decal_count;
    uint index_capacity;
    uint reserved_0;
    uint2 reserved_1;
};

struct VkrVkLightClusterPush
{
    VkrVkLightClusterBuildRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkLightClusterPush>
    g_light_cluster_push;

uint vkr_vk_light_cluster_pack(uint3 cell)
{
    return cell.x | (cell.y << 12u) | (cell.z << 24u);
}

uint3 vkr_vk_light_cluster_unpack(uint packed)
{
    return uint3(packed & 0xFFFu, (packed >> 12u) & 0xFFFu, packed >> 24u);
}

uint vkr_vk_light_cluster_slice(VkrVulkanLightClusters *clusters, float w)
{
    float slice = floor(log2(max(w, 1.0e-6f)) * clusters.grid_scale.z +
                        clusters.grid_scale.w);
    return uint(clamp(slice, 0.0f, float(clusters.dimensions.z - 1u)));
}

// The cluster that holds `world_position`. A view that does not project
// keeps one cluster with every light and decal.
VkrVulkanLightCluster vkr_vk_light_cluster_header(
    VkrVulkanLightClusters *clusters, float3 world_position)
{
    uint cluster = 0u;
    if (clusters.dimensions.w != 0u)
    {
        float4 clip =
            mul(clusters.view_projection, float4(world_position, 1.0f));
        float2 uv = clip.xy / max(clip.w, 1.0e-6f) * 0.5f + 0.5f;
        float2 last = float2(clusters.dimensions.xy - 1u);
        uint2 tile =
            uint2(clamp(floor(uv * clusters.grid_scale.xy), 0.0f, last));
        uint slice = vkr_vk_light_cluster_slice(clusters, clip.w);
        // A non-finite position (an invalid pixel) converts to any index, so
        // the cluster count bounds it.
        cluster = min(tile.x + clusters.dimensions.x *
                                   (tile.y + clusters.dimensions.y * slice),
                      clusters.dimensions.x * clusters.dimensions.y *
                              clusters.dimensions.z -
                          1u);
    }
    return clusters.headers[cluster];
}

// The light head, VKR_POINT_LIGHT_GRID_LIGHT_MAX: only these first rows of
// the light table can be shadowed or have their contribution counted.
#define VKR_LIGHT_CLUSTER_HEAD_COUNT 128u

// Past the end of a lane's list: WaveActiveMin of every lane's next index
// returns it once the whole wave has walked its lists.
#define VKR_LIGHT_CLUSTER_LIST_END 0xFFFFFFFFu

// The box of clusters that a world-space sphere reaches: the screen
// rectangle of the corners of its axis-aligned box, which bound its
// projection while every corner lies in front of the near plane, and the
// slices of its clip-w range. A sphere wholly behind the near plane reaches
// none.
uint2 vkr_vk_light_cluster_sphere_box(VkrVulkanLightClusters *clusters,
                                      float3 center, float radius,
                                      float near_w)
{
    float4x4 view_projection = clusters.view_projection;
    float4 w_row = float4(view_projection[3]);
    float center_w = dot(w_row, float4(center, 1.0f));
    float w_extent = radius * length(w_row.xyz);
    if (center_w + w_extent < near_w)
        return VKR_LIGHT_CLUSTER_EMPTY_BOX;
    uint3 last = clusters.dimensions.xyz - 1u;
    float2 uv_min = float2(0.0f);
    float2 uv_max = float2(1.0f);
    bool behind = false;
    float2 corner_min = float2(1.0e30f);
    float2 corner_max = float2(-1.0e30f);
    [unroll]
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        float3 offset = float3((corner & 1u) != 0u ? radius : -radius,
                               (corner & 2u) != 0u ? radius : -radius,
                               (corner & 4u) != 0u ? radius : -radius);
        float4 clip = mul(view_projection, float4(center + offset, 1.0f));
        behind = behind || clip.w <= near_w;
        float2 uv = clip.xy / max(clip.w, 1.0e-6f) * 0.5f + 0.5f;
        corner_min = min(corner_min, uv);
        corner_max = max(corner_max, uv);
    }
    if (!behind)
    {
        if (any(corner_max < 0.0f) || any(corner_min > 1.0f))
            return VKR_LIGHT_CLUSTER_EMPTY_BOX;
        uv_min = saturate(corner_min);
        uv_max = saturate(corner_max);
    }
    float2 tiles = clusters.grid_scale.xy;
    uint3 low = uint3(min(uint2(floor(uv_min * tiles)), last.xy),
                      vkr_vk_light_cluster_slice(clusters,
                                                 center_w - w_extent));
    uint3 high = uint3(min(uint2(floor(uv_max * tiles)), last.xy),
                       vkr_vk_light_cluster_slice(clusters,
                                                  center_w + w_extent));
    return uint2(vkr_vk_light_cluster_pack(low),
                 vkr_vk_light_cluster_pack(high));
}

// A light's bounding sphere: its range, or for a spot whose outer cone is
// narrower than a hemisphere the smallest sphere around the cone and its
// cap. A light without a finite range reaches every cluster (radius < 0).
float4 vkr_vk_light_cluster_light_sphere(VkrGpuPointLightRow light)
{
    uint kind = uint(light.p2.w + 0.5f);
    float range = light.p2.z;
    if (kind == 0u || !(range > 0.0f))
        return float4(light.p0.xyz, -1.0f);
    if (kind == 2u)
    {
        float cos_outer = light.p1.w;
        float3 direction = normalize(light.p3.xyz);
        // Below 45 degrees the sphere through the apex and the cap's rim is
        // smaller; from there to 90 degrees, the sphere on the rim's circle.
        if (cos_outer > 0.70710678f)
        {
            float radius = range / (2.0f * cos_outer);
            return float4(light.p0.xyz + direction * radius, radius);
        }
        if (cos_outer > 0.0f)
        {
            float sin_outer = sqrt(saturate(1.0f - cos_outer * cos_outer));
            return float4(light.p0.xyz + direction * (range * cos_outer),
                          range * sin_outer);
        }
    }
    return float4(light.p0.xyz, range);
}

// A decal's bounding sphere: the box [-0.5, 0.5]^3 of its world_to_box
// transform, mapped back to the world. The columns of the inverse of the
// rows r0, r1, r2 are their cross products over the determinant, and they
// are the box's axes in the world.
float4 vkr_vk_light_cluster_decal_sphere(VkrVulkanDecalRow decal)
{
    float3 r0 = decal.world_to_box[0].xyz;
    float3 r1 = decal.world_to_box[1].xyz;
    float3 r2 = decal.world_to_box[2].xyz;
    float3 axis_x = cross(r1, r2);
    float3 axis_y = cross(r2, r0);
    float3 axis_z = cross(r0, r1);
    float inverse_determinant = 1.0f / dot(r0, axis_x);
    axis_x *= inverse_determinant;
    axis_y *= inverse_determinant;
    axis_z *= inverse_determinant;
    float3 center = -(axis_x * decal.world_to_box[0].w +
                      axis_y * decal.world_to_box[1].w +
                      axis_z * decal.world_to_box[2].w);
    return float4(center, 0.5f * sqrt(dot(axis_x, axis_x) +
                                      dot(axis_y, axis_y) +
                                      dot(axis_z, axis_z)));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_light_cluster_prepare(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkLightClusterBuildRoot root =
        vkr_vk_uniform_root(*g_light_cluster_push.root);
    uint item = thread_id.x;
    if (item == 0u)
        root.counters[0] = uint4(0u);
    if (item >= root.light_count + root.decal_count)
        return;
    VkrVulkanLightClusters *clusters = root.clusters;
    uint3 last = clusters.dimensions.xyz - 1u;
    uint2 full = uint2(0u, vkr_vk_light_cluster_pack(last));
    float near_w = clusters.near_w;
    uint sphere_slot = VKR_LIGHT_CLUSTER_ITEM_MAX + item * 2u;
    if (clusters.dimensions.w == 0u)
    {
        root.boxes[item] = full;
        root.boxes[sphere_slot + 1u] = asuint(float2(0.0f, -1.0f));
        return;
    }
    // Branches, not a select: each row loads only for its own table.
    float4 sphere;
    if (item < root.light_count)
        sphere = vkr_vk_light_cluster_light_sphere(root.lights[item]);
    else
        sphere = vkr_vk_light_cluster_decal_sphere(
            root.decals[item - root.light_count]);
    root.boxes[sphere_slot] = asuint(sphere.xy);
    root.boxes[sphere_slot + 1u] = asuint(sphere.zw);
    root.boxes[item] =
        sphere.w < 0.0f
            ? full
            : vkr_vk_light_cluster_sphere_box(clusters, sphere.xyz,
                                              sphere.w, near_w);
}

groupshared uint g_light_cluster_items[VKR_LIGHT_CLUSTER_ITEM_WORDS];

[shader("compute")]
[numthreads(VKR_LIGHT_CLUSTER_BIN_THREADS, 1, 1)]
void vk_light_cluster_bin(uint3 group_id : SV_GroupID,
                          uint thread_index : SV_GroupIndex)
{
    VkrVkLightClusterBuildRoot root =
        vkr_vk_uniform_root(*g_light_cluster_push.root);
    VkrVulkanLightClusters *clusters = root.clusters;
    uint3 dimensions = clusters.dimensions.xyz;
    uint2 tile = uint2(group_id.x % dimensions.x, group_id.x / dimensions.x);
    uint item_count = root.light_count + root.decal_count;
    uint word_count = (item_count + 31u) / 32u;
    for (uint word = thread_index; word < word_count;
         word += VKR_LIGHT_CLUSTER_BIN_THREADS)
        g_light_cluster_items[word] = 0u;
    GroupMemoryBarrierWithGroupSync();

    // The tile's four side planes through the eye, in world space: a point
    // p lies inside where dot(plane.xyz, p) + plane.w >= 0, from the clip
    // conditions x >= left * w, x <= right * w and likewise for y.
    float4x4 view_projection = clusters.view_projection;
    float4 row_x = float4(view_projection[0]);
    float4 row_y = float4(view_projection[1]);
    float4 row_w = float4(view_projection[3]);
    float2 ndc_low = float2(tile) / clusters.grid_scale.xy * 2.0f - 1.0f;
    float2 ndc_high = float2(tile + 1u) / clusters.grid_scale.xy * 2.0f - 1.0f;
    float4 planes[4] = {row_x - ndc_low.x * row_w, ndc_high.x * row_w - row_x,
                        row_y - ndc_low.y * row_w, ndc_high.y * row_w - row_y};
    [unroll]
    for (uint side = 0u; side < 4u; ++side)
        planes[side] /= length(planes[side].xyz);

    // Mark the items whose box covers this tile and whose sphere reaches the
    // tile's side planes.
    for (uint item = thread_index; item < item_count;
         item += VKR_LIGHT_CLUSTER_BIN_THREADS)
    {
        uint2 box = root.boxes[item];
        uint3 low = vkr_vk_light_cluster_unpack(box.x);
        uint3 high = vkr_vk_light_cluster_unpack(box.y);
        if (box.x == VKR_LIGHT_CLUSTER_EMPTY_BOX.x || any(tile < low.xy) ||
            any(tile > high.xy))
            continue;
        uint sphere_slot = VKR_LIGHT_CLUSTER_ITEM_MAX + item * 2u;
        float4 sphere = float4(asfloat(root.boxes[sphere_slot]),
                               asfloat(root.boxes[sphere_slot + 1u]));
        bool reaches = true;
        if (sphere.w >= 0.0f)
        {
            [unroll]
            for (uint side = 0u; side < 4u; ++side)
                reaches = reaches && dot(planes[side].xyz, sphere.xyz) +
                                             planes[side].w >=
                                         -sphere.w;
        }
        if (reaches)
        {
            uint ignored;
            InterlockedOr(g_light_cluster_items[item / 32u],
                          1u << (item % 32u), ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // One thread per slice: mark the slice's head lights, count its other
    // lights and its decals, take their run of the index pool, then write
    // them in ascending row order.
    uint slice = thread_index;
    if (slice >= dimensions.z)
        return;
    uint head_count = min(root.light_count, VKR_LIGHT_CLUSTER_HEAD_COUNT);
    VkrVulkanLightCluster header;
    header.head_lights = uint4(0u);
    header.first = 0u;
    header.tail_light_count = 0u;
    header.decal_count = 0u;
    header.reserved = 0u;
    for (uint word = 0u; word < word_count; ++word)
    {
        uint remaining = g_light_cluster_items[word];
        while (remaining != 0u)
        {
            uint item = word * 32u + uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            uint2 box = root.boxes[item];
            uint low = vkr_vk_light_cluster_unpack(box.x).z;
            uint high = vkr_vk_light_cluster_unpack(box.y).z;
            if (slice < low || slice > high)
                continue;
            if (item < head_count)
                header.head_lights[word] |= 1u << (item % 32u);
            else if (item < root.light_count)
                ++header.tail_light_count;
            else
                ++header.decal_count;
        }
    }
    uint count = header.tail_light_count + header.decal_count;
    if (count != 0u)
        InterlockedAdd(root.counters[0].x, count, header.first);
    // A pool too small for this list drops it whole and counts the
    // references, so a lit pixel never reads another cluster's indices.
    if (header.first + count > root.index_capacity)
    {
        uint ignored;
        InterlockedAdd(root.counters[0].y, count, ignored);
        header.tail_light_count = 0u;
        header.decal_count = 0u;
        count = 0u;
    }
    if (count != 0u)
    {
        uint ignored;
        InterlockedMax(root.counters[0].z, count, ignored);
    }
    uint cluster = tile.x + dimensions.x * (tile.y + dimensions.y * slice);
    clusters.headers[cluster] = header;
    uint cursor = header.first;
    uint first_word = head_count / 32u;
    uint end = header.first + count;
    for (uint word = first_word; word < word_count && cursor < end; ++word)
    {
        uint remaining = g_light_cluster_items[word];
        if (word * 32u < head_count)
            remaining &= ~0u << (head_count - word * 32u);
        while (remaining != 0u)
        {
            uint item = word * 32u + uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            uint2 box = root.boxes[item];
            uint low = vkr_vk_light_cluster_unpack(box.x).z;
            uint high = vkr_vk_light_cluster_unpack(box.y).z;
            if (slice < low || slice > high)
                continue;
            clusters.indices[cursor++] =
                item < root.light_count ? item : item - root.light_count;
        }
    }
}

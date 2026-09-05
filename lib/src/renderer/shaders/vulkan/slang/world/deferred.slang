#include "../../../shared/temporal_filter_kernel.slangh"

// Vulkan deferred visibility path. All variable-sized data is reached through
// physical storage-buffer addresses; descriptor indices name graph images.

static const uint VKR_VK_PRIMITIVE_ID_MASK = 0x7fffffffu;
static const uint VKR_VK_FRONT_FACE_BIT = 0x80000000u;
static const uint VKR_VK_DRAW_BUCKET_COUNT = 4u;
static const uint VKR_VK_FRUSTUM_PLANE_COUNT = 6u;
static const uint VKR_VK_HZB_MIP_MAX = 16u;

struct VkrVkDrawState
{
    uint2 execution_ranges[VKR_VK_DRAW_BUCKET_COUNT];
    uint bucket_counts[VKR_VK_DRAW_BUCKET_COUNT];
    uint bucket_cursors[VKR_VK_DRAW_BUCKET_COUNT];
    uint visible_count;
    uint overflow_count;
    uint resolve_invalid_count;
    uint occlusion_culled_count;
};

struct VkrVkIndirectCommand
{
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};

struct VkrVkGeometryRow
{
    VkrPackedStaticVertex *vertices;
    uint *indices;
    uint first_vertex;
    uint first_index;
    uint vertex_stride;
    uint vertex_layout;
    uint publication_generation;
    uint flags;
    VkrGpuGeometryDecodeRecord *decode;
};

struct VkrVkCullRoot
{
    VkrGpuCandidateDrawRow *candidates;
    uint *classifications;
    VkrGpuVisibleDrawRow *visible;
    VkrVkDrawState *states;
    VkrVkIndirectCommand *commands;
    VkrVulkanInstance *instances;
    float4x4 *view_projections;
    float4 *frustum_planes;
    uint candidate_count;
    uint view_count;
    uint candidate_capacity;
    uint command_partition_capacity;
    uint hzb_textures[VKR_VK_HZB_MIP_MAX];
    uint hzb_mip_count;
    uint2 hzb_extent;
    uint hzb_enabled;
    float hzb_depth_epsilon;
    uint camera_required_flags;
    uint shadow_required_flags;
};

struct VkrVkRasterRoot
{
    VkrVkGeometryRow *geometry_rows;
    VkrGpuVisibleDrawRow *visible_rows;
    VkrVkDrawState *states;
    VkrVulkanPacketFrameRoot *frame;
    uint view_index;
    uint visible_capacity;
    uint previous_depth_texture;
    uint previous_depth_layer;
};

struct VkrVkVisibilityOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint visible_index : TEXCOORD1;
};

struct VkrVkVisibilityFragment
{
    uint2 value : SV_Target0;
};

struct VkrVkTemporalTransformRoot
{
    VkrVulkanInstance *instances;
    VkrTemporalTransform *transforms;
    uint instance_count;
    uint transform_capacity;
    uint frame_index;
    uint reserved;
};

struct VkrVkResolveRoot
{
    VkrVkGeometryRow *geometry_rows;
    VkrGpuVisibleDrawRow *visible_rows;
    uint2 reserved_address;
    VkrVulkanInstance *instances;
    VkrVulkanMaterial *materials;
    VkrPackedStaticVertex *vertices;
    uint *indices;
    VkrVkDrawState *compaction_state;
    float4x4 view_projection;
    float4x4 current_view_projection;
    float4x4 previous_view_projection;
    VkrTemporalTransform *previous_transforms;
    uint vbuffer_texture;
    uint albedo_texture;
    uint specular_texture;
    uint normal_texture;
    uint emissive_texture;
    uint debug_texture;
    uint scene_texture;
    uint motion_texture;
    uint validity_texture;
    uint2 extent;
    uint visible_capacity;
    uint geometry_count;
    uint material_count;
    uint instance_count;
    uint render_mode;
    uint history_valid;
    uint previous_frame_index;
    uint4 reserved_tail;
    float4x4 sky_reprojection;
};

struct VkrVkTemporalResolveRoot
{
    VkrGpuVisibleDrawRow *visible_rows;
    VkrVulkanInstance *instances;
    uint scene_texture;
    uint pre_transmission_texture;
    uint motion_texture;
    uint validity_texture;
    uint depth_texture;
    uint vbuffer_texture;
    uint history_color_texture;
    uint history_depth_texture;
    uint history_identity_texture;
    uint history_surface_texture;
    uint output_color_texture;
    uint output_depth_texture;
    uint output_identity_texture;
    uint output_surface_texture;
    uint history_sampler;
    uint2 extent;
    uint history_valid;
    uint render_mode;
    uint camera_stationary;
    VkrGpuVisibleDrawRow *transmission_visible_rows;
    VkrVulkanInstance *transmission_instances;
    uint transmission_vbuffer_texture;
    uint transmission_depth_texture;
    uint transmission_enabled;
    uint scene_stationary;
    float2 current_jitter_pixels;
    float2 previous_jitter_pixels;
};

struct VkrVkLightingRoot
{
    VkrVulkanPacketFrameRoot *frame;
    uint2 frame_address_padding;
    float4x4 inverse_view_projection;
    uint vbuffer_texture;
    uint depth_texture;
    uint albedo_texture;
    uint specular_texture;
    uint normal_texture;
    uint scene_texture;
    uint2 extent;
    uint sky_texture;
    uint sky_sampler;
    uint sky_enabled;
    uint gtao_visibility_texture;
};

struct VkrVkHzbRoot
{
    uint source_texture;
    uint destination_texture;
    uint2 source_extent;
    uint2 destination_extent;
    uint source_is_depth;
    uint3 reserved;
};
struct VkrVkSdsmState
{
    uint min_device_z_bits;
    uint max_device_z_bits;
    uint occupied_count;
    uint reserved;
};

struct VkrVkSdsmRoot
{
    VkrVkSdsmState *reduce_state;
    uint depth_texture;
    uint vbuffer_texture;
    uint2 extent;
    uint2 reserved;
};

struct VkrVkPickingRoot
{
    VkrGpuVisibleDrawRow *opaque_visible;
    VkrGpuVisibleDrawRow *transmission_visible;
    VkrVulkanInstance *opaque_instances;
    VkrVulkanInstance *transmission_instances;
    uint opaque_vbuffer;
    uint transmission_vbuffer;
    uint output_texture;
    uint2 pixel;
    uint transmission_layer;
    uint use_transmission;
};

struct VkrVkTransmissionRoot
{
    VkrGpuVisibleDrawRow *visible_rows;
    VkrVulkanMaterial *materials;
    VkrVulkanTransmissionMaterial *transmission_materials;
    uint opaque_texture;
    uint opaque_mip_count;
    VkrVkGeometryRow *geometry_rows;
    VkrVulkanInstance *instances;
    VkrPackedStaticVertex *vertices;
    uint *indices;
    VkrVkDrawState *compaction_state;
    uint *pixel_list;
    uint *compact_counts;
    VkrVulkanPacketFrameRoot *frame;
    uint4 frame_address_padding;
    float4x4 view_projection;
    float4x4 inverse_view_projection;
    uint vbuffer_texture;
    uint depth_texture;
    uint feedback_texture;
    uint feedback_sampler;
    uint output_texture;
    uint layer;
    uint2 extent;
    VkrTemporalTransform *previous_transforms;
    uint2 previous_transform_address_padding;
    float4x4 current_view_projection;
    float4x4 previous_view_projection;
    uint motion_texture;
    uint validity_texture;
    uint history_valid;
    uint previous_frame_index;
    uint visible_capacity;
    uint geometry_count;
    uint material_count;
    uint instance_count;
    uint pixel_capacity;
    uint compact_layer;
    uint compact_enabled;
    uint reserved;
};

struct VkrVkTransmissionCompactRoot
{
    uint *pixel_list;
    uint *covered_pixels;
    uint *overflow_counts;
    uint *indirect_arguments;
    VkrGpuVisibleDrawRow *visible_rows;
    VkrVulkanMaterial *materials;
    uint vbuffer_texture;
    uint source_texture;
    uint destination_texture;
    uint2 extent;
    uint layer;
    uint capacity;
    uint reserved[5];
};

struct VkrVkTransmissionCoverageRoot
{
    uint *covered_pixels;
    uint vbuffer_texture;
    uint layer;
    uint2 extent;
    uint2 reserved;
};

struct VkrVkCullPush
{
    VkrVkCullRoot *root;
    uint2 reserved;
};
struct VkrVkRasterPush
{
    VkrVkRasterRoot *root;
    uint bucket;
    uint reserved;
};
struct VkrVkTemporalTransformPush
{
    VkrVkTemporalTransformRoot *root;
    uint2 reserved;
};
struct VkrVkResolvePush
{
    VkrVkResolveRoot *root;
    uint2 reserved;
};
struct VkrVkLightingPush
{
    VkrVkLightingRoot *root;
    uint2 reserved;
};
struct VkrVkTemporalResolvePush
{
    VkrVkTemporalResolveRoot *root;
    uint2 reserved;
};
struct VkrVkHzbPush
{
    VkrVkHzbRoot *root;
    uint2 reserved;
};
struct VkrVkSdsmPush
{
    VkrVkSdsmRoot *root;
    uint2 reserved;
};
struct VkrVkPickingPush
{
    VkrVkPickingRoot *root;
    uint2 reserved;
};
struct VkrVkTransmissionPush
{
    VkrVkTransmissionRoot *root;
    uint2 reserved;
};
struct VkrVkTransmissionCoveragePush
{
    VkrVkTransmissionCoverageRoot *root;
    uint2 reserved;
};
struct VkrVkTransmissionCompactPush
{
    VkrVkTransmissionCompactRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkCullPush> g_cull_push;
[[vk::push_constant]] ConstantBuffer<VkrVkRasterPush> g_raster_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTemporalTransformPush>
    g_temporal_transform_push;
[[vk::push_constant]] ConstantBuffer<VkrVkResolvePush> g_resolve_push;
[[vk::push_constant]] ConstantBuffer<VkrVkLightingPush> g_lighting_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTemporalResolvePush>
    g_temporal_resolve_push;
[[vk::push_constant]] ConstantBuffer<VkrVkHzbPush> g_hzb_push;
[[vk::push_constant]] ConstantBuffer<VkrVkSdsmPush> g_sdsm_push;
[[vk::push_constant]] ConstantBuffer<VkrVkPickingPush> g_picking_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTransmissionPush> g_transmission_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTransmissionCoveragePush>
    g_transmission_coverage_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTransmissionCompactPush>
    g_transmission_compact_push;

static VkrVkCullRoot *vkr_vk_cull_root() { return g_cull_push.root; }

static bool vkr_vk_sphere_visible(VkrVkCullRoot *root, float4 sphere,
                                  uint view_index)
{
    float radius = max(sphere.w, 0.0f);
    [unroll] for (uint plane = 0u; plane < VKR_VK_FRUSTUM_PLANE_COUNT; ++plane)
    {
        float4 equation =
            root.frustum_planes[view_index * VKR_VK_FRUSTUM_PLANE_COUNT +
                                plane];
        if (dot(equation.xyz, sphere.xyz) + equation.w < -radius)
            return false;
    }
    return true;
}

static bool vkr_vk_candidate_occluded(VkrVkCullRoot *root,
                                      VkrGpuCandidateDrawRow candidate)
{
    if (root.hzb_enabled == 0u || root.hzb_mip_count == 0u ||
        any(root.hzb_extent == 0u) ||
        (vkr_gpu_draw_candidate_flags(candidate.state_flags) & 1u) == 0u)
        return false;
    VkrVulkanInstance instance = root.instances[candidate.instance_index];
    float3 center =
        mul(instance.model, float4(candidate.local_bounding_sphere.xyz, 1.0f))
            .xyz;
    float3 scale_x = mul(instance.model, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz;
    float3 scale_y = mul(instance.model, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz;
    float3 scale_z = mul(instance.model, float4(0.0f, 0.0f, 1.0f, 0.0f)).xyz;
    float scale = max(length(scale_x), max(length(scale_y), length(scale_z)));
    float radius = max(candidate.local_bounding_sphere.w * scale, 0.0f);
    float2 ndc_min = 1e30f;
    float2 ndc_max = -1e30f;
    float nearest_depth = 1.0f;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        float3 sign_vector = float3((corner & 1u) != 0u ? 1.0f : -1.0f,
                                    (corner & 2u) != 0u ? 1.0f : -1.0f,
                                    (corner & 4u) != 0u ? 1.0f : -1.0f);
        float4 clip = mul(root.view_projections[0],
                          float4(center + sign_vector * radius, 1.0f));
        if (!all(isfinite(clip)) || clip.w <= 1e-6f)
            return false;
        float3 ndc = clip.xyz / clip.w;
        ndc_min = min(ndc_min, ndc.xy);
        ndc_max = max(ndc_max, ndc.xy);
        nearest_depth = min(nearest_depth, ndc.z);
    }
    if (nearest_depth <= 0.0f || any(ndc_min > 1.0f) || any(ndc_max < -1.0f))
        return false;
    float2 uv_min =
        clamp(float2(ndc_min.x, -ndc_max.y) * 0.5f + 0.5f, 0.0f, 1.0f);
    float2 uv_max =
        clamp(float2(ndc_max.x, -ndc_min.y) * 0.5f + 0.5f, 0.0f, 1.0f);
    uint2 base_min = min(uint2(floor(uv_min * float2(root.hzb_extent))),
                         root.hzb_extent - 1u);
    uint2 base_max = min(uint2(floor(uv_max * float2(root.hzb_extent))),
                         root.hzb_extent - 1u);
    uint2 span = base_max - base_min + 1u;
    uint max_span = max(span.x, span.y);
    uint mip = min(max_span > 1u ? uint(floor(log2(float(max_span)))) : 0u,
                   root.hzb_mip_count - 1u);
    uint2 mip_extent = max(root.hzb_extent >> mip, 1u);
    uint2 sample_min = min(base_min >> mip, mip_extent - 1u);
    uint2 sample_max = min(base_max >> mip, mip_extent - 1u);
    for (uint y = sample_min.y; y <= sample_max.y; ++y)
    {
        for (uint x = sample_min.x; x <= sample_max.x; ++x)
        {
            // `mip` is derived per candidate, so lanes in this workgroup can
            // select different HZB levels. A divergent descriptor index must be
            // decorated NonUniform; without it the access is undefined.
            float occluder_depth =
                g_storage_2d[NonUniformResourceIndex(root.hzb_textures[mip])]
                            [uint2(x, y)]
                                .x;
            if (nearest_depth <= occluder_depth + root.hzb_depth_epsilon)
                return false;
        }
    }
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_gpu_draw_classify(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkCullRoot *root = vkr_vk_cull_root();
    uint candidate_index = thread_id.x;
    uint view_index = thread_id.y;
    if (candidate_index >= root.candidate_count || view_index >= root.view_count)
        return;
    VkrGpuCandidateDrawRow candidate = root.candidates[candidate_index];
    uint required_flags = view_index == 0u ? root.camera_required_flags
                                           : root.shadow_required_flags;
    uint candidate_flags = vkr_gpu_draw_candidate_flags(candidate.state_flags);
    uint bucket = vkr_gpu_draw_state_bucket(candidate.state_flags);
    if ((candidate_flags & required_flags) != required_flags)
    {
        root.classifications[view_index * root.candidate_capacity +
                             candidate_index] = 0xffffffffu;
        return;
    }
    VkrVulkanInstance instance = root.instances[candidate.instance_index];
    float4 local = candidate.local_bounding_sphere;
    float4 world_center = mul(instance.model, float4(local.xyz, 1.0f));
    float3 scale_x = mul(instance.model, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz;
    float3 scale_y = mul(instance.model, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz;
    float3 scale_z = mul(instance.model, float4(0.0f, 0.0f, 1.0f, 0.0f)).xyz;
    float world_radius =
        local.w * max(length(scale_x), max(length(scale_y), length(scale_z)));
    uint classification_index =
        view_index * root.candidate_capacity + candidate_index;
    float4 world_sphere = float4(world_center.xyz, world_radius);
    if ((candidate_flags & 1u) != 0u &&
        !vkr_vk_sphere_visible(root, world_sphere, view_index))
    {
        root.classifications[classification_index] = 0xffffffffu;
        return;
    }
    if (view_index == 0u && vkr_vk_candidate_occluded(root, candidate))
    {
        root.classifications[classification_index] = 0xffffffffu;
        uint ignored;
        InterlockedAdd(root.states[view_index].occlusion_culled_count, 1u,
                       ignored);
        return;
    }
    root.classifications[classification_index] = bucket;
    uint ignored;
    InterlockedAdd(root.states[view_index].bucket_counts[bucket], 1u, ignored);
}

[shader("compute")]
[numthreads(5, 1, 1)]
void vk_gpu_draw_prefix(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkCullRoot *root = vkr_vk_cull_root();
    uint view_index = thread_id.x;
    if (view_index >= root.view_count)
        return;
    uint cursor = 0u;
    uint overflow = 0u;
    [unroll] for (uint bucket = 0u; bucket < VKR_VK_DRAW_BUCKET_COUNT; ++bucket)
    {
        uint count = root.states[view_index].bucket_counts[bucket];
        uint written_count = min(count, root.command_partition_capacity);
        root.states[view_index].execution_ranges[bucket] =
            uint2(cursor, written_count);
        root.states[view_index].bucket_cursors[bucket] = 0u;
        cursor += written_count;
        overflow += count - written_count;
    }
    root.states[view_index].visible_count = cursor;
    root.states[view_index].overflow_count = overflow;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_gpu_draw_encode(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkCullRoot *root = vkr_vk_cull_root();
    uint candidate_index = thread_id.x;
    uint view_index = thread_id.y;
    if (candidate_index >= root.candidate_count || view_index >= root.view_count)
        return;
    uint classification_index =
        view_index * root.candidate_capacity + candidate_index;
    uint bucket = root.classifications[classification_index];
    if (bucket >= VKR_VK_DRAW_BUCKET_COUNT)
        return;
    uint local_index;
    InterlockedAdd(root.states[view_index].bucket_cursors[bucket], 1u,
                   local_index);
    if (local_index >= root.command_partition_capacity)
        return;
    uint visible_index =
        root.states[view_index].execution_ranges[bucket].x + local_index;
    if (visible_index >= root.candidate_capacity)
        return;
    VkrGpuCandidateDrawRow candidate = root.candidates[candidate_index];
    VkrGpuVisibleDrawRow visible;
    visible.geometry_index = candidate.geometry_index;
    visible.material_index = candidate.material_index;
    visible.instance_index = candidate.instance_index;
    visible.first_index = candidate.first_index;
    visible.index_count = candidate.index_count;
    visible.vertex_offset = candidate.vertex_offset;
    visible.decode_index = candidate.decode_index;
    visible.state_flags = candidate.state_flags;
    root.visible[view_index * root.candidate_capacity + visible_index] =
        visible;
    uint argument_index = (view_index * VKR_VK_DRAW_BUCKET_COUNT + bucket) *
                              root.command_partition_capacity +
                          local_index;
    VkrVkIndirectCommand command;
    command.index_count = candidate.index_count;
    command.instance_count = 1u;
    command.first_index = candidate.first_index;
    command.vertex_offset = candidate.vertex_offset;
    command.first_instance = 0u;
    root.commands[argument_index] = command;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_temporal_transform(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkTemporalTransformRoot *root = g_temporal_transform_push.root;
    if (thread_id.x >= root.instance_count)
        return;
    VkrVulkanInstance instance = root.instances[thread_id.x];
    if ((instance.temporal_flags & VKR_INSTANCE_TEMPORAL_OWNER) == 0u ||
        instance.temporal_index >= root.transform_capacity)
        return;
    VkrTemporalTransform transform;
    transform.model = instance.model;
    transform.generation = instance.temporal_generation;
    transform.frame_index = root.frame_index;
    transform.valid = 1u;
    transform.reserved = 0u;
    root.transforms[instance.temporal_index] = transform;
}

[shader("vertex")]
VkrVkVisibilityOutput vk_visibility_vertex(uint vertex_id : SV_VertexID,
                                           uint draw_index : SV_DrawIndex)
{
    VkrVkRasterRoot *root = g_raster_push.root;
    uint visible_instance =
        root.states[root.view_index].execution_ranges[g_raster_push.bucket].x +
        draw_index;
    uint visible_index =
        root.view_index * root.visible_capacity + visible_instance;
    VkrGpuVisibleDrawRow visible = root.visible_rows[visible_index];
    VkrVkGeometryRow geometry = root.geometry_rows[visible.geometry_index];
    VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
        geometry.vertices[geometry.first_vertex + vertex_id],
        geometry.decode[visible.decode_index]);
    VkrVulkanInstance instance = root.frame.instances[visible.instance_index];
    VkrVkVisibilityOutput output;
    output.position = mul(root.frame.view_projection,
                          mul(instance.model, float4(vertex.position, 1.0f)));
    output.uv = vertex.texcoord;
    output.color = vertex.color;
    output.visible_index = visible_instance;
    return output;
}

[shader("fragment")]
VkrVkVisibilityFragment
vk_visibility_fragment(VkrVkVisibilityOutput input,
                       uint primitive_id : SV_PrimitiveID,
                       bool front_facing : SV_IsFrontFace)
{
    VkrVkRasterRoot *root = g_raster_push.root;
    if (root.previous_depth_texture != 0xffffffffu)
    {
        float previous_depth =
            g_array_textures[root.previous_depth_texture]
                .Load(int4(int2(input.position.xy),
                           int(root.previous_depth_layer), 0))
                .r;
        if (input.position.z <= previous_depth + 1e-5f)
            discard;
    }
    VkrGpuVisibleDrawRow visible =
        root.visible_rows[root.view_index * root.visible_capacity +
                          input.visible_index];
    VkrVulkanMaterial material = root.frame.materials[visible.material_index];
    if (material.alpha_mode == 1u)
    {
        float alpha =
            g_textures[NonUniformResourceIndex(material.base_color_texture)]
                .Sample(g_samplers[NonUniformResourceIndex(
                            material.base_color_sampler)],
                        input.uv)
                .a *
            material.tint.a * input.color.a;
        if (alpha < material.material_alpha.x)
            discard;
    }
    VkrVkVisibilityFragment output;
    output.value =
        uint2(input.visible_index + 1u,
              primitive_id | (front_facing ? VKR_VK_FRONT_FACE_BIT : 0u));
    return output;
}

// Camera-opaque visibility draws reach neither discard above. A shader that
// can discard prevents unconditional early depth writes, so opaque buckets
// use this fragment with early tests. Cutouts retain alpha testing, and
// transmission retains the preceding peel-depth rejection.
[shader("fragment")]
[earlydepthstencil]
VkrVkVisibilityFragment
vk_visibility_opaque_fragment(VkrVkVisibilityOutput input,
                              uint primitive_id : SV_PrimitiveID,
                              bool front_facing : SV_IsFrontFace)
{
    VkrVkVisibilityFragment output;
    output.value =
        uint2(input.visible_index + 1u,
              primitive_id | (front_facing ? VKR_VK_FRONT_FACE_BIT : 0u));
    return output;
}

[shader("fragment")]
void vk_visibility_shadow_fragment(VkrVkVisibilityOutput input)
{
    VkrVkRasterRoot *root = g_raster_push.root;
    VkrGpuVisibleDrawRow visible =
        root.visible_rows[root.view_index * root.visible_capacity +
                          input.visible_index];
    VkrVulkanMaterial material = root.frame.materials[visible.material_index];
    if (material.alpha_mode == 1u)
    {
        float alpha =
            g_textures[NonUniformResourceIndex(material.base_color_texture)]
                .Sample(g_samplers[NonUniformResourceIndex(
                            material.base_color_sampler)],
                        input.uv)
                .a *
            material.tint.a * input.color.a;
        if (alpha < material.material_alpha.x)
            discard;
    }
}

float2 vkr_vk_octahedral_encode(float3 normal)
{
    normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-7f);
    float2 encoded = normal.xy;
    if (normal.z < 0.0f)
    {
        float2 sign_value = float2(encoded.x >= 0.0f ? 1.0f : -1.0f,
                                   encoded.y >= 0.0f ? 1.0f : -1.0f);
        encoded = (1.0f - abs(encoded.yx)) * sign_value;
    }
    return clamp(encoded, -1.0f, 1.0f);
}

float3 vkr_vk_octahedral_decode(float2 encoded)
{
    float3 normal = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0.0f)
    {
        float2 sign_value = float2(normal.x >= 0.0f ? 1.0f : -1.0f,
                                   normal.y >= 0.0f ? 1.0f : -1.0f);
        normal.xy = (1.0f - abs(normal.yx)) * sign_value;
    }
    return normalize(normal);
}

bool vkr_vk_perspective_barycentric(float2 point, float4 ca, float4 cb,
                                    float4 cc, uint2 extent,
                                    out float3 barycentric,
                                    out float3 barycentric_dx,
                                    out float3 barycentric_dy)
{
    barycentric = 0.0f;
    barycentric_dx = 0.0f;
    barycentric_dy = 0.0f;

    // Solve the homogeneous screen-point constraints plus sum(lambda)=1.
    // This stays defined when the original triangle crosses the eye plane and
    // Vulkan rasterizes the clipped polygon; dividing each original vertex by
    // w does not.
    float3 horizontal = float3(ca.x - point.x * ca.w, cb.x - point.x * cb.w,
                               cc.x - point.x * cc.w);
    float3 vertical = float3(ca.y - point.y * ca.w, cb.y - point.y * cb.w,
                             cc.y - point.y * cc.w);
    float3 clip_w = float3(ca.w, cb.w, cc.w);
    float3 weights = cross(horizontal, vertical);
    float3 weights_dx = cross(-clip_w, vertical);
    float3 weights_dy = cross(horizontal, -clip_w);
    // A zero-area or edge-on triangle is legitimate mesh content rather than
    // corrupt data, so these two magnitude guards stay. Everything they used to
    // be paired with was a finiteness test, and finiteness here follows from
    // finite vertex positions and a finite view-projection; those tests are
    // implied by the guards that remain.
    float scale = max(abs(weights.x), max(abs(weights.y), abs(weights.z)));
    if (scale <= 1e-12f)
        return false;
    weights /= scale;
    weights_dx /= scale;
    weights_dy /= scale;
    float normalization = weights.x + weights.y + weights.z;
    if (abs(normalization) <= 1e-12f)
        return false;
    barycentric = weights / normalization;
    barycentric_dx = (weights_dx - barycentric * dot(weights_dx, 1.0f)) /
                     normalization * (2.0f / float(extent.x));
    barycentric_dy = (weights_dy - barycentric * dot(weights_dy, 1.0f)) /
                     normalization * (2.0f / float(extent.y));
    return true;
}

float2 vkr_vk_resolve_ndc(float2 pixel, uint2 extent)
{
    // Vulkan's positive-height viewport maps framebuffer top to NDC Y = -1.
    // The shared projection already inverts Y, so flipping it again here would
    // reconstruct attributes from the vertically mirrored raster location.
    return pixel / float2(extent) * 2.0f - 1.0f;
}

bool vkr_vk_finite_nonzero(float3 value)
{
    return all(isfinite(value)) && dot(value, value) > 1e-12f;
}

// `marker` is the documented resolve-debug alpha: 0 for an unwritten background
// pixel and -1 for a rejected row, matching the Metal channel contract.
void vkr_vk_resolve_defaults(VkrVkResolveRoot *root, uint2 pixel, float marker)
{
    g_storage_2d[root.albedo_texture][pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_storage_2d[root.specular_texture][pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_storage_float2_2d[root.normal_texture][pixel] = 0.0f;
    g_storage_2d[root.emissive_texture][pixel] = 0.0f;
    g_storage_2d[root.debug_texture][pixel] = float4(0.0f, 0.0f, 0.0f, marker);
    g_storage_2d[root.scene_texture][pixel] = 0.0f;
    g_storage_float2_2d[root.motion_texture][pixel] = 0.0f;
    g_storage_float2_2d[root.validity_texture][pixel] = 0.0f;
}

// One visibility sample reconstructed back into shadeable material inputs.
// The G-buffer resolve and every transmission layer read the same encoding, so
// they share this reconstruction rather than keeping two copies of the
// barycentric, gradient, tangent-frame, and validity rules.
struct VkrVkSurfaceSource
{
    VkrVkGeometryRow *geometry_rows;
    VkrGpuVisibleDrawRow *visible_rows;
    VkrVulkanInstance *instances;
    VkrVulkanMaterial *materials;
    VkrPackedStaticVertex *vertices;
    uint *indices;
    VkrVkDrawState *compaction_state;
    float4x4 view_projection;
    uint2 extent;
    uint visible_capacity;
    uint geometry_count;
    uint material_count;
    uint instance_count;
};

bool vkr_vk_reject_surface(VkrVkSurfaceSource source)
{
    uint ignored;
    InterlockedAdd(source.compaction_state.resolve_invalid_count, 1u, ignored);
    return false;
}

struct VkrVkSurface
{
    float4 base;
    float3 normal;
    float3 object_position;
    float3 object_normal;
    float4 object_tangent;
    float3 tangent_normal;
    float3 emissive;
    float3 barycentric;
    float face_sign;
    float metallic;
    float roughness;
    float feedback_roughness;
    float occlusion;
    float selected_lod;
    float2 uv;
    float2 uv_dx;
    float2 uv_dy;
    uint material_index;
    uint instance_index;
    uint normal_mapped;
};

// Returns false for a row this pass must not shade, counting it as an invalid
// resolve first. The caller owns the fallback, because the G-buffer writes
// defaults while a transmission layer keeps its background.
bool vkr_vk_resolve_surface(VkrVkSurfaceSource source, uint2 encoded,
                            uint2 pixel, out VkrVkSurface surface)
{
    surface = (VkrVkSurface)0;
    // The visibility buffer, the compacted visible rows, and the geometry table
    // are all produced by this renderer's own GPU culling pipeline, and the
    // caller has already established that this pixel carries a written sample.
    // Re-deriving those bounds here duplicated a contract the producer owns and
    // cost ten comparisons on every shaded pixel. Degenerate-triangle rejection
    // still happens below, where the input genuinely can be degenerate.
    uint visible_index = encoded.x - 1u;
    uint primitive_id = encoded.y & VKR_VK_PRIMITIVE_ID_MASK;
    VkrGpuVisibleDrawRow visible = source.visible_rows[visible_index];
    VkrVulkanMaterial material = source.materials[visible.material_index];
    VkrVkGeometryRow geometry = source.geometry_rows[visible.geometry_index];
    uint triangle = primitive_id * 3u;
    int ia = int(source.indices[visible.first_index + triangle + 0u]) +
             visible.vertex_offset;
    int ib = int(source.indices[visible.first_index + triangle + 1u]) +
             visible.vertex_offset;
    int ic = int(source.indices[visible.first_index + triangle + 2u]) +
             visible.vertex_offset;
    VkrGpuGeometryDecodeRecord decode = geometry.decode[visible.decode_index];
    VkrGpuDecodedVertex a = vkr_decode_packed_vertex(
        source.vertices[geometry.first_vertex + uint(ia)], decode);
    VkrGpuDecodedVertex b = vkr_decode_packed_vertex(
        source.vertices[geometry.first_vertex + uint(ib)], decode);
    VkrGpuDecodedVertex c = vkr_decode_packed_vertex(
        source.vertices[geometry.first_vertex + uint(ic)], decode);
    VkrVulkanInstance instance = source.instances[visible.instance_index];
    float4 wa = mul(instance.model, float4(a.position, 1.0f));
    float4 wb = mul(instance.model, float4(b.position, 1.0f));
    float4 wc = mul(instance.model, float4(c.position, 1.0f));
    float4 ca = mul(source.view_projection, wa);
    float4 cb = mul(source.view_projection, wb);
    float4 cc = mul(source.view_projection, wc);
    float2 p = float2(pixel) + 0.5f;
    float3 perspective;
    float3 perspective_dx;
    float3 perspective_dy;
    if (!vkr_vk_perspective_barycentric(vkr_vk_resolve_ndc(p, source.extent),
                                        ca, cb, cc, source.extent, perspective,
                                        perspective_dx, perspective_dy))
        return vkr_vk_reject_surface(source);
    float2 uv = a.texcoord * perspective.x + b.texcoord * perspective.y +
                c.texcoord * perspective.z;
    float2 uv_dx = a.texcoord * perspective_dx.x +
                   b.texcoord * perspective_dx.y +
                   c.texcoord * perspective_dx.z;
    float2 uv_dy = a.texcoord * perspective_dy.x +
                   b.texcoord * perspective_dy.y +
                   c.texcoord * perspective_dy.z;
    float4 color = a.color * perspective.x + b.color * perspective.y +
                   c.color * perspective.z;
    float4 base =
        g_textures[NonUniformResourceIndex(material.base_color_texture)]
            .SampleGrad(g_samplers[NonUniformResourceIndex(
                            material.base_color_sampler)],
                        uv, uv_dx, uv_dy) *
        material.tint * color;
    float metallic = saturate(material.material_surface.x);
    float feedback_roughness = saturate(material.material_surface.y);
    float roughness = clamp(feedback_roughness, 0.04f, 1.0f);
    float ao = saturate(material.material_surface.w);
    if ((material.flags & 2u) != 0u)
    {
        float3 orm =
            g_textures[NonUniformResourceIndex(material.orm_texture)]
                .SampleGrad(
                    g_samplers[NonUniformResourceIndex(material.orm_sampler)],
                    uv, uv_dx, uv_dy)
                .rgb;
        ao *= orm.r;
        feedback_roughness = saturate(feedback_roughness * orm.g);
        roughness = clamp(feedback_roughness, 0.04f, 1.0f);
        metallic = saturate(metallic * orm.b);
    }
    float3 object_normal = a.normal * perspective.x + b.normal * perspective.y +
                           c.normal * perspective.z;
    float3 object_position = a.position * perspective.x +
                             b.position * perspective.y +
                             c.position * perspective.z;
    float4 object_tangent = a.tangent * perspective.x +
                            b.tangent * perspective.y +
                            c.tangent * perspective.z;
    if ((material.flags & 1u) != 0u)
    {
        surface.tangent_normal = vkr_normal_map_decode(
            g_textures[NonUniformResourceIndex(material.normal_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(
                                material.normal_sampler)],
                            uv, uv_dx, uv_dy)
                .xyz,
            material.material_surface.z);
        surface.normal_mapped = 1u;
    }
    float3 emissive = material.material_emissive.rgb;
    if ((material.flags & 4u) != 0u)
        emissive *=
            g_textures[NonUniformResourceIndex(material.emissive_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(
                                material.emissive_sampler)],
                            uv, uv_dx, uv_dy)
                .rgb;
    uint2 base_color_extent;
    g_textures[NonUniformResourceIndex(material.base_color_texture)]
        .GetDimensions(base_color_extent.x, base_color_extent.y);
    float2 texture_extent = float2(base_color_extent);
    float footprint =
        max(length(uv_dx * texture_extent), length(uv_dy * texture_extent));
    surface.base = base;
    surface.object_position = object_position;
    surface.object_normal = object_normal;
    surface.object_tangent = object_tangent;
    surface.emissive = emissive;
    surface.barycentric = perspective;
    surface.metallic = metallic;
    surface.roughness = roughness;
    surface.feedback_roughness = feedback_roughness;
    surface.occlusion = ao;
    surface.selected_lod = max(log2(max(footprint, 1e-8f)), 0.0f);
    surface.uv = uv;
    surface.uv_dx = uv_dx;
    surface.uv_dy = uv_dy;
    surface.material_index = visible.material_index;
    surface.instance_index = visible.instance_index;
    surface.face_sign =
        (encoded.y & VKR_VK_FRONT_FACE_BIT) != 0u ? 1.0f : -1.0f;
    return true;
}

bool vkr_vk_finish_surface_normal(VkrVkSurfaceSource source,
                                  float3 transformed_normal,
                                  float3 transformed_tangent,
                                  inout VkrVkSurface surface)
{
    if (!vkr_vk_finite_nonzero(transformed_normal))
        return vkr_vk_reject_surface(source);
    float3 normal = normalize(transformed_normal) * surface.face_sign;
    if (surface.normal_mapped != 0u)
    {
        if (!vkr_vk_finite_nonzero(surface.tangent_normal) ||
            !vkr_vk_finite_nonzero(transformed_tangent))
            return vkr_vk_reject_surface(source);
        float3 tangent = normalize(transformed_tangent);
        tangent -= dot(tangent, normal) * normal;
        if (!vkr_vk_finite_nonzero(tangent))
            return vkr_vk_reject_surface(source);
        tangent = normalize(tangent);
        float tangent_handedness =
            surface.object_tangent.w < 0.0f ? -1.0f : 1.0f;
        // `normal` and `tangent` are unit and orthogonal by the Gram-Schmidt
        // step above, so the cross product is unit and `mapped` has exactly the
        // magnitude of `tangent_normal`, which the guard above already
        // established as nonzero. Both former checks were implied.
        float3 bitangent =
            normalize(cross(normal, tangent)) * tangent_handedness;
        float3 mapped = tangent * surface.tangent_normal.x +
                        bitangent * surface.tangent_normal.y +
                        normal * surface.tangent_normal.z;
        normal = normalize(mapped);
    }
    surface.normal = normal;
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_gbuffer_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkResolveRoot *root = g_resolve_push.root;
    if (any(thread_id.xy >= root.extent))
        return;
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy];
    if (encoded.x == 0u)
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, 0.0f);
        if (root.history_valid != 0u)
        {
            float2 uv = (float2(thread_id.xy) + 0.5f) / float2(root.extent);
            float4 previous_clip = mul(root.sky_reprojection,
                                        float4(uv * 2.0f - 1.0f, 1.0f, 1.0f));
            if (previous_clip.w > 1e-6f)
            {
                float2 previous_uv = previous_clip.xy / previous_clip.w * 0.5f + 0.5f;
                g_storage_float2_2d[root.motion_texture][thread_id.xy] = previous_uv - uv;
                g_storage_float2_2d[root.validity_texture][thread_id.xy] = float2(1.0f, 1.0f);
            }
        }
        return;
    }
    VkrVkSurfaceSource source = {
        root.geometry_rows,    root.visible_rows,    root.instances,
        root.materials,        root.vertices,        root.indices,
        root.compaction_state, root.view_projection, root.extent,
        root.visible_capacity, root.geometry_count,  root.material_count,
        root.instance_count,
    };
    VkrVkSurface surface;
    if (!vkr_vk_resolve_surface(source, encoded, thread_id.xy, surface))
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, -1.0f);
        return;
    }
    VkrVulkanInstance instance = source.instances[surface.instance_index];
    surface.object_tangent.w *= instance.normal_column0.w;
    if (!vkr_vk_finish_surface_normal(
            source,
            vkr_instance_transform_normal(instance.normal_column0.xyz,
                                          instance.normal_column1.xyz,
                                          instance.normal_column2.xyz,
                                          surface.object_normal),
            mul(instance.model, float4(surface.object_tangent.xyz, 0.0f)).xyz,
            surface))
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, -1.0f);
        return;
    }
    float2 motion = 0.0f;
    float2 validity = 0.0f;
    if (root.history_valid != 0u &&
        instance.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY)
    {
        VkrTemporalTransform previous =
            root.previous_transforms[instance.temporal_index];
        if (previous.valid != 0u &&
            previous.generation == instance.temporal_generation &&
            previous.frame_index == root.previous_frame_index)
        {
            float4 current_clip = mul(
                root.current_view_projection,
                mul(instance.model, float4(surface.object_position, 1.0f)));
            float4 previous_clip = mul(
                root.previous_view_projection,
                mul(previous.model, float4(surface.object_position, 1.0f)));
            if (current_clip.w > 1e-6f && previous_clip.w > 1e-6f)
            {
                float2 current_ndc = current_clip.xy / current_clip.w;
                float2 previous_ndc = previous_clip.xy / previous_clip.w;
                float2 current_uv =
                    float2(current_ndc.x * 0.5f + 0.5f,
                           current_ndc.y * 0.5f + 0.5f);
                float2 previous_uv =
                    float2(previous_ndc.x * 0.5f + 0.5f,
                           previous_ndc.y * 0.5f + 0.5f);
                motion = previous_uv - current_uv;
                validity = float2(1.0f, previous_clip.z / previous_clip.w);
            }
        }
    }
    g_storage_float2_2d[root.motion_texture][thread_id.xy] = motion;
    g_storage_float2_2d[root.validity_texture][thread_id.xy] = validity;
    float3 f0 = lerp(saturate(root.materials[surface.material_index]
                                  .material_dielectric_specular.rgb),
                     surface.base.rgb, surface.metallic);
    g_storage_2d[root.albedo_texture][thread_id.xy] =
        float4(surface.base.rgb * (1.0f - surface.metallic), surface.occlusion);
    g_storage_2d[root.specular_texture][thread_id.xy] =
        float4(f0, surface.roughness);
    g_storage_float2_2d[root.normal_texture][thread_id.xy] =
        vkr_vk_octahedral_encode(surface.normal);
    g_storage_2d[root.emissive_texture][thread_id.xy] =
        float4(surface.emissive, 1.0f);
    g_storage_2d[root.debug_texture][thread_id.xy] =
        float4(surface.barycentric, surface.selected_lod + 1.0f);
    // Alpha carries metallic so deferred lighting can answer the material
    // debug view without a fifth G-buffer target.
    float3 hdr_seed = root.render_mode == 3u
                          ? surface.base.rgb + surface.emissive
                          : surface.emissive;
    g_storage_2d[root.scene_texture][thread_id.xy] =
        float4(hdr_seed, surface.metallic);
}

// Reconstruction keeps the vertex homogeneous: a pixel whose w crosses the eye
// plane must not be divided by a clamped magnitude alone.
float3 vkr_vk_deferred_homogeneous(float4 value)
{
    return value.xyz / max(abs(value.w), 1e-7f) * sign(value.w);
}

// The deferred lighting path owns the background colour when no cubemap is
// available.
float3 vkr_vk_deferred_sky(VkrVkLightingRoot *root, uint2 pixel)
{
    if (root.sky_enabled == 0u)
        return float3(0.02f, 0.02f, 0.03f);
    // Framebuffer-derived NDC needs no Y flip here: the pipeline's viewport
    // already owns that flip, so inverting the view-projection alone lands in
    // world space. The resolve flips because it performs the viewport
    // transform by hand instead of inverting a matrix.
    float2 ndc = (float2(pixel) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
    float3 far_world = vkr_vk_deferred_homogeneous(
        mul(root.inverse_view_projection, float4(ndc, 1.0f, 1.0f)));
    float3 direction = normalize(far_world - root.frame.view_position.xyz);
    // Pass-root descriptor indices are dynamically uniform; see
    // packet_directional_shadow.
    return g_cube_textures[root.sky_texture]
        .SampleLevel(g_samplers[root.sky_sampler], direction, 0.0f)
        .rgb;
}

float vkr_vk_deferred_filter_roughness(VkrVkLightingRoot *root, uint2 pixel,
                                       uint visible_index, float3 normal,
                                       float roughness)
{
    uint2 limit = root.extent - 1u;
    uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
    uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
    uint visible_x = g_storage_uint2_2d[root.vbuffer_texture][pixel_x].x;
    uint visible_y = g_storage_uint2_2d[root.vbuffer_texture][pixel_y].x;
    float3 normal_x = vkr_vk_octahedral_decode(
        g_storage_float2_2d[root.normal_texture][pixel_x]);
    float3 normal_y = vkr_vk_octahedral_decode(
        g_storage_float2_2d[root.normal_texture][pixel_y]);
    float same_x = visible_x == visible_index ? 1.0f : 0.0f;
    float same_y = visible_y == visible_index ? 1.0f : 0.0f;
    float3 normal_dx = (normal_x - normal) * same_x;
    float3 normal_dy = (normal_y - normal) * same_y;
    float normal_variance =
        0.25f * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
    return sqrt(saturate(
        roughness * roughness + min(normal_variance, 0.25f)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_deferred_lighting(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkLightingRoot *root = g_lighting_push.root;
    if (any(thread_id.xy >= root.extent))
        return;
    VkrVulkanPacketFrameRoot *frame = root.frame;
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy];
    if (encoded.x == 0u)
    {
        // The indirect-diffuse channel carries only surface environment
        // response, so background sky must not contaminate its comparison.
        float3 background = frame.render_mode == 9u
                                ? float3(0.0f, 0.0f, 0.0f)
                                : vkr_vk_deferred_sky(root, thread_id.xy);
        g_storage_2d[root.scene_texture][thread_id.xy] =
            float4(background, 1.0f);
        return;
    }
    float4 seed = g_storage_2d[root.scene_texture][thread_id.xy];
    if (frame.render_mode == 3u)
    {
        g_storage_2d[root.scene_texture][thread_id.xy] = float4(seed.rgb, 1.0f);
        return;
    }
    float4 albedo_ao = g_storage_2d[root.albedo_texture][thread_id.xy];
    float4 f0_roughness = g_storage_2d[root.specular_texture][thread_id.xy];
    float3 normal = vkr_vk_octahedral_decode(
        g_storage_float2_2d[root.normal_texture][thread_id.xy]);
    float3 diffuse_albedo = albedo_ao.rgb;
    float ao = saturate(albedo_ao.a);
    float3 f0 = saturate(f0_roughness.rgb);
    float roughness = clamp(f0_roughness.a, 0.04f, 1.0f);
    if (frame.render_mode == 2u)
    {
        g_storage_2d[root.scene_texture][thread_id.xy] =
            float4(normal * 0.5f + 0.5f, 1.0f);
        return;
    }
    float depth =
        g_depth_textures[root.depth_texture].Load(int3(thread_id.xy, 0));
    // See vkr_vk_deferred_sky: no Y flip when inverting the view-projection.
    float2 ndc_xy =
        (float2(thread_id.xy) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
    float3 world = vkr_vk_deferred_homogeneous(
        mul(root.inverse_view_projection, float4(ndc_xy, depth, 1.0f)));
    roughness = vkr_vk_deferred_filter_roughness(
        root, thread_id.xy, encoded.x, normal, roughness);
    if (frame.render_mode == 6u)
    {
        g_storage_2d[root.scene_texture][thread_id.xy] =
            float4(seed.a, roughness, max(f0.x, max(f0.y, f0.z)), 1.0f);
        return;
    }
    if (frame.shadow_debug_mode != 0u)
    {
        VkrVulkanShadowDebugSample shadow_sample =
            packet_directional_shadow_debug(frame, world, normal);
        g_storage_2d[root.scene_texture][thread_id.xy] = float4(
            packet_shadow_debug_color(frame.shadow_debug_mode, shadow_sample),
            1.0f);
        return;
    }
    float3 view = normalize(frame.view_position.xyz - world);
    // The G-buffer already folded metallic into diffuse albedo and F0, so the
    // shared helpers take a zero metallic factor here.
    float3 analytic_diffuse = 0.0f;
    float3 analytic_specular = 0.0f;
    float directional_shadow = 0.0f;
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        directional_shadow = packet_directional_shadow(frame, world, normal);
        VkrVulkanDirectResult direct = packet_direct(
            normal, view, normalize(-frame.directional_direction_enabled.xyz),
            frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * directional_shadow,
            diffuse_albedo, 0.0f, roughness, f0);
        analytic_diffuse = direct.diffuse;
        analytic_specular = direct.specular;
    }
    VkrVulkanDirectResult punctual = packet_punctual(
        frame, world, normal, view, diffuse_albedo, 0.0f, roughness, f0);
    analytic_diffuse += punctual.diffuse;
    analytic_specular += punctual.specular;
    if (frame.render_mode == 1u || frame.render_mode == 4u ||
        frame.render_mode == 5u)
    {
        float3 debug = frame.render_mode == 4u
                           ? analytic_diffuse
                           : (frame.render_mode == 5u
                                  ? analytic_specular
                                  : analytic_diffuse + analytic_specular);
        g_storage_2d[root.scene_texture][thread_id.xy] = float4(debug, 1.0f);
        return;
    }
    float2 gtao_uv =
        (float2(thread_id.xy) + 0.5f) / float2(root.extent);
    float gtao_visibility =
        g_textures[root.gtao_visibility_texture]
            .SampleLevel(g_samplers[0], gtao_uv, 0.0f)
            .r;
    float3 color = analytic_diffuse + analytic_specular;
    if ((frame.flags & 2u) != 0u)
    {
        VkrVulkanEnvironmentResult environment = packet_environment_terms(
            frame, world, normal, normal, view, diffuse_albedo, 0.0f, roughness,
            ao, f0);
        // Mode 9 isolates the environment diffuse term: no direct light, no
        // specular IBL, no emissive seed, and no ambient fallback. The GTAO
        // multiply is dropped too, so the channel isolates diffuse IBL rather
        // than the occlusion applied on top of it.
        if (frame.render_mode == 9u)
        {
            g_storage_2d[root.scene_texture][thread_id.xy] =
                float4(environment.diffuse, 1.0f);
            return;
        }
        color += environment.diffuse * gtao_visibility +
                 environment.specular;
    }
    else
    {
        // Without an environment the channel has nothing to report, and the
        // constant-ambient fallback must not stand in for one.
        if (frame.render_mode == 9u)
        {
            g_storage_2d[root.scene_texture][thread_id.xy] =
                float4(0.0f, 0.0f, 0.0f, 1.0f);
            return;
        }
        color +=
            frame.ambient_color.rgb * diffuse_albedo * ao * gtao_visibility;
    }
    color += seed.rgb;
    g_storage_2d[root.scene_texture][thread_id.xy] = float4(color, 1.0f);
}

float vkr_vk_temporal_luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

struct VkrVkTemporalHistorySample
{
    float4 color;
    float confidence;
    bool accepted;
};

// Metadata remains on the raw jittered grid. Color is reconstructed onto
// the canonical grid, so each color tap validates its own metadata footprint.
bool vkr_vk_temporal_coverage_support(
    VkrVkTemporalResolveRoot *root, uint2 pixel, uint2 previous_pixel, float2 reference_motion,
    uint2 identity, uint surface_token, float previous_depth)
{
    if (identity.x != 0u && surface_token == 0u)
        return false;
    float2 previous_position = float2(previous_pixel) + 0.5f - root.previous_jitter_pixels;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            uint2 p = uint2(clamp(int2(pixel) + int2(x, y), int2(0), int2(root.extent) - 1));
            float2 valid = g_textures[root.validity_texture].Load(int3(p, 0)).xy;
            if (valid.x != 1.0f || abs(valid.y - previous_depth) > 0.005f)
                continue;
            float2 motion = g_textures[root.motion_texture].Load(int3(p, 0)).xy;
            if (length((motion - reference_motion) * float2(root.extent)) > 0.5f)
                continue;
            float2 projected = float2(p) + 0.5f - root.current_jitter_pixels + motion * float2(root.extent);
            if (any(abs(projected - previous_position) > 1.5f))
                continue;
            if (root.camera_stationary != 0u && length(motion * float2(root.extent)) < 0.01f)
            {
                float depth = g_depth_textures[root.depth_texture].Load(int3(p, 0));
                if (abs(depth - valid.y) > 0.005f)
                    continue;
            }
            uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][p];
            if ((encoded.x & 0x80000000u) != 0u ||
                (root.transmission_enabled != 0u && g_storage_uint2_images[root.transmission_vbuffer_texture][uint3(p, 0u)].x != 0u))
                continue;
            uint2 neighbor_identity = 0u;
            uint neighbor_surface = 0u;
            if (encoded.x != 0u)
            {
                VkrGpuVisibleDrawRow row = root.visible_rows[encoded.x - 1u];
                VkrVulkanInstance instance = root.instances[row.instance_index];
                neighbor_identity = uint2(instance.temporal_index + 1u, instance.temporal_generation);
                neighbor_surface = instance.temporal_flags >> 1u;
            }
            if (all(neighbor_identity == identity) && neighbor_surface == surface_token)
                return true;
        }
    return false;
}

VkrVkTemporalHistorySample vkr_vk_temporal_history_sample(
    VkrVkTemporalResolveRoot *root, float2 history_uv, uint2 identity,
    uint surface_token, float expected_depth, bool check_depth,
    uint2 current_pixel, float2 reference_motion, bool allow_coverage)
{


    float2 texel = history_uv * float2(root.extent) - 0.5f;
    int2 base = int2(floor(texel));
    float2 fraction = frac(texel);
    bool cubic_candidate = check_depth && any(fraction != 0.0f) &&
                           all(base >= 1) && all(base + 2 < int2(root.extent));
    int2 color_span = int2(cubic_candidate || fraction.x != 0.0f ? 2 : 1,
                          cubic_candidate || fraction.y != 0.0f ? 2 : 1);
    float2 metadata_fraction = frac(root.previous_jitter_pixels);
    int2 extra = int2(metadata_fraction.x > 0.0f ? 1 : 0, metadata_fraction.y > 0.0f ? 1 : 0);
    // Clamp canonical color taps first: their source footprints belong to
    // the clamped color position, including when bilinear taps hit an edge.
    int2 color_base = clamp(base, int2(0), int2(root.extent) - 1);
    int2 color_upper = clamp(base + color_span - 1, int2(0), int2(root.extent) - 1);
    int2 metadata_base = color_base + int2(floor(root.previous_jitter_pixels));
    int2 metadata_span = color_upper - color_base + 1 + extra;
    // The canonical inner 2x2 needs at most nine distinct raw metadata taps.
    uint metadata_mask = 0u;
    bool cubic_compatible = cubic_candidate;
    for (int y = 0; y < metadata_span.y; ++y)
        for (int x = 0; x < metadata_span.x; ++x)
        {
            uint2 p = uint2(clamp(metadata_base + int2(x, y), int2(0), int2(root.extent) - 1));
            uint2 previous_identity = g_storage_uint2_2d[root.history_identity_texture][p];
            uint previous_surface = g_storage_uint_2d[root.history_surface_texture][p];
            float previous_depth = g_storage_float2_2d[root.history_depth_texture][p].x;
            bool strict = all(previous_identity == identity) && previous_surface == surface_token &&
                          (!check_depth || abs(previous_depth - expected_depth) <= 0.005f);
            cubic_compatible = cubic_compatible && strict;
            bool accepted = strict;
            if (!accepted && allow_coverage)
                accepted = vkr_vk_temporal_coverage_support(root, current_pixel, p, reference_motion,
                                                     previous_identity, previous_surface, previous_depth);
            metadata_mask |= accepted ? 1u << uint(y * 3 + x) : 0u;
        }
    // Cubic color spans four canonical taps per axis; validate only the raw
    // outer ring not already cached above (at most 25 metadata taps total).
    if (cubic_compatible)
        for (int y = -1; y <= 2 + extra.y && cubic_compatible; ++y)
            for (int x = -1; x <= 2 + extra.x && cubic_compatible; ++x)
            {
                if (x >= 0 && x < metadata_span.x && y >= 0 && y < metadata_span.y)
                    continue;
                uint2 p = uint2(clamp(metadata_base + int2(x, y), int2(0), int2(root.extent) - 1));
                if (any(g_storage_uint2_2d[root.history_identity_texture][p] != identity) || g_storage_uint_2d[root.history_surface_texture][p] != surface_token ||
                    abs(g_storage_float2_2d[root.history_depth_texture][p].x - expected_depth) > 0.005f)
                    cubic_compatible = false;
            }
    VkrVkTemporalHistorySample result;
    result.color = 0.0f;
    result.confidence = 0.0f;
    result.accepted = false;
    if (cubic_compatible)
    {
        float4 wx = vkr_temporal_cubic_weights(fraction.x);
        float4 wy = vkr_temporal_cubic_weights(fraction.y);
        float3 weights_x = float3(wx.x, wx.y + wx.z, wx.w);
        float3 weights_y = float3(wy.x, wy.y + wy.z, wy.w);
        float3 coords_x = (float(base.x) + float3(-1.0f, wx.z / weights_x.y, 2.0f) + 0.5f) / float(root.extent.x);
        float3 coords_y = (float(base.y) + float3(-1.0f, wy.z / weights_y.y, 2.0f) + 0.5f) / float(root.extent.y);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                result.color += g_textures[root.history_color_texture].SampleLevel(g_samplers[root.history_sampler], float2(coords_x[x], coords_y[y]), 0.0f) * (weights_x[x] * weights_y[y]);
        result.accepted = true;
        result.confidence = 1.0f;
        return result;
    }
    float weight_sum = 0.0f;
    for (int y = 0; y < color_span.y; ++y)
        for (int x = 0; x < color_span.x; ++x)
        {
            float weight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                           (y == 0 ? 1.0f - fraction.y : fraction.y);
            if (weight <= 1e-5f)
                continue;
            uint2 p = uint2(clamp(base + int2(x, y), int2(0), int2(root.extent) - 1));
            int2 metadata_offset = int2(p) - color_base;
            float confidence = 0.0f;
            for (int sy = 0; sy <= extra.y; ++sy)
                for (int sx = 0; sx <= extra.x; ++sx)
                {
                    uint bit = 1u << uint((metadata_offset.y + sy) * 3 + metadata_offset.x + sx);
                    float metadata_weight =
                        (sx == 0 ? 1.0f - metadata_fraction.x : metadata_fraction.x) *
                        (sy == 0 ? 1.0f - metadata_fraction.y : metadata_fraction.y);
                    confidence += (metadata_mask & bit) != 0u ? metadata_weight : 0.0f;
                }
            // Preserve partial support both when reconstructing color and
            // when choosing retention; normalization alone loses confidence.
            weight *= confidence;
            if (weight <= 1e-5f)
                continue;
            result.color += g_textures[root.history_color_texture].Load(int3(p, 0)) * weight;
            weight_sum += weight;
        }
    result.accepted = weight_sum > 1e-5f;
    result.confidence = result.accepted ? saturate(weight_sum) : 0.0f;
    result.color = result.accepted ? result.color / weight_sum : float4(0.0f);
    return result;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_temporal_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkTemporalResolveRoot *root = g_temporal_resolve_push.root;
    if (any(thread_id.xy >= root.extent))
        return;

    uint2 pixel = thread_id.xy;
    // Projection shifts raster samples by +jitter. Reconstruct current color
    // at canonical pixel p from raw samples around p + jitter.
    float4 current = 0.0f;
    float current_weight = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float weight = vkr_temporal_current_weight(float2(x, y), root.current_jitter_pixels);
            if (weight <= 0.0f)
                continue;
            uint2 p = uint2(clamp(int2(pixel) + int2(x, y), int2(0), int2(root.extent) - 1));
            current += g_textures[root.scene_texture].Load(int3(p, 0)) * weight;
            current_weight += weight;
        }
    current /= current_weight;
    float surface_depth =
        g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][pixel];
    const uint overlay_bit = 0x80000000u;
    const uint generation_mask = 0x7fffffffu;
    const uint surface_mask = 0x1ffffu;

    // Keep metadata on the raw center grid; history lookup accounts for jitter.
    float2 motion = g_textures[root.motion_texture].Load(int3(pixel, 0)).xy;
    float2 validity = g_textures[root.validity_texture].Load(int3(pixel, 0)).xy;
    bool blend_overlay = (encoded.x & overlay_bit) != 0u;
    bool check_history_depth = !blend_overlay;
    uint2 identity = 0u;
    uint surface_token = 0u;
    if (blend_overlay)
    {
        uint generation = encoded.x & generation_mask;
        if (encoded.y != 0u && generation != 0u)
        {
            identity = uint2((encoded.y >> 17u) + 1u, generation);
            surface_token = encoded.y & surface_mask;
        }
    }
    else
    {
        uint2 transmission_encoded =
            root.transmission_enabled != 0u
                ? g_storage_uint2_images[root.transmission_vbuffer_texture]
                                        [uint3(pixel, 0u)]
                : uint2(0u);
        if (transmission_encoded.x != 0u)
        {
            VkrGpuVisibleDrawRow visible =
                root.transmission_visible_rows[transmission_encoded.x - 1u];
            VkrVulkanInstance instance =
                root.transmission_instances[visible.instance_index];
            identity =
                uint2(instance.temporal_index + 1u,
                      instance.temporal_generation);
            surface_token = instance.temporal_flags >> 1u;
            surface_depth = g_depth_array_textures
                [root.transmission_depth_texture]
                    .Load(int4(pixel, 0u, 0));
        }
        else if (encoded.x != 0u)
        {
            VkrGpuVisibleDrawRow visible = root.visible_rows[encoded.x - 1u];
            VkrVulkanInstance instance = root.instances[visible.instance_index];
            identity =
                uint2(instance.temporal_index + 1u,
                      instance.temporal_generation);
            surface_token = instance.temporal_flags >> 1u;
        }
    }

    // CPU scene/resource equality proves that missing current coverage is
    // another sample of the same radiance field, including thin transmission.
    bool checked_static = root.scene_stationary != 0u && root.history_valid != 0u &&
                          validity.x > 0.5f && !(blend_overlay && identity.x == 0u) &&
                          (identity.x == 0u || surface_token != 0u);
    if (checked_static)
    {

        float2 uv = (float2(pixel) + 0.5f) / float2(root.extent);
        float previous_age = g_storage_float2_2d[root.history_depth_texture][pixel].y;
        float age = min(previous_age + 1.0f, VKR_TEMPORAL_STATIC_SAMPLE_COUNT);
        float authored_reactive = validity.x >= 2.0f ? saturate(validity.x - 2.0f) : 0.0f;
        float history_weight = ((age - 1.0f) / age) * (1.0f - authored_reactive);
        float4 history = g_textures[root.history_color_texture].Load(int3(pixel, 0));
        // A capped EMA still oscillates with thin-coverage jitter forever.
        // Copy the completed static integral exactly; any scene change resets
        // its age through the ordinary resolve before this path can resume.
        float4 resolved = previous_age >= VKR_TEMPORAL_STATIC_SAMPLE_COUNT && authored_reactive == 0.0f
                              ? history
                              : float4(lerp(current.rgb, history.rgb, history_weight), current.a);
        if (root.render_mode == 7u)
            resolved = float4(saturate(abs(motion) * float2(root.extent) / 16.0f), validity.x, 1.0f);
        else if (root.render_mode == 8u)
            resolved = float4(0.0f, 1.0f, 0.0f, 1.0f);
        g_storage_2d[root.output_color_texture][pixel] = resolved;
        g_storage_float2_2d[root.output_depth_texture][pixel] = float2(surface_depth, age);
        g_storage_uint2_2d[root.output_identity_texture][pixel] = identity;
        g_storage_uint_2d[root.output_surface_texture][pixel] = surface_token;
        return;
    }

    // Optical contrast is used only by the normal temporal resolve. The
    // checked static path returned before these texture reads and RGB state.
    float3 pre_transmission = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float weight = vkr_temporal_current_weight(float2(x, y), root.current_jitter_pixels);
            if (weight <= 0.0f)
                continue;
            uint2 p = uint2(clamp(int2(pixel) + int2(x, y), int2(0), int2(root.extent) - 1));
            pre_transmission += g_textures[root.pre_transmission_texture].Load(int3(p, 0)).rgb * weight;
        }
    pre_transmission /= current_weight;

    float current_luminance = vkr_vk_temporal_luminance(current.rgb);
    float pre_luminance = vkr_vk_temporal_luminance(pre_transmission);
    float derived_reactive =
        min(0.75f,
                  saturate(abs(current_luminance - pre_luminance) /
                           max(max(current_luminance, pre_luminance), 0.05f)));
    float authored_reactive =
        validity.x >= 2.0f ? saturate(validity.x - 2.0f) : 0.0f;
    float reactive = max(derived_reactive, authored_reactive);
    float2 uv = (float2(pixel) + 0.5f) / float2(root.extent);
    float2 history_uv = uv + motion;
    float motion_pixels = length(motion * float2(root.extent));
    bool stationary_surface =
        root.camera_stationary != 0u && motion_pixels < 0.01f;
    bool allow_coverage = validity.x == 1.0f && reactive == 0.0f && !blend_overlay &&
                          (!stationary_surface || abs(surface_depth - validity.y) <= 0.005f);
    bool accepted =
        root.history_valid != 0u &&
        !(blend_overlay && identity.x == 0u) &&
        validity.x > 0.5f && (identity.x == 0u || surface_token != 0u) &&
        all(history_uv >= 0.0f) && all(history_uv <= 1.0f);
    // The output slot is distinct from accepted history inputs. Publish raw
    // metadata now so depth need not stay live through history reconstruction.
    g_storage_float2_2d[root.output_depth_texture][pixel] =
        float2(surface_depth, 0.0f);
    g_storage_uint2_2d[root.output_identity_texture][pixel] = identity;
    g_storage_uint_2d[root.output_surface_texture][pixel] = surface_token;
    float4 history = current;
    float history_confidence = 0.0f;
    if (accepted)
    {
        VkrVkTemporalHistorySample sample = vkr_vk_temporal_history_sample(
            root, history_uv, identity, surface_token, validity.y,
            check_history_depth, pixel, motion, allow_coverage);
        accepted = sample.accepted;
        history_confidence = sample.confidence;
        history = accepted ? sample.color : current;
    }

    if (accepted)
    {
        float3 neighborhood_min = current.rgb;
        float3 neighborhood_max = current.rgb;
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                int2 sample_pixel =
                    clamp(int2(pixel) + int2(x, y), int2(0),
                          int2(root.extent) - 1);
                float3 sample_color =
                    g_textures[root.scene_texture]
                        .Load(int3(sample_pixel, 0))
                        .rgb;
                neighborhood_min = min(neighborhood_min, sample_color);
                neighborhood_max = max(neighborhood_max, sample_color);
            }
        }
        history.rgb = clamp(history.rgb, neighborhood_min, neighborhood_max);
    }
    // A fixed 0.9 EMA preserves a visible periodic residual from the
    // eight-phase jitter sequence. Once both the camera and surface are
    // stationary, retain enough history to make that residual sub-perceptual.
    // Moving surfaces keep the responsive path even when the camera is still.
    float history_retention = stationary_surface ? 0.99f : 0.9f;
    float history_weight = accepted
                               ? history_retention * history_confidence * (1.0f - reactive) *
                                     saturate(1.0f - motion_pixels / 128.0f)
                               : 0.0f;
    float4 resolved =
        float4(lerp(current.rgb, history.rgb, history_weight), current.a);
    if (root.render_mode == 7u)
        resolved = float4(saturate(abs(motion) * float2(root.extent) / 16.0f),
                          validity.x, 1.0f);
    else if (root.render_mode == 8u)
        resolved = accepted ? float4(0.0f, 1.0f, 0.0f, 1.0f)
                            : float4(1.0f, 0.0f, 0.0f, 1.0f);

    g_storage_2d[root.output_color_texture][pixel] = resolved;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_hzb_build(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkHzbRoot *root = g_hzb_push.root;
    if (any(thread_id.xy >= root.destination_extent))
        return;
    if (root.source_is_depth == 2u)
    {
        float depth = g_depth_textures[root.source_texture].Load(
            int3(min(thread_id.xy, root.source_extent - 1u), 0));
        g_storage_2d[root.destination_texture][thread_id.xy] =
            float4(depth, 0.0f, 0.0f, 0.0f);
        return;
    }
    uint2 source_begin = thread_id.xy * 2u;
    uint2 source_end = min(source_begin + 2u, root.source_extent);
    // Floor-sized mips assign the unpaired source edge to the last texel.
    if (thread_id.x + 1u == root.destination_extent.x)
        source_end.x = root.source_extent.x;
    if (thread_id.y + 1u == root.destination_extent.y)
        source_end.y = root.source_extent.y;
    float maximum = 0.0f;
    for (uint y = source_begin.y; y < source_end.y; ++y)
    {
        for (uint x = source_begin.x; x < source_end.x; ++x)
        {
            uint2 p = uint2(x, y);
            float value =
                root.source_is_depth != 0u
                    ? g_depth_textures[root.source_texture].Load(int3(p, 0))
                    : g_storage_2d[root.source_texture][p].x;
            maximum = max(maximum, value);
        }
    }
    g_storage_2d[root.destination_texture][thread_id.xy] =
        float4(maximum, 0.0f, 0.0f, 0.0f);
}
[shader("compute")]
[numthreads(16, 16, 1)]
void vk_sdsm_reduce(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSdsmRoot *root = g_sdsm_push.root;
    bool occupied = false;
    float depth = 0.0f;
    if (all(thread_id.xy < root.extent))
    {
        occupied = g_uint2_textures[root.vbuffer_texture]
                       .Load(int3(thread_id.xy, 0))
                       .x != 0u;
        if (occupied)
            depth = g_depth_textures[root.depth_texture].Load(
                int3(thread_id.xy, 0));
    }
    uint wave_occupied = WaveActiveCountBits(occupied);
    float wave_minimum = WaveActiveMin(occupied ? depth : 1.0f);
    float wave_maximum = WaveActiveMax(occupied ? depth : 0.0f);
    if (WaveIsFirstLane() && wave_occupied != 0u)
    {
        uint ignored;
        InterlockedMin(root.reduce_state->min_device_z_bits,
                       asuint(wave_minimum), ignored);
        InterlockedMax(root.reduce_state->max_device_z_bits,
                       asuint(wave_maximum), ignored);
        InterlockedAdd(root.reduce_state->occupied_count, wave_occupied,
                       ignored);
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void vk_picking_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkPickingRoot *root = g_picking_push.root;
    uint2 opaque_encoded = g_storage_uint2_2d[root.opaque_vbuffer][root.pixel];
    uint2 transmission_encoded =
        root.use_transmission != 0u
            ? g_storage_uint2_images[root.transmission_vbuffer]
                                    [uint3(root.pixel, root.transmission_layer)]
            : uint2(0u);
    bool selected_transmission = transmission_encoded.x != 0u;
    uint2 encoded =
        selected_transmission ? transmission_encoded : opaque_encoded;
    uint identifier = 0u;
    if (encoded.x != 0u)
    {
        VkrGpuVisibleDrawRow visible =
            selected_transmission ? root.transmission_visible[encoded.x - 1u]
                                  : root.opaque_visible[encoded.x - 1u];
        identifier =
            selected_transmission
                ? root.transmission_instances[visible.instance_index].object_id
                : root.opaque_instances[visible.instance_index].object_id;
    }
    g_storage_uint_2d[root.output_texture][root.pixel] = identifier;
}

// The 8x8 group can contain at most 64 subgroups. Do not assume a native
// subgroup width: Vulkan only guarantees the device-reported width at runtime.
groupshared uint g_transmission_compact_thin_wave_totals[64];
groupshared uint g_transmission_compact_extended_wave_totals[64];
groupshared uint g_transmission_compact_group_bases[2];

[shader("compute")]
[numthreads(1, 1, 1)]
void vk_transmission_compact_clear(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkTransmissionCompactRoot *root = g_transmission_compact_push.root;
    root.indirect_arguments[6] = 0u;
    root.indirect_arguments[7] = 0u;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_compact(uint3 thread_id : SV_DispatchThreadID,
                             uint group_index : SV_GroupIndex)
{
    VkrVkTransmissionCompactRoot *root = g_transmission_compact_push.root;
    bool in_extent = all(thread_id.xy < root.extent);
    if (in_extent)
        g_storage_2d[root.destination_texture][thread_id.xy] =
            g_storage_2d[root.source_texture][thread_id.xy];
    bool covered = in_extent &&
        g_storage_uint2_images[root.vbuffer_texture]
                              [uint3(thread_id.xy, root.layer)].x != 0u;
    uint partition = 0u;
    if (covered)
    {
        uint visible_index =
            g_storage_uint2_images[root.vbuffer_texture]
                                  [uint3(thread_id.xy, root.layer)].x - 1u;
        uint material_index = root.visible_rows[visible_index].material_index;
        VkrVulkanMaterial material = root.materials[material_index];
        bool extended = (material.flags & 8u) != 0u ||
                        material.material_alpha.w > 0.0f;
        partition = extended ? 1u : 0u;
    }
    uint wave_index = WaveGetWaveIndex();
    uint wave_count = WaveGetNumWaves();
    bool thin_covered = covered && partition == 0u;
    bool extended_covered = covered && partition == 1u;
    uint thin_lane_offset = WavePrefixCountBits(thin_covered);
    uint extended_lane_offset = WavePrefixCountBits(extended_covered);
    uint thin_wave_total = WaveActiveCountBits(thin_covered);
    uint extended_wave_total = WaveActiveCountBits(extended_covered);
    if (WaveIsFirstLane())
    {
        g_transmission_compact_thin_wave_totals[wave_index] =
            thin_wave_total;
        g_transmission_compact_extended_wave_totals[wave_index] =
            extended_wave_total;
    }
    GroupMemoryBarrierWithGroupSync();

    uint thin_wave_base = 0u;
    uint extended_wave_base = 0u;
    for (uint i = 0u; i < wave_index; ++i)
    {
        thin_wave_base += g_transmission_compact_thin_wave_totals[i];
        extended_wave_base += g_transmission_compact_extended_wave_totals[i];
    }
    if (group_index == 0u)
    {
        uint thin_total = 0u;
        uint extended_total = 0u;
        for (uint i = 0u; i < wave_count; ++i)
        {
            thin_total += g_transmission_compact_thin_wave_totals[i];
            extended_total += g_transmission_compact_extended_wave_totals[i];
        }
        uint ignored;
        InterlockedAdd(root.indirect_arguments[6], thin_total,
                       g_transmission_compact_group_bases[0]);
        InterlockedAdd(root.indirect_arguments[7], extended_total,
                       g_transmission_compact_group_bases[1]);
        uint thin_writable =
            g_transmission_compact_group_bases[0] < root.capacity
            ? min(thin_total,
                  root.capacity - g_transmission_compact_group_bases[0])
            : 0u;
        uint extended_writable =
            g_transmission_compact_group_bases[1] < root.capacity
            ? min(extended_total,
                  root.capacity - g_transmission_compact_group_bases[1])
            : 0u;
        uint overflow = thin_total - thin_writable + extended_total -
                        extended_writable;
        InterlockedAdd(root.covered_pixels[root.layer],
                       thin_total + extended_total, ignored);
        if (overflow != 0u)
        {
            InterlockedAdd(root.overflow_counts[root.layer], overflow, ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (covered)
    {
        uint destination = g_transmission_compact_group_bases[partition] +
                           (partition == 0u ? thin_wave_base
                                            : extended_wave_base) +
                           (partition == 0u ? thin_lane_offset
                                            : extended_lane_offset);
        if (destination < root.capacity)
            root.pixel_list[partition * root.capacity + destination] =
                (thread_id.y << 16u) | thread_id.x;
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void vk_transmission_compact_finalize(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkTransmissionCompactRoot *root = g_transmission_compact_push.root;
    uint total = 0u;
    for (uint partition = 0u; partition < 2u; ++partition)
        total += min(root.indirect_arguments[6u + partition], root.capacity);
    root.indirect_arguments[0] = (total + 63u) / 64u;
    root.indirect_arguments[1] = 1u;
    root.indirect_arguments[2] = 1u;
}

void vkr_vk_transmission_shade_impl(uint3 thread_id, uint partition,
                                    bool textured_enabled,
                                    bool volume_enabled,
                                    bool diagnostic_enabled,
                                    uint temporal_mode)
{
    VkrVkTransmissionRoot *root = g_transmission_push.root;
    uint2 pixel = thread_id.xy;
    if (root.compact_enabled != 0u)
    {
        if (root.compact_layer >= 4u || thread_id.x >=
            min(root.compact_counts[partition], root.pixel_capacity))
            return;
        uint packed =
            root.pixel_list[partition * root.pixel_capacity + thread_id.x];
        pixel = uint2(packed & 0xffffu, packed >> 16u);
    }
    if (any(pixel >= root.extent))
        return;
    float2 screen_uv = (float2(pixel) + 0.5f) / float2(root.extent);
    uint2 encoded = g_storage_uint2_images[root.vbuffer_texture]
                                          [uint3(pixel, root.layer)];
    if (encoded.x == 0u)
    {
        g_storage_2d[root.output_texture][pixel] =
            g_textures[root.feedback_texture].SampleLevel(
                g_samplers[root.feedback_sampler], screen_uv, 0.0f);
        return;
    }
    bool temporal_outputs = temporal_mode == 1u ||
        (temporal_mode == 2u && root.layer == 0u);
    if (temporal_outputs)
    {
        g_storage_float2_2d[root.motion_texture][pixel] = 0.0f;
        g_storage_float2_2d[root.validity_texture][pixel] = 0.0f;
    }
    VkrVkSurfaceSource source = {
        root.geometry_rows,    root.visible_rows,    root.instances,
        root.materials,        root.vertices,        root.indices,
        root.compaction_state, root.view_projection, root.extent,
        root.visible_capacity, root.geometry_count,  root.material_count,
        root.instance_count,
    };
    VkrVkSurface surface;
    if (!vkr_vk_resolve_surface(source, encoded, pixel, surface))
    {
        g_storage_2d[root.output_texture][pixel] =
            g_textures[root.feedback_texture].SampleLevel(
                g_samplers[root.feedback_sampler], screen_uv, 0.0f);
        return;
    }
    VkrVulkanInstance instance = source.instances[surface.instance_index];
    surface.object_tangent.w *= instance.normal_column0.w;
    if (!vkr_vk_finish_surface_normal(
            source,
            vkr_instance_transform_normal(instance.normal_column0.xyz,
                                          instance.normal_column1.xyz,
                                          instance.normal_column2.xyz,
                                          surface.object_normal),
            mul(instance.model, float4(surface.object_tangent.xyz, 0.0f)).xyz,
            surface))
    {
        g_storage_2d[root.output_texture][pixel] =
            g_textures[root.feedback_texture].SampleLevel(
                g_samplers[root.feedback_sampler], screen_uv, 0.0f);
        return;
    }
    VkrVulkanPacketFrameRoot *frame = root.frame;
    VkrVulkanMaterial material = root.materials[surface.material_index];
    if (temporal_outputs && root.history_valid != 0u &&
        instance.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY)
    {
        VkrTemporalTransform previous =
            root.previous_transforms[instance.temporal_index];
        if (previous.valid != 0u &&
            previous.generation == instance.temporal_generation &&
            previous.frame_index == root.previous_frame_index)
        {
            float4 current_clip = mul(
                root.current_view_projection,
                mul(instance.model, float4(surface.object_position, 1.0f)));
            float4 previous_clip = mul(
                root.previous_view_projection,
                mul(previous.model, float4(surface.object_position, 1.0f)));
            if (current_clip.w > 1e-6f && previous_clip.w > 1e-6f)
            {
                float2 current_uv =
                    current_clip.xy / current_clip.w * 0.5f + 0.5f;
                float2 previous_uv =
                    previous_clip.xy / previous_clip.w * 0.5f + 0.5f;
                g_storage_float2_2d[root.motion_texture][pixel] =
                    previous_uv - current_uv;
                g_storage_float2_2d[root.validity_texture][pixel] =
                    float2(2.0f + saturate(material.temporal_reactivity),
                           previous_clip.z / previous_clip.w);
            }
        }
    }
    float depth = g_depth_array_textures[root.depth_texture].Load(
        int4(pixel, root.layer, 0));
    // See vkr_vk_deferred_sky: no Y flip when inverting the view-projection.
    float2 ndc_xy = screen_uv * 2.0f - 1.0f;
    float3 world = vkr_vk_deferred_homogeneous(
        mul(root.inverse_view_projection, float4(ndc_xy, depth, 1.0f)));
    float3 f0 = lerp(saturate(material.material_dielectric_specular.rgb),
                     surface.base.rgb, surface.metallic);
    float3 view = normalize(frame.view_position.xyz - world);
    float no_v = saturate(dot(surface.normal, view));
    bool composition_enabled = !diagnostic_enabled ||
        (frame.render_mode == 0u && frame.shadow_debug_mode == 0u);
    float transmission = saturate(material.material_alpha.y);
    if (composition_enabled && textured_enabled &&
        (material.flags & 8u) != 0u)
    {
        VkrVulkanTransmissionMaterial transmission_material =
            root.transmission_materials[surface.material_index];
        transmission *=
            g_textures[NonUniformResourceIndex(
                           transmission_material.transmission_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(
                                transmission_material.transmission_sampler)],
                            surface.uv, surface.uv_dx, surface.uv_dy)
                .r;
    }
    transmission = saturate(transmission);
    // The composition contract multiplies diffuse by zero at resolved T == 1.
    // Diagnostics retain the general lobe so term replays remain observable.
    bool diffuse_enabled = diagnostic_enabled || transmission < 1.0f;
    float3 transmitted = 0.0f;
    if (!diagnostic_enabled ||
        (frame.shadow_debug_mode == 0u && frame.render_mode != 1u &&
         frame.render_mode != 2u && frame.render_mode != 3u &&
         frame.render_mode != 4u && frame.render_mode != 5u &&
         frame.render_mode != 6u && frame.render_mode != 9u))
    {
        float thickness =
            volume_enabled ? max(material.material_alpha.w, 0.0f) : 0.0f;
        if (volume_enabled && textured_enabled && (material.flags & 16u) != 0u)
        {
            VkrVulkanTransmissionMaterial transmission_material =
                root.transmission_materials[surface.material_index];
            thickness *=
                g_textures[NonUniformResourceIndex(
                               transmission_material.thickness_texture)]
                    .SampleGrad(g_samplers[NonUniformResourceIndex(
                                    transmission_material.thickness_sampler)],
                                surface.uv, surface.uv_dx, surface.uv_dy)
                    .g;
        }
        thickness = max(thickness, 0.0f);
        float2 sample_uv = screen_uv;
        float path_length = 0.0f;
        if (volume_enabled)
        {
            VkrTransmissionExit exit = vkr_transmission_exit_point(
                world, frame.view_position.xyz, surface.normal,
                mul(instance.model, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz,
                mul(instance.model, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz,
                mul(instance.model, float4(0.0f, 0.0f, 1.0f, 0.0f)).xyz,
                material.material_alpha.z, thickness);
            float4 exit_clip =
                mul(root.view_projection, float4(exit.position, 1.0f));
            sample_uv =
                clamp(vkr_transmission_project_uv(exit_clip, 1.0f), 0.0f, 1.0f);
            path_length = exit.path_length;
        }
        float3 ordered =
            g_textures[root.feedback_texture]
                .SampleLevel(g_samplers[root.feedback_sampler], sample_uv, 0.0f)
                .rgb;
        float rough_lod = vkr_transmission_rough_lod(surface.feedback_roughness,
                                                     material.material_alpha.z,
                                                     root.opaque_mip_count);
        float3 rough = g_textures[root.opaque_texture]
                           .SampleLevel(g_samplers[root.feedback_sampler],
                                        sample_uv, rough_lod)
                           .rgb;
        transmitted =
            lerp(ordered, rough,
                 vkr_transmission_feedback_blend(surface.feedback_roughness));
        if (volume_enabled && material.material_attenuation_color.w > 1e-4f)
        {
            transmitted *=
                pow(clamp(material.material_attenuation_color.rgb, 1e-4f, 1.0f),
                    path_length / material.material_attenuation_color.w);
        }
    }
    float3 color;
    // Transmissive surfaces keep their unfolded base colour and metallic
    // factor, so the shared helpers take the same arguments the forward
    // fragment shader passes rather than the G-buffer's folded albedo.
    if (diagnostic_enabled && frame.render_mode == 2u)
    {
        color = surface.normal * 0.5f + 0.5f;
    }
    else if (diagnostic_enabled && frame.render_mode == 3u)
    {
        color = surface.base.rgb + surface.emissive;
    }
    else if (diagnostic_enabled && frame.render_mode == 6u)
    {
        color = float3(surface.metallic, surface.roughness,
                       max(f0.x, max(f0.y, f0.z)));
    }
    else if (diagnostic_enabled && frame.shadow_debug_mode != 0u)
    {
        VkrVulkanShadowDebugSample shadow_sample =
            packet_directional_shadow_debug(frame, world, surface.normal);
        color =
            packet_shadow_debug_color(frame.shadow_debug_mode, shadow_sample);
    }
    else
    {
        float3 analytic_diffuse = 0.0f;
        float3 analytic_specular = 0.0f;
        float directional_shadow = 0.0f;
        if (frame.directional_direction_enabled.w > 0.5f)
        {
            directional_shadow =
                packet_directional_shadow(frame, world, surface.normal);
            float3 light =
                normalize(-frame.directional_direction_enabled.xyz);
            float3 radiance = frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * directional_shadow;
            if (diffuse_enabled)
            {
                VkrVulkanDirectResult direct = packet_direct(
                    surface.normal, view, light, radiance, surface.base.rgb,
                    surface.metallic, surface.roughness, f0);
                analytic_diffuse = direct.diffuse;
                analytic_specular = direct.specular;
            }
            else
            {
                analytic_specular = packet_direct_specular(
                    surface.normal, view, light, radiance, surface.roughness,
                    f0);
            }
        }
        if (diffuse_enabled)
        {
            VkrVulkanDirectResult punctual = packet_punctual(
                frame, world, surface.normal, view, surface.base.rgb,
                surface.metallic, surface.roughness, f0);
            analytic_diffuse += punctual.diffuse;
            analytic_specular += punctual.specular;
        }
        else
        {
            analytic_specular += packet_punctual_specular(
                frame, world, surface.normal, view, surface.roughness, f0);
        }
        if (diagnostic_enabled &&
            (frame.render_mode == 1u || frame.render_mode == 4u ||
             frame.render_mode == 5u))
        {
            color = frame.render_mode == 4u
                        ? analytic_diffuse
                        : (frame.render_mode == 5u
                               ? analytic_specular
                               : analytic_diffuse + analytic_specular);
        }
        else if (diagnostic_enabled && frame.render_mode == 9u)
        {
            // Mode 9 on the transmission path reports the same environment
            // diffuse term the opaque path does, so a transmissive surface does
            // not read as black in the comparison channel.
            color = 0.0f;
            if ((frame.flags & 2u) != 0u)
            {
                color = packet_environment_terms(
                            frame, world, surface.normal, surface.normal, view,
                            surface.base.rgb, surface.metallic,
                            surface.roughness, surface.occlusion, f0)
                            .diffuse;
            }
        }
        else
        {
            float3 diffuse = analytic_diffuse;
            float3 specular = analytic_specular;
            if ((frame.flags & 2u) != 0u)
            {
                if (diffuse_enabled)
                {
                    VkrVulkanEnvironmentResult environment =
                        packet_environment_terms(
                            frame, world, surface.normal, surface.normal, view,
                            surface.base.rgb, surface.metallic,
                            surface.roughness, surface.occlusion, f0);
                    diffuse += environment.diffuse;
                    specular += environment.specular;
                }
                else
                {
                    specular += packet_environment_specular(
                        frame, world, surface.normal, surface.normal, view,
                        surface.roughness, surface.occlusion, f0);
                }
            }
            else if (diffuse_enabled)
            {
                diffuse += frame.ambient_color.rgb * surface.base.rgb *
                           surface.occlusion;
            }
            VkrTransmissionLobes lobes = {
                diffuse, specular, surface.emissive
            };
            color = vkr_transmission_compose(
                lobes, transmitted, surface.base.rgb,
                packet_fresnel(no_v, f0), transmission, surface.metallic);
        }
    }
    g_storage_2d[root.output_texture][pixel] = float4(color, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_shade(uint3 thread_id : SV_DispatchThreadID)
{
    vkr_vk_transmission_shade_impl(thread_id, 0u, true, true, true, 2u);
}

void vkr_vk_transmission_shade_partitioned_impl(uint3 thread_id,
                                                bool diagnostic_enabled,
                                                uint temporal_mode)
{
    VkrVkTransmissionRoot *root = g_transmission_push.root;
    uint compact_index = thread_id.x;
    uint thin_factor_count = min(root.compact_counts[0], root.pixel_capacity);
    if (compact_index < thin_factor_count)
    {
        vkr_vk_transmission_shade_impl(thread_id, 0u, false, false,
                                       diagnostic_enabled, temporal_mode);
        return;
    }
    compact_index -= thin_factor_count;
    vkr_vk_transmission_shade_impl(uint3(compact_index, 0u, 0u), 1u,
                                   true, true, diagnostic_enabled,
                                   temporal_mode);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_transmission_shade_partitioned(uint3 thread_id : SV_DispatchThreadID)
{
    vkr_vk_transmission_shade_partitioned_impl(thread_id, true, 2u);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_shade_production(
    uint3 thread_id : SV_DispatchThreadID)
{
    vkr_vk_transmission_shade_impl(thread_id, 0u, true, true, false, 0u);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_shade_production_temporal(
    uint3 thread_id : SV_DispatchThreadID)
{
    vkr_vk_transmission_shade_impl(thread_id, 0u, true, true, false, 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_transmission_shade_partitioned_production(
    uint3 thread_id : SV_DispatchThreadID)
{
    vkr_vk_transmission_shade_partitioned_impl(thread_id, false, 0u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_transmission_shade_partitioned_production_temporal(
    uint3 thread_id : SV_DispatchThreadID)
{
    vkr_vk_transmission_shade_partitioned_impl(thread_id, false, 1u);
}

groupshared uint g_transmission_group_coverage;

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_coverage(uint3 thread_id : SV_DispatchThreadID,
                              uint3 group_thread_id : SV_GroupThreadID)
{
    VkrVkTransmissionCoverageRoot *root = g_transmission_coverage_push.root;
    if (all(group_thread_id == 0u))
        g_transmission_group_coverage = 0u;
    GroupMemoryBarrierWithGroupSync();

    // One shared-memory atomic per wave rather than one per covered lane; a
    // full-screen dispatch otherwise serializes 64 lanes onto one address.
    bool covered = false;
    if (all(thread_id.xy < root.extent))
    {
        covered = g_storage_uint2_images[root.vbuffer_texture]
                                        [uint3(thread_id.xy, root.layer)]
                                            .x != 0u;
    }
    uint wave_covered = WaveActiveCountBits(covered);
    if (WaveIsFirstLane() && wave_covered != 0u)
    {
        uint ignored;
        InterlockedAdd(g_transmission_group_coverage, wave_covered, ignored);
    }
    GroupMemoryBarrierWithGroupSync();

    if (all(group_thread_id == 0u) && g_transmission_group_coverage != 0u)
    {
        uint ignored;
        InterlockedAdd(root.covered_pixels[root.layer],
                       g_transmission_group_coverage, ignored);
    }
}

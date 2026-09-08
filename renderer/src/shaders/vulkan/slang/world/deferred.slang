#include "../../../shared/temporal_filter_kernel.slangh"

// Vulkan deferred visibility path. All variable-sized data is reached through
// physical storage-buffer addresses; descriptor indices name graph images.

static const uint VKR_VK_PRIMITIVE_ID_MASK = 0x7fffffffu;
static const uint VKR_VK_FRONT_FACE_BIT = 0x80000000u;
static const uint VKR_VK_DRAW_BUCKET_COUNT = 8u;
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
    uint clearcoat_texture;
    uint sheen_texture;
    uint anisotropy_texture;
    uint reserved_tail;
    float4x4 sky_reprojection;
};

#ifndef VKR_VULKAN_GBUFFER_WRITE_EMISSIVE
#define VKR_VULKAN_GBUFFER_WRITE_EMISSIVE 0
#endif
#ifndef VKR_VULKAN_GBUFFER_WRITE_DEBUG
#define VKR_VULKAN_GBUFFER_WRITE_DEBUG 0
#endif

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
    // RGB radiance of the alpha-covered disc; source RGB deliberately omits it.
    float4 solar_disk_radiance;
    uint direct_source_texture;
    uint ssgi_enabled;
    uint clearcoat_texture;
    uint sheen_texture;
    uint anisotropy_texture;
    uint visible_rows_reserved0;
    uint visible_rows_reserved1;
    uint visible_rows_reserved2;
    VkrGpuVisibleDrawRow* visible_rows;
    uint subsurface_source_texture;
    uint subsurface_profile_count;
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
struct VkrVkSsrDepthBaseRoot
{
    VkrSsrParams params;
    uint depth_texture;
    uint vbuffer_texture;
    uint destination_depth_texture;
    uint receiver_texture;
};

struct VkrVkSsrDepthMipRoot
{
    uint source_depth_texture;
    uint destination_depth_texture;
    uint2 source_extent;
    uint2 destination_extent;
    uint2 reserved;
};

struct VkrVkSsrTraceRoot
{
    VkrSsrParams params;
    uint depth_texture;
    uint vbuffer_texture;
    uint normal_texture;
    uint specular_texture;
    uint depth_pyramid_texture;
    uint receiver_texture;
    uint source_texture;
    uint destination_texture;
    uint clearcoat_texture;
    uint3 reserved;
};

struct VkrVkSsrTemporalRoot
{
    VkrSsrParams params;
    VkrGpuVisibleDrawRow *visible_rows;
    VkrVulkanInstance *instances;
    uint raw_texture;
    uint receiver_texture;
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
    uint specular_texture;
    uint clearcoat_texture;
};

struct VkrVkSsrCompositeRoot
{
    VkrSsrParams params;
    VkrVulkanPacketFrameRoot *frame;
    uint2 frame_address_padding;
    float4x4 inverse_view_projection;
    uint scene_texture;
    uint reflection_texture;
    uint vbuffer_texture;
    uint depth_texture;
    uint albedo_texture;
    uint specular_texture;
    uint normal_texture;
    uint gtao_visibility_texture;
    uint history_depth_texture;
    uint receiver_texture;
    uint linear_sampler;
    uint clearcoat_texture;
    uint sheen_texture;
    uint anisotropy_texture;
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
struct VkrVkSsrDepthBasePush
{
    VkrVkSsrDepthBaseRoot *root;
    uint2 reserved;
};
struct VkrVkSsrDepthMipPush
{
    VkrVkSsrDepthMipRoot *root;
    uint2 reserved;
};
struct VkrVkSsrTracePush
{
    VkrVkSsrTraceRoot *root;
    uint2 reserved;
};
struct VkrVkSsrTemporalPush
{
    VkrVkSsrTemporalRoot *root;
    uint2 reserved;
};
struct VkrVkSsrCompositePush
{
    VkrVkSsrCompositeRoot *root;
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
[[vk::push_constant]] ConstantBuffer<VkrVkSsrDepthBasePush> g_ssr_depth_base_push;
[[vk::push_constant]] ConstantBuffer<VkrVkSsrDepthMipPush> g_ssr_depth_mip_push;
[[vk::push_constant]] ConstantBuffer<VkrVkSsrTracePush> g_ssr_trace_push;
[[vk::push_constant]] ConstantBuffer<VkrVkSsrTemporalPush> g_ssr_temporal_push;
[[vk::push_constant]] ConstantBuffer<VkrVkSsrCompositePush> g_ssr_composite_push;
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
    float radius = max(candidate.local_bounding_sphere.w *
                       instance.normal_column1.w, 0.0f);
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
    // Positive-height Vulkan viewports and the HZB producer keep image Y.
    float2 uv_min = clamp(ndc_min * 0.5f + 0.5f, 0.0f, 1.0f);
    float2 uv_max = clamp(ndc_max * 0.5f + 0.5f, 0.0f, 1.0f);
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
    float world_radius = local.w * instance.normal_column1.w;
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
    g_storage_2d[root.clearcoat_texture][pixel] = 0.0f;
    g_storage_2d[root.sheen_texture][pixel] = 0.0f;
    g_storage_2d[root.anisotropy_texture][pixel] = 0.0f;
#if VKR_VULKAN_GBUFFER_WRITE_EMISSIVE
    g_storage_2d[root.emissive_texture][pixel] = 0.0f;
#endif
#if VKR_VULKAN_GBUFFER_WRITE_DEBUG
    g_storage_2d[root.debug_texture][pixel] = float4(0.0f, 0.0f, 0.0f, marker);
#endif
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
    float3 geometric_normal;
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
    surface.geometric_normal = normal;
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
    float clearcoat_factor = saturate(
        root.materials[surface.material_index].material_clearcoat.x);
    VkrVulkanMaterial clearcoat_material =
        root.materials[surface.material_index];
    if (clearcoat_factor > 0.0f && (clearcoat_material.flags & 32u) != 0u)
        clearcoat_factor *= g_textures[NonUniformResourceIndex(
                                clearcoat_material.clearcoat_texture)]
                                .SampleGrad(g_samplers[NonUniformResourceIndex(
                                    clearcoat_material.clearcoat_sampler)],
                                            surface.uv, surface.uv_dx,
                                            surface.uv_dy)
                                .r;
    float clearcoat_roughness = 0.0f;
    float3 clearcoat_normal = surface.geometric_normal;
    if (clearcoat_factor > 0.0f)
    {
        clearcoat_roughness = clearcoat_material.material_clearcoat.y;
        if ((clearcoat_material.flags & 64u) != 0u)
            clearcoat_roughness *= g_textures[NonUniformResourceIndex(
                                        clearcoat_material.clearcoat_roughness_texture)]
                                        .SampleGrad(g_samplers[NonUniformResourceIndex(
                                            clearcoat_material.clearcoat_roughness_sampler)],
                                                    surface.uv, surface.uv_dx,
                                                    surface.uv_dy)
                                        .g;
        clearcoat_roughness = clamp(clearcoat_roughness, 0.04f, 1.0f);
        if ((clearcoat_material.flags & 128u) != 0u)
        {
            float3 sampled = vkr_normal_map_decode(
                g_textures[NonUniformResourceIndex(
                    clearcoat_material.clearcoat_normal_texture)]
                    .SampleGrad(g_samplers[NonUniformResourceIndex(
                        clearcoat_material.clearcoat_normal_sampler)],
                                surface.uv, surface.uv_dx, surface.uv_dy)
                    .xyz,
                clearcoat_material.material_clearcoat.z);
            float3 tangent = mul(instance.model,
                                 float4(surface.object_tangent.xyz, 0.0f)).xyz;
            tangent = normalize(tangent - dot(tangent, surface.geometric_normal) *
                                            surface.geometric_normal);
            float tangent_handedness =
                surface.object_tangent.w < 0.0f ? -1.0f : 1.0f;
            float3 bitangent = normalize(cross(surface.geometric_normal, tangent)) *
                              tangent_handedness;
            clearcoat_normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                                         surface.geometric_normal * sampled.z);
        }
    }
    g_storage_2d[root.clearcoat_texture][thread_id.xy] =
        float4(clearcoat_factor, clearcoat_roughness,
               vkr_vk_octahedral_encode(clearcoat_normal));
    float3 sheen_color = max(clearcoat_material.material_sheen.rgb, 0.0f);
    float sheen_roughness = 0.0f;
    if (vkr_sheen_active(sheen_color))
    {
        if ((clearcoat_material.flags & 256u) != 0u)
            sheen_color *= g_textures[NonUniformResourceIndex(
                               clearcoat_material.sheen_color_texture)]
                               .SampleGrad(g_samplers[NonUniformResourceIndex(
                                   clearcoat_material.sheen_color_sampler)],
                                           surface.uv, surface.uv_dx,
                                           surface.uv_dy)
                               .rgb;
        if (vkr_sheen_active(sheen_color))
        {
            sheen_roughness = clearcoat_material.material_sheen.w;
            if ((clearcoat_material.flags & 512u) != 0u)
                sheen_roughness *= g_textures[NonUniformResourceIndex(
                                      clearcoat_material.sheen_roughness_texture)]
                                      .SampleGrad(g_samplers[NonUniformResourceIndex(
                                          clearcoat_material.sheen_roughness_sampler)],
                                                  surface.uv, surface.uv_dx,
                                                  surface.uv_dy)
                                      .a;
            sheen_roughness = clamp(sheen_roughness,
                                    VKR_SHEEN_MIN_ROUGHNESS, 1.0f);
        }
        else
            sheen_roughness = 0.0f;
    }
    g_storage_2d[root.sheen_texture][thread_id.xy] =
        float4(sheen_color, sheen_roughness);
  float anisotropy_strength = clearcoat_material.material_anisotropy.x;
  float3 anisotropy_axis = float3(0.0f);
  if (anisotropy_strength > 0.0f) {
    float3 map = float3(1.0f, 0.5f, 1.0f);
    if ((clearcoat_material.flags & 1024u) != 0u)
      map = g_textures[NonUniformResourceIndex(clearcoat_material.anisotropy_texture)].SampleGrad(g_samplers[NonUniformResourceIndex(clearcoat_material.anisotropy_sampler)], surface.uv, surface.uv_dx, surface.uv_dy).rgb;
    anisotropy_strength *= map.b;
    anisotropy_axis = vkr_anisotropy_axis(surface.geometric_normal, surface.normal,
        float4(mul(instance.model, float4(surface.object_tangent.xyz, 0.0f)).xyz, surface.object_tangent.w < 0.0f ? -1.0f : 1.0f), map.rg * 2.0f - 1.0f, clearcoat_material.material_anisotropy.yz);
  }
  g_storage_2d[root.anisotropy_texture][thread_id.xy] = float4(anisotropy_strength > 0.0f ? vkr_anisotropy_encode_axis(anisotropy_axis) : float2(0.0f), anisotropy_strength, 0.0f);
#if VKR_VULKAN_GBUFFER_WRITE_EMISSIVE
    g_storage_2d[root.emissive_texture][thread_id.xy] =
        float4(surface.emissive, 1.0f);
#endif
#if VKR_VULKAN_GBUFFER_WRITE_DEBUG
    g_storage_2d[root.debug_texture][thread_id.xy] =
        float4(surface.barycentric, surface.selected_lod + 1.0f);
#endif
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
    float4 sky_sample = g_cube_textures[root.sky_texture]
        .SampleLevel(g_samplers[root.sky_sampler], direction, 0.0f);
    return sky_sample.rgb + sky_sample.a * root.solar_disk_radiance.rgb;
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
    return vkr_ggx_filter_roughness(roughness, normal_variance);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_deferred_lighting(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkLightingRoot *root = g_lighting_push.root;
    if (any(thread_id.xy >= root.extent))
        return;
    VkrVulkanPacketFrameRoot *frame = root.frame;
    if (root.subsurface_profile_count != 0u)
        g_storage_2d[root.subsurface_source_texture][thread_id.xy] = float4(0.0f);
    if (root.ssgi_enabled != 0u)
        g_storage_2d[root.direct_source_texture][thread_id.xy] = float4(0.0f);
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
    VkrGgxMaterialEnergy energy = packet_prepare_gbuffer_brdf(
        frame, normal, view, roughness, f0, g_storage_2d[root.anisotropy_texture][thread_id.xy]);
    energy = vkr_ggx_diffuse_transmission(energy,
        frame.materials[root.visible_rows[encoded.x - 1u].material_index].material_diffuse_transmission);
    float4 clearcoat_packed = g_storage_2d[root.clearcoat_texture][thread_id.xy];
    bool clearcoat_active = vkr_clearcoat_active(clearcoat_packed.x);
    VkrClearcoatLayer clearcoat;
    if (clearcoat_active)
        clearcoat = packet_prepare_clearcoat(
            frame, clearcoat_packed.x, clearcoat_packed.y,
            vkr_vk_octahedral_decode(clearcoat_packed.zw), view);
    float4 sheen_packed = g_storage_2d[root.sheen_texture][thread_id.xy];
    bool sheen_active = vkr_sheen_active(sheen_packed.rgb);
    VkrSheenLayer sheen;
    VkrSheenLtcSample sheen_ltc;
    if (sheen_active)
    {
        sheen_ltc = packet_sheen_ltc_sample(
            frame, max(dot(normal, view), 0.0f), sheen_packed.a);
        sheen = packet_prepare_sheen(
            frame, sheen_packed.rgb, sheen_packed.a, normal, view,
            sheen_ltc.allocation_reserve);
    }
    float base_transmission =
        (clearcoat_active ? clearcoat.base_transmission : 1.0f) *
        (sheen_active ? sheen.base_transmission : 1.0f);
    float sheen_transmission =
        clearcoat_active ? clearcoat.base_transmission : 1.0f;
    float3 diffuse_irradiance = float3(0.0f);
    float3 analytic_diffuse = 0.0f;
    float3 analytic_specular = 0.0f;
    float3 clearcoat_direct = 0.0f;
    float3 sheen_direct = 0.0f;
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        float3 sun_direction = normalize(-frame.directional_direction_enabled.xyz);
        bool back_lit = energy.diffuse_transmission_strength > 0.0f &&
                        dot(normal, sun_direction) < 0.0f;
        float base_shadow = packet_directional_shadow(
            frame, world, back_lit ? -normal : normal);
        float directional_shadow = base_shadow;
        if (back_lit && clearcoat_active)
            directional_shadow = packet_directional_shadow(frame, world, normal);
        VkrVulkanDirectResult direct = packet_direct(
            normal, view, normalize(-frame.directional_direction_enabled.xyz),
            frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * base_shadow,
            diffuse_albedo, 0.0f, roughness, f0, energy);
        diffuse_irradiance = direct.irradiance;
        analytic_diffuse = direct.diffuse;
        analytic_specular = direct.specular;
        if (clearcoat_active)
            clearcoat_direct += packet_clearcoat_direct(
                view, normalize(-frame.directional_direction_enabled.xyz),
                frame.directional_color_intensity.rgb *
                    frame.directional_color_intensity.w * directional_shadow,
                clearcoat);
        if (sheen_active)
            sheen_direct += packet_sheen_direct(
                normal, view, normalize(-frame.directional_direction_enabled.xyz),
                frame.directional_color_intensity.rgb *
                    frame.directional_color_intensity.w * directional_shadow,
                sheen, sheen_ltc.energy_normalization);
    }
    VkrVulkanDirectResult punctual = packet_punctual(
        frame, world, normal, view, diffuse_albedo, 0.0f, roughness, f0, energy);
    diffuse_irradiance += punctual.irradiance;
    analytic_diffuse += punctual.diffuse;
    analytic_specular += punctual.specular;
    if (clearcoat_active)
    {
        VkrVulkanDirectResult coat_punctual = packet_punctual(
            frame, world, clearcoat.normal, view, float3(0.0f), 0.0f,
            clearcoat.roughness, float3(VKR_CLEARCOAT_F0), clearcoat.energy);
        clearcoat_direct += coat_punctual.specular * clearcoat.factor;
    }
    if (sheen_active)
        sheen_direct += packet_punctual_sheen(
            frame, world, normal, view, sheen, sheen_ltc.energy_normalization);
    VkrVulkanDirectResult rectangles = packet_rectangle_lights(
        frame, world, normal, view, diffuse_albedo, 0.0f, roughness, f0,
        energy, true);
    diffuse_irradiance += rectangles.irradiance;
    analytic_diffuse += rectangles.diffuse;
    analytic_specular += rectangles.specular;
    if (clearcoat_active)
    {
        VkrVulkanDirectResult coat_rectangles = packet_rectangle_lights(
            frame, world, clearcoat.normal, view, float3(0.0f), 0.0f,
            clearcoat.roughness, float3(VKR_CLEARCOAT_F0), clearcoat.energy, false);
        clearcoat_direct += coat_rectangles.specular * clearcoat.factor;
    }
    if (sheen_active)
        sheen_direct += packet_sheen_rectangle_lights(
            frame, world, normal, view, sheen, sheen_ltc);
    if (root.ssgi_enabled != 0u && frame.render_mode == 0u &&
        frame.shadow_debug_mode == 0u)
        g_storage_2d[root.direct_source_texture][thread_id.xy] =
            float4((analytic_diffuse + analytic_specular) * base_transmission +
                   clearcoat_direct + sheen_direct * sheen_transmission +
                   seed.rgb * base_transmission, 1.0f);
    if (frame.render_mode == 1u || frame.render_mode == 4u ||
        frame.render_mode == 5u)
    {
        float3 debug = frame.render_mode == 4u
                           ? analytic_diffuse * base_transmission
                           : (frame.render_mode == 5u
                                  ? analytic_specular * base_transmission +
                                        clearcoat_direct +
                                        sheen_direct * sheen_transmission
                                  : (analytic_diffuse + analytic_specular) *
                                        base_transmission + clearcoat_direct +
                                        sheen_direct * sheen_transmission);
        g_storage_2d[root.scene_texture][thread_id.xy] = float4(debug, 1.0f);
        return;
    }
    float2 gtao_uv =
        (float2(thread_id.xy) + 0.5f) / float2(root.extent);
    float4 gtao =
        g_textures[root.gtao_visibility_texture]
            .SampleLevel(g_samplers[0], gtao_uv, 0.0f);
    float gtao_visibility = vkr_gtao_decode_visibility(gtao);
    float3 bent_normal = vkr_gtao_decode_bent_normal(gtao, normal);
    float gtao_cone = vkr_gtao_cone_specular(
        gtao_visibility, bent_normal, reflect(-view, normal), roughness);
    // Mode 9 reports the selected pre-GTAO indirect diffuse response.
    float3 indirect_diffuse_normal =
        frame.render_mode == 9u ? normal : bent_normal;
    float3 indirect_irradiance;
    float3 indirect_diffuse;
    float3 indirect_specular = 0.0f;
    float3 clearcoat_indirect = 0.0f;
    float3 sheen_indirect = 0.0f;
    bool environment_enabled = (frame.flags & 2u) != 0u;
    if ((frame.flags & 2u) != 0u)
    {
        VkrVulkanEnvironmentResult environment =
            packet_environment_terms_with_diffuse_normal(
                frame, world, indirect_diffuse_normal, normal, normal, view,
                diffuse_albedo, 0.0f, roughness, ao, energy);
        indirect_irradiance = environment.irradiance;
        indirect_diffuse = environment.diffuse;
        indirect_specular = environment.specular * gtao_cone;
        if (clearcoat_active)
            clearcoat_indirect = clearcoat.factor * packet_environment_specular(
                frame, world, clearcoat.normal, clearcoat.normal, view,
                clearcoat.roughness, ao, clearcoat.energy);
        if (sheen_active)
        {
            float sheen_cone = vkr_gtao_cone_specular(
                gtao_visibility, bent_normal, reflect(-view, normal),
                sheen.roughness);
            sheen_indirect = packet_sheen_environment(
                frame, world, normal, view, ao, sheen_cone, sheen);
        }
    }
    else
    {
        indirect_irradiance = frame.ambient_color.rgb * ao;
        indirect_diffuse = frame.ambient_color.rgb * diffuse_albedo * ao *
                           energy.diffuse_weight;
    }
    float3 volume_irradiance;
    float3 volume_diffuse;
    bool volume_valid = packet_diffuse_volume_terms(
        frame, world, normal, diffuse_albedo, 0.0f, ao, energy, volume_diffuse, volume_irradiance);
    if (volume_valid) {
        indirect_diffuse = volume_diffuse;
        indirect_irradiance = volume_irradiance;
    }
    // Mode 9 omits GTAO so it reports the selected indirect diffuse response.
    if (frame.render_mode == 9u)
    {
        g_storage_2d[root.scene_texture][thread_id.xy] =
            float4(indirect_diffuse * base_transmission, 1.0f);
        return;
    }
    float3 indirect_visibility = volume_valid
                                    ? float3(gtao_visibility)
                                    : (environment_enabled
                                           ? vkr_gtao_multibounce(
                                                 gtao_visibility, diffuse_albedo)
                                           : float3(gtao_visibility));
    if (root.subsurface_profile_count != 0u) {
        float4 subsurface = frame.materials[root.visible_rows[encoded.x-1u].material_index].material_subsurface;
        if (subsurface.x > 0.0f && uint(subsurface.y) < root.subsurface_profile_count) {
            float3 amplitude = vkr_subsurface_root_amplitude(diffuse_albedo);
            g_storage_2d[root.subsurface_source_texture][thread_id.xy] =
                float4(vkr_subsurface_source_storage(amplitude *
                    (diffuse_irradiance + indirect_irradiance * indirect_visibility)), 1.0f);
        }
    }
    float3 color = (analytic_diffuse + analytic_specular +
                    indirect_diffuse * indirect_visibility + indirect_specular +
                    seed.rgb) * base_transmission + clearcoat_direct +
                   sheen_direct * sheen_transmission +
                   clearcoat_indirect + sheen_indirect * sheen_transmission;
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
float vkr_vk_ssr_positive_depth(VkrSsrParams params, uint2 pixel,
                                uint2 extent, float device_depth)
{
    float3 view = vkr_ssr_reconstruct_view_position(
        params, vkr_ssr_uv_from_pixel(pixel, extent), device_depth);
    float result = -view.z;
    return vkr_ssr_valid_depth(result) ? result : 0.0f;
}

uint2 vkr_vk_ssr_identity(VkrVkSsrTemporalRoot *root, uint visible_index)
{
    if (visible_index == 0u)
        return uint2(0u, 0u);
    VkrGpuVisibleDrawRow visible = root.visible_rows[visible_index - 1u];
    VkrVulkanInstance instance = root.instances[visible.instance_index];
    return uint2(instance.temporal_index + 1u, instance.temporal_generation);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_ssr_depth_base(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsrDepthBaseRoot *root = g_ssr_depth_base_push.root;
    uint2 cell = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    if (any(cell >= trace_extent)) return;
    uint2 begin = uint2(vkr_ssr_reduction_child_begin(cell.x),
                        vkr_ssr_reduction_child_begin(cell.y));
    uint2 end = uint2(vkr_ssr_reduction_child_end(cell.x, trace_extent.x, source_extent.x),
                      vkr_ssr_reduction_child_end(cell.y, trace_extent.y, source_extent.y));
    float nearest = 0.0f;
    uint2 receiver = uint2(0xffffffffu, 0xffffffffu);
    for (uint y = begin.y; y < end.y; ++y)
    for (uint x = begin.x; x < end.x; ++x)
    {
        uint2 p = uint2(x, y);
        if (g_uint2_textures[root.vbuffer_texture].Load(int3(p, 0)).x == 0u)
            continue;
        float positive = vkr_vk_ssr_positive_depth(
            root.params, p, source_extent,
            g_depth_textures[root.depth_texture].Load(int3(p, 0)));
        if (vkr_ssr_valid_depth(positive) &&
            (!vkr_ssr_valid_depth(nearest) || positive < nearest))
        {
            nearest = positive;
            receiver = p;
        }
    }
    g_storage_2d[root.destination_depth_texture][cell] = float4(nearest, 0, 0, 0);
    g_storage_uint2_2d[root.receiver_texture][cell] = receiver;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_ssr_depth_mip(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsrDepthMipRoot *root = g_ssr_depth_mip_push.root;
    uint2 cell = thread_id.xy;
    if (any(cell >= root.destination_extent)) return;
    uint2 begin = uint2(vkr_ssr_reduction_child_begin(cell.x),
                        vkr_ssr_reduction_child_begin(cell.y));
    uint2 end = uint2(vkr_ssr_reduction_child_end(cell.x, root.destination_extent.x,
                                                   root.source_extent.x),
                      vkr_ssr_reduction_child_end(cell.y, root.destination_extent.y,
                                                   root.source_extent.y));
    float values[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; uint count = 0u;
    for (uint y = begin.y; y < end.y; ++y)
    for (uint x = begin.x; x < end.x; ++x)
        values[count++] = g_depth_textures[root.source_depth_texture].Load(int3(x, y, 0));
    g_storage_2d[root.destination_depth_texture][cell] = float4(
        vkr_ssr_depth_reduce(values[0], values[1], values[2], values[3], values[4],
                             values[5], values[6], values[7], values[8], count), 0, 0, 0);
}

float3 vkr_vk_ssr_selected_normal(uint normal_texture, uint clearcoat_texture,
                                  uint2 pixel)
{
    float4 packed = g_textures[clearcoat_texture].Load(int3(pixel, 0));
    return vkr_clearcoat_active(packed.x)
        ? vkr_vk_octahedral_decode(packed.zw)
        : vkr_vk_octahedral_decode(g_textures[normal_texture].Load(int3(pixel, 0)).xy);
}

float vkr_vk_ssr_selected_roughness(uint specular_texture, uint clearcoat_texture,
                                    uint2 pixel)
{
    float4 packed = g_textures[clearcoat_texture].Load(int3(pixel, 0));
    return vkr_clearcoat_active(packed.x) ? clamp(packed.y, 0.04f, 1.0f)
        : clamp(g_textures[specular_texture].Load(int3(pixel, 0)).a, 0.04f, 1.0f);
}

float3 vkr_vk_ssr_trace_color(VkrVkSsrTraceRoot *root, float2 hit_uv,
                               float roughness)
{
    uint2 extent = uint2(root.params.source_width, root.params.source_height);
    int2 center = clamp(int2(hit_uv * float2(extent)), int2(0), int2(extent) - 1);
    uint count = vkr_ssr_hit_filter_tap_count(roughness);
    float radius = vkr_ssr_hit_filter_radius_pixels(roughness, root.params);
    float3 result = 0.0f;
    for (uint i = 0u; i < count; ++i)
    {
        int2 sample_pixel = clamp(center + int2(round(vkr_ssr_hit_filter_offset(i) * radius)),
                                  int2(0), int2(extent) - 1);
        result += g_textures[root.source_texture].Load(int3(sample_pixel, 0)).rgb;
    }
    return result / float(count);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_ssr_trace(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsrTraceRoot *root = g_ssr_trace_push.root;
    uint2 cell = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    if (any(cell >= trace_extent)) return;
    uint2 receiver = g_uint2_textures[root.receiver_texture].Load(int3(cell, 0));
    float4 raw = 0.0f;
    if (any(receiver >= source_extent)) { g_storage_2d[root.destination_texture][cell] = raw; return; }
    uint visible = g_uint2_textures[root.vbuffer_texture].Load(int3(receiver, 0)).x;
    float roughness = vkr_vk_ssr_selected_roughness(
        root.specular_texture, root.clearcoat_texture, receiver);
    if (visible == 0u || !vkr_ssr_eligible(roughness, true, root.params)) {
        g_storage_2d[root.destination_texture][cell] = raw; return;
    }
    float depth = g_depth_textures[root.depth_texture].Load(int3(receiver, 0));
    float3 position = vkr_ssr_reconstruct_view_position(
        root.params, vkr_ssr_uv_from_pixel(receiver, source_extent), depth);
    if (!vkr_ssr_valid_depth(-position.z)) { g_storage_2d[root.destination_texture][cell] = raw; return; }
    float3 world_normal = vkr_vk_ssr_selected_normal(
        root.normal_texture, root.clearcoat_texture, receiver);
    float3 normal = normalize(mul(root.params.view, float4(world_normal, 0.0f)).xyz);
    if (dot(normal, -position) < 0.0f)
        normal = -normal;
    float3 view = normalize(-position);
    VkrSsrTrace trace = vkr_ssr_trace_begin(root.params, position + normal * VKR_SSR_RAY_ORIGIN_BIAS,
                                             reflect(-view, normal), root.params.depth_mip_count - 1u);
    for (uint step = 0u; step < root.params.max_steps && trace.active != 0u; ++step)
    {
        uint2 mip_extent = max(trace_extent >> trace.mip, uint2(1u, 1u));
        float2 uv = vkr_ssr_trace_uv(trace);
        uint2 source_pixel = min(uint2(uv * float2(source_extent)), source_extent - 1u);
        uint2 mip_cell = min(source_pixel >> (trace.mip + 1u), mip_extent - 1u);
        float exit_t = vkr_ssr_trace_cell_exit(trace, mip_extent, root.params);
        float hierarchy_depth = g_depth_textures[root.depth_pyramid_texture].Load(int3(mip_cell, trace.mip));
        VkrSsrTraceResolve resolve = vkr_ssr_trace_resolve(trace, hierarchy_depth, exit_t, root.params);
        if (resolve.hit != 0u)
        {
            VkrSsrTrace hit_trace = trace;
            hit_trace.t = resolve.hit_t;
            float2 hit_uv = vkr_ssr_trace_uv(hit_trace);
            uint2 hit_pixel = min(uint2(hit_uv * float2(source_extent)), source_extent - 1u);
            uint hit_visible = g_uint2_textures[root.vbuffer_texture].Load(int3(hit_pixel, 0)).x;
            float hit_device_depth = g_depth_textures[root.depth_texture].Load(int3(hit_pixel, 0));
            float hit_depth = vkr_vk_ssr_positive_depth(root.params, hit_pixel,
                                                        source_extent, hit_device_depth);
            float hit_trace_depth = vkr_ssr_trace_depth(hit_trace);
            if (vkr_ssr_valid_depth(hit_depth) &&
                abs(hit_depth - hit_trace_depth) > root.params.thickness)
            {
                VkrSsrTraceResolve refined = vkr_ssr_trace_refine_loaded_depth(
                    trace, hit_depth, exit_t, hit_pixel, root.params);
                if (refined.hit != 0u)
                {
                    resolve = refined;
                    hit_trace.t = resolve.hit_t;
                    hit_uv = vkr_ssr_trace_uv(hit_trace);
                    hit_trace_depth = vkr_ssr_trace_depth(hit_trace);
                }
            }
            float3 hit_normal = vkr_vk_ssr_selected_normal(
                root.normal_texture, root.clearcoat_texture, hit_pixel);
            hit_normal = normalize(mul(root.params.view, float4(hit_normal, 0.0f)).xyz);
            float3 ray = normalize(reflect(-view, normal));
            if (hit_visible != 0u && vkr_ssr_valid_depth(hit_depth) &&
                abs(hit_depth - hit_trace_depth) <= root.params.thickness &&
                dot(hit_normal, -ray) > 0.0f)
            {
                raw = float4(vkr_vk_ssr_trace_color(root, hit_uv, roughness),
                             vkr_ssr_trace_confidence(roughness, hit_uv,
                                                      resolve.depth_confidence, root.params));
                break;
            }
            trace = vkr_ssr_trace_advance(trace, exit_t);
        }
        else if (resolve.descend != 0u) trace = vkr_ssr_trace_descend(trace);
        else if (resolve.advance != 0u) trace = vkr_ssr_trace_advance(trace, exit_t);
        else break;
    }
    g_storage_2d[root.destination_texture][cell] = raw;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_ssr_temporal(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsrTemporalRoot *root = g_ssr_temporal_push.root;
    uint2 cell = thread_id.xy;
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    uint2 source_extent = uint2(root.params.source_width, root.params.source_height);
    if (any(cell >= trace_extent)) return;
    float4 raw = g_textures[root.raw_texture].Load(int3(cell, 0));
    uint2 receiver = g_uint2_textures[root.receiver_texture].Load(int3(cell, 0));
    uint2 identity = uint2(0u, 0u); float positive_depth = 0.0f;
    if (all(receiver < source_extent))
    {
        uint visible = g_uint2_textures[root.vbuffer_texture].Load(int3(receiver, 0)).x;
        identity = vkr_vk_ssr_identity(root, visible);
        positive_depth = vkr_vk_ssr_positive_depth(root.params, receiver, source_extent,
            g_depth_textures[root.depth_texture].Load(int3(receiver, 0)));
        float roughness = vkr_vk_ssr_selected_roughness(
            root.specular_texture, root.clearcoat_texture, receiver);
        if (vkr_ssr_hit_filter_tap_count(roughness) > 1u &&
            vkr_ssr_valid_depth(positive_depth))
        {
            float3 normal = vkr_vk_ssr_selected_normal(
                root.normal_texture, root.clearcoat_texture, receiver);
            float4 filtered = 0.0f; float weights = 0.0f;
            for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
            {
                int2 q = int2(cell) + int2(x, y);
                if (any(q < 0) || any(q >= int2(trace_extent))) continue;
                uint2 other_receiver = g_uint2_textures[root.receiver_texture].Load(int3(q, 0));
                if (any(other_receiver >= source_extent)) continue;
                float other_depth = vkr_vk_ssr_positive_depth(root.params, other_receiver, source_extent,
                    g_depth_textures[root.depth_texture].Load(int3(other_receiver, 0)));
                float3 other_normal = vkr_vk_ssr_selected_normal(
                    root.normal_texture, root.clearcoat_texture, other_receiver);
                float w = vkr_ssr_receiver_bilateral_weight(float2(x, y), positive_depth, other_depth,
                                                            normal, other_normal, root.params);
                filtered += vkr_ssr_spatial_sample(
                    g_textures[root.raw_texture].Load(int3(q, 0)), w);
                weights += w;
            }
            if (weights > 0.0f) raw = vkr_ssr_spatial_resolve(filtered, weights);
        }
    }
    // History is stored on the trace grid; receiver selection supplies motion only.
    float2 uv = vkr_ssr_uv_from_pixel(cell, trace_extent);
    float2 motion = all(receiver < source_extent) ? g_textures[root.motion_texture].Load(int3(receiver, 0)).xy : 0.0f;
    float2 previous_uv = vkr_ssr_previous_uv(root.params, uv, motion);
    float2 half_texel = float2(root.params.trace_texel_size_x,
                               root.params.trace_texel_size_y) * 0.5f;
    float2 coordinate = clamp(previous_uv, half_texel, 1.0f - half_texel) *
                            float2(trace_extent) - 0.5f;
    int2 first = int2(floor(coordinate));
    float2 validity = all(receiver < source_extent) ?
        g_textures[root.validity_texture].Load(int3(receiver, 0)).xy : 0.0f;
    float4 history_sum = 0.0f;
    float history_support = 0.0f;
    if (root.params.history_valid != 0u) {
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
            int2 p = first + int2(x, y);
            if (any(p < 0) || any(p >= int2(trace_extent))) continue;
            float weight = vkr_ssr_history_tap_weight(coordinate, p);
            if (weight <= 0.0f) continue;
            // Each color tap proves its own metadata before interpolation.
            VkrSsrHistoryDecision tap = vkr_ssr_temporal_accept(root.params,
                identity, g_storage_uint2_2d[root.history_identity_texture][p],
                g_depth_textures[root.history_depth_texture].Load(int3(p, 0)),
                validity.y, previous_uv, validity.x);
            if (tap.accepted == 0u) continue;
            history_sum += vkr_ssr_spatial_sample(
                g_textures[root.history_color_texture].Load(int3(p, 0)), weight);
            history_support += weight;
        }
    }
    float4 history = vkr_ssr_spatial_resolve(history_sum, history_support);
    VkrSsrHistoryDecision decision;
    decision.accepted = history_support > 0.0f ? 1u : 0u;
    decision.weight = decision.accepted != 0u ? root.params.temporal_weight : 0.0f;
    decision.expected_previous_depth = 0.0f;
    decision.reserved_float_0 = 0.0f;
    float3 neighborhood_min = raw.w > 0.0f ? raw.rgb : float3(3.402823466e+38f);
    float3 neighborhood_max = raw.w > 0.0f ? raw.rgb : float3(-3.402823466e+38f);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        int2 q = int2(cell) + int2(x, y);
        if (any(q < 0) || any(q >= int2(trace_extent))) continue;
        float4 sample = g_textures[root.raw_texture].Load(int3(q, 0));
        if (sample.w > 0.0f) {
            neighborhood_min = min(neighborhood_min, sample.rgb);
            neighborhood_max = max(neighborhood_max, sample.rgb);
        }
    }
    float4 result = vkr_ssr_temporal_filter(root.params, raw, history, neighborhood_min,
                                            neighborhood_max, decision);
    g_storage_2d[root.output_color_texture][cell] = result;
    g_storage_2d[root.output_depth_texture][cell] = float4(positive_depth, 0, 0, 0);
    g_storage_uint2_2d[root.output_identity_texture][cell] = identity;
}

float vkr_vk_ssr_composite_filter_roughness(VkrVkSsrCompositeRoot *root,
                                             uint2 pixel, uint visible,
                                             float3 normal, float roughness)
{
    uint2 extent = uint2(root.params.source_width, root.params.source_height);
    uint2 limit = extent - 1u;
    uint2 pixel_x = min(pixel + uint2(1u, 0u), limit);
    uint2 pixel_y = min(pixel + uint2(0u, 1u), limit);
    float3 normal_x = vkr_vk_ssr_selected_normal(
        root.normal_texture, root.clearcoat_texture, pixel_x);
    float3 normal_y = vkr_vk_ssr_selected_normal(
        root.normal_texture, root.clearcoat_texture, pixel_y);
    float same_x = g_uint2_textures[root.vbuffer_texture].Load(int3(pixel_x, 0)).x == visible ? 1.0f : 0.0f;
    float same_y = g_uint2_textures[root.vbuffer_texture].Load(int3(pixel_y, 0)).x == visible ? 1.0f : 0.0f;
    float3 dx = (normal_x - normal) * same_x;
    float3 dy = (normal_y - normal) * same_y;
    return vkr_ggx_filter_roughness(roughness,
                                    0.25f * (dot(dx, dx) + dot(dy, dy)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_ssr_composite(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSsrCompositeRoot *root = g_ssr_composite_push.root;
    uint2 pixel = thread_id.xy;
    uint2 extent = uint2(root.params.source_width, root.params.source_height);
    uint2 trace_extent = uint2(root.params.trace_width, root.params.trace_height);
    if (any(pixel >= extent)) return;
    uint visible = g_uint2_textures[root.vbuffer_texture].Load(int3(pixel, 0)).x;
    if (visible == 0u) return;
    float4 specular = g_textures[root.specular_texture].Load(int3(pixel, 0));
    float4 clearcoat_packed = g_textures[root.clearcoat_texture].Load(int3(pixel, 0));
    bool clearcoat_active = vkr_clearcoat_active(clearcoat_packed.x);
    float3 normal = clearcoat_active ? vkr_vk_octahedral_decode(clearcoat_packed.zw)
                                     : vkr_vk_octahedral_decode(g_textures[root.normal_texture].Load(int3(pixel, 0)).xy);
    float material_roughness = clearcoat_active ? clamp(clearcoat_packed.y, 0.04f, 1.0f)
                                               : clamp(specular.a, 0.04f, 1.0f);
    // Match trace eligibility; raster-dependent normal variance only broadens
    // BRDF weights.
    if (!vkr_ssr_eligible(material_roughness, true, root.params)) return;
    float roughness = vkr_vk_ssr_composite_filter_roughness(
        root, pixel, visible, normal, material_roughness);
    float device_depth = g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    float positive_depth = vkr_vk_ssr_positive_depth(root.params, pixel, extent, device_depth);
    if (!vkr_ssr_valid_depth(positive_depth)) return;
    float2 trace_coordinate = (float2(pixel) + 0.5f) * float2(trace_extent) /
                              float2(extent) - 0.5f;
    int2 first = int2(floor(trace_coordinate));
    float3 reflected_radiance = 0.0f;
    float accepted_weight = 0.0f;
    float support_weight = 0.0f;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x)
    {
        int2 cell = first + int2(x, y);
        if (any(cell < 0) || any(cell >= int2(trace_extent))) continue;
        uint2 receiver = g_uint2_textures[root.receiver_texture].Load(int3(cell, 0));
        if (any(receiver >= extent)) continue;
        float2 offset = float2(cell) - trace_coordinate;
        float spatial = max(1.0f - abs(offset.x), 0.0f) *
                        max(1.0f - abs(offset.y), 0.0f);
        float receiver_depth = g_depth_textures[root.history_depth_texture].Load(int3(cell, 0));
        float3 receiver_normal = vkr_vk_ssr_selected_normal(
            root.normal_texture, root.clearcoat_texture, receiver);
        float support = spatial * vkr_ssr_receiver_bilateral_weight(
            offset, positive_depth, receiver_depth, normal, receiver_normal,
            root.params);
        support_weight += support;
        float4 sample = g_textures[root.reflection_texture].Load(int3(cell, 0));
        float accepted = support * vkr_ssr_saturate(sample.a);
        reflected_radiance += sample.rgb * accepted;
        accepted_weight += accepted;
    }
    if (accepted_weight <= 0.0f || support_weight <= 0.0f) return;
    float3 reflection = reflected_radiance / accepted_weight;
    float confidence = accepted_weight / support_weight;
    float2 uv = vkr_ssr_uv_from_pixel(pixel, extent);
    float3 world = vkr_vk_deferred_homogeneous(mul(root.inverse_view_projection,
        float4(uv * 2.0f - 1.0f, device_depth, 1.0f)));
    float4 albedo_ao = g_textures[root.albedo_texture].Load(int3(pixel, 0));
    float3 view = normalize(root.frame.view_position.xyz - world);
    VkrGgxMaterialEnergy energy = packet_prepare_brdf(root.frame, max(dot(normal, view), 0.0f),
                                                       roughness, clearcoat_active ? float3(VKR_CLEARCOAT_F0) : saturate(specular.rgb));
    if (!clearcoat_active)
        energy = packet_prepare_gbuffer_brdf(root.frame, normal, view, roughness, saturate(specular.rgb), g_textures[root.anisotropy_texture].Load(int3(pixel, 0)));
    float4 gtao = g_textures[root.gtao_visibility_texture].Load(int3(pixel, 0));
    float gtao_visibility = vkr_gtao_decode_visibility(gtao);
    float3 bent_normal = vkr_gtao_decode_bent_normal(gtao, normal);
    float gtao_cone = vkr_gtao_cone_specular(
        gtao_visibility, bent_normal, reflect(-view, normal), roughness);
    float coat_factor = clearcoat_active ? clearcoat_packed.x : 1.0f;
    float3 old_specular = (root.frame.flags & 2u) != 0u
                              ? packet_environment_specular(
                                    root.frame, world, normal, normal, view,
                                    roughness, saturate(albedo_ao.a), energy)
                              : 0.0f;
    float sheen_base_transmission = 1.0f;
    if (!clearcoat_active)
    {
        float4 sheen_packed = g_textures[root.sheen_texture].Load(int3(pixel, 0));
        if (vkr_sheen_active(sheen_packed.rgb))
            sheen_base_transmission = packet_prepare_sheen(
                root.frame, sheen_packed.rgb, sheen_packed.a, normal, view,
                packet_sheen_allocation_reserve(
                    root.frame, max(dot(normal, view), 0.0f), sheen_packed.a))
                .base_transmission;
    }
    old_specular *= gtao_cone * coat_factor * sheen_base_transmission;
    float3 receiver_weight = packet_environment_specular_weight(
        normal, normal, view, roughness, saturate(albedo_ao.a), energy) *
                             gtao_cone * coat_factor * sheen_base_transmission;
    float4 scene = g_storage_2d[root.scene_texture][pixel];
    g_storage_2d[root.scene_texture][pixel] = float4(vkr_ssr_replace_environment_specular(
        scene.rgb, old_specular, reflection, receiver_weight, confidence), scene.a);
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
  float anisotropy_strength = material.material_anisotropy.x;
  float3 anisotropy_axis = float3(0.0f);
  if (anisotropy_strength > 0.0f) {
    float3 map = float3(1.0f, 0.5f, 1.0f);
    if ((material.flags & 1024u) != 0u)
      map = g_textures[NonUniformResourceIndex(material.anisotropy_texture)].SampleGrad(g_samplers[NonUniformResourceIndex(material.anisotropy_sampler)], surface.uv, surface.uv_dx, surface.uv_dy).rgb;
    anisotropy_strength *= map.b;
    anisotropy_axis = vkr_anisotropy_axis(surface.geometric_normal, surface.normal,
        float4(mul(instance.model, float4(surface.object_tangent.xyz, 0.0f)).xyz, surface.object_tangent.w < 0.0f ? -1.0f : 1.0f), map.rg * 2.0f - 1.0f, material.material_anisotropy.yz);
  }
        VkrGgxMaterialEnergy energy = packet_prepare_brdf(
            frame, no_v, surface.roughness, f0);
    if (anisotropy_strength > 0.0f)
      energy = packet_prepare_anisotropy(frame, surface.normal, anisotropy_axis, view, surface.roughness, anisotropy_strength, f0);
        float clearcoat_factor = saturate(material.material_clearcoat.x);
        if (clearcoat_factor > 0.0f && (material.flags & 32u) != 0u)
            clearcoat_factor *= g_textures[NonUniformResourceIndex(
                                    material.clearcoat_texture)]
                                    .SampleGrad(g_samplers[NonUniformResourceIndex(
                                        material.clearcoat_sampler)],
                                                surface.uv, surface.uv_dx,
                                                surface.uv_dy)
                                    .r;
        bool clearcoat_active = vkr_clearcoat_active(clearcoat_factor);
        VkrClearcoatLayer clearcoat;
        if (clearcoat_active)
        {
            float clearcoat_roughness = material.material_clearcoat.y;
            if ((material.flags & 64u) != 0u)
                clearcoat_roughness *= g_textures[NonUniformResourceIndex(
                                            material.clearcoat_roughness_texture)]
                                            .SampleGrad(g_samplers[NonUniformResourceIndex(
                                                material.clearcoat_roughness_sampler)],
                                                        surface.uv, surface.uv_dx,
                                                        surface.uv_dy)
                                            .g;
            float3 clearcoat_normal = surface.geometric_normal;
            if ((material.flags & 128u) != 0u)
            {
                float3 tangent = mul(instance.model,
                                     float4(surface.object_tangent.xyz, 0.0f)).xyz;
                tangent -= dot(tangent, surface.geometric_normal) *
                           surface.geometric_normal;
                if (vkr_vk_finite_nonzero(tangent))
                {
                    tangent = normalize(tangent);
                    float3 bitangent = cross(surface.geometric_normal, tangent) *
                        (surface.object_tangent.w < 0.0f ? -1.0f : 1.0f);
                    float3 sampled = vkr_normal_map_decode(
                        g_textures[NonUniformResourceIndex(
                            material.clearcoat_normal_texture)]
                            .SampleGrad(g_samplers[NonUniformResourceIndex(
                                material.clearcoat_normal_sampler)],
                                        surface.uv, surface.uv_dx, surface.uv_dy)
                            .xyz,
                        material.material_clearcoat.z);
                    float3 mapped = tangent * sampled.x + bitangent * sampled.y +
                                    surface.geometric_normal * sampled.z;
                    if (vkr_vk_finite_nonzero(mapped))
                        clearcoat_normal = normalize(mapped);
                }
            }
            clearcoat = packet_prepare_clearcoat(
                frame, clearcoat_factor, clearcoat_roughness,
                clearcoat_normal, view);
        }
        float3 sheen_color = max(material.material_sheen.rgb, 0.0f);
        if (vkr_sheen_active(sheen_color) && (material.flags & 256u) != 0u)
            sheen_color *= g_textures[NonUniformResourceIndex(
                                material.sheen_color_texture)]
                               .SampleGrad(g_samplers[NonUniformResourceIndex(
                                               material.sheen_color_sampler)],
                                           surface.uv, surface.uv_dx,
                                           surface.uv_dy)
                               .rgb;
        bool sheen_active = vkr_sheen_active(sheen_color);
        VkrSheenLayer sheen;
        VkrSheenLtcSample sheen_ltc;
        if (sheen_active)
        {
            float sheen_roughness = material.material_sheen.w;
            if ((material.flags & 512u) != 0u)
                sheen_roughness *= g_textures[NonUniformResourceIndex(
                                        material.sheen_roughness_texture)]
                                       .SampleGrad(g_samplers[NonUniformResourceIndex(
                                                       material.sheen_roughness_sampler)],
                                                   surface.uv, surface.uv_dx,
                                                   surface.uv_dy)
                                       .a;
            sheen_ltc = packet_sheen_ltc_sample(frame, no_v, sheen_roughness);
            sheen = packet_prepare_sheen(
                frame, sheen_color, sheen_roughness, surface.normal, view,
                sheen_ltc.allocation_reserve);
        }
        float base_transmission =
            (clearcoat_active ? clearcoat.base_transmission : 1.0f) *
            (sheen_active ? sheen.base_transmission : 1.0f);
        float sheen_transmission =
            clearcoat_active ? clearcoat.base_transmission : 1.0f;
        float3 clearcoat_direct = 0.0f;
        float3 sheen_direct = 0.0f;
        float directional_shadow = 0.0f;
        float3 analytic_diffuse = 0.0f;
        float3 analytic_specular = 0.0f;
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
                    surface.metallic, surface.roughness, f0, energy);
                analytic_diffuse = direct.diffuse;
                analytic_specular = direct.specular;
            }
            else
            {
                analytic_specular = packet_direct_specular(
                    surface.normal, view, light, radiance, surface.roughness,
                    f0, energy);
            }
            if (clearcoat_active)
                clearcoat_direct += packet_clearcoat_direct(
                    view, light, radiance, clearcoat);
            if (sheen_active)
                sheen_direct += packet_sheen_direct(
                    surface.normal, view, light, radiance, sheen,
                    sheen_ltc.energy_normalization);
        }
        if (diffuse_enabled)
        {
            VkrVulkanDirectResult punctual = packet_punctual(
                frame, world, surface.normal, view, surface.base.rgb,
                surface.metallic, surface.roughness, f0, energy);
            analytic_diffuse += punctual.diffuse;
            analytic_specular += punctual.specular;
        }
        else
        {
            analytic_specular += packet_punctual_specular(
                frame, world, surface.normal, view, surface.roughness, f0, energy);
        }
        if (clearcoat_active)
        {
            VkrVulkanDirectResult coat_punctual = packet_punctual(
                frame, world, clearcoat.normal, view, float3(0.0f), 0.0f,
                clearcoat.roughness, float3(VKR_CLEARCOAT_F0), clearcoat.energy);
            clearcoat_direct += coat_punctual.specular * clearcoat.factor;
        }
        if (sheen_active)
            sheen_direct += packet_punctual_sheen(
                frame, world, surface.normal, view, sheen,
                sheen_ltc.energy_normalization);
        VkrVulkanDirectResult rectangles = packet_rectangle_lights(
            frame, world, surface.normal, view, surface.base.rgb,
            surface.metallic, surface.roughness, f0, energy, diffuse_enabled);
        analytic_diffuse += rectangles.diffuse;
        analytic_specular += rectangles.specular;
        if (clearcoat_active)
        {
            VkrVulkanDirectResult coat_rectangles = packet_rectangle_lights(
                frame, world, clearcoat.normal, view, float3(0.0f), 0.0f,
                clearcoat.roughness, float3(VKR_CLEARCOAT_F0), clearcoat.energy,
                false);
            clearcoat_direct += coat_rectangles.specular * clearcoat.factor;
        }
        if (sheen_active)
            sheen_direct += packet_sheen_rectangle_lights(
                frame, world, surface.normal, view, sheen, sheen_ltc);
        if (diagnostic_enabled &&
            (frame.render_mode == 1u || frame.render_mode == 4u ||
             frame.render_mode == 5u))
        {
            color = frame.render_mode == 4u
                        ? analytic_diffuse * base_transmission
                        : (frame.render_mode == 5u
                               ? analytic_specular * base_transmission +
                                     clearcoat_direct +
                                     sheen_direct * sheen_transmission
                               : (analytic_diffuse + analytic_specular) *
                                     base_transmission + clearcoat_direct +
                                     sheen_direct * sheen_transmission);
        }
        else if (diagnostic_enabled && frame.render_mode == 9u)
        {
            // Mode 9 on the transmission path reports the same environment
            // diffuse term the opaque path does, so a transmissive surface does
            // not read as black in the comparison channel.
            color = 0.0f;
            if (diffuse_enabled)
            {
                if ((frame.flags & 2u) != 0u)
                {
                    color = packet_environment_terms(
                                frame, world, surface.normal, surface.normal,
                                view, surface.base.rgb, surface.metallic,
                                surface.roughness, surface.occlusion, energy)
                                .diffuse;
                }
                float3 volume_diffuse;
                if (packet_diffuse_volume(
                        frame, world, surface.normal, surface.base.rgb,
                        surface.metallic, surface.occlusion, energy,
                        volume_diffuse))
                    color = volume_diffuse;
                color *= base_transmission;
            }
        }
        else
        {
            float3 diffuse = analytic_diffuse;
            float3 specular = analytic_specular;
            float3 clearcoat_indirect = 0.0f;
            float3 sheen_indirect = 0.0f;
            if ((frame.flags & 2u) != 0u)
            {
                if (diffuse_enabled)
                {
                    VkrVulkanEnvironmentResult environment =
                        packet_environment_terms(
                            frame, world, surface.normal, surface.normal, view,
                            surface.base.rgb, surface.metallic,
                            surface.roughness, surface.occlusion, energy);
                    diffuse += environment.diffuse;
                    specular += environment.specular;
                }
                else
                {
                    specular += packet_environment_specular(
                        frame, world, surface.normal, surface.normal, view,
                        surface.roughness, surface.occlusion, energy);
                }
            }
            else if (diffuse_enabled)
            {
                diffuse += frame.ambient_color.rgb * surface.base.rgb *
                           surface.occlusion * (1.0f - surface.metallic) *
                           energy.diffuse_weight;
            }
            if (diffuse_enabled)
            {
                float3 volume_diffuse;
                if (packet_diffuse_volume(
                        frame, world, surface.normal, surface.base.rgb,
                        surface.metallic, surface.occlusion, energy,
                        volume_diffuse))
                    diffuse = analytic_diffuse + volume_diffuse;
            }
            if (clearcoat_active)
            {
                if ((frame.flags & 2u) != 0u)
                    clearcoat_indirect = clearcoat.factor *
                        packet_environment_specular(
                        frame, world, clearcoat.normal, clearcoat.normal, view,
                        clearcoat.roughness, surface.occlusion, clearcoat.energy);
            }
            if (sheen_active && (frame.flags & 2u) != 0u)
                sheen_indirect = packet_sheen_environment(
                    frame, world, surface.normal, view, surface.occlusion,
                    1.0f, sheen);
            diffuse *= base_transmission;
            specular *= base_transmission;
            sheen_direct *= sheen_transmission;
            sheen_indirect *= sheen_transmission;
            specular += clearcoat_direct + clearcoat_indirect +
                        sheen_direct + sheen_indirect;
            float3 base_reflectance = sheen_active
                ? sheen.color * sheen.directional_albedo +
                      sheen.base_transmission * energy.reflectance
                : energy.reflectance;
            float3 layered_reflectance = clearcoat_active
                ? clearcoat.factor * clearcoat.energy.reflectance +
                      clearcoat.base_transmission * base_reflectance
                : base_reflectance;
            VkrTransmissionLobes lobes = {
                diffuse, specular, surface.emissive * base_transmission
            };
            VkrFroxelFogParams froxel_params = frame.froxel_fog[0];
            if (vkr_froxel_enabled(froxel_params))
            {
                float dielectric = 1.0f - saturate(surface.metallic);
                float3 feedback_weight = transmission * dielectric *
                    surface.base.rgb * (1.0f - saturate(layered_reflectance));
                float3 local = vkr_transmission_compose(
                    lobes, float3(0.0f), surface.base.rgb,
                    layered_reflectance, transmission, surface.metallic);
                VkrTransmissionLobes empty_lobes = {
                    float3(0.0f), float3(0.0f), float3(0.0f)
                };
                float3 feedback = vkr_transmission_compose(
                    empty_lobes, transmitted, surface.base.rgb,
                    layered_reflectance, transmission, surface.metallic);
                VkrFroxelSample froxel = packet_froxel_sample(frame, world);
                color = vkr_froxel_apply_local_over_feedback(
                            local, feedback_weight, froxel) +
                        feedback;
            }
            else if (frame.fog[0].color_density.w > 0.0f)
            {
                float dielectric = 1.0f - saturate(surface.metallic);
                float3 feedback_weight = transmission * dielectric *
                    surface.base.rgb * (1.0f - saturate(layered_reflectance));
                float3 local = vkr_transmission_compose(
                    lobes, float3(0.0f), surface.base.rgb,
                    layered_reflectance, transmission, surface.metallic);
                VkrTransmissionLobes empty_lobes = {
                    float3(0.0f), float3(0.0f), float3(0.0f)
                };
                float3 feedback = vkr_transmission_compose(
                    empty_lobes, transmitted, surface.base.rgb,
                    layered_reflectance, transmission, surface.metallic);
                color = vkr_fog_apply_local_over_feedback(
                            local, feedback_weight, frame.fog[0],
                            vkr_fog_surface_sample(
                                frame.fog[0], frame.view_position.xyz,
                                world)) +
                        feedback;
            }
            else
            {
                color = vkr_transmission_compose(
                    lobes, transmitted, surface.base.rgb,
                    layered_reflectance, transmission, surface.metallic);
            }
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

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
    VkrBindlessVertex* vertices;
    uint* indices;
    uint first_vertex;
    uint first_index;
    uint vertex_stride;
    uint vertex_layout;
    uint publication_generation;
    uint flags;
    uint2 reserved;
};

struct VkrVkCullRoot
{
    VkrGpuCandidateDrawRow* candidates;
    uint* classifications;
    VkrGpuVisibleDrawRow* visible;
    VkrVkDrawState* states;
    VkrVkIndirectCommand* commands;
    VkrBindlessInstance* instances;
    float4x4* view_projections;
    float4* frustum_planes;
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
    VkrVkGeometryRow* geometry_rows;
    VkrGpuVisibleDrawRow* visible_rows;
    VkrVkDrawState* states;
    VkrBindlessPacketFrameRoot* frame;
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

struct VkrVkResolveRoot
{
    VkrVkGeometryRow* geometry_rows;
    VkrGpuVisibleDrawRow* visible_rows;
    uint2 reserved_address;
    VkrBindlessInstance* instances;
    VkrBindlessMaterial* materials;
    VkrBindlessVertex* vertices;
    uint* indices;
    VkrVkDrawState* compaction_state;
    float4x4 view_projection;
    uint vbuffer_texture;
    uint albedo_texture;
    uint specular_texture;
    uint normal_texture;
    uint emissive_texture;
    uint debug_texture;
    uint scene_texture;
    uint2 extent;
    uint visible_capacity;
    uint geometry_count;
    uint material_count;
    uint instance_count;
    uint render_mode;
    uint2 reserved_tail;
};

struct VkrVkLightingRoot
{
    VkrBindlessPacketFrameRoot* frame;
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
    uint reserved_sky;
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

struct VkrVkPickingRoot
{
    VkrGpuVisibleDrawRow* opaque_visible;
    VkrGpuVisibleDrawRow* transmission_visible;
    VkrBindlessInstance* opaque_instances;
    VkrBindlessInstance* transmission_instances;
    uint opaque_vbuffer;
    uint transmission_vbuffer;
    uint output_texture;
    uint2 pixel;
    uint transmission_layer;
    uint use_transmission;
};

struct VkrVkTransmissionRoot
{
    VkrGpuVisibleDrawRow* visible_rows;
    VkrBindlessMaterial* materials;
    // Offset 16 stays free of physical-buffer addresses; see the resolve root.
    uint2 reserved_address;
    VkrVkGeometryRow* geometry_rows;
    VkrBindlessInstance* instances;
    VkrBindlessVertex* vertices;
    uint* indices;
    VkrVkDrawState* compaction_state;
    VkrBindlessPacketFrameRoot* frame;
    uint2 frame_address_padding;
    float4x4 view_projection;
    float4x4 inverse_view_projection;
    uint vbuffer_texture;
    uint depth_texture;
    uint feedback_texture;
    uint feedback_sampler;
    uint output_texture;
    uint layer;
    uint2 extent;
    uint visible_capacity;
    uint geometry_count;
    uint material_count;
    uint instance_count;
};

struct VkrVkTransmissionCoverageRoot
{
    uint* covered_pixels;
    uint vbuffer_texture;
    uint layer;
    uint2 extent;
    uint2 reserved;
};

struct VkrVkCullPush { VkrVkCullRoot* root; uint2 reserved; };
struct VkrVkRasterPush { VkrVkRasterRoot* root; uint bucket; uint reserved; };
struct VkrVkResolvePush { VkrVkResolveRoot* root; uint2 reserved; };
struct VkrVkLightingPush { VkrVkLightingRoot* root; uint2 reserved; };
struct VkrVkHzbPush { VkrVkHzbRoot* root; uint2 reserved; };
struct VkrVkPickingPush { VkrVkPickingRoot* root; uint2 reserved; };
struct VkrVkTransmissionPush { VkrVkTransmissionRoot* root; uint2 reserved; };
struct VkrVkTransmissionCoveragePush
{
    VkrVkTransmissionCoverageRoot* root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkCullPush> g_cull_push;
[[vk::push_constant]] ConstantBuffer<VkrVkRasterPush> g_raster_push;
[[vk::push_constant]] ConstantBuffer<VkrVkResolvePush> g_resolve_push;
[[vk::push_constant]] ConstantBuffer<VkrVkLightingPush> g_lighting_push;
[[vk::push_constant]] ConstantBuffer<VkrVkHzbPush> g_hzb_push;
[[vk::push_constant]] ConstantBuffer<VkrVkPickingPush> g_picking_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTransmissionPush> g_transmission_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTransmissionCoveragePush>
    g_transmission_coverage_push;

static VkrVkCullRoot* vkr_vk_cull_root()
{
    return g_cull_push.root;
}

static bool vkr_vk_sphere_visible(VkrVkCullRoot* root, float4 sphere,
                                  uint view_index)
{
    float radius = max(sphere.w, 0.0f);
    [unroll]
    for (uint plane = 0u; plane < VKR_VK_FRUSTUM_PLANE_COUNT; ++plane)
    {
        float4 equation = root.frustum_planes[
            view_index * VKR_VK_FRUSTUM_PLANE_COUNT + plane];
        if (dot(equation.xyz, sphere.xyz) + equation.w < -radius)
            return false;
    }
    return true;
}

static bool vkr_vk_candidate_occluded(VkrVkCullRoot* root,
                                      VkrGpuCandidateDrawRow candidate)
{
    if (root.hzb_enabled == 0u || root.hzb_mip_count == 0u ||
        any(root.hzb_extent == 0u) || (candidate.flags & 1u) == 0u)
        return false;
    VkrBindlessInstance instance = root.instances[candidate.instance_index];
    float3 center = mul(instance.model,
                        float4(candidate.local_bounding_sphere.xyz, 1.0f)).xyz;
    float scale = max(length(instance.model[0].xyz),
                      max(length(instance.model[1].xyz),
                          length(instance.model[2].xyz)));
    float radius = max(candidate.local_bounding_sphere.w * scale, 0.0f);
    float2 ndc_min = 1e30f;
    float2 ndc_max = -1e30f;
    float nearest_depth = 1.0f;
    [unroll]
    for (uint corner = 0u; corner < 8u; ++corner)
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
    float2 uv_min = clamp(float2(ndc_min.x, -ndc_max.y) * 0.5f + 0.5f,
                          0.0f, 1.0f);
    float2 uv_max = clamp(float2(ndc_max.x, -ndc_min.y) * 0.5f + 0.5f,
                          0.0f, 1.0f);
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
                            [uint2(x, y)].x;
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
    VkrVkCullRoot* root = vkr_vk_cull_root();
    uint total = root.candidate_count * root.view_count;
    if (thread_id.x >= total)
        return;
    uint view_index = thread_id.x / root.candidate_count;
    uint candidate_index = thread_id.x - view_index * root.candidate_count;
    VkrGpuCandidateDrawRow candidate = root.candidates[candidate_index];
    uint required_flags = view_index == 0u ? root.camera_required_flags
                                          : root.shadow_required_flags;
    if (candidate.state_bucket >= VKR_VK_DRAW_BUCKET_COUNT ||
        (candidate.flags & required_flags) != required_flags)
    {
        root.classifications[view_index * root.candidate_capacity + candidate_index] =
            0xffffffffu;
        return;
    }
    VkrBindlessInstance instance = root.instances[candidate.instance_index];
    float4 local = candidate.local_bounding_sphere;
    float4 world_center = mul(instance.model, float4(local.xyz, 1.0f));
    float3 scale_x = instance.model[0].xyz;
    float3 scale_y = instance.model[1].xyz;
    float3 scale_z = instance.model[2].xyz;
    float world_radius = local.w * max(length(scale_x), max(length(scale_y), length(scale_z)));
    uint classification_index = view_index * root.candidate_capacity + candidate_index;
    float4 world_sphere = float4(world_center.xyz, world_radius);
    if ((candidate.flags & 1u) != 0u &&
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
    uint bucket = min(candidate.state_bucket, VKR_VK_DRAW_BUCKET_COUNT - 1u);
    root.classifications[classification_index] = bucket;
    uint ignored;
    InterlockedAdd(root.states[view_index].bucket_counts[bucket], 1u, ignored);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void vk_gpu_draw_prefix(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkCullRoot* root = vkr_vk_cull_root();
    uint view_index = thread_id.x;
    if (view_index >= root.view_count)
        return;
    uint cursor = 0u;
    uint overflow = 0u;
    [unroll]
    for (uint bucket = 0u; bucket < VKR_VK_DRAW_BUCKET_COUNT; ++bucket)
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
    VkrVkCullRoot* root = vkr_vk_cull_root();
    uint total = root.candidate_count * root.view_count;
    if (thread_id.x >= total)
        return;
    uint view_index = thread_id.x / root.candidate_count;
    uint candidate_index = thread_id.x - view_index * root.candidate_count;
    uint classification_index = view_index * root.candidate_capacity + candidate_index;
    uint bucket = root.classifications[classification_index];
    if (bucket >= VKR_VK_DRAW_BUCKET_COUNT)
        return;
    uint local_index;
    InterlockedAdd(root.states[view_index].bucket_cursors[bucket], 1u, local_index);
    if (local_index >= root.command_partition_capacity)
        return;
    uint visible_index = root.states[view_index].execution_ranges[bucket].x + local_index;
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
    visible.state_bucket = bucket;
    visible.flags = candidate.flags;
    root.visible[view_index * root.candidate_capacity + visible_index] = visible;
    uint argument_index = (view_index * VKR_VK_DRAW_BUCKET_COUNT + bucket) *
                          root.command_partition_capacity + local_index;
    VkrVkIndirectCommand command;
    command.index_count = candidate.index_count;
    command.instance_count = 1u;
    command.first_index = candidate.first_index;
    command.vertex_offset = candidate.vertex_offset;
    command.first_instance = 0u;
    root.commands[argument_index] = command;
}

[shader("vertex")]
VkrVkVisibilityOutput vk_visibility_vertex(uint vertex_id : SV_VertexID,
                                           uint draw_index : SV_DrawIndex)
{
    VkrVkRasterRoot* root = g_raster_push.root;
    uint visible_instance =
        root.states[root.view_index].execution_ranges[g_raster_push.bucket].x +
        draw_index;
    uint visible_index = root.view_index * root.visible_capacity + visible_instance;
    VkrGpuVisibleDrawRow visible = root.visible_rows[visible_index];
    VkrVkGeometryRow geometry = root.geometry_rows[visible.geometry_index];
    VkrBindlessVertex vertex = geometry.vertices[geometry.first_vertex + vertex_id];
    VkrBindlessInstance instance = root.frame.instances[visible.instance_index];
    VkrVkVisibilityOutput output;
    output.position = mul(root.frame.view_projection,
                          mul(instance.model, float4(vertex.position, 1.0f)));
    output.uv = vertex.texcoord;
    output.color = vertex.color;
    output.visible_index = visible_instance;
    return output;
}

[shader("fragment")]
VkrVkVisibilityFragment vk_visibility_fragment(
    VkrVkVisibilityOutput input, uint primitive_id : SV_PrimitiveID,
    bool front_facing : SV_IsFrontFace)
{
    VkrVkRasterRoot* root = g_raster_push.root;
    if (root.previous_depth_texture != 0xffffffffu)
    {
        float previous_depth =
            g_array_textures[root.previous_depth_texture].Load(
                int4(int2(input.position.xy), int(root.previous_depth_layer), 0)).r;
        if (input.position.z <= previous_depth + 1e-5f)
            discard;
    }
    VkrGpuVisibleDrawRow visible = root.visible_rows[
        root.view_index * root.visible_capacity + input.visible_index];
    VkrBindlessMaterial material = root.frame.materials[visible.material_index];
    if (material.alpha_mode == 1u)
    {
        float alpha = g_textures[NonUniformResourceIndex(material.base_color_texture)]
                          .Sample(g_samplers[NonUniformResourceIndex(material.base_color_sampler)], input.uv).a *
                      material.tint.a * input.color.a;
        if (alpha < material.material_alpha.x)
            discard;
    }
    VkrVkVisibilityFragment output;
    output.value = uint2(
        input.visible_index + 1u,
        primitive_id | (front_facing ? VKR_VK_FRONT_FACE_BIT : 0u));
    return output;
}

// Camera-opaque visibility draws reach neither discard above. A shader that can
// discard prevents unconditional early depth writes, so opaque buckets use this
// fragment with early tests. Cutouts retain alpha testing, and transmission
// retains the preceding peel-depth rejection.
[shader("fragment")]
[earlydepthstencil]
VkrVkVisibilityFragment vk_visibility_opaque_fragment(
    VkrVkVisibilityOutput input, uint primitive_id : SV_PrimitiveID,
    bool front_facing : SV_IsFrontFace)
{
    VkrVkVisibilityFragment output;
    output.value = uint2(
        input.visible_index + 1u,
        primitive_id | (front_facing ? VKR_VK_FRONT_FACE_BIT : 0u));
    return output;
}

[shader("fragment")]
void vk_visibility_shadow_fragment(VkrVkVisibilityOutput input)
{
    VkrVkRasterRoot* root = g_raster_push.root;
    VkrGpuVisibleDrawRow visible = root.visible_rows[
        root.view_index * root.visible_capacity + input.visible_index];
    VkrBindlessMaterial material = root.frame.materials[visible.material_index];
    if (material.alpha_mode == 1u)
    {
        float alpha = g_textures[NonUniformResourceIndex(material.base_color_texture)]
                          .Sample(g_samplers[NonUniformResourceIndex(material.base_color_sampler)], input.uv).a *
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
    float3 horizontal = float3(ca.x - point.x * ca.w,
                               cb.x - point.x * cb.w,
                               cc.x - point.x * cc.w);
    float3 vertical = float3(ca.y - point.y * ca.w,
                             cb.y - point.y * cb.w,
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
    barycentric_dx =
        (weights_dx - barycentric * dot(weights_dx, 1.0f)) /
        normalization * (2.0f / float(extent.x));
    barycentric_dy =
        (weights_dy - barycentric * dot(weights_dy, 1.0f)) /
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
void vkr_vk_resolve_defaults(VkrVkResolveRoot* root, uint2 pixel, float marker)
{
    g_storage_2d[root.albedo_texture][pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_storage_2d[root.specular_texture][pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_storage_float2_2d[root.normal_texture][pixel] = 0.0f;
    g_storage_2d[root.emissive_texture][pixel] = 0.0f;
    g_storage_2d[root.debug_texture][pixel] = float4(0.0f, 0.0f, 0.0f, marker);
    g_storage_2d[root.scene_texture][pixel] = 0.0f;
}

// One visibility sample reconstructed back into shadeable material inputs.
// The G-buffer resolve and every transmission layer read the same encoding, so
// they share this reconstruction rather than keeping two copies of the
// barycentric, gradient, tangent-frame, and validity rules.
struct VkrVkSurfaceSource
{
    VkrVkGeometryRow* geometry_rows;
    VkrGpuVisibleDrawRow* visible_rows;
    VkrBindlessInstance* instances;
    VkrBindlessMaterial* materials;
    VkrBindlessVertex* vertices;
    uint* indices;
    VkrVkDrawState* compaction_state;
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
    float3 object_normal;
    float4 object_tangent;
    float3 tangent_normal;
    float3 emissive;
    float3 barycentric;
    float face_sign;
    float metallic;
    float roughness;
    float occlusion;
    float selected_lod;
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
    VkrBindlessMaterial material = source.materials[visible.material_index];
    VkrVkGeometryRow geometry = source.geometry_rows[visible.geometry_index];
    uint triangle = primitive_id * 3u;
    int ia = int(source.indices[visible.first_index + triangle + 0u]) + visible.vertex_offset;
    int ib = int(source.indices[visible.first_index + triangle + 1u]) + visible.vertex_offset;
    int ic = int(source.indices[visible.first_index + triangle + 2u]) + visible.vertex_offset;
    VkrBindlessVertex a = source.vertices[geometry.first_vertex + uint(ia)];
    VkrBindlessVertex b = source.vertices[geometry.first_vertex + uint(ib)];
    VkrBindlessVertex c = source.vertices[geometry.first_vertex + uint(ic)];
    VkrBindlessInstance instance = source.instances[visible.instance_index];
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
    if (!vkr_vk_perspective_barycentric(
            vkr_vk_resolve_ndc(p, source.extent), ca, cb, cc, source.extent,
            perspective, perspective_dx, perspective_dy))
        return vkr_vk_reject_surface(source);
    float2 uv = a.texcoord * perspective.x + b.texcoord * perspective.y + c.texcoord * perspective.z;
    float2 uv_dx = a.texcoord * perspective_dx.x + b.texcoord * perspective_dx.y +
                   c.texcoord * perspective_dx.z;
    float2 uv_dy = a.texcoord * perspective_dy.x + b.texcoord * perspective_dy.y +
                   c.texcoord * perspective_dy.z;
    float4 color = a.color * perspective.x + b.color * perspective.y + c.color * perspective.z;
    float4 base = g_textures[NonUniformResourceIndex(material.base_color_texture)]
                      .SampleGrad(g_samplers[NonUniformResourceIndex(material.base_color_sampler)],
                                  uv, uv_dx, uv_dy) *
                  material.tint * color;
    float metallic = saturate(material.material_surface.x);
    float roughness = clamp(material.material_surface.y, 0.04f, 1.0f);
    float ao = saturate(material.material_surface.w);
    if ((material.flags & 2u) != 0u)
    {
        float3 orm = g_textures[NonUniformResourceIndex(material.orm_texture)]
                         .SampleGrad(g_samplers[NonUniformResourceIndex(material.orm_sampler)],
                                     uv, uv_dx, uv_dy).rgb;
        ao *= orm.r;
        roughness = clamp(roughness * orm.g, 0.04f, 1.0f);
        metallic = saturate(metallic * orm.b);
    }
    float3 object_normal = a.normal * perspective.x + b.normal * perspective.y +
                           c.normal * perspective.z;
    float4 object_tangent = a.tangent * perspective.x + b.tangent * perspective.y +
                            c.tangent * perspective.z;
    if ((material.flags & 1u) != 0u)
    {
        surface.tangent_normal =
            g_textures[NonUniformResourceIndex(material.normal_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(material.normal_sampler)],
                            uv, uv_dx, uv_dy).xyz * 2.0f - 1.0f;
        surface.tangent_normal.xy *= material.material_surface.z;
        surface.tangent_normal.y = -surface.tangent_normal.y;
        surface.normal_mapped = 1u;
    }
    float3 emissive = material.material_emissive.rgb;
    if ((material.flags & 4u) != 0u)
        emissive *= g_textures[NonUniformResourceIndex(material.emissive_texture)]
                        .SampleGrad(g_samplers[NonUniformResourceIndex(material.emissive_sampler)],
                                    uv, uv_dx, uv_dy).rgb;
    uint2 base_color_extent;
    g_textures[NonUniformResourceIndex(material.base_color_texture)]
        .GetDimensions(base_color_extent.x, base_color_extent.y);
    float2 texture_extent = float2(base_color_extent);
    float footprint = max(length(uv_dx * texture_extent),
                          length(uv_dy * texture_extent));
    surface.base = base;
    surface.object_normal = object_normal;
    surface.object_tangent = object_tangent;
    surface.emissive = emissive;
    surface.barycentric = perspective;
    surface.metallic = metallic;
    surface.roughness = roughness;
    surface.occlusion = ao;
    surface.selected_lod = max(log2(max(footprint, 1e-8f)), 0.0f);
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
        float tangent_handedness = surface.object_tangent.w < 0.0f ? -1.0f : 1.0f;
        // `normal` and `tangent` are unit and orthogonal by the Gram-Schmidt
        // step above, so the cross product is unit and `mapped` has exactly the
        // magnitude of `tangent_normal`, which the guard above already
        // established as nonzero. Both former checks were implied.
        float3 bitangent = normalize(cross(normal, tangent)) * tangent_handedness;
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
    VkrVkResolveRoot* root = g_resolve_push.root;
    if (any(thread_id.xy >= root.extent))
        return;
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy];
    if (encoded.x == 0u)
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, 0.0f);
        return;
    }
    VkrVkSurfaceSource source = {
        root.geometry_rows, root.visible_rows, root.instances, root.materials,
        root.vertices, root.indices, root.compaction_state,
        root.view_projection, root.extent, root.visible_capacity,
        root.geometry_count, root.material_count, root.instance_count,
    };
    VkrVkSurface surface;
    if (!vkr_vk_resolve_surface(source, encoded, thread_id.xy, surface))
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, -1.0f);
        return;
    }
    VkrBindlessInstance instance = source.instances[surface.instance_index];
    // Opaque and transmission own different fixed matrix operations. Matched
    // retained-raster captures require this opaque OpMatrixTimesVector form;
    // keeping it at the entry call site avoids a per-pixel convention selector.
    if (!vkr_vk_finish_surface_normal(
            source,
            mul(float4(surface.object_normal, 0.0f), instance.model).xyz,
            mul(float4(surface.object_tangent.xyz, 0.0f), instance.model).xyz,
            surface))
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, -1.0f);
        return;
    }
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

// Deferred owns the background because the authored skybox pass is declared
// The deferred lighting path owns the background colour when no cubemap is
// available.
float3 vkr_vk_deferred_sky(VkrVkLightingRoot* root, uint2 pixel)
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
        .SampleLevel(g_samplers[root.sky_sampler], direction, 0.0f).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_deferred_lighting(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkLightingRoot* root = g_lighting_push.root;
    if (any(thread_id.xy >= root.extent))
        return;
    VkrBindlessPacketFrameRoot* frame = root.frame;
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy];
    if (encoded.x == 0u)
    {
        g_storage_2d[root.scene_texture][thread_id.xy] =
            float4(vkr_vk_deferred_sky(root, thread_id.xy), 1.0f);
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
    if (frame.render_mode == 6u)
    {
        g_storage_2d[root.scene_texture][thread_id.xy] =
            float4(seed.a, roughness, max(f0.x, max(f0.y, f0.z)), 1.0f);
        return;
    }
    float depth = g_depth_textures[root.depth_texture].Load(int3(thread_id.xy, 0));
    // See vkr_vk_deferred_sky: no Y flip when inverting the view-projection.
    float2 ndc_xy = (float2(thread_id.xy) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
    float3 world = vkr_vk_deferred_homogeneous(
        mul(root.inverse_view_projection, float4(ndc_xy, depth, 1.0f)));
    float3 view = normalize(frame.view_position.xyz - world);
    // The G-buffer already folded metallic into diffuse albedo and F0, so the
    // shared helpers take a zero metallic factor here.
    float3 analytic_diffuse = 0.0f;
    float3 analytic_specular = 0.0f;
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        float shadow = packet_directional_shadow(frame, world);
        VkrBindlessDirectResult direct = packet_direct(
            normal, view, normalize(-frame.directional_direction_enabled.xyz),
            frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * shadow,
            diffuse_albedo, 0.0f, roughness, f0);
        analytic_diffuse = direct.diffuse;
        analytic_specular = direct.specular;
    }
    VkrBindlessDirectResult punctual = packet_punctual(
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
    float3 color = analytic_diffuse + analytic_specular;
    if ((frame.flags & 2u) != 0u)
    {
        color += packet_environment(frame, world, normal, normal, view,
                                    diffuse_albedo, 0.0f, roughness, ao, f0);
    }
    else
    {
        color += frame.ambient_color.rgb * diffuse_albedo * ao;
    }
    color += seed.rgb;
    g_storage_2d[root.scene_texture][thread_id.xy] = float4(color, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_hzb_build(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkHzbRoot* root = g_hzb_push.root;
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
    uint2 source = thread_id.xy * 2u;
    float maximum = 0.0f;
    [unroll]
    for (uint y = 0u; y < 2u; ++y)
    [unroll]
    for (uint x = 0u; x < 2u; ++x)
    {
        uint2 p = min(source + uint2(x, y), root.source_extent - 1u);
        float value = root.source_is_depth != 0u
                          ? g_depth_textures[root.source_texture].Load(int3(p, 0))
                          : g_storage_2d[root.source_texture][p].x;
        maximum = max(maximum, value);
    }
    g_storage_2d[root.destination_texture][thread_id.xy] = float4(maximum, 0.0f, 0.0f, 0.0f);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void vk_picking_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkPickingRoot* root = g_picking_push.root;
    uint2 opaque_encoded = g_storage_uint2_2d[root.opaque_vbuffer][root.pixel];
    uint2 transmission_encoded = root.use_transmission != 0u
                                     ? g_storage_uint2_images[root.transmission_vbuffer][uint3(root.pixel, root.transmission_layer)]
                                     : uint2(0u);
    bool selected_transmission = transmission_encoded.x != 0u;
    uint2 encoded = selected_transmission ? transmission_encoded : opaque_encoded;
    uint identifier = 0u;
    if (encoded.x != 0u)
    {
        VkrGpuVisibleDrawRow visible = selected_transmission
                                           ? root.transmission_visible[encoded.x - 1u]
                                           : root.opaque_visible[encoded.x - 1u];
        identifier = selected_transmission
                         ? root.transmission_instances[visible.instance_index].object_id
                         : root.opaque_instances[visible.instance_index].object_id;
    }
    g_storage_uint_2d[root.output_texture][root.pixel] = identifier;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_shade(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkTransmissionRoot* root = g_transmission_push.root;
    if (any(thread_id.xy >= root.extent))
        return;
    float2 screen_uv = (float2(thread_id.xy) + 0.5f) / float2(root.extent);
    float4 background = g_textures[root.feedback_texture].SampleLevel(
        g_samplers[root.feedback_sampler], screen_uv, 0.0f);
    uint2 encoded =
        g_storage_uint2_images[root.vbuffer_texture][uint3(thread_id.xy, root.layer)];
    if (encoded.x == 0u)
    {
        g_storage_2d[root.output_texture][thread_id.xy] = background;
        return;
    }
    VkrVkSurfaceSource source = {
        root.geometry_rows, root.visible_rows, root.instances, root.materials,
        root.vertices, root.indices, root.compaction_state,
        root.view_projection, root.extent, root.visible_capacity,
        root.geometry_count, root.material_count, root.instance_count,
    };
    VkrVkSurface surface;
    if (!vkr_vk_resolve_surface(source, encoded, thread_id.xy, surface))
    {
        g_storage_2d[root.output_texture][thread_id.xy] = background;
        return;
    }
    VkrBindlessInstance instance = source.instances[surface.instance_index];
    if (!vkr_vk_finish_surface_normal(
            source,
            mul(instance.model, float4(surface.object_normal, 0.0f)).xyz,
            mul(instance.model, float4(surface.object_tangent.xyz, 0.0f)).xyz,
            surface))
    {
        g_storage_2d[root.output_texture][thread_id.xy] = background;
        return;
    }
    VkrBindlessPacketFrameRoot* frame = root.frame;
    VkrBindlessMaterial material = root.materials[surface.material_index];
    float depth = g_depth_textures[root.depth_texture]
                      .Load(int3(thread_id.xy, 0));
    // See vkr_vk_deferred_sky: no Y flip when inverting the view-projection.
    float2 ndc_xy = screen_uv * 2.0f - 1.0f;
    float3 world = vkr_vk_deferred_homogeneous(
        mul(root.inverse_view_projection, float4(ndc_xy, depth, 1.0f)));
    float3 f0 = lerp(saturate(material.material_dielectric_specular.rgb),
                     surface.base.rgb, surface.metallic);
    float3 view = normalize(frame.view_position.xyz - world);
    float no_v = max(dot(surface.normal, view), 0.0f);
    float3 color;
    // Transmissive surfaces keep their unfolded base colour and metallic
    // factor, so the shared helpers take the same arguments the forward
    // fragment shader passes rather than the G-buffer's folded albedo.
    if (frame.render_mode == 2u)
    {
        color = surface.normal * 0.5f + 0.5f;
    }
    else if (frame.render_mode == 3u)
    {
        color = surface.base.rgb + surface.emissive;
    }
    else if (frame.render_mode == 6u)
    {
        color = float3(surface.metallic, surface.roughness,
                       max(f0.x, max(f0.y, f0.z)));
    }
    else
    {
        float3 analytic_diffuse = 0.0f;
        float3 analytic_specular = 0.0f;
        if (frame.directional_direction_enabled.w > 0.5f)
        {
            float shadow = packet_directional_shadow(frame, world);
            VkrBindlessDirectResult direct = packet_direct(
                surface.normal, view,
                normalize(-frame.directional_direction_enabled.xyz),
                frame.directional_color_intensity.rgb *
                    frame.directional_color_intensity.w * shadow,
                surface.base.rgb, surface.metallic, surface.roughness, f0);
            analytic_diffuse = direct.diffuse;
            analytic_specular = direct.specular;
        }
        VkrBindlessDirectResult punctual = packet_punctual(
            frame, world, surface.normal, view, surface.base.rgb,
            surface.metallic, surface.roughness, f0);
        analytic_diffuse += punctual.diffuse;
        analytic_specular += punctual.specular;
        if (frame.render_mode == 1u || frame.render_mode == 4u ||
            frame.render_mode == 5u)
        {
            color = frame.render_mode == 4u
                        ? analytic_diffuse
                        : (frame.render_mode == 5u
                               ? analytic_specular
                               : analytic_diffuse + analytic_specular);
        }
        else
        {
            color = analytic_diffuse + analytic_specular;
            if ((frame.flags & 2u) != 0u)
            {
                color += packet_environment(
                    frame, world, surface.normal, surface.normal, view,
                    surface.base.rgb, surface.metallic, surface.roughness,
                    surface.occlusion, f0);
            }
            else
            {
                color += frame.ambient_color.rgb * surface.base.rgb *
                         surface.occlusion;
            }
            color += surface.emissive;
            if (material.material_alpha.y > 0.0f)
            {
                // Refraction reads the immutable layer background at a
                // thickness-scaled screen offset, then attenuates over the
                // travelled distance and gives up whatever the surface
                // reflects.
                float thickness = max(material.material_alpha.w, 0.0f);
                float3 view_normal =
                    normalize(mul(frame.view, float4(surface.normal, 0.0f)).xyz);
                float3 refracted =
                    refract(float3(0.0f, 0.0f, -1.0f), view_normal,
                            1.0f / max(material.material_alpha.z, 1.0f));
                float2 offset = refracted.xy / max(abs(refracted.z), 0.25f) *
                                thickness * 0.02f;
                float3 transmitted =
                    g_textures[root.feedback_texture].SampleLevel(
                        g_samplers[root.feedback_sampler],
                        clamp(screen_uv + offset, 0.0f, 1.0f), 0.0f).rgb;
                if (material.material_attenuation_color.w > 1e-4f &&
                    thickness > 0.0f)
                {
                    transmitted *= pow(
                        clamp(material.material_attenuation_color.rgb, 1e-4f, 1.0f),
                        thickness / material.material_attenuation_color.w);
                }
                float3 fresnel = packet_fresnel(no_v, f0);
                float weight = saturate(material.material_alpha.y) *
                               (1.0f - surface.metallic) *
                               (1.0f - max(fresnel.x, max(fresnel.y, fresnel.z)));
                color = lerp(color, transmitted, saturate(weight));
            }
        }
    }
    g_storage_2d[root.output_texture][thread_id.xy] = float4(color, 1.0f);
}

groupshared uint g_transmission_group_coverage;

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_coverage(uint3 thread_id : SV_DispatchThreadID,
                              uint3 group_thread_id : SV_GroupThreadID)
{
    VkrVkTransmissionCoverageRoot* root =
        g_transmission_coverage_push.root;
    if (all(group_thread_id == 0u))
        g_transmission_group_coverage = 0u;
    GroupMemoryBarrierWithGroupSync();

    // One shared-memory atomic per wave rather than one per covered lane; a
    // full-screen dispatch otherwise serializes 64 lanes onto one address.
    bool covered = false;
    if (all(thread_id.xy < root.extent))
    {
        covered = g_storage_uint2_images[root.vbuffer_texture][
            uint3(thread_id.xy, root.layer)].x != 0u;
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


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

// Each state bucket's slice of a culling view's indirect commands: its first
// command row and its row count, the frame's packed candidates of that
// bucket. A bucket belongs to the candidate and a view classifies each
// candidate at most once, so no view's bucket outgrows its slice.
struct VkrVkCommandPartitions
{
    uint first[VKR_VK_DRAW_BUCKET_COUNT];
    uint capacity[VKR_VK_DRAW_BUCKET_COUNT];
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
    // Rows of each view in the classification, visible and command tables:
    // a view writes at most one row per candidate.
    uint candidate_capacity;
    uint hzb_textures[VKR_VK_HZB_MIP_MAX];
    uint hzb_mip_count;
    uint2 hzb_extent;
    uint hzb_enabled;
    float hzb_depth_epsilon;
    uint camera_required_flags;
    uint shadow_required_flags;
    uint local_shadow_first_view;
    uint transmission_first_view;
    uint transmission_required_flags;
    uint local_shadow_excluded_flags;
    // Retained cascades' views, whose passes do not run this frame.
    uint encode_idle_view_mask;
    // Candidates below it are static; a local face view whose LOD flags
    // select one mobility skips the others.
    uint static_candidate_count;
    VkrGpuLodView *lod_views;
    VkrVkGeometryRow *geometry_rows;
    VkrVkCommandPartitions command_partitions;
    // The planar reflection's mirror plane and culling view (UINT_MAX
    // without one), and its reflector's object id.
    float4 reflection_plane;
    uint reflection_view;
    uint reflection_object_id;
    // Candidate flags the directional cascades skip: glass casts no sun
    // shadow.
    uint cascade_excluded_flags;
};

struct VkrVkRasterRoot
{
    VkrVkGeometryRow *geometry_rows;
    VkrGpuVisibleDrawRow *visible_rows;
    VkrVkDrawState *states;
    VkrVulkanPacketFrameRoot *frame;
    uint view_index;
    // Visible rows of each culling view: the stream's candidate capacity.
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

// A planar reflection's mirrored view: the surface the forward shading
// lights, and the visible row it belongs to.
struct VkrVkReflectionOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 world_position : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float4 world_tangent : TEXCOORD3;
    nointerpolation uint visible_index : TEXCOORD4;
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
    VkrVulkanTerrainMaterial *terrain_materials;
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
    float pre_exposure;
    float4x4 sky_reprojection;
    // The frame's decals (ADR-092): rows in compositing order and the world
    // grid's uint2 cell masks. dimensions_count.w is the decal count; zero
    // leaves both addresses unused.
    VkrVulkanDecalRow *decals;
    uint2 *decal_masks;
    float4 decal_grid_origin_cell_size;
    uint4 decal_grid_dimensions_count;
    // Baked static lamps (ADR-104): lightmapped draws'
    // page rectangles, the lamp-layer texture array whose slice
    // page * lightmap_layer_count + layer holds one layer of a page, and the
    // active layers with their weights. A zero rectangle count writes zero
    // into baked_lamps_texture, which is the sentinel without baked lamps.
    VkrLightmapRect *lightmap_rects;
    uint lightmap_texture;
    uint lightmap_sampler;
    uint lightmap_rect_count;
    uint lightmap_layer_count;
    uint lightmap_active_count;
    uint baked_lamps_texture;
    float lightmap_inverse_page_size;
    // The lamp layers' dominant-direction array, zero without one.
    uint lightmap_direction_texture;
    uint4 lightmap_active_layers[2];
    float4 lightmap_active_weights[2];
    // Dominant direction and directionality for deferred lighting.
    uint baked_direction_texture;
    // SMAA S2x's edge mask, nonzero for the second sample's resolve: only
    // pixels whose two samples name different surfaces resolve, and the
    // temporal outputs stay the first sample's.
    uint edge_mask_texture;
    // SMAA S2x's listed pixels (x | y << 16) for the second sample's
    // resolve, one per thread of the indirect dispatch; null otherwise.
    uint *sample1_pixels;
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
    uint motion_texture;
    uint validity_texture;
    uint depth_texture;
    uint vbuffer_texture;
    uint history_depth_texture;
    uint history_identity_texture;
    uint history_surface_texture;
    uint output_color_texture;
    uint output_depth_texture;
    uint output_identity_texture;
    uint output_surface_texture;
    uint output_accept_texture;
    uint2 extent;
    uint history_valid;
    uint render_mode;
    uint reserved0;
    VkrGpuVisibleDrawRow *transmission_visible_rows;
    VkrVulkanInstance *transmission_instances;
    uint transmission_vbuffer_texture;
    uint transmission_depth_texture;
    uint transmission_enabled;
    uint reserved[3];
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
    uint sky_view_texture;
    uint sky_sampler;
    // VKR_SKY_MODE_*: fallback colour, uniform radiance or the atmosphere.
    uint sky_mode;
    uint gtao_visibility_texture;
    // Uniform background radiance for the constant sky mode.
    float4 sky_radiance;
    uint clearcoat_texture;
    uint sheen_texture;
    uint anisotropy_texture;
    // Nonzero when the layered kernel also runs and owns tiles with layer
    // data. Bit 1 also gives it pixels whose material uses a BRDF extension
    // (ADR-113).
    uint layered_tiles;
    // Shadow.LocalMask output: one RGBA8 layer per per-pixel overlap slot.
    uint local_shadow_mask_texture;
    // Far-cascade EVSM moments array, or VKR_VK_SHADOW_MOMENTS_NONE.
    uint shadow_moments_texture;
    VkrGpuVisibleDrawRow* visible_rows;
    uint subsurface_source_texture;
    uint subsurface_profile_count;
    // Per-light visible contribution counters, or null when not measured.
    uint *light_contribution;
    uint shadow_moments_sampler;
    // The G-buffer resolve's baked lamp irradiance (alpha: the scale from
    // the lightmap surface's irradiance to irradiance facing the dominant
    // direction) and dominant direction (0.5 * d + 0.5, directionality in
    // alpha); zero without baked lamps.
    uint baked_lamps_texture;
    uint baked_direction_texture;
    // DiffuseVolume.Sample's per-pixel volume response; zero without a
    // volume.
    uint diffuse_volume_texture;
    // Shadow.LocalMask's pre-exposed irradiance of the selected baked lamps
    // that their moving casters hide; zero without a selected lamp.
    uint baked_lamp_shadow_texture;
    // SMAA S2x's edge mask, nonzero when lighting the second sample: only
    // its edge pixels shade.
    uint edge_mask_texture;
    // Reflection.Raster's mirrored view (zero without a planar reflection),
    // its plane, the drawn part of its target and its reflector's object id.
    uint planar_reflection_texture;
    uint planar_reflection_sampler;
    float4 planar_reflection_plane;
    float2 planar_reflection_uv_scale;
    uint planar_reflection_object_id;
    // SMAA S2x's first-sample visibility when lighting the second sample,
    // which takes its per-pixel inputs from a pixel whose first sample lies
    // on its draw; zero otherwise.
    uint side_vbuffer_texture;
    // The background's camera-relative view directions
    // (vkr_atmosphere_view_rays): no camera translation, so a direction
    // keeps its precision however far the eye is from the origin.
    float4x4 view_rays;
    // SMAA S2x's listed pixels for the second sample's lighting; null
    // otherwise.
    uint *sample1_pixels;
    // Shadow.SunRayResolve's RG8 mask: the traced sun visibility and its
    // weight over the cascades; zero without ray-query sun shadows.
    uint sun_ray_shadow_texture;
    // The sun cascade filter's and contact march's noise phase: the frame's
    // temporal phase under FSR 3.1 or SMAA T2x, zero otherwise.
    uint sun_noise_index;
    // Nonzero: a contact march toward the sun multiplies its visibility
    // where the rays do not cover the pixel.
    uint sun_contact;
    uint3 sun_reserved;
};

// DiffuseVolume.Sample and DiffuseVolume.Upsample inputs: the G-buffer
// surface, its camera reconstruction, and the baked lamp irradiance whose
// alpha marks a lightmapped pixel (zero without baked lamps).
struct VkrVkDiffuseVolumeSampleRoot
{
    VkrVulkanPacketFrameRoot *frame;
    uint2 frame_address_padding;
    float4x4 inverse_view_projection;
    uint vbuffer_texture;
    uint depth_texture;
    uint normal_texture;
    uint baked_lamps_texture;
    uint2 extent;
    uint2 half_extent;
    // RGBA16F: the volume's response (E/pi) and its sky share, zero where
    // the volume does not cover the pixel, at half resolution from the
    // sample pass and full from the upsample.
    uint output_texture;
    // RGBA16F half resolution: the representative pixel's normal and camera
    // distance, negative where its lightmap holds the baked lamps.
    uint guide_texture;
    // The sample pass's half-resolution response, for the upsample.
    uint half_texture;
    uint reserved;
    // DiffuseVolume.Fallback's indirect arguments (groups x, y, z, then the
    // pixel count) and its pixels, each x | y << 16: the ones no
    // half-resolution texel matched. The sample pass resets the arguments
    // and the upsample appends.
    uint *fallback_arguments;
    uint *fallback_pixels;
    // The camera view's rows, which name a pixel's instance and so its
    // lighting origin (ADR-120).
    VkrGpuVisibleDrawRow *visible_rows;
};

// A baked lamp whose moving casters Shadow.LocalMask shadows (desktop baked
// lamps proposal): its pre-exposed light row and the first views of its
// static and composite blocks.
struct VkrVkBakedShadowLamp
{
    VkrGpuPointLightRow light;
    uint static_first_view;
    uint composite_first_view;
    uint2 reserved;
};

// Shadow.LocalMask inputs: the G-buffer surface, its camera reconstruction,
// and the noise index that varies the contact-shadow march between frames.
struct VkrVkLocalShadowMaskRoot
{
    VkrVulkanPacketFrameRoot *frame;
    uint2 frame_address_padding;
    float4x4 inverse_view_projection;
    VkrGpuVisibleDrawRow* visible_rows;
    uint vbuffer_texture;
    uint depth_texture;
    uint normal_texture;
    // One RGBA8 layer of local-shadow visibility per shadowed light.
    uint mask_texture;
    uint2 extent;
    // Zero without temporal reconstruction, so the march and tap patterns stay
    // fixed.
    uint contact_noise_index;
    // Nonzero under temporal reconstruction: fully filtered lights take the
    // rotated temporal taps.
    uint temporal_filter;
    // RGBA16F: the pre-exposed irradiance the selected baked lamps' moving
    // casters hide, for `baked_lamp_count` lamps.
    uint baked_lamp_shadow_texture;
    uint baked_lamp_count;
    VkrVkBakedShadowLamp *baked_lamps;
    // The G-buffer resolve's baked lamp irradiance: its alpha marks the
    // lightmapped pixels, the only ones deferred lighting subtracts from.
    uint baked_lamps_texture;
    uint baked_reserved;
    // Bit i: light i is shadowed and its range reaches the camera frustum.
    // No other light is in range of a visible pixel with a shadow view.
    uint4 mask_lights;
};

// One far cascade's EVSM moments layer, built from its depth layer.
struct VkrVkShadowMomentsRoot
{
    uint depth_texture;
    uint moments_texture;
    uint depth_layer;
    uint moments_layer;
    uint depth_size;
    uint moments_size;
    uint2 reserved;
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

// The opaque depth at the picked pixel, read back with the object id. A
// struct, not a `float *`: with a `float *` (or `uint *`) here, the AMD
// driver (RX 6700 XT, 26.6.3) gave the shader the address's low word in both
// halves and the store lost the device on every pick.
struct VkrVkPickingDepth
{
    float depth;
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
    uint depth_texture;
    // The editor lifts its grid onto the visible floor from this depth.
    VkrVkPickingDepth *depth_output;
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
    // Baked lamps (ADR-104): the G-buffer resolve's lightmap binding, so a
    // lightmapped transmissive draw shades them too; a zero rectangle count
    // without.
    VkrLightmapRect *lightmap_rects;
    uint lightmap_texture;
    uint lightmap_sampler;
    uint lightmap_rect_count;
    uint lightmap_layer_count;
    uint lightmap_active_count;
    float lightmap_inverse_page_size;
    uint lightmap_direction_texture;
    uint3 lightmap_reserved;
    uint4 lightmap_active_layers[2];
    float4 lightmap_active_weights[2];
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
struct VkrVkDiffuseVolumeSamplePush
{
    VkrVkDiffuseVolumeSampleRoot *root;
    uint2 reserved;
};

struct VkrVkLocalShadowMaskPush
{
    VkrVkLocalShadowMaskRoot *root;
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
struct VkrVkShadowMomentsPush
{
    VkrVkShadowMomentsRoot *root;
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
[[vk::push_constant]] ConstantBuffer<VkrVkLocalShadowMaskPush>
    g_local_shadow_mask_push;
[[vk::push_constant]] ConstantBuffer<VkrVkDiffuseVolumeSamplePush>
    g_diffuse_volume_sample_push;
[[vk::push_constant]] ConstantBuffer<VkrVkTemporalResolvePush>
    g_temporal_resolve_push;
[[vk::push_constant]] ConstantBuffer<VkrVkHzbPush> g_hzb_push;
[[vk::push_constant]] ConstantBuffer<VkrVkShadowMomentsPush>
    g_shadow_moments_push;
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
    // Reversed camera depth (ADR-109): near is 1, far 0, so the nearest corner
    // has the largest depth and the HZB keeps each cell's smallest.
    float2 ndc_min = 1e30f;
    float2 ndc_max = -1e30f;
    float nearest_depth = 0.0f;
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
        nearest_depth = max(nearest_depth, ndc.z);
    }
    if (nearest_depth >= 1.0f || any(ndc_min > 1.0f) || any(ndc_max < -1.0f))
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
            if (nearest_depth >= occluder_depth - root.hzb_depth_epsilon)
                return false;
        }
    }
    return true;
}

// A reflector pixel lies on the mirror plane within this many metres and
// faces it at least this closely; its sides and other draws keep probes.
#define VKR_PLANAR_REFLECTION_PLANE_TOLERANCE 0.05f
#define VKR_PLANAR_REFLECTION_FACING_MIN 0.9f
// Screen-space shift of the reflection per unit of the shading normal's tilt
// off the plane, in view-space units.
#define VKR_PLANAR_REFLECTION_DISTORTION 0.05f

// The environment specular of a planar reflector's pixel: the mirrored
// view's radiance where it covers the pixel, the probes' elsewhere. The
// mirrored view shares the camera's projection, so a point's reflection
// lies at its own screen position; the shading normal's tilt shifts it.
float3 vk_planar_reflection_specular(VkrVkLightingRoot root,
                                     VkrVulkanPacketFrameRoot *frame,
                                     uint encoded, uint2 pixel, float3 world,
                                     float3 normal, float3 horizon_normal,
                                     float3 view, float roughness, float ao,
                                     VkrGgxMaterialEnergy energy,
                                     float3 environment_specular,
                                     float gtao_cone)
{
    VkrGpuVisibleDrawRow visible = root.visible_rows[encoded - 1u];
    if (frame.instances[visible.instance_index].object_id !=
        root.planar_reflection_object_id)
        return environment_specular;
    float4 plane = root.planar_reflection_plane;
    if (abs(dot(plane.xyz, world) + plane.w) >
            VKR_PLANAR_REFLECTION_PLANE_TOLERANCE ||
        dot(horizon_normal, plane.xyz) < VKR_PLANAR_REFLECTION_FACING_MIN)
        return environment_specular;
    float3 tilt = normal - plane.xyz * dot(normal, plane.xyz);
    float2 offset = mul((float3x3)frame.view, tilt).xy * float2(1.0f, -1.0f);
    float2 uv = (float2(pixel) + 0.5f) / float2(root.extent) +
                offset * VKR_PLANAR_REFLECTION_DISTORTION;
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return environment_specular;
    float4 reflected =
        g_textures[root.planar_reflection_texture].SampleLevel(
            g_samplers[root.planar_reflection_sampler],
            uv * root.planar_reflection_uv_scale, 0.0f);
    float3 planar = reflected.rgb *
                    packet_environment_specular_weight(
                        normal, horizon_normal, view, roughness, ao, energy) *
                    gtao_cone;
    return lerp(environment_specular, planar, reflected.a);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_gpu_draw_classify(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkCullRoot *root = vkr_vk_cull_root();
    uint candidate_index = thread_id.x;
    uint view_index = thread_id.y;
    if (candidate_index >= root.candidate_count ||
        view_index >= root.view_count)
        return;
    VkrGpuCandidateDrawRow candidate = root.candidates[candidate_index];
    // A planar reflection's mirrored view draws what the camera draws.
    bool reflection = view_index == root.reflection_view;
    bool camera_rules = view_index == 0u || reflection;
    uint required_flags = camera_rules ? root.camera_required_flags
                                       : root.shadow_required_flags;
    uint candidate_flags = vkr_gpu_draw_candidate_flags(candidate.state_flags);
    uint excluded_flags = 0u;
    // The reflection view follows the transmission views but keeps the
    // camera's flags; the cascades take their own exclusions.
    if (!reflection && view_index >= root.transmission_first_view)
    {
        required_flags = root.transmission_required_flags;
    }
    else if (!reflection && view_index >= root.local_shadow_first_view)
    {
        excluded_flags = root.local_shadow_excluded_flags;
    }
    else if (!camera_rules)
    {
        excluded_flags = root.cascade_excluded_flags;
    }
    // A static square or cascade layer holds the static casters; a dynamic
    // square or composed cascade adds the dynamic ones over a copy of it.
    // The camera and transmission views keep every mobility.
    uint mobility = root.lod_views[view_index].flags;
    bool static_candidate = candidate_index < root.static_candidate_count;
    bool mobility_excluded =
        ((mobility & VKR_GPU_LOD_VIEW_STATIC_CASTERS_ONLY) != 0u &&
         !static_candidate) ||
        ((mobility & VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY) != 0u &&
         static_candidate);
    uint bucket = vkr_gpu_draw_state_bucket(candidate.state_flags);
    if ((candidate_flags & required_flags) != required_flags ||
        (candidate_flags & excluded_flags) != 0u || mobility_excluded)
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
    // The mirrored view leaves out the reflector, which would reflect
    // itself, and whatever lies wholly below its plane.
    if (reflection &&
        (instance.object_id == root.reflection_object_id ||
         ((candidate_flags & 1u) != 0u &&
          dot(root.reflection_plane.xyz, world_center.xyz) +
                  root.reflection_plane.w < -world_radius)))
    {
        root.classifications[classification_index] = 0xffffffffu;
        return;
    }
    float4 world_sphere = float4(world_center.xyz, world_radius);
    if ((candidate_flags & 1u) != 0u &&
        !vkr_vk_sphere_visible(root, world_sphere, view_index))
    {
        root.classifications[classification_index] = 0xffffffffu;
        return;
    }
    // The camera skips candidates in hidden rooms, past their draw distance
    // or below their smallest drawn size; a cascade skips sealed casters and
    // those whose shadow distance ends before its slice. Other views have no
    // slice start and keep them.
    VkrGpuLodView lod_view = root.lod_views[view_index];
    uint cull = vkr_gpu_draw_cull_word(candidate.state_flags);
    if (cull != 0u && (candidate_flags & 1u) != 0u &&
        (camera_rules
             ? vkr_gpu_draw_distance_culled(
                   cull, lod_view.position_scale.w, lod_view.flags,
                   lod_view.distance_scale,
                   vkr_lod_distance(lod_view.position_scale.xyz,
                                    world_center.xyz, world_radius),
                   world_radius)
             : vkr_gpu_draw_shadow_culled(cull, lod_view.shadow_near)))
    {
        root.classifications[classification_index] = 0xffffffffu;
        return;
    }
    // Sub-texel casters, and copies below their larger limit, skip the
    // directional cascades.
    if (!camera_rules && (candidate_flags & 1u) != 0u &&
        vkr_gpu_cascade_caster_too_small(cull, lod_view.position_scale.w,
                                         lod_view.flags, world_radius))
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
    // The LOD state rides above the bucket (ADR-084); a candidate without
    // valid bounds has no distance and keeps level 0.
    uint lod_state = (candidate_flags & 1u) == 0u
        ? 0u
        : vkr_gpu_lod_select(
              vkr_gpu_geometry_lod_row(
                  root.geometry_rows[candidate.geometry_index].decode,
                  candidate.decode_index),
              lod_view, world_center.xyz, world_radius,
              instance.normal_column1.w);
    root.classifications[classification_index] = bucket | lod_state;
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
    // The partition holds every packed candidate of its bucket, so overflow
    // stays zero unless a count and its partition disagree.
    [unroll] for (uint bucket = 0u; bucket < VKR_VK_DRAW_BUCKET_COUNT; ++bucket)
    {
        uint count = root.states[view_index].bucket_counts[bucket];
        uint written_count =
            min(count, root.command_partitions.capacity[bucket]);
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
    if (candidate_index >= root.candidate_count ||
        view_index >= root.view_count ||
        (view_index < 32u &&
         (root.encode_idle_view_mask & (1u << view_index)) != 0u))
        return;
    uint classification_index =
        view_index * root.candidate_capacity + candidate_index;
    uint classification = root.classifications[classification_index];
    uint bucket = classification & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK;
    uint lod_state = classification & ~VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK;
    if (bucket >= VKR_VK_DRAW_BUCKET_COUNT)
        return;
    uint local_index;
    InterlockedAdd(root.states[view_index].bucket_cursors[bucket], 1u,
                   local_index);
    if (local_index >= root.command_partitions.capacity[bucket])
        return;
    uint visible_index =
        root.states[view_index].execution_ranges[bucket].x + local_index;
    if (visible_index >= root.candidate_capacity)
        return;
    VkrGpuCandidateDrawRow candidate = root.candidates[candidate_index];
    // The selected level's indices replace the range's (ADR-084).
    uint first_index = candidate.first_index;
    uint index_count = candidate.index_count;
    uint level = vkr_gpu_draw_lod_level(lod_state);
    if (level != 0u)
    {
        VkrGpuGeometryLodRow *row = vkr_gpu_geometry_lod_row(
            root.geometry_rows[candidate.geometry_index].decode,
            candidate.decode_index);
        first_index += row.levels[level].first_index;
        index_count = row.levels[level].index_count;
    }
    VkrGpuVisibleDrawRow visible;
    visible.geometry_index = candidate.geometry_index;
    visible.material_index = candidate.material_index;
    visible.instance_index = candidate.instance_index;
    visible.first_index = first_index;
    visible.index_count = index_count;
    visible.vertex_offset = candidate.vertex_offset;
    visible.decode_index = candidate.decode_index;
    visible.state_flags =
        (candidate.state_flags & VKR_GPU_DRAW_CANDIDATE_STATE_MASK) | lod_state;
    root.visible[view_index * root.candidate_capacity + visible_index] =
        visible;
    uint argument_index = view_index * root.candidate_capacity +
                          root.command_partitions.first[bucket] + local_index;
    VkrVkIndirectCommand command;
    command.index_count = index_count;
    command.instance_count = 1u;
    command.first_index = first_index;
    command.vertex_offset = candidate.vertex_offset;
    command.first_instance = 0u;
    root.commands[argument_index] = command;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_temporal_transform(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkTemporalTransformRoot root = vkr_vk_uniform_root(*g_temporal_transform_push.root);
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
    vertex = vkr_apply_deformation(vertex, instance.deformation_address, vertex_id);
    // Terrain tiles morph toward their next level (ADR-084).
    float previous_height;
    vertex = vkr_gpu_terrain_morph(vertex, geometry.vertices + geometry.first_vertex,
                                   geometry.decode[visible.decode_index],
                                   vertex_id, visible.state_flags,
                                   previous_height);
    VkrVkVisibilityOutput output;
    output.position = mul(root.frame.view_projection,
                          mul(instance.model, float4(vertex.position, 1.0f)));
    output.uv = vertex.texcoord;
    output.color = vertex.color;
    output.visible_index = visible_instance;
    return output;
}

[shader("vertex")]
VkrVkReflectionOutput vk_reflection_vertex(uint vertex_id : SV_VertexID,
                                           uint draw_index : SV_DrawIndex)
{
    VkrVkRasterRoot *root = g_raster_push.root;
    uint visible_instance =
        root.states[root.view_index].execution_ranges[g_raster_push.bucket].x +
        draw_index;
    VkrGpuVisibleDrawRow visible =
        root.visible_rows[root.view_index * root.visible_capacity +
                          visible_instance];
    VkrVkGeometryRow geometry = root.geometry_rows[visible.geometry_index];
    VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
        geometry.vertices[geometry.first_vertex + vertex_id],
        geometry.decode[visible.decode_index]);
    VkrVulkanInstance instance = root.frame.instances[visible.instance_index];
    vertex = vkr_apply_deformation(vertex, instance.deformation_address, vertex_id);
    float previous_height;
    vertex = vkr_gpu_terrain_morph(vertex, geometry.vertices + geometry.first_vertex,
                                   geometry.decode[visible.decode_index],
                                   vertex_id, visible.state_flags,
                                   previous_height);
    float4 world = mul(instance.model, float4(vertex.position, 1.0f));
    VkrVkReflectionOutput output;
    output.position = mul(root.frame.view_projection, world);
    output.uv = vertex.texcoord;
    output.color = vertex.color;
    output.world_position = world.xyz;
    output.world_normal = normalize(vkr_instance_transform_normal(
        instance.normal_column0.xyz, instance.normal_column1.xyz,
        instance.normal_column2.xyz, vertex.normal));
    output.world_tangent = float4(
        normalize(mul(instance.model, float4(vertex.tangent.xyz, 0.0f)).xyz),
        vertex.tangent.w * instance.normal_column0.w);
    output.visible_index = visible_instance;
    return output;
}

// Alpha one marks the pixels the mirrored view covers; lighting takes the
// probes and environment elsewhere.
[shader("fragment")]
float4 vk_reflection_fragment(VkrVkReflectionOutput input,
                              bool front_facing : SV_IsFrontFace)
    : SV_Target0
{
    VkrVkRasterRoot *root = g_raster_push.root;
    VkrGpuVisibleDrawRow visible =
        root.visible_rows[root.view_index * root.visible_capacity +
                          input.visible_index];
    VkrVulkanVertexOutput surface;
    surface.position = input.position;
    surface.uv = input.uv;
    surface.color = input.color;
    surface.world_position = input.world_position;
    surface.world_normal = input.world_normal;
    surface.object_id = 0u;
    surface.world_tangent = input.world_tangent;
    surface.instance_index = visible.instance_index;
    float4 color = packet_world_shade_surface(root.frame, visible, surface,
                                              front_facing);
    return float4(color.rgb, 1.0f);
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
        // Keep only fragments behind the previous layer: smaller reversed
        // depth (ADR-109).
        if (input.position.z >= previous_depth - 1e-5f)
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

// SMAA S2x: the camera's visibility raster samples each pixel twice. Sample
// zero becomes the single-sample visibility image every consumer reads; the
// second sample's identity and depth go to their own images, where the
// second sample is shaded. Mirrors VkrVulkanVbufferSplitRoot.
struct VkrVkVbufferSplitRoot
{
    uint ms_vbuffer_texture;
    uint ms_depth_texture;
    uint vbuffer_storage;
    uint vbuffer_s1_storage;
    uint depth_s1_storage;
    // One where the second sample is shaded, else zero.
    uint edge_mask_storage;
    uint2 extent;
    // The listed pixels' indirect dispatch (groups of 64, 1, 1, then the
    // count) and the pixels, x | y << 16.
    uint *sample1_arguments;
    uint *sample1_pixels;
};

struct VkrVkVbufferSplitPush
{
    VkrVkVbufferSplitRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkVbufferSplitPush> g_vbuffer_split_push;

// Whether a pixel's two depth samples can lie on one continuous surface, for
// VBuffer.SplitDepth's pixel-centre depth. Two triangles of a surface (a
// tessellation edge) differ by well under the tolerance across a pixel up to
// about 86 degrees of incidence at a 70 degree field of view and 720 rows; a
// silhouette over a depth gap exceeds it.
bool vkr_vk_msaa_depth_continuous(float sample0, float sample1)
{
    const float continuity_tolerance = 0.02f;
    return abs(sample0 - sample1) <=
           continuity_tolerance * max(sample0, sample1);
}

// Empties the list of pixels whose second sample is shaded, before the split
// appends to it.
[shader("compute")]
[numthreads(1, 1, 1)]
void vk_msaa_sample1_list_reset()
{
    VkrVkVbufferSplitRoot root =
        vkr_vk_uniform_root(*g_vbuffer_split_push.root);
    root.sample1_arguments[0] = 0u;
    root.sample1_arguments[1] = 1u;
    root.sample1_arguments[2] = 1u;
    root.sample1_arguments[3] = 0u;
}

// The depth step, per pixel along `step`, of the surface that pixel's first
// sample hit, from a neighbouring first sample on the same draw at a
// continuous depth: on dense meshes the neighbour is rarely on the same
// triangle. False without one on either side.
bool vk_msaa_depth_step(Texture2DMS<uint2> vbuffer, Texture2DMS<float> depth,
                        int2 pixel, int2 step, int2 extent, uint draw,
                        float depth0, out float gradient)
{
    gradient = 0.0f;
    int2 before = pixel - step;
    if (all(before >= 0) && vbuffer.Load(before, 0).x == draw)
    {
        float neighbour = depth.Load(before, 0);
        if (vkr_vk_msaa_depth_continuous(depth0, neighbour))
        {
            gradient = depth0 - neighbour;
            return true;
        }
    }
    int2 after = pixel + step;
    if (all(after < extent) && vbuffer.Load(after, 0).x == draw)
    {
        float neighbour = depth.Load(after, 0);
        if (vkr_vk_msaa_depth_continuous(depth0, neighbour))
        {
            gradient = neighbour - depth0;
            return true;
        }
    }
    return false;
}

// Whether the second sample, on another triangle of the first sample's draw,
// continues the first sample's surface: its depth lies on the first
// sample's surface extrapolated half a pixel up and left, within 0.1 %. A
// tessellation edge stays far inside it; a railing 5 cm before a wall 10 m
// away departs by 0.5 %.
bool vk_msaa_sample1_continues(Texture2DMS<uint2> vbuffer,
                               Texture2DMS<float> depth, int2 pixel,
                               int2 extent, uint2 sample0, uint2 sample1,
                               float depth1)
{
    const float plane_tolerance = 0.001f;
    if (sample0.x != sample1.x)
        return false;
    float depth0 = depth.Load(pixel, 0);
    float gradient_x;
    float gradient_y;
    if (!vk_msaa_depth_step(vbuffer, depth, pixel, int2(1, 0), extent,
                            sample0.x, depth0, gradient_x) ||
        !vk_msaa_depth_step(vbuffer, depth, pixel, int2(0, 1), extent,
                            sample0.x, depth0, gradient_y))
        return false;
    float predicted = depth0 - 0.5f * (gradient_x + gradient_y);
    return abs(depth1 - predicted) <= plane_tolerance * max(depth0, depth1);
}

// The second sample is shaded where it names another triangle than the first
// and does not continue the first's surface: silhouettes, thin geometry and
// draw borders, including railings a few centimetres before a wall in the
// wall's draw. Shading every triangle change cost 2.6 ms at 2560 x 1440 on
// the Bistro street view, and depth continuity alone (2 %) missed those
// railings, so both SMAA runs filtered the first sample's image with
// different area rows. Each listed pixel joins the list once, so the list,
// one entry per viewport pixel, cannot overflow; one atomic per wave appends
// its lanes.
[shader("compute")]
[numthreads(8, 8, 1)]
void vk_vbuffer_split(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkVbufferSplitRoot root =
        vkr_vk_uniform_root(*g_vbuffer_split_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    int2 pixel = int2(thread_id.xy);
    Texture2DMS<uint2> vbuffer = g_ms_uint2_textures[root.ms_vbuffer_texture];
    uint2 sample0 = vbuffer.Load(pixel, 0);
    uint2 sample1 = vbuffer.Load(pixel, 1);
    g_storage_uint2_2d[root.vbuffer_storage][thread_id.xy] = sample0;
    g_storage_uint2_2d[root.vbuffer_s1_storage][thread_id.xy] = sample1;
    Texture2DMS<float> depth = g_ms_depth_textures[root.ms_depth_texture];
    float depth1 = depth.Load(pixel, 1);
    bool shade_sample1 =
        any(sample0 != sample1) &&
        !vk_msaa_sample1_continues(vbuffer, depth, pixel, int2(root.extent),
                                   sample0, sample1, depth1);
    g_storage_2d[root.edge_mask_storage][thread_id.xy] =
        float4(shade_sample1 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    g_storage_2d[root.depth_s1_storage][thread_id.xy] =
        float4(depth1, 0.0f, 0.0f, 0.0f);
    uint wave_count = WaveActiveCountBits(shade_sample1);
    if (wave_count == 0u)
        return;
    uint base = 0u;
    if (WaveIsFirstLane())
    {
        InterlockedAdd(root.sample1_arguments[3], wave_count, base);
        uint groups;
        InterlockedMax(root.sample1_arguments[0],
                       (base + wave_count + 63u) / 64u, groups);
    }
    base = WaveReadLaneFirst(base);
    if (shade_sample1)
        root.sample1_pixels[base + WavePrefixCountBits(shade_sample1)] =
            thread_id.x | (thread_id.y << 16u);
}

// SMAA S2x: the pixel a listed thread shades. The list's indirect dispatch
// runs 64-thread groups along x; the last group's slots past the count hold
// stale pixels, which the edge mask rejects or which shade their own correct
// result again.
uint3 vk_msaa_sample1_pixel(uint *pixels, uint3 group_id, uint group_index)
{
    uint packed = pixels[group_id.x * 64u + group_index];
    return uint3(packed & 0xffffu, packed >> 16u, 0u);
}

// SMAA S2x: the second sample's colour for display: the first sample's
// post-processed colour plus, where VBuffer.Split marked the second sample,
// the difference between their lit colours. Bloom is additive, so the sum
// equals the second sample bloomed by the first sample's halo. A pixel a
// blended overlay or transmission layer covers keeps the first sample's,
// which alone carries that layer. Mirrors VkrVulkanMsaaComposeRoot.
struct VkrVkMsaaComposeRoot
{
    uint sample0_texture;
    uint sample1_texture;
    uint edge_mask_texture;
    uint vbuffer_texture;
    uint transmission_vbuffer_texture;
    uint destination_texture;
    uint2 extent;
    // The first sample's bloomed colour, zero without bloom.
    uint post_texture;
    uint3 reserved;
};

struct VkrVkMsaaComposePush
{
    VkrVkMsaaComposeRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkMsaaComposePush> g_msaa_compose_push;

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_msaa_compose(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkMsaaComposeRoot root = vkr_vk_uniform_root(*g_msaa_compose_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    uint2 pixel = thread_id.xy;
    bool own = g_storage_2d[root.edge_mask_texture][pixel].x != 0.0f &&
               (g_storage_uint2_2d[root.vbuffer_texture][pixel].x &
                0x80000000u) == 0u;
    if (own && root.transmission_vbuffer_texture != 0u)
        own = g_storage_uint2_images[root.transmission_vbuffer_texture]
                                    [uint3(pixel, 0u)].x == 0u;
    float4 sample0 = g_storage_2d[root.sample0_texture][pixel];
    float4 post =
        root.post_texture != 0u ? g_storage_2d[root.post_texture][pixel]
                                : sample0;
    if (own)
        post.rgb += g_storage_2d[root.sample1_texture][pixel].rgb - sample0.rgb;
    g_storage_2d[root.destination_texture][pixel] = post;
}

// The single-sample depth image that later passes depth-test against, copy,
// or reconstruct positions from at pixel centres. The standard 2x positions
// lie symmetric about the centre, and device depth is affine across a
// triangle, so where both samples hit one triangle their mean is the centre's
// depth, and across a continuous surface's triangle edge nearly so;
// elsewhere sample zero's stays. Mirrors
// VkrVulkanVbufferSplitDepthRoot.
struct VkrVkVbufferSplitDepthRoot
{
    uint ms_depth_texture;
    uint ms_vbuffer_texture;
    uint2 reserved;
};

struct VkrVkVbufferSplitDepthPush
{
    VkrVkVbufferSplitDepthRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkVbufferSplitDepthPush>
    g_vbuffer_split_depth_push;

[shader("vertex")]
float4 vk_vbuffer_split_depth_vertex(uint vertex_id : SV_VertexID)
    : SV_Position
{
    float2 uv = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
    return float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
}

[shader("fragment")]
float vk_vbuffer_split_depth_fragment(float4 position : SV_Position)
    : SV_Depth
{
    VkrVkVbufferSplitDepthRoot *root = g_vbuffer_split_depth_push.root;
    int2 pixel = int2(position.xy);
    Texture2DMS<float> depth = g_ms_depth_textures[root.ms_depth_texture];
    Texture2DMS<uint2> vbuffer = g_ms_uint2_textures[root.ms_vbuffer_texture];
    float sample0 = depth.Load(pixel, 0);
    float sample1 = depth.Load(pixel, 1);
    bool same_triangle =
        all(vbuffer.Load(pixel, 0) == vbuffer.Load(pixel, 1));
    if (!same_triangle && !vkr_vk_msaa_depth_continuous(sample0, sample1))
        return sample0;
    return 0.5f * (sample0 + sample1);
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
void vkr_vk_resolve_defaults(VkrVkResolveRoot root, uint2 pixel, float marker)
{
    g_storage_2d[root.albedo_texture][pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_storage_2d[root.specular_texture][pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_storage_float2_2d[root.normal_texture][pixel] = 0.0f;
    bool first_sample = root.edge_mask_texture == 0u;
    if (root.clearcoat_texture != 0u)
    {
        g_storage_2d[root.clearcoat_texture][pixel] = 0.0f;
    }
    if (root.sheen_texture != 0u)
    {
        g_storage_2d[root.sheen_texture][pixel] = 0.0f;
    }
    if (root.anisotropy_texture != 0u)
    {
        g_storage_2d[root.anisotropy_texture][pixel] = 0.0f;
    }
#if VKR_VULKAN_GBUFFER_WRITE_EMISSIVE
    g_storage_2d[root.emissive_texture][pixel] = 0.0f;
#endif
#if VKR_VULKAN_GBUFFER_WRITE_DEBUG
    g_storage_2d[root.debug_texture][pixel] = float4(0.0f, 0.0f, 0.0f, marker);
#endif
    g_storage_2d[root.scene_texture][pixel] = 0.0f;
    if (first_sample)
    {
        g_storage_float2_2d[root.motion_texture][pixel] = 0.0f;
        g_storage_float2_2d[root.validity_texture][pixel] = 0.0f;
    }
    if (root.baked_lamps_texture != 0u)
    {
        g_storage_2d[root.baked_lamps_texture][pixel] = 0.0f;
        g_storage_2d[root.baked_direction_texture][pixel] = 0.0f;
    }
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
    uint render_mode;
    // Scale for material emissive, which is lowered physical.
    float pre_exposure;
    // Null where no terrain material reaches, as in transmission.
    VkrVulkanTerrainMaterial *terrain_materials;
    // Nonzero when the frame has decals: the surface then carries its world
    // position and that position's screen derivatives (ADR-092).
    uint project_decals;
    // Nonzero when the frame samples baked lamps: a draw whose geometry
    // carries lightmap UVs then reports its interpolated lightmap UV.
    uint sample_lightmap;
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
    float3 previous_object_position;
    float3 object_normal;
    float4 object_tangent;
    float3 tangent_normal;
    float3 emissive;
    float3 barycentric;
    float3 wire_color;
    float face_sign;
    float metallic;
    float roughness;
    float feedback_roughness;
    float occlusion;
    float selected_lod;
    float2 uv;
    float2 uv_dx;
    float2 uv_dy;
    // Set only when the source projects decals.
    float3 world_position;
    float3 world_dx;
    float3 world_dy;
    uint material_index;
    uint instance_index;
    uint normal_mapped;
    // The draw's lightmap UV over its source mesh's chart atlas, set only
    // when the source samples lightmaps and the geometry carries them.
    float2 lightmap_uv;
    uint has_lightmap_uv;
};

// One extra terrain layer's surface from its row; its slots always hold a map
// or the white and flat defaults.
VkrTerrainSurface vkr_vk_terrain_layer(uint base_color_texture,
                                       uint normal_texture, uint orm_texture,
                                       uint sampler_index, float4 tint,
                                       float4 surface, float2 uv,
                                       float2 uv_dx, float2 uv_dy,
                                       bool normal_mapped)
{
    float3 tangent_normal = float3(0.0f, 0.0f, 1.0f);
    if (normal_mapped)
    {
        tangent_normal = vkr_normal_map_decode(
            g_textures[NonUniformResourceIndex(normal_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(sampler_index)],
                            uv, uv_dx, uv_dy)
                .xyz,
            surface.z);
    }
    float4 base_sample =
        g_textures[NonUniformResourceIndex(base_color_texture)].SampleGrad(
            g_samplers[NonUniformResourceIndex(sampler_index)], uv, uv_dx,
            uv_dy);
    float3 orm =
        g_textures[NonUniformResourceIndex(orm_texture)]
            .SampleGrad(g_samplers[NonUniformResourceIndex(sampler_index)], uv,
                        uv_dx, uv_dy)
            .rgb;
    return vkr_terrain_layer(base_sample, orm, tangent_normal, tint, surface);
}

// The blended surface of a terrain material: `layer0` from the common row,
// then each extra layer the weights reach.
VkrTerrainSurface vkr_vk_terrain_surface(VkrTerrainSurface layer0,
                                         VkrVulkanTerrainMaterial *terrain,
                                         float4 weights, float2 uv,
                                         float2 uv_dx, float2 uv_dy,
                                         bool normal_mapped)
{
    VkrTerrainSurface sum = layer0;
    sum.base *= weights.x;
    sum.tangent_normal *= weights.x;
    sum.metallic *= weights.x;
    sum.roughness *= weights.x;
    sum.occlusion *= weights.x;
    if (weights.y > 0.0f)
    {
        sum = vkr_terrain_accumulate(
            sum,
            vkr_vk_terrain_layer(
                terrain.layer1_base_color_texture, terrain.layer1_normal_texture,
                terrain.layer1_orm_texture, terrain.layer1_sampler,
                terrain.layer1_tint, terrain.layer1_surface, uv, uv_dx, uv_dy,
                normal_mapped),
            weights.y);
    }
    if (weights.z > 0.0f)
    {
        sum = vkr_terrain_accumulate(
            sum,
            vkr_vk_terrain_layer(
                terrain.layer2_base_color_texture, terrain.layer2_normal_texture,
                terrain.layer2_orm_texture, terrain.layer2_sampler,
                terrain.layer2_tint, terrain.layer2_surface, uv, uv_dx, uv_dy,
                normal_mapped),
            weights.z);
    }
    if (weights.w > 0.0f)
    {
        sum = vkr_terrain_accumulate(
            sum,
            vkr_vk_terrain_layer(
                terrain.layer3_base_color_texture, terrain.layer3_normal_texture,
                terrain.layer3_orm_texture, terrain.layer3_sampler,
                terrain.layer3_tint, terrain.layer3_surface, uv, uv_dx, uv_dy,
                normal_mapped),
            weights.w);
    }
    return vkr_terrain_finish(sum);
}

// The material samplers' -0.5 LOD bias sharpens colour for SMAA T2x's
// jitter. Normal and roughness maps carry each mip's normal variance in
// roughness (vkr_vkt_pack_normal_roughness), which only the mip matching the
// pixel footprint holds, so their fetches scale the gradients by sqrt(2):
// +0.5 LOD cancels the bias.
#define VKR_VK_SPECULAR_FOOTPRINT_SCALE 1.41421356f

// Returns false for a row this pass must not shade, counting it as an invalid
// resolve first. The caller owns the fallback, because the G-buffer writes
// defaults while a transmission layer keeps its background.
// `position` is the raster position in pixels the surface is resolved at:
// the pixel centre, or SMAA S2x's second sample.
bool vkr_vk_resolve_surface(VkrVkSurfaceSource source, uint2 encoded,
                            float2 position, out VkrVkSurface surface)
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
    bool neutral_lighting = vkr_editor_neutral_lighting(source.render_mode);
    bool skip_normal_map = vkr_editor_skip_normal_map(source.render_mode);
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
    a = vkr_apply_deformation(a, instance.deformation_address, uint(ia));
    b = vkr_apply_deformation(b, instance.deformation_address, uint(ib));
    c = vkr_apply_deformation(c, instance.deformation_address, uint(ic));
    // Terrain tiles morph as the raster did (ADR-084).
    float previous_height_a;
    float previous_height_b;
    float previous_height_c;
    VkrPackedStaticVertex *tile_vertices =
        source.vertices + geometry.first_vertex;
    a = vkr_gpu_terrain_morph(a, tile_vertices, decode, uint(ia),
                              visible.state_flags, previous_height_a);
    b = vkr_gpu_terrain_morph(b, tile_vertices, decode, uint(ib),
                              visible.state_flags, previous_height_b);
    c = vkr_gpu_terrain_morph(c, tile_vertices, decode, uint(ic),
                              visible.state_flags, previous_height_c);
    float4 wa = mul(instance.model, float4(a.position, 1.0f));
    float4 wb = mul(instance.model, float4(b.position, 1.0f));
    float4 wc = mul(instance.model, float4(c.position, 1.0f));
    float4 ca = mul(source.view_projection, wa);
    float4 cb = mul(source.view_projection, wb);
    float4 cc = mul(source.view_projection, wc);
    float2 p = position;
    float3 perspective;
    float3 perspective_dx;
    float3 perspective_dy;
    if (!vkr_vk_perspective_barycentric(vkr_vk_resolve_ndc(p, source.extent),
                                        ca, cb, cc, source.extent, perspective,
                                        perspective_dx, perspective_dy))
        return vkr_vk_reject_surface(source);
    if (source.render_mode == 12u)
    {
        surface.wire_color = vkr_editor_wire_color(
            perspective, perspective_dx, perspective_dy);
        return true;
    }
    if (source.project_decals != 0u)
    {
        surface.world_position = wa.xyz * perspective.x +
                                 wb.xyz * perspective.y +
                                 wc.xyz * perspective.z;
        surface.world_dx = wa.xyz * perspective_dx.x +
                           wb.xyz * perspective_dx.y +
                           wc.xyz * perspective_dx.z;
        surface.world_dy = wa.xyz * perspective_dy.x +
                           wb.xyz * perspective_dy.y +
                           wc.xyz * perspective_dy.z;
    }
    if (source.sample_lightmap != 0u &&
        (decode.flags & VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV) != 0u)
    {
        surface.lightmap_uv =
            vkr_decode_packed_lightmap_uv(
                source.vertices[geometry.first_vertex + uint(ia)]) *
                perspective.x +
            vkr_decode_packed_lightmap_uv(
                source.vertices[geometry.first_vertex + uint(ib)]) *
                perspective.y +
            vkr_decode_packed_lightmap_uv(
                source.vertices[geometry.first_vertex + uint(ic)]) *
                perspective.z;
        surface.has_lightmap_uv = 1u;
    }
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
    // A terrain material reads the vertex color as layer weights, not as a
    // tint (ADR-084).
    bool terrain = (material.flags & 2048u) != 0u;
    float4 base =
        g_textures[NonUniformResourceIndex(material.base_color_texture)]
            .SampleGrad(g_samplers[NonUniformResourceIndex(
                            material.base_color_sampler)],
                        uv, uv_dx, uv_dy) *
        material.tint * (terrain ? float4(1.0f) : color);
    VkrTerrainSurface layered = (VkrTerrainSurface)0;
    if (terrain)
    {
        float3 orm0 = float3(1.0f);
        if ((material.flags & 2u) != 0u)
        {
            orm0 = g_textures[NonUniformResourceIndex(material.orm_texture)]
                       .SampleGrad(g_samplers[NonUniformResourceIndex(
                                       material.orm_sampler)],
                                   uv, uv_dx, uv_dy)
                       .rgb;
        }
        float3 normal0 = float3(0.0f, 0.0f, 1.0f);
        if (!skip_normal_map && (material.flags & 1u) != 0u)
        {
            normal0 = vkr_normal_map_decode(
                g_textures[NonUniformResourceIndex(material.normal_texture)]
                    .SampleGrad(g_samplers[NonUniformResourceIndex(
                                    material.normal_sampler)],
                                uv, uv_dx, uv_dy)
                    .xyz,
                material.material_surface.z);
        }
        VkrVulkanTerrainMaterial *layers =
            source.terrain_materials + visible.material_index;
        // Slope and height masks read the world normal and height here,
        // before the caller finishes the surface normal.
        float4 mask = float4(0.0f);
        if (layers.mask_source == VKR_LAYER_MASK_TEXTURE)
        {
            mask = g_textures[NonUniformResourceIndex(layers.mask_texture)]
                       .SampleGrad(g_samplers[NonUniformResourceIndex(
                                       layers.mask_sampler)],
                                   uv, uv_dx, uv_dy);
        }
        float world_up = 0.0f;
        if (layers.mask_source == VKR_LAYER_MASK_SLOPE)
        {
            float3 world_normal = vkr_instance_transform_normal(
                instance.normal_column0.xyz, instance.normal_column1.xyz,
                instance.normal_column2.xyz,
                a.normal * perspective.x + b.normal * perspective.y +
                    c.normal * perspective.z);
            float length_squared = dot(world_normal, world_normal);
            world_up = length_squared > 1e-12f
                           ? world_normal.y * rsqrt(length_squared)
                           : 0.0f;
        }
        float world_height = wa.y * perspective.x + wb.y * perspective.y +
                             wc.y * perspective.z;
        layered = vkr_vk_terrain_surface(
            vkr_terrain_layer(base, orm0, normal0, float4(1.0f),
                              material.material_surface),
            layers,
            vkr_layer_mask_weights(layers.mask_source, color, mask, world_up,
                                   world_height, layers.mask_range.xy),
            uv, uv_dx, uv_dy, !skip_normal_map);
        base = layered.base;
    }
    if (neutral_lighting)
    {
        base.rgb = float3(0.5f);
    }
    float metallic =
        neutral_lighting ? 0.0f : saturate(material.material_surface.x);
    float feedback_roughness =
        neutral_lighting ? 0.5f : saturate(material.material_surface.y);
    float roughness = clamp(feedback_roughness, 0.04f, 1.0f);
    float ao = neutral_lighting ? 1.0f : saturate(material.material_surface.w);
    if (!neutral_lighting && terrain)
    {
        metallic = layered.metallic;
        feedback_roughness = layered.roughness;
        roughness = layered.roughness;
        ao = layered.occlusion;
    }
    else if (!neutral_lighting && (material.flags & 2u) != 0u)
    {
        float3 orm =
            g_textures[NonUniformResourceIndex(material.orm_texture)]
                .SampleGrad(
                    g_samplers[NonUniformResourceIndex(material.orm_sampler)],
                    uv, uv_dx * VKR_VK_SPECULAR_FOOTPRINT_SCALE,
                    uv_dy * VKR_VK_SPECULAR_FOOTPRINT_SCALE)
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
    if (!skip_normal_map && terrain)
    {
        surface.tangent_normal = layered.tangent_normal;
        surface.normal_mapped = 1u;
    }
    else if (!skip_normal_map && (material.flags & 1u) != 0u)
    {
        surface.tangent_normal = vkr_normal_map_decode(
            g_textures[NonUniformResourceIndex(material.normal_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(
                                material.normal_sampler)],
                            uv, uv_dx * VKR_VK_SPECULAR_FOOTPRINT_SCALE,
                            uv_dy * VKR_VK_SPECULAR_FOOTPRINT_SCALE)
                .xyz,
            material.material_surface.z);
        surface.normal_mapped = 1u;
    }
    float3 emissive =
        neutral_lighting ? float3(0.0f) : material.material_emissive.rgb;
    if (!neutral_lighting && (material.flags & 4u) != 0u)
        emissive *=
            g_textures[NonUniformResourceIndex(material.emissive_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(
                                material.emissive_sampler)],
                            uv, uv_dx, uv_dy)
                .rgb;
    emissive *= source.pre_exposure;
    uint2 base_color_extent;
    g_textures[NonUniformResourceIndex(material.base_color_texture)]
        .GetDimensions(base_color_extent.x, base_color_extent.y);
    float2 texture_extent = float2(base_color_extent);
    float footprint =
        max(length(uv_dx * texture_extent), length(uv_dy * texture_extent));
    surface.base = base;
    surface.object_position = object_position;
    surface.previous_object_position =
        vkr_previous_deformed_position(a.position, instance.previous_deformation_address, uint(ia)) * perspective.x +
        vkr_previous_deformed_position(b.position, instance.previous_deformation_address, uint(ib)) * perspective.y +
        vkr_previous_deformed_position(c.position, instance.previous_deformation_address, uint(ic)) * perspective.z;
    surface.previous_object_position.y +=
        (previous_height_a - a.position.y) * perspective.x +
        (previous_height_b - b.position.y) * perspective.y +
        (previous_height_c - c.position.y) * perspective.z;
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

// The frame's decals over a resolved surface's base colour, before lighting
// (ADR-092), with the tiled pipeline's rules. A decal's box projects along its
// -Y onto the surfaces facing its +Y. Seen from its +Y with its -Z up, its
// texture reads upright: u runs along +X and, as VKR images keep their bottom
// row at v = 0, v along -Z. Its material's base colour times its tint covers
// the surface by the texture's alpha, the decal's opacity, the fade by the
// angle between `normal` and the box's +Y and the fade toward the box's near
// and far faces; later decals lie on top. The wave iterates the union of its
// lanes' cell masks, so each decal row loads once for the wave; a lane outside
// a decal's box skips it, and a lane inside it has it in its own cell.
float3 vkr_vk_apply_decals(VkrVkResolveRoot root, float3 world_position,
                           float3 world_dx, float3 world_dy, float3 normal,
                           float3 base)
{
    float4 grid = root.decal_grid_origin_cell_size;
    uint3 dimensions = root.decal_grid_dimensions_count.xyz;
    int3 cell = int3(floor((world_position - grid.xyz) / grid.w));
    uint2 mask = uint2(0u);
    if (all(cell >= 0) && cell.x < int(dimensions.x) &&
        cell.y < int(dimensions.y) && cell.z < int(dimensions.z))
    {
        mask = root.decal_masks[uint(cell.x) +
                                dimensions.x * (uint(cell.y) +
                                                dimensions.y * uint(cell.z))];
    }
    float4 position = float4(world_position, 1.0f);
    [unroll]
    for (uint word = 0u; word < 2u; ++word)
    {
        uint remaining = WaveActiveBitOr(mask[word]);
        while (remaining != 0u)
        {
            uint index = word * 32u + firstbitlow(remaining);
            remaining &= remaining - 1u;
            VkrVulkanDecalRow decal = root.decals[index];
            float3 box = float3(dot(decal.world_to_box[0], position),
                                dot(decal.world_to_box[1], position),
                                dot(decal.world_to_box[2], position));
            if (any(abs(box) > 0.5f))
                continue;
            float weight =
                decal.fade.w *
                saturate(dot(normal, decal.world_to_box[1].xyz) *
                             decal.fade.x +
                         decal.fade.y) *
                saturate((0.5f - abs(box.y)) * decal.fade.z);
            if (weight <= 0.0f)
                continue;
            float2 uv_dx = float2(dot(decal.world_to_box[0].xyz, world_dx),
                                  -dot(decal.world_to_box[2].xyz, world_dx));
            float2 uv_dy = float2(dot(decal.world_to_box[0].xyz, world_dy),
                                  -dot(decal.world_to_box[2].xyz, world_dy));
            VkrVulkanMaterial material = root.materials[decal.material_index];
            float4 colour =
                g_textures[NonUniformResourceIndex(material.base_color_texture)]
                    .SampleGrad(g_samplers[NonUniformResourceIndex(
                                    material.base_color_sampler)],
                                float2(box.x + 0.5f, 0.5f - box.z), uv_dx,
                                uv_dy) *
                material.tint;
            base = lerp(base, colour.rgb, saturate(colour.a * weight));
        }
    }
    return base;
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

struct VkrVkBakedLamps
{
    float4 irradiance;
    float4 direction;
};

// The lamp layers a kernel samples (desktop baked lamps, ADR-104): the
// G-buffer resolve's and the transmission root's lightmap fields; a zero
// rectangle count samples none.
struct VkrVkLightmapSource
{
    VkrLightmapRect *lightmap_rects;
    uint lightmap_texture;
    uint lightmap_sampler;
    uint lightmap_rect_count;
    uint lightmap_layer_count;
    uint lightmap_active_count;
    float lightmap_inverse_page_size;
    uint lightmap_direction_texture;
    uint4 lightmap_active_layers[2];
    float4 lightmap_active_weights[2];
};

// The static lamps the bake holds at a lightmapped surface: the active lamp
// layers of its rectangle, each at its weight. Irradiance alpha scales the
// directional part from the lightmap surface (normal `surface_normal`) to a
// surface facing the dominant direction, and is zero for a draw without a
// lightmap. Direction is the layers' luminance-weighted dominant direction
// as 0.5 * d + 0.5 with its directionality in alpha.
VkrVkBakedLamps vkr_vk_baked_lamps(VkrVkLightmapSource root,
                                   VkrVulkanInstance instance,
                                   VkrVkSurface surface, float3 surface_normal)
{
    VkrVkBakedLamps baked = (VkrVkBakedLamps)0;
    uint slot = uint(instance.normal_column2.w);
    if (surface.has_lightmap_uv == 0u || slot == 0u ||
        slot > root.lightmap_rect_count)
        return baked;
    VkrLightmapRect rect = root.lightmap_rects[slot - 1u];
    float2 origin = float2(float(rect.origin & 0xffffu), float(rect.origin >> 16u));
    float2 extent = float2(float(rect.extent & 0xffffu), float(rect.extent >> 16u));
    float2 uv = (origin + surface.lightmap_uv * extent) *
                root.lightmap_inverse_page_size;
    uint slice_base = rect.page * root.lightmap_layer_count;
    float3 irradiance = 0.0f;
    float3 direction_sum = 0.0f;
    float luminance_sum = 0.0f;
    for (uint i = 0u; i < root.lightmap_active_count; ++i)
    {
        uint layer = root.lightmap_active_layers[i / 4u][i % 4u];
        float weight = root.lightmap_active_weights[i / 4u][i % 4u];
        float3 slice_uv = float3(uv, float(slice_base + layer));
        float3 layer_irradiance = weight *
            g_array_textures[root.lightmap_texture]
                .SampleLevel(g_samplers[root.lightmap_sampler], slice_uv, 0.0f)
                .rgb;
        irradiance += layer_irradiance;
        if (root.lightmap_direction_texture != 0u)
        {
            float4 encoded = g_array_textures[root.lightmap_direction_texture]
                .SampleLevel(g_samplers[root.lightmap_sampler], slice_uv, 0.0f);
            float3 d = encoded.rgb * 2.0f - 1.0f;
            float length_d = length(d);
            float luminance = dot(layer_irradiance, float3(0.2126f, 0.7152f, 0.0722f));
            if (length_d > 1.0e-4f)
                direction_sum += luminance * encoded.a * (d / length_d);
            luminance_sum += luminance;
        }
    }
    float3 mean = luminance_sum > 0.0f ? direction_sum / luminance_sum : 0.0f;
    float directionality = saturate(length(mean));
    float3 dominant = directionality > 1.0e-4f ? mean / directionality
                                               : surface_normal;
    // Irradiance facing the dominant direction, per unit of the lightmap
    // surface's irradiance; bounded where the direction grazes it.
    float facing = 1.0f / max(dot(surface_normal, dominant), 0.1f);
    baked.irradiance = float4(irradiance, facing);
    baked.direction = float4(dominant * 0.5f + 0.5f, directionality);
    return baked;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_gbuffer_resolve(uint3 dispatch_id : SV_DispatchThreadID,
                        uint3 group_id : SV_GroupID,
                        uint group_index : SV_GroupIndex)
{
    VkrVkResolveRoot root = vkr_vk_uniform_root(*g_resolve_push.root);
    uint3 thread_id =
        root.sample1_pixels != nullptr
            ? vk_msaa_sample1_pixel(root.sample1_pixels, group_id, group_index)
            : dispatch_id;
    if (any(thread_id.xy >= root.extent))
        return;
    // SMAA S2x's second sample resolves only where it names another
    // surface than the first.
    bool first_sample = root.edge_mask_texture == 0u;
    if (!first_sample &&
        g_storage_2d[root.edge_mask_texture][thread_id.xy].x == 0.0f)
        return;
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy];
    if (encoded.x == 0u)
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, 0.0f);
        if (first_sample && root.history_valid != 0u)
        {
            // Sky pixels lie on the far plane, reversed depth 0 (ADR-109).
            float2 uv = (float2(thread_id.xy) + 0.5f) / float2(root.extent);
            float4 previous_clip = mul(root.sky_reprojection,
                                        float4(uv * 2.0f - 1.0f, 0.0f, 1.0f));
            if (previous_clip.w > 1e-6f)
            {
                float2 previous_uv = previous_clip.xy / previous_clip.w * 0.5f + 0.5f;
                g_storage_float2_2d[root.motion_texture][thread_id.xy] = previous_uv - uv;
                // The second channel is the previous device depth: the far
                // plane's 0 for sky.
                g_storage_float2_2d[root.validity_texture][thread_id.xy] = float2(1.0f, 0.0f);
            }
        }
        return;
    }
    VkrVkSurfaceSource source = {
        root.geometry_rows,    root.visible_rows,    root.instances,
        root.materials,        root.vertices,        root.indices,
        root.compaction_state, root.view_projection, root.extent,
        root.visible_capacity, root.geometry_count,  root.material_count,
        root.instance_count, root.render_mode, root.pre_exposure,
        root.terrain_materials, root.decal_grid_dimensions_count.w,
        root.lightmap_rect_count,
    };
    // SMAA S2x's second sample lies at (0.25, 0.25) in the pixel, where its
    // visibility and depth were rasterized.
    float2 position =
        float2(thread_id.xy) + (first_sample ? 0.5f : 0.25f);
    VkrVkSurface surface;
    if (!vkr_vk_resolve_surface(source, encoded, position, surface))
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, -1.0f);
        return;
    }
    if (root.render_mode == 12u)
    {
        vkr_vk_resolve_defaults(root, thread_id.xy, 0.0f);
        g_storage_2d[root.scene_texture][thread_id.xy] = float4(surface.wire_color, 0.0f);
        return;
    }
    bool neutral_lighting = vkr_editor_neutral_lighting(root.render_mode);
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
        if ((instance.deformation_address == 0 ||
         instance.previous_deformation_address != 0) &&
            previous.valid != 0u &&
            previous.generation == instance.temporal_generation &&
            previous.frame_index == root.previous_frame_index)
        {
            float4 current_clip = mul(
                root.current_view_projection,
                mul(instance.model, float4(surface.object_position, 1.0f)));
            float4 previous_clip = mul(
                root.previous_view_projection,
                mul(previous.model, float4(surface.previous_object_position, 1.0f)));
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
    if (first_sample)
    {
        g_storage_float2_2d[root.motion_texture][thread_id.xy] = motion;
        g_storage_float2_2d[root.validity_texture][thread_id.xy] = validity;
    }
    if (root.baked_lamps_texture != 0u)
    {
        // The lightmap's surface: its interpolated normal, before normal
        // mapping, which the bake's texels share.
        float3 surface_normal = normalize(vkr_instance_transform_normal(
            instance.normal_column0.xyz, instance.normal_column1.xyz,
            instance.normal_column2.xyz, surface.object_normal));
        VkrVkLightmapSource lightmap = {
            root.lightmap_rects, root.lightmap_texture, root.lightmap_sampler,
            root.lightmap_rect_count, root.lightmap_layer_count,
            root.lightmap_active_count, root.lightmap_inverse_page_size,
            root.lightmap_direction_texture, root.lightmap_active_layers,
            root.lightmap_active_weights,
        };
        VkrVkBakedLamps baked =
            vkr_vk_baked_lamps(lightmap, instance, surface, surface_normal);
        g_storage_2d[root.baked_lamps_texture][thread_id.xy] = baked.irradiance;
        g_storage_2d[root.baked_direction_texture][thread_id.xy] =
            baked.direction;
    }
    // Decals change the base colour the lighting views see; the neutral
    // inspection views keep their neutral base.
    if (root.decal_grid_dimensions_count.w != 0u && !neutral_lighting)
    {
        surface.base.rgb = vkr_vk_apply_decals(
            root, surface.world_position, surface.world_dx, surface.world_dy,
            surface.geometric_normal, surface.base.rgb);
    }
    // One material row serves F0 and every optional layer below.
    VkrVulkanMaterial material = root.materials[surface.material_index];
    float3 f0 = lerp(neutral_lighting ? float3(0.04f)
                     : saturate(material.material_dielectric_specular.rgb),
                     surface.base.rgb, surface.metallic);
    g_storage_2d[root.albedo_texture][thread_id.xy] =
        float4(surface.base.rgb * (1.0f - surface.metallic), surface.occlusion);
    g_storage_2d[root.specular_texture][thread_id.xy] =
        float4(f0, surface.roughness);
    g_storage_float2_2d[root.normal_texture][thread_id.xy] =
        vkr_vk_octahedral_encode(surface.normal);
    float clearcoat_factor =
        neutral_lighting ? 0.0f : saturate(material.material_clearcoat.x);
    if (clearcoat_factor > 0.0f && (material.flags & 32u) != 0u)
        clearcoat_factor *= g_textures[NonUniformResourceIndex(
                                material.clearcoat_texture)]
                                .SampleGrad(g_samplers[NonUniformResourceIndex(
                                    material.clearcoat_sampler)],
                                            surface.uv, surface.uv_dx,
                                            surface.uv_dy)
                                .r;
    float clearcoat_roughness = 0.0f;
    float3 clearcoat_normal = surface.geometric_normal;
    if (clearcoat_factor > 0.0f)
    {
        clearcoat_roughness = material.material_clearcoat.y;
        if ((material.flags & 64u) != 0u)
            clearcoat_roughness *= g_textures[NonUniformResourceIndex(
                                        material.clearcoat_roughness_texture)]
                                        .SampleGrad(g_samplers[NonUniformResourceIndex(
                                            material.clearcoat_roughness_sampler)],
                                                    surface.uv, surface.uv_dx,
                                                    surface.uv_dy)
                                        .g;
        clearcoat_roughness = clamp(clearcoat_roughness, 0.04f, 1.0f);
        if ((material.flags & 128u) != 0u)
        {
            float3 sampled = vkr_normal_map_decode(
                g_textures[NonUniformResourceIndex(
                    material.clearcoat_normal_texture)]
                    .SampleGrad(g_samplers[NonUniformResourceIndex(
                        material.clearcoat_normal_sampler)],
                                surface.uv, surface.uv_dx, surface.uv_dy)
                    .xyz,
                material.material_clearcoat.z);
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
    if (root.clearcoat_texture != 0u)
    {
        g_storage_2d[root.clearcoat_texture][thread_id.xy] =
        float4(clearcoat_factor, clearcoat_roughness,
        vkr_vk_octahedral_encode(clearcoat_normal));
    }
    float3 sheen_color =
        neutral_lighting ? float3(0.0f) : max(material.material_sheen.rgb, 0.0f);
    float sheen_roughness = 0.0f;
    if (vkr_sheen_active(sheen_color))
    {
        if ((material.flags & 256u) != 0u)
            sheen_color *= g_textures[NonUniformResourceIndex(
                               material.sheen_color_texture)]
                               .SampleGrad(g_samplers[NonUniformResourceIndex(
                                   material.sheen_color_sampler)],
                                           surface.uv, surface.uv_dx,
                                           surface.uv_dy)
                               .rgb;
        if (vkr_sheen_active(sheen_color))
        {
            sheen_roughness = material.material_sheen.w;
            if ((material.flags & 512u) != 0u)
                sheen_roughness *= g_textures[NonUniformResourceIndex(
                                      material.sheen_roughness_texture)]
                                      .SampleGrad(g_samplers[NonUniformResourceIndex(
                                          material.sheen_roughness_sampler)],
                                                  surface.uv, surface.uv_dx,
                                                  surface.uv_dy)
                                      .a;
            sheen_roughness = clamp(sheen_roughness,
                                    VKR_SHEEN_MIN_ROUGHNESS, 1.0f);
        }
        else
            sheen_roughness = 0.0f;
    }
    if (root.sheen_texture != 0u)
    {
        g_storage_2d[root.sheen_texture][thread_id.xy] =
        float4(sheen_color, sheen_roughness);
    }
  float anisotropy_strength =
      neutral_lighting ? 0.0f : material.material_anisotropy.x;
  float3 anisotropy_axis = float3(0.0f);
  if (anisotropy_strength > 0.0f) {
    float3 map = float3(1.0f, 0.5f, 1.0f);
    if ((material.flags & 1024u) != 0u)
      map = g_textures[NonUniformResourceIndex(material.anisotropy_texture)].SampleGrad(g_samplers[NonUniformResourceIndex(material.anisotropy_sampler)], surface.uv, surface.uv_dx, surface.uv_dy).rgb;
    anisotropy_strength *= map.b;
    anisotropy_axis = vkr_anisotropy_axis(surface.geometric_normal, surface.normal,
        float4(mul(instance.model, float4(surface.object_tangent.xyz, 0.0f)).xyz, surface.object_tangent.w < 0.0f ? -1.0f : 1.0f), map.rg * 2.0f - 1.0f, material.material_anisotropy.yz);
  }
  if (root.anisotropy_texture != 0u)
  {
      g_storage_2d[root.anisotropy_texture][thread_id.xy] = float4(anisotropy_strength > 0.0f ? vkr_anisotropy_encode_axis(anisotropy_axis) : float2(0.0f), anisotropy_strength, 0.0f);
  }
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

// The deferred lighting path owns the background: a fallback colour, a
// uniform radiance, or the camera-dependent atmosphere.
float3 vkr_vk_deferred_sky(VkrVkLightingRoot root, uint2 pixel)
{
    if (root.sky_mode == VKR_SKY_MODE_NONE)
        return float3(0.02f, 0.02f, 0.03f) * root.frame.pre_exposure;
    if (root.sky_mode == VKR_SKY_MODE_CONSTANT)
        return root.sky_radiance.rgb;
    // Framebuffer-derived NDC needs no Y flip here: the pipeline's viewport
    // already owns that flip, so the view-ray matrix of the projection alone
    // lands in world space. The resolve flips because it performs the
    // viewport transform by hand instead of inverting a matrix.
    // Depth 1 is the near plane under reversed camera depth (ADR-109). An
    // orthographic camera's matrix returns its forward axis for every pixel.
    float2 ndc = (float2(pixel) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
    float3 direction =
        normalize(mul(root.view_rays, float4(ndc, 1.0f, 1.0f)).xyz);
    // The sky-view lookup already integrates to the top of the atmosphere, so
    // sky pixels take no aerial perspective. Its sun and moon tables add; the
    // discs and their glows are analytic. Pass-root descriptor indices are
    // dynamically uniform; see packet_directional_shadow.
    VkrVulkanSky *sky = root.frame.sky;
    VkrAtmosphereParams params = sky.params.atmosphere;
    float3 radiance = g_textures[root.sky_view_texture]
        .SampleLevel(g_samplers[root.sky_sampler],
                     vkr_sky_view_uv(params, direction, params.sun.xyz,
                                     VKR_ATMOSPHERE_SKY_VIEW_SUN_TABLE),
                     0.0f)
        .rgb;
    if (!vkr_atmosphere_light_dark(params.lunar))
        radiance += g_textures[root.sky_view_texture]
            .SampleLevel(g_samplers[root.sky_sampler],
                         vkr_sky_view_uv(params, direction, params.moon.xyz,
                                         VKR_ATMOSPHERE_SKY_VIEW_MOON_TABLE),
                         0.0f)
            .rgb;
    float3 disc = float3(0.0f);
    if (!vkr_sky_view_hits_ground(params, direction.y))
    {
        float3 view_transmittance = g_textures[sky.transmittance_texture]
            .SampleLevel(g_samplers[sky.sampler],
                         vkr_atmosphere_transmittance_uv(
                             params, params.planet.x + params.planet.z,
                             direction.y),
                         0.0f)
            .rgb;
        disc = vkr_atmosphere_sun_disc(params, direction, view_transmittance) +
               vkr_atmosphere_moon_disc(params, direction, view_transmittance);
        radiance += vkr_atmosphere_light_glow(params, direction, params.sun,
                                              params.solar,
                                              view_transmittance) +
                    vkr_atmosphere_light_glow(params, direction, params.moon,
                                              params.lunar,
                                              view_transmittance);
        // Stars sit behind the whole atmosphere; the neighbouring pixel's ray
        // sets each star's footprint.
        if (sky.params.star_axis.w > 0.0f)
        {
            float2 next_ndc =
                (float2(pixel) + float2(1.5f, 0.5f)) / float2(root.extent) *
                    2.0f -
                1.0f;
            float3 next = normalize(
                mul(root.view_rays, float4(next_ndc, 1.0f, 1.0f)).xyz);
            radiance += vkr_atmosphere_stars(sky.params, direction,
                                             length(cross(direction, next))) *
                        view_transmittance;
        }
    }
    // The cloud trace shares screen coordinates with this pass; clouds carry
    // aerial perspective at their own depth. The disc follows the cloud's
    // apparent opacity; see vkr_cloud_disc_visibility.
    if (sky.params.clouds.noise.w > 0.0f)
    {
        float2 uv = (float2(pixel) + 0.5f) / float2(root.extent);
        float4 cloud = g_textures[sky.cloud_radiance_texture].SampleLevel(
            g_samplers[sky.sampler], uv, 0.0f);
        radiance = vkr_cloud_composite(radiance, cloud);
        disc *= vkr_cloud_disc_visibility(cloud.a);
    }
    return radiance + disc;
}

float vkr_vk_deferred_filter_roughness(VkrVkLightingRoot root, uint2 pixel,
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
    if (root.edge_mask_texture != 0u)
    {
        // SMAA S2x's second sample: only listed pixels hold a resolved
        // second-sample normal.
        same_x *= g_storage_2d[root.edge_mask_texture][pixel_x].x;
        same_y *= g_storage_2d[root.edge_mask_texture][pixel_y].x;
    }
    float3 normal_dx = (normal_x - normal) * same_x;
    float3 normal_dy = (normal_y - normal) * same_y;
    float normal_variance =
        0.25f * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
    return vkr_ggx_filter_roughness(roughness, normal_variance);
}

// Camera depth of one view and how a pixel's depth reconstructs its world
// position: the inputs the geometric normal reads.
struct VkrVkDepthView
{
    uint vbuffer_texture;
    uint depth_texture;
    uint2 extent;
    float4x4 inverse_view_projection;
    // Where in the pixel its depth lies: 0.5, or 0.25 for SMAA S2x's second
    // sample.
    float sample_position;
};

float3 vkr_vk_depth_view_world(VkrVkDepthView view, uint2 pixel, float depth)
{
    // See vkr_vk_deferred_sky: no Y flip when inverting the view-projection.
    float2 uv = (float2(pixel) + view.sample_position) / float2(view.extent);
    float2 ndc_xy = uv * 2.0f - 1.0f;
    return vkr_vk_deferred_homogeneous(
        mul(view.inverse_view_projection, float4(ndc_xy, depth, 1.0f)));
}

// The step from `world` to a neighbour along one screen axis on the same
// visible draw. It takes the side whose depth changes less, so a silhouette
// or a fold within the draw does not tilt it, and fails when neither
// neighbour lies on the draw.
bool vkr_vk_depth_view_axis(VkrVkDepthView view, uint2 pixel,
                            uint2 pixel_step, uint visible_index, float depth,
                            float3 world, out float3 axis)
{
    axis = 0.0f;
    bool forward_valid = false;
    bool backward_valid = false;
    float forward_depth = 0.0f;
    float backward_depth = 0.0f;
    uint2 forward_pixel = pixel + pixel_step;
    if (all(forward_pixel < view.extent) &&
        g_storage_uint2_2d[view.vbuffer_texture][forward_pixel].x ==
            visible_index)
    {
        forward_valid = true;
        forward_depth = g_depth_textures[view.depth_texture].Load(
            int3(forward_pixel, 0));
    }
    if (all(pixel >= pixel_step))
    {
        uint2 backward_pixel = pixel - pixel_step;
        if (g_storage_uint2_2d[view.vbuffer_texture][backward_pixel].x ==
            visible_index)
        {
            backward_valid = true;
            backward_depth = g_depth_textures[view.depth_texture].Load(
                int3(backward_pixel, 0));
        }
    }
    if (!forward_valid && !backward_valid)
        return false;
    bool use_forward =
        forward_valid &&
        (!backward_valid ||
         abs(forward_depth - depth) <= abs(backward_depth - depth));
    if (use_forward)
    {
        axis = vkr_vk_depth_view_world(view, forward_pixel, forward_depth) -
               world;
    }
    else
    {
        axis = world - vkr_vk_depth_view_world(view, pixel - pixel_step,
                                               backward_depth);
    }
    return true;
}

// The geometric normal: the G-buffer keeps only the shading normal, so this
// reconstructs the surface's normal from neighbouring depth on the same draw,
// facing the viewer. A pixel without a neighbour on the draw along either
// axis keeps the shading normal.
float3 vkr_vk_depth_view_geometric_normal(VkrVkDepthView view, uint2 pixel,
                                          uint visible_index, float depth,
                                          float3 world, float3 normal,
                                          float3 to_viewer)
{
    float3 axis_x;
    float3 axis_y;
    if (!vkr_vk_depth_view_axis(view, pixel, uint2(1u, 0u), visible_index,
                                depth, world, axis_x) ||
        !vkr_vk_depth_view_axis(view, pixel, uint2(0u, 1u), visible_index,
                                depth, world, axis_y))
        return normal;
    float3 geometric = cross(axis_x, axis_y);
    float length_squared = dot(geometric, geometric);
    if (length_squared <= 1e-20f)
        return normal;
    geometric *= rsqrt(length_squared);
    return dot(geometric, to_viewer) < 0.0f ? -geometric : geometric;
}

VkrVkDepthView vkr_vk_lighting_depth_view(VkrVkLightingRoot root)
{
    VkrVkDepthView view;
    view.vbuffer_texture = root.vbuffer_texture;
    view.depth_texture = root.depth_texture;
    view.extent = root.extent;
    view.inverse_view_projection = root.inverse_view_projection;
    // SMAA S2x's second sample's depth lies at (0.25, 0.25) in the pixel.
    view.sample_position = root.edge_mask_texture != 0u ? 0.25f : 0.5f;
    return view;
}

float3 vkr_vk_deferred_pixel_world(VkrVkLightingRoot root, uint2 pixel,
                                   float depth)
{
    return vkr_vk_depth_view_world(vkr_vk_lighting_depth_view(root), pixel,
                                   depth);
}

float3 vk_local_shadow_mask_world(VkrVkLocalShadowMaskRoot root, float2 ndc,
                                  float depth)
{
    return vkr_vk_deferred_homogeneous(
        mul(root.inverse_view_projection, float4(ndc, depth, 1.0f)));
}

// Contact visibility toward one light: marches the depth buffer from `origin`
// along `direction` and stops at the first sample whose camera ray passes
// behind a nearby surface. The march leaves the screen without occlusion.
float vk_contact_shadow(VkrVulkanPacketFrameRoot *frame, VkrVkDepthView depth,
                        float3 origin, float3 direction, float march_length,
                        uint steps, float noise)
{
    float4 clip_origin = mul(frame.view_projection, float4(origin, 1.0f));
    float4 clip_span =
        mul(frame.view_projection, float4(direction * march_length, 0.0f));
    for (uint step = 0u; step < steps; ++step)
    {
        float t = (float(step) + noise) / float(steps);
        float4 clip = clip_origin + t * clip_span;
        if (clip.w <= 1e-4f)
            break;
        float2 ndc = clip.xy / clip.w;
        if (any(abs(ndc) >= 1.0f))
            break;
        // See vkr_vk_deferred_sky: no Y flip between NDC and pixels.
        uint2 pixel = min(uint2((ndc * 0.5f + 0.5f) * float2(depth.extent)),
                          depth.extent - 1u);
        float3 surface = vkr_vk_deferred_homogeneous(mul(
            depth.inverse_view_projection,
            float4(ndc,
                   g_depth_textures[depth.depth_texture].Load(int3(pixel, 0)),
                   1.0f)));
        float3 sample_position = origin + direction * (march_length * t);
        float surface_in_front =
            dot(surface - sample_position,
                packet_view_direction(frame, sample_position));
        if (vkr_local_shadow_contact_occluded(
                surface_in_front,
                distance(sample_position, frame.view_position.xyz)))
            return vkr_local_shadow_contact_visibility(t);
    }
    return 1.0f;
}

// Shadow.LocalMask stores every in-range shadowed light's visibility with the
// receiver normal, range test and light traversal of deferred lighting, so the
// lighting kernels read one texel per light instead of filtering shadow maps.
// Under temporal reconstruction, fully filtered lights take rotated temporal
// taps. In the contact variant, which the Ultra preset selects, a short
// contact-shadow march multiplies the filtered visibility of each light that
// takes the full filter; without it the march's registers do not lower the
// kernel's occupancy.
void vk_local_shadow_mask_impl<let ContactShadows : bool,
                               let BakedLamps : bool>(uint3 thread_id)
{
    VkrVkLocalShadowMaskRoot root = vkr_vk_uniform_root(*g_local_shadow_mask_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy];
    if (encoded.x == 0u)
        return;
    VkrVulkanPacketFrameRoot *frame = root.frame;
    float depth =
        g_depth_textures[root.depth_texture].Load(int3(thread_id.xy, 0));
    float2 ndc_xy =
        (float2(thread_id.xy) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
    float3 world = vk_local_shadow_mask_world(root, ndc_xy, depth);
    // Only the shadowed lights that reach the view can take a slot. A pixel
    // in none of their grid cells stores nothing, so it skips the surface
    // work unless the baked-lamp part below needs it.
    uint4 point_mask = packet_point_light_mask(frame, world) & root.mask_lights;
    bool point_lights = any(point_mask != 0u);
    if (!BakedLamps && !point_lights)
        return;
    float3 normal = vkr_vk_octahedral_decode(
        g_storage_float2_2d[root.normal_texture][thread_id.xy]);
    VkrVkDepthView depth_view;
    depth_view.vbuffer_texture = root.vbuffer_texture;
    depth_view.depth_texture = root.depth_texture;
    depth_view.extent = root.extent;
    depth_view.inverse_view_projection = root.inverse_view_projection;
    depth_view.sample_position = 0.5f;
    float camera_distance = distance(world, frame.view_position.xyz);
    float contact_noise =
        vkr_local_shadow_contact_noise(thread_id.xy, root.contact_noise_index);
    // Without temporal reconstruction the full filter keeps its fixed taps.
    float2 tap_rotation = float2(1.0f, 0.0f);
    uint tap_count = VKR_LOCAL_SHADOW_FILTER_TAP_COUNT;
    uint blocker_count = VKR_LOCAL_SHADOW_PCSS_BLOCKER_COUNT;
    if (root.temporal_filter != 0u)
    {
        tap_rotation = vkr_shadow_kernel_rotation(thread_id.xy,
                                                  root.contact_noise_index);
        tap_count = VKR_LOCAL_SHADOW_TEMPORAL_TAP_COUNT;
        blocker_count = VKR_LOCAL_SHADOW_PCSS_TEMPORAL_BLOCKER_COUNT;
    }
    float diffuse_transmission = 0.0f;
    if (point_lights && !vkr_editor_neutral_lighting(frame.render_mode))
    {
        diffuse_transmission =
            frame.materials[root.visible_rows[encoded.x - 1u].material_index]
                .material_diffuse_transmission.w;
    }
    uint point_count = min(frame.point_light_count, 128u);
    // Slot of the next shadowed light this pixel stores; lighting counts the
    // same lights in the same order.
    uint slot = 0u;
    for (uint word = 0u; word < 4u; ++word)
    {
        uint remaining = WaveActiveBitOr(point_mask[word]);
        while (remaining != 0u)
        {
            uint bit = uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            uint light_index = word * 32u + bit;
            if (light_index >= point_count ||
                (point_mask[word] & (1u << bit)) == 0u)
                continue;
            VkrGpuPointLightRow light = frame.point_light_data[light_index];
            if (light.p3.w == 0.0f)
                continue;
            VkrPunctualLightTerm term = vkr_punctual_light_term(
                light.p0, light.p1, light.p2, light.p3, world);
            if (!term.in_range || term.cone <= 0.0f)
                continue;
            bool back_lit = diffuse_transmission > 0.0f &&
                            dot(normal, term.direction) < 0.0f;
            float3 shadow_normal = back_lit ? -normal : normal;
            if (vkr_local_light_contribution(
                    light.p1.rgb, light.p2.x, term.attenuation * term.cone,
                    dot(shadow_normal, term.direction), frame.pre_exposure) <
                VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF)
                continue;
            // A light that cannot light the pixel takes no slot, and lighting
            // filters lights past the last slot inline.
            if (!vkr_local_shadow_light_faces(shadow_normal, term.direction) ||
                slot >= VKR_LOCAL_SHADOW_MASK_SLOT_COUNT)
                continue;
            float3 visibility = packet_local_shadow(
                frame, light, term.kind, world, shadow_normal, tap_rotation,
                tap_count, blocker_count);
            VkrLocalShadowView view =
                frame.local_shadow_views[uint(light.p3.w) - 1u];
            if (ContactShadows && view.shadow_params.z < 0.5f &&
                any(visibility > 0.0f))
            {
                float3 origin =
                    world + shadow_normal *
                                vkr_local_shadow_contact_start_offset(
                                    camera_distance);
                float march_length =
                    min(VKR_LOCAL_SHADOW_CONTACT_LENGTH,
                        0.5f * distance(light.p0.xyz, world));
                float contact = vk_contact_shadow(
                    frame, depth_view, origin, term.direction, march_length,
                    VKR_LOCAL_SHADOW_CONTACT_STEPS, contact_noise);
                visibility *= vkr_local_shadow_apply_strength(
                    float3(contact), view.shadow_params.x);
            }
            g_storage_images[root.mask_texture][uint3(thread_id.xy, slot)] =
                float4(visibility, vkr_local_shadow_mask_tag(light_index));
            ++slot;
        }
    }
    // The baked-lamp variants alone carry this work, so the default kernels
    // keep their registers.
    if (!BakedLamps || root.baked_lamp_count == 0u ||
        g_storage_2d[root.baked_lamps_texture][thread_id.xy].a <= 0.0f)
        return;
    // A selected baked lamp's lightmap holds its light past the static
    // casters only. The same taps through its static and composite squares
    // give v_static - v_composite, the share of that light its moving
    // casters hide; deferred lighting subtracts it from the baked light.
    float3 hidden = 0.0f;
    for (uint lamp = 0u; lamp < root.baked_lamp_count; ++lamp)
    {
        VkrVkBakedShadowLamp baked = root.baked_lamps[lamp];
        VkrGpuPointLightRow light = baked.light;
        VkrPunctualLightTerm term = vkr_punctual_light_term(
            light.p0, light.p1, light.p2, light.p3, world);
        float n_dot_l = dot(normal, term.direction);
        if (!term.in_range || term.cone <= 0.0f || n_dot_l <= 0.0f)
            continue;
        // A face without a dynamic square holds no moving caster: its
        // static and composite views name one square, so the difference is
        // zero and neither is read. A moving caster that shadows the pixel
        // lies on its ray, in the same face.
        uint face = term.kind == 2u
            ? 0u
            : vkr_local_shadow_face(world - light.p0.xyz);
        if (all(frame.local_shadow_views[baked.static_first_view + face]
                    .atlas_rect ==
                frame.local_shadow_views[baked.composite_first_view + face]
                    .atlas_rect))
            continue;
        light.p3.w = float(baked.static_first_view + 1u);
        float3 static_visibility = packet_local_shadow(
            frame, light, term.kind, world, normal, tap_rotation, tap_count,
            blocker_count);
        if (all(static_visibility <= 0.0f))
            continue;
        light.p3.w = float(baked.composite_first_view + 1u);
        float3 composite_visibility = packet_local_shadow(
            frame, light, term.kind, world, normal, tap_rotation, tap_count,
            blocker_count);
        hidden += light.p1.rgb * light.p2.x * term.attenuation * term.cone *
                  n_dot_l * max(static_visibility - composite_visibility, 0.0f);
    }
    g_storage_2d[root.baked_lamp_shadow_texture][thread_id.xy] =
        float4(hidden, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_local_shadow_mask(uint3 thread_id : SV_DispatchThreadID)
{
    vk_local_shadow_mask_impl<false, false>(thread_id);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_local_shadow_mask_contact(uint3 thread_id : SV_DispatchThreadID)
{
    vk_local_shadow_mask_impl<true, false>(thread_id);
}

// With selected baked lamps (ADR-104), the mask also
// writes the light their moving casters hide.
[shader("compute")]
[numthreads(8, 8, 1)]
void vk_local_shadow_mask_baked(uint3 thread_id : SV_DispatchThreadID)
{
    vk_local_shadow_mask_impl<false, true>(thread_id);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_local_shadow_mask_contact_baked(uint3 thread_id : SV_DispatchThreadID)
{
    vk_local_shadow_mask_impl<true, true>(thread_id);
}

// A G-buffer surface pixel as the volume lookup takes it. A lightmapped
// pixel whose lightmap holds the baked lamps leaves out the volume's lamp
// part.
struct VkDiffuseVolumePixel
{
    float3 world;
    float3 normal;
    bool lamps_baked;
};

VkDiffuseVolumePixel vk_diffuse_volume_pixel(VkrVkDiffuseVolumeSampleRoot root,
                                             uint2 pixel)
{
    VkDiffuseVolumePixel surface;
    surface.normal = vkr_vk_octahedral_decode(
        g_storage_float2_2d[root.normal_texture][pixel]);
    float depth = g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    float2 ndc_xy = (float2(pixel) + 0.5f) / float2(root.extent) * 2.0f - 1.0f;
    surface.world = vkr_vk_deferred_homogeneous(
        mul(root.inverse_view_projection, float4(ndc_xy, depth, 1.0f)));
    surface.lamps_baked =
        root.baked_lamps_texture != 0u &&
        g_storage_2d[root.baked_lamps_texture][pixel].a > 0.0f;
    return surface;
}

// Camera distance, negative where the lightmap holds the baked lamps, so
// the upsample never mixes the two compositions.
float vk_diffuse_volume_guide_distance(VkrVulkanPacketFrameRoot *frame,
                                       VkDiffuseVolumePixel surface)
{
    float camera_distance = distance(surface.world, frame.view_position.xyz);
    return surface.lamps_baked ? -camera_distance : camera_distance;
}

// The volume images hold a pixel's response in RGB and, in alpha, zero
// where the volume does not cover it, otherwise the response's sky share
// (packet_diffuse_volume_lookup), kept at least this so that it stays
// nonzero.
#define VK_DIFFUSE_VOLUME_MIN_SKY_SHARE (1.0f / 1024.0f)

// A surface pixel's lookup. A mover's pixels look the volume up at its
// lighting origin (ADR-120) with their own normal; `pixel` holds a surface.
float4 vk_diffuse_volume_lookup(VkrVkDiffuseVolumeSampleRoot root,
                                uint2 pixel, VkDiffuseVolumePixel surface)
{
    uint visible = g_storage_uint2_2d[root.vbuffer_texture][pixel].x;
    float3 position = packet_lighting_position(
        root.frame, root.visible_rows[visible - 1u].instance_index,
        surface.world);
    float3 response;
    float sky_share;
    if (packet_diffuse_volume_lookup(root.frame, position, surface.normal,
                                     !surface.lamps_baked, response,
                                     sky_share))
        return float4(response,
                      max(sky_share, VK_DIFFUSE_VOLUME_MIN_SKY_SHARE));
    return 0.0f;
}

// A pixel's baked light from its DiffuseVolume texel.
VkrVulkanBakedLight vk_diffuse_volume_baked_light(
    VkrVulkanPacketFrameRoot *frame, float4 sampled)
{
    if (sampled.a <= 0.0f)
        return packet_no_baked_light();
    // The baked volume stores physical E/pi; lighting is pre-exposed.
    return packet_volume_baked_light(sampled.rgb * frame.pre_exposure,
                                     sampled.a);
}

// The pre-exposed irradiance of the baked lamps the G-buffer resolve wrote
// for a pixel (`baked`, ADR-104), less the light Shadow.LocalMask found
// their moving casters hide there (`shadow_texture`, zero without a
// selected lamp). Zero at a pixel without a lightmap.
float3 vk_baked_lamp_irradiance(VkrVulkanPacketFrameRoot *frame, float4 baked,
                                uint shadow_texture, uint2 pixel)
{
    float3 irradiance = baked.rgb * frame.pre_exposure;
    if (baked.a > 0.0f && shadow_texture != 0u)
    {
        irradiance = max(irradiance - g_storage_2d[shadow_texture][pixel].rgb,
                         0.0f);
    }
    return irradiance;
}

// The volume at half resolution: each texel looks it up at the first
// surface pixel of its 2x2 block and keeps that pixel as its guide.
[shader("compute")]
[numthreads(8, 8, 1)]
void vk_diffuse_volume_sample(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkDiffuseVolumeSampleRoot root =
        vkr_vk_uniform_root(*g_diffuse_volume_sample_push.root);
    if (all(thread_id.xy == 0u))
    {
        root.fallback_arguments[0] = 0u;
        root.fallback_arguments[1] = 1u;
        root.fallback_arguments[2] = 1u;
        root.fallback_arguments[3] = 0u;
    }
    if (any(thread_id.xy >= root.half_extent))
        return;
    VkrVulkanPacketFrameRoot *frame = root.frame;
    float4 result = 0.0f;
    float4 guide = 0.0f;
    uint2 chosen = thread_id.xy * 2u;
    bool found = false;
    [loop] for (uint i = 0u; i < 4u; ++i)
    {
        uint2 pixel = thread_id.xy * 2u + uint2(i & 1u, i >> 1u);
        if (all(pixel < root.extent) &&
            g_storage_uint2_2d[root.vbuffer_texture][pixel].x != 0u)
        {
            chosen = pixel;
            found = true;
            break;
        }
    }
    if (found)
    {
        VkDiffuseVolumePixel surface = vk_diffuse_volume_pixel(root, chosen);
        result = vk_diffuse_volume_lookup(root, chosen, surface);
        guide = float4(surface.normal,
                       vk_diffuse_volume_guide_distance(frame, surface));
    }
    g_storage_2d[root.output_texture][thread_id.xy] = result;
    g_storage_2d[root.guide_texture][thread_id.xy] = guide;
}

// Relative camera-distance difference over which a half-resolution texel
// stops counting for a pixel, and the normal agreement exponent.
#define VK_DIFFUSE_VOLUME_UPSAMPLE_DEPTH 0.05f
#define VK_DIFFUSE_VOLUME_UPSAMPLE_NORMAL_POWER 8.0f
// Total weight below which a pixel looks the volume up itself.
#define VK_DIFFUSE_VOLUME_UPSAMPLE_MIN_WEIGHT 0.05f

// The volume at full resolution: the bilinear half-resolution texels that
// match the pixel's surface, weighted by distance and normal agreement. A
// pixel none matches, at a silhouette or on thin geometry, joins the
// fallback list, which DiffuseVolume.Fallback looks up.
[shader("compute")]
[numthreads(8, 8, 1)]
void vk_diffuse_volume_upsample(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkDiffuseVolumeSampleRoot root =
        vkr_vk_uniform_root(*g_diffuse_volume_sample_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    float4 result = 0.0f;
    if (g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy].x != 0u)
    {
        VkrVulkanPacketFrameRoot *frame = root.frame;
        VkDiffuseVolumePixel surface =
            vk_diffuse_volume_pixel(root, thread_id.xy);
        float guide_distance = vk_diffuse_volume_guide_distance(frame, surface);
        // Half texel i is (mostly) full pixel 2i, so pixel x sits at x / 2.
        float2 position = float2(thread_id.xy) * 0.5f;
        int2 base = int2(floor(position));
        float2 fraction = position - float2(base);
        int2 last = int2(root.half_extent) - 1;
        float4 sum = 0.0f;
        float total = 0.0f;
        [unroll] for (uint tap = 0u; tap < 4u; ++tap)
        {
            int2 offset = int2(int(tap & 1u), int(tap >> 1u));
            int2 texel = clamp(base + offset, int2(0), last);
            float bilinear = (offset.x != 0 ? fraction.x : 1.0f - fraction.x) *
                             (offset.y != 0 ? fraction.y : 1.0f - fraction.y);
            if (bilinear <= 0.0f)
                continue;
            float4 sampled = g_storage_2d[root.half_texture][texel];
            float4 guide = g_storage_2d[root.guide_texture][texel];
            // A texel without the volume, or with the other composition,
            // does not count.
            if (sampled.a <= 0.0f || guide.w * guide_distance <= 0.0f)
                continue;
            float relative = abs(abs(guide.w) - abs(guide_distance)) /
                             max(abs(guide_distance), 1.0e-3f);
            float depth_weight =
                saturate(1.0f - relative / VK_DIFFUSE_VOLUME_UPSAMPLE_DEPTH);
            float normal_weight =
                pow(saturate(dot(surface.normal, guide.xyz)),
                    VK_DIFFUSE_VOLUME_UPSAMPLE_NORMAL_POWER);
            float weight = bilinear * depth_weight * normal_weight;
            sum += weight * sampled;
            total += weight;
        }
        if (total >= VK_DIFFUSE_VOLUME_UPSAMPLE_MIN_WEIGHT)
        {
            // Every counted texel's sky share is at least the minimum, so
            // their weighted mean is too.
            result = sum / total;
        }
        else
        {
            // Every pixel joins at most once, so the list, one entry per
            // viewport pixel, cannot overflow.
            uint index;
            InterlockedAdd(root.fallback_arguments[3], 1u, index);
            uint groups;
            InterlockedMax(root.fallback_arguments[0], index / 64u + 1u,
                           groups);
            root.fallback_pixels[index] = thread_id.x | (thread_id.y << 16u);
        }
    }
    g_storage_2d[root.output_texture][thread_id.xy] = result;
}

// The volume at each listed pixel. The last group's slots past the count
// hold stale pixels, whose lookup writes their own correct response again.
[shader("compute")]
[numthreads(64, 1, 1)]
void vk_diffuse_volume_fallback(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkDiffuseVolumeSampleRoot root =
        vkr_vk_uniform_root(*g_diffuse_volume_sample_push.root);
    // The list holds one entry per viewport pixel.
    if (thread_id.x >= root.extent.x * root.extent.y)
        return;
    uint packed = root.fallback_pixels[thread_id.x];
    uint2 pixel = uint2(packed & 0xffffu, packed >> 16u);
    if (any(pixel >= root.extent))
        return;
    float4 result = 0.0f;
    if (g_storage_uint2_2d[root.vbuffer_texture][pixel].x != 0u)
        result = vk_diffuse_volume_lookup(root, pixel,
                                          vk_diffuse_volume_pixel(root, pixel));
    g_storage_2d[root.output_texture][pixel] = result;
}

groupshared uint g_deferred_tile_layered;

// A pixel with no clearcoat, sheen or anisotropy G-buffer data and no BRDF
// extension shades identically in the base kernel, which compiles those
// layers and extensions out.
bool vk_deferred_pixel_layered(VkrVkLightingRoot root, uint2 pixel)
{
    if (any(pixel >= root.extent))
        return false;
    uint visible = g_storage_uint2_2d[root.vbuffer_texture][pixel].x;
    if (visible == 0u)
        return false;
    return ((root.layered_tiles & 2u) != 0u &&
            vkr_brdf_extension_active(packet_material_brdf_extension(
                root.frame.materials[root.visible_rows[visible - 1u]
                                         .material_index]))) ||
           (root.clearcoat_texture != 0u &&
            any(g_storage_2d[root.clearcoat_texture][pixel] != 0.0f)) ||
           (root.sheen_texture != 0u &&
            any(g_storage_2d[root.sheen_texture][pixel] != 0.0f)) ||
           (root.anisotropy_texture != 0u &&
            any(g_storage_2d[root.anisotropy_texture][pixel] != 0.0f));
}

// Both lighting kernels classify the same 8x8 group, so each tile is shaded
// by exactly one of them. Every invocation reaches both barriers.
bool vk_deferred_tile_layered(VkrVkLightingRoot root, uint2 pixel,
                              uint group_index)
{
    if (group_index == 0u)
        g_deferred_tile_layered = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (vk_deferred_pixel_layered(root, pixel))
        InterlockedOr(g_deferred_tile_layered, 1u);
    GroupMemoryBarrierWithGroupSync();
    return g_deferred_tile_layered != 0u;
}

// SMAA S2x's second sample takes its per-pixel lighting inputs (GTAO, the
// local shadow mask, the diffuse volume, baked lamps and their shadow) from
// the nearest pixel whose first sample lies on the same draw, since those
// inputs belong to each pixel's first-sample surface. The second sample at
// (0.25, 0.25) is nearest the left and upper pixels. Without such a pixel it
// keeps its own pixel's.
uint2 vk_msaa_side_pixel(VkrVkLightingRoot root, uint2 pixel, uint visible)
{
    const int2 offsets[5] = {int2(-1, 0), int2(0, -1), int2(-1, -1),
                             int2(1, 0), int2(0, 1)};
    for (uint i = 0u; i < 5u; ++i)
    {
        int2 neighbour = int2(pixel) + offsets[i];
        if (any(neighbour < 0) || any(neighbour >= int2(root.extent)))
            continue;
        if (g_storage_uint2_2d[root.side_vbuffer_texture][uint2(neighbour)]
                .x == visible)
            return uint2(neighbour);
    }
    return pixel;
}

// The sun's visibility: Shadow.SunRayResolve's traced value where its weight
// covers the pixel, the cascades beyond, blended across the ray range's end.
// The cascades are not sampled where the rays cover the pixel alone.
float vk_deferred_sun_shadow(VkrVulkanPacketFrameRoot *frame, float3 world,
                             float3 normal, float2 rotation, float2 sun_ray,
                             uint moments_texture, uint moments_sampler)
{
    if (sun_ray.y >= 1.0f)
        return sun_ray.x;
    float cascades = packet_directional_shadow(frame, world, normal, rotation,
                                               moments_texture,
                                               moments_sampler);
    return lerp(cascades, sun_ray.x, sun_ray.y);
}

// Layered selects the clearcoat, sheen and anisotropy paths at compile time;
// without them the kernel needs fewer registers.
void vk_deferred_shade<let Layered : bool>(uint3 thread_id)
{
    VkrVkLightingRoot root = vkr_vk_uniform_root(*g_lighting_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    // SMAA S2x's second sample shades only its edge pixels.
    if (root.edge_mask_texture != 0u &&
        g_storage_2d[root.edge_mask_texture][thread_id.xy].x == 0.0f)
        return;
    VkrVulkanPacketFrameRoot *frame = root.frame;
    if (root.subsurface_profile_count != 0u)
        g_storage_2d[root.subsurface_source_texture][thread_id.xy] = float4(0.0f);
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][thread_id.xy];
    uint2 side_pixel = root.side_vbuffer_texture != 0u
                           ? vk_msaa_side_pixel(root, thread_id.xy, encoded.x)
                           : thread_id.xy;
    if (encoded.x == 0u)
    {
        // The indirect-diffuse channel carries only surface environment
        // response, so background sky must not contaminate its comparison.
        float3 background = frame.render_mode == 9u || frame.render_mode == 12u
                                ? float3(0.0f, 0.0f, 0.0f)
                                : vkr_vk_deferred_sky(root, thread_id.xy);
        g_storage_2d[root.scene_texture][thread_id.xy] =
            float4(background, 1.0f);
        return;
    }
    float4 seed = g_storage_2d[root.scene_texture][thread_id.xy];
    if (frame.render_mode == 3u || frame.render_mode == 12u)
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
    float3 world = vkr_vk_deferred_pixel_world(root, thread_id.xy, depth);
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
        // The factor view shows the visibility lighting takes, the sun
        // rays' included.
        if (root.sun_ray_shadow_texture != 0u)
        {
            float2 sun_ray =
                g_storage_float2_2d[root.sun_ray_shadow_texture][side_pixel];
            shadow_sample.factor =
                lerp(shadow_sample.factor, sun_ray.x, sun_ray.y);
        }
        g_storage_2d[root.scene_texture][thread_id.xy] = float4(
            packet_shadow_debug_color(frame.shadow_debug_mode, shadow_sample),
            1.0f);
        return;
    }
    float3 view = packet_view_direction(frame, world);
    // The sun's receiver offset and bias and the environment's horizon fade
    // take the geometric normal; the shading normal would push the receiver
    // along normal-map relief.
    VkrVkDepthView depth_view = vkr_vk_lighting_depth_view(root);
    float3 geometric_normal = normal;
    if (frame.directional_direction_enabled.w > 0.5f ||
        (frame.flags & 2u) != 0u)
        geometric_normal = vkr_vk_depth_view_geometric_normal(
            depth_view, thread_id.xy, encoded.x, depth, world, normal, view);
    // The G-buffer already folded metallic into diffuse albedo and F0, so the
    // shared helpers take a zero metallic factor here.
    float4 anisotropy_packed = 0.0f;
    if (Layered && root.anisotropy_texture != 0u)
    {
        anisotropy_packed = g_storage_2d[root.anisotropy_texture][thread_id.xy];
    }
    VkrGgxMaterialEnergy energy = packet_prepare_gbuffer_brdf(
        frame, normal, view, roughness, f0, anisotropy_packed);
    if (!vkr_editor_neutral_lighting(frame.render_mode))
    {
        VkrVulkanMaterial material =
            frame.materials[root.visible_rows[encoded.x - 1u].material_index];
        // Only the layered kernel carries the extensions; the base kernel
        // shades the pixels whose extensions are all at identity.
        VkrBrdfExtension extension = Layered
            ? packet_material_brdf_extension(material)
            : vkr_brdf_extension_identity();
        if (Layered)
            energy = packet_apply_specular_extension(frame, energy, extension,
                                                     roughness, f0);
        energy = vkr_ggx_diffuse_transmission(
            energy, material.material_diffuse_transmission);
        if (Layered)
            energy = vkr_brdf_apply_diffuse_extension(energy, extension,
                                                      diffuse_albedo);
    }
    float4 clearcoat_packed = 0.0f;
    if (Layered && root.clearcoat_texture != 0u)
    {
        clearcoat_packed = g_storage_2d[root.clearcoat_texture][thread_id.xy];
    }
    bool clearcoat_active =
        Layered && vkr_clearcoat_active(clearcoat_packed.x);
    VkrClearcoatLayer clearcoat = (VkrClearcoatLayer)0;
    if (clearcoat_active)
        clearcoat = packet_prepare_clearcoat(
            frame, clearcoat_packed.x, clearcoat_packed.y,
            vkr_vk_octahedral_decode(clearcoat_packed.zw), view);
    float4 sheen_packed = 0.0f;
    if (Layered && root.sheen_texture != 0u)
    {
        sheen_packed = g_storage_2d[root.sheen_texture][thread_id.xy];
    }
    bool sheen_active = Layered && vkr_sheen_active(sheen_packed.rgb);
    VkrSheenLayer sheen = (VkrSheenLayer)0;
    VkrSheenLtcSample sheen_ltc = (VkrSheenLtcSample)0;
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
        float cloud_shadow = packet_cloud_shadow(frame, world);
        // The kernel turns with the temporal phase, which the resolve
        // averages; without a temporal resolve it stays fixed.
        float2 shadow_rotation =
            vkr_shadow_kernel_rotation(thread_id.xy, root.sun_noise_index);
        // Without ray-query sun shadows the weight is zero: the cascades
        // alone.
        float2 sun_ray = root.sun_ray_shadow_texture != 0u
                             ? g_storage_float2_2d[root.sun_ray_shadow_texture]
                                                  [side_pixel]
                             : float2(1.0f, 0.0f);
        float3 sun_normal = back_lit ? -geometric_normal : geometric_normal;
        float base_shadow =
            vk_deferred_sun_shadow(frame, world, sun_normal, shadow_rotation,
                                   sun_ray, root.shadow_moments_texture,
                                   root.shadow_moments_sampler) *
            cloud_shadow;
        float directional_shadow = base_shadow;
        if (back_lit && clearcoat_active)
            directional_shadow =
                vk_deferred_sun_shadow(frame, world, geometric_normal,
                                       shadow_rotation, sun_ray,
                                       root.shadow_moments_texture,
                                       root.shadow_moments_sampler) *
                cloud_shadow;
        // Sun contact shadows (shadow_kernel.slangh), where the rays do
        // not cover the pixel: the rays find contacts exactly. As on the
        // tiled pipeline, the march starts along the sun, its length grows
        // with camera distance and its strength fades out by
        // VKR_SUN_CONTACT_DISTANCE.
        if (root.sun_contact != 0u && sun_ray.y < 1.0f && base_shadow > 0.0f)
        {
            float camera_distance = distance(world, frame.view_position.xyz);
            float strength = vkr_sun_contact_strength(camera_distance) *
                             (1.0f - sun_ray.y);
            if (strength > 0.0f)
            {
                float3 origin =
                    world + sun_direction *
                                vkr_local_shadow_contact_start_offset(
                                    camera_distance);
                float contact = vk_contact_shadow(
                    frame, depth_view, origin, sun_direction,
                    vkr_sun_contact_length(camera_distance),
                    VKR_SUN_CONTACT_STEPS,
                    vkr_local_shadow_contact_noise(thread_id.xy,
                                                   root.sun_noise_index));
                contact = 1.0f - strength * (1.0f - contact);
                base_shadow *= contact;
                directional_shadow *= contact;
            }
        }
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
    VkrVulkanDirectResult punctual = packet_punctual_layered<true>(
        frame, world, normal, view, diffuse_albedo, 0.0f, roughness, f0, energy,
        true, clearcoat_active, clearcoat, sheen_active, sheen,
        sheen_ltc.energy_normalization, clearcoat_direct, sheen_direct,
        root.local_shadow_mask_texture, side_pixel, root.light_contribution);
    diffuse_irradiance += punctual.irradiance;
    analytic_diffuse += punctual.diffuse;
    analytic_specular += punctual.specular;
    VkrVulkanDirectResult rectangles = packet_layered_rectangle_lights(
        frame, world, normal, view, diffuse_albedo, 0.0f, roughness, f0,
        energy, true, clearcoat_active, clearcoat, sheen_active, sheen,
        sheen_ltc, clearcoat_direct, sheen_direct);
    diffuse_irradiance += rectangles.irradiance;
    analytic_diffuse += rectangles.diffuse;
    analytic_specular += rectangles.specular;
    // Baked static lamps (ADR-104): the lightmap holds
    // their direct light, shadows and bounce as irradiance, so it shades as
    // diffuse light without GTAO, as on the tiled pipeline; the bake holds
    // their bounce.
    // The lamps' irradiance also joins the surroundings a covered pixel's
    // environment reflection takes (ADR-054).
    float3 baked_lamp_irradiance = 0.0f;
    if (root.baked_lamps_texture != 0u)
    {
        float4 baked = g_storage_2d[root.baked_lamps_texture][side_pixel];
        float4 dominant = g_storage_2d[root.baked_direction_texture][side_pixel];
        // The selected baked lamps' moving casters hide part of their baked
        // light (Shadow.LocalMask); it leaves before the split.
        float3 irradiance = vk_baked_lamp_irradiance(
            frame, baked, root.baked_lamp_shadow_texture, side_pixel);
        baked_lamp_irradiance = irradiance;
        // The undirected share shades as diffuse light; the directed share
        // as a light from the dominant direction, at the irradiance a
        // surface facing it would take, so normal maps modulate it and the
        // material's BRDF gives its highlight.
        float directionality = dominant.a;
        float3 undirected = irradiance * ((1.0f - directionality) / 3.14159265f);
        diffuse_irradiance += undirected;
        analytic_diffuse += energy.diffuse_weight * diffuse_albedo * undirected;
        if (directionality > 0.0f)
        {
            VkrVulkanDirectResult directed = packet_direct(
                normal, view, normalize(dominant.rgb * 2.0f - 1.0f),
                irradiance * directionality * baked.a, diffuse_albedo, 0.0f,
                roughness, f0, energy);
            diffuse_irradiance += directed.irradiance;
            analytic_diffuse += directed.diffuse;
            analytic_specular += directed.specular;
        }
    }
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
    float2 gtao_uv = (float2(side_pixel) + 0.5f) / float2(root.extent);
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
    // DiffuseVolume.Sample looked the volume up for this pixel; a covered
    // pixel's volume light replaces only diffuse indirect light.
    float3 volume_irradiance = 0.0f;
    float3 volume_diffuse = 0.0f;
    bool volume_valid = false;
    // The volume light, with a lightmapped pixel's baked lamps, also scales
    // the environment reflections (ADR-054).
    VkrVulkanBakedLight baked = packet_no_baked_light();
    if (root.diffuse_volume_texture != 0u)
    {
        baked = vk_diffuse_volume_baked_light(
            frame, g_storage_2d[root.diffuse_volume_texture][side_pixel]);
        volume_valid = baked.valid;
        volume_irradiance = baked.response * ao;
        volume_diffuse =
            energy.diffuse_weight * diffuse_albedo * ao * baked.response;
        baked = packet_baked_light_with_lamps(baked, baked_lamp_irradiance);
    }
    float3 indirect_irradiance;
    float3 indirect_diffuse;
    float3 indirect_specular = 0.0f;
    float3 clearcoat_indirect = 0.0f;
    float3 sheen_indirect = 0.0f;
    bool environment_enabled = (frame.flags & 2u) != 0u;
    if ((frame.flags & 2u) != 0u)
    {
        float3 horizon_normal = geometric_normal;
        // A mover's pixels take their probes at its lighting origin
        // (ADR-120); DiffuseVolume.Sample looked the volume up there.
        float3 environment_position = packet_lighting_position(
            frame, root.visible_rows[encoded.x - 1u].instance_index, world);
        if (volume_valid)
        {
            indirect_irradiance = volume_irradiance;
            indirect_diffuse = volume_diffuse;
            indirect_specular = packet_environment_specular(
                frame, environment_position, normal, horizon_normal, view,
                roughness, ao, energy, baked) * gtao_cone;
        }
        else
        {
            VkrVulkanEnvironmentResult environment =
                packet_environment_terms_with_diffuse_normal(
                    frame, environment_position, indirect_diffuse_normal,
                    normal, horizon_normal, view, diffuse_albedo, 0.0f,
                    roughness, ao, energy);
            indirect_irradiance = environment.irradiance;
            indirect_diffuse = environment.diffuse;
            indirect_specular = environment.specular * gtao_cone;
        }
        if (root.planar_reflection_texture != 0u)
            indirect_specular = vk_planar_reflection_specular(
                root, frame, encoded.x, thread_id.xy, world, normal,
                horizon_normal, view, roughness, ao, energy,
                indirect_specular, gtao_cone);
        if (clearcoat_active)
        {
            float clearcoat_gtao_cone = vkr_gtao_cone_specular(
                gtao_visibility,
                vkr_gtao_decode_bent_normal(gtao, clearcoat.normal),
                reflect(-view, clearcoat.normal), clearcoat.roughness);
            clearcoat_indirect = clearcoat.factor * packet_environment_specular(
                frame, environment_position, clearcoat.normal,
                clearcoat.normal, view, clearcoat.roughness, ao,
                clearcoat.energy, baked) *
                clearcoat_gtao_cone;
        }
        if (sheen_active)
        {
            float sheen_cone = vkr_gtao_cone_specular(
                gtao_visibility, bent_normal, reflect(-view, normal),
                sheen.roughness);
            sheen_indirect = packet_sheen_environment(
                frame, environment_position, normal, view, ao, sheen_cone,
                sheen, baked);
        }
    }
    else
    {
        indirect_irradiance = frame.ambient_color.rgb * ao;
        indirect_diffuse = frame.ambient_color.rgb * diffuse_albedo * ao *
                           energy.diffuse_weight;
    }
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

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_deferred_lighting(uint3 dispatch_id : SV_DispatchThreadID,
                          uint3 group_id : SV_GroupID,
                          uint group_index : SV_GroupIndex)
{
    VkrVkLightingRoot root = vkr_vk_uniform_root(*g_lighting_push.root);
    if (root.sample1_pixels != nullptr)
    {
        vk_deferred_shade<false>(
            vk_msaa_sample1_pixel(root.sample1_pixels, group_id, group_index));
        return;
    }
    uint3 thread_id = dispatch_id;
    if (root.layered_tiles != 0u &&
        vk_deferred_tile_layered(root, thread_id.xy, group_index))
        return;
    vk_deferred_shade<false>(thread_id);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_deferred_lighting_layered(uint3 thread_id : SV_DispatchThreadID,
                                  uint group_index : SV_GroupIndex)
{
    VkrVkLightingRoot root = vkr_vk_uniform_root(*g_lighting_push.root);
    if (!vk_deferred_tile_layered(root, thread_id.xy, group_index))
        return;
    vk_deferred_shade<true>(thread_id);
}

// Relative device-depth difference within which last frame's sample shows the
// same surface. Reversed depth is close to inverse linear depth, so this is a
// relative linear-depth tolerance at any distance.
#define VKR_TEMPORAL_DEPTH_TOLERANCE 0.02f

// Motion in pixels below which a sample counts as still. A still pixel's
// history is SMAA T2x's other subsample of the same content, which may land
// on another leaf, wire or silhouette side; the identity and depth checks
// apply only to moving pixels, where history can disocclude.
#define VKR_TEMPORAL_STILL_PIXELS 0.5f

// Whether last frame's raw samples around `history_uv` hold the current
// surface: one of the 3x3 texels around it has its identity and surface token
// and a device depth near `expected_depth`, the depth the surface had then.
// The neighbourhood accepts a silhouette or thin geometry that last frame's
// subsample missed at this pixel, whose two subsamples T2x averages, and
// rejects a surface that was hidden nearby, a disocclusion.
bool vkr_vk_temporal_history_matches(VkrVkTemporalResolveRoot root,
                                     float2 history_uv, uint2 identity,
                                     uint surface_token, float expected_depth,
                                     bool check_depth)
{
    int2 base = int2(floor(history_uv * float2(root.extent))) - 1;
    for (int y = 0; y <= 2; ++y)
    {
        for (int x = 0; x <= 2; ++x)
        {
            uint2 p = uint2(clamp(base + int2(x, y), int2(0),
                                  int2(root.extent) - 1));
            if (any(g_storage_uint2_2d[root.history_identity_texture][p] !=
                    identity) ||
                g_storage_uint_2d[root.history_surface_texture][p] !=
                    surface_token)
                continue;
            float previous_depth =
                g_storage_2d[root.history_depth_texture][p].x;
            if (!check_depth ||
                abs(previous_depth - expected_depth) <=
                VKR_TEMPORAL_DEPTH_TOLERANCE *
                    max(previous_depth, expected_depth))
                return true;
        }
    }
    return false;
}

// SMAA T2x resolves after the tonemap (ADR-037). This pass hands the jittered
// raster colour to post-processing and records, per pixel, how much of last
// frame's sample of the same surface the T2x resolve may blend in: zero where
// the surface's identity, surface or depth changed, on blended overlays
// without an identity, and off screen; one elsewhere. A blended overlay's
// depth is the opaque surface's behind it, so its match checks identity and
// surface only. Authored temporal reactivity shortened the long history this
// pass once kept; two frames ghost for at most one, so T2x ignores it, and a
// partial weight would make a still image alternate between its subsamples.
[shader("compute")]
[numthreads(8, 8, 1)]
void vk_temporal_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkTemporalResolveRoot root =
        vkr_vk_uniform_root(*g_temporal_resolve_push.root);
    if (any(thread_id.xy >= root.extent))
        return;

    uint2 pixel = thread_id.xy;
    float4 current = g_textures[root.scene_texture].Load(int3(pixel, 0));
    float surface_depth =
        g_depth_textures[root.depth_texture].Load(int3(pixel, 0));
    uint2 encoded = g_storage_uint2_2d[root.vbuffer_texture][pixel];
    const uint overlay_bit = 0x80000000u;
    const uint generation_mask = 0x7fffffffu;
    const uint surface_mask = 0x1ffffu;

    float2 motion = g_textures[root.motion_texture].Load(int3(pixel, 0)).xy;
    float2 validity = g_textures[root.validity_texture].Load(int3(pixel, 0)).xy;
    bool blend_overlay = (encoded.x & overlay_bit) != 0u;
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

    float2 uv = (float2(pixel) + 0.5f) / float2(root.extent);
    float2 history_uv = uv + motion;
    bool still = length(motion * float2(root.extent)) < VKR_TEMPORAL_STILL_PIXELS;
    bool accepted =
        root.history_valid != 0u && validity.x > 0.5f &&
        !(blend_overlay && identity.x == 0u) &&
        (identity.x == 0u || surface_token != 0u) &&
        all(history_uv >= 0.0f) && all(history_uv <= 1.0f) &&
        (still || vkr_vk_temporal_history_matches(root, history_uv, identity,
                                                  surface_token, validity.y,
                                                  !blend_overlay));
    float accept = accepted ? 1.0f : 0.0f;

    if (root.render_mode == 7u)
        current = float4(saturate(abs(motion) * float2(root.extent) / 16.0f),
                         validity.x, 1.0f);
    else if (root.render_mode == 8u)
        current = float4(1.0f - accept, accept, 0.0f, 1.0f);

    g_storage_2d[root.output_color_texture][pixel] = current;
    g_storage_2d[root.output_depth_texture][pixel] =
        float4(surface_depth, 0.0f, 0.0f, 0.0f);
    g_storage_uint2_2d[root.output_identity_texture][pixel] = identity;
    g_storage_uint_2d[root.output_surface_texture][pixel] = surface_token;
    g_storage_2d[root.output_accept_texture][pixel] =
        float4(accept, 0.0f, 0.0f, 0.0f);
}

// Each moments texel takes the 4x4 depth texels around its 2x2 footprint with
// (1, 3, 3, 1) tent weights per axis, clamped at the map edge, and stores
// their weighted EVSM moments.
[shader("compute")]
[numthreads(8, 8, 1)]
void vk_shadow_moments(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkShadowMomentsRoot root = vkr_vk_uniform_root(*g_shadow_moments_push.root);
    if (any(thread_id.xy >= uint2(root.moments_size, root.moments_size)))
        return;
    int2 origin = int2(thread_id.xy) * 2 - 1;
    int last = int(root.depth_size) - 1;
    float4 moments = 0.0f;
    for (uint y = 0u; y < 4u; ++y)
    {
        for (uint x = 0u; x < 4u; ++x)
        {
            int2 texel = clamp(origin + int2(int(x), int(y)), 0, last);
            float depth = g_depth_array_textures[root.depth_texture].Load(
                int4(texel, int(root.depth_layer), 0));
            moments += vkr_shadow_evsm_moments(depth) *
                       (vkr_shadow_evsm_tent_weight(x) *
                        vkr_shadow_evsm_tent_weight(y));
        }
    }
    g_storage_images[root.moments_texture]
                    [uint3(thread_id.xy, root.moments_layer)] = moments;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_hzb_build(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkHzbRoot root = vkr_vk_uniform_root(*g_hzb_push.root);
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
    // Each cell keeps its farthest depth, the smallest under reversed camera
    // depth (ADR-109).
    float farthest = 1.0f;
    for (uint y = source_begin.y; y < source_end.y; ++y)
    {
        for (uint x = source_begin.x; x < source_end.x; ++x)
        {
            uint2 p = uint2(x, y);
            float value =
                root.source_is_depth != 0u
                    ? g_depth_textures[root.source_texture].Load(int3(p, 0))
                    : g_storage_2d[root.source_texture][p].x;
            farthest = min(farthest, value);
        }
    }
    g_storage_2d[root.destination_texture][thread_id.xy] =
        float4(farthest, 0.0f, 0.0f, 0.0f);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void vk_sdsm_reduce(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSdsmRoot root = vkr_vk_uniform_root(*g_sdsm_push.root);
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
    VkrVkPickingRoot root = vkr_vk_uniform_root(*g_picking_push.root);
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
    // The editor lifts its grid onto the visible floor from this depth when
    // the scene has no collision; reversed depth 0 is the far plane.
    root.depth_output[0].depth =
        g_depth_textures[root.depth_texture].Load(int3(root.pixel, 0));
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
    VkrVkTransmissionCompactRoot root = vkr_vk_uniform_root(*g_transmission_compact_push.root);
    root.indirect_arguments[6] = 0u;
    root.indirect_arguments[7] = 0u;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_transmission_compact(uint3 thread_id : SV_DispatchThreadID,
                             uint group_index : SV_GroupIndex)
{
    VkrVkTransmissionCompactRoot root = vkr_vk_uniform_root(*g_transmission_compact_push.root);
    bool in_extent = all(thread_id.xy < root.extent);
    if (vkr_transmission_compact_copies_source(root.layer) && in_extent)
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
    VkrVkTransmissionCompactRoot root = vkr_vk_uniform_root(*g_transmission_compact_push.root);
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
    VkrVkTransmissionRoot root = vkr_vk_uniform_root(*g_transmission_push.root);
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
        root.instance_count, root.frame.render_mode, root.frame.pre_exposure,
        nullptr, 0u, root.lightmap_rect_count,
    };
    VkrVkSurface surface;
    if (!vkr_vk_resolve_surface(source, encoded, float2(pixel) + 0.5f,
                                surface))
    {
        g_storage_2d[root.output_texture][pixel] =
            g_textures[root.feedback_texture].SampleLevel(
                g_samplers[root.feedback_sampler], screen_uv, 0.0f);
        return;
    }
    if (diagnostic_enabled && root.frame.render_mode == 12u)
    {
        g_storage_2d[root.output_texture][pixel] = float4(surface.wire_color, 1.0f);
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
    bool neutral_lighting = vkr_editor_neutral_lighting(frame.render_mode);
    VkrVulkanMaterial material = root.materials[surface.material_index];
    if (temporal_outputs && root.history_valid != 0u &&
        instance.temporal_index < VKR_TEMPORAL_TRANSFORM_CAPACITY)
    {
        VkrTemporalTransform previous =
            root.previous_transforms[instance.temporal_index];
        if ((instance.deformation_address == 0 ||
         instance.previous_deformation_address != 0) &&
            previous.valid != 0u &&
            previous.generation == instance.temporal_generation &&
            previous.frame_index == root.previous_frame_index)
        {
            float4 current_clip = mul(
                root.current_view_projection,
                mul(instance.model, float4(surface.object_position, 1.0f)));
            float4 previous_clip = mul(
                root.previous_view_projection,
                mul(previous.model, float4(surface.previous_object_position, 1.0f)));
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
    // A mover's surfaces take baked light and probes at its lighting origin
    // (ADR-120).
    float3 lighting_position = instance.lighting_origin.w != 0.0f
                                   ? instance.lighting_origin.xyz
                                   : world;
    float3 f0 = lerp(neutral_lighting ? float3(0.04f)
                     : saturate(material.material_dielectric_specular.rgb),
                     surface.base.rgb, surface.metallic);
    float3 view = packet_view_direction(frame, world);
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
         frame.render_mode != 6u && frame.render_mode != 9u &&
         !neutral_lighting))
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
                world, packet_is_orthographic(frame)
                           ? world + view : frame.view_position.xyz,
                surface.normal,
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
  float anisotropy_strength =
      neutral_lighting ? 0.0f : material.material_anisotropy.x;
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
        VkrBrdfExtension extension =
            neutral_lighting ? vkr_brdf_extension_identity()
                             : packet_material_brdf_extension(material);
        energy = packet_apply_specular_extension(frame, energy, extension,
                                                 surface.roughness, f0);
        energy = vkr_brdf_apply_diffuse_extension(
            energy, extension, surface.base.rgb * (1.0f - surface.metallic));
        float clearcoat_factor =
            neutral_lighting ? 0.0f : saturate(material.material_clearcoat.x);
        if (clearcoat_factor > 0.0f && (material.flags & 32u) != 0u)
            clearcoat_factor *= g_textures[NonUniformResourceIndex(
                                    material.clearcoat_texture)]
                                    .SampleGrad(g_samplers[NonUniformResourceIndex(
                                        material.clearcoat_sampler)],
                                                surface.uv, surface.uv_dx,
                                                surface.uv_dy)
                                    .r;
        bool clearcoat_active = vkr_clearcoat_active(clearcoat_factor);
        VkrClearcoatLayer clearcoat = (VkrClearcoatLayer)0;
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
        float3 sheen_color =
            neutral_lighting ? float3(0.0f) : max(material.material_sheen.rgb, 0.0f);
        if (vkr_sheen_active(sheen_color) && (material.flags & 256u) != 0u)
            sheen_color *= g_textures[NonUniformResourceIndex(
                                material.sheen_color_texture)]
                               .SampleGrad(g_samplers[NonUniformResourceIndex(
                                               material.sheen_color_sampler)],
                                           surface.uv, surface.uv_dx,
                                           surface.uv_dy)
                               .rgb;
        bool sheen_active = vkr_sheen_active(sheen_color);
        VkrSheenLayer sheen = (VkrSheenLayer)0;
        VkrSheenLtcSample sheen_ltc = (VkrSheenLtcSample)0;
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
                packet_directional_shadow(
                    frame, world, surface.geometric_normal,
                    vkr_shadow_kernel_rotation(pixel, 0u)) *
                packet_cloud_shadow(frame, world);
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
        // Each light list is traversed once for the base, coat and sheen
        // lobes; a fully transmissive base contributes specular only.
        VkrVulkanDirectResult punctual = packet_punctual_layered<false>(
            frame, world, surface.normal, view, surface.base.rgb,
            surface.metallic, surface.roughness, f0, energy, diffuse_enabled,
            clearcoat_active, clearcoat, sheen_active, sheen,
            sheen_ltc.energy_normalization, clearcoat_direct, sheen_direct,
            0u, uint2(0u, 0u), nullptr);
        analytic_diffuse += punctual.diffuse;
        analytic_specular += punctual.specular;
        VkrVulkanDirectResult rectangles = packet_layered_rectangle_lights(
            frame, world, surface.normal, view, surface.base.rgb,
            surface.metallic, surface.roughness, f0, energy, diffuse_enabled,
            clearcoat_active, clearcoat, sheen_active, sheen, sheen_ltc,
            clearcoat_direct, sheen_direct);
        analytic_diffuse += rectangles.diffuse;
        analytic_specular += rectangles.specular;
        // Baked static lamps on a lightmapped transmissive draw, shaded as
        // deferred lighting shades them: the undirected share as diffuse
        // light, the directed share as a light from the dominant direction.
        // Their irradiance also joins the surroundings a covered surface's
        // environment reflection takes (ADR-054).
        bool lamps_baked = false;
        float3 baked_lamp_irradiance = 0.0f;
        if (root.lightmap_rect_count != 0u)
        {
            VkrVkLightmapSource lightmap = {
                root.lightmap_rects, root.lightmap_texture,
                root.lightmap_sampler, root.lightmap_rect_count,
                root.lightmap_layer_count, root.lightmap_active_count,
                root.lightmap_inverse_page_size,
                root.lightmap_direction_texture, root.lightmap_active_layers,
                root.lightmap_active_weights,
            };
            float3 surface_normal = normalize(vkr_instance_transform_normal(
                instance.normal_column0.xyz, instance.normal_column1.xyz,
                instance.normal_column2.xyz, surface.object_normal));
            VkrVkBakedLamps baked =
                vkr_vk_baked_lamps(lightmap, instance, surface, surface_normal);
            lamps_baked = baked.irradiance.a > 0.0f;
            if (lamps_baked)
            {
                float3 irradiance = baked.irradiance.rgb * frame.pre_exposure;
                baked_lamp_irradiance = irradiance;
                float directionality = baked.direction.a;
                float3 undirected =
                    irradiance * ((1.0f - directionality) / 3.14159265f);
                if (diffuse_enabled)
                    analytic_diffuse += energy.diffuse_weight *
                                        (1.0f - surface.metallic) *
                                        surface.base.rgb * undirected;
                if (directionality > 0.0f)
                {
                    float3 light = normalize(baked.direction.rgb * 2.0f - 1.0f);
                    float3 radiance =
                        irradiance * directionality * baked.irradiance.a;
                    if (diffuse_enabled)
                    {
                        VkrVulkanDirectResult direct = packet_direct(
                            surface.normal, view, light, radiance,
                            surface.base.rgb, surface.metallic,
                            surface.roughness, f0, energy);
                        analytic_diffuse += direct.diffuse;
                        analytic_specular += direct.specular;
                    }
                    else
                    {
                        analytic_specular += packet_direct_specular(
                            surface.normal, view, light, radiance,
                            surface.roughness, f0, energy);
                    }
                }
            }
        }
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
                                frame, lighting_position, surface.normal,
                                surface.normal, view, surface.base.rgb,
                                surface.metallic,
                                surface.roughness, surface.occlusion, energy)
                                .diffuse;
                }
                float3 volume_diffuse;
                VkrVulkanBakedLight unused_baked;
                if (packet_diffuse_volume_terms(
                        frame, lighting_position, surface.normal, surface.base.rgb,
                        surface.metallic, surface.occlusion, !lamps_baked,
                        energy, volume_diffuse, unused_baked))
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
            bool environment_enabled = (frame.flags & 2u) != 0u;
            // A covered surface takes its diffuse light from the volume,
            // which also scales its environment reflections (ADR-054).
            float3 volume_diffuse = 0.0f;
            VkrVulkanBakedLight baked = packet_no_baked_light();
            bool volume_valid =
                (diffuse_enabled || environment_enabled) &&
                packet_diffuse_volume_terms(
                    frame, lighting_position, surface.normal, surface.base.rgb,
                    surface.metallic, surface.occlusion, !lamps_baked,
                    energy, volume_diffuse, baked);
            baked = packet_baked_light_with_lamps(baked, baked_lamp_irradiance);
            if (environment_enabled)
            {
                if (diffuse_enabled && !volume_valid)
                {
                    VkrVulkanEnvironmentResult environment =
                        packet_environment_terms(
                            frame, lighting_position, surface.normal,
                            surface.geometric_normal, view, surface.base.rgb,
                            surface.metallic, surface.roughness,
                            surface.occlusion, energy);
                    diffuse += environment.diffuse;
                    specular += environment.specular;
                }
                else
                {
                    specular += packet_environment_specular(
                        frame, lighting_position, surface.normal,
                        surface.geometric_normal, view, surface.roughness,
                        surface.occlusion, energy, baked);
                }
            }
            else if (diffuse_enabled && !volume_valid)
            {
                diffuse += frame.ambient_color.rgb * surface.base.rgb *
                           surface.occlusion * (1.0f - surface.metallic) *
                           energy.diffuse_weight;
            }
            if (diffuse_enabled && volume_valid)
                diffuse += volume_diffuse;
            if (clearcoat_active && environment_enabled)
                clearcoat_indirect = clearcoat.factor *
                    packet_environment_specular(
                    frame, lighting_position, clearcoat.normal,
                    clearcoat.normal, view, clearcoat.roughness,
                    surface.occlusion, clearcoat.energy, baked);
            if (sheen_active && environment_enabled)
                sheen_indirect = packet_sheen_environment(
                    frame, lighting_position, surface.normal, view,
                    surface.occlusion, 1.0f, sheen, baked);
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
            VkrVulkanSky *sky = frame.sky;
            bool aerial = sky.params.aerial.w > 0.0f;
            bool froxel = vkr_froxel_enabled(froxel_params);
            bool fog = frame.fog[0].color_density.w > 0.0f;
            if (aerial || froxel || fog)
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
                // Aerial perspective is the farther medium, so it applies
                // before fog.
                if (aerial)
                {
                    VkrVkAerialSample aerial_sample = packet_aerial_sample(
                        sky, sky.aerial_perspective_texture, world);
                    local = vkr_sky_apply_aerial_over_feedback(
                        local, feedback_weight, aerial_sample.packed,
                        aerial_sample.weight);
                }
                if (froxel)
                {
                    VkrFroxelSample froxel_sample =
                        packet_froxel_sample(frame, world);
                    local = vkr_froxel_apply_local_over_feedback(
                        local, feedback_weight, froxel_sample);
                }
                else if (fog)
                {
                    float3 inscatter = vkr_fog_inscatter(
                        frame.fog[0], packet_fog_lighting(frame),
                        vkr_fog_direction(world - frame.view_position.xyz));
                    local = vkr_fog_apply_local_over_feedback(
                        local, feedback_weight, inscatter,
                        vkr_fog_surface_sample(frame.fog[0],
                                               frame.view_position.xyz,
                                               world));
                }
                color = local + feedback;
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
    VkrVkTransmissionRoot root = vkr_vk_uniform_root(*g_transmission_push.root);
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
    VkrVkTransmissionCoverageRoot root = vkr_vk_uniform_root(*g_transmission_coverage_push.root);
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

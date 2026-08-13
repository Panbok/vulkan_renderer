#include "tonemap.slangh"

struct VkrBindlessVertex
{
    float3 position;
    float3 normal;
    float2 texcoord;
    float4 color;
    float4 tangent;
};

struct VkrBindlessInstance
{
    float4x4 model;
    uint object_id;
    uint3 reserved;
};

struct VkrBindlessMaterial
{
    float4 tint;
    uint base_color_texture;
    uint normal_texture;
    uint orm_texture;
    uint emissive_texture;
    uint base_color_sampler;
    uint normal_sampler;
    uint orm_sampler;
    uint emissive_sampler;
    uint material_id;
    uint flags;
    uint alpha_mode;
    uint reserved;
    float4 material_emissive;
    float4 material_dielectric_specular;
    float4 material_surface;
    float4 material_alpha;
    float4 material_attenuation_color;
};

struct VkrBindlessShadowCascade
{
    float4x4 light_view_projection;
    float4 split_depth;
};

struct VkrBindlessIblProbe
{
    uint irradiance_texture;
    uint irradiance_sampler;
    uint prefilter_texture;
    uint prefilter_sampler;
    float4 center_blend;
    float4 extents_weight;
    float4 intensity_box;
};

struct VkrBindlessPacketFrameRoot
{
    VkrBindlessInstance* instances;
    uint2 instance_address_padding;
    float4x4 view_projection;
    VkrBindlessMaterial* materials;
    uint irradiance_texture;
    uint irradiance_sampler;
    uint prefilter_texture;
    uint prefilter_sampler;
    // Retired; the environment BRDF is analytic. Explicit padding.
    uint reserved_brdf_texture;
    uint reserved_brdf_sampler;
    uint shadow_texture;
    uint shadow_sampler;
    uint transmission_texture;
    uint transmission_sampler;
    float4 view_position;
    uint prefilter_mip_count;
    uint flags;
    uint2 reserved_0;
    float4 ibl_controls;
    float4 directional_direction_enabled;
    float4 directional_color_intensity;
    float4 ambient_color;
    uint render_mode;
    uint3 reserved_1;
    float4* point_light_data;
    uint4* point_light_masks;
    float4 point_light_grid_origin_cell_size;
    uint4 point_light_grid_dimensions_count;
    uint4 point_light_global_mask;
    uint point_light_count;
    uint3 point_light_reserved;
    VkrBindlessShadowCascade* shadow_cascades;
    uint2 shadow_address_padding;
    float4x4 view;
    uint shadow_cascade_count;
    float shadow_bias;
    uint2 shadow_reserved;
    VkrBindlessIblProbe* ibl_probes;
    uint ibl_probe_count;
    uint ibl_probe_reserved;
};

struct VkrGpuGeometryRow
{
    uint64_t vertex_address;
    uint64_t index_address;
    uint first_vertex;
    uint first_index;
    uint vertex_stride;
    uint vertex_layout;
    uint publication_generation;
    uint flags;
    uint2 reserved;
};

struct VkrGpuCandidateDrawRow
{
    uint geometry_index;
    uint material_index;
    uint instance_index;
    uint first_index;
    uint index_count;
    int vertex_offset;
    uint state_bucket;
    uint flags;
    float4 local_bounding_sphere;
};

struct VkrGpuVisibleDrawRow
{
    uint geometry_index;
    uint material_index;
    uint instance_index;
    uint first_index;
    uint index_count;
    int vertex_offset;
    uint state_bucket;
    uint flags;
};

struct VkrBindlessPacketDrawRoot
{
    VkrGpuGeometryRow* geometry_rows;
    VkrGpuVisibleDrawRow* visible_rows;
    VkrBindlessVertex* vertices;
    VkrBindlessPacketFrameRoot* frame;
    uint visible_row_index;
    uint flags;
    uint2 reserved;
};

struct VkrBindlessPacketUtilityRoot
{
    VkrBindlessVertex* vertices;
    VkrBindlessInstance* instances;
    float4x4 view_projection;
    VkrBindlessMaterial* materials;
    uint irradiance_texture;
    uint irradiance_sampler;
    uint prefilter_texture;
    uint prefilter_sampler;
    uint reserved_brdf_texture;
    uint reserved_brdf_sampler;
    uint shadow_texture;
    uint shadow_sampler;
    uint transmission_texture;
    uint transmission_sampler;
    float4 view_position;
    uint material_index;
    uint first_instance;
    uint prefilter_mip_count;
    uint flags;
    float4 material_emissive;
    float4 material_dielectric_specular;
    float4 material_surface;
    float4 material_alpha;
    float4 material_attenuation_color;
    float4 ibl_controls;
    float4 directional_direction_enabled;
    float4 directional_color_intensity;
    float4 ambient_color;
    uint render_mode;
    uint alpha_mode;
    uint material_flags;
    uint reserved_0;
    float4* point_light_data;
    uint4* point_light_masks;
    float4 point_light_grid_origin_cell_size;
    uint4 point_light_grid_dimensions_count;
    uint4 point_light_global_mask;
    uint point_light_count;
    uint3 point_light_reserved;
    VkrBindlessShadowCascade* shadow_cascades;
    uint2 shadow_address_padding;
    float4x4 view;
    uint shadow_cascade_count;
    float shadow_bias;
    uint2 shadow_reserved;
    VkrBindlessIblProbe* ibl_probes;
    uint ibl_probe_count;
    uint ibl_probe_reserved;
};

struct VkrBindlessPushConstants
{
    VkrBindlessPacketDrawRoot* root;
    uint material_index;
    uint flags;
};

struct VkrBindlessIblRoot
{
    uint source_texture;
    uint source_sampler;
    uint target_texture;
    uint target_size;
    uint sample_count;
    uint source_face_size;
    uint source_mip_count;
    float roughness;
};

struct VkrBindlessVertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 world_position : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    nointerpolation uint object_id : TEXCOORD3;
    float4 world_tangent : TEXCOORD4;
};

struct VkrBindlessTextVertex
{
    float2 position;
    float2 texcoord;
    float4 color;
};

struct VkrBindlessTextOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint object_id : TEXCOORD1;
};

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(0, 0)]] TextureCube<float4> g_cube_textures[];
[[vk::binding(0, 0)]] Texture2DArray<float4> g_array_textures[];
[[vk::binding(0, 1)]] SamplerState g_samplers[];
[[vk::binding(1, 0)]] RWTexture2DArray<float4> g_storage_images[];
[[vk::push_constant]] ConstantBuffer<VkrBindlessPushConstants> g_push;

static const float vkr_pi = 3.14159265359f;

float radical_inverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 hammersley(uint index, uint count)
{
    return float2(float(index) / float(max(count, 1u)), radical_inverse(index));
}

float3 cube_direction(uint face, float2 uv)
{
    float2 st = uv * 2.0f - 1.0f;
    switch (face)
    {
    case 0u: return float3(1.0f, -st.y, -st.x);
    case 1u: return float3(-1.0f, -st.y, st.x);
    case 2u: return float3(st.x, 1.0f, st.y);
    case 3u: return float3(st.x, -1.0f, -st.y);
    case 4u: return float3(st.x, -st.y, 1.0f);
    default: return float3(-st.x, -st.y, -1.0f);
    }
}

float3 tangent_sample(float3 normal, float3 sample_value)
{
    float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f)
                                             : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    return normalize(tangent * sample_value.x +
                     cross(normal, tangent) * sample_value.y +
                     normal * sample_value.z);
}

float3 importance_ggx(float2 xi, float3 normal, float roughness)
{
    float alpha = roughness * roughness;
    float phi = 2.0f * vkr_pi * xi.x;
    float cos_theta = sqrt((1.0f - xi.y) /
                           (1.0f + (alpha * alpha - 1.0f) * xi.y));
    float sin_theta = sqrt(max(1.0f - cos_theta * cos_theta, 0.0f));
    return tangent_sample(normal, float3(cos(phi) * sin_theta,
                                         sin(phi) * sin_theta, cos_theta));
}

float distribution_ggx(float no_h, float roughness)
{
    float alpha = max(roughness * roughness, 1e-6f);
    float alpha2 = alpha * alpha;
    float denominator = no_h * no_h * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / max(vkr_pi * denominator * denominator, 1e-6f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_equirect(uint3 position : SV_DispatchThreadID)
{
    VkrBindlessIblRoot* root = (VkrBindlessIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 face_uv =
        (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 direction = normalize(cube_direction(position.z, face_uv));
    float2 environment_uv = float2(
        atan2(direction.z, direction.x) * (0.5f / vkr_pi) + 0.5f,
        acos(clamp(direction.y, -1.0f, 1.0f)) / vkr_pi);
    uint source = NonUniformResourceIndex(root.source_texture);
    uint sampler_index = NonUniformResourceIndex(root.source_sampler);
    uint target = NonUniformResourceIndex(root.target_texture);
    g_storage_images[target][position] = g_textures[source].SampleLevel(
        g_samplers[sampler_index], environment_uv, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_irradiance(uint3 position : SV_DispatchThreadID)
{
    VkrBindlessIblRoot* root = (VkrBindlessIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 uv = (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 normal = normalize(cube_direction(position.z, uv));
    float3 irradiance = 0.0f;
    uint count = max(root.sample_count, 1u);
    uint source = NonUniformResourceIndex(root.source_texture);
    uint sampler_index = NonUniformResourceIndex(root.source_sampler);
    for (uint i = 0u; i < count; ++i)
    {
        float2 xi = hammersley(i, count);
        float phi = 2.0f * vkr_pi * xi.x;
        float cos_theta = sqrt(1.0f - xi.y);
        float sin_theta = sqrt(xi.y);
        float3 direction = tangent_sample(
            normal, float3(cos(phi) * sin_theta, sin(phi) * sin_theta,
                           cos_theta));
        irradiance += g_cube_textures[source].SampleLevel(
            g_samplers[sampler_index], direction, 0.0f).rgb;
    }
    uint target = NonUniformResourceIndex(root.target_texture);
    g_storage_images[target][position] =
        float4(irradiance / float(count), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ibl_prefilter(uint3 position : SV_DispatchThreadID)
{
    VkrBindlessIblRoot* root = (VkrBindlessIblRoot*)g_push.root;
    if (position.x >= root.target_size || position.y >= root.target_size ||
        position.z >= 6u)
        return;
    float2 uv = (float2(position.xy) + 0.5f) / float(root.target_size);
    float3 normal = normalize(cube_direction(position.z, uv));
    float3 view = normal;
    float3 color = 0.0f;
    float total_weight = 0.0f;
    uint count = max(root.sample_count, 1u);
    uint source = NonUniformResourceIndex(root.source_texture);
    uint sampler_index = NonUniformResourceIndex(root.source_sampler);
    for (uint i = 0u; i < count; ++i)
    {
        float3 half_vector = importance_ggx(hammersley(i, count), normal,
                                            root.roughness);
        float3 light = normalize(2.0f * dot(view, half_vector) * half_vector -
                                 view);
        float no_l = max(dot(normal, light), 0.0f);
        if (no_l <= 0.0f)
            continue;
        float no_h = max(dot(normal, half_vector), 1e-6f);
        float vo_h = max(dot(view, half_vector), 1e-6f);
        float pdf = distribution_ggx(no_h, root.roughness) * no_h /
                    max(4.0f * vo_h, 1e-6f);
        float sample_solid_angle = 1.0f / (float(count) * max(pdf, 1e-6f));
        float texel_solid_angle = 4.0f * vkr_pi /
            max(6.0f * float(root.source_face_size * root.source_face_size),
                1.0f);
        float lod = root.roughness <= 0.001f
                        ? 0.0f
                        : 0.5f * log2(4.0f * sample_solid_angle /
                                      texel_solid_angle);
        lod = clamp(lod, 0.0f, float(max(root.source_mip_count, 1u) - 1u));
        color += g_cube_textures[source].SampleLevel(
            g_samplers[sampler_index], light, lod).rgb * no_l;
        total_weight += no_l;
    }
    uint target = NonUniformResourceIndex(root.target_texture);
    g_storage_images[target][position] =
        float4(color / max(total_weight, 1e-4f), 1.0f);
}

VkrBindlessVertexOutput packet_vertex_impl(uint vertex_id, uint instance_id,
                                            float4x4 view_projection)
{
    VkrBindlessPacketDrawRoot* draw = g_push.root;
    VkrBindlessPacketFrameRoot* frame = draw.frame;
    VkrGpuVisibleDrawRow visible = draw.visible_rows[draw.visible_row_index];
    VkrGpuGeometryRow geometry = draw.geometry_rows[visible.geometry_index];
    VkrBindlessVertex vertex = draw.vertices[geometry.first_vertex + vertex_id];
    VkrBindlessInstance instance =
        frame.instances[visible.instance_index + instance_id];
    float4 world = mul(instance.model, float4(vertex.position, 1.0f));
    VkrBindlessVertexOutput output;
    output.position = mul(view_projection, world);
    output.uv = vertex.texcoord;
    output.color = vertex.color;
    output.world_position = world.xyz;
    output.world_normal = normalize(mul(instance.model,
                                        float4(vertex.normal, 0.0f)).xyz);
    output.world_tangent = float4(
        normalize(mul(instance.model, float4(vertex.tangent.xyz, 0.0f)).xyz),
        vertex.tangent.w);
    output.object_id = instance.object_id;
    return output;
}

[shader("vertex")]
VkrBindlessVertexOutput world_vertex(uint vertex_id : SV_VertexID,
                                     uint instance_id : SV_InstanceID)
{
    return packet_vertex_impl(vertex_id, instance_id,
                              g_push.root.frame.view_projection);
}

[shader("vertex")]
VkrBindlessVertexOutput shadow_vertex(uint vertex_id : SV_VertexID,
                                      uint instance_id : SV_InstanceID)
{
    return packet_vertex_impl(vertex_id, instance_id,
                              g_push.root.frame.view_projection);
}

float4 packet_base_color(VkrBindlessVertexOutput input)
{
    VkrGpuVisibleDrawRow visible =
        g_push.root.visible_rows[g_push.root.visible_row_index];
    VkrBindlessMaterial material =
        g_push.root.frame.materials[visible.material_index];
    uint texture_index = NonUniformResourceIndex(material.base_color_texture);
    uint sampler_index = NonUniformResourceIndex(material.base_color_sampler);
    return g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv)
           * material.tint * input.color;
}

struct VkrBindlessDirectResult
{
    float3 diffuse;
    float3 specular;
};

float3 packet_fresnel(float cosine, float3 f0)
{
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    return f0 + (f90 - f0) * pow(max(1.0f - cosine, 0.0f), 5.0f);
}

float3 packet_fresnel_roughness(float cosine, float3 f0, float roughness)
{
    float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
    float3 rough_f90 = f90 * (1.0f - roughness);
    return f0 + (max(rough_f90, f0) - f0) *
                    pow(max(1.0f - cosine, 0.0f), 5.0f);
}

float packet_geometry_schlick(float no_x, float roughness)
{
    float r = roughness + 1.0f;
    float k = r * r * 0.125f;
    return no_x / max(no_x * (1.0f - k) + k, 1e-6f);
}

VkrBindlessDirectResult packet_direct(float3 normal, float3 view, float3 light,
                                      float3 radiance, float3 albedo,
                                      float metallic, float roughness,
                                      float3 f0)
{
    float3 half_vector = normalize(view + light);
    float no_l = max(dot(normal, light), 0.0f);
    float no_v = max(dot(normal, view), 0.0f);
    float3 fresnel = packet_fresnel(max(dot(half_vector, view), 0.0f), f0);
    float distribution = distribution_ggx(max(dot(normal, half_vector), 0.0f),
                                          roughness);
    float geometry = packet_geometry_schlick(no_v, roughness) *
                     packet_geometry_schlick(no_l, roughness);
    float3 specular = distribution * geometry * fresnel /
                      max(4.0f * no_v * no_l, 1e-6f);
    float3 kd = (1.0f - fresnel) * (1.0f - metallic);
    float3 scale = radiance * no_l;
    VkrBindlessDirectResult result;
    result.diffuse = kd * albedo / vkr_pi * scale;
    result.specular = specular * scale;
    return result;
}

float packet_specular_ao(float ao, float no_v, float roughness)
{
    return saturate(pow(no_v + ao, exp2(-16.0f * roughness - 1.0f)) -
                    1.0f + ao);
}

// Epic's split-sum fit avoids a per-frame dependency on a mutable BRDF LUT.
float2 packet_brdf_approximation(float no_v, float roughness)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * no_v)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

uint4 packet_point_light_mask(VkrBindlessPacketFrameRoot* root,
                              float3 world_position)
{
    uint4 result = root.point_light_global_mask;
    uint4 dimensions = root.point_light_grid_dimensions_count;
    float cell_size = root.point_light_grid_origin_cell_size.w;
    if (dimensions.w == 0u || cell_size <= 0.0f)
        return result;
    int3 cell = int3(floor((world_position -
                            root.point_light_grid_origin_cell_size.xyz) /
                           cell_size));
    if (any(cell < 0) || cell.x >= int(dimensions.x) ||
        cell.y >= int(dimensions.y) || cell.z >= int(dimensions.z))
        return result;
    uint index = uint(cell.x) + dimensions.x *
                 (uint(cell.y) + dimensions.y * uint(cell.z));
    return result | root.point_light_masks[index];
}

float packet_directional_shadow(VkrBindlessPacketFrameRoot* root,
                                float3 world_position)
{
    if (root.shadow_cascade_count == 0u)
        return 1.0f;
    float view_depth = abs(mul(root.view, float4(world_position, 1.0f)).z);
    uint cascade = 0u;
    for (uint i = 0u; i + 1u < root.shadow_cascade_count; ++i)
    {
        if (view_depth > root.shadow_cascades[i].split_depth.x)
            cascade = i + 1u;
    }
    float4 clip = mul(root.shadow_cascades[cascade].light_view_projection,
                      float4(world_position, 1.0f));
    if (abs(clip.w) <= 1e-6f)
        return 1.0f;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || projected.z < 0.0f ||
        projected.z > 1.0f)
        return 1.0f;
    uint texture_index = NonUniformResourceIndex(root.shadow_texture);
    uint sampler_index = NonUniformResourceIndex(root.shadow_sampler);
    float shadow_depth = g_array_textures[texture_index].SampleLevel(
        g_samplers[sampler_index], float3(uv, float(cascade)), 0.0f).r;
    return projected.z - root.shadow_bias <= shadow_depth ? 1.0f : 0.0f;
}

float packet_probe_influence(VkrBindlessIblProbe probe,
                             float3 world_position)
{
    float weight = max(probe.extents_weight.w, 0.0f);
    if (weight <= 1e-6f || probe.intensity_box.w <= 0.5f)
        return weight;
    float3 outside = max(abs(world_position - probe.center_blend.xyz) -
                         max(probe.extents_weight.xyz, 0.0f), 0.0f);
    float distance = length(outside);
    if (distance <= 1e-6f)
        return weight;
    return probe.center_blend.w > 1e-6f
               ? weight * saturate(1.0f - distance / probe.center_blend.w)
               : 0.0f;
}

float3 packet_box_project(float3 direction, float3 world_position,
                          float3 center, float3 extents)
{
    if (any(extents <= 1e-4f))
        return normalize(direction);
    float3 safe_direction = direction;
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        if (abs(safe_direction[axis]) < 1e-4f)
            safe_direction[axis] = safe_direction[axis] < 0.0f
                                           ? -1e-4f
                                           : 1e-4f;
    }
    float3 t_min = (center - extents - world_position) / safe_direction;
    float3 t_max = (center + extents - world_position) / safe_direction;
    float3 t_far = max(t_min, t_max);
    float t_hit = max(min(t_far.x, min(t_far.y, t_far.z)), 0.0f);
    return normalize(world_position + safe_direction * t_hit - center);
}

[shader("fragment")]
float4 world_fragment(VkrBindlessVertexOutput input,
                      bool front_facing : SV_IsFrontFace) : SV_Target0
{
    VkrBindlessPacketDrawRoot* draw = g_push.root;
    VkrBindlessPacketFrameRoot* frame = draw.frame;
    VkrGpuVisibleDrawRow visible = draw.visible_rows[draw.visible_row_index];
    VkrBindlessMaterial material = frame.materials[visible.material_index];
    float4 base = packet_base_color(input);
    if (material.alpha_mode == 1u && base.a < material.material_alpha.x)
        discard;
    if ((frame.flags & 1u) == 0u)
        return base;

    float face_sign = front_facing ? 1.0f : -1.0f;
    float3 geometric_normal = normalize(input.world_normal) * face_sign;
    float3 normal = geometric_normal;
    if ((material.flags & 1u) != 0u)
    {
        uint texture_index = NonUniformResourceIndex(material.normal_texture);
        uint sampler_index = NonUniformResourceIndex(material.normal_sampler);
        float3 sampled = g_textures[texture_index].Sample(
            g_samplers[sampler_index], input.uv).xyz * 2.0f - 1.0f;
        sampled.xy *= material.material_surface.z;
        sampled.y = -sampled.y;
        float3 tangent = normalize(input.world_tangent.xyz);
        tangent = normalize(tangent - dot(tangent, normal) * normal);
        float3 bitangent = cross(normal, tangent) * input.world_tangent.w;
        float3x3 tangent_basis = float3x3(tangent, bitangent, normal);
        normal = normalize(mul(tangent_basis, sampled));
    }
    if (frame.render_mode == 2u)
        return float4(normal * 0.5f + 0.5f, 1.0f);

    float3 view = normalize(frame.view_position.xyz - input.world_position);
    float no_v = max(dot(normal, view), 0.0f);
    float metallic = saturate(material.material_surface.x);
    float roughness = clamp(material.material_surface.y, 0.04f, 1.0f);
    float ao = saturate(material.material_surface.w);
    float3 emissive = material.material_emissive.rgb;
    if ((material.flags & 2u) != 0u)
    {
        uint texture_index = NonUniformResourceIndex(material.orm_texture);
        uint sampler_index = NonUniformResourceIndex(material.orm_sampler);
        float3 orm = g_textures[texture_index].Sample(
            g_samplers[sampler_index], input.uv).rgb;
        ao *= orm.r;
        roughness = clamp(roughness * orm.g, 0.04f, 1.0f);
        metallic = saturate(metallic * orm.b);
    }
    float3 normal_dx = ddx(normal);
    float3 normal_dy = ddy(normal);
    float variance = 0.25f * (dot(normal_dx, normal_dx) +
                              dot(normal_dy, normal_dy));
    roughness = sqrt(saturate(roughness * roughness + min(variance, 0.25f)));
    if ((material.flags & 4u) != 0u)
    {
        uint texture_index = NonUniformResourceIndex(material.emissive_texture);
        uint sampler_index = NonUniformResourceIndex(material.emissive_sampler);
        emissive *= g_textures[texture_index].Sample(
            g_samplers[sampler_index], input.uv).rgb;
    }
    if (frame.render_mode == 3u)
        return float4(base.rgb + emissive,
                      material.alpha_mode == 0u ? 1.0f : base.a);

    float3 f0 = lerp(saturate(material.material_dielectric_specular.rgb),
                     base.rgb, metallic);
    if (frame.render_mode == 6u)
        return float4(metallic, roughness,
                      max(f0.x, max(f0.y, f0.z)), 1.0f);

    float3 analytic_diffuse = 0.0f;
    float3 analytic_specular = 0.0f;
    if (frame.directional_direction_enabled.w > 0.5f)
    {
        float shadow = packet_directional_shadow(frame, input.world_position);
        VkrBindlessDirectResult direct = packet_direct(
            normal, view, normalize(-frame.directional_direction_enabled.xyz),
            frame.directional_color_intensity.rgb *
                frame.directional_color_intensity.w * shadow,
            base.rgb, metallic, roughness, f0);
        analytic_diffuse = direct.diffuse;
        analytic_specular = direct.specular;
    }
    uint4 point_mask = packet_point_light_mask(frame, input.world_position);
    uint point_count = min(frame.point_light_count, 128u);
    for (uint word = 0u; word < 4u; ++word)
    {
        uint remaining = point_mask[word];
        while (remaining != 0u)
        {
            uint light_index = word * 32u + uint(firstbitlow(remaining));
            remaining &= remaining - 1u;
            if (light_index >= point_count)
                continue;
            float4 p0 = frame.point_light_data[light_index * 4u + 0u];
            float4 p1 = frame.point_light_data[light_index * 4u + 1u];
            float4 p2 = frame.point_light_data[light_index * 4u + 2u];
            float4 p3 = frame.point_light_data[light_index * 4u + 3u];
            uint kind = uint(p2.w + 0.5f);
            float3 to_light = p0.xyz - input.world_position;
            float distance_squared = dot(to_light, to_light);
            if (kind != 0u && p2.z > 0.0f &&
                distance_squared >= p2.z * p2.z)
                continue;
            float distance = sqrt(distance_squared);
            float3 light_direction = distance > 1e-6f
                                         ? to_light / distance
                                         : float3(0.0f);
            float attenuation = 0.0f;
            if (kind == 0u)
            {
                attenuation = 1.0f /
                    max(max(p0.w, 1.0f) + p1.w * distance +
                            p2.y * distance_squared,
                        1e-6f);
            }
            else
            {
                float range_attenuation = 1.0f;
                if (p2.z > 0.0f)
                {
                    float ratio = distance / p2.z;
                    range_attenuation = saturate(1.0f - ratio * ratio *
                                                 ratio * ratio);
                    range_attenuation *= range_attenuation;
                }
                attenuation = range_attenuation /
                              max(distance_squared, 1e-4f);
                if (kind == 2u)
                {
                    float cone = dot(-light_direction, normalize(p3.xyz));
                    float cone_attenuation = smoothstep(p1.w, p0.w, cone);
                    if (cone_attenuation <= 0.0f)
                        continue;
                    attenuation *= cone_attenuation;
                }
            }
            VkrBindlessDirectResult direct = packet_direct(
                normal, view, light_direction, p1.rgb * p2.x * attenuation,
                base.rgb, metallic, roughness, f0);
            analytic_diffuse += direct.diffuse;
            analytic_specular += direct.specular;
        }
    }
    float3 color = analytic_diffuse + analytic_specular;

    if ((frame.flags & 2u) != 0u)
    {
        float3 reflection = reflect(-view, normal);
        float3 fresnel = packet_fresnel_roughness(no_v, f0, roughness);
        float3 kd = (1.0f - fresnel) * (1.0f - metallic);
        uint irradiance_texture =
            NonUniformResourceIndex(frame.irradiance_texture);
        uint irradiance_sampler =
            NonUniformResourceIndex(frame.irradiance_sampler);
        uint prefilter_texture =
            NonUniformResourceIndex(frame.prefilter_texture);
        uint prefilter_sampler =
            NonUniformResourceIndex(frame.prefilter_sampler);
        float3 global_irradiance = g_cube_textures[irradiance_texture]
            .SampleLevel(g_samplers[irradiance_sampler], normal, 0.0f).rgb;
        float3 global_prefiltered = g_cube_textures[prefilter_texture]
            .SampleLevel(g_samplers[prefilter_sampler], reflection,
                         roughness * float(max(frame.prefilter_mip_count, 1u) -
                                           1u)).rgb;
        float2 brdf = packet_brdf_approximation(no_v, roughness);
        float f90 = saturate(max(f0.x, max(f0.y, f0.z)) * 25.0f);
        float horizon = saturate(1.0f + dot(reflection, geometric_normal));
        float specular_visibility = horizon * horizon *
            packet_specular_ao(ao, no_v, roughness);
        float3 diffuse = 0.0f;
        float3 specular = 0.0f;
        float local_weight_sum = 0.0f;
        uint probe_count = min(frame.ibl_probe_count, 16u);
        for (uint i = 0u; i < probe_count; ++i)
            local_weight_sum += packet_probe_influence(frame.ibl_probes[i],
                                                        input.world_position);
        float weight_scale = local_weight_sum > 1.0f
                                 ? 1.0f / local_weight_sum
                                 : 1.0f;
        for (uint i = 0u; i < probe_count; ++i)
        {
            VkrBindlessIblProbe probe = frame.ibl_probes[i];
            float weight = packet_probe_influence(probe,
                                                  input.world_position) *
                           weight_scale;
            if (weight <= 1e-6f)
                continue;
            float3 probe_reflection = probe.intensity_box.w > 0.5f
                ? packet_box_project(reflection, input.world_position,
                                     probe.center_blend.xyz,
                                     max(probe.extents_weight.xyz, 0.0f))
                : reflection;
            uint probe_irradiance_texture =
                NonUniformResourceIndex(probe.irradiance_texture);
            uint probe_irradiance_sampler =
                NonUniformResourceIndex(probe.irradiance_sampler);
            uint probe_prefilter_texture =
                NonUniformResourceIndex(probe.prefilter_texture);
            uint probe_prefilter_sampler =
                NonUniformResourceIndex(probe.prefilter_sampler);
            float3 probe_irradiance =
                g_cube_textures[probe_irradiance_texture].SampleLevel(
                    g_samplers[probe_irradiance_sampler], normal, 0.0f).rgb;
            float3 probe_prefiltered =
                g_cube_textures[probe_prefilter_texture].SampleLevel(
                    g_samplers[probe_prefilter_sampler], probe_reflection,
                     roughness * float(max(frame.prefilter_mip_count, 1u) -
                                      1u)).rgb;
            diffuse += kd * probe_irradiance * base.rgb * ao *
                       probe.intensity_box.x * probe.intensity_box.y * weight;
            specular += probe_prefiltered *
                        (fresnel * brdf.x + f90 * brdf.y) *
                        specular_visibility * probe.intensity_box.x *
                        probe.intensity_box.z * weight;
        }
        float global_weight = max(1.0f - min(local_weight_sum, 1.0f), 0.0f);
        diffuse += kd * global_irradiance * base.rgb * ao * global_weight;
        specular += global_prefiltered *
                    (fresnel * brdf.x + f90 * brdf.y) *
                    specular_visibility * global_weight;
        color += (diffuse * frame.ibl_controls.y +
                  specular * frame.ibl_controls.z) * frame.ibl_controls.x;
    }
    else
    {
        color += frame.ambient_color.rgb * base.rgb * ao;
    }
    if (frame.render_mode == 1u)
        return float4(analytic_diffuse + analytic_specular, 1.0f);
    if (frame.render_mode == 4u)
        return float4(analytic_diffuse, 1.0f);
    if (frame.render_mode == 5u)
        return float4(analytic_specular, 1.0f);
    color += emissive;

    if ((frame.flags & 4u) != 0u && material.material_alpha.y > 0.0f)
    {
        float transmission = saturate(material.material_alpha.y);
        float thickness = max(material.material_alpha.w, 0.0f);
        float3 view_normal = normalize(mul(frame.view,
                                           float4(normal, 0.0f)).xyz);
        float3 refracted = refract(float3(0.0f, 0.0f, -1.0f), view_normal,
                                   1.0f / max(material.material_alpha.z, 1.0f));
        float2 screen_uv = input.position.xy *
                           float2(frame.ibl_controls.w, frame.ambient_color.w);
        float2 offset = refracted.xy / max(abs(refracted.z), 0.25f) *
                        thickness * 0.02f;
        uint texture_index =
            NonUniformResourceIndex(frame.transmission_texture);
        uint sampler_index =
            NonUniformResourceIndex(frame.transmission_sampler);
        float3 transmitted = g_textures[texture_index].SampleLevel(
            g_samplers[sampler_index],
            clamp(screen_uv + offset, 0.0f, 1.0f), 0.0f).rgb;
        if (material.material_attenuation_color.w > 1e-4f && thickness > 0.0f)
        {
            transmitted *= pow(clamp(material.material_attenuation_color.rgb,
                                     1e-4f, 1.0f),
                               thickness /
                                   material.material_attenuation_color.w);
        }
        float3 fresnel = packet_fresnel(no_v, f0);
        float weight = transmission * (1.0f - metallic) *
                       (1.0f - max(fresnel.x, max(fresnel.y, fresnel.z)));
        color = lerp(color, transmitted, saturate(weight));
    }
    return float4(color, material.alpha_mode == 0u ? 1.0f : base.a);
}

[shader("fragment")]
void shadow_fragment(VkrBindlessVertexOutput input)
{
    VkrGpuVisibleDrawRow visible =
        g_push.root.visible_rows[g_push.root.visible_row_index];
    VkrBindlessMaterial material =
        g_push.root.frame.materials[visible.material_index];
    if ((g_push.flags & 1u) != 0u &&
        packet_base_color(input).a < material.material_alpha.x)
        discard;
}

[shader("fragment")]
uint picking_fragment(VkrBindlessVertexOutput input) : SV_Target0
{
    return input.object_id;
}

[shader("vertex")]
VkrBindlessVertexOutput fullscreen_vertex(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(3.0f, -1.0f),
        float2(-1.0f, 3.0f),
    };
    VkrBindlessVertexOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    output.uv = positions[vertex_id] * 0.5f + 0.5f;
    output.color = 1.0f;
    output.world_position = 0.0f;
    output.world_normal = float3(0.0f, 0.0f, 1.0f);
    output.world_tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
    output.object_id = 0u;
    return output;
}

[shader("fragment")]
float4 fullscreen_fragment(VkrBindlessVertexOutput input) : SV_Target0
{
    VkrBindlessPacketUtilityRoot* root =
        (VkrBindlessPacketUtilityRoot*)g_push.root;
    uint texture_index =
        NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index =
        NonUniformResourceIndex(root.transmission_sampler);
    float4 source =
        g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv);
    if ((g_push.flags & 2u) != 0u)
    {
        float3 mapped = vkr_aces_fitted(
            max(source.rgb * root.ibl_controls.x, 0.0f));
        source.rgb = pow(mapped, 1.0f / 2.2f);
    }
    return source;
}

[shader("fragment")]
float4 skybox_fragment(VkrBindlessVertexOutput input) : SV_Target0
{
    VkrBindlessPacketUtilityRoot* root =
        (VkrBindlessPacketUtilityRoot*)g_push.root;
    float2 ndc = input.uv * 2.0f - 1.0f;
    float4 world = mul(root.view_projection,
                       float4(ndc, 1.0f, 1.0f));
    float3 direction = normalize(world.xyz / world.w -
                                  root.view_position.xyz);
    uint texture_index =
        NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index =
        NonUniformResourceIndex(root.transmission_sampler);
    return g_cube_textures[texture_index].Sample(g_samplers[sampler_index],
                                                  direction);
}

[shader("vertex")]
VkrBindlessTextOutput text_vertex(uint vertex_id : SV_VertexID)
{
    VkrBindlessPacketUtilityRoot* root =
        (VkrBindlessPacketUtilityRoot*)g_push.root;
    VkrBindlessTextVertex* vertices =
        (VkrBindlessTextVertex*)root.vertices;
    VkrBindlessTextVertex glyph = vertices[vertex_id];
    float4 position = mul(root.view, float4(glyph.position, 0.0f, 1.0f));
    VkrBindlessTextOutput output;
    if ((root.flags & 1u) != 0u)
    {
        float2 target = max(root.point_light_grid_origin_cell_size.xy,
                            float2(1.0f, 1.0f));
        output.position = float4(position.x / target.x * 2.0f - 1.0f,
                                 1.0f - position.y / target.y * 2.0f,
                                 position.z, position.w);
    }
    else
    {
        output.position = mul(root.view_projection, position);
    }
    output.uv = glyph.texcoord;
    output.color = glyph.color;
    output.object_id = root.first_instance;
    return output;
}

float text_alpha(VkrBindlessTextOutput input)
{
    VkrBindlessPacketUtilityRoot* root =
        (VkrBindlessPacketUtilityRoot*)g_push.root;
    uint texture_index = NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index = NonUniformResourceIndex(root.transmission_sampler);
    float4 atlas =
        g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv);
    if (root.material_flags == 0u)
        return atlas.a;
    float distance = max(min(atlas.r, atlas.g),
                         min(max(atlas.r, atlas.g), atlas.b)) - 0.5f;
    return saturate(distance / max(fwidth(distance), 1e-6f) + 0.5f);
}

[shader("fragment")]
float4 text_fragment(VkrBindlessTextOutput input) : SV_Target0
{
    float alpha = text_alpha(input);
    return float4(input.color.rgb, input.color.a * alpha);
}

[shader("fragment")]
uint text_picking_fragment(VkrBindlessTextOutput input) : SV_Target0
{
    if (text_alpha(input) <= 0.01f)
        discard;
    return input.object_id;
}

/*
 * Vulkan ray-query lightmap transport (ADR-088 "GPU transport"). The trace
 * benchmark and the texel gather are the Metal kernels in vkr_bake_metal.mm
 * translated to Slang: the same seeds per texel and sample, the same random
 * draws in the same order, and the same light, material and shadow rules; a
 * change to one kernel changes the other. The gather's paths run through
 * path_radiance, which the probe gather of the diffuse volume bake
 * (ADR-054) shares, on both backends.
 *
 * vkr_bake_vulkan.cpp binds the resources below and compiles this file to
 * SPIR-V once per entry point at build time.
 */

static const float kPi = 3.14159265358979323846f;
static const float kEpsilon = 1.0e-4f;
static const uint kMaxSkips = 16u;

float infinity()
{
    return asfloat(0x7f800000u);
}

uint mix_seed(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

float random_unit(uint seed)
{
    return float(mix_seed(seed) >> 8) * (1.0f / 16777216.0f);
}

/* Counter-based stream for one (texel, sample) pair. */
struct Rng
{
    uint state;

    [mutating]
    float next()
    {
        state = mix_seed(state + 0x9e3779b9u);
        return float(state >> 8) * (1.0f / 16777216.0f);
    }
};

/* Cosine-weighted direction about a unit normal (same frame as the CPU
   baker's cosine_direction). */
float3 cosine_direction(float3 normal, float u1, float u2)
{
    float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + normal.z);
    float b = normal.x * normal.y * a;
    float3 tangent = float3(1.0f + sign * normal.x * normal.x * a, sign * b,
                            -sign * normal.x);
    float3 bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);
    float radius = sqrt(u1);
    float phi = 6.28318530718f * u2;
    return normalize(tangent * (radius * cos(phi)) +
                     bitangent * (radius * sin(phi)) +
                     normal * sqrt(max(0.0f, 1.0f - u1)));
}

/* Unpacks RGBA8 with red in the lowest byte, as Metal's
   unpack_unorm4x8_to_float. */
float4 unpack_unorm4x8(uint value)
{
    return float4(float(value & 0xffu), float((value >> 8) & 0xffu),
                  float((value >> 16) & 0xffu), float(value >> 24)) *
           (1.0f / 255.0f);
}

float max3(float x, float y, float z)
{
    return max(max(x, y), z);
}

struct BenchmarkArgs
{
    uint first_texel;
    uint texel_count;
    uint samples;
    uint seed;
};

struct GpuMaterial
{
    float4 base_color;
    float4 emissive_metallic;
    /* x alpha cutoff, y transmission, z alpha mode, w 1 when shadow rays pass
       through tinted (thin transmissive or index-matched). */
    float4 misc;
    /* xyz dielectric specular color, w perceptual roughness. */
    float4 specular;
    /* Texture layers, -1 when absent: x base color, y emission,
       z metallic-roughness, w transmission. */
    int4 layers;
    /* x 1 when double-sided: both sides are front faces. */
    float4 surface;
};

struct GpuLight
{
    /* xyz position, w kind (0 directional, 1 point, 2 spot, 3 polynomial,
       4 rectangle). */
    float4 position_kind;
    float4 direction_range;
    float4 color_intensity;
    /* x constant, y linear, z quadratic, w rectangle radiance. */
    float4 attenuation;
    float4 right_half_width;
    float4 up_half_height;
    /* x cos inner, y cos outer, z casts shadow, w enabled. */
    float4 cone;
    /* x one for a stationary light (ADR-107); yzw unused. */
    float4 stationary;
};

/* Every member is a 32-bit scalar, so std140 and std430 lay it out alike;
   vkr_bake_vulkan.cpp's GatherArgs matches it byte for byte. */
struct GatherArgs
{
    uint first_texel;
    uint texel_count;
    /* All samples per texel, and the run of them this dispatch takes. */
    uint samples;
    uint first_sample;
    uint sample_count;
    uint seed;
    uint max_depth;
    uint rr_start_depth;
    uint light_count;
    /* Layer lights from this index on are rectangles, sampled once per
       sample; the lights before it are evaluated once per texel. */
    uint rectangle_first;
    /* bit 0 sky, bit 1 emission, bit 2 texel direct, bit 3 occlusion, bit 4
       back-face fraction, bit 5 incident direction. */
    uint flags;
    /* First triangle of each geometry in the acceleration structure's index
       buffer: opaque triangles, then cutout and blended ones. */
    uint geometry_first0;
    uint geometry_first1;
    /* World distance within which a first hit occludes, falling off linearly
       to it. */
    float occlusion_radius;
    /* Largest luminance of one indirect sample; zero keeps every sample. */
    float indirect_clamp;
};

/* Descriptor set 0; the binding numbers are vkr_bake_vulkan.cpp's Binding
   enumeration. */
[[vk::binding(0, 0)]] StructuredBuffer<float4> texel_positions;
[[vk::binding(1, 0)]] StructuredBuffer<float4> texel_normals;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> indirect_out;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> hit_fraction_out;
[[vk::binding(4, 0)]] RaytracingAccelerationStructure scene_as;
[[vk::binding(5, 0)]] StructuredBuffer<float> corner_positions;
[[vk::binding(6, 0)]] StructuredBuffer<float> corner_normals;
[[vk::binding(7, 0)]] StructuredBuffer<float2> corner_uvs;
[[vk::binding(8, 0)]] StructuredBuffer<uint> corner_colors;
[[vk::binding(9, 0)]] StructuredBuffer<uint> triangle_materials;
[[vk::binding(10, 0)]] StructuredBuffer<GpuMaterial> materials;
[[vk::binding(11, 0)]] StructuredBuffer<GpuLight> lights;
[[vk::binding(12, 0)]] StructuredBuffer<uint> layer_lights;
[[vk::binding(13, 0)]] StructuredBuffer<uint> as_indices;
[[vk::binding(14, 0)]] RWStructuredBuffer<float> occlusion_out;
[[vk::binding(15, 0)]] RWStructuredBuffer<float4> direct_out;
[[vk::binding(16, 0)]] RWStructuredBuffer<float2> moments_out;
[[vk::binding(17, 0)]] RWStructuredBuffer<float> backface_out;
[[vk::binding(18, 0)]] StructuredBuffer<float4> texel_footprints;
[[vk::binding(19, 0)]] Texture2DArray<float4> material_textures;
[[vk::binding(20, 0)]] Texture2D<float4> sky_texture;
[[vk::binding(21, 0)]] Texture2D<float4> dfg_texture;
/* Probe centers (xyz), path seeds and per-pixel mean radiance of the probe
   gather. */
[[vk::binding(25, 0)]] StructuredBuffer<float4> probe_positions;
[[vk::binding(26, 0)]] StructuredBuffer<uint> probe_seeds;
[[vk::binding(27, 0)]] RWStructuredBuffer<float4> probe_radiance_out;
/* The texel gather's luminance-weighted incident direction: xyz the sum of
   each contribution's luminance times its direction, w the luminance sum. */
[[vk::binding(28, 0)]] RWStructuredBuffer<float4> direction_out;
/* Normalized, linear, repeat. */
[[vk::binding(22, 0)]] SamplerState texture_sampler;
/* Linear, repeat across longitude, clamped at the poles. */
[[vk::binding(23, 0)]] SamplerState sky_sampler;
/* Linear, clamped to edge. */
[[vk::binding(24, 0)]] SamplerState dfg_sampler;

[shader("compute")]
[numthreads(64, 1, 1)]
void lightmap_trace_benchmark(uint3 thread_id: SV_DispatchThreadID,
                              uniform BenchmarkArgs args)
{
    uint id = thread_id.x;
    if (id >= args.texel_count)
    {
        return;
    }
    uint texel = args.first_texel + id;
    float3 normal = texel_normals[texel].xyz;
    float3 origin = texel_positions[texel].xyz + normal * 1.0e-3f;
    uint texel_seed = mix_seed(args.seed ^ mix_seed(texel));
    uint hits = 0u;
    for (uint s = 0u; s < args.samples; ++s)
    {
        uint seed = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = cosine_direction(normal, random_unit(seed),
                                         random_unit(seed ^ 0x68bc21ebu));
        ray.TMin = 0.0f;
        ray.TMax = infinity();
        /* Closest hits as the CPU BVH finds them: every triangle is solid. */
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(scene_as, RAY_FLAG_NONE, 0xffu, ray);
        while (query.Proceed())
        {
        }
        hits += query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
    }
    hit_fraction_out[texel] = float(hits) / float(args.samples);
}

struct Material
{
    float4 base;
    float3 emissive;
    float metallic;
    float cutoff;
    float transmission;
    uint mode;
    bool shadow_layer;
    float3 dielectric_specular;
    float roughness;
    bool double_sided;
};

float4 sample_layer(int layer, float2 uv)
{
    return material_textures.SampleLevel(texture_sampler,
                                         float3(uv, float(uint(layer))), 0.0f);
}

Material material_at(uint triangle, float2 bary)
{
    float w0 = 1.0f - bary.x - bary.y;
    float2 uv = corner_uvs[3u * triangle] * w0 +
                corner_uvs[3u * triangle + 1u] * bary.x +
                corner_uvs[3u * triangle + 2u] * bary.y;
    float4 color = unpack_unorm4x8(corner_colors[3u * triangle]) * w0 +
                   unpack_unorm4x8(corner_colors[3u * triangle + 1u]) * bary.x +
                   unpack_unorm4x8(corner_colors[3u * triangle + 2u]) * bary.y;
    GpuMaterial source = materials[triangle_materials[triangle]];
    Material material;
    material.base = source.base_color * color;
    if (source.layers.x >= 0)
    {
        material.base *= sample_layer(source.layers.x, uv);
    }
    material.emissive = source.emissive_metallic.xyz;
    if (source.layers.y >= 0)
    {
        material.emissive *= sample_layer(source.layers.y, uv).xyz;
    }
    /* As vkr_bake_material_sample: metallic and roughness scale by the
       metallic-roughness texture's blue and green, transmission by the
       transmission texture's red. */
    material.metallic = saturate(source.emissive_metallic.w);
    material.roughness = clamp(source.specular.w, 0.04f, 1.0f);
    if (source.layers.z >= 0)
    {
        float4 orm = sample_layer(source.layers.z, uv);
        material.metallic = saturate(material.metallic * orm.z);
        material.roughness = clamp(material.roughness * orm.y, 0.04f, 1.0f);
    }
    material.transmission = saturate(source.misc.y);
    if (source.layers.w >= 0)
    {
        material.transmission = saturate(material.transmission *
                                         sample_layer(source.layers.w, uv).x);
    }
    material.cutoff = source.misc.x;
    material.mode = uint(source.misc.z);
    material.shadow_layer = source.misc.w != 0.0f;
    material.dielectric_specular = source.specular.xyz;
    material.double_sided = source.surface.x != 0.0f;
    return material;
}

float3 corner_position(uint triangle, uint c)
{
    uint i = 9u * triangle + 3u * c;
    return float3(corner_positions[i], corner_positions[i + 1u],
                  corner_positions[i + 2u]);
}

float3 corner_normal(uint triangle, uint c)
{
    uint i = 9u * triangle + 3u * c;
    return float3(corner_normals[i], corner_normals[i + 1u],
                  corner_normals[i + 2u]);
}

/* Whether a hit is passed through: cutout below its cutoff, or blended and
   rejected by its alpha. */
bool passes(Material material, inout Rng rng)
{
    float alpha = saturate(material.base.w);
    if (material.mode == 1u)
    {
        return alpha < saturate(material.cutoff);
    }
    if (material.mode == 2u)
    {
        return alpha <= 0.0f || (alpha < 1.0f && rng.next() >= alpha);
    }
    return false;
}

struct Hit
{
    bool found;
    /* The ray reached the back of a one-sided surface: the side its
       interpolated normal points away from. */
    bool back_face;
    float3 position;
    float3 geometric;
    float3 normal;
    Material material;
};

uint scene_triangle(uint geometry_first0, uint geometry_first1,
                    uint geometry, uint primitive)
{
    uint first = min(geometry, 1u) == 0u ? geometry_first0 : geometry_first1;
    return as_indices[3u * (first + primitive)] / 3u;
}

/* Whether a cutout texel lies below its cutoff; only the alpha inputs of
   material_at are evaluated. */
bool cutout_passes(uint triangle, float2 bary)
{
    GpuMaterial source = materials[triangle_materials[triangle]];
    if (uint(source.misc.z) != 1u)
    {
        return false;
    }
    float w0 = 1.0f - bary.x - bary.y;
    float alpha =
        source.base_color.w *
        (unpack_unorm4x8(corner_colors[3u * triangle]).w * w0 +
         unpack_unorm4x8(corner_colors[3u * triangle + 1u]).w * bary.x +
         unpack_unorm4x8(corner_colors[3u * triangle + 2u]).w * bary.y);
    if (source.layers.x >= 0)
    {
        float2 uv = corner_uvs[3u * triangle] * w0 +
                    corner_uvs[3u * triangle + 1u] * bary.x +
                    corner_uvs[3u * triangle + 2u] * bary.y;
        alpha *= sample_layer(source.layers.x, uv).w;
    }
    return saturate(alpha) < saturate(source.misc.x);
}

/* What one path reads besides the scene: the acceleration structure's
   geometry groups, the layer lights, the bounce limits and which sources the
   layer holds. Both kernels that trace paths fill it from their push
   constants. */
struct Transport
{
    uint geometry_first0;
    uint geometry_first1;
    uint light_count;
    uint max_depth;
    uint rr_start_depth;
    bool use_sky;
    bool use_emission;
};

struct Stop
{
    bool found;
    uint triangle;
    float2 bary;
    float distance;
};

/* The closest triangle within (kEpsilon, max_distance) that is not a cutout
   texel below its cutoff, in one traversal: opaque triangles commit as the
   traversal finds them, and cutout and blended candidates commit unless the
   cutout passes. Blended surfaces are resolved by the caller, because their
   stochastic pass must be drawn once per surface. With `any`, the first such
   triangle found ends the traversal instead of the closest. */
Stop find_stop(Transport transport, float3 origin, float3 direction,
               float max_distance, bool any)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = kEpsilon;
    ray.TMax = max_distance;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene_as,
                         any ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
                             : RAY_FLAG_NONE,
                         0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            continue;
        }
        uint triangle = scene_triangle(
            transport.geometry_first0, transport.geometry_first1,
            query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex());
        if (cutout_passes(triangle, query.CandidateTriangleBarycentrics()))
        {
            continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    Stop stop;
    stop.found = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    stop.triangle = 0u;
    stop.bary = float2(0.0f);
    stop.distance = 0.0f;
    if (stop.found)
    {
        stop.triangle = scene_triangle(
            transport.geometry_first0, transport.geometry_first1,
            query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex());
        stop.bary = query.CommittedTriangleBarycentrics();
        stop.distance = query.CommittedRayT();
    }
    return stop;
}

/* Closest surface the ray stops at, skipping passed-through surfaces. The
   geometric normal faces the ray origin and the shading normal its side;
   back_face records which side of the surface the ray reached. */
Hit trace_surface(Transport transport, float3 origin, float3 direction,
                  inout Rng rng)
{
    Hit result;
    result.found = false;
    result.back_face = false;
    for (uint skip = 0u; skip < kMaxSkips; ++skip)
    {
        Stop stop = find_stop(transport, origin, direction, infinity(), false);
        if (!stop.found)
        {
            return result;
        }
        uint triangle = stop.triangle;
        float2 bary = stop.bary;
        Material material = material_at(triangle, bary);
        float3 position = origin + direction * stop.distance;
        if (passes(material, rng))
        {
            origin = position;
            continue;
        }
        float3 p0 = corner_position(triangle, 0u);
        float3 p1 = corner_position(triangle, 1u);
        float3 p2 = corner_position(triangle, 2u);
        float3 geometric = normalize(cross(p1 - p0, p2 - p0));
        if (dot(geometric, direction) > 0.0f)
        {
            geometric = -geometric;
        }
        float w0 = 1.0f - bary.x - bary.y;
        float3 normal = normalize(corner_normal(triangle, 0u) * w0 +
                                  corner_normal(triangle, 1u) * bary.x +
                                  corner_normal(triangle, 2u) * bary.y);
        result.back_face =
            !material.double_sided && dot(normal, direction) > 0.0f;
        if (dot(normal, geometric) < 0.0f)
        {
            normal = -normal;
        }
        result.found = true;
        result.position = position;
        result.geometric = geometric;
        result.normal = normal;
        result.material = material;
        return result;
    }
    return result;
}

/* Transmittance toward a light, as the CPU integrator's shadow walk: cutout
   surfaces below their cutoff pass; thin transmissive or index-matched
   surfaces tint by their glass color, blended ones mixed with (1 - alpha);
   anything else blocks. Any blocker zeroes the product whatever its order, so
   one any-hit traversal settles a segment unless the surface it finds tints;
   then the segment is walked in order. */
float3 shadow_transmittance(Transport transport, float3 origin, float3 direction,
                            float distance)
{
    Stop any = find_stop(transport, origin, direction, distance, true);
    if (!any.found)
    {
        return float3(1.0f);
    }
    GpuMaterial any_source = materials[triangle_materials[any.triangle]];
    if (any_source.misc.w == 0.0f && uint(any_source.misc.z) != 2u)
    {
        return float3(0.0f);
    }
    float3 transmittance = float3(1.0f);
    for (uint skip = 0u; skip < kMaxSkips; ++skip)
    {
        Stop stop = find_stop(transport, origin, direction, distance, false);
        if (!stop.found)
        {
            return transmittance;
        }
        Material material = material_at(stop.triangle, stop.bary);
        float alpha = saturate(material.base.w);
        bool cutout_pass =
            material.mode == 1u && alpha < saturate(material.cutoff);
        if (!cutout_pass)
        {
            if (!material.shadow_layer)
            {
                return float3(0.0f);
            }
            float3 glass = max(material.base.xyz, float3(0.0f)) *
                           saturate(material.transmission);
            transmittance *= material.mode == 2u
                                 ? (1.0f - alpha) + glass * alpha
                                 : glass;
            if (max3(transmittance.x, transmittance.y, transmittance.z) <=
                1.0e-6f)
            {
                return float3(0.0f);
            }
        }
        origin += direction * stop.distance;
        distance -= stop.distance;
        if (distance <= kEpsilon)
        {
            return transmittance;
        }
    }
    return float3(0.0f);
}

struct LightSample
{
    bool valid;
    float3 direction;
    float3 radiance;
    float distance;
    float weight;
};

/* The CPU integrator's evaluate_light and sample_rectangle_light. */
LightSample sample_light(GpuLight light, float3 position, inout Rng rng)
{
    LightSample sample;
    sample.valid = false;
    sample.direction = float3(0.0f);
    sample.radiance = float3(0.0f);
    sample.distance = 0.0f;
    sample.weight = 1.0f;
    uint kind = uint(light.position_kind.w);
    if (light.cone.w == 0.0f)
    {
        return sample;
    }
    if (kind == 4u)
    {
        float radiance = light.attenuation.w;
        if (radiance <= 0.0f)
        {
            return sample;
        }
        /* Two draws in source order, as Metal evaluates them. */
        float right_draw = rng.next();
        float up_draw = rng.next();
        float3 point =
            light.position_kind.xyz +
            light.right_half_width.xyz *
                ((2.0f * right_draw - 1.0f) * light.right_half_width.w) +
            light.up_half_height.xyz *
                ((2.0f * up_draw - 1.0f) * light.up_half_height.w);
        float3 to_light = point - position;
        float distance_squared = dot(to_light, to_light);
        if (!(distance_squared > 1.0e-12f))
        {
            return sample;
        }
        float distance = sqrt(distance_squared);
        float3 direction = to_light / distance;
        float emitter_cosine = dot(light.direction_range.xyz, -direction);
        if (!(emitter_cosine > 0.0f))
        {
            return sample;
        }
        float area = 4.0f * light.right_half_width.w * light.up_half_height.w;
        sample.valid = true;
        sample.direction = direction;
        sample.radiance = light.color_intensity.xyz * radiance;
        sample.distance = distance;
        sample.weight = emitter_cosine * area / distance_squared;
        return sample;
    }
    float intensity = light.color_intensity.w;
    if (intensity <= 0.0f)
    {
        return sample;
    }
    if (kind == 0u)
    {
        sample.valid = true;
        sample.direction = normalize(-light.direction_range.xyz);
        sample.radiance = light.color_intensity.xyz * intensity;
        sample.distance = infinity();
        return sample;
    }
    float3 to_light = light.position_kind.xyz - position;
    float distance_squared = dot(to_light, to_light);
    if (!(distance_squared > 1.0e-12f))
    {
        return sample;
    }
    float distance = sqrt(distance_squared);
    float range = light.direction_range.w;
    if (kind != 3u && range > 0.0f && distance > range)
    {
        return sample;
    }
    float3 direction = to_light / distance;
    float attenuation;
    if (kind == 3u)
    {
        attenuation = 1.0f / max(max(light.attenuation.x, 1.0f) +
                                     light.attenuation.y * distance +
                                     light.attenuation.z * distance_squared,
                                 1.0e-6f);
    }
    else
    {
        float range_attenuation = 1.0f;
        if (range > 0.0f)
        {
            float ratio = distance / range;
            range_attenuation = saturate(1.0f - ratio * ratio * ratio * ratio);
            range_attenuation *= range_attenuation;
        }
        attenuation = range_attenuation / max(distance_squared, 1.0e-4f);
        if (kind == 2u)
        {
            float3 axis = normalize(light.direction_range.xyz);
            float inner = light.cone.x;
            float outer = light.cone.y;
            float cosine = dot(-direction, axis);
            float cone = inner > outer
                             ? saturate((cosine - outer) / (inner - outer))
                             : (cosine >= outer ? 1.0f : 0.0f);
            attenuation *= cone * cone;
        }
    }
    if (!(attenuation > 0.0f))
    {
        return sample;
    }
    sample.valid = true;
    sample.direction = direction;
    sample.radiance = light.color_intensity.xyz * (intensity * attenuation);
    sample.distance = distance;
    return sample;
}

/* Albedo of the one diffuse lobe an opaque bounce scatters with: the CPU
   BSDF's view-dependent split-sum specular reflectance R plus its diffuse
   residual (1 - R)(1 - metallic) base. Specular energy is kept and
   redistributed as a cosine lobe. */
float3 bounce_albedo(Material material, float no_v)
{
    float metallic = saturate(material.metallic);
    float3 base = saturate(material.base.xyz);
    float3 f0 = saturate(material.dielectric_specular) * (1.0f - metallic) +
                base * metallic;
    uint width;
    uint height;
    dfg_texture.GetDimensions(width, height);
    float size = float(width);
    float2 coordinate =
        (float2(sqrt(saturate(no_v)), saturate(material.roughness)) *
             (size - 1.0f) +
         0.5f) /
        size;
    float2 split_sum = dfg_texture.SampleLevel(dfg_sampler, coordinate, 0.0f).xy;
    float3 scale =
        1.0f + f0 * (1.0f / max(split_sum.x + split_sum.y, 0.25f) - 1.0f);
    float f90 = saturate(max3(f0.x, f0.y, f0.z) * 25.0f);
    float3 reflectance =
        saturate((f0 * split_sum.x + f90 * split_sum.y) * scale);
    return reflectance + (1.0f - reflectance) * (1.0f - metallic) * base;
}

/* Length of a punctual light's shadow segment, next to the light, that no
   occluder blocks: the runtime's local shadow maps clip it as their near
   plane (vkr_local_shadow_system.c), so the bulb or socket mesh around an
   imported lamp does not shadow it. Directional and rectangle lights have
   none. */
float punctual_occluder_clip(GpuLight light)
{
    uint kind = uint(light.position_kind.w);
    if (kind == 0u || kind == 4u)
    {
        return 0.0f;
    }
    float range = light.direction_range.w;
    return range > 0.0f ? min(0.05f, range * 0.01f) : 0.05f;
}

/* A point just off a surface on the side `direction` leaves through. */
float3 leave_surface(float3 position, float3 geometric, float3 direction)
{
    float side = dot(geometric, direction) >= 0.0f ? 1.0f : -1.0f;
    return position + geometric * (side * 1.0e-3f);
}

/* Direct irradiance from the layer lights [first_light, end_light) at a
   point with this normal, without stationary lights when
   `skip_stationary`: a Vulkan host bakes desktop sets, which light them at
   runtime everywhere (ADR-107); the Metal kernel's ownership mode has no
   counterpart here. As in the CPU integrator, a light above the shading
   normal counts even when it is below the geometric surface; its shadow ray
   leaves on the light's side. With a positive `direction_weight`, each
   light's contribution also adds its luminance times that weight, times its
   direction, to `direction_sum` (w the weighted luminance). */
float3 direct_irradiance(Transport transport, uint first_light, uint end_light,
                         float3 position, float3 normal, float3 geometric,
                         inout Rng rng, inout float4 direction_sum,
                         float direction_weight, bool skip_stationary)
{
    float3 result = float3(0.0f);
    for (uint i = first_light; i < end_light; ++i)
    {
        GpuLight light = lights[layer_lights[i]];
        if (skip_stationary && light.stationary.x != 0.0f)
        {
            continue;
        }
        LightSample sample = sample_light(light, position, rng);
        if (!sample.valid)
        {
            continue;
        }
        float cosine = dot(normal, sample.direction);
        if (cosine <= 0.0f)
        {
            continue;
        }
        float3 visibility = float3(1.0f);
        bool rectangle = uint(light.position_kind.w) == 4u;
        float shadow_distance =
            isinf(sample.distance)
                ? infinity()
                : sample.distance - 2.0e-3f - punctual_occluder_clip(light);
        if ((rectangle || light.cone.z != 0.0f) && shadow_distance > 0.0f)
        {
            float3 origin = leave_surface(position, geometric, sample.direction);
            visibility = shadow_transmittance(
                transport, origin, sample.direction, shadow_distance);
        }
        float3 contribution =
            sample.radiance * visibility * (cosine * sample.weight);
        result += contribution;
        if (direction_weight > 0.0f)
        {
            direction_sum += float4(sample.direction, 1.0f) *
                             (luminance(contribution) * direction_weight);
        }
    }
    return result;
}

float3 sky_radiance(float3 direction)
{
    float u = atan2(direction.z, direction.x) / (2.0f * kPi) + 0.5f;
    float v = acos(clamp(direction.y, -1.0f, 1.0f)) / kPi;
    return sky_texture.SampleLevel(sky_sampler, float2(u, v), 0.0f).xyz;
}

float luminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

/* Whether `point`, on the texel's surface within its footprint, is the
   surface the texel sees: nothing lies between the texel's center and it,
   just off the surface. A point under a wall standing on the surface, or
   past a thin wall, is not, so its light does not leak into the texel. */
bool footprint_reaches(Transport transport, float3 center, float3 normal,
                       float3 point)
{
    float3 from = center + normal * 1.0e-3f;
    float3 to = point + normal * 1.0e-3f;
    float span = distance(from, to);
    if (!(span > 1.0e-5f))
    {
        return true;
    }
    return !find_stop(transport, from, (to - from) / span, span, true).found;
}

/* The first surface a path meets: whether there is one, how far it is, its
   opacity (one minus its glass fraction) and whether the path reached the
   back of a one-sided surface. */
struct FirstHit
{
    bool found;
    bool back_face;
    float distance;
    float opacity;
};

/* Radiance arriving at `origin` from `direction` through one path of the
   lightmap subset of the ADR-054 transport: the sky on escape, surface
   emission, the layer lights at every surface, glass passed straight through
   and one cosine lobe per bounce, with Russian roulette from
   transport.rr_start_depth. */
float3 path_radiance(Transport transport, float3 origin, float3 direction,
                     inout Rng rng, out FirstHit first)
{
    first.found = false;
    first.back_face = false;
    first.distance = 0.0f;
    first.opacity = 0.0f;
    float3 ray_origin = origin;
    float3 throughput = float3(1.0f);
    float3 radiance = float3(0.0f);
    float4 untracked = float4(0.0f);
    for (uint depth = 0u; depth < transport.max_depth; ++depth)
    {
        Hit hit = trace_surface(transport, ray_origin, direction, rng);
        if (depth == 0u && hit.found)
        {
            first.found = true;
            first.back_face = hit.back_face;
            first.distance = distance(hit.position, ray_origin);
            first.opacity = 1.0f - (1.0f - saturate(hit.material.metallic)) *
                                       saturate(hit.material.transmission);
        }
        if (!hit.found)
        {
            if (transport.use_sky)
            {
                radiance += throughput * sky_radiance(direction);
            }
            break;
        }
        if (transport.use_emission)
        {
            radiance += throughput * max(hit.material.emissive, float3(0.0f));
        }
        /* The glass fraction passes the path straight through, tinted by
           the base color (a thin-walled or slab approximation of
           refraction). */
        float glass = (1.0f - saturate(hit.material.metallic)) *
                      saturate(hit.material.transmission);
        if (glass > 0.0f && rng.next() < glass)
        {
            throughput *= max(hit.material.base.xyz, float3(0.0f));
            ray_origin = leave_surface(hit.position, hit.geometric, direction);
            continue;
        }
        float3 albedo = bounce_albedo(hit.material, dot(hit.normal, -direction));
        radiance += throughput * albedo * (1.0f / kPi) *
                    direct_irradiance(transport, 0u, transport.light_count,
                                      hit.position, hit.normal, hit.geometric,
                                      rng, untracked, 0.0f, false);
        throughput *= albedo;
        /* As in the CPU integrator, a direction sampled about the shading
           normal continues even below the geometric surface. */
        float bounce_first_draw = rng.next();
        float bounce_second_draw = rng.next();
        direction =
            cosine_direction(hit.normal, bounce_first_draw, bounce_second_draw);
        ray_origin = leave_surface(hit.position, hit.geometric, direction);
        if (transport.rr_start_depth > 0u &&
            depth + 1u >= transport.rr_start_depth)
        {
            float survival =
                min(0.95f, max3(throughput.x, throughput.y, throughput.z));
            if (survival <= 0.0f || rng.next() >= survival)
            {
                break;
            }
            throughput /= survival;
        }
    }
    return radiance;
}

/* One layer's gather at one texel, over a run of its samples. Each run adds
   its share of every mean; the first run starts them. Direct light from
   point, spot and directional lights is the same at every sample, so the
   first run evaluates it once, as the mean over a 3x3 grid of points across
   the texel's footprint, so a shadow edge crossing the texel leaves its
   covered share instead of all or nothing; rectangle lights take one point
   per sample, at a random point of the footprint. Footprint points the
   texel's center does not reach on its surface are left out. */
[shader("compute")]
[numthreads(64, 1, 1)]
void lightmap_gather(uint3 thread_id: SV_DispatchThreadID,
                     uniform GatherArgs args)
{
    uint id = thread_id.x;
    if (id >= args.texel_count)
    {
        return;
    }
    uint texel = args.first_texel + id;
    float3 texel_normal = texel_normals[texel].xyz;
    float3 texel_position = texel_positions[texel].xyz;
    float3 step_x = texel_footprints[2u * texel + 0u].xyz;
    float3 step_y = texel_footprints[2u * texel + 1u].xyz;
    float3 origin = texel_position + texel_normal * 1.0e-3f;
    Transport transport;
    transport.geometry_first0 = args.geometry_first0;
    transport.geometry_first1 = args.geometry_first1;
    transport.light_count = args.light_count;
    transport.max_depth = args.max_depth;
    transport.rr_start_depth = args.rr_start_depth;
    transport.use_sky = (args.flags & 1u) != 0u;
    transport.use_emission = (args.flags & 2u) != 0u;
    bool texel_direct = (args.flags & 4u) != 0u;
    bool use_occlusion = (args.flags & 8u) != 0u;
    bool use_backface = (args.flags & 16u) != 0u;
    bool use_direction = (args.flags & 32u) != 0u;
    bool first_run = args.first_sample == 0u;
    float4 direction_sum = float4(0.0f);
    uint texel_seed = mix_seed(args.seed ^ mix_seed(texel));
    float inverse_samples = 1.0f / float(args.samples);

    float3 direct_total = float3(0.0f);
    if (texel_direct && first_run && args.rectangle_first > 0u)
    {
        /* These lights draw no random numbers. */
        Rng unused;
        unused.state = texel_seed;
        float points = 0.0f;
        float4 point_directions = float4(0.0f);
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                float3 point = texel_position + step_x * (float(x) / 3.0f) +
                               step_y * (float(y) / 3.0f);
                if ((x != 0 || y != 0) &&
                    !footprint_reaches(transport, texel_position,
                                       texel_normal, point))
                {
                    continue;
                }
                direct_total += direct_irradiance(
                    transport, 0u, args.rectangle_first, point, texel_normal,
                    texel_normal, unused, point_directions,
                    use_direction ? 1.0f : 0.0f, true);
                points += 1.0f;
            }
        }
        direct_total /= points;
        direction_sum += point_directions / points;
    }
    bool texel_rectangles =
        texel_direct && args.rectangle_first < args.light_count;

    float3 indirect_total = float3(0.0f);
    float luminance_sum = 0.0f;
    float luminance_squared_sum = 0.0f;
    /* Ambient occlusion from each sample's first bounce: a hit within the
       radius occludes by its distance and its opacity; glass lets the rest
       through. */
    float occluded = 0.0f;
    float backface_hits = 0.0f;
    for (uint s = args.first_sample; s < args.first_sample + args.sample_count;
         ++s)
    {
        Rng rng;
        rng.state = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
        if (texel_rectangles)
        {
            /* Two draws in source order, as Metal evaluates them. */
            float x_draw = rng.next();
            float y_draw = rng.next();
            float3 point = texel_position + step_x * (x_draw - 0.5f) +
                           step_y * (y_draw - 0.5f);
            if (!footprint_reaches(transport, texel_position, texel_normal,
                                   point))
            {
                point = texel_position;
            }
            direct_total +=
                inverse_samples *
                direct_irradiance(transport, args.rectangle_first,
                                  args.light_count, point, texel_normal,
                                  texel_normal, rng, direction_sum,
                                  use_direction ? inverse_samples : 0.0f,
                                  true);
        }
        float first_draw = rng.next();
        float second_draw = rng.next();
        float3 direction =
            cosine_direction(texel_normal, first_draw, second_draw);
        FirstHit first;
        float3 radiance = path_radiance(transport, origin, direction, rng, first);
        if (first.found)
        {
            if (use_occlusion)
            {
                occluded += first.opacity *
                            saturate(1.0f - first.distance / args.occlusion_radius);
            }
            if (use_backface && first.back_face)
            {
                backface_hits += 1.0f;
            }
        }
        /* Cosine-weighted sampling: irradiance is pi times mean radiance. */
        float3 sample_irradiance = kPi * radiance;
        float sample_luminance = luminance(sample_irradiance);
        if (args.indirect_clamp > 0.0f && sample_luminance > args.indirect_clamp)
        {
            sample_irradiance *= args.indirect_clamp / sample_luminance;
            sample_luminance = args.indirect_clamp;
        }
        indirect_total += sample_irradiance;
        if (use_direction)
        {
            direction_sum += float4(direction, 1.0f) *
                             (sample_luminance * inverse_samples);
        }
        luminance_sum += sample_luminance;
        luminance_squared_sum += sample_luminance * sample_luminance;
    }

    float3 indirect_share = indirect_total * inverse_samples;
    float2 moment_share =
        float2(luminance_sum, luminance_squared_sum) * inverse_samples;
    indirect_out[texel] = first_run
                              ? float4(indirect_share, 1.0f)
                              : indirect_out[texel] + float4(indirect_share, 0.0f);
    direct_out[texel] = first_run
                            ? float4(direct_total, 1.0f)
                            : direct_out[texel] + float4(direct_total, 0.0f);
    moments_out[texel] =
        first_run ? moment_share : moments_out[texel] + moment_share;
    if (use_occlusion)
    {
        float visibility =
            (float(args.sample_count) - occluded) * inverse_samples;
        occlusion_out[texel] =
            first_run ? visibility : occlusion_out[texel] + visibility;
    }
    if (use_direction)
    {
        direction_out[texel] =
            first_run ? direction_sum : direction_out[texel] + direction_sum;
    }
    if (use_backface)
    {
        float share = backface_hits * inverse_samples;
        backface_out[texel] =
            first_run ? share : backface_out[texel] + share;
    }
}

/* vkr_bake_cube_direction (tools/bake/vkr_bake_sh.cpp): KTX face order and
   the renderer's cube coordinates. */
float3 cube_direction(uint face, float s, float t)
{
    float3 direction;
    switch (face)
    {
    case 0u:
        direction = float3(1.0f, -t, -s);
        break;
    case 1u:
        direction = float3(-1.0f, -t, s);
        break;
    case 2u:
        direction = float3(s, 1.0f, t);
        break;
    case 3u:
        direction = float3(s, -1.0f, -t);
        break;
    case 4u:
        direction = float3(s, -t, 1.0f);
        break;
    default:
        direction = float3(-s, -t, -1.0f);
        break;
    }
    return normalize(direction);
}

/* Every member is a 32-bit scalar; vkr_bake_vulkan.cpp's ProbeArgs matches
   it byte for byte. */
struct ProbeArgs
{
    uint first_probe;
    uint probe_count;
    uint face_size;
    uint samples;
    uint max_depth;
    uint rr_start_depth;
    uint light_count;
    /* bit 0 sky, bit 1 emission. */
    uint flags;
    uint geometry_first0;
    uint geometry_first1;
};

/* One cube-face pixel of one probe: the mean radiance of `samples` paths
   through jittered points of the pixel, starting at the probe's center in
   free space. Seeds follow vkr_diffuse_baker's CPU bake: the probe's seed,
   the pixel and the sample. The radiance goes to the pixel's slot of this
   dispatch; the host projects it to SH. */
[shader("compute")]
[numthreads(64, 1, 1)]
void probe_gather(uint3 thread_id: SV_DispatchThreadID,
                  uniform ProbeArgs args)
{
    uint pixels = 6u * args.face_size * args.face_size;
    uint id = thread_id.x;
    uint local_probe = id / pixels;
    if (local_probe >= args.probe_count)
    {
        return;
    }
    uint pixel = id - local_probe * pixels;
    uint probe = args.first_probe + local_probe;
    uint face = pixel / (args.face_size * args.face_size);
    uint in_face = pixel - face * args.face_size * args.face_size;
    uint y = in_face / args.face_size;
    uint x = in_face - y * args.face_size;
    Transport transport;
    transport.geometry_first0 = args.geometry_first0;
    transport.geometry_first1 = args.geometry_first1;
    transport.light_count = args.light_count;
    transport.max_depth = args.max_depth;
    transport.rr_start_depth = args.rr_start_depth;
    transport.use_sky = (args.flags & 1u) != 0u;
    transport.use_emission = (args.flags & 2u) != 0u;
    float3 origin = probe_positions[probe].xyz;
    uint base_seed = probe_seeds[probe];
    float face_size = float(args.face_size);

    float3 sum = float3(0.0f);
    for (uint s = 0u; s < args.samples; ++s)
    {
        uint seed = mix_seed(base_seed ^ mix_seed(pixel + 256u) ^
                             mix_seed(s + 65536u));
        float u = 2.0f * (float(x) + random_unit(seed)) / face_size - 1.0f;
        float v = 2.0f * (float(y) + random_unit(seed ^ 0x9e3779b9u)) /
                      face_size -
                  1.0f;
        float3 direction = cube_direction(face, u, v);
        /* The path's own stream, apart from the two jitter draws. */
        Rng rng;
        rng.state = mix_seed(seed ^ 0x68bc21ebu);
        FirstHit first;
        sum += path_radiance(transport, origin, direction, rng, first);
    }
    probe_radiance_out[id] = float4(sum / float(args.samples), 0.0f);
}

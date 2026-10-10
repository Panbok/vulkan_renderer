#include "bake/vkr_bake_gpu.h"

#include "bake/vkr_bake_sh.h"
#include "vkr_dfg_lut.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <utility>

namespace {

/* Texels per command buffer, and samples per texel the gather takes per
   command buffer: each buffer stays near a quarter second of a textured
   Bistro layer on the M1 Pro, so a long bake leaves the display GPU time and
   never trips the system watchdog, while a buffer still fills the GPU.
   Textured Bistro layers at 64 samples in one buffer per 65,536 texels ran
   7 s each and were ended for impacting interactivity, and so was a buffer
   of 65,536 texels at 8 samples, near a second, while a browser drew on the
   same GPU (2026-10-06). */
constexpr uint32_t kTexelsPerDispatch = 16384u;
constexpr uint32_t kSamplesPerDispatch = 8u;
/* Paths per probe-gather command buffer: the texel gather's budget. Like the
   texel gather, the probe gather takes its samples in runs, one sample of
   each pixel per buffer: a path evaluates every layer light at every bounce,
   and with all four samples in one buffer a 72-lamp Bistro layer's slowest
   buffer ran 0.88 s. */
constexpr uint64_t kProbePathsPerDispatch =
    (uint64_t)kTexelsPerDispatch * kSamplesPerDispatch;
constexpr uint32_t kProbeSamplesPerDispatch = 1u;

/* Material textures are resampled to this edge for transport: bounce light
   needs their low frequencies only. */
constexpr uint32_t kTextureEdge = 128u;
constexpr uint32_t kSkyWidth = 512u;
constexpr uint32_t kSkyHeight = 256u;
constexpr float kPi = 3.14159265358979323846f;

/* vkr_bake_lightmap.slang is this kernel for Vulkan, statement by statement;
   a change to one changes the other. */
const char *kKernelSource = R"METAL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

constant float kPi = 3.14159265358979323846f;
constant float kEpsilon = 1.0e-4f;
constant uint kMaxSkips = 16u;

static uint mix_seed(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  return x ^ (x >> 16);
}

static float random_unit(uint seed) {
  return float(mix_seed(seed) >> 8) * (1.0f / 16777216.0f);
}

/* Counter-based stream for one (texel, sample) pair. */
struct Rng {
  uint state;
  float next() {
    state = mix_seed(state + 0x9e3779b9u);
    return float(state >> 8) * (1.0f / 16777216.0f);
  }
};

/* Cosine-weighted direction about a unit normal (same frame as the CPU
   baker's cosine_direction). */
static float3 cosine_direction(float3 normal, float u1, float u2) {
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

struct BenchmarkArgs {
  uint first_texel;
  uint texel_count;
  uint samples;
  uint seed;
};

kernel void lightmap_trace_benchmark(
    device const float4 *positions [[buffer(0)]],
    device const float4 *normals [[buffer(1)]],
    device float *hit_fraction [[buffer(2)]],
    constant BenchmarkArgs &args [[buffer(3)]],
    primitive_acceleration_structure scene [[buffer(4)]],
    uint id [[thread_position_in_grid]]) {
  if (id >= args.texel_count) {
    return;
  }
  uint texel = args.first_texel + id;
  float3 normal = normals[texel].xyz;
  float3 origin = positions[texel].xyz + normal * 1.0e-3f;
  intersector<triangle_data> query;
  /* Closest hits as the CPU BVH finds them: every triangle is solid. */
  query.force_opacity(forced_opacity::opaque);
  uint texel_seed = mix_seed(args.seed ^ mix_seed(texel));
  uint hits = 0u;
  for (uint s = 0u; s < args.samples; ++s) {
    uint seed = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
    ray r;
    r.origin = origin;
    r.direction = cosine_direction(normal, random_unit(seed),
                                   random_unit(seed ^ 0x68bc21ebu));
    r.min_distance = 0.0f;
    r.max_distance = INFINITY;
    auto hit = query.intersect(r, scene);
    hits += hit.type != intersection_type::none ? 1u : 0u;
  }
  hit_fraction[texel] = float(hits) / float(args.samples);
}

struct GpuMaterial {
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

struct GpuLight {
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
  /* x a point or spot light's emitter radius (ADR-108); yzw unused. */
  float4 source;
};

struct GatherArgs {
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
     back-face fraction, bit 5 incident direction, bit 6 direct only: the
     samples trace their first hits for the back-face fraction and gather
     no indirect light (ADR-108). */
  uint flags;
  /* First triangle of each geometry in the acceleration structure's index
     buffer: opaque triangles, then cutout and blended ones. */
  uint geometry_first[2];
  /* World distance within which a first hit occludes, falling off linearly
     to it. */
  float occlusion_radius;
  /* Largest luminance of one indirect sample; zero keeps every sample. */
  float indirect_clamp;
};

struct Scene {
  primitive_acceleration_structure as;
  device const float *positions;
  device const float *normals;
  device const float2 *uvs;
  device const uint *colors;
  device const uint *triangle_materials;
  device const GpuMaterial *materials;
  texture2d_array<half> textures;
  sampler texture_sampler;
  /* Corner indices, three per triangle, grouped by geometry; a corner index
     divided by three is the scene triangle. */
  device const uint *as_indices;
  uint geometry_first[2];
};

struct Material {
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

static Material material_at(thread const Scene &scene, uint triangle,
                            float2 bary) {
  float w0 = 1.0f - bary.x - bary.y;
  float2 uv = scene.uvs[3u * triangle] * w0 +
              scene.uvs[3u * triangle + 1u] * bary.x +
              scene.uvs[3u * triangle + 2u] * bary.y;
  float4 color = unpack_unorm4x8_to_float(scene.colors[3u * triangle]) * w0 +
                 unpack_unorm4x8_to_float(scene.colors[3u * triangle + 1u]) *
                     bary.x +
                 unpack_unorm4x8_to_float(scene.colors[3u * triangle + 2u]) *
                     bary.y;
  GpuMaterial source = scene.materials[scene.triangle_materials[triangle]];
  Material material;
  material.base = source.base_color * color;
  if (source.layers.x >= 0) {
    material.base *= float4(scene.textures.sample(scene.texture_sampler, uv,
                                                  uint(source.layers.x)));
  }
  material.emissive = source.emissive_metallic.xyz;
  if (source.layers.y >= 0) {
    material.emissive *= float3(
        scene.textures.sample(scene.texture_sampler, uv, uint(source.layers.y))
            .xyz);
  }
  /* As vkr_bake_material_sample: metallic and roughness scale by the
     metallic-roughness texture's blue and green, transmission by the
     transmission texture's red. */
  material.metallic = saturate(source.emissive_metallic.w);
  material.roughness = clamp(source.specular.w, 0.04f, 1.0f);
  if (source.layers.z >= 0) {
    float4 orm = float4(
        scene.textures.sample(scene.texture_sampler, uv, uint(source.layers.z)));
    material.metallic = saturate(material.metallic * orm.z);
    material.roughness = clamp(material.roughness * orm.y, 0.04f, 1.0f);
  }
  material.transmission = saturate(source.misc.y);
  if (source.layers.w >= 0) {
    material.transmission = saturate(
        material.transmission *
        float(scene.textures
                  .sample(scene.texture_sampler, uv, uint(source.layers.w))
                  .x));
  }
  material.cutoff = source.misc.x;
  material.mode = uint(source.misc.z);
  material.shadow_layer = source.misc.w != 0.0f;
  material.dielectric_specular = source.specular.xyz;
  material.double_sided = source.surface.x != 0.0f;
  return material;
}

static float3 corner(device const float *values, uint triangle, uint c) {
  uint i = 9u * triangle + 3u * c;
  return float3(values[i], values[i + 1u], values[i + 2u]);
}

/* Whether a hit is passed through: cutout below its cutoff, or blended and
   rejected by its alpha. */
static bool passes(thread const Material &material, thread Rng &rng) {
  float alpha = saturate(material.base.w);
  if (material.mode == 1u) {
    return alpha < saturate(material.cutoff);
  }
  if (material.mode == 2u) {
    return alpha <= 0.0f || (alpha < 1.0f && rng.next() >= alpha);
  }
  return false;
}

struct Hit {
  bool found;
  /* The ray reached the back of a one-sided surface: the side its
     interpolated normal points away from. */
  bool back_face;
  float3 position;
  float3 geometric;
  float3 normal;
  Material material;
};

static uint scene_triangle(thread const Scene &scene, uint geometry,
                           uint primitive) {
  uint first = scene.geometry_first[min(geometry, 1u)];
  return scene.as_indices[3u * (first + primitive)] / 3u;
}

/* Whether a cutout texel lies below its cutoff; only the alpha inputs of
   material_at are evaluated. */
static bool cutout_passes(thread const Scene &scene, uint triangle,
                          float2 bary) {
  GpuMaterial source = scene.materials[scene.triangle_materials[triangle]];
  if (uint(source.misc.z) != 1u) {
    return false;
  }
  float w0 = 1.0f - bary.x - bary.y;
  float alpha =
      source.base_color.w *
      (unpack_unorm4x8_to_float(scene.colors[3u * triangle]).w * w0 +
       unpack_unorm4x8_to_float(scene.colors[3u * triangle + 1u]).w * bary.x +
       unpack_unorm4x8_to_float(scene.colors[3u * triangle + 2u]).w * bary.y);
  if (source.layers.x >= 0) {
    float2 uv = scene.uvs[3u * triangle] * w0 +
                scene.uvs[3u * triangle + 1u] * bary.x +
                scene.uvs[3u * triangle + 2u] * bary.y;
    alpha *= float(scene.textures
                       .sample(scene.texture_sampler, uv, uint(source.layers.x))
                       .w);
  }
  return saturate(alpha) < saturate(source.misc.x);
}

struct Stop {
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
static Stop find_stop(thread const Scene &scene, float3 origin,
                      float3 direction, float max_distance, bool any) {
  ray r;
  r.origin = origin;
  r.direction = direction;
  r.min_distance = kEpsilon;
  r.max_distance = max_distance;
  intersection_params params;
  params.accept_any_intersection(any);
  intersection_query<triangle_data> query;
  query.reset(r, scene.as, params);
  while (query.next()) {
    if (query.get_candidate_intersection_type() !=
        intersection_type::triangle) {
      continue;
    }
    uint triangle =
        scene_triangle(scene, query.get_candidate_geometry_id(),
                       query.get_candidate_primitive_id());
    if (cutout_passes(scene, triangle,
                      query.get_candidate_triangle_barycentric_coord())) {
      continue;
    }
    query.commit_triangle_intersection();
  }
  Stop stop;
  stop.found =
      query.get_committed_intersection_type() == intersection_type::triangle;
  if (stop.found) {
    stop.triangle = scene_triangle(scene, query.get_committed_geometry_id(),
                                   query.get_committed_primitive_id());
    stop.bary = query.get_committed_triangle_barycentric_coord();
    stop.distance = query.get_committed_distance();
  }
  return stop;
}

/* Closest surface the ray stops at, skipping passed-through surfaces. The
   geometric normal faces the ray origin and the shading normal its side;
   back_face records which side of the surface the ray reached. */
static Hit trace_surface(thread const Scene &scene, float3 origin,
                         float3 direction, thread Rng &rng) {
  Hit result;
  result.found = false;
  result.back_face = false;
  for (uint skip = 0u; skip < kMaxSkips; ++skip) {
    Stop stop = find_stop(scene, origin, direction, INFINITY, false);
    if (!stop.found) {
      return result;
    }
    uint triangle = stop.triangle;
    float2 bary = stop.bary;
    Material material = material_at(scene, triangle, bary);
    float3 position = origin + direction * stop.distance;
    if (passes(material, rng)) {
      origin = position;
      continue;
    }
    float3 p0 = corner(scene.positions, triangle, 0u);
    float3 p1 = corner(scene.positions, triangle, 1u);
    float3 p2 = corner(scene.positions, triangle, 2u);
    float3 geometric = normalize(cross(p1 - p0, p2 - p0));
    if (dot(geometric, direction) > 0.0f) {
      geometric = -geometric;
    }
    float w0 = 1.0f - bary.x - bary.y;
    float3 normal = normalize(corner(scene.normals, triangle, 0u) * w0 +
                              corner(scene.normals, triangle, 1u) * bary.x +
                              corner(scene.normals, triangle, 2u) * bary.y);
    result.back_face =
        !material.double_sided && dot(normal, direction) > 0.0f;
    if (dot(normal, geometric) < 0.0f) {
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
static float3 shadow_transmittance(thread const Scene &scene, float3 origin,
                                   float3 direction, float distance) {
  Stop any = find_stop(scene, origin, direction, distance, true);
  if (!any.found) {
    return float3(1.0f);
  }
  GpuMaterial any_source =
      scene.materials[scene.triangle_materials[any.triangle]];
  if (any_source.misc.w == 0.0f && uint(any_source.misc.z) != 2u) {
    return float3(0.0f);
  }
  float3 transmittance = float3(1.0f);
  for (uint skip = 0u; skip < kMaxSkips; ++skip) {
    Stop stop = find_stop(scene, origin, direction, distance, false);
    if (!stop.found) {
      return transmittance;
    }
    Material material = material_at(scene, stop.triangle, stop.bary);
    float alpha = saturate(material.base.w);
    bool cutout_pass = material.mode == 1u && alpha < saturate(material.cutoff);
    if (!cutout_pass) {
      if (!material.shadow_layer) {
        return float3(0.0f);
      }
      float3 glass = max(material.base.xyz, float3(0.0f)) *
                     saturate(material.transmission);
      transmittance *= material.mode == 2u ? (1.0f - alpha) + glass * alpha
                                           : glass;
      if (max3(transmittance.x, transmittance.y, transmittance.z) <= 1.0e-6f) {
        return float3(0.0f);
      }
    }
    origin += direction * stop.distance;
    distance -= stop.distance;
    if (distance <= kEpsilon) {
      return transmittance;
    }
  }
  return float3(0.0f);
}

struct LightSample {
  bool valid;
  float3 direction;
  float3 radiance;
  float distance;
  float weight;
};

/* The CPU integrator's evaluate_light and sample_rectangle_light. */
static LightSample sample_light(GpuLight light, float3 position,
                                thread Rng &rng) {
  LightSample sample;
  sample.valid = false;
  sample.weight = 1.0f;
  uint kind = uint(light.position_kind.w);
  if (light.cone.w == 0.0f) {
    return sample;
  }
  if (kind == 4u) {
    float radiance = light.attenuation.w;
    if (radiance <= 0.0f) {
      return sample;
    }
    float3 point = light.position_kind.xyz +
                   light.right_half_width.xyz *
                       ((2.0f * rng.next() - 1.0f) * light.right_half_width.w) +
                   light.up_half_height.xyz *
                       ((2.0f * rng.next() - 1.0f) * light.up_half_height.w);
    float3 to_light = point - position;
    float distance_squared = dot(to_light, to_light);
    if (!(distance_squared > 1.0e-12f)) {
      return sample;
    }
    float distance = sqrt(distance_squared);
    float3 direction = to_light / distance;
    float emitter_cosine = dot(light.direction_range.xyz, -direction);
    if (!(emitter_cosine > 0.0f)) {
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
  if (intensity <= 0.0f) {
    return sample;
  }
  if (kind == 0u) {
    sample.valid = true;
    sample.direction = normalize(-light.direction_range.xyz);
    sample.radiance = light.color_intensity.xyz * intensity;
    sample.distance = INFINITY;
    return sample;
  }
  float3 to_light = light.position_kind.xyz - position;
  float distance_squared = dot(to_light, to_light);
  if (!(distance_squared > 1.0e-12f)) {
    return sample;
  }
  float distance = sqrt(distance_squared);
  float range = light.direction_range.w;
  if (kind != 3u && range > 0.0f && distance > range) {
    return sample;
  }
  float3 direction = to_light / distance;
  float attenuation;
  if (kind == 3u) {
    attenuation = 1.0f / max(max(light.attenuation.x, 1.0f) +
                                 light.attenuation.y * distance +
                                 light.attenuation.z * distance_squared,
                             1.0e-6f);
  } else {
    float range_attenuation = 1.0f;
    if (range > 0.0f) {
      float ratio = distance / range;
      range_attenuation = saturate(1.0f - ratio * ratio * ratio * ratio);
      range_attenuation *= range_attenuation;
    }
    attenuation = range_attenuation / max(distance_squared, 1.0e-4f);
    if (kind == 2u) {
      float3 axis = normalize(light.direction_range.xyz);
      float inner = light.cone.x;
      float outer = light.cone.y;
      float cosine = dot(-direction, axis);
      float cone = inner > outer ? saturate((cosine - outer) / (inner - outer))
                                 : (cosine >= outer ? 1.0f : 0.0f);
      attenuation *= cone * cone;
    }
  }
  if (!(attenuation > 0.0f)) {
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
static float3 bounce_albedo(thread const Material &material, float no_v,
                            texture2d<float> dfg) {
  constexpr sampler dfg_sampler(coord::normalized, address::clamp_to_edge,
                                filter::linear);
  float metallic = saturate(material.metallic);
  float3 base = saturate(material.base.xyz);
  float3 f0 = saturate(material.dielectric_specular) * (1.0f - metallic) +
              base * metallic;
  float size = float(dfg.get_width());
  float2 coordinate = (float2(sqrt(saturate(no_v)),
                              saturate(material.roughness)) *
                           (size - 1.0f) +
                       0.5f) /
                      size;
  float2 split_sum = dfg.sample(dfg_sampler, coordinate, level(0.0f)).xy;
  float3 scale = 1.0f + f0 * (1.0f / max(split_sum.x + split_sum.y, 0.25f) -
                              1.0f);
  float f90 = saturate(max3(f0.x, f0.y, f0.z) * 25.0f);
  float3 reflectance = saturate((f0 * split_sum.x + f90 * split_sum.y) * scale);
  return reflectance + (1.0f - reflectance) * (1.0f - metallic) * base;
}

/* Length of a punctual light's shadow segment, next to the light, that no
   occluder blocks: the runtime's local shadow maps clip it as their near
   plane (vkr_local_shadow_system.c), so the bulb or socket mesh around an
   imported lamp does not shadow it. Directional and rectangle lights have
   none. */
static float punctual_occluder_clip(GpuLight light) {
  uint kind = uint(light.position_kind.w);
  if (kind == 0u || kind == 4u) {
    return 0.0f;
  }
  float range = light.direction_range.w;
  return range > 0.0f ? min(0.05f, range * 0.01f) : 0.05f;
}

/* A point just off a surface on the side `direction` leaves through. */
static float3 leave_surface(float3 position, float3 geometric,
                            float3 direction) {
  float side = dot(geometric, direction) >= 0.0f ? 1.0f : -1.0f;
  return position + geometric * (side * 1.0e-3f);
}

/* Shadow rays a texel's direct term casts toward each footprint point's
   view of a sphere light (ADR-108); bounce paths cast one. */
constant uint kSoftShadowRays = 4u;

/* The k-th of `count` points on a sphere light's disc facing the receiver:
   a golden-angle spiral of equal-area rings turned by `turn` radians. */
static float3 sphere_light_point(float3 center, float radius,
                                 float3 toward_receiver, uint k, uint count,
                                 float turn) {
  float3 axis = normalize(toward_receiver);
  float3 reference =
      abs(axis.y) < 0.9f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
  float3 u = normalize(cross(reference, axis));
  float3 v = cross(axis, u);
  float r = radius * sqrt((float(k) + 0.5f) / float(count));
  float angle = float(k) * 2.39996323f + turn;
  return center + (u * cos(angle) + v * sin(angle)) * r;
}

/* Direct irradiance from the layer lights [first_light, end_light) at a
   point with this normal. A point or spot light with an emitter radius is
   seen through `soft_rays` shadow rays across its disc, so its shadow takes
   the penumbra of a sphere light; its unshadowed light stays the point
   light's. As in the CPU integrator, a light above the shading
   normal counts even when it is below the geometric surface; its shadow ray
   leaves on the light's side. With a positive `direction_weight`, each
   light's contribution also adds its luminance times that weight, times its
   direction, to `direction_sum` (w the weighted luminance). */
static float3 direct_irradiance(thread const Scene &scene,
                                device const GpuLight *lights,
                                device const uint *layer_lights,
                                uint first_light, uint end_light,
                                float3 position, float3 normal,
                                float3 geometric, thread Rng &rng,
                                thread float4 &direction_sum,
                                float direction_weight, uint soft_rays) {
  float3 result = float3(0.0f);
  for (uint i = first_light; i < end_light; ++i) {
    GpuLight light = lights[layer_lights[i]];
    LightSample sample = sample_light(light, position, rng);
    if (!sample.valid) {
      continue;
    }
    float cosine = dot(normal, sample.direction);
    if (cosine <= 0.0f) {
      continue;
    }
    float3 visibility = float3(1.0f);
    bool rectangle = uint(light.position_kind.w) == 4u;
    float shadow_distance =
        isinf(sample.distance)
            ? INFINITY
            : sample.distance - 2.0e-3f - punctual_occluder_clip(light);
    float radius = light.source.x;
    if (!rectangle && radius > 0.0f && !isinf(sample.distance) &&
        light.cone.z != 0.0f && shadow_distance > 0.0f) {
      /* One random disc point, or a turned spiral of `soft_rays`. */
      uint count = max(soft_rays, 1u);
      float turn = 6.28318531f * rng.next();
      float3 center = position + sample.direction * sample.distance;
      float3 origin = leave_surface(position, geometric, sample.direction);
      visibility = float3(0.0f);
      for (uint k = 0u; k < count; ++k) {
        float3 target = sphere_light_point(
            center, radius, position - center,
            count > 1u ? k : uint(rng.next() * 64.0f), count > 1u ? count : 64u,
            turn);
        float3 to_target = target - origin;
        float target_distance = length(to_target);
        float3 direction = to_target / max(target_distance, 1.0e-6f);
        float ray_distance =
            target_distance - 2.0e-3f - punctual_occluder_clip(light);
        visibility += ray_distance > 0.0f
                          ? shadow_transmittance(scene, origin, direction,
                                                 ray_distance)
                          : float3(1.0f);
      }
      visibility /= float(count);
    } else if ((rectangle || light.cone.z != 0.0f) && shadow_distance > 0.0f) {
      float3 origin = leave_surface(position, geometric, sample.direction);
      visibility = shadow_transmittance(scene, origin, sample.direction,
                                        shadow_distance);
    }
    float3 contribution =
        sample.radiance * visibility * (cosine * sample.weight);
    result += contribution;
    if (direction_weight > 0.0f) {
      direction_sum +=
          float4(sample.direction, 1.0f) *
          (dot(contribution, float3(0.2126f, 0.7152f, 0.0722f)) *
           direction_weight);
    }
  }
  return result;
}

static float3 sky_radiance(texture2d<float> sky, sampler sky_sampler,
                           float3 direction) {
  float u = atan2(direction.z, direction.x) / (2.0f * kPi) + 0.5f;
  float v = acos(clamp(direction.y, -1.0f, 1.0f)) / kPi;
  return sky.sample(sky_sampler, float2(u, v), level(0.0f)).xyz;
}

static float luminance(float3 value) {
  return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

/* Whether `point`, on the texel's surface within its footprint, is the
   surface the texel sees: nothing lies between the texel's center and it,
   just off the surface. A point under a wall standing on the surface, or
   past a thin wall, is not, so its light does not leak into the texel. */
static bool footprint_reaches(thread const Scene &scene, float3 center,
                              float3 normal, float3 point) {
  float3 from = center + normal * 1.0e-3f;
  float3 to = point + normal * 1.0e-3f;
  float span = distance(from, to);
  if (!(span > 1.0e-5f)) {
    return true;
  }
  return !find_stop(scene, from, (to - from) / span, span, true).found;
}

/* One layer's gather at one texel, over a run of its samples. Each run adds
   its share of every mean; the first run starts them. Direct light from
   point, spot and directional lights is the same at every sample, so the
   first run evaluates it once, as the mean over a 3x3 grid of points across
   the texel's footprint, so a shadow edge crossing the texel leaves its
   covered share instead of all or nothing; rectangle lights take one point
   per sample, at a random point of the footprint. Footprint points the
   texel's center does not reach on its surface are left out. */
/* A layer's lights and transport settings, as path_radiance takes them. */
struct Transport {
  device const GpuLight *lights;
  device const uint *layer_lights;
  uint light_count;
  uint max_depth;
  uint rr_start_depth;
  bool use_sky;
  bool use_emission;
  texture2d<float> sky;
  sampler sky_sampler;
  texture2d<float> dfg;
};

/* The first surface a path meets: whether there is one, how far it is, its
   opacity (one minus its glass fraction) and whether the path reached the
   back of a one-sided surface. */
struct FirstHit {
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
static float3 path_radiance(thread const Scene &scene,
                            thread const Transport &transport, float3 origin,
                            float3 direction, thread Rng &rng,
                            thread FirstHit &first) {
  first.found = false;
  first.back_face = false;
  first.distance = 0.0f;
  first.opacity = 0.0f;
  float3 ray_origin = origin;
  float3 throughput = float3(1.0f);
  float3 radiance = float3(0.0f);
  float4 untracked = float4(0.0f);
  for (uint depth = 0u; depth < transport.max_depth; ++depth) {
    Hit hit = trace_surface(scene, ray_origin, direction, rng);
    if (depth == 0u && hit.found) {
      first.found = true;
      first.back_face = hit.back_face;
      first.distance = distance(hit.position, ray_origin);
      first.opacity = 1.0f - (1.0f - saturate(hit.material.metallic)) *
                                 saturate(hit.material.transmission);
    }
    if (!hit.found) {
      if (transport.use_sky) {
        radiance += throughput * sky_radiance(transport.sky,
                                              transport.sky_sampler, direction);
      }
      break;
    }
    if (transport.use_emission) {
      radiance += throughput * max(hit.material.emissive, float3(0.0f));
    }
    /* The glass fraction passes the path straight through, tinted by the
       base color (a thin-walled or slab approximation of refraction). */
    float glass = (1.0f - saturate(hit.material.metallic)) *
                  saturate(hit.material.transmission);
    if (glass > 0.0f && rng.next() < glass) {
      throughput *= max(hit.material.base.xyz, float3(0.0f));
      ray_origin = leave_surface(hit.position, hit.geometric, direction);
      continue;
    }
    float3 albedo = bounce_albedo(hit.material, dot(hit.normal, -direction),
                                  transport.dfg);
    radiance += throughput * albedo * (1.0f / kPi) *
                direct_irradiance(scene, transport.lights,
                                  transport.layer_lights, 0u,
                                  transport.light_count, hit.position,
                                  hit.normal, hit.geometric, rng, untracked,
                                  0.0f, 1u);
    throughput *= albedo;
    /* As in the CPU integrator, a direction sampled about the shading
       normal continues even below the geometric surface. */
    direction = cosine_direction(hit.normal, rng.next(), rng.next());
    ray_origin = leave_surface(hit.position, hit.geometric, direction);
    if (transport.rr_start_depth > 0u &&
        depth + 1u >= transport.rr_start_depth) {
      float survival = min(0.95f, max3(throughput.x, throughput.y,
                                       throughput.z));
      if (survival <= 0.0f || rng.next() >= survival) {
        break;
      }
      throughput /= survival;
    }
  }
  return radiance;
}

kernel void lightmap_gather(
    device const float4 *texel_positions [[buffer(0)]],
    device const float4 *texel_normals [[buffer(1)]],
    device float4 *indirect [[buffer(2)]],
    constant GatherArgs &args [[buffer(3)]],
    primitive_acceleration_structure as [[buffer(4)]],
    device const float *positions [[buffer(5)]],
    device const float *normals [[buffer(6)]],
    device const float2 *uvs [[buffer(7)]],
    device const uint *colors [[buffer(8)]],
    device const uint *triangle_materials [[buffer(9)]],
    device const GpuMaterial *materials [[buffer(10)]],
    device const GpuLight *lights [[buffer(11)]],
    device const uint *layer_lights [[buffer(12)]],
    device const uint *as_indices [[buffer(13)]],
    device float *occlusion [[buffer(14)]],
    device float4 *direct [[buffer(15)]],
    device float2 *moments [[buffer(16)]],
    device float *backface [[buffer(17)]],
    device const float4 *texel_footprints [[buffer(18)]],
    device float4 *direction_out [[buffer(19)]],
    texture2d_array<half> textures [[texture(0)]],
    texture2d<float> sky [[texture(1)]],
    texture2d<float> dfg [[texture(2)]],
    uint id [[thread_position_in_grid]]) {
  if (id >= args.texel_count) {
    return;
  }
  constexpr sampler texture_sampler(coord::normalized, address::repeat,
                                    filter::linear);
  constexpr sampler sky_sampler(coord::normalized, s_address::repeat,
                                t_address::clamp_to_edge, filter::linear);
  Scene scene = {as,
                 positions,
                 normals,
                 uvs,
                 colors,
                 triangle_materials,
                 materials,
                 textures,
                 texture_sampler,
                 as_indices,
                 {args.geometry_first[0], args.geometry_first[1]}};
  uint texel = args.first_texel + id;
  float3 texel_normal = texel_normals[texel].xyz;
  float3 texel_position = texel_positions[texel].xyz;
  float3 step_x = texel_footprints[2u * texel + 0u].xyz;
  float3 step_y = texel_footprints[2u * texel + 1u].xyz;
  float3 origin = texel_position + texel_normal * 1.0e-3f;
  Transport transport = {lights,
                         layer_lights,
                         args.light_count,
                         args.max_depth,
                         args.rr_start_depth,
                         (args.flags & 1u) != 0u,
                         (args.flags & 2u) != 0u,
                         sky,
                         sky_sampler,
                         dfg};
  bool texel_direct = (args.flags & 4u) != 0u;
  bool use_occlusion = (args.flags & 8u) != 0u;
  bool use_backface = (args.flags & 16u) != 0u;
  bool use_direction = (args.flags & 32u) != 0u;
  bool direct_only = (args.flags & 64u) != 0u;
  bool first_run = args.first_sample == 0u;
  float4 direction_sum = float4(0.0f);
  uint texel_seed = mix_seed(args.seed ^ mix_seed(texel));
  float inverse_samples = 1.0f / float(args.samples);

  float3 direct_total = float3(0.0f);
  if (texel_direct && first_run && args.rectangle_first > 0u) {
    /* These lights draw no random numbers. */
    Rng unused;
    unused.state = texel_seed;
    float points = 0.0f;
    float4 point_directions = float4(0.0f);
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        float3 point = texel_position + step_x * (float(x) / 3.0f) +
                       step_y * (float(y) / 3.0f);
        if ((x != 0 || y != 0) &&
            !footprint_reaches(scene, texel_position, texel_normal, point)) {
          continue;
        }
        direct_total += direct_irradiance(
            scene, lights, layer_lights, 0u, args.rectangle_first, point,
            texel_normal, texel_normal, unused, point_directions,
            use_direction ? 1.0f : 0.0f, kSoftShadowRays);
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
  for (uint s = args.first_sample;
       s < args.first_sample + args.sample_count; ++s) {
    Rng rng;
    rng.state = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
    if (texel_rectangles) {
      float3 point = texel_position + step_x * (rng.next() - 0.5f) +
                     step_y * (rng.next() - 0.5f);
      if (!footprint_reaches(scene, texel_position, texel_normal, point)) {
        point = texel_position;
      }
      direct_total +=
          inverse_samples *
          direct_irradiance(scene, lights, layer_lights, args.rectangle_first,
                            args.light_count, point, texel_normal,
                            texel_normal, rng, direction_sum,
                            use_direction ? inverse_samples : 0.0f, 1u);
    }
    float3 direction = cosine_direction(texel_normal, rng.next(), rng.next());
    if (direct_only) {
      /* A direct-only gather still finds buried texels by their first
         hits. */
      if (use_backface) {
        Hit hit = trace_surface(scene, origin, direction, rng);
        backface_hits += hit.found && hit.back_face ? 1.0f : 0.0f;
      }
      continue;
    }
    FirstHit first;
    float3 radiance = path_radiance(scene, transport, origin, direction, rng,
                                    first);
    if (first.found) {
      if (use_occlusion) {
        occluded += first.opacity *
                    saturate(1.0f - first.distance / args.occlusion_radius);
      }
      if (use_backface && first.back_face) {
        backface_hits += 1.0f;
      }
    }
    /* Cosine-weighted sampling: irradiance is pi times mean radiance. */
    float3 sample_irradiance = kPi * radiance;
    float sample_luminance = luminance(sample_irradiance);
    if (args.indirect_clamp > 0.0f &&
        sample_luminance > args.indirect_clamp) {
      sample_irradiance *= args.indirect_clamp / sample_luminance;
      sample_luminance = args.indirect_clamp;
    }
    indirect_total += sample_irradiance;
    if (use_direction) {
      direction_sum += float4(direction, 1.0f) *
                       (sample_luminance * inverse_samples);
    }
    luminance_sum += sample_luminance;
    luminance_squared_sum += sample_luminance * sample_luminance;
  }

  float3 indirect_share = indirect_total * inverse_samples;
  float2 moment_share =
      float2(luminance_sum, luminance_squared_sum) * inverse_samples;
  indirect[texel] = first_run ? float4(indirect_share, 1.0f)
                              : indirect[texel] + float4(indirect_share, 0.0f);
  direct[texel] = first_run ? float4(direct_total, 1.0f)
                            : direct[texel] + float4(direct_total, 0.0f);
  moments[texel] = first_run ? moment_share : moments[texel] + moment_share;
  if (use_occlusion) {
    float visibility =
        (float(args.sample_count) - occluded) * inverse_samples;
    occlusion[texel] = first_run ? visibility : occlusion[texel] + visibility;
  }
  if (use_direction) {
    direction_out[texel] =
        first_run ? direction_sum : direction_out[texel] + direction_sum;
  }
  if (use_backface) {
    float share = backface_hits * inverse_samples;
    backface[texel] = first_run ? share : backface[texel] + share;
  }
}

/* vkr_bake_cube_direction (tools/bake/vkr_bake_sh.cpp): KTX face order and
   the renderer's cube coordinates. */
static float3 cube_direction(uint face, float s, float t) {
  float3 direction;
  switch (face) {
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

/* Every member is a 32-bit scalar; the host's ProbeArgs matches it byte for
   byte. */
struct ProbeArgs {
  uint first_probe;
  uint probe_count;
  uint face_size;
  uint samples;
  /* This run's samples of each pixel. */
  uint first_sample;
  uint sample_count;
  uint max_depth;
  uint rr_start_depth;
  uint light_count;
  /* bit 0 sky, bit 1 emission. */
  uint flags;
  uint geometry_first[2];
};

/* One cube-face pixel of one probe: the mean radiance of `samples` paths
   through jittered points of the pixel, starting at the probe's center in
   free space. Seeds follow vkr_diffuse_baker's CPU bake: the probe's seed,
   the pixel and the sample. A run adds its samples to the pixel's slot of
   this dispatch, in sample order, and the last run leaves the mean, which
   the host projects to SH. */
kernel void probe_gather(
    device const float4 *probe_positions [[buffer(0)]],
    device const uint *probe_seeds [[buffer(1)]],
    device float4 *probe_radiance [[buffer(2)]],
    constant ProbeArgs &args [[buffer(3)]],
    primitive_acceleration_structure as [[buffer(4)]],
    device const float *positions [[buffer(5)]],
    device const float *normals [[buffer(6)]],
    device const float2 *uvs [[buffer(7)]],
    device const uint *colors [[buffer(8)]],
    device const uint *triangle_materials [[buffer(9)]],
    device const GpuMaterial *materials [[buffer(10)]],
    device const GpuLight *lights [[buffer(11)]],
    device const uint *layer_lights [[buffer(12)]],
    device const uint *as_indices [[buffer(13)]],
    texture2d_array<half> textures [[texture(0)]],
    texture2d<float> sky [[texture(1)]],
    texture2d<float> dfg [[texture(2)]],
    uint id [[thread_position_in_grid]]) {
  uint pixels = 6u * args.face_size * args.face_size;
  uint local_probe = id / pixels;
  if (local_probe >= args.probe_count) {
    return;
  }
  constexpr sampler texture_sampler(coord::normalized, address::repeat,
                                    filter::linear);
  constexpr sampler sky_sampler(coord::normalized, s_address::repeat,
                                t_address::clamp_to_edge, filter::linear);
  Scene scene = {as,
                 positions,
                 normals,
                 uvs,
                 colors,
                 triangle_materials,
                 materials,
                 textures,
                 texture_sampler,
                 as_indices,
                 {args.geometry_first[0], args.geometry_first[1]}};
  Transport transport = {lights,
                         layer_lights,
                         args.light_count,
                         args.max_depth,
                         args.rr_start_depth,
                         (args.flags & 1u) != 0u,
                         (args.flags & 2u) != 0u,
                         sky,
                         sky_sampler,
                         dfg};
  uint pixel = id - local_probe * pixels;
  uint probe = args.first_probe + local_probe;
  uint face = pixel / (args.face_size * args.face_size);
  uint in_face = pixel - face * args.face_size * args.face_size;
  uint y = in_face / args.face_size;
  uint x = in_face - y * args.face_size;
  float3 origin = probe_positions[probe].xyz;
  uint base_seed = probe_seeds[probe];
  float face_size = float(args.face_size);

  float3 sum =
      args.first_sample == 0u ? float3(0.0f) : probe_radiance[id].xyz;
  for (uint s = args.first_sample; s < args.first_sample + args.sample_count;
       ++s) {
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
    sum += path_radiance(scene, transport, origin, direction, rng, first);
  }
  bool last_run = args.first_sample + args.sample_count == args.samples;
  probe_radiance[id] =
      float4(last_run ? sum / float(args.samples) : sum, 0.0f);
}
)METAL";

struct BenchmarkArgs {
  uint32_t first_texel;
  uint32_t texel_count;
  uint32_t samples;
  uint32_t seed;
};

struct GatherArgs {
  uint32_t first_texel;
  uint32_t texel_count;
  uint32_t samples;
  uint32_t first_sample;
  uint32_t sample_count;
  uint32_t seed;
  uint32_t max_depth;
  uint32_t rr_start_depth;
  uint32_t light_count;
  uint32_t rectangle_first;
  uint32_t flags;
  uint32_t geometry_first[2];
  float occlusion_radius;
  float indirect_clamp;
};
static_assert(sizeof(GatherArgs) == 60u, "GatherArgs matches the kernel");

struct ProbeArgs {
  uint32_t first_probe;
  uint32_t probe_count;
  uint32_t face_size;
  uint32_t samples;
  uint32_t first_sample;
  uint32_t sample_count;
  uint32_t max_depth;
  uint32_t rr_start_depth;
  uint32_t light_count;
  uint32_t flags;
  uint32_t geometry_first[2];
};
static_assert(sizeof(ProbeArgs) == 48u, "ProbeArgs matches the kernel");

struct GpuMaterial {
  float base_color[4];
  float emissive_metallic[4];
  float misc[4];
  float specular[4];
  int32_t layers[4];
  float surface[4];
};
static_assert(sizeof(GpuMaterial) == 96u, "GpuMaterial matches the kernel");

struct GpuLight {
  float position_kind[4];
  float direction_range[4];
  float color_intensity[4];
  float attenuation[4];
  float right_half_width[4];
  float up_half_height[4];
  float cone[4];
  float source[4];
};

uint32_t pack_unorm4(Vec4 color) {
  auto channel = [](float value) {
    const float clamped = std::fmin(std::fmax(value, 0.0f), 1.0f);
    return (uint32_t)std::lround(clamped * 255.0f);
  };
  return channel(color.x) | (channel(color.y) << 8u) |
         (channel(color.z) << 16u) | (channel(color.w) << 24u);
}

id<MTLBuffer> shared_buffer(id<MTLDevice> device, NSUInteger length) {
  return [device newBufferWithLength:std::max<NSUInteger>(length, 16u)
                             options:MTLResourceStorageModeShared];
}

} // namespace

struct VkrBakeGpuContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLAccelerationStructure> scene = nil;
  id<MTLComputePipelineState> benchmark = nil;
  id<MTLComputePipelineState> gather = nil;
  id<MTLComputePipelineState> probes = nil;
  id<MTLBuffer> positions = nil;
  id<MTLBuffer> normals = nil;
  id<MTLBuffer> uvs = nil;
  id<MTLBuffer> colors = nil;
  id<MTLBuffer> triangle_materials = nil;
  id<MTLBuffer> materials = nil;
  id<MTLBuffer> lights = nil;
  /* Corner indices of the acceleration structure, grouped by geometry. */
  id<MTLBuffer> as_indices = nil;
  /* Per material, the lowest alpha its factor and base color texture give,
     as the kernel samples them. */
  std::vector<float> material_alpha_floor;
  uint32_t geometry_first[2] = {0u, 0u};
  id<MTLTexture> textures = nil;
  id<MTLTexture> sky = nil;
  id<MTLTexture> dfg = nil;
};

bool vkr_bake_gpu_available() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device && device.supportsRaytracing;
  }
}

namespace {

/* Whether a triangle needs its alpha tested during traversal: a cutout
   triangle whose lowest alpha (material floor times its lowest vertex alpha,
   which bounds the interpolated one) falls below the cutoff. Blended surfaces
   always stop a traversal and are resolved after it, so they gain nothing
   from testing. */
bool triangle_alpha_tested(const VkrBakeGpuContext *context,
                           const VkrBakeScene &scene, uint32_t triangle) {
  const VkrBakeTriangle &source = scene.triangles[triangle];
  const uint32_t material = source.material_index;
  if (material >= scene.materials.size() ||
      scene.materials[material].alpha_mode != VKR_BAKE_MATERIAL_ALPHA_CUTOUT) {
    return false;
  }
  float vertex_alpha = 1.0f;
  for (uint32_t c = 0u; c < 3u; ++c) {
    /* The kernel reads vertex colors as RGBA8. */
    const uint32_t packed = pack_unorm4(source.vertex[c].color);
    vertex_alpha = std::fmin(vertex_alpha, (float)(packed >> 24u) / 255.0f);
  }
  const float cutoff =
      std::fmin(std::fmax(scene.materials[material].alpha_cutoff, 0.0f), 1.0f);
  return context->material_alpha_floor[material] * vertex_alpha < cutoff;
}

/* Two geometries over the corner positions: opaque triangles, which commit
   during traversal, then cutout triangles that can pass, which the kernels
   test as candidates. An empty group gets no geometry. */
bool build_acceleration_structure(VkrBakeGpuContext *context,
                                  const VkrBakeScene &scene) {
  const uint32_t triangle_count = (uint32_t)scene.triangles.size();
  context->as_indices = shared_buffer(
      context->device, (NSUInteger)triangle_count * 3u * sizeof(uint32_t));
  if (!context->as_indices) {
    return false;
  }
  uint32_t *indices = static_cast<uint32_t *>(context->as_indices.contents);
  uint32_t written = 0u;
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    context->geometry_first[pass] = written;
    for (uint32_t t = 0u; t < triangle_count; ++t) {
      const bool alpha = triangle_alpha_tested(context, scene, t);
      if (alpha != (pass == 1u)) {
        continue;
      }
      indices[3u * written + 0u] = 3u * t;
      indices[3u * written + 1u] = 3u * t + 1u;
      indices[3u * written + 2u] = 3u * t + 2u;
      ++written;
    }
  }
  const uint32_t counts[2] = {context->geometry_first[1],
                              triangle_count - context->geometry_first[1]};
  NSMutableArray *geometries = [NSMutableArray array];
  for (uint32_t g = 0u; g < 2u; ++g) {
    if (counts[g] == 0u) {
      continue;
    }
    MTLAccelerationStructureTriangleGeometryDescriptor *geometry =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    geometry.vertexBuffer = context->positions;
    geometry.vertexStride = 3u * sizeof(float);
    geometry.vertexFormat = MTLAttributeFormatFloat3;
    geometry.indexBuffer = context->as_indices;
    geometry.indexType = MTLIndexTypeUInt32;
    geometry.indexBufferOffset =
        (NSUInteger)context->geometry_first[g] * 3u * sizeof(uint32_t);
    geometry.triangleCount = counts[g];
    geometry.opaque = g == 0u ? YES : NO;
    [geometries addObject:geometry];
  }
  /* With one group, geometry 0 is that group. */
  if (counts[0] == 0u) {
    context->geometry_first[0] = context->geometry_first[1];
  }
  MTLPrimitiveAccelerationStructureDescriptor *descriptor =
      [MTLPrimitiveAccelerationStructureDescriptor descriptor];
  descriptor.geometryDescriptors = geometries;
  const MTLAccelerationStructureSizes sizes =
      [context->device accelerationStructureSizesWithDescriptor:descriptor];
  context->scene = [context->device
      newAccelerationStructureWithSize:sizes.accelerationStructureSize];
  id<MTLBuffer> scratch =
      [context->device newBufferWithLength:sizes.buildScratchBufferSize
                                   options:MTLResourceStorageModePrivate];
  if (!context->scene || !scratch) {
    return false;
  }
  id<MTLCommandBuffer> command = [context->queue commandBuffer];
  id<MTLAccelerationStructureCommandEncoder> encoder =
      [command accelerationStructureCommandEncoder];
  [encoder buildAccelerationStructure:context->scene
                           descriptor:descriptor
                        scratchBuffer:scratch
                  scratchBufferOffset:0u];
  [encoder endEncoding];
  [command commit];
  [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted) {
    return false;
  }
  std::printf("gpu_acceleration_structure_mb=%.1f build_ms=%.1f "
              "alpha_tested_triangles=%u\n",
              sizes.accelerationStructureSize / 1048576.0,
              (command.GPUEndTime - command.GPUStartTime) * 1000.0, counts[1]);
  return true;
}

/* Corner attributes, triangle materials and positions, three corners per
   triangle in the scene's (BVH-partitioned) triangle order. */
bool upload_triangles(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  const NSUInteger count = scene.triangles.size();
  context->positions =
      shared_buffer(context->device, count * 9u * sizeof(float));
  context->normals = shared_buffer(context->device, count * 9u * sizeof(float));
  context->uvs = shared_buffer(context->device, count * 6u * sizeof(float));
  context->colors =
      shared_buffer(context->device, count * 3u * sizeof(uint32_t));
  context->triangle_materials =
      shared_buffer(context->device, count * sizeof(uint32_t));
  if (!context->positions || !context->normals || !context->uvs ||
      !context->colors || !context->triangle_materials) {
    return false;
  }
  float *positions = static_cast<float *>(context->positions.contents);
  float *normals = static_cast<float *>(context->normals.contents);
  float *uvs = static_cast<float *>(context->uvs.contents);
  uint32_t *colors = static_cast<uint32_t *>(context->colors.contents);
  uint32_t *materials =
      static_cast<uint32_t *>(context->triangle_materials.contents);
  for (NSUInteger t = 0u; t < count; ++t) {
    const VkrBakeTriangle &triangle = scene.triangles[t];
    for (uint32_t c = 0u; c < 3u; ++c) {
      const VkrBakeVertex &vertex = triangle.vertex[c];
      positions[9u * t + 3u * c + 0u] = vertex.position.x;
      positions[9u * t + 3u * c + 1u] = vertex.position.y;
      positions[9u * t + 3u * c + 2u] = vertex.position.z;
      normals[9u * t + 3u * c + 0u] = vertex.normal.x;
      normals[9u * t + 3u * c + 1u] = vertex.normal.y;
      normals[9u * t + 3u * c + 2u] = vertex.normal.z;
      uvs[6u * t + 2u * c + 0u] = vertex.uv.x;
      uvs[6u * t + 2u * c + 1u] = vertex.uv.y;
      colors[3u * t + c] = pack_unorm4(vertex.color);
    }
    materials[t] = triangle.material_index;
  }
  return true;
}

/* Materials and their base color and emission textures, resampled into one
   RGBA16F array of kTextureEdge layers. */
bool upload_materials(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  std::map<std::pair<uint32_t, bool>, int32_t> layer_of;
  std::vector<VkrBakeMaterialTextureRef> layer_refs;
  auto layer_for = [&](VkrBakeMaterialTextureRef ref) -> int32_t {
    if (!ref.present) {
      return -1;
    }
    const auto key = std::make_pair(ref.texture_index, (bool)ref.srgb);
    const auto found = layer_of.find(key);
    if (found != layer_of.end()) {
      return found->second;
    }
    const int32_t layer = (int32_t)layer_refs.size();
    layer_of.emplace(key, layer);
    layer_refs.push_back(ref);
    return layer;
  };
  const NSUInteger count = std::max<size_t>(scene.materials.size(), 1u);
  context->materials =
      shared_buffer(context->device, count * sizeof(GpuMaterial));
  if (!context->materials) {
    return false;
  }
  GpuMaterial *gpu = static_cast<GpuMaterial *>(context->materials.contents);
  for (size_t m = 0u; m < scene.materials.size(); ++m) {
    const VkrBakeMaterial &material = scene.materials[m];
    gpu[m] = {
        {material.base_color.x, material.base_color.y, material.base_color.z,
         material.base_color.w},
        {material.emissive_factor.x, material.emissive_factor.y,
         material.emissive_factor.z, material.metallic},
        {material.alpha_cutoff, material.transmission_factor,
         (float)material.alpha_mode,
         (material.transmission_factor > 0.0f &&
          material.thickness_factor <= 0.0f) ||
                 std::fabs(std::fmax(material.ior, 1.0f) - 1.0f) <= 1.0e-4f
             ? 1.0f
             : 0.0f},
        {material.dielectric_specular.x, material.dielectric_specular.y,
         material.dielectric_specular.z, material.roughness},
        {layer_for(material.textures[VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR]),
         layer_for(material.textures[VKR_BAKE_MATERIAL_TEXTURE_EMISSIVE]),
         layer_for(
             material.textures[VKR_BAKE_MATERIAL_TEXTURE_METALLIC_ROUGHNESS]),
         layer_for(material.textures[VKR_BAKE_MATERIAL_TEXTURE_TRANSMISSION])},
        {material.double_sided ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
  }

  const NSUInteger layers = std::max<size_t>(layer_refs.size(), 1u);
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
  descriptor.textureType = MTLTextureType2DArray;
  descriptor.pixelFormat = MTLPixelFormatRGBA16Float;
  descriptor.width = kTextureEdge;
  descriptor.height = kTextureEdge;
  descriptor.arrayLength = layers;
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  context->textures = [context->device newTextureWithDescriptor:descriptor];
  if (!context->textures) {
    return false;
  }
  std::vector<float> rgba(4u * kTextureEdge * kTextureEdge, 1.0f);
  std::vector<uint16_t> halves(rgba.size());
  std::vector<float> layer_min_alpha(layers, 1.0f);
  for (NSUInteger layer = 0u; layer < layers; ++layer) {
    if (layer < layer_refs.size() &&
        !vkr_bake_texture_store_resample(scene.texture_store, layer_refs[layer],
                                         kTextureEdge, kTextureEdge,
                                         rgba.data())) {
      return false;
    }
    for (size_t i = 0u; i < rgba.size(); ++i) {
      _Float16 half = (_Float16)rgba[i];
      std::memcpy(&halves[i], &half, sizeof(uint16_t));
      if (i % 4u == 3u) {
        layer_min_alpha[layer] = std::fmin(layer_min_alpha[layer], (float)half);
      }
    }
    [context->textures
        replaceRegion:MTLRegionMake2D(0u, 0u, kTextureEdge, kTextureEdge)
          mipmapLevel:0u
                slice:layer
            withBytes:halves.data()
          bytesPerRow:kTextureEdge * 4u * sizeof(uint16_t)
        bytesPerImage:0u];
  }
  /* Bilinear filtering stays within its texels, so a layer's lowest texel
     alpha bounds every sample. */
  context->material_alpha_floor.assign(scene.materials.size(), 1.0f);
  for (size_t m = 0u; m < scene.materials.size(); ++m) {
    const int32_t layer = gpu[m].layers[0];
    context->material_alpha_floor[m] =
        std::fmax(scene.materials[m].base_color.w, 0.0f) *
        (layer >= 0 ? layer_min_alpha[(size_t)layer] : 1.0f);
  }
  std::printf("gpu_materials=%zu texture_layers=%zu\n", scene.materials.size(),
              layer_refs.size());
  return true;
}

bool upload_lights(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  const NSUInteger count = std::max<size_t>(scene.lights.size(), 1u);
  context->lights = shared_buffer(context->device, count * sizeof(GpuLight));
  if (!context->lights) {
    return false;
  }
  GpuLight *gpu = static_cast<GpuLight *>(context->lights.contents);
  for (size_t i = 0u; i < scene.lights.size(); ++i) {
    const VkrBakeSceneLight &light = scene.lights[i];
    gpu[i] = {
        {light.position.x, light.position.y, light.position.z,
         (float)(uint8_t)light.kind},
        {light.direction.x, light.direction.y, light.direction.z, light.range},
        {light.color.x, light.color.y, light.color.z, light.intensity},
        {light.constant, light.linear, light.quadratic, light.radiance},
        {light.right.x, light.right.y, light.right.z, light.half_width},
        {light.up.x, light.up.y, light.up.z, light.half_height},
        // cone.z asks the kernel for shadow rays. Every baked light is
        // static and reaches the screen only through its bake, so it is
        // shadowed whatever casts_shadow says about runtime shadow maps;
        // an unshadowed lamp lit the far side of every wall.
        {std::cos(light.inner_cone_angle), std::cos(light.outer_cone_angle),
         1.0f, light.enabled ? 1.0f : 0.0f},
        {light.source_radius, 0.0f, 0.0f, 0.0f}};
  }
  return true;
}

/* The renderer's split-sum table, as the CPU BSDF reads it. */
bool upload_dfg(VkrBakeGpuContext *context) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                   width:VKR_DFG_LUT_SIZE
                                  height:VKR_DFG_LUT_SIZE
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  context->dfg = [context->device newTextureWithDescriptor:descriptor];
  if (!context->dfg) {
    return false;
  }
  [context->dfg
      replaceRegion:MTLRegionMake2D(0u, 0u, VKR_DFG_LUT_SIZE, VKR_DFG_LUT_SIZE)
        mipmapLevel:0u
          withBytes:vkr_dfg_lut_pixels
        bytesPerRow:VKR_DFG_LUT_SIZE * 2u * sizeof(uint16_t)];
  return true;
}

/* Equirectangular sky from the CPU scene environment, so escaped paths see
   the same radiance as the reference integrator up to filtering. */
bool upload_sky(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                   width:kSkyWidth
                                  height:kSkyHeight
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  context->sky = [context->device newTextureWithDescriptor:descriptor];
  if (!context->sky) {
    return false;
  }
  std::vector<float> rgba(4u * kSkyWidth * kSkyHeight, 0.0f);
  for (uint32_t y = 0u; y < kSkyHeight; ++y) {
    for (uint32_t x = 0u; x < kSkyWidth; ++x) {
      const float phi = ((x + 0.5f) / kSkyWidth - 0.5f) * 2.0f * kPi;
      const float theta = (y + 0.5f) / kSkyHeight * kPi;
      const Vec3 direction =
          vec3_new(std::sin(theta) * std::cos(phi), std::cos(theta),
                   std::sin(theta) * std::sin(phi));
      const Vec3 radiance =
          vkr_bake_scene_sample_environment(&scene, direction);
      float *texel = &rgba[4u * (y * kSkyWidth + x)];
      texel[0] = radiance.x;
      texel[1] = radiance.y;
      texel[2] = radiance.z;
      texel[3] = 1.0f;
    }
  }
  [context->sky replaceRegion:MTLRegionMake2D(0u, 0u, kSkyWidth, kSkyHeight)
                  mipmapLevel:0u
                    withBytes:rgba.data()
                  bytesPerRow:kSkyWidth * 4u * sizeof(float)];
  return true;
}

id<MTLComputePipelineState>
make_pipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString *name) {
  NSError *error = nil;
  id<MTLFunction> function = [library newFunctionWithName:name];
  id<MTLComputePipelineState> pipeline =
      function
          ? [device newComputePipelineStateWithFunction:function error:&error]
          : nil;
  if (!pipeline) {
    std::fprintf(stderr, "Lightmap pipeline %s failed: %s\n", name.UTF8String,
                 error ? error.localizedDescription.UTF8String : "missing");
  }
  return pipeline;
}

} // namespace

VkrBakeGpuContext *vkr_bake_gpu_create(const VkrBakeScene &scene) {
  @autoreleasepool {
    if (scene.triangles.empty()) {
      return nullptr;
    }
    VkrBakeGpuContext *context = new (std::nothrow) VkrBakeGpuContext();
    if (!context) {
      return nullptr;
    }
    context->device = MTLCreateSystemDefaultDevice();
    if (!context->device || !context->device.supportsRaytracing) {
      delete context;
      return nullptr;
    }
    context->queue = [context->device newCommandQueue];
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> library =
        [context->device newLibraryWithSource:@(kKernelSource)
                                      options:options
                                        error:&error];
    if (!library) {
      std::fprintf(stderr, "Lightmap kernel compile failed: %s\n",
                   error ? error.localizedDescription.UTF8String : "unknown");
      delete context;
      return nullptr;
    }
    context->benchmark =
        make_pipeline(context->device, library, @"lightmap_trace_benchmark");
    context->gather =
        make_pipeline(context->device, library, @"lightmap_gather");
    context->probes = make_pipeline(context->device, library, @"probe_gather");
    if (!context->benchmark || !context->gather || !context->probes ||
        !upload_triangles(context, scene) ||
        !upload_materials(context, scene) ||
        !build_acceleration_structure(context, scene) ||
        !upload_lights(context, scene) || !upload_sky(context, scene) ||
        !upload_dfg(context)) {
      std::fprintf(stderr, "Lightmap GPU scene upload failed\n");
      delete context;
      return nullptr;
    }
    return context;
  }
}

void vkr_bake_gpu_destroy(VkrBakeGpuContext *context) { delete context; }

bool vkr_bake_gpu_update_lighting(VkrBakeGpuContext *context,
                                  const VkrBakeScene &scene) {
  @autoreleasepool {
    return context && upload_lights(context, scene) &&
           upload_sky(context, scene);
  }
}

namespace {

/* Texel positions and normals as float4 arrays. */
/* Texel centers and normals, and when `out_footprints` is set the two
   footprint steps of each texel. */
bool upload_texels(id<MTLDevice> device,
                   const std::vector<VkrBakeLightmapTexel> &texels,
                   id<MTLBuffer> *out_positions, id<MTLBuffer> *out_normals,
                   id<MTLBuffer> *out_footprints = nullptr) {
  const NSUInteger count = texels.size();
  *out_positions = shared_buffer(device, count * 4u * sizeof(float));
  *out_normals = shared_buffer(device, count * 4u * sizeof(float));
  if (!*out_positions || !*out_normals) {
    return false;
  }
  if (out_footprints) {
    *out_footprints = shared_buffer(device, count * 8u * sizeof(float));
    if (!*out_footprints) {
      return false;
    }
    float *f = static_cast<float *>((*out_footprints).contents);
    for (NSUInteger i = 0u; i < count; ++i) {
      const VkrBakeLightmapTexel &texel = texels[i];
      const Vec3 steps[2] = {texel.step_x, texel.step_y};
      for (uint32_t k = 0u; k < 2u; ++k) {
        f[8u * i + 4u * k + 0u] = steps[k].x;
        f[8u * i + 4u * k + 1u] = steps[k].y;
        f[8u * i + 4u * k + 2u] = steps[k].z;
        f[8u * i + 4u * k + 3u] = 0.0f;
      }
    }
  }
  float *p = static_cast<float *>((*out_positions).contents);
  float *n = static_cast<float *>((*out_normals).contents);
  for (NSUInteger i = 0u; i < count; ++i) {
    const VkrBakeLightmapTexel &texel = texels[i];
    p[4u * i + 0u] = texel.position.x;
    p[4u * i + 1u] = texel.position.y;
    p[4u * i + 2u] = texel.position.z;
    p[4u * i + 3u] = 1.0f;
    n[4u * i + 0u] = texel.normal.x;
    n[4u * i + 1u] = texel.normal.y;
    n[4u * i + 2u] = texel.normal.z;
    n[4u * i + 3u] = 0.0f;
  }
  return true;
}

bool finish(id<MTLCommandBuffer> command, double *in_out_seconds) {
  [command commit];
  [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted) {
    std::fprintf(stderr, "Lightmap dispatch failed: %s\n",
                 command.error.localizedDescription.UTF8String);
    return false;
  }
  *in_out_seconds += command.GPUEndTime - command.GPUStartTime;
  return true;
}

} // namespace

bool vkr_bake_gpu_trace_benchmark(
    VkrBakeGpuContext *context, const std::vector<VkrBakeLightmapTexel> &texels,
    uint32_t samples, uint32_t seed, std::vector<float32_t> *out_hit_fraction,
    double *out_gpu_seconds) {
  @autoreleasepool {
    if (!context || !out_hit_fraction || !out_gpu_seconds || samples == 0u) {
      return false;
    }
    const NSUInteger count = texels.size();
    out_hit_fraction->assign(count, 0.0f);
    *out_gpu_seconds = 0.0;
    if (count == 0u) {
      return true;
    }
    id<MTLBuffer> positions = nil;
    id<MTLBuffer> normals = nil;
    id<MTLBuffer> hits = shared_buffer(context->device, count * sizeof(float));
    if (!hits ||
        !upload_texels(context->device, texels, &positions, &normals)) {
      return false;
    }
    const NSUInteger width = context->benchmark.threadExecutionWidth;
    for (NSUInteger first = 0u; first < count; first += kTexelsPerDispatch) {
      const BenchmarkArgs args = {
          (uint32_t)first,
          (uint32_t)std::min<NSUInteger>(kTexelsPerDispatch, count - first),
          samples, seed};
      id<MTLCommandBuffer> command = [context->queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:context->benchmark];
      [encoder setBuffer:positions offset:0u atIndex:0u];
      [encoder setBuffer:normals offset:0u atIndex:1u];
      [encoder setBuffer:hits offset:0u atIndex:2u];
      [encoder setBytes:&args length:sizeof(args) atIndex:3u];
      [encoder setAccelerationStructure:context->scene atBufferIndex:4u];
      [encoder useResource:context->scene usage:MTLResourceUsageRead];
      [encoder dispatchThreads:MTLSizeMake(args.texel_count, 1u, 1u)
          threadsPerThreadgroup:MTLSizeMake(width, 1u, 1u)];
      [encoder endEncoding];
      if (!finish(command, out_gpu_seconds)) {
        return false;
      }
    }
    const float *result = static_cast<const float *>(hits.contents);
    std::copy(result, result + count, out_hit_fraction->begin());
    return true;
  }
}

bool vkr_bake_gpu_gather(VkrBakeGpuContext *context,
                         const std::vector<VkrBakeLightmapTexel> &texels,
                         const VkrBakeGpuLayer &layer,
                         const VkrBakeGpuGatherSettings &settings,
                         VkrBakeGpuGatherResult *out_result) {
  @autoreleasepool {
    if (!context || !out_result || settings.samples == 0u ||
        settings.max_depth == 0u ||
        (settings.occlusion && !(settings.occlusion_radius > 0.0f)) ||
        !(settings.indirect_clamp >= 0.0f)) {
      return false;
    }
    const NSUInteger count = texels.size();
    VkrBakeGpuGatherResult &result = *out_result;
    result.direct.assign(count, vec3_zero());
    result.indirect.assign(count, vec3_zero());
    result.indirect_variance.assign(count, 0.0f);
    result.occlusion.assign(settings.occlusion ? count : 0u, 1.0f);
    result.backface.assign(settings.backface ? count : 0u, 0.0f);
    result.direction.assign(settings.direction ? count : 0u, vec4_zero());
    result.gpu_seconds = 0.0;
    if (count == 0u) {
      return true;
    }
    id<MTLBuffer> positions = nil;
    id<MTLBuffer> normals = nil;
    id<MTLBuffer> indirect =
        shared_buffer(context->device, count * 4u * sizeof(float));
    id<MTLBuffer> direct =
        shared_buffer(context->device, count * 4u * sizeof(float));
    id<MTLBuffer> moments =
        shared_buffer(context->device, count * 2u * sizeof(float));
    id<MTLBuffer> layer_lights =
        shared_buffer(context->device, layer.lights.size() * sizeof(uint32_t));
    /* A buffer the gather does not fill is bound to a placeholder. */
    id<MTLBuffer> occlusion =
        settings.occlusion ? shared_buffer(context->device, count * sizeof(float))
                           : moments;
    id<MTLBuffer> backface =
        settings.backface ? shared_buffer(context->device, count * sizeof(float))
                          : moments;
    id<MTLBuffer> direction =
        settings.direction
            ? shared_buffer(context->device, count * 4u * sizeof(float))
            : moments;
    id<MTLBuffer> footprints = nil;
    if (!indirect || !direct || !moments || !layer_lights || !occlusion ||
        !backface || !direction ||
        !upload_texels(context->device, texels, &positions, &normals,
                       &footprints)) {
      return false;
    }
    /* Lights the texel evaluates once come first, then the rectangles it
       samples per sample. */
    const GpuLight *gpu_lights =
        static_cast<const GpuLight *>(context->lights.contents);
    std::vector<uint32_t> ordered = layer.lights;
    const auto rectangles = std::stable_partition(
        ordered.begin(), ordered.end(), [&](uint32_t light) {
          return (uint32_t)gpu_lights[light].position_kind[3] !=
                 (uint32_t)VkrBakeSceneLightKind::Rectangle;
        });
    const uint32_t rectangle_first = (uint32_t)(rectangles - ordered.begin());
    if (!ordered.empty()) {
      std::memcpy(layer_lights.contents, ordered.data(),
                  ordered.size() * sizeof(uint32_t));
    }
    const uint32_t flags =
        (layer.sky ? 1u : 0u) | (layer.emission ? 2u : 0u) |
        (layer.texel_direct ? 4u : 0u) | (settings.occlusion ? 8u : 0u) |
        (settings.backface ? 16u : 0u) | (settings.direction ? 32u : 0u) |
        (settings.direct_only ? 64u : 0u);
    const NSUInteger width = context->gather.threadExecutionWidth;
    /* Each batch of texels takes its samples in runs of
       kSamplesPerDispatch, a command buffer each. */
    const uint32_t runs =
        (settings.samples + kSamplesPerDispatch - 1u) / kSamplesPerDispatch;
    const NSUInteger batches =
        (count + kTexelsPerDispatch - 1u) / kTexelsPerDispatch;
    for (NSUInteger dispatch = 0u; dispatch < batches * runs; ++dispatch) {
      const NSUInteger first = (dispatch / runs) * kTexelsPerDispatch;
      const uint32_t first_sample =
          (uint32_t)(dispatch % runs) * kSamplesPerDispatch;
      const GatherArgs args = {
          (uint32_t)first,
          (uint32_t)std::min<NSUInteger>(kTexelsPerDispatch, count - first),
          settings.samples,
          first_sample,
          std::min(kSamplesPerDispatch, settings.samples - first_sample),
          settings.seed,
          settings.max_depth,
          settings.rr_start_depth,
          (uint32_t)layer.lights.size(),
          rectangle_first,
          flags,
          {context->geometry_first[0], context->geometry_first[1]},
          settings.occlusion_radius,
          settings.indirect_clamp};
      id<MTLCommandBuffer> command = [context->queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:context->gather];
      [encoder setBuffer:positions offset:0u atIndex:0u];
      [encoder setBuffer:normals offset:0u atIndex:1u];
      [encoder setBuffer:indirect offset:0u atIndex:2u];
      [encoder setBytes:&args length:sizeof(args) atIndex:3u];
      [encoder setAccelerationStructure:context->scene atBufferIndex:4u];
      [encoder setBuffer:context->positions offset:0u atIndex:5u];
      [encoder setBuffer:context->normals offset:0u atIndex:6u];
      [encoder setBuffer:context->uvs offset:0u atIndex:7u];
      [encoder setBuffer:context->colors offset:0u atIndex:8u];
      [encoder setBuffer:context->triangle_materials offset:0u atIndex:9u];
      [encoder setBuffer:context->materials offset:0u atIndex:10u];
      [encoder setBuffer:context->lights offset:0u atIndex:11u];
      [encoder setBuffer:layer_lights offset:0u atIndex:12u];
      [encoder setBuffer:context->as_indices offset:0u atIndex:13u];
      [encoder setBuffer:occlusion offset:0u atIndex:14u];
      [encoder setBuffer:direct offset:0u atIndex:15u];
      [encoder setBuffer:moments offset:0u atIndex:16u];
      [encoder setBuffer:backface offset:0u atIndex:17u];
      [encoder setBuffer:footprints offset:0u atIndex:18u];
      [encoder setBuffer:direction offset:0u atIndex:19u];
      [encoder setTexture:context->textures atIndex:0u];
      [encoder setTexture:context->sky atIndex:1u];
      [encoder setTexture:context->dfg atIndex:2u];
      [encoder useResource:context->scene usage:MTLResourceUsageRead];
      [encoder dispatchThreads:MTLSizeMake(args.texel_count, 1u, 1u)
          threadsPerThreadgroup:MTLSizeMake(width, 1u, 1u)];
      [encoder endEncoding];
      if (!finish(command, &result.gpu_seconds)) {
        return false;
      }
    }
    const float *indirect_values = static_cast<const float *>(indirect.contents);
    const float *direct_values = static_cast<const float *>(direct.contents);
    const float *moment_values = static_cast<const float *>(moments.contents);
    for (NSUInteger i = 0u; i < count; ++i) {
      result.indirect[i] =
          vec3_new(indirect_values[4u * i], indirect_values[4u * i + 1u],
                   indirect_values[4u * i + 2u]);
      result.direct[i] =
          vec3_new(direct_values[4u * i], direct_values[4u * i + 1u],
                   direct_values[4u * i + 2u]);
      /* The variance of a mean of n samples is the sample variance over n. */
      const float mean = moment_values[2u * i];
      const float mean_square = moment_values[2u * i + 1u];
      result.indirect_variance[i] =
          std::fmax(mean_square - mean * mean, 0.0f) / (float)settings.samples;
    }
    if (settings.occlusion) {
      const float *visibility = static_cast<const float *>(occlusion.contents);
      for (NSUInteger i = 0u; i < count; ++i) {
        result.occlusion[i] = std::clamp(visibility[i], 0.0f, 1.0f);
      }
    }
    if (settings.backface) {
      const float *share = static_cast<const float *>(backface.contents);
      for (NSUInteger i = 0u; i < count; ++i) {
        result.backface[i] = std::clamp(share[i], 0.0f, 1.0f);
      }
    }
    if (settings.direction) {
      const float *sums = static_cast<const float *>(direction.contents);
      for (NSUInteger i = 0u; i < count; ++i) {
        result.direction[i] = vec4_new(sums[4u * i], sums[4u * i + 1u],
                                       sums[4u * i + 2u], sums[4u * i + 3u]);
      }
    }
    return true;
  }
}

bool vkr_bake_gpu_probe_gather_available() { return vkr_bake_gpu_available(); }

bool vkr_bake_gpu_gather_probes(VkrBakeGpuContext *context,
                                const std::vector<Vec3> &positions,
                                const std::vector<uint32_t> &seeds,
                                const VkrBakeGpuLayer &layer,
                                const VkrBakeGpuProbeSettings &settings,
                                std::vector<float32_t> *out_sh,
                                double *out_gpu_seconds) {
  @autoreleasepool {
    if (!context || !out_sh || !out_gpu_seconds ||
        seeds.size() != positions.size() || settings.face_size == 0u ||
        settings.face_size > VKR_SH_PROJECTION_MAX_FACE_SIZE ||
        settings.samples == 0u || settings.max_depth == 0u ||
        !std::isfinite(settings.deringing) || settings.deringing < 0.0f) {
      return false;
    }
    const size_t count = positions.size();
    out_sh->assign(count * 12u, 0.0f);
    *out_gpu_seconds = 0.0;
    if (count == 0u) {
      return true;
    }
    const NSUInteger light_capacity =
        context->lights.length / sizeof(GpuLight);
    for (uint32_t light : layer.lights) {
      if (light >= light_capacity) {
        return false;
      }
    }
    const uint32_t pixels = 6u * settings.face_size * settings.face_size;
    const uint32_t run_samples =
        std::min(kProbeSamplesPerDispatch, settings.samples);
    const uint32_t runs = (settings.samples + run_samples - 1u) / run_samples;
    const uint64_t probe_paths = (uint64_t)pixels * run_samples;
    const uint32_t probes_per_dispatch = (uint32_t)std::max<uint64_t>(
        1u, kProbePathsPerDispatch / probe_paths);

    /* Probe inputs for the whole layer, and one dispatch's radiance, which
       the host projects before the next dispatch overwrites it. */
    id<MTLBuffer> probe_positions =
        shared_buffer(context->device, count * 4u * sizeof(float));
    id<MTLBuffer> probe_seeds =
        shared_buffer(context->device, count * sizeof(uint32_t));
    id<MTLBuffer> radiance =
        shared_buffer(context->device,
                      (NSUInteger)probes_per_dispatch * pixels * 4u *
                          sizeof(float));
    id<MTLBuffer> layer_lights =
        shared_buffer(context->device, layer.lights.size() * sizeof(uint32_t));
    if (!probe_positions || !probe_seeds || !radiance || !layer_lights) {
      return false;
    }
    float *p = static_cast<float *>(probe_positions.contents);
    for (size_t i = 0u; i < count; ++i) {
      p[4u * i + 0u] = positions[i].x;
      p[4u * i + 1u] = positions[i].y;
      p[4u * i + 2u] = positions[i].z;
      p[4u * i + 3u] = 1.0f;
    }
    std::memcpy(probe_seeds.contents, seeds.data(), count * sizeof(uint32_t));
    if (!layer.lights.empty()) {
      std::memcpy(layer_lights.contents, layer.lights.data(),
                  layer.lights.size() * sizeof(uint32_t));
    }

    const uint32_t flags = (layer.sky ? 1u : 0u) | (layer.emission ? 2u : 0u);
    const NSUInteger width = context->probes.threadExecutionWidth;
    std::vector<Vec3> cube(pixels);
    double longest_dispatch = 0.0;
    uint32_t dispatches = 0u;
    for (size_t first = 0u; first < count; first += probes_per_dispatch) {
      const uint32_t batch_probes =
          (uint32_t)std::min<size_t>(probes_per_dispatch, count - first);
      for (uint32_t run = 0u; run < runs; ++run) {
        const uint32_t first_sample = run * run_samples;
        const ProbeArgs args = {
            (uint32_t)first,
            batch_probes,
            settings.face_size,
            settings.samples,
            first_sample,
            std::min(run_samples, settings.samples - first_sample),
            settings.max_depth,
            settings.rr_start_depth,
            (uint32_t)layer.lights.size(),
            flags,
            {context->geometry_first[0], context->geometry_first[1]}};
        id<MTLCommandBuffer> command = [context->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:context->probes];
        [encoder setBuffer:probe_positions offset:0u atIndex:0u];
        [encoder setBuffer:probe_seeds offset:0u atIndex:1u];
        [encoder setBuffer:radiance offset:0u atIndex:2u];
        [encoder setBytes:&args length:sizeof(args) atIndex:3u];
        [encoder setAccelerationStructure:context->scene atBufferIndex:4u];
        [encoder setBuffer:context->positions offset:0u atIndex:5u];
        [encoder setBuffer:context->normals offset:0u atIndex:6u];
        [encoder setBuffer:context->uvs offset:0u atIndex:7u];
        [encoder setBuffer:context->colors offset:0u atIndex:8u];
        [encoder setBuffer:context->triangle_materials offset:0u atIndex:9u];
        [encoder setBuffer:context->materials offset:0u atIndex:10u];
        [encoder setBuffer:context->lights offset:0u atIndex:11u];
        [encoder setBuffer:layer_lights offset:0u atIndex:12u];
        [encoder setBuffer:context->as_indices offset:0u atIndex:13u];
        [encoder setTexture:context->textures atIndex:0u];
        [encoder setTexture:context->sky atIndex:1u];
        [encoder setTexture:context->dfg atIndex:2u];
        [encoder useResource:context->scene usage:MTLResourceUsageRead];
        const NSUInteger threads = (NSUInteger)args.probe_count * pixels;
        [encoder dispatchThreads:MTLSizeMake(threads, 1u, 1u)
            threadsPerThreadgroup:MTLSizeMake(width, 1u, 1u)];
        [encoder endEncoding];
        double seconds = 0.0;
        if (!finish(command, &seconds)) {
          return false;
        }
        *out_gpu_seconds += seconds;
        longest_dispatch = std::max(longest_dispatch, seconds);
        ++dispatches;
      }
      const float *values = static_cast<const float *>(radiance.contents);
      for (uint32_t local = 0u; local < batch_probes; ++local) {
        const float *source = values + (size_t)local * pixels * 4u;
        for (uint32_t pixel = 0u; pixel < pixels; ++pixel) {
          cube[pixel] = vec3_new(source[4u * pixel], source[4u * pixel + 1u],
                                 source[4u * pixel + 2u]);
        }
        const size_t probe = first + local;
        float32_t sh[3][4];
        if (!vkr_bake_sh_project_l1(cube.data(), settings.face_size,
                                    settings.deringing, sh)) {
          std::fprintf(stderr, "Probe %zu gathered non-finite radiance\n",
                       probe);
          return false;
        }
        std::memcpy(out_sh->data() + probe * 12u, sh, sizeof(sh));
      }
    }
    std::printf("gpu_probe_gather probes=%zu dispatches=%u "
                "probes_per_dispatch=%u gpu_s=%.3f longest_dispatch_ms=%.1f\n",
                count, dispatches, probes_per_dispatch, *out_gpu_seconds,
                longest_dispatch * 1000.0);
    return true;
  }
}

/* The BC page encoders run on Vulkan only (owner decision 2026-10-09): an
   Apple host bakes for the tiled pipeline, which samples no BC plane. */
bool vkr_bake_gpu_bc_available(const VkrBakeGpuContext *context) {
  (void)context;
  return false;
}

bool vkr_bake_gpu_encode_bc6h(VkrBakeGpuContext *context, const float32_t *rgba,
                              uint32_t width, uint32_t height,
                              std::vector<uint8_t> *out_blocks,
                              double *out_gpu_seconds) {
  (void)context;
  (void)rgba;
  (void)width;
  (void)height;
  (void)out_blocks;
  *out_gpu_seconds = 0.0;
  std::fprintf(stderr, "The BC encoders are unavailable on Metal\n");
  return false;
}

bool vkr_bake_gpu_encode_bc7(VkrBakeGpuContext *context, const uint8_t *rgba8,
                             uint32_t width, uint32_t height,
                             std::vector<uint8_t> *out_blocks,
                             double *out_gpu_seconds) {
  (void)context;
  (void)rgba8;
  (void)width;
  (void)height;
  (void)out_blocks;
  *out_gpu_seconds = 0.0;
  std::fprintf(stderr, "The BC encoders are unavailable on Metal\n");
  return false;
}

bool vkr_bake_gpu_decode_bc(VkrBakeGpuContext *context,
                            VkrBakeGpuBcFormat format, const uint8_t *blocks,
                            uint32_t width, uint32_t height,
                            std::vector<float32_t> *out_rgba) {
  (void)context;
  (void)format;
  (void)blocks;
  (void)width;
  (void)height;
  (void)out_rgba;
  std::fprintf(stderr, "The BC encoders are unavailable on Metal\n");
  return false;
}

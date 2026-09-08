#include "bake/vkr_bake_integrator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

constexpr float32_t k_ray_max = 1.0e30f;
constexpr float32_t k_pi = 3.14159265358979323846f;

struct VkrBakePcg32 {
  uint64_t state;
  uint64_t increment;
};

struct VkrBakeMedium {
  uint32_t source_instance_index;
  float32_t ior;
  Vec3 sigma_a;
};

struct VkrBakeMediumStack {
  VkrBakeMedium entries[VKR_BAKE_INTEGRATOR_MAX_MEDIUM_DEPTH];
  uint32_t count;
};

struct VkrBakeSurface {
  const VkrBakeTriangle *triangle;
  VkrBakeHit hit;
  VkrBakeMaterialSample material;
  Vec3 position;
  Vec3 geometric_normal;
  Vec3 normal;
  Vec3 tangent;
  Vec3 bitangent;
  Vec3 clearcoat_normal; /* Unit normal in the base BSDF frame. */
  Vec2 uv;
  Vec4 color;
};

/* Cold diagnostic for invalid solid-boundary traversal.  It is emitted only
   when the LIFO proof fails, never from normal path work. */
void report_medium_mismatch(const char *operation,
                            const VkrBakeMediumStack *media,
                            const VkrBakeSurface *surface) {
  std::fprintf(stderr,
               "Bake medium mismatch %s: triangle=%u material=%u instance=%u front=%u position=(%g,%g,%g) stack=%u",
               operation, surface->hit.triangle_index,
               surface->triangle->material_index,
               surface->triangle->source_instance_index,
               surface->hit.front_face ? 1u : 0u, surface->position.x,
               surface->position.y, surface->position.z, media->count);
  for (uint32_t index = 0u; index < media->count; ++index)
    std::fprintf(stderr, " [%u ior=%g]", media->entries[index].source_instance_index,
                 media->entries[index].ior);
  std::fputc('\n', stderr);
}

struct VkrBakePhotonEmission {
  const VkrBakeSceneLight *light;
  Vec3 origin;
  Vec3 direction;
  Vec3 flux;
  bool8_t polynomial;
};

struct VkrBakeRectangleLightSample {
  Vec3 direction;
  Vec3 radiance;
  float32_t distance;
  float32_t solid_angle_pdf;
};

enum class VkrBakeRayEvent : uint8_t { Miss, Surface, Error };

bool8_t apply_beer(const VkrBakeMediumStack *media, float32_t distance,
                   Vec3 *in_out_throughput);

float32_t clamp01(float32_t value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

Vec3 add(Vec3 a, Vec3 b) { return vec3_new(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 sub(Vec3 a, Vec3 b) { return vec3_new(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 mul(Vec3 a, float32_t b) { return vec3_new(a.x * b, a.y * b, a.z * b); }
Vec3 mul(Vec3 a, Vec3 b) { return vec3_new(a.x * b.x, a.y * b.y, a.z * b.z); }
Vec3 neg(Vec3 a) { return vec3_new(-a.x, -a.y, -a.z); }
float32_t dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(Vec3 a, Vec3 b) {
  return vec3_new(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                  a.x * b.y - a.y * b.x);
}
float32_t length2(Vec3 value) { return dot3(value, value); }
Vec3 normalize3(Vec3 value) {
  const float32_t squared = length2(value);
  return squared > 1.0e-20f ? mul(value, 1.0f / sqrtf(squared))
                            : vec3_new(0, 0, 0);
}
bool8_t finite3(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}
bool8_t finite_nonnegative3(Vec3 value) {
  return finite3(value) && value.x >= 0.0f && value.y >= 0.0f &&
         value.z >= 0.0f;
}
bool8_t unit3(Vec3 value) {
  return finite3(value) && fabsf(length2(value) - 1.0f) <= 2.0e-3f;
}
Vec3 max0(Vec3 value) {
  return vec3_new(fmaxf(value.x, 0.0f), fmaxf(value.y, 0.0f),
                  fmaxf(value.z, 0.0f));
}
float32_t luminance(Vec3 value) {
  return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}
float32_t max_component(Vec3 value) {
  return fmaxf(value.x, fmaxf(value.y, value.z));
}

uint32_t pcg_next(VkrBakePcg32 *rng) {
  const uint64_t old_state = rng->state;
  rng->state = old_state * 6364136223846793005ULL + rng->increment;
  const uint32_t xorshifted = (uint32_t)(((old_state >> 18u) ^ old_state) >> 27u);
  const uint32_t rotation = (uint32_t)(old_state >> 59u);
  return (xorshifted >> rotation) | (xorshifted << ((-rotation) & 31u));
}

void pcg_seed(VkrBakePcg32 *rng, uint64_t seed) {
  rng->state = 0u;
  rng->increment = (seed << 1u) | 1u;
  (void)pcg_next(rng);
  rng->state += seed ^ 0x9e3779b97f4a7c15ULL;
  (void)pcg_next(rng);
}

float32_t pcg_float(VkrBakePcg32 *rng) {
  return (float32_t)(pcg_next(rng) >> 8u) * (1.0f / 16777216.0f);
}

void set_error(VkrBakeIntegratorError error, VkrBakeIntegratorError *out_error) {
  if (out_error) *out_error = error;
}

bool8_t material_has_texture(const VkrBakeMaterial *material) {
  for (uint32_t slot = 0u; slot < VKR_BAKE_MATERIAL_TEXTURE_COUNT; ++slot)
    if (material->textures[slot].present) return true_v;
  return false_v;
}

bool8_t light_valid(const VkrBakeSceneLight &light) {
  if (!finite3(light.position) || !finite3(light.direction) ||
      !finite_nonnegative3(light.color) || !std::isfinite(light.intensity) ||
      light.intensity < 0.0f || !std::isfinite(light.range) ||
      light.range < 0.0f || !std::isfinite(light.constant) ||
      !std::isfinite(light.linear) || !std::isfinite(light.quadratic) ||
      !std::isfinite(light.inner_cone_angle) ||
      !std::isfinite(light.outer_cone_angle))
    return false_v;
  if (light.kind != VkrBakeSceneLightKind::Rectangle) return true_v;
  const Vec3 emitter_normal = normalize3(neg(cross3(light.right, light.up)));
  return finite3(light.right) && finite3(light.up) && unit3(light.direction) &&
         unit3(light.right) && unit3(light.up) && unit3(emitter_normal) &&
         dot3(light.direction, emitter_normal) >= 0.998f &&
         fabsf(dot3(light.right, light.up)) <= 2.0e-3f &&
         fabsf(dot3(light.direction, light.right)) <= 2.0e-3f &&
         fabsf(dot3(light.direction, light.up)) <= 2.0e-3f &&
         std::isfinite(light.half_width) && light.half_width > 0.0f &&
         std::isfinite(light.half_height) && light.half_height > 0.0f &&
         std::isfinite(light.radiance) && light.radiance >= 0.0f;
}

bool8_t scene_valid(VkrBakeIntegratorSettings settings) {
  const VkrBakeIntegratorScene scene = settings.scene;
  if (!scene.bvh || !scene.materials || scene.material_count == 0u ||
      (scene.light_count > 0u && !scene.lights) ||
      scene.subsurface_profile_count > VKR_SUBSURFACE_PROFILE_COUNT ||
      (scene.subsurface_profile_count > 0u && !scene.subsurface_profiles) ||
      settings.max_depth == 0u ||
      settings.max_depth > VKR_BAKE_INTEGRATOR_MAX_DEPTH ||
      settings.rr_start_depth > settings.max_depth ||
      settings.max_transparent_layers == 0u ||
      settings.max_transparent_layers > VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS ||
      !std::isfinite(settings.ray_epsilon) || settings.ray_epsilon <= 0.0f)
    return false_v;
  if (scene.bvh->geometry.triangle_count > 0u &&
      (!scene.bvh->geometry.triangles || scene.bvh->node_count == 0u ||
       !scene.bvh->nodes))
    return false_v;
  for (uint32_t profile = 0u; profile < scene.subsurface_profile_count; ++profile)
    if (!vkr_subsurface_profile_valid(scene.subsurface_profiles[profile])) return false_v;
  for (uint32_t triangle = 0u; triangle < scene.bvh->geometry.triangle_count;
       ++triangle)
    if (scene.bvh->geometry.triangles[triangle].material_index >=
        scene.material_count)
      return false_v;
  for (uint32_t material = 0u; material < scene.material_count; ++material)
    if (material_has_texture(&scene.materials[material]) && !scene.texture_store)
      return false_v;
  for (uint32_t light = 0u; light < scene.light_count; ++light)
    if (!light_valid(scene.lights[light])) return false_v;
  if (settings.photon_map &&
      (!settings.photon_map->photons || !settings.photon_map->photon_capacity ||
       !settings.photon_map->cell_offsets))
    return false_v;
  return true_v;
}

Vec3 lerp3(Vec3 a, Vec3 b, Vec3 c, float32_t u, float32_t v) {
  const float32_t w = 1.0f - u - v;
  return add(add(mul(a, w), mul(b, u)), mul(c, v));
}

Vec2 lerp2(Vec2 a, Vec2 b, Vec2 c, float32_t u, float32_t v) {
  const float32_t w = 1.0f - u - v;
  return vec2_new(a.x * w + b.x * u + c.x * v,
                  a.y * w + b.y * u + c.y * v);
}

Vec4 lerp4(Vec4 a, Vec4 b, Vec4 c, float32_t u, float32_t v) {
  const float32_t w = 1.0f - u - v;
  return vec4_new(a.x * w + b.x * u + c.x * v,
                  a.y * w + b.y * u + c.y * v,
                  a.z * w + b.z * u + c.z * v,
                  a.w * w + b.w * u + c.w * v);
}

void fallback_basis(Vec3 normal, Vec3 *out_tangent, Vec3 *out_bitangent) {
  const Vec3 axis = fabsf(normal.z) < 0.999f ? vec3_new(0, 0, 1)
                                               : vec3_new(0, 1, 0);
  *out_tangent = normalize3(cross3(axis, normal));
  *out_bitangent = cross3(normal, *out_tangent);
}

void surface_frame(const VkrBakeTriangle *triangle, Vec3 normal,
                   Vec3 *out_tangent, Vec3 *out_bitangent) {
  const Vec3 edge0 = sub(triangle->vertex[1].position, triangle->vertex[0].position);
  const Vec3 edge1 = sub(triangle->vertex[2].position, triangle->vertex[0].position);
  const Vec2 uv0 = triangle->vertex[0].uv;
  const Vec2 uv1 = triangle->vertex[1].uv;
  const Vec2 uv2 = triangle->vertex[2].uv;
  const float32_t du0 = uv1.x - uv0.x, dv0 = uv1.y - uv0.y;
  const float32_t du1 = uv2.x - uv0.x, dv1 = uv2.y - uv0.y;
  const float32_t determinant = du0 * dv1 - dv0 * du1;
  if (fabsf(determinant) <= 1.0e-8f) {
    fallback_basis(normal, out_tangent, out_bitangent);
    return;
  }
  const float32_t inverse = 1.0f / determinant;
  const Vec3 tangent_raw =
      normalize3(mul(sub(mul(edge0, dv1), mul(edge1, dv0)), inverse));
  const Vec3 bitangent_raw = normalize3(mul(sub(mul(edge1, du0), mul(edge0, du1)), inverse));
  const Vec3 tangent = normalize3(
      sub(tangent_raw, mul(normal, dot3(normal, tangent_raw))));
  if (length2(tangent) <= 1.0e-12f || length2(bitangent_raw) <= 1.0e-12f) {
    fallback_basis(normal, out_tangent, out_bitangent);
    return;
  }
  const float32_t handedness = dot3(cross3(normal, tangent), bitangent_raw) < 0.0f ? -1.0f : 1.0f;
  *out_tangent = tangent;
  *out_bitangent = mul(cross3(normal, tangent), handedness);
}

bool8_t make_surface(const VkrBakeIntegrator *integrator, VkrBakeHit hit,
                     Vec3 view, VkrBakeSurface *out_surface) {
  const VkrBakeTriangle *triangle =
      &integrator->settings.scene.bvh->geometry.triangles[hit.triangle_index];
  if (triangle->material_index >= integrator->settings.scene.material_count)
    return false_v;
  const float32_t u = hit.bary_u, v = hit.bary_v;
  Vec3 geometric = triangle->geometric_normal;
  if (!hit.front_face) geometric = neg(geometric);
  Vec3 normal = normalize3(lerp3(triangle->vertex[0].normal,
                                  triangle->vertex[1].normal,
                                  triangle->vertex[2].normal, u, v));
  if (dot3(normal, geometric) < 0.0f) normal = neg(normal);
  Vec3 tangent = {}, bitangent = {};
  const bool8_t anisotropic = integrator->settings.scene.materials[triangle->material_index].anisotropy_strength > 0.0f;
  if (anisotropic && triangle->vertex[0].tangent.w != 0.0f) {
    const Vec4 t = lerp4(triangle->vertex[0].tangent, triangle->vertex[1].tangent,
                         triangle->vertex[2].tangent, u, v);
    const Vec3 raw = vec3_new(t.x, t.y, t.z);
    tangent = normalize3(sub(raw, mul(normal, dot3(normal, raw))));
    bitangent = mul(cross3(normal, tangent), t.w < 0.0f ? -1.0f : 1.0f);
  } else {
    surface_frame(triangle, normal, &tangent, &bitangent);
  }
  const Vec2 uv = lerp2(triangle->vertex[0].uv, triangle->vertex[1].uv,
                         triangle->vertex[2].uv, u, v);
  const Vec4 color = lerp4(triangle->vertex[0].color, triangle->vertex[1].color,
                            triangle->vertex[2].color, u, v);
  VkrBakeMaterialSample material = {};
  vkr_bake_material_sample(integrator->settings.scene.texture_store,
                           &integrator->settings.scene.materials[triangle->material_index],
                           uv, color, &material);
  // An absent scene profile preserves the ordinary local diffuse material.
  if (material.subsurface_profile >= integrator->settings.scene.subsurface_profile_count)
    material.subsurface_strength = 0.0f;
  Vec3 mapped = normalize3(add(add(mul(tangent, material.tangent_normal.x),
                                   mul(bitangent, material.tangent_normal.y)),
                               mul(normal, material.tangent_normal.z)));
  if (dot3(mapped, geometric) <= 1.0e-5f) mapped = normal;
  if (dot3(mapped, view) < 0.0f) mapped = neg(mapped);
  Vec3 coat_world = normalize3(add(add(mul(tangent, material.clearcoat_tangent_normal.x),
                                        mul(bitangent, material.clearcoat_tangent_normal.y)),
                                    mul(normal, material.clearcoat_tangent_normal.z)));
  if (dot3(coat_world, geometric) <= 1.0e-5f) coat_world = normal;
  Vec3 anisotropy_world = {};
  if (anisotropic) {
    const Vec3 raw = add(mul(tangent, material.anisotropy_direction.x),
                         mul(bitangent, material.anisotropy_direction.y));
    Vec3 projected = sub(raw, mul(mapped, dot3(mapped, raw)));
    if (length2(projected) <= 1e-12f)
      projected = cross3(mapped, add(mul(tangent, -material.anisotropy_direction.y),
                                      mul(bitangent, material.anisotropy_direction.x)));
    anisotropy_world = normalize3(projected);
  }
  /* A mapped shading normal needs an orthonormal frame for unit local rays.
     Preserve the source UV handedness and construct the coat before this step. */
  const float32_t handedness = dot3(cross3(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
  tangent = normalize3(sub(tangent, mul(mapped, dot3(mapped, tangent))));
  if (length2(tangent) <= 1.0e-12f)
    fallback_basis(mapped, &tangent, &bitangent);
  bitangent = mul(cross3(mapped, tangent), handedness);
  if (anisotropic)
    material.anisotropy_direction = vec2_new(dot3(anisotropy_world, tangent), dot3(anisotropy_world, bitangent));
  const Vec3 coat_local = vec3_new(dot3(coat_world, tangent),
                                    dot3(coat_world, bitangent), dot3(coat_world, mapped));
  const Vec3 position = lerp3(triangle->vertex[0].position,
                              triangle->vertex[1].position,
                              triangle->vertex[2].position, u, v);
  if (!finite3(position) || !unit3(geometric) || !unit3(mapped) ||
      !finite3(vec3_new(material.base_color.x, material.base_color.y, material.base_color.z)) || !std::isfinite(material.base_color.w))
    return false_v;
  *out_surface = {.triangle = triangle,
                  .hit = hit,
                  .material = material,
                  .position = position,
                  .geometric_normal = geometric,
                  .normal = mapped,
                  .tangent = tangent,
                  .bitangent = bitangent,
                  .clearcoat_normal = coat_local,
                  .uv = uv,
                  .color = color};
  return true_v;
}

/* The surface approximation connects only one authored object's consistently
   oriented side. Projection rays are geometric queries, not transport rays:
   they neither cross dielectric media nor stochastically traverse opacity. */
struct VkrBakeSubsurfaceChord {
  uint32_t count;
  VkrBakeSurface selected;
};

uint32_t uniform_index(VkrBakePcg32 *rng, uint32_t count) {
  const uint32_t threshold = (0u - count) % count;
  uint32_t bits;
  do {
    bits = pcg_next(rng);
  } while (bits < threshold);
  return bits % count;
}

bool8_t subsurface_chord(const VkrBakeIntegrator *integrator,
                         const VkrBakeSurface *receiver,
                         const float64_t point[3], Vec3 axis, VkrBakePcg32 *rng,
                         VkrBakeSubsurfaceChord *out_chord,
                         VkrBakeIntegratorError *out_error) {
  *out_chord = {};
  const VkrBakeBvh *bvh = integrator->settings.scene.bvh;
  const VkrBakeAabb bounds = bvh->nodes[0].bounds;
  float64_t enter = -std::numeric_limits<float64_t>::infinity();
  float64_t leave = std::numeric_limits<float64_t>::infinity();
  for (uint32_t component = 0u; component < 3u; ++component) {
    const float64_t direction = axis.elements[component];
    if (direction == 0.0) {
      if (point[component] < bounds.min.elements[component] ||
          point[component] > bounds.max.elements[component])
        return true_v;
      continue;
    }
    const float64_t t0 =
        (bounds.min.elements[component] - point[component]) / direction;
    const float64_t t1 =
        (bounds.max.elements[component] - point[component]) / direction;
    enter = std::max(enter, std::min(t0, t1));
    leave = std::min(leave, std::max(t0, t1));
    if (enter > leave)
      return true_v;
  }
  /* Use the entire bounds chord for every RGB proposal. Its eligible-hit
     count is then independent of the sampled channel or diffusion distance. */
  const float64_t margin = integrator->settings.ray_epsilon;
  VkrBakeRay ray = {.direction = axis,
                    .t_min = 0.0f,
                    .t_max = (float32_t)(leave - enter + 2.0 * margin)};
  for (uint32_t component = 0u; component < 3u; ++component)
    ray.origin.elements[component] =
        (float32_t)(point[component] +
                    (enter - margin) * axis.elements[component]);
  if (!finite3(ray.origin) || !std::isfinite(ray.t_max) || ray.t_max <= 0.0f) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  for (uint32_t visited = 0u; visited < bvh->geometry.triangle_count;
       ++visited) {
    VkrBakeHit hit = {};
    if (!vkr_bake_bvh_intersect_closest(bvh, ray, &hit))
      break;
    // Advancing t_min, rather than the origin, keeps all chord queries on
    // precisely the same line and never adds a world-space gap between hits.
    ray.t_min =
        std::nextafter(hit.t, std::numeric_limits<float32_t>::infinity());
    const VkrBakeTriangle *triangle =
        &bvh->geometry.triangles[hit.triangle_index];
    const VkrBakeMaterial *material =
        &integrator->settings.scene.materials[triangle->material_index];
    const Vec3 oriented_normal = receiver->hit.front_face
                                     ? triangle->geometric_normal
                                     : neg(triangle->geometric_normal);
    if (triangle->source_instance_index ==
            receiver->triangle->source_instance_index &&
        material->subsurface_profile == receiver->material.subsurface_profile &&
        material->alpha_mode != VKR_BAKE_MATERIAL_ALPHA_BLEND &&
        material->transmission_factor == 0.0f &&
        material->thickness_factor == 0.0f &&
        material->diffuse_transmission_strength == 0.0f &&
        dot3(oriented_normal, receiver->geometric_normal) > 0.0f) {
      // The receiver chooses the authored surface side. An arbitrary chord
      // direction must not flip its shading frame or the outgoing hemisphere.
      hit.front_face = receiver->hit.front_face;
      VkrBakeSurface candidate = {};
      if (!make_surface(integrator, hit, oriented_normal, &candidate)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
      const bool8_t cutout_hole =
          candidate.material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_CUTOUT &&
          clamp01(candidate.material.base_color.w) <
              clamp01(candidate.material.alpha_cutoff);
      if (!cutout_hole) {
        ++out_chord->count;
        if (uniform_index(rng, out_chord->count) == 0u)
          out_chord->selected = candidate;
      }
    }
    if (ray.t_min >= ray.t_max)
      break;
  }
  return true_v;
}

VkrBakeRayEvent sample_subsurface(const VkrBakeIntegrator *integrator,
                                  const VkrBakeSurface *receiver,
                                  const VkrBakeBsdf *bsdf, Vec3 wo,
                                  VkrBakePcg32 *rng, VkrBakeSurface *out_entry,
                                  Vec3 *out_weight,
                                  VkrBakeIntegratorError *out_error) {
  Vec3 axes[3] = {receiver->geometric_normal, {}, {}};
  fallback_basis(axes[0], &axes[1], &axes[2]);
  const float64_t probabilities[3] = {0.5, 0.25, 0.25};
  const float32_t choice = pcg_float(rng);
  const uint32_t selected_axis =
      choice < 0.5f ? 0u : (choice < 0.75f ? 1u : 2u);
  const uint32_t channel = uniform_index(rng, 3u);
  const VkrSubsurfaceProfile profile =
      integrator->settings.scene
          .subsurface_profiles[receiver->material.subsurface_profile];
  // Open-interval uniform input retains the full analytic tail, including
  // arbitrarily small radii, without evaluating the inverse CDF at an endpoint.
  const float64_t radius_u = ((float64_t)pcg_next(rng) + 0.5) / 4294967296.0;
  const float64_t radius = vkr_subsurface_profile_sample_radius(
      profile.diffusion_distance.elements[channel], radius_u);
  const float64_t phi = 6.28318530717958647692 *
                        (((float64_t)pcg_next(rng) + 0.5) / 4294967296.0);
  const Vec3 tangent = axes[(selected_axis + 1u) % 3u];
  const Vec3 bitangent = axes[(selected_axis + 2u) % 3u];
  float64_t disk_delta[3], point[3];
  for (uint32_t component = 0u; component < 3u; ++component) {
    disk_delta[component] =
        radius * (std::cos(phi) * tangent.elements[component] +
                  std::sin(phi) * bitangent.elements[component]);
    point[component] =
        receiver->position.elements[component] + disk_delta[component];
  }
  VkrBakeSubsurfaceChord chosen = {};
  if (!subsurface_chord(integrator, receiver, point, axes[selected_axis], rng,
                        &chosen, out_error))
    return VkrBakeRayEvent::Error;
  if (chosen.count == 0u)
    return VkrBakeRayEvent::Miss;
  const VkrBakeSurface entry = chosen.selected;
  float64_t delta[3], distance_squared = 0.0;
  for (uint32_t component = 0u; component < 3u; ++component) {
    delta[component] = (float64_t)entry.position.elements[component] -
                       receiver->position.elements[component];
    distance_squared += delta[component] * delta[component];
  }
  if (distance_squared == 0.0) {
    /* Float geometry can quantize a nonzero sampled disk to the receiver.
       Retain the intended displacement on that triangle's tangent plane for
       the kernel/PDF ratio; sampling a singular R(0) would create NaNs. The
       external ray still begins at the representable surface position. */
    float64_t normal_offset = 0.0;
    for (uint32_t component = 0u; component < 3u; ++component)
      normal_offset +=
          disk_delta[component] * entry.geometric_normal.elements[component];
    const float64_t correction =
        -normal_offset / dot3(axes[selected_axis], entry.geometric_normal);
    for (uint32_t component = 0u; component < 3u; ++component) {
      delta[component] = disk_delta[component] +
                         correction * axes[selected_axis].elements[component];
      distance_squared += delta[component] * delta[component];
    }
  }
  const float64_t distance = std::sqrt(distance_squared);
  float64_t area_pdf = 0.0;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float64_t jacobian =
        std::fabs(dot3(entry.geometric_normal, axes[axis]));
    if (jacobian == 0.0)
      continue;
    uint32_t intersection_count = chosen.count;
    if (axis != selected_axis) {
      float64_t entry_point[3] = {entry.position.x, entry.position.y,
                                  entry.position.z};
      VkrBakeSubsurfaceChord alternatives = {};
      // Only the count is needed. A private RNG copy leaves the path sequence
      // independent of the number of alternative-axis reservoir replacements.
      VkrBakePcg32 counting_rng = *rng;
      if (!subsurface_chord(integrator, receiver, entry_point, axes[axis],
                            &counting_rng, &alternatives, out_error))
        return VkrBakeRayEvent::Error;
      intersection_count = alternatives.count;
    }
    if (intersection_count == 0u)
      continue;
    float64_t axial_distance = 0.0, projected_squared = 0.0;
    for (uint32_t component = 0u; component < 3u; ++component)
      axial_distance += delta[component] * axes[axis].elements[component];
    for (uint32_t component = 0u; component < 3u; ++component) {
      const float64_t projected =
          delta[component] - axial_distance * axes[axis].elements[component];
      projected_squared += projected * projected;
    }
    const float64_t projected_radius = std::sqrt(projected_squared);
    float64_t channel_pdf = 0.0;
    for (uint32_t component = 0u; component < 3u; ++component)
      channel_pdf += vkr_subsurface_profile_evaluate(
                         projected_radius,
                         profile.diffusion_distance.elements[component]) /
                     3.0;
    area_pdf +=
        probabilities[axis] * jacobian * channel_pdf / intersection_count;
  }
  const Vec3 endpoint = mul(vkr_bake_bsdf_subsurface_receiver(bsdf, wo),
                            vkr_bake_subsurface_amplitude(entry.material));
  const float32_t strength = fminf(receiver->material.subsurface_strength,
                                   entry.material.subsurface_strength);
  Vec3 weight = {};
  for (uint32_t component = 0u; component < 3u; ++component)
    weight.elements[component] =
        (float32_t)(endpoint.elements[component] * strength *
                    vkr_subsurface_profile_evaluate(
                        distance,
                        profile.diffusion_distance.elements[component]) /
                    area_pdf);
  if (!finite_nonnegative3(weight) || !std::isfinite(area_pdf) ||
      area_pdf <= 0.0) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return VkrBakeRayEvent::Error;
  }
  *out_entry = entry;
  *out_weight = weight;
  return VkrBakeRayEvent::Surface;
}

VkrBakeRayEvent next_surface(const VkrBakeIntegrator *integrator,
                             VkrBakeRay *ray, VkrBakePcg32 *rng,
                             const VkrBakeMediumStack *media,
                             Vec3 *in_out_throughput,
                             float32_t *in_out_distance,
                             uint32_t *in_out_skips, VkrBakeSurface *out_surface,
                             VkrBakeIntegratorError *out_error) {
  while (*in_out_skips < integrator->settings.max_transparent_layers) {
    VkrBakeHit hit = {};
    if (!vkr_bake_bvh_intersect_closest(integrator->settings.scene.bvh, *ray, &hit))
      return VkrBakeRayEvent::Miss;
    if (in_out_distance) *in_out_distance += hit.t;
    if (!apply_beer(media, hit.t, in_out_throughput)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return VkrBakeRayEvent::Error;
    }
    VkrBakeSurface surface = {};
    if (!make_surface(integrator, hit, neg(ray->direction), &surface)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return VkrBakeRayEvent::Error;
    }
    const float32_t alpha = clamp01(surface.material.base_color.w);
    const bool8_t pass_mask =
        surface.material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_CUTOUT &&
        alpha < clamp01(surface.material.alpha_cutoff);
    const bool8_t pass_blend =
        surface.material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_BLEND &&
        (alpha <= 0.0f || (alpha < 1.0f && pcg_float(rng) >= alpha));
    if (!pass_mask && !pass_blend) {
      *out_surface = surface;
      return VkrBakeRayEvent::Surface;
    }
    ++*in_out_skips;
    ray->origin = add(surface.position,
                      mul(ray->direction, integrator->settings.ray_epsilon));
    ray->t_min = integrator->settings.ray_epsilon;
  }
  set_error(VKR_BAKE_INTEGRATOR_ERROR_TRANSPARENT_LAYER_LIMIT, out_error);
  return VkrBakeRayEvent::Error;
}

float32_t medium_ior(const VkrBakeMediumStack *media) {
  return media->count ? media->entries[media->count - 1u].ior : 1.0f;
}

Vec3 attenuation_sigma(VkrBakeMaterialSample material) {
  if (!std::isfinite(material.attenuation_distance) ||
      material.attenuation_distance <= 0.0f)
    return vec3_new(0, 0, 0);
  const Vec3 color = vec3_new(fmaxf(material.attenuation_color.x, 1.0e-6f),
                              fmaxf(material.attenuation_color.y, 1.0e-6f),
                              fmaxf(material.attenuation_color.z, 1.0e-6f));
  return vec3_new(-logf(color.x) / material.attenuation_distance,
                  -logf(color.y) / material.attenuation_distance,
                  -logf(color.z) / material.attenuation_distance);
}

bool8_t apply_beer(const VkrBakeMediumStack *media, float32_t distance,
                   Vec3 *in_out_throughput) {
  if (!std::isfinite(distance) || distance < 0.0f) return false_v;
  if (!media->count) return true_v;
  const Vec3 sigma = media->entries[media->count - 1u].sigma_a;
  const Vec3 transmittance = vec3_new(expf(-sigma.x * distance),
                                      expf(-sigma.y * distance),
                                      expf(-sigma.z * distance));
  *in_out_throughput = mul(*in_out_throughput, transmittance);
  return finite_nonnegative3(*in_out_throughput);
}

bool8_t is_volume(VkrBakeMaterialSample material) {
  return material.transmission > 0.0f && material.thickness > 0.0f;
}

bool8_t medium_target(const VkrBakeMediumStack *media,
                      const VkrBakeSurface *surface, float32_t *out_eta) {
  if (!is_volume(surface->material)) {
    *out_eta = medium_ior(media);
    return true_v;
  }
  if (surface->hit.front_face) {
    *out_eta = surface->material.ior;
    return true_v;
  }
  if (!media->count ||
      media->entries[media->count - 1u].source_instance_index !=
          surface->triangle->source_instance_index) {
    report_medium_mismatch("target", media, surface);
    return false_v;
  }
  *out_eta = media->count > 1u ? media->entries[media->count - 2u].ior : 1.0f;
  return true_v;
}

bool8_t medium_cross(VkrBakeMediumStack *media, const VkrBakeSurface *surface,
                     VkrBakeIntegratorError *out_error) {
  if (!is_volume(surface->material)) return true_v;
  if (surface->hit.front_face) {
    if (media->count >= VKR_BAKE_INTEGRATOR_MAX_MEDIUM_DEPTH) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_OVERFLOW, out_error);
      return false_v;
    }
    media->entries[media->count++] = {
        .source_instance_index = surface->triangle->source_instance_index,
        .ior = surface->material.ior,
        .sigma_a = attenuation_sigma(surface->material)};
    return true_v;
  }
  if (!media->count ||
      media->entries[media->count - 1u].source_instance_index !=
          surface->triangle->source_instance_index) {
    report_medium_mismatch("cross", media, surface);
    set_error(VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_MISMATCH, out_error);
    return false_v;
  }
  --media->count;
  return true_v;
}

Vec3 to_local(const VkrBakeSurface *surface, Vec3 direction) {
  return vec3_new(dot3(direction, surface->tangent),
                  dot3(direction, surface->bitangent),
                  dot3(direction, surface->normal));
}

Vec3 to_world(const VkrBakeSurface *surface, Vec3 direction) {
  return add(add(mul(surface->tangent, direction.x),
                 mul(surface->bitangent, direction.y)),
             mul(surface->normal, direction.z));
}

bool8_t evaluate_light(const VkrBakeSceneLight &light, Vec3 position,
                       Vec3 *out_direction, Vec3 *out_radiance,
                       float32_t *out_distance) {
  *out_radiance = vec3_new(0, 0, 0);
  *out_distance = 0.0f;
  if (!light.enabled || light.intensity <= 0.0f ||
      light.kind == VkrBakeSceneLightKind::Rectangle)
    return false_v;
  if (light.kind == VkrBakeSceneLightKind::Directional) {
    const Vec3 direction = normalize3(neg(light.direction));
    if (!unit3(direction)) return false_v;
    *out_direction = direction;
    *out_radiance = mul(light.color, light.intensity);
    *out_distance = k_ray_max;
    return finite_nonnegative3(*out_radiance);
  }
  const Vec3 to_light = sub(light.position, position);
  const float32_t distance_squared = length2(to_light);
  if (!(distance_squared > 1.0e-12f) || !std::isfinite(distance_squared))
    return false_v;
  const float32_t distance = sqrtf(distance_squared);
  if (light.kind != VkrBakeSceneLightKind::Polynomial &&
      light.range > 0.0f && distance > light.range)
    return false_v;
  const Vec3 direction = mul(to_light, 1.0f / distance);
  float32_t attenuation = 0.0f;
  if (light.kind == VkrBakeSceneLightKind::Polynomial) {
    attenuation = 1.0f / fmaxf(fmaxf(light.constant, 1.0f) +
                                    light.linear * distance +
                                    light.quadratic * distance_squared,
                                1.0e-6f);
  } else {
    float32_t range_attenuation = 1.0f;
    if (light.range > 0.0f) {
      const float32_t ratio = distance / light.range;
      range_attenuation = clamp01(1.0f - ratio * ratio * ratio * ratio);
      range_attenuation *= range_attenuation;
    }
    attenuation = range_attenuation / fmaxf(distance_squared, 1.0e-4f);
    if (light.kind == VkrBakeSceneLightKind::Spot) {
      const Vec3 axis = normalize3(light.direction);
      const float32_t inner = cosf(light.inner_cone_angle);
      const float32_t outer = cosf(light.outer_cone_angle);
      const float32_t cosine = dot3(neg(direction), axis);
      const float32_t cone = inner > outer
                                 ? clamp01((cosine - outer) / (inner - outer))
                                 : (cosine >= outer ? 1.0f : 0.0f);
      attenuation *= cone * cone;
    }
  }
  *out_direction = direction;
  *out_radiance = mul(light.color, light.intensity * fmaxf(attenuation, 0.0f));
  *out_distance = distance;
  return finite_nonnegative3(*out_radiance) && attenuation > 0.0f;
}

bool8_t sample_rectangle_light(const VkrBakeSceneLight &light, Vec3 position,
                               VkrBakePcg32 *rng,
                               VkrBakeRectangleLightSample *out_sample) {
  if (!light.enabled || light.radiance <= 0.0f) return false_v;
  const Vec3 light_position =
      add(add(light.position,
              mul(light.right, (2.0f * pcg_float(rng) - 1.0f) * light.half_width)),
          mul(light.up, (2.0f * pcg_float(rng) - 1.0f) * light.half_height));
  const Vec3 to_light = sub(light_position, position);
  const float32_t distance_squared = length2(to_light);
  if (!(distance_squared > 1.0e-12f)) return false_v;
  const float32_t distance = sqrtf(distance_squared);
  const Vec3 direction = mul(to_light, 1.0f / distance);
  const float32_t emitter_cosine = dot3(light.direction, neg(direction));
  if (!(emitter_cosine > 0.0f)) return false_v;
  const float32_t area = 4.0f * light.half_width * light.half_height;
  *out_sample = {.direction = direction,
                 .radiance = mul(light.color, light.radiance),
                 .distance = distance,
                 .solid_angle_pdf = distance_squared / (emitter_cosine * area)};
  return true_v;
}

bool8_t shadow_transmittance(const VkrBakeIntegrator *integrator,
                             Vec3 origin, Vec3 direction, float32_t distance,
                             VkrBakeMediumStack media, Vec3 *out_transmittance,
                             VkrBakeIntegratorError *out_error) {
  Vec3 transmittance = vec3_new(1, 1, 1);
  VkrBakeRay ray = {.origin = origin,
                     .direction = direction,
                     .t_min = integrator->settings.ray_epsilon,
                     .t_max = distance};
  uint32_t layers = 0u;
  while (layers < integrator->settings.max_transparent_layers) {
    VkrBakeHit hit = {};
    if (!vkr_bake_bvh_intersect_closest(integrator->settings.scene.bvh, ray, &hit)) {
      if (distance < k_ray_max && !apply_beer(&media, distance, &transmittance)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
      *out_transmittance = transmittance;
      return true_v;
    }
    if (!apply_beer(&media, hit.t, &transmittance)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return false_v;
    }
    VkrBakeSurface surface = {};
    if (!make_surface(integrator, hit, neg(ray.direction), &surface)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return false_v;
    }
    const float32_t alpha = clamp01(surface.material.base_color.w);
    const bool8_t mask_pass =
        surface.material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_CUTOUT &&
        alpha < clamp01(surface.material.alpha_cutoff);
    if (!mask_pass) {
      const bool8_t thin = surface.material.transmission > 0.0f &&
                           surface.material.thickness <= 0.0f;
      const bool8_t eta_one = fabsf(surface.material.ior - 1.0f) <= 1.0e-4f;
      if (!thin && !eta_one) {
        *out_transmittance = vec3_new(0, 0, 0);
        return true_v;
      }
      float32_t base_transmission = 1.0f;
      if (surface.material.clearcoat_factor > 0.0f || surface.material.sheen_color.x > 0.0f ||
          surface.material.sheen_color.y > 0.0f || surface.material.sheen_color.z > 0.0f) {
        VkrBakeBsdf bsdf = {};
        if (!vkr_bake_bsdf_init(surface.material, medium_ior(&media), surface.material.ior,
              thin, surface.clearcoat_normal, to_local(&surface, surface.geometric_normal), &bsdf)) {
          set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
          return false_v;
        }
        base_transmission = vkr_bake_bsdf_base_transmission(&bsdf, to_local(&surface, neg(direction)));
      }
      const Vec3 glass = mul(max0(vec3_new(surface.material.base_color.x,
                                             surface.material.base_color.y,
                                             surface.material.base_color.z)),
                             clamp01(surface.material.transmission) * base_transmission);
      const Vec3 layer = surface.material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_BLEND
                             ? add(mul(vec3_new(1, 1, 1), 1.0f - alpha),
                                   mul(glass, alpha))
                             : glass;
      transmittance = mul(transmittance, layer);
      if (max_component(transmittance) <= 1.0e-6f) {
        *out_transmittance = vec3_new(0, 0, 0);
        return true_v;
      }
    }
    ++layers;
    ray.origin = add(surface.position,
                     mul(direction, integrator->settings.ray_epsilon));
    ray.t_min = integrator->settings.ray_epsilon;
    if (distance < k_ray_max) {
      distance -= hit.t + integrator->settings.ray_epsilon;
      if (distance <= 0.0f) {
        *out_transmittance = transmittance;
        return true_v;
      }
      ray.t_max = distance;
    }
  }
  set_error(VKR_BAKE_INTEGRATOR_ERROR_TRANSPARENT_LAYER_LIMIT, out_error);
  return false_v;
}

bool8_t direct_lighting(const VkrBakeIntegrator *integrator,
                        const VkrBakeSurface *surface, Vec3 wo,
                        const VkrBakeMediumStack *media, VkrBakePcg32 *rng,
                        Vec3 *out_radiance,
                        VkrBakeIntegratorError *out_error) {
  Vec3 result = vec3_new(0, 0, 0);
  float32_t eta_target = 1.0f;
  if (!medium_target(media, surface, &eta_target)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_MISMATCH, out_error);
    return false_v;
  }
  VkrBakeBsdf bsdf = {};
  if (!vkr_bake_bsdf_init(surface->material, medium_ior(media), eta_target,
                          !is_volume(surface->material), surface->clearcoat_normal, to_local(surface, surface->geometric_normal), &bsdf)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  for (uint32_t index = 0u; index < integrator->settings.scene.light_count;
       ++index) {
    const VkrBakeSceneLight &light = integrator->settings.scene.lights[index];
    Vec3 wi_world = {}, light_radiance = {};
    float32_t distance = 0.0f;
    float32_t light_weight = 1.0f;
    const bool8_t rectangle = light.kind == VkrBakeSceneLightKind::Rectangle;
    if (rectangle) {
      VkrBakeRectangleLightSample sample = {};
      if (!sample_rectangle_light(light, surface->position, rng, &sample)) continue;
      wi_world = sample.direction;
      light_radiance = sample.radiance;
      distance = sample.distance;
      light_weight = 1.0f / sample.solid_angle_pdf;
    } else if (!evaluate_light(light, surface->position, &wi_world,
                               &light_radiance, &distance)) {
      continue;
    }
    const Vec3 wi = to_local(surface, wi_world);
    const VkrBakeBsdfEval evaluation = vkr_bake_bsdf_evaluate(&bsdf, wo, wi);
    const float32_t cosine = fabsf(wi.z);
    if (evaluation.pdf <= 0.0f || cosine <= 0.0f ||
        !finite_nonnegative3(evaluation.f))
      continue;
    VkrBakeMediumStack shadow_media = *media;
    if (dot3(wi_world, surface->geometric_normal) < 0.0f &&
        !medium_cross(&shadow_media, surface, out_error))
      return false_v;
    Vec3 visibility = vec3_new(1, 1, 1);
    if ((rectangle || light.casts_shadow) &&
        !shadow_transmittance(integrator,
                              add(surface->position,
                                  mul(wi_world, integrator->settings.ray_epsilon)),
                              wi_world,
                              distance < k_ray_max ? distance - integrator->settings.ray_epsilon
                                                   : k_ray_max,
                              shadow_media, &visibility, out_error))
      return false_v;
    result = add(result,
                 mul(mul(mul(evaluation.f, light_radiance), visibility),
                     cosine * light_weight));
  }
  if (!finite_nonnegative3(result)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  *out_radiance = result;
  return true_v;
}

bool8_t photon_settings_valid(VkrBakePhotonSettings settings,
                              uint32_t *out_cell_count) {
  if (!settings.photon_count || !settings.batch_count || !settings.max_depth ||
      settings.max_depth > VKR_BAKE_INTEGRATOR_MAX_DEPTH ||
      settings.sample_offset > settings.photon_count ||
      settings.batch_count > settings.photon_count - settings.sample_offset ||
      !std::isfinite(settings.radius) || settings.radius <= 0.0f)
    return false_v;
  uint64_t cell_count = 1u;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if (!settings.grid_dimensions[axis]) return false_v;
    cell_count *= settings.grid_dimensions[axis];
  }
  if (cell_count > VKR_BAKE_PHOTON_GRID_MAX_CELLS || cell_count > UINT32_MAX)
    return false_v;
  *out_cell_count = (uint32_t)cell_count;
  return true_v;
}

bool8_t photon_bounds_valid(VkrBakeAabb bounds) {
  return finite3(bounds.min) && finite3(bounds.max) &&
         bounds.max.x > bounds.min.x && bounds.max.y > bounds.min.y &&
         bounds.max.z > bounds.min.z;
}

uint32_t photon_cell_index(const VkrBakePhotonMap *map, Vec3 position) {
  const Vec3 extent = sub(map->bounds.max, map->bounds.min);
  uint32_t coordinate[3] = {};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t relative =
        (position.elements[axis] - map->bounds.min.elements[axis]) /
        extent.elements[axis];
    const int32_t value = (int32_t)floorf(relative * map->grid_dimensions[axis]);
    coordinate[axis] = (uint32_t)(value < 0 ? 0 :
        (value >= (int32_t)map->grid_dimensions[axis]
             ? (int32_t)map->grid_dimensions[axis] - 1
             : value));
  }
  return coordinate[0] + map->grid_dimensions[0] *
      (coordinate[1] + map->grid_dimensions[1] * coordinate[2]);
}

float32_t photon_light_weight(const VkrBakeIntegrator *integrator,
                              const VkrBakeSceneLight &light) {
  if (!light.enabled) return 0.0f;
  if (light.kind == VkrBakeSceneLightKind::Rectangle) {
    if (light.radiance <= 0.0f) return 0.0f;
    const float32_t area = 4.0f * light.half_width * light.half_height;
    return luminance(mul(light.color, light.radiance)) * k_pi * area;
  }
  if (light.intensity <= 0.0f) return 0.0f;
  const float32_t intensity = luminance(mul(light.color, light.intensity));
  if (!(intensity > 0.0f)) return 0.0f;
  if (light.kind == VkrBakeSceneLightKind::Directional) {
    const Vec3 direction = normalize3(light.direction);
    if (!unit3(direction)) return 0.0f;
    const VkrBakeAabb bounds = integrator->settings.scene.bvh->nodes[0].bounds;
    const Vec3 axis = fabsf(direction.z) < .999f ? vec3_new(0, 0, 1)
                                                   : vec3_new(0, 1, 0);
    const Vec3 tangent = normalize3(cross3(axis, direction));
    const Vec3 bitangent = cross3(direction, tangent);
    float32_t min_t = std::numeric_limits<float32_t>::infinity();
    float32_t max_t = -min_t, min_b = min_t, max_b = -min_t;
    for (uint32_t corner = 0u; corner < 8u; ++corner) {
      const Vec3 point = vec3_new((corner & 1u) ? bounds.max.x : bounds.min.x,
                                  (corner & 2u) ? bounds.max.y : bounds.min.y,
                                  (corner & 4u) ? bounds.max.z : bounds.min.z);
      const float32_t t = dot3(point, tangent), b = dot3(point, bitangent);
      min_t = fminf(min_t, t); max_t = fmaxf(max_t, t);
      min_b = fminf(min_b, b); max_b = fmaxf(max_b, b);
    }
    return intensity * fmaxf(0.0f, (max_t - min_t) * (max_b - min_b));
  }
  if (light.kind == VkrBakeSceneLightKind::Spot) {
    const float32_t cosine = cosf(light.outer_cone_angle);
    return intensity * (2.0f * k_pi * fmaxf(0.0f, 1.0f - cosine));
  }
  return intensity * (4.0f * k_pi);
}

Vec3 sample_unit_sphere(VkrBakePcg32 *rng) {
  const float32_t z = 1.0f - 2.0f * pcg_float(rng);
  const float32_t phi = 2.0f * k_pi * pcg_float(rng);
  const float32_t radius = sqrtf(fmaxf(0.0f, 1.0f - z * z));
  return vec3_new(radius * cosf(phi), radius * sinf(phi), z);
}

bool8_t photon_emit(const VkrBakeIntegrator *integrator,
                    const VkrBakeSceneLight &light, float32_t selection_pdf,
                    uint32_t photon_count, VkrBakePcg32 *rng,
                    VkrBakePhotonEmission *out_emission) {
  VkrBakePhotonEmission emission = {.light = &light};
  if (light.kind == VkrBakeSceneLightKind::Rectangle) {
    const float32_t area = 4.0f * light.half_width * light.half_height;
    emission.origin =
        add(add(light.position,
                mul(light.right,
                    (2.0f * pcg_float(rng) - 1.0f) * light.half_width)),
            mul(light.up, (2.0f * pcg_float(rng) - 1.0f) * light.half_height));
    const float32_t cosine = sqrtf(1.0f - pcg_float(rng));
    const float32_t phi = 2.0f * k_pi * pcg_float(rng);
    const float32_t sine = sqrtf(fmaxf(0.0f, 1.0f - cosine * cosine));
    emission.direction = normalize3(
        add(add(mul(light.right, sine * cosf(phi)), mul(light.up, sine * sinf(phi))),
            mul(light.direction, cosine)));
    emission.flux = mul(light.color, light.radiance * k_pi * area /
                                         (selection_pdf * (float32_t)photon_count));
  } else if (light.kind == VkrBakeSceneLightKind::Directional) {
    const Vec3 power_scale = mul(light.color, light.intensity /
        (selection_pdf * (float32_t)photon_count));
    const Vec3 direction = normalize3(light.direction);
    if (!unit3(direction)) return false_v;
    const VkrBakeAabb bounds = integrator->settings.scene.bvh->nodes[0].bounds;
    const Vec3 axis = fabsf(direction.z) < .999f ? vec3_new(0, 0, 1)
                                                   : vec3_new(0, 1, 0);
    const Vec3 tangent = normalize3(cross3(axis, direction));
    const Vec3 bitangent = cross3(direction, tangent);
    float32_t min_t = std::numeric_limits<float32_t>::infinity();
    float32_t max_t = -min_t, min_b = min_t, max_b = -min_t, min_d = min_t;
    for (uint32_t corner = 0u; corner < 8u; ++corner) {
      const Vec3 point = vec3_new((corner & 1u) ? bounds.max.x : bounds.min.x,
                                  (corner & 2u) ? bounds.max.y : bounds.min.y,
                                  (corner & 4u) ? bounds.max.z : bounds.min.z);
      min_t = fminf(min_t, dot3(point, tangent));
      max_t = fmaxf(max_t, dot3(point, tangent));
      min_b = fminf(min_b, dot3(point, bitangent));
      max_b = fmaxf(max_b, dot3(point, bitangent));
      min_d = fminf(min_d, dot3(point, direction));
    }
    const float32_t area = (max_t - min_t) * (max_b - min_b);
    emission.origin = add(add(mul(tangent, min_t + (max_t - min_t) * pcg_float(rng)),
                               mul(bitangent, min_b + (max_b - min_b) * pcg_float(rng))),
                          mul(direction, min_d - integrator->settings.ray_epsilon));
    emission.direction = direction;
    emission.flux = mul(power_scale, area);
  } else if (light.kind == VkrBakeSceneLightKind::Spot) {
    const Vec3 power_scale = mul(light.color, light.intensity /
        (selection_pdf * (float32_t)photon_count));
    const Vec3 axis = normalize3(light.direction);
    if (!unit3(axis)) return false_v;
    const Vec3 basis_axis = fabsf(axis.z) < .999f ? vec3_new(0, 0, 1)
                                                    : vec3_new(0, 1, 0);
    const Vec3 tangent = normalize3(cross3(basis_axis, axis));
    const Vec3 bitangent = cross3(axis, tangent);
    const float32_t outer_cosine = cosf(light.outer_cone_angle);
    const float32_t cosine = 1.0f - pcg_float(rng) * (1.0f - outer_cosine);
    const float32_t phi = 2.0f * k_pi * pcg_float(rng);
    const float32_t radius = sqrtf(fmaxf(0.0f, 1.0f - cosine * cosine));
    emission.origin = light.position;
    emission.direction = normalize3(add(add(mul(tangent, radius * cosf(phi)),
                                            mul(bitangent, radius * sinf(phi))),
                                        mul(axis, cosine)));
    const float32_t inner_cosine = cosf(light.inner_cone_angle);
    const float32_t cone = inner_cosine > outer_cosine
                               ? clamp01((cosine - outer_cosine) /
                                         (inner_cosine - outer_cosine))
                               : 1.0f;
    emission.flux = mul(power_scale,
                         2.0f * 3.14159265358979323846f *
                             fmaxf(0.0f, 1.0f - outer_cosine) * cone * cone);
  } else {
    const Vec3 power_scale = mul(light.color, light.intensity /
        (selection_pdf * (float32_t)photon_count));
    emission.origin = light.position;
    emission.direction = sample_unit_sphere(rng);
    emission.flux = mul(power_scale, 4.0f * k_pi);
    emission.polynomial = light.kind == VkrBakeSceneLightKind::Polynomial;
  }
  if (!unit3(emission.direction) || !finite_nonnegative3(emission.flux))
    return false_v;
  *out_emission = emission;
  return true_v;
}

float32_t photon_first_segment_scale(const VkrBakePhotonEmission *emission,
                                     float32_t distance) {
  const VkrBakeSceneLight &light = *emission->light;
  if (emission->polynomial) {
    const float32_t denominator = fmaxf(fmaxf(light.constant, 1.0f) +
                                            light.linear * distance +
                                            light.quadratic * distance * distance,
                                        1.0e-6f);
    return distance * distance / denominator;
  }
  if ((light.kind == VkrBakeSceneLightKind::Point ||
       light.kind == VkrBakeSceneLightKind::Spot) && light.range > 0.0f) {
    const float32_t ratio = distance / light.range;
    const float32_t attenuation = clamp01(1.0f - ratio * ratio * ratio * ratio);
    return attenuation * attenuation;
  }
  return 1.0f;
}

bool8_t diffuse_capable(VkrBakeMaterialSample material) {
  return material.metallic < 0.999f && material.transmission < 0.999f &&
         max_component(vec3_new(material.base_color.x, material.base_color.y,
                                 material.base_color.z)) > 0.0f;
}

/* Thin refractive sheets remain in straight delta NEE shadows. A photon chain begins only
   when that shadow estimator cannot represent the surface connection. */
bool8_t caustic_chain_surface(VkrBakeMaterialSample material) {
  const bool8_t metal = material.metallic >= 0.999f;
  const bool8_t thick_refractor =
      material.transmission > 0.0f && material.thickness > 0.0f &&
      fabsf(material.ior - 1.0f) > 1.0e-4f;
  return metal || thick_refractor;
}

bool8_t photon_density(const VkrBakeIntegrator *integrator,
                       const VkrBakeSurface *surface, Vec3 wo,
                       const VkrBakeMediumStack *media, Vec3 *out_radiance,
                       VkrBakeIntegratorError *out_error) {
  *out_radiance = vec3_new(0, 0, 0);
  const VkrBakePhotonMap *map = integrator->settings.photon_map;
  if (!map || !map->grid_ready || !map->photon_count ||
      !diffuse_capable(surface->material))
    return true_v;
  float32_t eta_target = 1.0f;
  if (!medium_target(media, surface, &eta_target)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_MISMATCH, out_error);
    return false_v;
  }
  VkrBakeBsdf bsdf = {};
  if (!vkr_bake_bsdf_init(surface->material, medium_ior(media), eta_target,
                          !is_volume(surface->material), surface->clearcoat_normal, to_local(surface, surface->geometric_normal), &bsdf)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  const Vec3 extent = sub(map->bounds.max, map->bounds.min);
  int32_t low[3] = {}, high[3] = {};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t cell_size = extent.elements[axis] / map->grid_dimensions[axis];
    const float32_t relative =
        (surface->position.elements[axis] - map->bounds.min.elements[axis]) / cell_size;
    const int32_t reach = (int32_t)ceilf(map->radius / cell_size);
    low[axis] = Max(0, (int32_t)floorf(relative) - reach);
    high[axis] = Min((int32_t)map->grid_dimensions[axis] - 1,
                     (int32_t)floorf(relative) + reach);
  }
  const float32_t radius2 = map->radius * map->radius;
  const float32_t scale = 1.0f / (3.14159265358979323846f * radius2);
  Vec3 result = vec3_new(0, 0, 0);
  for (int32_t z = low[2]; z <= high[2]; ++z)
    for (int32_t y = low[1]; y <= high[1]; ++y)
      for (int32_t x = low[0]; x <= high[0]; ++x) {
        const uint32_t cell = (uint32_t)x + map->grid_dimensions[0] *
            ((uint32_t)y + map->grid_dimensions[1] * (uint32_t)z);
        for (uint32_t index = map->cell_offsets[cell];
             index < map->cell_offsets[cell + 1u]; ++index) {
          const VkrBakePhoton &photon = map->photons[index];
          const Vec3 connection = sub(photon.position, surface->position);
          const float32_t connection_length2 = length2(connection);
          // A diffuse sheet can receive caustics from the opposite side. The
          // BSDF still enforces its shading and geometric hemisphere rules.
          const float32_t normal_alignment = dot3(photon.normal, surface->normal);
          const float32_t receiver_alignment =
              surface->material.diffuse_transmission_strength > 0.0f
                  ? fabsf(normal_alignment) : normal_alignment;
          if (connection_length2 > radius2 || receiver_alignment <= 0.5f)
            continue;
          const float32_t connection_length = sqrtf(connection_length2);
          if (connection_length > integrator->settings.ray_epsilon) {
            const Vec3 connection_direction =
                mul(connection, 1.0f / connection_length);
            Vec3 visibility = {};
            if (!shadow_transmittance(
                    integrator,
                    add(surface->position,
                        mul(connection_direction, integrator->settings.ray_epsilon)),
                    connection_direction,
                    connection_length - integrator->settings.ray_epsilon,
                    *media, &visibility, out_error))
              return false_v;
            if (max_component(visibility) <= 1.0e-6f) continue;
          }
          const Vec3 wi = to_local(surface, neg(photon.incident_direction));
          const VkrBakeBsdfEval evaluation = vkr_bake_bsdf_evaluate(&bsdf, wo, wi);
          if (evaluation.pdf <= 0.0f || !finite_nonnegative3(evaluation.f))
            continue;
          result = add(result, mul(mul(evaluation.f, photon.flux), scale));
        }
      }
  if (!finite_nonnegative3(result)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  *out_radiance = result;
  return true_v;
}

/* Incident irradiance at the entry surface, before either endpoint's material
   allocation. The spatial event supplies both square-root albedos exactly once.
 */
bool8_t subsurface_direct_irradiance(const VkrBakeIntegrator *integrator,
                                     const VkrBakeSurface *entry,
                                     const VkrBakeMediumStack *media,
                                     VkrBakePcg32 *rng, Vec3 *out_irradiance,
                                     VkrBakeIntegratorError *out_error) {
  Vec3 result = {};
  for (uint32_t index = 0u; index < integrator->settings.scene.light_count;
       ++index) {
    const VkrBakeSceneLight &light = integrator->settings.scene.lights[index];
    Vec3 direction = {}, radiance = {};
    float32_t distance = 0.0f, light_weight = 1.0f;
    const bool8_t rectangle = light.kind == VkrBakeSceneLightKind::Rectangle;
    if (rectangle) {
      VkrBakeRectangleLightSample sample = {};
      if (!sample_rectangle_light(light, entry->position, rng, &sample))
        continue;
      direction = sample.direction;
      radiance = sample.radiance;
      distance = sample.distance;
      light_weight = 1.0f / sample.solid_angle_pdf;
    } else if (!evaluate_light(light, entry->position, &direction, &radiance,
                               &distance)) {
      continue;
    }
    const float32_t cosine = dot3(direction, entry->normal);
    if (cosine <= 0.0f || dot3(direction, entry->geometric_normal) <= 0.0f)
      continue;
    Vec3 visibility = vec3_new(1, 1, 1);
    if ((rectangle || light.casts_shadow) &&
        !shadow_transmittance(
            integrator,
            add(entry->position,
                mul(direction, integrator->settings.ray_epsilon)),
            direction,
            distance < k_ray_max ? distance - integrator->settings.ray_epsilon
                                 : k_ray_max,
            *media, &visibility, out_error))
      return false_v;
    result = add(result, mul(mul(radiance, visibility), cosine * light_weight));
  }
  if (!finite_nonnegative3(result)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  *out_irradiance = result;
  return true_v;
}

/* This is a local flux-density estimate at the sampled entry. It is not a
   direct sum of photons weighted by R(distance-to-exit): that different
   estimator must not also carry this 1/(pi*h*h) or the spatial PDF division. */
bool8_t subsurface_photon_irradiance(const VkrBakeIntegrator *integrator,
                                     const VkrBakeSurface *entry,
                                     const VkrBakeMediumStack *media,
                                     Vec3 *out_irradiance,
                                     VkrBakeIntegratorError *out_error) {
  *out_irradiance = {};
  const VkrBakePhotonMap *map = integrator->settings.photon_map;
  if (!map || !map->grid_ready || !map->photon_count)
    return true_v;
  const Vec3 extent = sub(map->bounds.max, map->bounds.min);
  int32_t low[3] = {}, high[3] = {};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t cell_size =
        extent.elements[axis] / map->grid_dimensions[axis];
    const float32_t relative =
        (entry->position.elements[axis] - map->bounds.min.elements[axis]) /
        cell_size;
    const int32_t reach = (int32_t)ceilf(map->radius / cell_size);
    low[axis] = Max(0, (int32_t)floorf(relative) - reach);
    high[axis] = Min((int32_t)map->grid_dimensions[axis] - 1,
                     (int32_t)floorf(relative) + reach);
  }
  const float32_t radius2 = map->radius * map->radius;
  const float32_t density_scale = 1.0f / (k_pi * radius2);
  Vec3 result = {};
  for (int32_t z = low[2]; z <= high[2]; ++z)
    for (int32_t y = low[1]; y <= high[1]; ++y)
      for (int32_t x = low[0]; x <= high[0]; ++x) {
        const uint32_t cell =
            (uint32_t)x +
            map->grid_dimensions[0] *
                ((uint32_t)y + map->grid_dimensions[1] * (uint32_t)z);
        for (uint32_t index = map->cell_offsets[cell];
             index < map->cell_offsets[cell + 1u]; ++index) {
          const VkrBakePhoton &photon = map->photons[index];
          const Vec3 delta = sub(photon.position, entry->position);
          const float32_t squared_distance = length2(delta);
          const Vec3 incoming = neg(photon.incident_direction);
          if (photon.source_instance_index !=
                  entry->triangle->source_instance_index ||
              photon.subsurface_profile != entry->material.subsurface_profile ||
              squared_distance > radius2 ||
              dot3(photon.normal, entry->normal) <= 0.5f ||
              dot3(incoming, entry->normal) <= 0.0f ||
              dot3(incoming, entry->geometric_normal) <= 0.0f)
            continue;
          const float32_t distance = sqrtf(squared_distance);
          if (distance > integrator->settings.ray_epsilon) {
            const Vec3 direction = mul(delta, 1.0f / distance);
            Vec3 visibility = {};
            // Preserve the existing photon kernel's local visibility test.
            // There is deliberately no air-visibility ray from exit to entry.
            if (!shadow_transmittance(
                    integrator,
                    add(entry->position,
                        mul(direction, integrator->settings.ray_epsilon)),
                    direction, distance - integrator->settings.ray_epsilon,
                    *media, &visibility, out_error))
              return false_v;
            if (max_component(visibility) <= 1.0e-6f)
              continue;
          }
          // Landing density already includes the incident cosine. The
          // BSSRDF's Lambert 1/pi is applied once by the spatial event.
          result = add(result, mul(photon.flux, density_scale));
        }
      }
  if (!finite_nonnegative3(result)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  *out_irradiance = result;
  return true_v;
}

}  // namespace

extern "C" bool8_t vkr_bake_integrator_init(
    const VkrBakeIntegratorSettings *settings, VkrBakeIntegrator *out_integrator,
    VkrBakeIntegratorError *out_error) {
  set_error(VKR_BAKE_INTEGRATOR_ERROR_NONE, out_error);
  if (!settings || !out_integrator) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
    return false_v;
  }
  if (!scene_valid(*settings)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_SCENE, out_error);
    return false_v;
  }
  out_integrator->settings = *settings;
  return true_v;
}

extern "C" bool8_t vkr_bake_integrator_trace(
    const VkrBakeIntegrator *integrator, Vec3 origin, Vec3 direction,
    uint64_t seed, VkrBakeIntegratorResult *out_result,
    VkrBakeIntegratorError *out_error) {
  set_error(VKR_BAKE_INTEGRATOR_ERROR_NONE, out_error);
  if (!integrator || !out_result || !finite3(origin) || !unit3(direction)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
    return false_v;
  }
  VkrBakePcg32 rng = {};
  pcg_seed(&rng, seed);
  VkrBakeMediumStack media = {};
  VkrBakeRay ray = {.origin = origin,
                     .direction = direction,
                     .t_min = integrator->settings.ray_epsilon,
                     .t_max = k_ray_max};
  Vec3 radiance = vec3_new(0, 0, 0);
  Vec3 throughput = vec3_new(1, 1, 1);
  uint32_t total_skips = 0u;
  uint32_t depth = 0u;
  bool8_t has_diffuse_vertex = false_v;
  bool8_t chain_has_photon_surface = false_v;
  while (depth < integrator->settings.max_depth) {
    VkrBakeSurface surface = {};
    const VkrBakeRayEvent event =
        next_surface(integrator, &ray, &rng, &media, &throughput, nullptr,
                     &total_skips, &surface, out_error);
    if (event == VkrBakeRayEvent::Error) return false_v;
    if (event == VkrBakeRayEvent::Miss) {
      if (integrator->settings.environment_radiance) {
        const Vec3 environment =
            integrator->settings.environment_radiance(integrator->settings.environment_user,
                                                      ray.direction);
        if (!finite_nonnegative3(environment)) {
          set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
          return false_v;
        }
        radiance = add(radiance, mul(throughput, environment));
      }
      break;
    }

    const Vec3 wo = to_local(&surface, neg(ray.direction));
    if (wo.z <= 0.0f || !unit3(wo)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return false_v;
    }
    float32_t eta_target = 1.0f;
    if (!medium_target(&media, &surface, &eta_target)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_MISMATCH, out_error);
      return false_v;
    }
    VkrBakeBsdf bsdf = {};
    if (!vkr_bake_bsdf_init(surface.material, medium_ior(&media), eta_target,
                            !is_volume(surface.material), surface.clearcoat_normal, to_local(&surface, surface.geometric_normal), &bsdf)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return false_v;
    }
    radiance = add(radiance, mul(throughput,
        mul(max0(surface.material.emissive), vkr_bake_bsdf_base_transmission(&bsdf, wo))));
    const bool8_t diffuse = diffuse_capable(surface.material);
    const bool8_t photon_map_active =
        integrator->settings.photon_map &&
        integrator->settings.photon_map->grid_ready;
    if (!diffuse && caustic_chain_surface(surface.material))
      chain_has_photon_surface = true_v;
    const bool8_t subsurface_active =
        surface.material.subsurface_strength > 0.0f;
    const bool8_t spatial = subsurface_active && pcg_float(&rng) < 0.5f;
    const Vec3 scattering_throughput =
        subsurface_active ? mul(throughput, 2.0f) : throughput;
    if (spatial) {
      VkrBakeSurface entry = {};
      Vec3 spatial_weight = {};
      const VkrBakeRayEvent sampled =
          sample_subsurface(integrator, &surface, &bsdf, wo, &rng, &entry,
                            &spatial_weight, out_error);
      if (sampled == VkrBakeRayEvent::Error)
        return false_v;
      if (sampled == VkrBakeRayEvent::Miss ||
          max_component(spatial_weight) == 0.0f)
        break;
      throughput = mul(scattering_throughput, spatial_weight);
      Vec3 direct = {}, caustic = {};
      if (!subsurface_direct_irradiance(integrator, &entry, &media, &rng,
                                        &direct, out_error) ||
          !subsurface_photon_irradiance(integrator, &entry, &media, &caustic,
                                        out_error))
        return false_v;
      radiance = add(radiance,
                     mul(throughput, mul(add(direct, caustic), 1.0f / k_pi)));
      // The event is diffuse even at strength one, where the local Lambert
      // lobe is absent. Later analytic-light connections behind glass/metal
      // use the existing caustic-chain exclusion against photon double
      // counting.
      has_diffuse_vertex = true_v;
      chain_has_photon_surface = false_v;
      const float32_t radial_squared = pcg_float(&rng);
      const float32_t phi = 2.0f * k_pi * pcg_float(&rng);
      const float32_t radius = sqrtf(radial_squared);
      const Vec3 wi = vec3_new(radius * cosf(phi), radius * sinf(phi),
                               sqrtf(1.0f - radial_squared));
      ray.direction = normalize3(to_world(&entry, wi));
      // Sampling cos(theta)/pi cancels the BSSRDF's cos(theta)/pi. Reject
      // directions below the actual surface without conditioning the sampler.
      if (dot3(ray.direction, entry.geometric_normal) <= 0.0f)
        break;
      ray.origin = add(entry.position,
                       mul(ray.direction, integrator->settings.ray_epsilon));
      if (!finite_nonnegative3(throughput) || !unit3(ray.direction)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
    } else {
      Vec3 direct = {};
      if ((diffuse || !photon_map_active || !has_diffuse_vertex ||
           !chain_has_photon_surface) &&
          !direct_lighting(integrator, &surface, wo, &media, &rng, &direct,
                           out_error))
        return false_v;
      radiance = add(radiance, mul(scattering_throughput, direct));
      Vec3 caustic = {};
      if (!photon_density(integrator, &surface, wo, &media, &caustic,
                          out_error))
        return false_v;
      radiance = add(radiance, mul(scattering_throughput, caustic));
      if (diffuse) {
        has_diffuse_vertex = true_v;
        chain_has_photon_surface = false_v;
      }
      const VkrBakeBsdfSample sample = vkr_bake_bsdf_sample(
          &bsdf, wo, pcg_float(&rng), pcg_float(&rng), pcg_float(&rng));
      const float32_t cosine = fabsf(sample.wi.z);
      if (sample.pdf <= 0.0f || cosine <= 0.0f ||
          !finite_nonnegative3(sample.f))
        break;
      throughput =
          mul(scattering_throughput, mul(sample.f, cosine / sample.pdf));
      if (!finite_nonnegative3(throughput)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
      if (sample.transmitted && !bsdf.thin_walled &&
          !medium_cross(&media, &surface, out_error))
        return false_v;
      ray.direction = normalize3(to_world(&surface, sample.wi));
      if (!unit3(ray.direction)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
      ray.origin = add(surface.position,
                       mul(ray.direction, integrator->settings.ray_epsilon));
    }
    ray.t_min = integrator->settings.ray_epsilon;
    ray.t_max = k_ray_max;
    ++depth;
    if (integrator->settings.rr_start_depth > 0u &&
        depth >= integrator->settings.rr_start_depth) {
      const float32_t survival = fminf(0.95f, max_component(throughput));
      if (survival <= 0.0f || pcg_float(&rng) >= survival)
        break;
      throughput = mul(throughput, 1.0f / survival);
    }
  }
  if (!finite_nonnegative3(radiance)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
    return false_v;
  }
  *out_result = {.radiance = radiance,
                 .surface_depth = depth,
                 .transparent_layers = total_skips};
  return true_v;
}

extern "C" bool8_t vkr_bake_integrator_photon_map_reset(
    const VkrBakeIntegrator *integrator, VkrBakePhotonSettings settings,
    VkrBakePhotonMap *in_out_map, VkrBakeIntegratorError *out_error) {
  set_error(VKR_BAKE_INTEGRATOR_ERROR_NONE, out_error);
  uint32_t cell_count = 0u;
  if (!integrator || !in_out_map || !in_out_map->photons ||
      !in_out_map->photon_capacity || !in_out_map->cell_offsets ||
      !photon_settings_valid(settings, &cell_count) ||
      !integrator->settings.scene.bvh->node_count ||
      !photon_bounds_valid(integrator->settings.scene.bvh->nodes[0].bounds)) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
    return false_v;
  }
  in_out_map->photon_count = 0u;
  in_out_map->grid_dimensions[0] = settings.grid_dimensions[0];
  in_out_map->grid_dimensions[1] = settings.grid_dimensions[1];
  in_out_map->grid_dimensions[2] = settings.grid_dimensions[2];
  in_out_map->bounds = integrator->settings.scene.bvh->nodes[0].bounds;
  in_out_map->radius = settings.radius;
  in_out_map->grid_ready = false_v;
  for (uint32_t cell = 0u; cell <= cell_count; ++cell)
    in_out_map->cell_offsets[cell] = 0u;
  return true_v;
}

extern "C" bool8_t vkr_bake_integrator_emit_photons(
    const VkrBakeIntegrator *integrator, VkrBakePhotonSettings settings,
    VkrBakePhotonMap *in_out_map, VkrBakeIntegratorError *out_error) {
  set_error(VKR_BAKE_INTEGRATOR_ERROR_NONE, out_error);
  uint32_t cell_count = 0u;
  if (!integrator || !in_out_map || !in_out_map->photons ||
      !photon_settings_valid(settings, &cell_count) ||
      in_out_map->grid_ready || in_out_map->radius != settings.radius ||
      in_out_map->grid_dimensions[0] != settings.grid_dimensions[0] ||
      in_out_map->grid_dimensions[1] != settings.grid_dimensions[1] ||
      in_out_map->grid_dimensions[2] != settings.grid_dimensions[2] ||
      in_out_map->photon_count > in_out_map->photon_capacity ||
      settings.batch_count > in_out_map->photon_capacity - in_out_map->photon_count) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
    return false_v;
  }
  float32_t total_weight = 0.0f;
  for (uint32_t light = 0u; light < integrator->settings.scene.light_count;
       ++light)
    total_weight += photon_light_weight(integrator,
                                         integrator->settings.scene.lights[light]);
  if (!(total_weight > 0.0f) || !std::isfinite(total_weight)) return true_v;

  for (uint32_t photon_index = 0u; photon_index < settings.batch_count;
       ++photon_index) {
    const uint64_t sample_index = settings.sample_offset + photon_index;
    VkrBakePcg32 rng = {};
    pcg_seed(&rng, settings.seed ^ (sample_index * 0x9e3779b97f4a7c15ULL));
    float32_t selected = pcg_float(&rng) * total_weight;
    uint32_t light_index = 0u;
    float32_t light_weight = 0.0f;
    for (; light_index < integrator->settings.scene.light_count; ++light_index) {
      light_weight = photon_light_weight(
          integrator, integrator->settings.scene.lights[light_index]);
      if (light_weight > 0.0f && selected < light_weight) break;
      selected -= light_weight;
    }
    if (light_index == integrator->settings.scene.light_count) continue;
    VkrBakePhotonEmission emission = {};
    if (!photon_emit(integrator, integrator->settings.scene.lights[light_index],
                     light_weight / total_weight, settings.photon_count, &rng,
                     &emission)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return false_v;
    }
    VkrBakeMediumStack media = {};
    VkrBakeRay ray = {.origin = emission.origin,
                       .direction = emission.direction,
                       .t_min = integrator->settings.ray_epsilon,
                       .t_max = k_ray_max};
    Vec3 throughput = emission.flux;
    uint32_t transparent_layers = 0u;
    float32_t traveled = 0.0f;
    bool8_t first_surface = true_v;
    bool8_t had_caustic_chain = false_v;
    for (uint32_t depth = 0u; depth < settings.max_depth; ++depth) {
      VkrBakeSurface surface = {};
      const VkrBakeRayEvent event = next_surface(
          integrator, &ray, &rng, &media, &throughput, &traveled,
          &transparent_layers, &surface, out_error);
      if (event == VkrBakeRayEvent::Error) {
        std::fprintf(stderr, "Bake photon transport failed: sample=%llu depth=%u\n",
                     (unsigned long long)sample_index, depth);
        return false_v;
      }
      if (event == VkrBakeRayEvent::Miss) break;
      if (first_surface) {
        throughput = mul(throughput,
                         photon_first_segment_scale(&emission, traveled));
        first_surface = false_v;
        if (!finite_nonnegative3(throughput)) {
          set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
          return false_v;
        }
      }
      if (diffuse_capable(surface.material)) {
        if (had_caustic_chain) {
          if (in_out_map->photon_count >= in_out_map->photon_capacity) {
            set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
            return false_v;
          }
          in_out_map->photons[in_out_map->photon_count++] = {
              .position = surface.position,
              .incident_direction = ray.direction,
              .normal = surface.normal,
              .flux = throughput,
              .source_instance_index = surface.triangle->source_instance_index,
              .subsurface_profile = surface.material.subsurface_profile};
        }
        break;
      }
      if (caustic_chain_surface(surface.material))
        had_caustic_chain = true_v;
      float32_t eta_target = 1.0f;
      if (!medium_target(&media, &surface, &eta_target)) {
        std::fprintf(stderr, "Bake photon transport failed: sample=%llu depth=%u\n",
                     (unsigned long long)sample_index, depth);
        set_error(VKR_BAKE_INTEGRATOR_ERROR_MEDIUM_STACK_MISMATCH, out_error);
        return false_v;
      }
      VkrBakeBsdf bsdf = {};
      if (!vkr_bake_bsdf_init(surface.material, medium_ior(&media), eta_target,
                              !is_volume(surface.material), surface.clearcoat_normal, to_local(&surface, surface.geometric_normal), &bsdf)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
      const Vec3 wo = to_local(&surface, neg(ray.direction));
      const VkrBakeBsdfSample sample =
          vkr_bake_bsdf_sample(&bsdf, wo, pcg_float(&rng), pcg_float(&rng),
                                pcg_float(&rng));
      const float32_t cosine = fabsf(sample.wi.z);
      if (sample.pdf <= 0.0f || cosine <= 0.0f ||
          !finite_nonnegative3(sample.f))
        break;
      throughput = mul(throughput, mul(sample.f, cosine / sample.pdf));
      /* Photons use importance transport: cancel the BSDF radiance eta^-2. */
      if (sample.transmitted && !bsdf.thin_walled)
        throughput = mul(throughput, sample.eta_ratio * sample.eta_ratio);
      if (!finite_nonnegative3(throughput)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
      if (sample.transmitted && !bsdf.thin_walled &&
          !medium_cross(&media, &surface, out_error)) {
        std::fprintf(stderr, "Bake photon transport failed: sample=%llu depth=%u\n",
                     (unsigned long long)sample_index, depth);
        return false_v;
      }
      ray.direction = normalize3(to_world(&surface, sample.wi));
      if (!unit3(ray.direction)) {
        set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
        return false_v;
      }
      ray.origin = add(surface.position,
                       mul(ray.direction, integrator->settings.ray_epsilon));
      ray.t_min = integrator->settings.ray_epsilon;
      ray.t_max = k_ray_max;
    }
  }
  return true_v;
}

extern "C" bool8_t vkr_bake_integrator_build_photon_grid(
    VkrBakePhotonMap *in_out_map, VkrBakeIntegratorError *out_error) {
  set_error(VKR_BAKE_INTEGRATOR_ERROR_NONE, out_error);
  if (!in_out_map || !in_out_map->photons || !in_out_map->cell_offsets ||
      !photon_bounds_valid(in_out_map->bounds) ||
      !std::isfinite(in_out_map->radius) || in_out_map->radius <= 0.0f) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
    return false_v;
  }
  const uint64_t cells64 = (uint64_t)in_out_map->grid_dimensions[0] *
      in_out_map->grid_dimensions[1] * in_out_map->grid_dimensions[2];
  if (!in_out_map->grid_dimensions[0] || !in_out_map->grid_dimensions[1] ||
      !in_out_map->grid_dimensions[2] || cells64 > VKR_BAKE_PHOTON_GRID_MAX_CELLS) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
    return false_v;
  }
  const uint32_t cell_count = (uint32_t)cells64;
  if (in_out_map->photon_count > in_out_map->photon_capacity) {
    set_error(VKR_BAKE_INTEGRATOR_ERROR_INVALID_ARGUMENT, out_error);
    return false_v;
  }
  /* A valid photon phase may produce no qualifying caustic deposits.  Publish
     an explicit empty grid so the camera estimator reads zero density while
     preserving its direct-light partition. */
  if (in_out_map->photon_count == 0u) {
    for (uint32_t cell = 0u; cell <= cell_count; ++cell)
      in_out_map->cell_offsets[cell] = 0u;
    in_out_map->grid_ready = true_v;
    return true_v;
  }
  for (uint32_t photon = 0u; photon < in_out_map->photon_count; ++photon)
    if (!finite3(in_out_map->photons[photon].position) ||
        !unit3(in_out_map->photons[photon].incident_direction) ||
        !unit3(in_out_map->photons[photon].normal) ||
        !finite_nonnegative3(in_out_map->photons[photon].flux)) {
      set_error(VKR_BAKE_INTEGRATOR_ERROR_NONFINITE_TRANSPORT, out_error);
      return false_v;
    }
  std::sort(in_out_map->photons, in_out_map->photons + in_out_map->photon_count,
            [in_out_map](const VkrBakePhoton &a, const VkrBakePhoton &b) {
              return photon_cell_index(in_out_map, a.position) <
                     photon_cell_index(in_out_map, b.position);
            });
  uint32_t photon = 0u;
  in_out_map->cell_offsets[0] = 0u;
  for (uint32_t cell = 0u; cell < cell_count; ++cell) {
    while (photon < in_out_map->photon_count &&
           photon_cell_index(in_out_map, in_out_map->photons[photon].position) == cell)
      ++photon;
    in_out_map->cell_offsets[cell + 1u] = photon;
  }
  in_out_map->grid_ready = true_v;
  return true_v;
}

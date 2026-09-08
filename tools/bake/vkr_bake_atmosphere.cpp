#include "bake/vkr_bake_atmosphere.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr float k_pi = 3.14159265358979323846f;
constexpr uint32_t k_transmittance_width = VKR_ATMOSPHERE_TRANSMITTANCE_WIDTH;
constexpr uint32_t k_transmittance_height = VKR_ATMOSPHERE_TRANSMITTANCE_HEIGHT;
constexpr uint32_t k_multiple_size = VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE;
constexpr uint32_t k_source_size = VKR_ATMOSPHERE_SOURCE_SIZE;
constexpr uint32_t k_transmittance_samples = 40u;
constexpr uint32_t k_multiple_directions = 64u;
constexpr uint32_t k_multiple_samples = 20u;
constexpr uint32_t k_source_samples = 32u;
constexpr float k_max_optical_depth = 80.0f;

struct Medium {
  Vec3 rayleigh_scattering;
  Vec3 mie_scattering;
  Vec3 scattering;
  Vec3 extinction;
};

Vec3 make3(float x, float y, float z) { return vec3_new(x, y, z); }
Vec3 add(Vec3 a, Vec3 b) { return make3(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 sub(Vec3 a, Vec3 b) { return make3(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 mul(Vec3 a, Vec3 b) { return make3(a.x * b.x, a.y * b.y, a.z * b.z); }
Vec3 scale(Vec3 a, float s) { return make3(a.x * s, a.y * s, a.z * s); }
Vec3 exp3(Vec3 a) { return make3(expf(a.x), expf(a.y), expf(a.z)); }
Vec3 min3(Vec3 a, float v) {
  return make3(std::min(a.x, v), std::min(a.y, v), std::min(a.z, v));
}
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return sqrtf(std::max(dot(a, a), 0.0f)); }
Vec3 normalized(Vec3 value, Vec3 fallback) {
  const float l = length(value);
  return l > 1.0e-10f ? scale(value, 1.0f / l) : fallback;
}
Vec3 zero() { return make3(0, 0, 0); }
Vec3 one() { return make3(1, 1, 1); }
Vec3 clamp_nonnegative(Vec3 a) {
  return make3(std::max(a.x, 0.0f), std::max(a.y, 0.0f), std::max(a.z, 0.0f));
}

float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
Vec3 lerp3(Vec3 a, Vec3 b, float t) { return add(a, scale(sub(b, a), t)); }

float ray_sphere_nearest(Vec3 origin, Vec3 direction, float radius) {
  const float b = dot(origin, direction);
  const float c = dot(origin, origin) - radius * radius;
  const float d = b * b - c;
  if (d < 0.0f)
    return -1.0f;
  const float root = sqrtf(d);
  const float near_hit = -b - root;
  const float far_hit = -b + root;
  return near_hit >= 0.0f ? near_hit : (far_hit >= 0.0f ? far_hit : -1.0f);
}

float segment_limit(const VkrAtmosphereGpuParams &p, Vec3 origin,
                    Vec3 direction, bool *hits_ground) {
  *hits_ground = false;
  if (dot(origin, direction) < 0.0f &&
      dot(origin, origin) <= p.planet.x * p.planet.x) {
    *hits_ground = true;
    return 0.0f;
  }
  const float b = dot(origin, direction);
  const float top = std::max(
      -b + sqrtf(std::max(b * b - dot(origin, origin) + p.planet.y * p.planet.y,
                          0.0f)),
      0.0f);
  const float bottom = ray_sphere_nearest(origin, direction, p.planet.x);
  *hits_ground = dot(origin, direction) < 0.0f && bottom >= 0.0f &&
                 (top < 0.0f || bottom < top);
  return *hits_ground ? bottom : top;
}

float height(const VkrAtmosphereGpuParams &p, Vec3 position) {
  return std::max(length(position) - p.planet.x, 0.0f);
}
float exponential_density(float h, float scale_height) {
  return scale_height > 1.0e-6f ? expf(-std::max(h, 0.0f) / scale_height)
                                : 0.0f;
}
float ozone_density(const VkrAtmosphereGpuParams &p, float h) {
  return saturate(1.0f - fabsf(h - p.ozone.w) / std::max(p.ground.w, 1.0e-6f));
}
Medium medium(const VkrAtmosphereGpuParams &p, Vec3 position) {
  const float h = height(p, position);
  const float rayleigh = exponential_density(h, p.rayleigh.w);
  const float mie = exponential_density(h, p.mie_scattering.w);
  const float ozone = ozone_density(p, h);
  Medium result = {};
  result.rayleigh_scattering =
      scale(make3(p.rayleigh.x, p.rayleigh.y, p.rayleigh.z), rayleigh);
  result.mie_scattering = scale(
      make3(p.mie_scattering.x, p.mie_scattering.y, p.mie_scattering.z), mie);
  result.scattering = add(result.rayleigh_scattering, result.mie_scattering);
  result.extinction =
      add(add(result.rayleigh_scattering,
              scale(make3(p.mie_extinction.x, p.mie_extinction.y,
                          p.mie_extinction.z),
                    mie)),
          scale(make3(p.ozone.x, p.ozone.y, p.ozone.z), ozone));
  return result;
}

bool sun_occluded(const VkrAtmosphereGpuParams &p, Vec3 position, Vec3 sun) {
  return dot(position, sun) < 0.0f &&
         ray_sphere_nearest(position, sun, p.planet.x) >= 0.0f;
}

void transmittance_params(const VkrAtmosphereGpuParams &p, float u, float v,
                          float *out_radius, float *out_mu) {
  const float bottom = p.planet.x, top = p.planet.y;
  const float H = sqrtf(std::max(top * top - bottom * bottom, 0.0f));
  const float rho = H * saturate(v);
  const float radius = sqrtf(rho * rho + bottom * bottom);
  const float min_distance = top - radius;
  const float max_distance = rho + H;
  const float distance =
      min_distance + saturate(u) * (max_distance - min_distance);
  float mu = distance > 1.0e-6f
                 ? (top * top - radius * radius - distance * distance) /
                       (2.0f * radius * distance)
                 : 1.0f;
  *out_radius = radius;
  *out_mu = std::clamp(mu, -1.0f, 1.0f);
}

Vec2 transmittance_uv(const VkrAtmosphereGpuParams &p, float radius, float mu) {
  const float bottom = p.planet.x, top = p.planet.y;
  const float H = sqrtf(std::max(top * top - bottom * bottom, 0.0f));
  const float rho = sqrtf(std::max(radius * radius - bottom * bottom, 0.0f));
  const float discriminant = radius * radius * (mu * mu - 1.0f) + top * top;
  const float distance =
      std::max(-radius * mu + sqrtf(std::max(discriminant, 0.0f)), 0.0f);
  const float min_distance = top - radius;
  const float max_distance = rho + H;
  return vec2_new(saturate((distance - min_distance) /
                           std::max(max_distance - min_distance, 1.0e-6f)),
                  saturate(rho / std::max(H, 1.0e-6f)));
}
Vec2 multi_uv(const VkrAtmosphereGpuParams &p, float radius, float sun_mu) {
  return vec2_new(saturate(sun_mu * .5f + .5f),
                  saturate((radius - p.planet.x) /
                           std::max(p.planet.y - p.planet.x, 1.0e-6f)));
}

Vec3 sample_lut(const std::vector<Vec3> &lut, uint32_t width, uint32_t height,
                Vec2 uv) {
  const float x = saturate(uv.x) * (float)width - .5f;
  const float y = saturate(uv.y) * (float)height - .5f;
  const int x_floor = (int)floorf(x), y_floor = (int)floorf(y);
  const int x0 = std::clamp(x_floor, 0, (int)width - 1);
  const int y0 = std::clamp(y_floor, 0, (int)height - 1);
  const int x1 = std::clamp(x_floor + 1, 0, (int)width - 1);
  const int y1 = std::clamp(y_floor + 1, 0, (int)height - 1);
  const float tx = x - floorf(x), ty = y - floorf(y);
  return lerp3(lerp3(lut[(uint32_t)y0 * width + (uint32_t)x0],
                     lut[(uint32_t)y0 * width + (uint32_t)x1], tx),
               lerp3(lut[(uint32_t)y1 * width + (uint32_t)x0],
                     lut[(uint32_t)y1 * width + (uint32_t)x1], tx),
               ty);
}

Vec3 transmittance_integral(const VkrAtmosphereGpuParams &p, Vec3 position,
                            Vec3 direction) {
  bool ground = false;
  const float distance = segment_limit(p, position, direction, &ground);
  if (distance <= 0.0f || ground)
    return zero();
  const float step = distance / (float)k_transmittance_samples;
  Vec3 optical = zero();
  for (uint32_t i = 0; i < k_transmittance_samples; ++i)
    optical = add(
        optical, scale(medium(p, add(position,
                                     scale(direction, ((float)i + .5f) * step)))
                           .extinction,
                       step));
  return exp3(scale(min3(optical, k_max_optical_depth), -1.0f));
}

Vec3 segment_source_integral(Vec3 source, Vec3 extinction, float step) {
  const Vec3 optical =
      min3(scale(extinction, std::max(step, 0.0f)), k_max_optical_depth);
  const Vec3 t = exp3(scale(optical, -1.0f));
  const Vec3 linear = scale(source, step);
  const Vec3 exact =
      make3(source.x * (1.0f - t.x) / std::max(extinction.x, 1.0e-6f),
            source.y * (1.0f - t.y) / std::max(extinction.y, 1.0e-6f),
            source.z * (1.0f - t.z) / std::max(extinction.z, 1.0e-6f));
  return make3(extinction.x * step < 1.0e-4f ? linear.x : exact.x,
               extinction.y * step < 1.0e-4f ? linear.y : exact.y,
               extinction.z * step < 1.0e-4f ? linear.z : exact.z);
}

float rayleigh_phase(float cosine) {
  return 3.0f * (1.0f + cosine * cosine) / (16.0f * k_pi);
}
float mie_phase(float g, float cosine) {
  const float g2 = g * g;
  const float d = std::max(1.0f + g2 - 2.0f * g * cosine, 1.0e-6f);
  const float c = 3.0f * (1.0f - g2) / (8.0f * k_pi * (2.0f + g2));
  return c * (1.0f + cosine * cosine) / powf(d, 1.5f);
}
Vec3 cube_direction(uint32_t face, float u, float v) {
  const float x = u * 2.0f - 1.0f, y = v * 2.0f - 1.0f;
  switch (face) {
  case 0:
    return normalized(make3(1, -y, -x), make3(0, 1, 0));
  case 1:
    return normalized(make3(-1, -y, x), make3(0, 1, 0));
  case 2:
    return normalized(make3(x, 1, y), make3(0, 1, 0));
  case 3:
    return normalized(make3(x, -1, -y), make3(0, 1, 0));
  case 4:
    return normalized(make3(x, -y, 1), make3(0, 1, 0));
  default:
    return normalized(make3(-x, -y, -1), make3(0, 1, 0));
  }
}

Vec3 source_radiance(Vec3 radiance) {
  const float peak = std::max(radiance.x, std::max(radiance.y, radiance.z));
  return scale(radiance, std::min(1.0f, 5000.0f / std::max(peak, 5000.0f)));
}

void cube_face_uv(Vec3 direction, uint32_t *out_face, Vec2 *out_uv) {
  const float ax = fabsf(direction.x), ay = fabsf(direction.y),
              az = fabsf(direction.z);
  uint32_t face = 0u;
  Vec2 uv = vec2_new(.5f, .5f);
  if (ax >= ay && ax >= az) {
    const float inv = .5f / ax;
    if (direction.x >= 0.0f) {
      face = 0;
      uv = vec2_new(.5f - direction.z * inv, .5f - direction.y * inv);
    } else {
      face = 1;
      uv = vec2_new(.5f + direction.z * inv, .5f - direction.y * inv);
    }
  } else if (ay >= az) {
    const float inv = .5f / ay;
    if (direction.y >= 0.0f) {
      face = 2;
      uv = vec2_new(.5f + direction.x * inv, .5f + direction.z * inv);
    } else {
      face = 3;
      uv = vec2_new(.5f + direction.x * inv, .5f - direction.z * inv);
    }
  } else {
    const float inv = .5f / az;
    if (direction.z >= 0.0f) {
      face = 4;
      uv = vec2_new(.5f + direction.x * inv, .5f - direction.y * inv);
    } else {
      face = 5;
      uv = vec2_new(.5f - direction.x * inv, .5f - direction.y * inv);
    }
  }
  *out_face = face;
  *out_uv = uv;
}

uint64_t fnv1a(uint64_t value, const void *bytes, size_t count) {
  const auto *data = static_cast<const uint8_t *>(bytes);
  for (size_t i = 0; i < count; ++i) {
    value ^= data[i];
    value *= UINT64_C(1099511628211);
  }
  return value;
}
} // namespace

bool vkr_bake_atmosphere_build(VkrBakeAtmosphere *out,
                               const VkrAtmosphereSettings *settings) {
  if (!out || !settings || !vkr_atmosphere_settings_valid(settings))
    return false;
  *out = {};
  if (!settings->enabled)
    return true;
  out->enabled = true_v;
  out->params = vkr_atmosphere_prepare(settings);
  const VkrAtmosphereGpuParams &p = out->params;
  std::vector<Vec3> trans(k_transmittance_width * k_transmittance_height);
  std::vector<Vec3> multi(k_multiple_size * k_multiple_size);
  for (uint32_t y = 0; y < k_transmittance_height; ++y)
    for (uint32_t x = 0; x < k_transmittance_width; ++x) {
      float radius = 0.0f, mu = 0.0f;
      transmittance_params(p, ((float)x + .5f) / k_transmittance_width,
                           ((float)y + .5f) / k_transmittance_height, &radius,
                           &mu);
      trans[y * k_transmittance_width + x] = transmittance_integral(
          p, make3(0, radius, 0),
          make3(sqrtf(std::max(1.0f - mu * mu, 0.0f)), mu, 0));
    }
  auto trans_sample = [&](Vec3 position, Vec3 direction) {
    const Vec3 up = normalized(position, make3(0, 1, 0));
    return sample_lut(
        trans, k_transmittance_width, k_transmittance_height,
        transmittance_uv(p, length(position), dot(up, direction)));
  };
  for (uint32_t y = 0; y < k_multiple_size; ++y)
    for (uint32_t x = 0; x < k_multiple_size; ++x) {
      const float radius = p.planet.x + ((float)y + .5f) / k_multiple_size *
                                            (p.planet.y - p.planet.x);
      const float mu = ((float)x + .5f) / k_multiple_size * 2.0f - 1.0f;
      const Vec3 sun = make3(0, sqrtf(std::max(1.0f - mu * mu, 0.0f)), mu);
      const Vec3 origin = make3(0, 0, radius);
      Vec3 sum_a = zero(), sum_l = zero();
      for (uint32_t lane = 0; lane < k_multiple_directions; ++lane) {
        const float z =
            1.0f - 2.0f * (((float)lane + .5f) / k_multiple_directions);
        const float phi =
            2.0f * k_pi * fmodf((float)lane * .61803398875f, 1.0f);
        const Vec3 direction =
            make3(cosf(phi) * sqrtf(std::max(1.0f - z * z, 0.0f)),
                  sinf(phi) * sqrtf(std::max(1.0f - z * z, 0.0f)), z);
        bool ground = false;
        const float distance = segment_limit(p, origin, direction, &ground);
        const float step = distance / k_multiple_samples;
        Vec3 t = one(), a = zero(), l = zero();
        for (uint32_t i = 0; i < k_multiple_samples; ++i) {
          const Vec3 position =
              add(origin, scale(direction, ((float)i + .5f) * step));
          const Medium m = medium(p, position);
          const Vec3 tr = exp3(scale(
              min3(scale(m.extinction, step), k_max_optical_depth), -1.0f));
          a = add(a, mul(t, segment_source_integral(m.scattering, m.extinction,
                                                    step)));
          const Vec3 ts = sun_occluded(p, position, sun)
                              ? zero()
                              : trans_sample(position, sun);
          l = add(l, mul(t, segment_source_integral(scale(mul(ts, m.scattering),
                                                          1.0f / (4.0f * k_pi)),
                                                    m.extinction, step)));
          t = mul(t, tr);
        }
        if (ground) {
          const Vec3 ground_point = add(origin, scale(direction, distance));
          const Vec3 up = normalized(ground_point, make3(0, 1, 0));
          const Vec3 surface = add(ground_point, scale(up, 1.0e-3f));
          const Vec3 ts = sun_occluded(p, surface, sun)
                              ? zero()
                              : trans_sample(surface, sun);
          l = add(l, scale(mul(mul(t, ts),
                               make3(p.ground.x, p.ground.y, p.ground.z)),
                           std::max(dot(up, sun), 0.0f) / k_pi));
        }
        sum_a = add(sum_a, a);
        sum_l = add(sum_l, l);
      }
      const Vec3 f = scale(sum_a, 1.0f / k_multiple_directions);
      const Vec3 l = scale(sum_l, 1.0f / k_multiple_directions);
      multi[y * k_multiple_size + x] = make3(l.x / std::max(1.0f - f.x, .001f),
                                             l.y / std::max(1.0f - f.y, .001f),
                                             l.z / std::max(1.0f - f.z, .001f));
    }
  auto multi_sample = [&](Vec3 position, Vec3 direction) {
    const Vec3 up = normalized(position, make3(0, 1, 0));
    return sample_lut(multi, k_multiple_size, k_multiple_size,
                      multi_uv(p, length(position), dot(up, direction)));
  };
  const Vec3 observer = make3(0, p.planet.x + p.planet.z, 0);
  const Vec3 sun = make3(p.sun.x, p.sun.y, p.sun.z);
  out->observer_irradiance = sun_occluded(p, observer, sun)
                                 ? zero()
                                 : mul(make3(p.solar.x, p.solar.y, p.solar.z),
                                       trans_sample(observer, sun));
  out->source_rgb.resize((size_t)6u * k_source_size * k_source_size);
  for (uint32_t face = 0; face < 6; ++face)
    for (uint32_t y = 0; y < k_source_size; ++y)
      for (uint32_t x = 0; x < k_source_size; ++x) {
        const Vec3 direction =
            cube_direction(face, ((float)x + .5f) / k_source_size,
                           ((float)y + .5f) / k_source_size);
        bool ground = false;
        const float distance = segment_limit(p, observer, direction, &ground);
        const float step = distance / k_source_samples;
        const float cosine = dot(direction, sun);
        const float pr = rayleigh_phase(cosine),
                    pm = mie_phase(p.planet.w, cosine);
        Vec3 t = one(), l = zero();
        for (uint32_t i = 0; i < k_source_samples; ++i) {
          const Vec3 position =
              add(observer, scale(direction, ((float)i + .5f) * step));
          const Medium m = medium(p, position);
          const Vec3 ts = sun_occluded(p, position, sun)
                              ? zero()
                              : trans_sample(position, sun);
          const Vec3 ms = multi_sample(position, sun);
          const Vec3 source =
              mul(make3(p.solar.x, p.solar.y, p.solar.z),
                  add(mul(ts, add(scale(m.rayleigh_scattering, pr),
                                  scale(m.mie_scattering, pm))),
                      mul(ms, m.scattering)));
          l = add(l,
                  mul(t, segment_source_integral(source, m.extinction, step)));
          t = mul(t, exp3(scale(
                         min3(scale(m.extinction, step), k_max_optical_depth),
                         -1.0f)));
        }
        out->source_rgb[((size_t)face * k_source_size + y) * k_source_size +
                        x] = source_radiance(clamp_nonnegative(l));
      }
  return true;
}

Vec3 vkr_bake_atmosphere_sample(const VkrBakeAtmosphere *atmosphere,
                                Vec3 direction) {
  uint32_t face = 0;
  Vec2 uv = {};
  cube_face_uv(direction, &face, &uv);
  const float x = saturate(uv.x) * k_source_size - .5f;
  const float y = saturate(uv.y) * k_source_size - .5f;
  const int x_floor = (int)floorf(x), y_floor = (int)floorf(y);
  const int x0 = std::clamp(x_floor, 0, (int)k_source_size - 1);
  const int y0 = std::clamp(y_floor, 0, (int)k_source_size - 1);
  const int x1 = std::clamp(x_floor + 1, 0, (int)k_source_size - 1);
  const int y1 = std::clamp(y_floor + 1, 0, (int)k_source_size - 1);
  const float tx = x - floorf(x), ty = y - floorf(y);
  const size_t base = (size_t)face * k_source_size * k_source_size;
  return lerp3(
      lerp3(atmosphere->source_rgb[base + (size_t)y0 * k_source_size + x0],
            atmosphere->source_rgb[base + (size_t)y0 * k_source_size + x1], tx),
      lerp3(atmosphere->source_rgb[base + (size_t)y1 * k_source_size + x0],
            atmosphere->source_rgb[base + (size_t)y1 * k_source_size + x1], tx),
      ty);
}

uint64_t vkr_bake_atmosphere_recipe_hash(const VkrBakeAtmosphere *atmosphere) {
  if (!atmosphere || !atmosphere->enabled)
    return 0u;
  uint64_t hash = UINT64_C(1469598103934665603);
  hash = fnv1a(hash, &VKR_BAKE_ATMOSPHERE_MODEL_VERSION,
               sizeof(VKR_BAKE_ATMOSPHERE_MODEL_VERSION));
  return fnv1a(hash, &atmosphere->params, sizeof(atmosphere->params));
}

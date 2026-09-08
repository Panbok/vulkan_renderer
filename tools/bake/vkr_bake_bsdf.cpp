#include "bake/vkr_bake_bsdf.h"

#include "vkr_dfg_lut.h"
#include "vkr_sheen_lut.h"
#include "vkr_anisotropy_lut.h"

#include <cmath>
#include <cstdint>

namespace {

constexpr float32_t k_pi = 3.14159265358979323846f;
constexpr float32_t k_min_roughness = 0.04f;

float32_t clamp01(float32_t value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

bool8_t finite3(Vec3 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

Vec3 add(Vec3 a, Vec3 b) { return vec3_new(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 sub(Vec3 a, Vec3 b) { return vec3_new(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 mul(Vec3 a, float32_t b) { return vec3_new(a.x * b, a.y * b, a.z * b); }
Vec3 mul(Vec3 a, Vec3 b) { return vec3_new(a.x * b.x, a.y * b.y, a.z * b.z); }
Vec3 one_minus(Vec3 value) {
  return vec3_new(1.0f - value.x, 1.0f - value.y, 1.0f - value.z);
}
Vec3 clamp01(Vec3 value) {
  return vec3_new(clamp01(value.x), clamp01(value.y), clamp01(value.z));
}
Vec3 rgb(Vec4 value) { return vec3_new(value.x, value.y, value.z); }
float32_t dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(Vec3 a, Vec3 b) {
  return vec3_new(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                  a.x * b.y - a.y * b.x);
}
float32_t length2(Vec3 value) { return dot3(value, value); }
Vec3 normalize3(Vec3 value) {
  const float32_t length_squared = length2(value);
  return length_squared > 1e-20f ? mul(value, 1.0f / sqrtf(length_squared))
                                 : vec3_new(0, 0, 0);
}
Vec3 reflect_wo(Vec3 wo, Vec3 wm) {
  return sub(mul(wm, 2.0f * dot3(wo, wm)), wo);
}
float32_t luminance(Vec3 value) {
  return fmaxf(0.0f, 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z);
}
bool8_t valid_direction(Vec3 value) {
  return finite3(value) && fabsf(length2(value) - 1.0f) <= 2e-3f;
}

float32_t half_to_float(uint16_t value) {
  const uint32_t sign = (uint32_t)(value & 0x8000u) << 16u;
  uint32_t exponent = (value >> 10u) & 31u;
  uint32_t mantissa = value & 1023u;
  uint32_t bits = 0u;
  if (exponent == 0u) {
    if (mantissa) {
      exponent = 113u;
      while ((mantissa & 1024u) == 0u) {
        mantissa <<= 1u;
        --exponent;
      }
      bits = sign | (exponent << 23u) | ((mantissa & 1023u) << 13u);
    } else
      bits = sign;
  } else if (exponent == 31u)
    bits = sign | 0x7f800000u | (mantissa << 13u);
  else
    bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
  float32_t result = 0.0f;
  MemCopy(&result, &bits, sizeof(result));
  return result;
}

Vec2 dfg(float32_t no_v, float32_t roughness) {
  const float32_t x = sqrtf(clamp01(no_v)) * (VKR_DFG_LUT_SIZE - 1u);
  const float32_t y = clamp01(roughness) * (VKR_DFG_LUT_SIZE - 1u);
  const uint32_t x0 = (uint32_t)floorf(x), y0 = (uint32_t)floorf(y);
  const uint32_t x1 = x0 + 1u < VKR_DFG_LUT_SIZE ? x0 + 1u : x0;
  const uint32_t y1 = y0 + 1u < VKR_DFG_LUT_SIZE ? y0 + 1u : y0;
  const float32_t tx = x - x0, ty = y - y0;
  const auto fetch = [](uint32_t ix, uint32_t iy) {
    const uint64_t offset = ((uint64_t)iy * VKR_DFG_LUT_SIZE + ix) * 2u;
    return vec2_new(half_to_float(vkr_dfg_lut_pixels[offset]),
                    half_to_float(vkr_dfg_lut_pixels[offset + 1u]));
  };
  const Vec2 a = fetch(x0, y0), b = fetch(x1, y0), c = fetch(x0, y1),
             d = fetch(x1, y1);
  return vec2_new(a.x + (b.x - a.x) * tx + (c.x - a.x) * ty +
                      (a.x - b.x - c.x + d.x) * tx * ty,
                  a.y + (b.y - a.y) * tx + (c.y - a.y) * ty +
                      (a.y - b.y - c.y + d.y) * tx * ty);
}

bool8_t sheen_active(const VkrBakeBsdf *bsdf) {
  const Vec3 color = bsdf->surface.sheen_color;
  return color.x > 0.0f || color.y > 0.0f || color.z > 0.0f;
}

float32_t sheen_energy(float32_t no_v, float32_t roughness) {
  const float32_t x = sqrtf(fmaxf(clamp01(no_v), VKR_SHEEN_MIN_NOV)) *
                      (VKR_SHEEN_ENERGY_LUT_SIZE - 1u);
  const float32_t y = (roughness - VKR_SHEEN_MIN_ROUGHNESS) /
                      (1.0f - VKR_SHEEN_MIN_ROUGHNESS) *
                      (VKR_SHEEN_ENERGY_LUT_SIZE - 1u);
  const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
  const uint32_t x1 = x0 + 1u < VKR_SHEEN_ENERGY_LUT_SIZE ? x0 + 1u : x0;
  const uint32_t y1 = y0 + 1u < VKR_SHEEN_ENERGY_LUT_SIZE ? y0 + 1u : y0;
  const auto fetch = [](uint32_t ix, uint32_t iy) {
    return half_to_float(
        vkr_sheen_energy_lut_pixels[iy * VKR_SHEEN_ENERGY_LUT_SIZE + ix]);
  };
  const float32_t a = fetch(x0, y0), b = fetch(x1, y0);
  const float32_t c = fetch(x0, y1), d = fetch(x1, y1);
  const float32_t tx = x - x0, ty = y - y0;
  return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
}

float32_t sheen_transmission(const VkrBakeBsdf *bsdf, Vec3 wo) {
  if (!sheen_active(bsdf))
    return 1.0f;
  const Vec3 color = bsdf->surface.sheen_color;
  const float32_t e = sheen_energy(wo.z, bsdf->surface.sheen_roughness);
  const float32_t allocated =
      clamp01(e + fminf(e * bsdf->sheen_allocation_reserve.x,
                        bsdf->sheen_allocation_reserve.y));
  return fmaxf(0.0f,
               1.0f - fmaxf(color.x, fmaxf(color.y, color.z)) * allocated);
}

float32_t sheen_lambda(float32_t cosine, float32_t alpha) {
  const float32_t t = (1.0f - alpha) * (1.0f - alpha);
  const float32_t a = 21.5473f + (25.3245f - 21.5473f) * t;
  const float32_t b = 3.82987f + (3.32435f - 3.82987f) * t;
  const float32_t c = 0.19823f + (0.16801f - 0.19823f) * t;
  const float32_t d = -1.97760f + (-1.27393f + 1.97760f) * t;
  const float32_t e = -4.32054f + (-4.85967f + 4.32054f) * t;
  const auto fitted = [=](float32_t x) {
    return a / (1.0f + b * powf(fmaxf(x, 1e-8f), c)) + d * x + e;
  };
  return expf(cosine < 0.5f ? fitted(cosine)
                            : 2.0f * fitted(0.5f) - fitted(1.0f - cosine));
}

float32_t sheen_distribution(float32_t no_h, float32_t roughness) {
  const float32_t inverse_alpha = 1.0f / (roughness * roughness);
  return (2.0f + inverse_alpha) *
         powf(fmaxf(0.0f, 1.0f - no_h * no_h), 0.5f * inverse_alpha) /
         (2.0f * k_pi);
}

Vec3 sheen_eval(const VkrBakeBsdf *bsdf, Vec3 wo, Vec3 wi, float32_t *out_pdf) {
  *out_pdf = 0.0f;
  if (wo.z <= 0.0f || wi.z <= 0.0f)
    return vec3_zero();
  const Vec3 h = normalize3(add(wo, wi));
  const float32_t wo_h = dot3(wo, h);
  if (h.z <= 0.0f || wo_h <= 0.0f)
    return vec3_zero();
  const float32_t roughness = bsdf->surface.sheen_roughness;
  const float32_t d = sheen_distribution(h.z, roughness);
  *out_pdf = d * h.z / (4.0f * wo_h);
  const float32_t no_v = fmaxf(wo.z, VKR_SHEEN_MIN_NOV);
  const float32_t no_l = fmaxf(wi.z, VKR_SHEEN_MIN_NOV);
  const float32_t alpha = roughness * roughness;
  const float32_t visibility =
      1.0f / ((1.0f + sheen_lambda(no_v, alpha) + sheen_lambda(no_l, alpha)) *
              4.0f * no_v * no_l);
  return mul(bsdf->surface.sheen_color,
             bsdf->sheen_normalization * d * visibility);
}

Vec3 sample_sheen_normal(float32_t roughness, float32_t u1, float32_t u2) {
  const float32_t alpha = roughness * roughness;
  const float32_t sine = powf(clamp01(u1), alpha / (1.0f + 2.0f * alpha));
  const float32_t phi = 2.0f * k_pi * clamp01(u2);
  return vec3_new(sine * cosf(phi), sine * sinf(phi),
                  sqrtf(fmaxf(0.0f, 1.0f - sine * sine)));
}

Vec3 fresnel_schlick(float32_t cosine, Vec3 f0) {
  const float32_t one_minus_cosine = 1.0f - clamp01(cosine);
  const float32_t factor = one_minus_cosine * one_minus_cosine *
                           one_minus_cosine * one_minus_cosine *
                           one_minus_cosine;
  const float32_t f90 = clamp01(fmaxf(f0.x, fmaxf(f0.y, f0.z)) * 25.0f);
  return add(f0, mul(sub(vec3_new(f90, f90, f90), f0), factor));
}

float32_t alpha_from_roughness(float32_t roughness) {
  const float32_t r = fmaxf(k_min_roughness, fminf(roughness, 1.0f));
  return r * r;
}

float32_t ggx_d(Vec3 wm, float32_t alpha) {
  const float32_t z = clamp01(wm.z);
  const float32_t a2 = alpha * alpha;
  const float32_t denominator = (1.0f - z * z) + a2 * z * z;
  return a2 / fmaxf(k_pi * denominator * denominator, 1e-20f);
}

float32_t smith_lambda(Vec3 w, float32_t alpha) {
  const float32_t z = fabsf(w.z);
  if (z <= 1e-6f)
    return INFINITY;
  const float32_t tan2 = fmaxf(0.0f, (1.0f - z * z) / (z * z));
  return 0.5f * (sqrtf(1.0f + alpha * alpha * tan2) - 1.0f);
}

float32_t smith_g2(Vec3 wo, Vec3 wi, float32_t alpha) {
  return 1.0f / fmaxf(1.0f + smith_lambda(wo, alpha) + smith_lambda(wi, alpha),
                      1e-20f);
}

float32_t smith_g1(Vec3 w, float32_t alpha) {
  return 1.0f / fmaxf(1.0f + smith_lambda(w, alpha), 1e-20f);
}

float32_t visible_normal_pdf(Vec3 wo, Vec3 wm, float32_t alpha) {
  const float32_t wo_z = fabsf(wo.z);
  const float32_t wo_wm = fabsf(dot3(wo, wm));
  if (wo_z <= 1e-6f || wo_wm <= 1e-6f || wm.z <= 0.0f)
    return 0.0f;
  return ggx_d(wm, alpha) * smith_g1(wo, alpha) * wo_wm / wo_z;
}

Vec3 sample_visible_normal(Vec3 wo, float32_t alpha, float32_t u1,
                           float32_t u2) {
  const Vec3 vh = normalize3(vec3_new(alpha * wo.x, alpha * wo.y, wo.z));
  const float32_t lensq = vh.x * vh.x + vh.y * vh.y;
  const Vec3 t1 =
      lensq > 1e-8f ? vec3_new(-vh.y / sqrtf(lensq), vh.x / sqrtf(lensq), 0.0f)
                    : vec3_new(1, 0, 0);
  const Vec3 t2 = vec3_new(vh.y * t1.z - vh.z * t1.y, vh.z * t1.x - vh.x * t1.z,
                           vh.x * t1.y - vh.y * t1.x);
  const float32_t radius = sqrtf(clamp01(u1));
  const float32_t phi = 2.0f * k_pi * clamp01(u2);
  const float32_t t1_sample = radius * cosf(phi);
  float32_t t2_sample = radius * sinf(phi);
  const float32_t blend = 0.5f * (1.0f + vh.z);
  t2_sample =
      (1.0f - blend) * sqrtf(fmaxf(0.0f, 1.0f - t1_sample * t1_sample)) +
      blend * t2_sample;
  const Vec3 nh = add(add(mul(t1, t1_sample), mul(t2, t2_sample)),
                      mul(vh, sqrtf(fmaxf(0.0f, 1.0f - t1_sample * t1_sample -
                                                    t2_sample * t2_sample))));
  return normalize3(vec3_new(alpha * nh.x, alpha * nh.y, fmaxf(0.0f, nh.z)));
}

Vec3 sample_anisotropic_normal(Vec3 wo, float32_t alpha_t, float32_t alpha_b, float32_t u1,
                           float32_t u2) {
  const Vec3 vh = normalize3(vec3_new(alpha_t * wo.x, alpha_b * wo.y, wo.z));
  const float32_t lensq = vh.x * vh.x + vh.y * vh.y;
  const Vec3 t1 =
      lensq > 1e-8f ? vec3_new(-vh.y / sqrtf(lensq), vh.x / sqrtf(lensq), 0.0f)
                    : vec3_new(1, 0, 0);
  const Vec3 t2 = vec3_new(vh.y * t1.z - vh.z * t1.y, vh.z * t1.x - vh.x * t1.z,
                           vh.x * t1.y - vh.y * t1.x);
  const float32_t radius = sqrtf(clamp01(u1));
  const float32_t phi = 2.0f * k_pi * clamp01(u2);
  const float32_t t1_sample = radius * cosf(phi);
  float32_t t2_sample = radius * sinf(phi);
  const float32_t blend = 0.5f * (1.0f + vh.z);
  t2_sample =
      (1.0f - blend) * sqrtf(fmaxf(0.0f, 1.0f - t1_sample * t1_sample)) +
      blend * t2_sample;
  const Vec3 nh = add(add(mul(t1, t1_sample), mul(t2, t2_sample)),
                      mul(vh, sqrtf(fmaxf(0.0f, 1.0f - t1_sample * t1_sample -
                                                    t2_sample * t2_sample))));
  return normalize3(vec3_new(alpha_t * nh.x, alpha_b * nh.y, fmaxf(0.0f, nh.z)));
}

/* Directions in the shading basis are rotated into the widened GGX axis. */
Vec3 anisotropy_local(const VkrBakeBsdf *bsdf, Vec3 w) {
  const Vec2 d = bsdf->surface.anisotropy_direction;
  return vec3_new(d.x*w.x + d.y*w.y, -d.y*w.x + d.x*w.y, w.z);
}

Vec3 anisotropy_base(const VkrBakeBsdf *bsdf, Vec3 w) {
  const Vec2 d = bsdf->surface.anisotropy_direction;
  return vec3_new(d.x*w.x - d.y*w.y, d.y*w.x + d.x*w.y, w.z);
}

float32_t anisotropy_alpha_t(const VkrBakeBsdf *bsdf, float32_t alpha_b) {
  const float32_t strength = bsdf->surface.anisotropy_strength;
  return alpha_b + (1.0f - alpha_b) * strength * strength;
}

float32_t anisotropy_d(Vec3 wm, float32_t at, float32_t ab) {
  const float32_t x = wm.x / at, y = wm.y / ab;
  const float32_t denominator = x*x + y*y + wm.z*wm.z;
  return 1.0f / (k_pi * at * ab * denominator * denominator);
}

float32_t anisotropy_lambda(Vec3 w, float32_t at, float32_t ab) {
  const float32_t x = w.x*at, y = w.y*ab;
  return 0.5f * (sqrtf(1.0f + (x*x+y*y)/(w.z*w.z)) - 1.0f);
}

Vec2 base_dfg(const VkrBakeBsdf *bsdf, Vec3 wo) {
  if (bsdf->surface.anisotropy_strength == 0.0f)
    return dfg(fmaxf(wo.z, 0.0f), bsdf->surface.roughness);
  const Vec3 local = anisotropy_local(bsdf, wo);
  float32_t ab[2];
  vkr_anisotropy_dfg_sample(fmaxf(wo.z, 0.0f), bsdf->surface.roughness,
                            bsdf->surface.anisotropy_strength,
                            atan2f(fabsf(local.y), fabsf(local.x)), ab);
  return vec2_new(ab[0], ab[1]);
}

bool8_t refract_wo(Vec3 wo, Vec3 wm, float32_t eta_i_over_t, Vec3 *out_wi) {
  const float32_t cos_o = dot3(wo, wm);
  const float32_t sin2_t =
      eta_i_over_t * eta_i_over_t * fmaxf(0.0f, 1.0f - cos_o * cos_o);
  if (sin2_t >= 1.0f)
    return false_v;
  const float32_t cos_t = sqrtf(fmaxf(0.0f, 1.0f - sin2_t));
  *out_wi = normalize3(
      add(mul(wo, -eta_i_over_t), mul(wm, eta_i_over_t * cos_o - cos_t)));
  return out_wi->z < -1e-6f;
}

Vec3 thin_fresnel(Vec3 f) {
  const Vec3 one_minus_f = one_minus(f);
  return clamp01(add(f, mul(mul(one_minus_f, one_minus_f),
                            vec3_new(f.x / fmaxf(1.0f - f.x * f.x, 1e-6f),
                                     f.y / fmaxf(1.0f - f.y * f.y, 1e-6f),
                                     f.z / fmaxf(1.0f - f.z * f.z, 1e-6f)))));
}

struct Weights {
  float32_t glass, opaque_spec, opaque_diffuse, total, diffuse_transmission;
};

Weights weights(const VkrBakeBsdf *bsdf, Vec3 wo) {
  const float32_t metallic = clamp01(bsdf->surface.metallic);
  const float32_t glass =
      (1.0f - metallic) * clamp01(bsdf->surface.transmission);
  const float32_t opaque = 1.0f - glass;
  const Vec3 f0 =
      add(mul(clamp01(bsdf->surface.dielectric_specular), 1.0f - metallic),
          mul(clamp01(rgb(bsdf->surface.base_color)), metallic));
  const Vec2 split_sum = base_dfg(bsdf, wo);
  const Vec3 scale =
      add(vec3_new(1, 1, 1),
          mul(f0, 1.0f / fmaxf(split_sum.x + split_sum.y, .25f) - 1.0f));
  const float32_t f90 = clamp01(fmaxf(f0.x, fmaxf(f0.y, f0.z)) * 25.0f);
  const Vec3 reflectance = clamp01(mul(
      add(mul(f0, split_sum.x),
          vec3_new(f90 * split_sum.y, f90 * split_sum.y, f90 * split_sum.y)),
      scale));
  const Vec3 diffuse = mul(one_minus(reflectance), 1.0f - metallic);
  Weights result = {glass, opaque * luminance(reflectance),
                    opaque * luminance(diffuse), 0.0f, 0.0f};
  if (bsdf->surface.diffuse_transmission_strength > 0.0f) {
    const float32_t strength = bsdf->surface.diffuse_transmission_strength;
    result.diffuse_transmission = result.opaque_diffuse * strength *
                                  luminance(bsdf->surface.diffuse_transmission_color);
    result.opaque_diffuse *= 1.0f - strength;
  }
  if (bsdf->surface.subsurface_strength > 0.0f)
    result.opaque_diffuse *= 1.0f - bsdf->surface.subsurface_strength;
  result.total = result.glass + result.opaque_spec + result.opaque_diffuse;
  if (bsdf->surface.diffuse_transmission_strength > 0.0f)
    result.total += result.diffuse_transmission;
  return result;
}

Vec3 opaque_f0(const VkrBakeBsdf *bsdf) {
  const float32_t metallic = clamp01(bsdf->surface.metallic);
  return add(mul(clamp01(bsdf->surface.dielectric_specular), 1.0f - metallic),
             mul(clamp01(rgb(bsdf->surface.base_color)), metallic));
}

Vec3 opaque_eval(const VkrBakeBsdf *bsdf, Vec3 wo, Vec3 wi,
                 float32_t *out_pdf) {
  *out_pdf = 0.0f;
  if (wo.z <= 0.0f || wi.z <= 0.0f)
    return vec3_new(0, 0, 0);
  const Vec3 wm = normalize3(add(wo, wi));
  if (wm.z <= 0.0f)
    return vec3_new(0, 0, 0);
  const float32_t alpha = alpha_from_roughness(bsdf->surface.roughness);
  float32_t d, g, pdf_spec;
  if (bsdf->surface.anisotropy_strength > 0.0f) {
    const float32_t at = anisotropy_alpha_t(bsdf, alpha);
    const Vec3 local_o = anisotropy_local(bsdf, wo), local_i = anisotropy_local(bsdf, wi);
    d = anisotropy_d(anisotropy_local(bsdf, wm), at, alpha);
    const float32_t lambda_o = anisotropy_lambda(local_o, at, alpha);
    g = 1.0f / (1.0f + lambda_o + anisotropy_lambda(local_i, at, alpha));
    pdf_spec = d / ((1.0f + lambda_o) * 4.0f * wo.z);
  } else {
    d = ggx_d(wm, alpha);
    g = smith_g2(wo, wi, alpha);
    pdf_spec = visible_normal_pdf(wo, wm, alpha) /
               fmaxf(4.0f * fabsf(dot3(wo, wm)), 1e-20f);
  }
  const Vec3 f0 = opaque_f0(bsdf);
  const Vec2 split_sum = base_dfg(bsdf, wo);
  const Vec3 scale =
      add(vec3_new(1, 1, 1),
          mul(f0, 1.0f / fmaxf(split_sum.x + split_sum.y, .25f) - 1.0f));
  const Vec3 specular =
      mul(mul(fresnel_schlick(fabsf(dot3(wo, wm)), f0), scale),
          d * g / fmaxf(4.0f * wo.z * wi.z, 1e-20f));
  const float32_t f90 = clamp01(fmaxf(f0.x, fmaxf(f0.y, f0.z)) * 25.0f);
  const Vec3 reflectance = clamp01(mul(
      add(mul(f0, split_sum.x),
          vec3_new(f90 * split_sum.y, f90 * split_sum.y, f90 * split_sum.y)),
      scale));
  const Vec3 diffuse = mul(
      mul(mul(one_minus(reflectance), 1.0f - clamp01(bsdf->surface.metallic)),
          clamp01(rgb(bsdf->surface.base_color))),
      1.0f / k_pi);
  *out_pdf = pdf_spec;
  if (bsdf->surface.subsurface_strength > 0.0f)
    return add(specular, mul(diffuse, 1.0f - bsdf->surface.subsurface_strength));
  return add(specular, bsdf->surface.diffuse_transmission_strength > 0.0f
                           ? mul(diffuse, 1.0f - bsdf->surface.diffuse_transmission_strength)
                           : diffuse);
}

// The same directional residual used by front Lambert, allocated to the
// opposite shading hemisphere. Tint absorbs its missing channels.
Vec3 diffuse_transmission_eval(const VkrBakeBsdf *bsdf, Vec3 wo) {
  const Vec3 f0 = opaque_f0(bsdf);
  const Vec2 split_sum = base_dfg(bsdf, wo);
  const Vec3 scale = add(vec3_new(1, 1, 1),
      mul(f0, 1.0f / fmaxf(split_sum.x + split_sum.y, .25f) - 1.0f));
  const float32_t f90 = clamp01(fmaxf(f0.x, fmaxf(f0.y, f0.z)) * 25.0f);
  const Vec3 reflectance = clamp01(mul(
      add(mul(f0, split_sum.x), vec3_new(f90 * split_sum.y, f90 * split_sum.y,
                                       f90 * split_sum.y)), scale));
  return mul(mul(mul(one_minus(reflectance), clamp01(rgb(bsdf->surface.base_color))),
                 bsdf->surface.diffuse_transmission_color),
             (1.0f - clamp01(bsdf->surface.metallic)) *
                 bsdf->surface.diffuse_transmission_strength / k_pi);
}

struct CoatFrame {
  Vec3 tangent;
  Vec3 bitangent;
  Vec3 normal;
};

CoatFrame coat_frame(Vec3 normal) {
  const Vec3 tangent = fabsf(normal.z) < 0.999f
                           ? normalize3(cross3(vec3_new(0, 0, 1), normal))
                           : vec3_new(1, 0, 0);
  return {tangent, cross3(normal, tangent), normal};
}

Vec3 coat_to_local(CoatFrame frame, Vec3 direction) {
  return vec3_new(dot3(direction, frame.tangent),
                  dot3(direction, frame.bitangent),
                  dot3(direction, frame.normal));
}

Vec3 coat_from_local(CoatFrame frame, Vec3 direction) {
  return add(
      add(mul(frame.tangent, direction.x), mul(frame.bitangent, direction.y)),
      mul(frame.normal, direction.z));
}

Vec3 coat_normal_for_view(const VkrBakeBsdf *bsdf, Vec3 wo) {
  const Vec3 normal = bsdf->clearcoat_normal;
  return dot3(normal, wo) >= 0.0f ? normal : mul(normal, -1.0f);
}

float32_t coat_reflectance(const VkrBakeBsdf *bsdf, Vec3 wo) {
  const float32_t no_v = dot3(wo, coat_normal_for_view(bsdf, wo));
  if (no_v <= 0.0f)
    return 0.0f;
  const Vec2 split_sum = dfg(no_v, bsdf->surface.clearcoat_roughness);
  constexpr float32_t f0 = 0.04f;
  const float32_t scale =
      1.0f + f0 * (1.0f / fmaxf(split_sum.x + split_sum.y, 0.25f) - 1.0f);
  return clamp01((f0 * split_sum.x + split_sum.y) * scale);
}

float32_t coat_transmission(const VkrBakeBsdf *bsdf, Vec3 wo) {
  if (bsdf->surface.clearcoat_factor == 0.0f)
    return 1.0f;
  return 1.0f - bsdf->surface.clearcoat_factor * coat_reflectance(bsdf, wo);
}

float32_t base_transmission(const VkrBakeBsdf *bsdf, Vec3 wo) {
  return coat_transmission(bsdf, wo) * sheen_transmission(bsdf, wo);
}

Vec3 coat_eval(const VkrBakeBsdf *bsdf, Vec3 wo, Vec3 wi, float32_t *out_pdf) {
  *out_pdf = 0.0f;
  const CoatFrame frame = coat_frame(coat_normal_for_view(bsdf, wo));
  const Vec3 coat_wo = coat_to_local(frame, wo);
  const Vec3 coat_wi = coat_to_local(frame, wi);
  if (coat_wo.z <= 0.0f || coat_wi.z <= 0.0f)
    return vec3_new(0, 0, 0);
  const Vec3 wm = normalize3(add(coat_wo, coat_wi));
  if (wm.z <= 0.0f)
    return vec3_new(0, 0, 0);
  const float32_t alpha =
      alpha_from_roughness(bsdf->surface.clearcoat_roughness);
  const float32_t d = ggx_d(wm, alpha);
  const float32_t g = smith_g2(coat_wo, coat_wi, alpha);
  constexpr float32_t f0 = 0.04f;
  const Vec2 split_sum = dfg(coat_wo.z, bsdf->surface.clearcoat_roughness);
  const float32_t scale =
      1.0f + f0 * (1.0f / fmaxf(split_sum.x + split_sum.y, 0.25f) - 1.0f);
  const Vec3 f =
      fresnel_schlick(fabsf(dot3(coat_wo, wm)), vec3_new(f0, f0, f0));
  const float32_t pdf_wm = visible_normal_pdf(coat_wo, wm, alpha);
  *out_pdf = pdf_wm / fmaxf(4.0f * fabsf(dot3(coat_wo, wm)), 1e-20f);
  const float32_t base_no_l = fabsf(wi.z);
  if (base_no_l <= 1e-6f)
    return vec3_new(0, 0, 0);
  /* Integrator callers multiply every evaluated lobe by base |N·L|. Convert
     this independent-normal lobe into that measure so it integrates with its
     own coat N·L, including tilted coat normal maps. */
  return mul(f, scale * d * g / fmaxf(4.0f * coat_wo.z * base_no_l, 1e-20f));
}

Vec3 glass_fresnel(const VkrBakeBsdf *bsdf, Vec3 wo, Vec3 wm,
                   bool8_t force_tir) {
  Vec3 result = fresnel_schlick(fabsf(dot3(wo, wm)),
                                clamp01(bsdf->surface.dielectric_specular));
  if (bsdf->thin_walled)
    result = thin_fresnel(result);
  return force_tir ? vec3_new(1, 1, 1) : result;
}

Vec3 glass_eval(const VkrBakeBsdf *bsdf, Vec3 wo, Vec3 wi, float32_t *out_pdf) {
  *out_pdf = 0.0f;
  if (wo.z <= 0.0f || fabsf(wi.z) <= 1e-6f)
    return vec3_new(0, 0, 0);
  const float32_t alpha = alpha_from_roughness(bsdf->surface.roughness);
  const bool8_t reflection = wi.z > 0.0f;
  const float32_t eta_ratio = bsdf->eta_transmitted / bsdf->eta_incident;
  const float32_t eta_i_over_t = 1.0f / eta_ratio;
  Vec3 wm = reflection ? normalize3(add(wo, wi))
                       : normalize3(add(mul(wi, eta_ratio), wo));
  if (wm.z < 0.0f)
    wm = mul(wm, -1.0f);
  if (wm.z <= 0.0f || dot3(wo, wm) <= 0.0f)
    return vec3_new(0, 0, 0);
  Vec3 refracted = {};
  const bool8_t can_refract = refract_wo(wo, wm, eta_i_over_t, &refracted);
  const Vec3 f =
      glass_fresnel(bsdf, wo, wm, !bsdf->thin_walled && !can_refract);
  const float32_t d = ggx_d(wm, alpha), g = smith_g2(wo, wi, alpha);
  const float32_t pdf_wm = visible_normal_pdf(wo, wm, alpha);
  const float32_t pr = clamp01(fmaxf(f.x, fmaxf(f.y, f.z)));
  if (reflection) {
    const float32_t pdf_reflect =
        pdf_wm / fmaxf(4.0f * fabsf(dot3(wo, wm)), 1e-20f);
    *out_pdf = pr * pdf_reflect;
    return mul(f, d * g / fmaxf(4.0f * wo.z * wi.z, 1e-20f));
  }
  if (bsdf->thin_walled || !can_refract)
    return vec3_new(0, 0, 0);
  const float32_t wi_wm = dot3(wi, wm), wo_wm = dot3(wo, wm);
  const float32_t denominator = wi_wm + wo_wm / eta_ratio;
  const float32_t denominator2 = denominator * denominator;
  if (denominator2 <= 1e-20f)
    return vec3_new(0, 0, 0);
  const float32_t dwm_dwi = fabsf(wi_wm) / denominator2;
  *out_pdf = (1.0f - pr) * pdf_wm * dwm_dwi;
  const float32_t factor =
      d * (1.0f) * g *
      fabsf(wi_wm * wo_wm / fmaxf(fabsf(wi.z * wo.z) * denominator2, 1e-20f)) /
      (eta_ratio * eta_ratio);
  return mul(mul(one_minus(f), clamp01(rgb(bsdf->surface.base_color))), factor);
}

VkrBakeBsdfEval evaluate_base(const VkrBakeBsdf *bsdf, Vec3 wo, Vec3 wi) {
  VkrBakeBsdfEval result = {};
  if (!bsdf || !valid_direction(wo) || !valid_direction(wi) || wo.z <= 0.0f)
    return result;
  const Weights probability = weights(bsdf, wo);
  if (probability.total <= 1e-8f)
    return result;
  const float32_t opaque_weight = 1.0f - probability.glass;
  if (wi.z > 0.0f) {
    float32_t opaque_pdf = 0.0f, glass_pdf = 0.0f;
    result.f =
        add(mul(opaque_eval(bsdf, wo, wi, &opaque_pdf), opaque_weight),
            mul(glass_eval(bsdf, wo, wi, &glass_pdf), probability.glass));
    const float32_t diffuse_pdf = wi.z * (1.0f / k_pi);
    result.pdf = (probability.opaque_spec * opaque_pdf +
                  probability.opaque_diffuse * diffuse_pdf +
                  probability.glass * glass_pdf) /
                 probability.total;
  } else if (bsdf->surface.diffuse_transmission_strength > 0.0f) {
    result.f = diffuse_transmission_eval(bsdf, wo);
    result.pdf = probability.diffuse_transmission * (-wi.z / k_pi) /
                 probability.total;
  } else if (!bsdf->thin_walled) {
    float32_t glass_pdf = 0.0f;
    result.f = mul(glass_eval(bsdf, wo, wi, &glass_pdf), probability.glass);
    result.pdf = probability.glass * glass_pdf / probability.total;
  }
  return result;
}

} // namespace

extern "C" bool8_t
vkr_bake_bsdf_init(VkrBakeMaterialSample surface, float32_t eta_incident,
                   float32_t eta_transmitted, bool8_t thin_walled,
                   Vec3 clearcoat_normal, Vec3 geometric_normal,
                   VkrBakeBsdf *out_bsdf) {
  if (!out_bsdf || !finite3(rgb(surface.base_color)) ||
      !finite3(surface.dielectric_specular) || !isfinite(surface.metallic) ||
      !isfinite(surface.roughness) || !isfinite(surface.transmission) ||
      !isfinite(surface.clearcoat_factor) ||
      !isfinite(surface.clearcoat_roughness) || !finite3(surface.sheen_color) ||
      !isfinite(surface.sheen_roughness) ||
      !isfinite(surface.subsurface_strength) || surface.subsurface_strength < 0.0f || surface.subsurface_strength > 1.0f ||
      surface.subsurface_profile >= 8u ||
      (surface.subsurface_strength > 0.0f &&
       (surface.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_BLEND || surface.transmission > 0.0f ||
        surface.thickness > 0.0f || surface.diffuse_transmission_strength > 0.0f)) ||
      !isfinite(surface.diffuse_transmission_strength) || surface.diffuse_transmission_strength < 0.0f || surface.diffuse_transmission_strength > 1.0f ||
      !finite3(surface.diffuse_transmission_color) ||
      surface.diffuse_transmission_color.x < 0.0f || surface.diffuse_transmission_color.x > 1.0f ||
      surface.diffuse_transmission_color.y < 0.0f || surface.diffuse_transmission_color.y > 1.0f ||
      surface.diffuse_transmission_color.z < 0.0f || surface.diffuse_transmission_color.z > 1.0f ||
      (surface.diffuse_transmission_strength > 0.0f &&
       (surface.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_BLEND || surface.transmission > 0.0f || surface.thickness > 0.0f)) ||
      !isfinite(surface.anisotropy_strength) || surface.anisotropy_strength < 0.0f || surface.anisotropy_strength > 1.0f ||
      (surface.anisotropy_strength > 0.0f && surface.transmission > 0.0f) ||
      !isfinite(eta_incident) ||
      !isfinite(eta_transmitted) || eta_incident <= 0.0f ||
      eta_transmitted <= 0.0f)
    return false_v;
  if (surface.anisotropy_strength > 0.0f) {
    const Vec2 axis = surface.anisotropy_direction;
    const float32_t length_sq = axis.x*axis.x + axis.y*axis.y;
    if (!isfinite(length_sq) || length_sq < 1e-12f) return false_v;
    const float32_t inverse_length = 1.0f / sqrtf(length_sq);
    surface.anisotropy_direction = vec2_new(axis.x*inverse_length, axis.y*inverse_length);
  }
  surface.metallic = clamp01(surface.metallic);
  surface.roughness = fmaxf(k_min_roughness, fminf(surface.roughness, 1.0f));
  surface.transmission = clamp01(surface.transmission);
  surface.clearcoat_factor = clamp01(surface.clearcoat_factor);
  surface.clearcoat_roughness =
      fmaxf(k_min_roughness, fminf(surface.clearcoat_roughness, 1.0f));
  surface.sheen_color = clamp01(surface.sheen_color);
  surface.sheen_roughness =
      fmaxf(VKR_SHEEN_MIN_ROUGHNESS, fminf(surface.sheen_roughness, 1.0f));
  surface.base_color.x = clamp01(surface.base_color.x);
  surface.base_color.y = clamp01(surface.base_color.y);
  surface.base_color.z = clamp01(surface.base_color.z);
  surface.dielectric_specular = clamp01(surface.dielectric_specular);
  Vec3 normalized_coat = vec3_new(0, 0, 1);
  Vec3 normalized_geometric = vec3_new(0, 0, 1);
  const bool8_t has_sheen = surface.sheen_color.x > 0.0f ||
                            surface.sheen_color.y > 0.0f ||
                            surface.sheen_color.z > 0.0f;
  if (surface.clearcoat_factor != 0.0f || has_sheen || surface.anisotropy_strength > 0.0f || surface.diffuse_transmission_strength > 0.0f || surface.subsurface_strength > 0.0f) {
    if (!finite3(geometric_normal) || length2(geometric_normal) <= 1e-20f)
      return false_v;
    normalized_geometric = normalize3(geometric_normal);
  }
  if (surface.clearcoat_factor != 0.0f) {
    if (!finite3(clearcoat_normal) || length2(clearcoat_normal) <= 1e-20f)
      return false_v;
    normalized_coat = normalize3(clearcoat_normal);
    if (dot3(normalized_coat, normalized_geometric) < 0.0f)
      normalized_coat = mul(normalized_coat, -1.0f);
  }
  float32_t sheen_normalization = 0.0f;
  Vec2 sheen_allocation_reserve = {};
  if (has_sheen) {
    /* G/B reserves and A normalization repeat over every view row. */
    const float32_t x = (surface.sheen_roughness - VKR_SHEEN_MIN_ROUGHNESS) /
                        (1.0f - VKR_SHEEN_MIN_ROUGHNESS) *
                        (VKR_SHEEN_LTC_LUT_SIZE - 1u);
    const uint32_t x0 = (uint32_t)x,
                   x1 = x0 + 1u < VKR_SHEEN_LTC_LUT_SIZE ? x0 + 1u : x0;
    const float32_t t = x - x0;
    const float32_t a =
        half_to_float(vkr_sheen_ltc_lut_pixels[1][x0 * 4u + 3u]);
    const float32_t b =
        half_to_float(vkr_sheen_ltc_lut_pixels[1][x1 * 4u + 3u]);
    sheen_normalization = a + (b - a) * t;
    const float32_t c =
        half_to_float(vkr_sheen_ltc_lut_pixels[1][x0 * 4u + 1u]);
    const float32_t d =
        half_to_float(vkr_sheen_ltc_lut_pixels[1][x1 * 4u + 1u]);
    sheen_allocation_reserve.x = c + (d - c) * t;
    const float32_t e =
        half_to_float(vkr_sheen_ltc_lut_pixels[1][x0 * 4u + 2u]);
    const float32_t f =
        half_to_float(vkr_sheen_ltc_lut_pixels[1][x1 * 4u + 2u]);
    sheen_allocation_reserve.y = e + (f - e) * t;
  }
  *out_bsdf = {.surface = surface,
               .eta_incident = eta_incident,
               .eta_transmitted = eta_transmitted,
               .thin_walled = thin_walled || surface.diffuse_transmission_strength > 0.0f,
               .clearcoat_normal = normalized_coat,
               .geometric_normal = normalized_geometric,
               .sheen_normalization = sheen_normalization,
               .sheen_allocation_reserve = sheen_allocation_reserve};
  return true_v;
}

extern "C" float32_t vkr_bake_bsdf_base_transmission(const VkrBakeBsdf *bsdf,
                                                     Vec3 wo) {
  return bsdf ? base_transmission(bsdf, wo) : 1.0f;
}

vkr_internal VkrBakeBsdfEval evaluate_layered(const VkrBakeBsdf *bsdf, Vec3 wo,
                                              Vec3 wi) {
  const float32_t geometric_no_l = dot3(wi, bsdf->geometric_normal);
  VkrBakeBsdfEval base = {};
  if ((wi.z > 0.0f && geometric_no_l > 1.0e-6f) ||
      (wi.z < 0.0f && geometric_no_l < -1.0e-6f))
    base = evaluate_base(bsdf, wo, wi);
  const float32_t base_weight = base_transmission(bsdf, wo);
  VkrBakeBsdfEval result = {
      .f = mul(base.f, base_weight),
      .pdf = base.pdf * base_weight,
  };
  if (geometric_no_l > 1.0e-6f) {
    const float32_t coat_base_weight = coat_transmission(bsdf, wo);
    if (bsdf->surface.clearcoat_factor != 0.0f) {
      float32_t coat_pdf = 0.0f;
      const Vec3 coat = coat_eval(bsdf, wo, wi, &coat_pdf);
      result.f = add(result.f, mul(coat, bsdf->surface.clearcoat_factor));
      result.pdf += (1.0f - coat_base_weight) * coat_pdf;
    }
    if (sheen_active(bsdf)) {
      float32_t sheen_pdf = 0.0f;
      result.f = add(result.f, mul(sheen_eval(bsdf, wo, wi, &sheen_pdf),
                                   coat_base_weight));
      result.pdf += (coat_base_weight - base_weight) * sheen_pdf;
    }
  }
  return result;
}

extern "C" VkrBakeBsdfEval vkr_bake_bsdf_evaluate(const VkrBakeBsdf *bsdf,
                                                  Vec3 wo, Vec3 wi) {
  if (!bsdf || (bsdf->surface.clearcoat_factor == 0.0f && !sheen_active(bsdf) && bsdf->surface.anisotropy_strength == 0.0f && bsdf->surface.diffuse_transmission_strength == 0.0f && bsdf->surface.subsurface_strength == 0.0f))
    return evaluate_base(bsdf, wo, wi);
  if (!valid_direction(wo) || !valid_direction(wi) || wo.z <= 0.0f)
    return {};
  if (dot3(wo, bsdf->geometric_normal) <= 1.0e-6f)
    return {};
  return evaluate_layered(bsdf, wo, wi);
}

vkr_internal VkrBakeBsdfSample sample_base(const VkrBakeBsdf *bsdf, Vec3 wo,
                                           float32_t u_lobe, float32_t u1,
                                           float32_t u2) {
  VkrBakeBsdfSample result = {};
  result.eta_ratio = 1.0f;
  if (!bsdf || !valid_direction(wo) || wo.z <= 0.0f || !isfinite(u_lobe) ||
      !isfinite(u1) || !isfinite(u2))
    return result;
  const Weights probability = weights(bsdf, wo);
  if (probability.total <= 1e-8f)
    return result;
  // Map the inclusive public endpoint onto the final nonempty interval so a
  // zero-width lobe is never selected when a caller supplies exactly one.
  const float32_t selected =
      fminf(clamp01(u_lobe), 0x1.fffffep-1f) * probability.total;
  const float32_t alpha = alpha_from_roughness(bsdf->surface.roughness);
  if (selected < probability.glass) {
    const float32_t branch =
        probability.glass > 0.0f ? selected / probability.glass : 0.0f;
    const Vec3 wm = sample_visible_normal(wo, alpha, u1, u2);
    Vec3 refracted = {};
    const float32_t eta_ratio = bsdf->eta_transmitted / bsdf->eta_incident;
    const bool8_t can_refract =
        !bsdf->thin_walled && refract_wo(wo, wm, 1.0f / eta_ratio, &refracted);
    const Vec3 f =
        glass_fresnel(bsdf, wo, wm, !bsdf->thin_walled && !can_refract);
    const float32_t pr = clamp01(fmaxf(f.x, fmaxf(f.y, f.z)));
    if (bsdf->thin_walled && branch >= pr) {
      result.wi = mul(wo, -1.0f);
      result.f = mul(mul(one_minus(f), clamp01(rgb(bsdf->surface.base_color))),
                     probability.glass / fmaxf(fabsf(result.wi.z), 1e-6f));
      result.pdf = probability.glass * (1.0f - pr) / probability.total;
      result.transmitted = true_v;
      result.delta = true_v;
      return result;
    }
    if (!bsdf->thin_walled && can_refract && branch >= pr) {
      result.wi = refracted;
      result.eta_ratio = eta_ratio;
      result.transmitted = true_v;
    } else {
      result.wi = reflect_wo(wo, wm);
    }
  } else if (selected < probability.glass + probability.opaque_spec) {
    const Vec3 wm = bsdf->surface.anisotropy_strength > 0.0f
        ? anisotropy_base(bsdf, sample_anisotropic_normal(anisotropy_local(bsdf, wo),
              anisotropy_alpha_t(bsdf, alpha), alpha, u1, u2))
        : sample_visible_normal(wo, alpha, u1, u2);
    result.wi = reflect_wo(wo, wm);
  } else {
    const float32_t radius = sqrtf(clamp01(u1));
    const float32_t phi = 2.0f * k_pi * clamp01(u2);
    result.wi = vec3_new(radius * cosf(phi), radius * sinf(phi),
                         sqrtf(fmaxf(0.0f, 1.0f - u1)));
    if (bsdf->surface.diffuse_transmission_strength > 0.0f &&
        selected >= probability.glass + probability.opaque_spec + probability.opaque_diffuse) {
      result.wi.z = -result.wi.z;
      result.transmitted = true_v;
    }
  }
  /* A sampled rough reflection can fall below the macrosurface.  Its BRDF
     contribution is zero; it must not be evaluated as BTDF while retaining a
     reflection flag, because transport would then cross geometry without
     updating the medium stack. */
  if ((result.transmitted && result.wi.z >= -1.0e-6f) ||
      (!result.transmitted && result.wi.z <= 1.0e-6f))
    return {};
  const VkrBakeBsdfEval value = evaluate_base(bsdf, wo, result.wi);
  result.f = value.f;
  result.pdf = value.pdf;
  if (result.pdf <= 0.0f || !finite3(result.f) || !isfinite(result.pdf))
    return {};
  return result;
}

extern "C" VkrBakeBsdfSample vkr_bake_bsdf_sample(const VkrBakeBsdf *bsdf,
                                                  Vec3 wo, float32_t u_lobe,
                                                  float32_t u1, float32_t u2) {
  if (!bsdf || (bsdf->surface.clearcoat_factor == 0.0f && !sheen_active(bsdf) && bsdf->surface.anisotropy_strength == 0.0f && bsdf->surface.diffuse_transmission_strength == 0.0f && bsdf->surface.subsurface_strength == 0.0f))
    return sample_base(bsdf, wo, u_lobe, u1, u2);

  VkrBakeBsdfSample result = {};
  result.eta_ratio = 1.0f;
  if (!valid_direction(wo) || wo.z <= 0.0f || !isfinite(u_lobe) ||
      !isfinite(u1) || !isfinite(u2))
    return result;
  if (dot3(wo, bsdf->geometric_normal) <= 1.0e-6f)
    return result;
  const float32_t base_weight = base_transmission(bsdf, wo);
  const float32_t coat_weight = 1.0f - coat_transmission(bsdf, wo);
  const float32_t sheen_weight =
      coat_transmission(bsdf, wo) * (1.0f - sheen_transmission(bsdf, wo));
  const float32_t selected = fminf(clamp01(u_lobe), 0x1.fffffep-1f);
  if (selected < coat_weight) {
    const CoatFrame frame = coat_frame(coat_normal_for_view(bsdf, wo));
    const Vec3 coat_wo = coat_to_local(frame, wo);
    if (coat_wo.z <= 0.0f)
      return result;
    const float32_t alpha =
        alpha_from_roughness(bsdf->surface.clearcoat_roughness);
    result.wi = normalize3(coat_from_local(
        frame,
        reflect_wo(coat_wo, sample_visible_normal(coat_wo, alpha, u1, u2))));
    if (dot3(result.wi, bsdf->geometric_normal) <= 1.0e-6f)
      return {};
  } else if (selected < coat_weight + sheen_weight) {
    const Vec3 h = sample_sheen_normal(bsdf->surface.sheen_roughness, u1, u2);
    if (dot3(wo, h) <= 0.0f)
      return {};
    result.wi = reflect_wo(wo, h);
    if (result.wi.z <= 1.0e-6f ||
        dot3(result.wi, bsdf->geometric_normal) <= 1.0e-6f)
      return {};
  } else {
    const float32_t base_lobe =
        base_weight > 0.0f
            ? (selected - coat_weight - sheen_weight) / base_weight
            : 0.0f;
    result = sample_base(bsdf, wo, base_lobe, u1, u2);
    if (!result.pdf)
      return result;
    if ((!result.transmitted &&
         dot3(result.wi, bsdf->geometric_normal) <= 1.0e-6f) ||
        (result.transmitted &&
         dot3(result.wi, bsdf->geometric_normal) >= -1.0e-6f))
      return {};
    if (result.delta) {
      /* Thin transmission is a delta carried by the base branch. Its emitted
         throughput and selection probability both retain the layered base
         transmission; evaluating a continuous coat lobe cannot represent it. */
      result.f = mul(result.f, base_weight);
      result.pdf *= base_weight;
      return result;
    }
  }
  const VkrBakeBsdfEval value = evaluate_layered(bsdf, wo, result.wi);
  result.f = value.f;
  result.pdf = value.pdf;
  if (result.pdf <= 0.0f || !finite3(result.f) || !isfinite(result.pdf))
    return {};
  return result;
}

extern "C" Vec3 vkr_bake_bsdf_subsurface_receiver(const VkrBakeBsdf *bsdf, Vec3 wo) {
  const Vec3 f0 = opaque_f0(bsdf);
  const Vec2 split_sum = base_dfg(bsdf, wo);
  const Vec3 scale = add(vec3_new(1, 1, 1),
      mul(f0, 1.0f / fmaxf(split_sum.x + split_sum.y, .25f) - 1.0f));
  const float32_t f90 = clamp01(fmaxf(f0.x, fmaxf(f0.y, f0.z)) * 25.0f);
  const Vec3 reflectance = clamp01(mul(
      add(mul(f0, split_sum.x), vec3_new(f90 * split_sum.y, f90 * split_sum.y, f90 * split_sum.y)), scale));
  const Vec3 amplitude = vkr_bake_subsurface_amplitude(bsdf->surface);
  return mul(mul(one_minus(reflectance), amplitude), base_transmission(bsdf, wo));
}

extern "C" Vec3 vkr_bake_subsurface_amplitude(VkrBakeMaterialSample material) {
  const Vec3 allocation = mul(clamp01(rgb(material.base_color)),
      1.0f - clamp01(material.metallic));
  return vec3_new(sqrtf(allocation.x), sqrtf(allocation.y), sqrtf(allocation.z));
}

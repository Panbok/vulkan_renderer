#include "vkr_subsurface.h"

#include <math.h>

#define VKR_SUBSURFACE_PI 3.14159265358979323846264338327950288

bool8_t vkr_subsurface_profile_valid(VkrSubsurfaceProfile profile) {
  const Vec3 distance = profile.diffusion_distance;
  return isfinite(distance.r) && distance.r > 0.0f && isfinite(distance.g) &&
         distance.g > 0.0f && isfinite(distance.b) && distance.b > 0.0f;
}

/* Christensen and Burley, Approximate Reflectance Profiles (2015):
 * R(r,d) = (exp(-r/d) + exp(-r/(3d))) / (8 pi d r).
 * Multiplying by the polar Jacobian gives a normalized radial mixture with
 * mass 1/4 on Exp(d) and 3/4 on Exp(3d). */
float64_t vkr_subsurface_profile_evaluate(float64_t radius,
                                          float64_t diffusion_distance) {
  return vkr_subsurface_profile_radial_pdf(radius, diffusion_distance) /
         (2.0 * VKR_SUBSURFACE_PI * radius);
}

float64_t vkr_subsurface_profile_radial_pdf(float64_t radius,
                                            float64_t diffusion_distance) {
  const float64_t x = radius / diffusion_distance;
  return (exp(-x) + exp(-x / 3.0)) / (4.0 * diffusion_distance);
}

float64_t vkr_subsurface_profile_radial_cdf(float64_t radius,
                                            float64_t diffusion_distance) {
  const float64_t x = radius / diffusion_distance;
  return -0.25 * expm1(-x) - 0.75 * expm1(-x / 3.0);
}

float64_t vkr_subsurface_profile_sample_radius(float64_t diffusion_distance,
                                               float64_t u) {
  float64_t x;
  if (u < 0.125) {
    /* Near zero, the cubic inversion loses precision through log(1-epsilon).
     * Stable CDF/PDF Newton steps retain the small positive radial mass. */
    x = 2.0 * u;
    for (uint32_t iteration = 0u; iteration < 4u; ++iteration) {
      const float64_t cdf = -0.25 * expm1(-x) - 0.75 * expm1(-x / 3.0);
      const float64_t pdf = (exp(-x) + exp(-x / 3.0)) * 0.25;
      x -= (cdf - u) / pdf;
    }
  } else {
    /* t=exp(-x/3) turns the CDF inversion into t^3+3t=4(1-u). */
    const float64_t t = 2.0 * sinh(asinh(2.0 * (1.0 - u)) / 3.0);
    x = -3.0 * log(t);
  }
  return diffusion_distance * x;
}

bool8_t vkr_subsurface_table_build(const VkrSubsurfaceProfile *profiles,
                                   uint32_t profile_count,
                                   VkrSubsurfaceTable *out_table) {
  if (!out_table || profile_count > VKR_SUBSURFACE_PROFILE_COUNT ||
      (profile_count != 0u && !profiles))
    return false_v;
  for (uint32_t profile = 0u; profile < profile_count; ++profile)
    if (!vkr_subsurface_profile_valid(profiles[profile]))
      return false_v;

  MemZero(out_table, sizeof(*out_table));
  /* Each channel owns strata even when RGB distances differ by many orders of
   * magnitude. Importance densities use these actual channel probabilities.
   * Antipodal pairs give zero first spatial moment without extra color taps. */
  const uint32_t pair_counts[3] = {6u, 5u, 5u};
  for (uint32_t profile = 0u; profile < profile_count; ++profile) {
    const Vec3 authored = profiles[profile].diffusion_distance;
    const float64_t distance[3] = {authored.r, authored.g, authored.b};
    const float64_t largest = fmax(distance[0], fmax(distance[1], distance[2]));
    float64_t weights[VKR_SUBSURFACE_TAP_COUNT][3];
    float64_t weight_sums[3] = {0.0, 0.0, 0.0};
    float64_t maximum_radius = 0.0;
    uint32_t pair = 0u;
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
      for (uint32_t stratum = 0u; stratum < pair_counts[channel]; ++stratum) {
        const float64_t u = ((float64_t)stratum + 0.5) / pair_counts[channel];
        const float64_t radius =
            vkr_subsurface_profile_sample_radius(distance[channel], u);
        const float64_t relative_radius = radius / largest;
        maximum_radius = fmax(maximum_radius, relative_radius);
        const float64_t angle = (float64_t)pair * 2.39996322972865332;
        const float32_t offset_x = (float32_t)(relative_radius * cos(angle));
        const float32_t offset_y = (float32_t)(relative_radius * sin(angle));
        float64_t pdf[3];
        float64_t mixture_pdf = 0.0;
        for (uint32_t component = 0u; component < 3u; ++component) {
          pdf[component] =
              vkr_subsurface_profile_radial_pdf(radius, distance[component]);
          mixture_pdf +=
              pdf[component] * ((float64_t)pair_counts[component] / 16.0);
        }
        for (uint32_t side = 0u; side < 2u; ++side) {
          const uint32_t tap = 2u * pair + side;
          const float32_t sign = side == 0u ? 1.0f : -1.0f;
          out_table->texels[profile][2u * tap].x = offset_x * sign;
          out_table->texels[profile][2u * tap].y = offset_y * sign;
          for (uint32_t component = 0u; component < 3u; ++component) {
            weights[tap][component] = pdf[component] / mixture_pdf;
            weight_sums[component] += weights[tap][component];
          }
        }
        ++pair;
      }
    }
    for (uint32_t tap = 0u; tap < VKR_SUBSURFACE_TAP_COUNT; ++tap) {
      out_table->texels[profile][2u * tap].z =
          (float32_t)(weights[tap][0] / weight_sums[0]);
      out_table->texels[profile][2u * tap].w =
          (float32_t)(weights[tap][1] / weight_sums[1]);
      out_table->texels[profile][2u * tap + 1u].x =
          (float32_t)(weights[tap][2] / weight_sums[2]);
    }
    out_table->texels[profile][64] =
        vec4_new(authored.r, authored.g, authored.b,
                 nextafterf((float32_t)maximum_radius, INFINITY));
  }
  return true_v;
}

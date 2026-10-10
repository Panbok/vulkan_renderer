#include "vkr_bake_sh.h"

#include <cmath>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double k0 = 0.28209479177387814;
constexpr double k1 = 0.48860251190291992;

double area_corner(double s, double t) {
  return std::atan2(s * t, std::sqrt(s * s + t * t + 1.0));
}

double band_window(unsigned band, double strength) {
  if (!band || strength == 0.0)
    return 1.0;
  const double x = pi * band / 3.0;
  return std::pow(std::sin(x) / x, strength);
}
} // namespace

Vec3 vkr_bake_cube_direction(uint32_t face, float32_t s, float32_t t) {
  Vec3 direction;
  switch (face) {
  case 0:
    direction = {1.0f, -t, -s};
    break;
  case 1:
    direction = {-1.0f, -t, s};
    break;
  case 2:
    direction = {s, 1.0f, t};
    break;
  case 3:
    direction = {s, -1.0f, -t};
    break;
  case 4:
    direction = {s, -t, 1.0f};
    break;
  default:
    direction = {-s, -t, -1.0f};
    break;
  }
  return vec3_normalize(direction);
}

bool8_t vkr_bake_sh_project_l1(const Vec3 *radiance, uint32_t face_size,
                               float32_t deringing, float32_t out[3][4]) {
  if (!radiance || !out || !face_size ||
      face_size > VKR_SH_PROJECTION_MAX_FACE_SIZE ||
      !std::isfinite(deringing) || deringing < 0.0f)
    return false_v;
  const uint32_t texel_count = 6u * face_size * face_size;
  for (uint32_t i = 0; i < texel_count; ++i) {
    if (!std::isfinite(radiance[i].x) || !std::isfinite(radiance[i].y) ||
        !std::isfinite(radiance[i].z) || radiance[i].x < 0.0f ||
        radiance[i].y < 0.0f || radiance[i].z < 0.0f)
      return false_v;
  }
  double coefficients[4][3] = {};
  double total_angle = 0.0;
  for (uint32_t face = 0; face < 6u; ++face) {
    for (uint32_t y = 0; y < face_size; ++y) {
      const double t0 = 2.0 * y / face_size - 1.0;
      const double t1 = 2.0 * (y + 1u) / face_size - 1.0;
      for (uint32_t x = 0; x < face_size; ++x) {
        const double s0 = 2.0 * x / face_size - 1.0;
        const double s1 = 2.0 * (x + 1u) / face_size - 1.0;
        const double angle = area_corner(s0, t0) - area_corner(s0, t1) -
                             area_corner(s1, t0) + area_corner(s1, t1);
        const Vec3 n = vkr_bake_cube_direction(face, (float)((s0 + s1) * .5),
                                               (float)((t0 + t1) * .5));
        const double basis[4] = {k0, k1 * n.y, k1 * n.z, k1 * n.x};
        const Vec3 color = radiance[(face * face_size + y) * face_size + x];
        const double channels[3] = {color.x, color.y, color.z};
        for (uint32_t c = 0; c < 4u; ++c)
          for (uint32_t channel = 0; channel < 3u; ++channel)
            coefficients[c][channel] += channels[channel] * basis[c] * angle;
        total_angle += angle;
      }
    }
  }
  // Match ADR-038's E/pi transfer before packing.
  const double normalization = 4.0 * pi / total_angle;
  for (uint32_t i = 0; i < 4u; ++i) {
    const unsigned band = i == 0u ? 0u : 1u;
    const double transfer = band == 0u ? 1.0 : 2.0 / 3.0;
    const double weight =
        normalization * transfer * band_window(band, deringing);
    for (uint32_t channel = 0; channel < 3u; ++channel)
      coefficients[i][channel] *= weight;
  }
  for (uint32_t c = 0; c < 3u; ++c) {
    out[c][0] = (float)(k1 * coefficients[3][c]);
    out[c][1] = (float)(k1 * coefficients[1][c]);
    out[c][2] = (float)(k1 * coefficients[2][c]);
    out[c][3] = (float)(k0 * coefficients[0][c]);
  }
  return true_v;
}

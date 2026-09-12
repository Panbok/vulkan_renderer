#include "vkr_atomic_file.h"
#include "vkr_sheen_lut.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double k_pi = 3.1415926535897932384626433832795;
constexpr uint32_t k_energy_axis_samples = 64u;
constexpr uint32_t k_peak_no_v_samples = 257u;
/* Peak integration is deliberately conservative: the normalized lobe retains
   room for the independent higher-sample furnace audit and table filtering. */
constexpr double k_energy_headroom = 0.85;
/* Decoded off-grid furnace maxima: .003107 relative and .002670 absolute. */
constexpr double k_allocation_relative_reserve = 0.004;
constexpr double k_allocation_absolute_reserve = 0.003;
constexpr double k_min_no_v = VKR_SHEEN_MIN_NOV;
constexpr double k_eps = 1.0e-12;

struct Vec3 { double x, y, z; };

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, double b) { return {a.x * b, a.y * b, a.z * b}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
double length2(Vec3 a) { return dot(a, a); }
Vec3 normalize(Vec3 a) {
  const double length_squared = length2(a);
  return length_squared > k_eps ? mul(a, 1.0 / std::sqrt(length_squared))
                                : Vec3{0.0, 0.0, 0.0};
}
double saturate(double value) { return std::clamp(value, 0.0, 1.0); }

double roughness_from_unit(double unit) {
  return VKR_SHEEN_MIN_ROUGHNESS +
         (1.0 - VKR_SHEEN_MIN_ROUGHNESS) * saturate(unit);
}

struct CharlieCoefficients { double a, b, c, d, e; };

CharlieCoefficients charlie_coefficients(double alpha) {
  const double interpolation = (1.0 - alpha) * (1.0 - alpha);
  return {
      21.5473 + (25.3245 - 21.5473) * interpolation,
      3.82987 + (3.32435 - 3.82987) * interpolation,
      0.19823 + (0.16801 - 0.19823) * interpolation,
      -1.97760 + (-1.27393 + 1.97760) * interpolation,
      -4.32054 + (-4.85967 + 4.32054) * interpolation,
  };
}

double charlie_lambda(double cosine, double alpha) {
  const double x = saturate(std::abs(cosine));
  const CharlieCoefficients p = charlie_coefficients(alpha);
  const auto l = [&p](double value) {
    return p.a / (1.0 + p.b * std::pow(std::max(value, 1.0e-8), p.c)) +
           p.d * value + p.e;
  };
  return x < 0.5 ? std::exp(l(x))
                 : std::exp(2.0 * l(0.5) - l(1.0 - x));
}

double charlie_d(double no_h, double roughness) {
  const double alpha = roughness * roughness;
  const double inv_alpha = 1.0 / alpha;
  const double sin2 = std::max(1.0 - no_h * no_h, 0.0);
  return (2.0 + inv_alpha) * std::pow(sin2, 0.5 * inv_alpha) /
         (2.0 * k_pi);
}

double charlie_visibility(double no_v, double no_l, double roughness);

double charlie_brdf(double no_v, Vec3 wi, double roughness) {
  if (no_v < 0.0 || wi.z < 0.0)
    return 0.0;
  /* Keep the actual geometric directions for the half vector; only the
     fitted visibility domain is regularized at grazing incidence. */
  const Vec3 wo = {std::sqrt(std::max(0.0, 1.0 - no_v * no_v)), 0.0, no_v};
  const Vec3 h = normalize(add(wo, wi));
  if (h.z <= 0.0)
    return 0.0;
  return charlie_d(h.z, roughness) *
         charlie_visibility(no_v, wi.z, roughness);
}

/* Integrating in actual h.z removes the NDF-sampling endpoint Jacobian.
   Charlie D is smooth in this domain after the fitted cosine regularization. */
struct GaussRule { std::array<double, k_energy_axis_samples> x, w; };

const GaussRule &gauss_rule() {
  static const GaussRule rule = [] {
    GaussRule result = {};
    constexpr uint32_t half = (k_energy_axis_samples + 1u) / 2u;
    for (uint32_t i = 0; i < half; ++i) {
      double z = std::cos(k_pi * (static_cast<double>(i) + 0.75) /
                          (k_energy_axis_samples + 0.5));
      for (;;) {
        double p0 = 1.0, p1 = 0.0;
        for (uint32_t n = 1; n <= k_energy_axis_samples; ++n) {
          const double p2 = p1;
          p1 = p0;
          p0 = ((2.0 * n - 1.0) * z * p1 - (n - 1.0) * p2) / n;
        }
        const double derivative = k_energy_axis_samples * (z * p0 - p1) /
                                  (z * z - 1.0);
        const double next = z - p0 / derivative;
        if (std::abs(next - z) <= 1.0e-15) {
          z = next;
          break;
        }
        z = next;
      }
      double p0 = 1.0, p1 = 0.0;
      for (uint32_t n = 1; n <= k_energy_axis_samples; ++n) {
        const double p2 = p1;
        p1 = p0;
        p0 = ((2.0 * n - 1.0) * z * p1 - (n - 1.0) * p2) / n;
      }
      const double derivative = k_energy_axis_samples * (z * p0 - p1) /
                                (z * z - 1.0);
      const double weight = 1.0 / ((1.0 - z * z) * derivative * derivative);
      result.x[i] = 0.5 * (1.0 - z);
      result.x[k_energy_axis_samples - 1u - i] = 0.5 * (1.0 + z);
      result.w[i] = result.w[k_energy_axis_samples - 1u - i] = weight;
    }
    return result;
  }();
  return rule;
}

double charlie_visibility(double no_v, double no_l, double roughness) {
  if (no_l <= 0.0)
    return 0.0;
  const double visible_v = std::max(no_v, k_min_no_v);
  const double visible_l = std::max(no_l, k_min_no_v);
  const double alpha = roughness * roughness;
  return 1.0 / ((1.0 + charlie_lambda(visible_v, alpha) +
                 charlie_lambda(visible_l, alpha)) *
                (4.0 * visible_v * visible_l));
}

double directional_energy(double no_v, double roughness) {
  if (no_v < 0.0 || no_v > 1.0)
    return 0.0;
  const Vec3 wo = {std::sqrt(std::max(0.0, 1.0 - no_v * no_v)), 0.0, no_v};
  const GaussRule &rule = gauss_rule();
  double value = 0.0;
  for (uint32_t h_index = 0; h_index < k_energy_axis_samples; ++h_index) {
    const double no_h = rule.x[h_index];
    const double sin_h = std::sqrt(std::max(0.0, 1.0 - no_h * no_h));
    const double d = charlie_d(no_h, roughness);
    for (uint32_t phi_index = 0; phi_index < k_energy_axis_samples; ++phi_index) {
      const double phi = 2.0 * k_pi *
                         (static_cast<double>(phi_index) + 0.5) /
                         k_energy_axis_samples;
      const Vec3 h = {sin_h * std::cos(phi), sin_h * std::sin(phi), no_h};
      const double wo_h = dot(wo, h);
      if (wo_h <= k_eps)
        continue;
      const Vec3 wi = sub(mul(h, 2.0 * wo_h), wo);
      if (wi.z <= 0.0)
        continue;
      value += rule.w[h_index] * d * charlie_visibility(no_v, wi.z, roughness) *
               wi.z * (4.0 * wo_h);
    }
  }
  return value * (2.0 * k_pi / k_energy_axis_samples);
}

double peak_energy(double roughness) {
  double peak = 0.0;
  for (uint32_t i = 0; i < k_peak_no_v_samples; ++i) {
    const double no_v = static_cast<double>(i) / (k_peak_no_v_samples - 1u);
    peak = std::max(peak, directional_energy(no_v, roughness));
  }
  constexpr std::array<double, 8> grazing = {
      0.0, 1.0e-6, 1.0e-5, 1.0e-4, 1.0e-3, 1.0e-2, 2.0e-2, 4.0e-2};
  for (double no_v : grazing)
    peak = std::max(peak, directional_energy(no_v, roughness));
  return peak;
}

double interpolate_scale(const std::array<double, VKR_SHEEN_LTC_LUT_SIZE> &scales,
                         double roughness) {
  const double unit = (std::clamp(roughness, static_cast<double>(VKR_SHEEN_MIN_ROUGHNESS),
                                  1.0) - VKR_SHEEN_MIN_ROUGHNESS) /
                      (1.0 - VKR_SHEEN_MIN_ROUGHNESS);
  const double coordinate = unit * (VKR_SHEEN_LTC_LUT_SIZE - 1u);
  const uint32_t first = static_cast<uint32_t>(std::floor(coordinate));
  const uint32_t second = std::min(first + 1u, VKR_SHEEN_LTC_LUT_SIZE - 1u);
  return scales[first] + (scales[second] - scales[first]) * (coordinate - first);
}

struct JointSample {
  Vec3 wi;
  double target;
  double weight;
};

struct JointRectangle {
  std::array<Vec3, 4> vertices;
  double target;
};

double charlie_rectangle_integral(const std::array<Vec3, 4> &vertices,
                                  double no_v, double roughness) {
  constexpr uint32_t k_side = 20u;
  const Vec3 edge_x = mul(sub(vertices[3], vertices[0]), 1.0 / k_side);
  const Vec3 edge_y = mul(sub(vertices[1], vertices[0]), 1.0 / k_side);
  const double area = std::sqrt(length2(cross(edge_x, edge_y)));
  double value = 0.0;
  for (uint32_t y = 0; y < k_side; ++y) for (uint32_t x = 0; x < k_side; ++x) {
    const Vec3 point = add(vertices[0], add(mul(edge_x, x + 0.5),
                                             mul(edge_y, y + 0.5)));
    const double distance_squared = length2(point);
    const double distance = std::sqrt(distance_squared);
    if (point.z <= 0.0 || distance <= k_eps) continue;
    const Vec3 wi = mul(point, 1.0 / distance);
    value += wi.z * charlie_brdf(no_v, wi, roughness) *
             point.z * area / (distance_squared * distance);
  }
  return value;
}

std::array<JointRectangle, 4> joint_rectangles(double no_v, double roughness, double target_energy) {
  struct Definition { Vec3 center; double half_width, half_height; };
  constexpr std::array<Definition, 4> definitions = {
      Definition{{0.0, 0.0, 1.5}, 0.25, 0.25},
      Definition{{0.55, 0.15, 1.2}, 0.5, 0.2},
      Definition{{-0.7, 0.4, 2.4}, 0.9, 0.45},
      Definition{{0.1, -0.25, 0.55}, 0.18, 0.35},
  };
  std::array<JointRectangle, 4> result = {};
  for (uint32_t i = 0; i < definitions.size(); ++i) {
    const Definition &d = definitions[i];
    result[i].vertices = {Vec3{d.center.x - d.half_width, d.center.y - d.half_height, d.center.z},
                          Vec3{d.center.x - d.half_width, d.center.y + d.half_height, d.center.z},
                          Vec3{d.center.x + d.half_width, d.center.y + d.half_height, d.center.z},
                          Vec3{d.center.x + d.half_width, d.center.y - d.half_height, d.center.z}};
    result[i].target = charlie_rectangle_integral(result[i].vertices, no_v, roughness) /
                       std::max(target_energy, k_eps);
  }
  return result;
}

constexpr double k_full_beta_limit = 3.10;
/* The wider encoded domain preserves positive support under filtering.
   Fitting uses a narrower cap to avoid nearly unsupported lobe solutions. */
constexpr double k_rectfit_beta_limit = 1.40;
struct FullFit { double log_s, log_k, h, beta, fraction; };

std::array<double, 4> full_matrix(const FullFit &fit) {
  const double s = std::exp2(std::clamp(fit.log_s, -6.0, 6.0));
  const double k = std::exp2(std::clamp(fit.log_k, -6.0, 6.0));
  const double beta = std::clamp(fit.beta, -k_full_beta_limit, k_full_beta_limit);
  const double x = s * std::cos(beta);
  const double z = s * std::sin(beta);
  return {x, z, fit.h * x - k * std::sin(beta),
          fit.h * z + k * std::cos(beta)};
}

double full_mass(const FullFit &fit) {
  return 0.5 * (1.0 + std::cos(std::clamp(fit.beta, -k_full_beta_limit,
                                            k_full_beta_limit)));
}

/* The transform depends only on the fit, so the per-sample loops below hoist
   it out rather than recomputing six transcendentals for every direction. */
double full_density_transformed(const std::array<double, 4> &a,
                                Vec3 direction) {
  const Vec3 transformed = {a[0] * direction.x + a[2] * direction.z, direction.y,
                            a[1] * direction.x + a[3] * direction.z};
  const double length = std::sqrt(length2(transformed));
  const double determinant = a[0] * a[3] - a[1] * a[2];
  if (length <= k_eps || determinant <= k_eps) return 0.0;
  const Vec3 local = mul(transformed, 1.0 / length);
  return local.z > 0.0 ? local.z * determinant /
      (k_pi * length * length * length) : 0.0;
}

double full_density(const FullFit &fit, Vec3 direction) {
  return full_density_transformed(full_matrix(fit), direction);
}

double full_edge_integral(Vec3 a, Vec3 b) {
  a = normalize(a);
  b = normalize(b);
  const double cosine = std::clamp(dot(a, b), -1.0, 1.0);
  return (a.x * b.y - a.y * b.x) * std::acos(cosine) /
      std::sqrt(std::max(1.0 - cosine * cosine, 1.0e-12));
}

double full_rectangle_component(const FullFit &fit,
                                const std::array<Vec3, 4> &vertices) {
  const std::array<double, 4> matrix = full_matrix(fit);
  std::array<Vec3, 5> clipped = {};
  uint32_t count = 0u;
  std::array<Vec3, 4> transformed = {};
  for (uint32_t i = 0; i < 4u; ++i) {
    const Vec3 v = vertices[i];
    transformed[i] = {matrix[0] * v.x + matrix[2] * v.z, v.y,
                      matrix[1] * v.x + matrix[3] * v.z};
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    const Vec3 a = transformed[i];
    const Vec3 b = transformed[(i + 1u) & 3u];
    const bool inside_a = a.z > 0.0;
    const bool inside_b = b.z > 0.0;
    if (inside_a) clipped[count++] = a;
    if (inside_a != inside_b) {
      const double denominator = a.z - b.z;
      if (std::abs(denominator) > k_eps)
        clipped[count++] = add(a, mul(sub(b, a), a.z / denominator));
    }
  }
  if (count < 3u) return 0.0;
  double sum = 0.0;
  for (uint32_t i = 0; i < count; ++i)
    sum += full_edge_integral(clipped[i], clipped[(i + 1u) % count]);
  return std::max(0.0, -sum / (2.0 * k_pi));
}

struct RectfitObjective {
  const std::vector<JointSample> *samples;
  const std::array<JointRectangle, 4> *rectangles;

  std::array<FullFit, 2> evaluate(const std::array<double, 8> &parameters,
                                  std::vector<double> *residuals) const {
    std::array<FullFit, 2> fit = {
        FullFit{parameters[0], parameters[1], std::clamp(parameters[2], -6.0, 6.0),
                std::clamp(parameters[3], -k_rectfit_beta_limit, k_rectfit_beta_limit), 0.0},
        FullFit{parameters[4], parameters[5], std::clamp(parameters[6], -6.0, 6.0),
                std::clamp(parameters[7], -k_rectfit_beta_limit, k_rectfit_beta_limit), 0.0}};
    const double first_mass = full_mass(fit[0]);
    const double second_mass = full_mass(fit[1]);
    const std::array<double, 4> first_matrix = full_matrix(fit[0]);
    const std::array<double, 4> second_matrix = full_matrix(fit[1]);
    const size_t point_count = samples->size();
    std::vector<double> first(point_count + rectangles->size());
    std::vector<double> second(point_count + rectangles->size());
    std::vector<double> target(point_count + rectangles->size());
    std::vector<double> weight(point_count + rectangles->size());
    for (size_t i = 0; i < point_count; ++i) {
      first[i] =
          full_density_transformed(first_matrix, (*samples)[i].wi) / first_mass;
      second[i] = full_density_transformed(second_matrix, (*samples)[i].wi) /
                  second_mass;
      target[i] = (*samples)[i].target;
      /* Physical solid-angle L2 term. */
      weight[i] = (*samples)[i].weight;
    }
    for (size_t i = 0; i < rectangles->size(); ++i) {
      const size_t index = point_count + i;
      first[index] = full_rectangle_component(fit[0], (*rectangles)[i].vertices) /
          first_mass;
      second[index] = full_rectangle_component(fit[1], (*rectangles)[i].vertices) /
          second_mass;
      target[index] = (*rectangles)[i].target;
      /* Polygon responses are held in normalized directional-energy units. */
      weight[index] = 1.0 / std::pow(std::max(target[index], 0.02), 2.0);
    }
    double denominator = 0.0;
    double numerator = 0.0;
    for (size_t i = 0; i < target.size(); ++i) {
      const double delta = first[i] - second[i];
      denominator += weight[i] * delta * delta;
      numerator += weight[i] * (target[i] - second[i]) * delta;
    }
    fit[0].fraction = denominator > k_eps ?
        std::clamp(numerator / denominator, 0.0, 1.0) : 0.5;
    fit[1].fraction = 1.0 - fit[0].fraction;
    residuals->resize(target.size());
    for (size_t i = 0; i < target.size(); ++i)
      (*residuals)[i] = std::sqrt(weight[i]) *
          (fit[0].fraction * first[i] + fit[1].fraction * second[i] - target[i]);
    return fit;
  }
};

double rectfit_squared_norm(const std::vector<double> &residuals) {
  double result = 0.0;
  for (double residual : residuals) result += residual * residual;
  return result;
}

bool rectfit_solve_linear(double matrix[8][9], std::array<double, 8> *out) {
  for (uint32_t column = 0; column < 8u; ++column) {
    uint32_t pivot = column;
    for (uint32_t row = column + 1u; row < 8u; ++row)
      if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column]))
        pivot = row;
    if (std::abs(matrix[pivot][column]) < 1.0e-25) return false;
    for (uint32_t j = column; j <= 8u; ++j)
      std::swap(matrix[column][j], matrix[pivot][j]);
    const double scale = matrix[column][column];
    for (uint32_t j = column; j <= 8u; ++j) matrix[column][j] /= scale;
    for (uint32_t row = 0; row < 8u; ++row) {
      if (row == column) continue;
      const double factor = matrix[row][column];
      for (uint32_t j = column; j <= 8u; ++j)
        matrix[row][j] -= factor * matrix[column][j];
    }
  }
  for (uint32_t i = 0; i < 8u; ++i) (*out)[i] = matrix[i][8u];
  return true;
}

std::array<double, 8> rectfit_optimize(const RectfitObjective &objective,
                                       std::array<double, 8> parameters) {
  std::vector<double> residuals, trial_residuals;
  objective.evaluate(parameters, &residuals);
  double cost = rectfit_squared_norm(residuals);
  double damping = 0.01;
  for (uint32_t iteration = 0; iteration < 72u; ++iteration) {
    std::array<std::vector<double>, 8> jacobian;
    for (uint32_t column = 0; column < 8u; ++column) {
      std::array<double, 8> displaced = parameters;
      const double step = 1.0e-4 * (1.0 + std::abs(parameters[column]));
      displaced[column] += step;
      objective.evaluate(displaced, &trial_residuals);
      jacobian[column].resize(residuals.size());
      for (size_t i = 0; i < residuals.size(); ++i)
        jacobian[column][i] = (trial_residuals[i] - residuals[i]) / step;
    }
    double hessian[8][8] = {};
    double gradient[8] = {};
    for (uint32_t row = 0; row < 8u; ++row) {
      for (size_t i = 0; i < residuals.size(); ++i)
        gradient[row] += jacobian[row][i] * residuals[i];
      for (uint32_t column = 0; column <= row; ++column) {
        for (size_t i = 0; i < residuals.size(); ++i)
          hessian[row][column] += jacobian[row][i] * jacobian[column][i];
        hessian[column][row] = hessian[row][column];
      }
    }
    bool accepted = false;
    for (uint32_t trial = 0; trial < 12u; ++trial) {
      double system[8][9] = {};
      for (uint32_t row = 0; row < 8u; ++row) {
        for (uint32_t column = 0; column < 8u; ++column)
          system[row][column] = hessian[row][column];
        system[row][row] += damping * std::max(hessian[row][row], 1.0e-8);
        system[row][8u] = -gradient[row];
      }
      std::array<double, 8> delta = {};
      if (!rectfit_solve_linear(system, &delta)) break;
      std::array<double, 8> candidate = parameters;
      double step_squared = 0.0;
      for (uint32_t i = 0; i < 8u; ++i) {
        candidate[i] += delta[i];
        candidate[i] = std::clamp(candidate[i],
            (i & 3u) == 3u ? -k_rectfit_beta_limit : -6.0,
            (i & 3u) == 3u ? k_rectfit_beta_limit : 6.0);
        step_squared += delta[i] * delta[i];
      }
      objective.evaluate(candidate, &trial_residuals);
      const double candidate_cost = rectfit_squared_norm(trial_residuals);
      if (candidate_cost < cost) {
        const double improvement = cost - candidate_cost;
        parameters = candidate;
        residuals.swap(trial_residuals);
        cost = candidate_cost;
        damping = std::max(1.0e-9, damping * 0.3);
        accepted = true;
        if (improvement < 1.0e-9 * std::max(cost, 1.0e-6) &&
            step_squared < 1.0e-6) return parameters;
        break;
      }
      damping *= 10.0;
    }
    if (!accepted) break;
  }
  return parameters;
}

std::array<FullFit, 2> rectfit_normal_view(const RectfitObjective &objective,
                                           std::vector<double> *residuals) {
  const auto evaluate = [&](double first_log_k, double second_log_k,
                            std::vector<double> *out) {
    const std::array<double, 8> parameters = {0.0, first_log_k, 0.0, 0.0,
                                                0.0, second_log_k, 0.0, 0.0};
    return objective.evaluate(parameters, out);
  };
  double first = -0.5;
  double second = 0.75;
  double best = std::numeric_limits<double>::infinity();
  for (double a = -4.0; a <= 4.0; a += 0.5) {
    for (double b = -4.0; b <= 4.0; b += 0.5) {
      std::vector<double> candidate;
      evaluate(a, b, &candidate);
      const double cost = rectfit_squared_norm(candidate);
      if (cost < best) { best = cost; first = a; second = b; }
    }
  }
  for (double step = 0.25; step >= 1.0e-4; step *= 0.5) {
    bool improved = true;
    while (improved) {
      improved = false;
      for (const std::array<double, 2> offset :
           {std::array<double, 2>{step, 0.0}, {-step, 0.0},
            {0.0, step}, {0.0, -step}}) {
        std::vector<double> candidate;
        evaluate(first + offset[0], second + offset[1], &candidate);
        const double cost = rectfit_squared_norm(candidate);
        if (cost < best) {
          best = cost;
          first += offset[0];
          second += offset[1];
          improved = true;
        }
      }
    }
  }
  std::array<FullFit, 2> result = evaluate(first, second, residuals);
  /* The endpoint is isotropic; use an ordered radial pair as its stable
     continuation identity for the first off-axis view row. */
  if (result[1].log_k < result[0].log_k) std::swap(result[0], result[1]);
  return result;
}

uint16_t half(double value) {
  if (!std::isfinite(value) || value < 0.0 || value > 65504.0) {
    std::cerr << "non-finite or out-of-range half value\n";
    std::exit(1);
  }
  const float input = static_cast<float>(value);
  uint32_t bits = 0u;
  std::memcpy(&bits, &input, sizeof(bits));
  const uint32_t sign = bits >> 31u;
  int32_t exponent = static_cast<int32_t>((bits >> 23u) & 0xffu) - 127;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent < -24) return 0u;
  if (exponent < -14) {
    mantissa |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(-exponent - 14);
    const uint32_t rounded = (mantissa + (1u << (shift + 12u))) >> (shift + 13u);
    if (rounded == 0u)
      return 0u;
    return static_cast<uint16_t>((sign << 15u) | rounded);
  }
  uint32_t rounded = mantissa + 0x1000u;
  if (rounded & 0x800000u) { rounded = 0u; ++exponent; }
  if (exponent > 15) return static_cast<uint16_t>((sign << 15u) | 0x7bffu);
  const uint32_t magnitude =
      (static_cast<uint32_t>(exponent + 15) << 10u) | (rounded >> 13u);
  /* A magnitude that rounds to zero carries no sign, and emitting -0 for a
     tiny negative fit residual makes the table depend on which side of zero
     the optimizer happened to land. Canonicalize both zeroes to +0 so the
     encoding is reproducible across hosts. */
  if (magnitude == 0u)
    return 0u;
  return static_cast<uint16_t>((sign << 15u) | magnitude);
}

void emit_values(std::ostringstream *output, const std::vector<uint16_t> &values) {
  for (uint32_t i = 0; i < values.size(); ++i) {
    *output << "0x" << std::hex << std::setw(4) << std::setfill('0')
            << values[i] << ',';
    *output << ((i & 7u) == 7u ? '\n' : ' ');
  }
  *output << std::dec;
}

}  // namespace

/* Runs body(row) for every row, one worker per hardware thread. Callers keep
   every cross-row reduction, validation and encoding in their own serial
   pass, so a table never depends on how many workers ran. */
template <typename Body> void vkr_parallel_rows(uint32_t rows, Body body) {
  std::atomic<uint32_t> next_row{0u};
  const uint32_t worker_count =
      std::min(rows, std::max(1u, std::thread::hardware_concurrency()));
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (uint32_t worker = 0; worker < worker_count; ++worker)
    workers.emplace_back([&]() {
      for (;;) {
        const uint32_t row = next_row.fetch_add(1u);
        if (row >= rows)
          break;
        body(row);
      }
    });
  for (std::thread &worker : workers)
    worker.join();
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: vkr_sheen_cooker <output.inc>\n";
    return 2;
  }

  std::array<double, VKR_SHEEN_LTC_LUT_SIZE> scales = {};
  double raw_peak = 0.0;
  /* Each roughness peak is an independent quadrature; only the serial pass
     below folds them into the shared scale table, so the result does not
     depend on the worker count. */
  std::array<double, VKR_SHEEN_LTC_LUT_SIZE> peaks = {};
  vkr_parallel_rows(VKR_SHEEN_LTC_LUT_SIZE, [&](uint32_t y) {
    peaks[y] = peak_energy(roughness_from_unit(
        static_cast<double>(y) / (VKR_SHEEN_LTC_LUT_SIZE - 1u)));
  });
  for (uint32_t y = 0; y < VKR_SHEEN_LTC_LUT_SIZE; ++y) {
    const double roughness = roughness_from_unit(
        static_cast<double>(y) / (VKR_SHEEN_LTC_LUT_SIZE - 1u));
    const double peak = peaks[y];
    if (!std::isfinite(peak) || peak <= 0.0) {
      std::cerr << "invalid Charlie peak at roughness " << roughness << '\n';
      return 1;
    }
    scales[y] = std::min(1.0, k_energy_headroom / peak);
    raw_peak = std::max(raw_peak, peak);
  }

  std::vector<uint16_t> energy;
  energy.reserve(VKR_SHEEN_ENERGY_LUT_TEXEL_COUNT);
  double normalized_peak = 0.0;
  /* One independent quadrature per texel; validation and encoding stay
     serial and in scan order. */
  std::vector<double> energy_values(VKR_SHEEN_ENERGY_LUT_TEXEL_COUNT);
  vkr_parallel_rows(VKR_SHEEN_ENERGY_LUT_SIZE, [&](uint32_t y) {
    const double roughness = roughness_from_unit(
        static_cast<double>(y) / (VKR_SHEEN_ENERGY_LUT_SIZE - 1u));
    const double scale = interpolate_scale(scales, roughness);
    for (uint32_t x = 0; x < VKR_SHEEN_ENERGY_LUT_SIZE; ++x) {
      const double sqrt_no_v = static_cast<double>(x) /
                               (VKR_SHEEN_ENERGY_LUT_SIZE - 1u);
      const double no_v = std::max(sqrt_no_v * sqrt_no_v, k_min_no_v);
      energy_values[static_cast<size_t>(y) * VKR_SHEEN_ENERGY_LUT_SIZE + x] =
          directional_energy(no_v, roughness) * scale;
    }
  });
  for (uint32_t y = 0; y < VKR_SHEEN_ENERGY_LUT_SIZE; ++y) {
    for (uint32_t x = 0; x < VKR_SHEEN_ENERGY_LUT_SIZE; ++x) {
      const double value =
          energy_values[static_cast<size_t>(y) * VKR_SHEEN_ENERGY_LUT_SIZE + x];
      if (!std::isfinite(value) || value < 0.0 || value > 1.0 + 1.0e-4) {
        std::cerr << "invalid normalized Charlie energy at " << x << ',' << y
                  << ": " << value << '\n';
        return 1;
      }
      normalized_peak = std::max(normalized_peak, value);
      energy.push_back(half(value));
    }
  }

  std::vector<uint16_t> ltc_matrix_a, ltc_amplitude_a;
  std::vector<uint16_t> ltc_matrix_b, ltc_amplitude_b;
  for (std::vector<uint16_t> *table : {&ltc_matrix_a, &ltc_amplitude_a,
                                       &ltc_matrix_b, &ltc_amplitude_b})
    table->assign(VKR_SHEEN_LTC_LUT_TABLE_TEXEL_COUNT * 4u, 0u);
  /* Each cell is seeded from its left neighbour and the cell above, so
     (x_index, y) depends only on (x_index - 1, y) and (x_index, y - 1). Every
     cell on one anti-diagonal is therefore independent, and solving a diagonal
     in parallel hands each cell exactly the seeds the serial scan gave it.
     The table is identical at any worker count. */
  std::vector<std::array<FullFit, 2>> fits(
      static_cast<size_t>(VKR_SHEEN_LTC_LUT_SIZE) * VKR_SHEEN_LTC_LUT_SIZE);
  std::vector<std::string> failures(fits.size());
  const auto solve_cell = [&](uint32_t y, uint32_t x_index) {
    const size_t cell =
        static_cast<size_t>(y) * VKR_SHEEN_LTC_LUT_SIZE + x_index;
    const bool has_left = x_index > 0u;
    const std::array<FullFit, 2> left =
        has_left ? fits[cell - 1u] : std::array<FullFit, 2>{};
    const double view_parameter = static_cast<double>(y) /
                                  (VKR_SHEEN_LTC_LUT_SIZE - 1u);
    /* Allocate 64² LTC rows near grazing where Charlie varies fastest. */
    const double no_v = (1.0 - view_parameter) * (1.0 - view_parameter);
    /* Charlie's nearly-zero roughness lobe is the unstable endpoint.  Fit
       from broad to narrow roughness and retain that continuation only for
       solving; table storage remains increasing roughness. */
    const uint32_t x = VKR_SHEEN_LTC_LUT_SIZE - 1u - x_index;
    const double roughness = roughness_from_unit(
        static_cast<double>(x) / (VKR_SHEEN_LTC_LUT_SIZE - 1u));
    const double scale = scales[x];
    const double target_energy = directional_energy(no_v, roughness);
    std::vector<JointSample> samples;
    samples.reserve(64u * 64u);
    const GaussRule &rule = gauss_rule();
    for (uint32_t z_index = 0; z_index < 64u; ++z_index) {
      const double no_l = rule.x[z_index];
      const double sin_l = std::sqrt(std::max(0.0, 1.0 - no_l * no_l));
      for (uint32_t phi_index = 0; phi_index < 64u; ++phi_index) {
        const double phi = 2.0 * k_pi * (phi_index + 0.5) / 64.0;
        const Vec3 wi = {sin_l * std::cos(phi), sin_l * std::sin(phi), no_l};
        samples.push_back({wi, no_l * charlie_brdf(no_v, wi, roughness) /
            std::max(target_energy, k_eps), rule.w[z_index] * 2.0 * k_pi / 64.0});
      }
    }
    const std::array<JointRectangle, 4> rectangles =
        joint_rectangles(no_v, roughness, target_energy);
    const RectfitObjective objective = {&samples, &rectangles};
    const auto parameters_for = [](const std::array<FullFit, 2> &pair) {
      return std::array<double, 8>{pair[0].log_s, pair[0].log_k, pair[0].h,
          pair[0].beta, pair[1].log_s, pair[1].log_k, pair[1].h, pair[1].beta};
    };
    const std::array<double, 8> fallback = {0.0, 0.0, 0.0, 0.0,
                                              -0.7, -0.7, 0.0, 0.35};
    std::array<FullFit, 2> pair = {};
    std::vector<double> residuals;
    const std::array<FullFit, 2> upper =
        y > 0u ? fits[(static_cast<size_t>(y) - 1u) * VKR_SHEEN_LTC_LUT_SIZE +
                      x_index]
               : std::array<FullFit, 2>{};
    if (y == 0u) {
      pair = rectfit_normal_view(objective, &residuals);
    } else {
      std::array<std::array<double, 8>, 3> starts = {fallback, fallback, fallback};
      uint32_t start_count = 1u;
      if (has_left) starts[start_count++] = parameters_for(left);
      starts[start_count++] = parameters_for(upper);
      double best_cost = std::numeric_limits<double>::infinity();
      for (uint32_t start_index = 0; start_index < start_count; ++start_index) {
        std::vector<double> candidate_residuals;
        const std::array<FullFit, 2> candidate = objective.evaluate(
            rectfit_optimize(objective, starts[start_index]), &candidate_residuals);
        const double cost = rectfit_squared_norm(candidate_residuals);
        if (cost < best_cost) {
          best_cost = cost;
          pair = candidate;
          residuals.swap(candidate_residuals);
        }
      }
    }
    /* A zero-weight component has no node response, but its encoded matrix
       is still bilinearly filtered.  Canonicalize it to the active lobe so
       an arbitrary optimizer endpoint cannot create a bright midpoint. */
    constexpr double k_inactive_fraction = 1.0e-4;
    if (pair[0].fraction <= k_inactive_fraction) {
      pair[0] = pair[1];
      /* The surviving lobe carries the whole mixture. Dropping the pruned
         weight without handing it over leaves the pair summing to just under
         one, which the mixture check below rejects. */
      pair[1].fraction = 1.0;
      pair[0].fraction = 0.0;
    } else if (pair[1].fraction <= k_inactive_fraction) {
      pair[1] = pair[0];
      pair[0].fraction = 1.0;
      pair[1].fraction = 0.0;
    }
    /* Components are equivalent under permutation.  Choose the assignment
       that minimizes the actual bilinear midpoint response, rather than
       a distance between encoded parameters. */
    const auto midpoint_error = [](const std::array<FullFit, 2> &current,
                                   const std::array<FullFit, 2> &neighbor,
                                   double midpoint_roughness,
                                   double midpoint_no_v) {
      std::array<FullFit, 2> midpoint = {};
      for (uint32_t component = 0; component < 2u; ++component) {
        midpoint[component] = {
            0.5 * (current[component].log_s + neighbor[component].log_s),
            0.5 * (current[component].log_k + neighbor[component].log_k),
            0.5 * (current[component].h + neighbor[component].h),
            0.5 * (current[component].beta + neighbor[component].beta),
            0.5 * (current[component].fraction + neighbor[component].fraction)};
      }
      const double fraction_sum = midpoint[0].fraction + midpoint[1].fraction;
      midpoint[0].fraction /= fraction_sum;
      midpoint[1].fraction /= fraction_sum;
      const double energy = directional_energy(midpoint_no_v, midpoint_roughness);
      double error = 0.0;
      for (uint32_t z_index = 0; z_index < 6u; ++z_index) {
        const double z = (z_index + 0.5) / 6.0;
        const double radial = std::sqrt(std::max(0.0, 1.0 - z * z));
        for (uint32_t phi_index = 0; phi_index < 8u; ++phi_index) {
          const double phi = 2.0 * k_pi * phi_index / 8.0;
          const Vec3 wi = {radial * std::cos(phi), radial * std::sin(phi), z};
          const double target = z * charlie_brdf(midpoint_no_v, wi,
                                                  midpoint_roughness) /
              std::max(energy, k_eps);
          const double estimate = midpoint[0].fraction *
                  full_density(midpoint[0], wi) / full_mass(midpoint[0]) +
              midpoint[1].fraction *
                  full_density(midpoint[1], wi) / full_mass(midpoint[1]);
          error += (estimate - target) * (estimate - target);
        }
      }
      return error;
    };
    if (has_left || y > 0u) {
      const std::array<FullFit, 2> swapped_pair = {pair[1], pair[0]};
      double direct = 0.0;
      double swapped = 0.0;
      const auto accumulate_neighbor = [&](const std::array<FullFit, 2> &neighbor,
                                           double midpoint_roughness,
                                           double midpoint_no_v) {
        direct += midpoint_error(pair, neighbor, midpoint_roughness,
                                 midpoint_no_v);
        swapped += midpoint_error(swapped_pair, neighbor, midpoint_roughness,
                                  midpoint_no_v);
      };
      if (has_left) {
        const double left_roughness = roughness_from_unit(
            static_cast<double>(x + 1u) / (VKR_SHEEN_LTC_LUT_SIZE - 1u));
        accumulate_neighbor(left, 0.5 * (roughness + left_roughness), no_v);
      }
      if (y > 0u) {
        const double upper_view = static_cast<double>(y - 0.5) /
            (VKR_SHEEN_LTC_LUT_SIZE - 1u);
        const double upper_no_v = (1.0 - upper_view) * (1.0 - upper_view);
        accumulate_neighbor(upper, roughness, upper_no_v);
      }
      if (swapped < direct) std::swap(pair[0], pair[1]);
    }
    for (const FullFit &component : pair) {
      if (!std::isfinite(component.log_s) || !std::isfinite(component.log_k) ||
          !std::isfinite(component.h) || !std::isfinite(component.beta) ||
          !std::isfinite(component.fraction) ||
          std::abs(component.log_s) > 6.0 || std::abs(component.log_k) > 6.0 ||
          std::abs(component.h) > 6.0 ||
          std::abs(component.beta) > k_full_beta_limit ||
          component.fraction < 0.0 || component.fraction > 1.0 ||
          full_mass(component) < 1.0e-4) {
        failures[cell] = "invalid encoded Charlie LTC at " +
                         std::to_string(x) + "," + std::to_string(y);
        return;
      }
    }
    if (std::abs(pair[0].fraction + pair[1].fraction - 1.0) > 1.0e-9) {
      failures[cell] = "invalid Charlie LTC mixture at " +
                       std::to_string(x) + "," + std::to_string(y);
      return;
    }
    fits[cell] = pair;
    const auto signed_half = [](double value) {
      const bool negative = value < 0.0;
      const uint16_t encoded = half(std::abs(value));
      return static_cast<uint16_t>(encoded | (negative ? 0x8000u : 0u));
    };
    const size_t offset = (static_cast<size_t>(y) * VKR_SHEEN_LTC_LUT_SIZE + x) * 4u;
    const std::array<double, 4> encoded_a = {pair[0].log_s, pair[0].log_k,
                                              pair[0].h, pair[0].beta};
    const std::array<double, 4> encoded_b = {pair[1].log_s, pair[1].log_k,
                                              pair[1].h, pair[1].beta};
    for (uint32_t channel = 0; channel < 4u; ++channel) {
      ltc_matrix_a[offset + channel] = signed_half(encoded_a[channel]);
      ltc_matrix_b[offset + channel] = signed_half(encoded_b[channel]);
    }
    ltc_amplitude_a[offset] = half(pair[0].fraction);
    ltc_amplitude_a[offset + 1u] = half(k_allocation_relative_reserve);
    ltc_amplitude_a[offset + 2u] = half(k_allocation_absolute_reserve);
    ltc_amplitude_a[offset + 3u] = half(scale);
    ltc_amplitude_b[offset] = half(pair[1].fraction);
  };

  {
    const uint32_t worker_count =
        std::max(1u, std::thread::hardware_concurrency());
    constexpr uint32_t last_index = VKR_SHEEN_LTC_LUT_SIZE - 1u;
    bool diagonal_failed = false;
    for (uint32_t diagonal = 0; !diagonal_failed && diagonal <= 2u * last_index;
         ++diagonal) {
      const uint32_t first_y =
          diagonal > last_index ? diagonal - last_index : 0u;
      const uint32_t last_y = std::min(diagonal, last_index);
      std::atomic<uint32_t> next_y{first_y};
      std::vector<std::thread> workers;
      const uint32_t count = std::min(worker_count, last_y - first_y + 1u);
      workers.reserve(count);
      for (uint32_t worker = 0; worker < count; ++worker)
        workers.emplace_back([&]() {
          for (;;) {
            const uint32_t y = next_y.fetch_add(1u);
            if (y > last_y)
              break;
            solve_cell(y, diagonal - y);
          }
        });
      for (std::thread &worker : workers)
        worker.join();
      for (uint32_t y = first_y; y <= last_y; ++y)
        diagonal_failed =
            diagonal_failed ||
            !failures[static_cast<size_t>(y) * VKR_SHEEN_LTC_LUT_SIZE +
                      (diagonal - y)].empty();
    }
    /* Report in scan order, so a rejected fit names the cell the serial scan
       would have named. */
    for (const std::string &failure : failures)
      if (!failure.empty()) {
        std::cerr << failure << '\n';
        return 1;
      }
  }

  std::ostringstream output;
  output << "// Generated by vkr_sheen_cooker. Charlie D plus Estevez/Kulla\n"
            "// visibility; raw R16F directional E and four RGBA16F LTC tables.\n"
            "// Matrices are {log2(s),log2(k),shear,beta}; table 1 is\n"
            "// {fraction A, relative reserve, absolute reserve, E scale};\n"
            "// table 3 stores fraction B in R. Regenerate all consumers together.\n\n"
            "#if defined(VKR_SHEEN_LUT_EMIT_ENERGY)\n";
  emit_values(&output, energy);
  output << "#elif defined(VKR_SHEEN_LUT_EMIT_LTC)\n{\n";
  emit_values(&output, ltc_matrix_a);
  output << "}, {\n";
  emit_values(&output, ltc_amplitude_a);
  output << "}, {\n";
  emit_values(&output, ltc_matrix_b);
  output << "}, {\n";
  emit_values(&output, ltc_amplitude_b);
  output << "}\n#else\n#error \"Select a Vkr sheen LUT section before including this file\"\n#endif\n";

  const std::string contents = output.str();
  std::string existing;
  {
    // Windows refuses to replace a file this process still holds open, so the
    // comparison read must be closed before the atomic rename.
    std::ifstream previous_file(argv[1], std::ios::binary);
    existing.assign((std::istreambuf_iterator<char>(previous_file)),
                    std::istreambuf_iterator<char>());
  }
  if (existing == contents) {
    std::cout << "Sheen LUT unchanged\n";
    return 0;
  }
  if (!vkr_tools::write_file_atomic(argv[1], contents)) return 1;
  std::cout << "Sheen LUT generated: raw_E_peak=" << raw_peak
            << " normalized_E_peak=" << normalized_peak
            << " scale_range=" << *std::min_element(scales.begin(), scales.end())
            << ".." << *std::max_element(scales.begin(), scales.end()) << '\n';
  return 0;
}

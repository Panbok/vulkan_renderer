#include "vkr_anisotropy_lut.h"
extern "C" {
#include "vkr_ltc_lut.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/* Deterministic reparameterization of the author's 8^4 sliced-Wasserstein
 * anisotropic LTC fit (pinned, licensed input below), plus independent
 * correlated-Smith Schlick DFG quadrature using the visible-normal sampler
 * of Heitz, JCGT 7(4), 2018. Resampling adds lookup resolution, not fit detail.
 * Cholesky factorization removes the irrelevant cosine-plane rotation before
 * the final FP16 table is filtered. This avoids determinant flips without
 * requiring the author's random alignment search during a build. */
namespace {
constexpr double pi = 3.1415926535897932384626433832795;
constexpr uint32_t dfg_samples = 4096u;

constexpr uint32_t size = VKR_ANISOTROPY_LUT_SIZE;
struct V3 {
  double x, y, z;
};
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(V3 a, double b) { return {a.x * b, a.y * b, a.z * b}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 normal(V3 a) { return mul(a, 1.0 / std::sqrt(dot(a, a))); }
uint32_t reverse_bits(uint32_t v) {
  v = (v << 16u) | (v >> 16u);
  v = ((v & 0x55555555u) << 1u) | ((v & 0xaaaaaaaau) >> 1u);
  v = ((v & 0x33333333u) << 2u) | ((v & 0xccccccccu) >> 2u);
  v = ((v & 0x0f0f0f0fu) << 4u) | ((v & 0xf0f0f0f0u) >> 4u);
  return ((v & 0x00ff00ffu) << 8u) | ((v & 0xff00ff00u) >> 8u);
}
struct Disk {
  double x, y;
};
std::vector<Disk> make_disk(uint32_t count) {
  std::vector<Disk> d(count);
  for (uint32_t i = 0; i < count; ++i) {
    double r = std::sqrt((i + .5) / count),
           p = 2 * pi * reverse_bits(i) / 4294967296.0;
    d[i] = {r * std::cos(p), r * std::sin(p)};
  }
  return d;
}
struct GGX {
  V3 v, vh, t1, t2;
  double ax, ay, rv, g1, blend;
  GGX(double nov, double rough, double strength, double phi) {
    double st = std::sqrt(1 - nov * nov);
    v = {st * std::cos(phi), st * std::sin(phi), nov};
    ay = rough * rough;
    ax = ay + (1 - ay) * strength * strength;
    vh = normal({ax * v.x, ay * v.y, v.z});
    double l2 = vh.x * vh.x + vh.y * vh.y;
    t1 = l2 > 1e-16 ? mul(V3{-vh.y, vh.x, 0}, 1 / std::sqrt(l2)) : V3{1, 0, 0};
    t2 = cross(vh, t1);
    blend = .5 * (1 + vh.z);
    rv = std::sqrt(ax * ax * v.x * v.x + ay * ay * v.y * v.y + v.z * v.z);
    g1 = 2 * v.z / (v.z + rv);
  }
  V3 sample(Disk d) const {
    double y =
        (1 - blend) * std::sqrt(std::max(0.0, 1 - d.x * d.x)) + blend * d.y;
    double z = std::sqrt(std::max(0.0, 1 - d.x * d.x - y * y));
    V3 h = add(add(mul(t1, d.x), mul(t2, y)), mul(vh, z));
    h = normal({ax * h.x, ay * h.y, std::max(0.0, h.z)});
    return add(mul(h, 2 * dot(v, h)), mul(v, -1));
  }
  void evaluate(V3 l, double &f, double &pdf, double &fr) const {
    if (l.z <= 0) {
      f = pdf = fr = 0;
      return;
    }
    V3 h = normal(add(v, l));
    double q = h.x * h.x / (ax * ax) + h.y * h.y / (ay * ay) + h.z * h.z;
    double d = 1 / (pi * ax * ay * q * q);
    double rl =
        std::sqrt(ax * ax * l.x * l.x + ay * ay * l.y * l.y + l.z * l.z);
    f = d * l.z / (2 * (l.z * rv + v.z * rl));
    pdf = d * g1 / (4 * v.z);
    double m = std::max(0.0, 1 - dot(v, h));
    fr = m * m * m * m * m;
  }
  std::array<double, 2> energy(const std::vector<Disk> &disk) const {
    std::array<double, 2> e = {};
    for (Disk d : disk) {
      V3 l = sample(d);
      if (l.z <= 0)
        continue;
      double f, pdf, fr;
      evaluate(l, f, pdf, fr);
      double w = f / pdf;
      e[0] += w * (1 - fr);
      e[1] += w * fr;
    }
    e[0] /= disk.size();
    e[1] /= disk.size();
    return e;
  }
};
#include "assets/anisotropy_ltc_seed.inc"
struct Shape {
  double log_a, log_b, h, j, k;
  V3 support;
  double mass() const {
    const double a = std::exp2(log_a), b = std::exp2(log_b);
    return .5 *
           (1 + dot(normal(support), normal({h * k - j * b, -k * a, a * b})));
  }
};
Shape encode_inverse(const V3 cols[3]) {
  double a[3][3];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      a[i][j] = dot(cols[i], cols[j]);
  double c22 = std::sqrt(a[2][2]), c21 = a[1][2] / c22, c20 = a[0][2] / c22;
  double c11 = std::sqrt(std::max(1e-12, a[1][1] - c21 * c21)),
         c10 = (a[0][1] - c20 * c21) / c11;
  double c00 = std::sqrt(std::max(1e-12, a[0][0] - c10 * c10 - c20 * c20));
  double qz = cols[2].z / c22, qy = (cols[1].z - c21 * qz) / c11,
         qx = (cols[0].z - c10 * qy - c20 * qz) / c00;
  return {std::log2(c00 / c22),
          std::log2(c11 / c22),
          c10 / c22,
          c20 / c22,
          c21 / c22,
          normal({qx, qy, qz})};
}
Shape shape_for(const GGX &g, double phi) {
  double pos[4] = {g.ax * 7, g.ay / g.ax * 7, std::acos(g.v.z) * 14 / pi,
                   phi * 14 / pi};
  int low[4], high[4];
  double w[4];
  for (int i = 0; i < 4; i++) {
    pos[i] = std::clamp(pos[i], 0.0, 7.0);
    low[i] = (int)pos[i];
    high[i] = std::min(7, low[i] + 1);
    w[i] = pos[i] - low[i];
  }
  double m[9] = {};
  for (int c = 0; c < 16; c++) {
    int id[4];
    double weight = 1;
    for (int i = 0; i < 4; i++) {
      bool up = (c >> i) & 1;
      id[i] = up ? high[i] : low[i];
      weight *= up ? w[i] : 1 - w[i];
    }
    for (int i = 0; i < 9; i++)
      m[i] += weight * anisotropy_seed_matrices[id[0]][id[1]][id[2]][id[3]][i];
  }
  V3 rows[3] = {{m[0], m[1], m[2]}, {m[3], m[4], m[5]}, {m[6], m[7], m[8]}};
  V3 cols[3] = {cross(rows[1], rows[2]), cross(rows[2], rows[0]),
                cross(rows[0], rows[1])};
  double det = dot(rows[0], cols[0]);
  for (V3 &v : cols)
    v = mul(v, 1 / det);
  return encode_inverse(cols);
}

uint16_t half(double value) {
  bool negative = value < 0;
  value = std::abs(value);
  auto even = [](double v) {
    uint32_t i = (uint32_t)std::floor(v);
    double f = v - i;
    return i + (f > .5 || (f == .5 && (i & 1)));
  };
  uint16_t h;
  if (value < std::ldexp(1.0, -14))
    h = (uint16_t)even(std::ldexp(value, 24));
  else {
    int e;
    double m = std::frexp(value, &e);
    h = (uint16_t)(((e + 14) << 10) + even((2 * m - 1) * 1024));
  }
  return h | (negative ? 0x8000u : 0u);
}
double unhalf(uint16_t h) {
  int e = (h >> 10) & 31;
  double v = e ? std::ldexp(1 + (h & 1023) / 1024.0, e - 15)
               : std::ldexp((double)(h & 1023), -24);
  return h & 0x8000 ? -v : v;
}
std::array<double, 4> isotropic_sample(uint32_t table, double no_v,
                                       double roughness) {
  double x = roughness * 63.0, y = std::sqrt(1.0 - no_v) * 63.0;
  uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
  uint32_t x1 = std::min(63u, x0 + 1u), y1 = std::min(63u, y0 + 1u);
  double fx = x - x0, fy = y - y0;
  std::array<double, 4> tuple = {};
  for (uint32_t corner = 0; corner < 4u; ++corner) {
    uint32_t ix = corner & 1u ? x1 : x0, iy = corner & 2u ? y1 : y0;
    double weight =
        (corner & 1u ? fx : 1.0 - fx) * (corner & 2u ? fy : 1.0 - fy);
    for (uint32_t channel = 0; channel < 4u; ++channel)
      tuple[channel] +=
          weight *
          unhalf(vkr_ltc_lut_pixels[table][(iy * 64u + ix) * 4u + channel]);
  }
  return tuple;
}
Shape isotropic_shape(double no_v, double roughness, double phi) {
  const auto tuple = isotropic_sample(0u, no_v, roughness);
  // The current isotropic fit uses the projected view as its X axis. Convert
  // inverse-matrix columns into the physical anisotropy T/B/N frame.
  double cs = std::cos(phi), sn = std::sin(phi);
  V3 columns[3] = {{tuple[0] * cs, -sn, tuple[1] * cs},
                   {tuple[0] * sn, cs, tuple[1] * sn},
                   {tuple[2], 0, tuple[3]}};
  return encode_inverse(columns);
}
struct Record {
  Shape shape;
  std::array<double, 2> ab;
};
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: vkr_anisotropy_cooker <output.inc>\n";
    return 2;
  }
  const auto energy_disk = make_disk(dfg_samples);
  constexpr size_t count = size * size * 64u;
  std::vector<Record> records(count);
  std::atomic<uint32_t> next_layer{0};
  std::vector<std::thread> workers;
  const uint32_t threads =
      std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
  for (uint32_t thread = 0; thread < threads; ++thread)
    workers.emplace_back([&]() {
      for (;;) {
        uint32_t layer = next_layer.fetch_add(1);
        if (layer >= 64)
          break;
        double strength = (layer / 8) / 7.0, phi = (layer % 8) * pi / 14;
        for (uint32_t y = 0; y < size; ++y)
          for (int x = size - 1; x >= 0; --x) {
            size_t offset = (layer * size + y) * size + x;
            double rough = .04 + .96 * x / (size - 1),
                   nov = std::pow(1 - .99 * y / (size - 1), 2);
            GGX g(nov, rough, strength, phi);
            records[offset] = {layer < 8u ? isotropic_shape(nov, rough, phi)
                                          : shape_for(g, phi),
                               g.energy(energy_disk)};
          }
      }
    });
  for (auto &worker : workers)
    worker.join();
  std::array<std::vector<uint16_t>, 3> tables;
  for (auto &t : tables)
    t.resize(count * 4);
  double min_mass = 1, min_energy = 1, max_energy = 0, min_support_z = 1;
  for (size_t i = 0; i < count; ++i) {
    const Record &r = records[i];
    const Shape &shape = r.shape;
    min_mass = std::min(min_mass, shape.mass());
    // Express support relative to the transformed physical horizon. Encoding
    // it in a fixed frame allows independently interpolated C/support to lose
    // almost all physical mass between otherwise well-conditioned knots.
    const V3 q = shape.support;
    const double a = std::exp2(shape.log_a), b = std::exp2(shape.log_b);
    const V3 w = normal({shape.h * shape.k - shape.j * b, -shape.k * a, a * b});
    const double basis_a = -1.0 / (1.0 + w.z);
    const double basis_b = w.x * w.y * basis_a;
    const V3 e1 = {1.0 + w.x * w.x * basis_a, basis_b, -w.x};
    const V3 e2 = {basis_b, 1.0 + w.y * w.y * basis_a, -w.y};
    const V3 u = {dot(q, e1), dot(q, e2), dot(q, w)};
    min_support_z = std::min(min_support_z, u.z);
    min_energy = std::min(min_energy, r.ab[0] + r.ab[1]);
    max_energy = std::max(max_energy, r.ab[0] + r.ab[1]);
    const double values[12] = {
        shape.log_a, shape.log_b, 0,   shape.h,           shape.j, shape.k,
        u.x,         u.y,         u.z, r.ab[0] + r.ab[1], r.ab[0], r.ab[1]};
    for (uint32_t c = 0; c < 12; ++c) {
      if (!std::isfinite(values[c]) || std::abs(values[c]) > 65504) {
        std::cerr << "Anisotropy fit non-finite/unrepresentable record " << i
                  << '\n';
        return 1;
      }
      tables[c / 4][i * 4 + c % 4] = half(values[c]);
    }
    // Independent rounding of A/B can exceed one by one half-precision ULP.
    // Lower the larger coefficient at the cooking boundary; convex filtering
    // then preserves the unit-Fresnel energy bound without shader clamps.
    uint16_t &a_half = tables[2][i * 4 + 2];
    uint16_t &b_half = tables[2][i * 4 + 3];
    if (unhalf(a_half) + unhalf(b_half) > 1.0) {
      if (a_half >= b_half)
        --a_half;
      else
        --b_half;
    }
    // Rectangle amplitude has its own Schlick A/B. At strength zero retain
    // the existing isotropic fit's amplitude, converted from full cosine mass
    // to physical-hemisphere mass. This avoids an 18% lighting jump at an
    // arbitrarily small authored anisotropy. Other layers use directional DFG.
    if (i < 8u * size * size) {
      const uint32_t x = (uint32_t)(i % size),
                     y = (uint32_t)((i / size) % size);
      const double roughness = .04 + .96 * x / (size - 1);
      const double no_v = std::pow(1 - .99 * y / (size - 1), 2);
      const auto amplitude = isotropic_sample(1u, no_v, roughness);
      const double mass = shape.mass();
      tables[0][i * 4 + 2] = half((amplitude[0] - amplitude[1]) * mass);
      tables[2][i * 4 + 1] = half(amplitude[1] * mass);
    } else {
      tables[0][i * 4 + 2] = a_half;
      tables[2][i * 4 + 1] = b_half;
    }
    if (unhalf(tables[2][i * 4]) <= 0) {
      std::cerr << "Anisotropy support hemisphere failure " << i << '\n';
      return 1;
    }
  }
  if (min_mass <= 0 || min_energy < .25 || max_energy > 1.00001) {
    std::cerr << "Anisotropy fit energy/support invariant failure\n";
    return 1;
  }
  std::ostringstream out;
  out << "// Generated by vkr_anisotropy_cooker: correlated-Smith anisotropic "
         "GGX.\n"
         "// Three 64x64x64 RGBA16F arrays: raw Cholesky, horizon-relative "
         "support,\n"
         "// separate rectangle A/B and directional DFG A/B. Strength0 "
         "retains\n"
         "// the existing 64^2 isotropic shape/amplitude; other shapes "
         "resample\n"
         "// the author's 8^4 SW fit. DFG uses 4096 visible-normal samples.\n";
  for (uint32_t table = 0; table < 3; ++table) {
    out << "{\n";
    for (size_t i = 0; i < tables[table].size(); ++i) {
      out << "0x" << std::hex << std::setw(4) << std::setfill('0')
          << tables[table][i] << ',';
      out << (i % 16 == 15 ? '\n' : ' ');
    }
    out << (table == 2 ? "}\n" : "},\n");
  }
  const std::string contents = out.str();
  std::ifstream old(argv[1], std::ios::binary);
  const std::string previous((std::istreambuf_iterator<char>(old)), {});
  if (previous == contents)
    std::cout << "Anisotropy LUT unchanged\n";
  else {
    std::ofstream file(argv[1], std::ios::binary | std::ios::trunc);
    file.write(contents.data(), contents.size());
    if (!file)
      return 1;
  }
  std::cout << "Anisotropy fit: nodes=" << count
            << " min_physical_mass=" << min_mass
            << " min_support_z=" << min_support_z
            << " energy_range=" << min_energy << ".." << max_energy << '\n';
  return 0;
}

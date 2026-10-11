/* Alpine terrain generator for VKR terrains (vkr-landscape skill).
 *
 * Builds one world height function (domain-warped ridged massifs and an
 * optional U valley with a cliff, a terrace and upper slopes), samples it
 * twice and erodes each grid:
 *   main:     N x N samples over the playable terrain (terrain.create size
 *             = (N-1) * spacing), detail added, constraints applied, its
 *             border blended to the backdrop;
 *   backdrop: N x N samples at a coarse spacing around it, sunk under the
 *             main terrain inside its footprint (8 m one sample in) so the
 *             two never z-fight.
 * Writes main_h.png / back_h.png (16-bit, terrain.stamp mode set with
 * center.y = --min and height_scale = --max - --min, or the backdrop's),
 * main_w.png / back_w.png (RGBA layer weights for mode paint: grass, rock,
 * scree, snow), *_preview.png (shaded relief) and main_h.f32 (floats, for
 * placement scripts and crop restores).
 *
 *   cc -O2 -o alpine_terrain alpine_terrain.c -lz -lm
 *   ./alpine_terrain --out DIR --center X Z --size 2048 [options]
 *
 * Options (metres):
 *   --size S          main terrain side (N = 1025 samples, spacing S/1024)
 *   --min H --max H   main stored height range (terrain.create
 *                     height_min/max); peaks roll off 220 m below --max
 *   --back-size S     backdrop side (default 8 * size), same N
 *   --back-min/--back-max  backdrop stored range (default -400 / 2800)
 *   --valley X        U valley along z through world x X at z = center
 *   --valley-floor H  its floor height (default min + 50)
 *   --seed K          noise seed
 *   --floor X0 Z0 X1 Z1 H   ground at least H inside the box (rock over a
 *                     buried room); repeatable
 *   --cap X0 Z0 X1 Z1 H     ground at most H inside the box (a corridor, a
 *                     bridge's clearance); repeatable
 * Erosion fills valley floors with sediment, so the valley is carved again
 * after it, and never cuts more than 12 m (main) or 60 m (backdrop) under
 * the uneroded surface. Heights assume a terrain entity at y = 0: add its
 * y to every printed center y otherwise. Runs in about 10 s and 50 MB at
 * N = 1025.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define N 1025
#define BOX_MAX 32

typedef double f64;

typedef struct Box {
  f64 x0, z0, x1, z1, h;
} Box;

static f64 g_cx, g_cz, g_size = 2048.0, g_min = -160.0, g_max = 1040.0;
static f64 g_back_size = 0.0, g_back_min = -400.0, g_back_max = 2800.0;
static f64 g_valley_x = NAN, g_valley_floor = NAN;
static uint32_t g_seed = 0u;
static Box g_floors[BOX_MAX], g_caps[BOX_MAX];
static int g_floor_count, g_cap_count;

/* ---------------------------------------------------------------- noise */

static uint32_t hash2(int32_t x, int32_t z, uint32_t seed) {
  uint32_t h =
      (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + seed * 1442695041u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

/* Gradient noise in [-1, 1]. */
static f64 gnoise(f64 x, f64 z, uint32_t seed) {
  const int32_t ix = (int32_t)floor(x);
  const int32_t iz = (int32_t)floor(z);
  const f64 fx = x - ix;
  const f64 fz = z - iz;
  f64 d[4];
  for (int i = 0; i < 4; ++i) {
    const int32_t cx = ix + (i & 1);
    const int32_t cz = iz + (i >> 1);
    const f64 a = (hash2(cx, cz, seed) & 0xFFFF) / 65536.0 * 6.283185307;
    d[i] = cos(a) * (fx - (i & 1)) + sin(a) * (fz - (i >> 1));
  }
  const f64 ux = fx * fx * fx * (fx * (fx * 6 - 15) + 10);
  const f64 uz = fz * fz * fz * (fz * (fz * 6 - 15) + 10);
  const f64 a = d[0] + (d[1] - d[0]) * ux;
  const f64 b = d[2] + (d[3] - d[2]) * ux;
  return (a + (b - a) * uz) * 1.41421356;
}

static f64 fbm(f64 x, f64 z, int octaves, uint32_t seed) {
  f64 total = 0, amp = 1, norm = 0, freq = 1;
  for (int o = 0; o < octaves; ++o) {
    total += gnoise(x * freq, z * freq, seed + g_seed + o * 31u) * amp;
    norm += amp;
    amp *= 0.5;
    freq *= 2.01;
  }
  return total / norm;
}

/* Ridged multifractal in [0, 1]: sharp crests, smooth valleys. */
static f64 ridged(f64 x, f64 z, int octaves, uint32_t seed) {
  f64 total = 0, amp = 1, freq = 1, weight = 1, norm = 0;
  for (int o = 0; o < octaves; ++o) {
    f64 n = 1.0 - fabs(gnoise(x * freq, z * freq, seed + g_seed + o * 17u));
    n = n * n * weight;
    weight = fmin(1.0, fmax(0.0, n * 1.8));
    total += n * amp;
    norm += amp;
    amp *= 0.48;
    freq *= 2.07;
  }
  return total / norm;
}

static f64 sstep(f64 a, f64 b, f64 t) {
  t = fmin(1.0, fmax(0.0, (t - a) / (b - a)));
  return t * t * (3 - 2 * t);
}

static f64 smin(f64 a, f64 b, f64 k) {
  const f64 h = fmax(k - fabs(a - b), 0.0) / k;
  return fmin(a, b) - h * h * k * 0.25;
}

static f64 smax(f64 a, f64 b, f64 k) { return -smin(-a, -b, k); }

static f64 lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }

/* ------------------------------------------------------------ the world */

static f64 axis_x(f64 z) {
  const f64 b = z - g_cz;
  return g_valley_x + 38.0 * sin(b / 260.0) + 16.0 * sin(b / 93.0);
}

static f64 massif(f64 x, f64 z, int octaves) {
  const f64 wx = x + 380.0 * fbm(x / 1400.0, z / 1400.0, 3, 41);
  const f64 wz = z + 380.0 * fbm(x / 1400.0, z / 1400.0, 3, 53);
  const f64 r = ridged(wx / 1500.0, wz / 1500.0, octaves, 7);
  const f64 broad = 0.5 + 0.5 * fbm(x / 5200.0, z / 5200.0, 2, 99);
  return g_min + 280.0 + (800.0 + 2000.0 * broad) * pow(r, 1.7);
}

/* The U valley's height at distance d from its axis: floor, a lower wall
   steepening into a cliff at its top, a terrace, the upper slopes. */
static f64 valley_profile(f64 d, f64 z) {
  const f64 floor_w = 80.0 + 12.0 * sin(z / 300.0);
  const f64 run = 130.0;
  const f64 wall_h = 180.0 + 40.0 * sin(z / 520.0 + 2.0);
  const f64 terrace_w = 150.0 + 60.0 * sin(z / 410.0 + 0.7);
  f64 y = g_valley_floor + 4.0 * sstep(10.0, floor_w, d);
  const f64 t = fmin(1.0, fmax(0.0, (d - floor_w) / run));
  y += wall_h * pow(t, 2.2);
  const f64 t0 = floor_w + run;
  y += 35.0 * sstep(t0, t0 + terrace_w, d);
  y += 1000.0 * pow(sstep(t0 + terrace_w, t0 + terrace_w + 1100.0, d), 1.3);
  return y;
}

static f64 valley_carve(f64 x, f64 z, f64 h) {
  if (isnan(g_valley_x)) {
    return h;
  }
  const f64 d = fabs(x - axis_x(z));
  if (d > 1400.0) {
    return h;
  }
  const f64 v = valley_profile(d, z) + 3.0 * fbm(x / 35.0, z / 35.0, 3, 23) -
                5.0 * (1.0 - sstep(0.0, 12.0, d));
  return smin(h, v, 24.0);
}

static f64 world_height(f64 x, f64 z, int fine) {
  f64 h = massif(x, z, fine ? 9 : 6);
  if (!isnan(g_valley_x)) {
    const f64 d = fabs(x - axis_x(z));
    const f64 gully = 40.0 * pow(fabs(gnoise(z / 210.0, x / 900.0, 5)), 0.6);
    h = smin(h,
             valley_profile(d, z) - gully * sstep(60.0, 220.0, d) *
                                        (1.0 - sstep(600.0, 1100.0, d)),
             120.0);
    h += (1.0 - sstep(60.0, 140.0, d)) * 5.0 * fbm(x / 45.0, z / 45.0, 3, 11);
  }
  if (fine) {
    h += 6.0 * fbm(x / 22.0, z / 22.0, 4, 19);
  }
  return h;
}

/* The --floor and --cap boxes, blended over a few metres. */
static f64 constrain(f64 x, f64 z, f64 h) {
  for (int i = 0; i < g_floor_count; ++i) {
    const Box *b = &g_floors[i];
    const f64 ox = fmax(fmax(b->x0 - x, x - b->x1), 0.0);
    const f64 oz = fmax(fmax(b->z0 - z, z - b->z1), 0.0);
    h = smax(h, b->h - sqrt(ox * ox + oz * oz) * 0.8, 16.0);
  }
  for (int i = 0; i < g_cap_count; ++i) {
    const Box *b = &g_caps[i];
    const f64 ox = fmax(fmax(b->x0 - x, x - b->x1), 0.0);
    const f64 oz = fmax(fmax(b->z0 - z, z - b->z1), 0.0);
    h = smin(h, b->h + sqrt(ox * ox + oz * oz) * 0.9, 6.0);
  }
  return h;
}

/* ------------------------------------------------------------- grids */

typedef struct Grid {
  f64 x0, z0, step;
  float *h;
  float *deposit;
} Grid;

/* Bilinear height at world (x, z), clamped to the grid. */
static f64 grid_sample(const Grid *g, const float *a, f64 x, f64 z) {
  f64 u = (x - g->x0) / g->step;
  f64 v = (z - g->z0) / g->step;
  u = fmin(N - 1.001, fmax(0.0, u));
  v = fmin(N - 1.001, fmax(0.0, v));
  const int i = (int)u;
  const int j = (int)v;
  const f64 fu = u - i;
  const f64 fv = v - j;
  const f64 a0 =
      a[(size_t)j * N + i] * (1 - fu) + a[(size_t)j * N + i + 1] * fu;
  const f64 a1 = a[(size_t)(j + 1) * N + i] * (1 - fu) +
                 a[(size_t)(j + 1) * N + i + 1] * fu;
  return a0 * (1 - fv) + a1 * fv;
}

/* Catmull-Rom height at world (x, z), so 16 m data upsamples smoothly. */
static f64 cubic(f64 p0, f64 p1, f64 p2, f64 p3, f64 t) {
  return p1 + 0.5 * t *
                  (p2 - p0 +
                   t * (2 * p0 - 5 * p1 + 4 * p2 - p3 +
                        t * (3 * (p1 - p2) + p3 - p0)));
}

static f64 grid_cubic(const Grid *g, const float *a, f64 x, f64 z) {
  const f64 u = (x - g->x0) / g->step;
  const f64 v = (z - g->z0) / g->step;
  const int i = (int)floor(u);
  const int j = (int)floor(v);
  const f64 fu = u - i;
  const f64 fv = v - j;
  f64 rows[4];
  for (int r = 0; r < 4; ++r) {
    const int jj = (int)fmin(N - 1, fmax(0, j - 1 + r));
    f64 p[4];
    for (int c = 0; c < 4; ++c) {
      const int ii = (int)fmin(N - 1, fmax(0, i - 1 + c));
      p[c] = a[(size_t)jj * N + ii];
    }
    rows[r] = cubic(p[0], p[1], p[2], p[3], fu);
  }
  return cubic(rows[0], rows[1], rows[2], rows[3], fv);
}

/* -------------------------------------------------------- erosion */

static uint32_t rng_state = 2463534242u;
static f64 rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return (rng_state & 0xFFFFFF) / 16777216.0;
}

/* Height and gradient at a fractional cell position, heights in cells. */
static void height_grad(const float *h, f64 px, f64 pz, f64 scale, f64 *out_h,
                        f64 *gx, f64 *gz) {
  const int i = (int)px;
  const int j = (int)pz;
  const f64 u = px - i;
  const f64 v = pz - j;
  const f64 a = h[(size_t)j * N + i] * scale;
  const f64 b = h[(size_t)j * N + i + 1] * scale;
  const f64 c = h[(size_t)(j + 1) * N + i] * scale;
  const f64 d = h[(size_t)(j + 1) * N + i + 1] * scale;
  *gx = (b - a) * (1 - v) + (d - c) * v;
  *gz = (c - a) * (1 - u) + (d - b) * u;
  *out_h =
      a * (1 - u) * (1 - v) + b * u * (1 - v) + c * (1 - u) * v + d * u * v;
}

/* Droplet hydraulic erosion (after Hans Beyer / Sebastian Lague), heights in
   metres converted to cell units by `step`. `deposit` accumulates settled
   sediment for the scree layer. `lock` marks cells it must not change. */
static void erode(float *h, float *deposit, const uint8_t *lock, f64 step,
                  int droplets, int radius, f64 strength) {
  const f64 scale = 1.0 / step;
  const f64 inertia = 0.06, capacity_k = 4.0, min_slope = 0.01;
  const f64 deposit_k = 0.3, erode_k = 0.3 * strength, evaporate = 0.015;
  const f64 gravity = 4.0;
  const int max_steps = 80;
  /* The erosion brush's weights. */
  const int bw = radius * 2 + 1;
  f64 *brush = malloc(sizeof(f64) * bw * bw);
  f64 brush_sum = 0;
  for (int dz = -radius; dz <= radius; ++dz) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const f64 w = fmax(0.0, radius - sqrt(dx * dx + dz * dz));
      brush[(dz + radius) * bw + dx + radius] = w;
      brush_sum += w;
    }
  }
  for (int k = 0; k < bw * bw; ++k) {
    brush[k] /= brush_sum;
  }
  for (int n = 0; n < droplets; ++n) {
    f64 px = 1 + rnd() * (N - 3);
    f64 pz = 1 + rnd() * (N - 3);
    f64 dx = 0, dz = 0, speed = 1, water = 1, sediment = 0;
    for (int s = 0; s < max_steps; ++s) {
      const int ci = (int)px;
      const int cj = (int)pz;
      const f64 ou = px - ci;
      const f64 ov = pz - cj;
      f64 hh, gx, gz;
      height_grad(h, px, pz, scale, &hh, &gx, &gz);
      dx = dx * inertia - gx * (1 - inertia);
      dz = dz * inertia - gz * (1 - inertia);
      const f64 len = sqrt(dx * dx + dz * dz);
      if (len < 1e-9) {
        break;
      }
      dx /= len;
      dz /= len;
      px += dx;
      pz += dz;
      if (px < 1 || pz < 1 || px >= N - 2 || pz >= N - 2) {
        break;
      }
      f64 nh, ngx, ngz;
      height_grad(h, px, pz, scale, &nh, &ngx, &ngz);
      const f64 dh = nh - hh;
      const f64 capacity = fmax(-dh, min_slope) * speed * water * capacity_k;
      if (sediment > capacity || dh > 0) {
        const f64 amount =
            dh > 0 ? fmin(dh, sediment) : (sediment - capacity) * deposit_k;
        sediment -= amount;
        const f64 m = amount * step;
        const size_t idx = (size_t)cj * N + ci;
        if (!lock || !lock[idx]) {
          h[idx] += (float)(m * (1 - ou) * (1 - ov));
          h[idx + 1] += (float)(m * ou * (1 - ov));
          h[idx + N] += (float)(m * (1 - ou) * ov);
          h[idx + N + 1] += (float)(m * ou * ov);
          deposit[idx] += (float)m;
        }
      } else {
        const f64 amount = fmin((capacity - sediment) * erode_k, -dh);
        for (int bz = -radius; bz <= radius; ++bz) {
          for (int bx = -radius; bx <= radius; ++bx) {
            const int ii = ci + bx;
            const int jj = cj + bz;
            if (ii < 0 || jj < 0 || ii >= N || jj >= N) {
              continue;
            }
            const size_t idx = (size_t)jj * N + ii;
            if (lock && lock[idx]) {
              continue;
            }
            const f64 w = brush[(bz + radius) * bw + bx + radius];
            const f64 take = amount * w * step;
            h[idx] -= (float)take;
          }
        }
        sediment += amount;
      }
      speed = sqrt(fmax(0.0, speed * speed - dh * gravity));
      water *= 1 - evaporate;
    }
  }
  free(brush);
}

/* Thermal relaxation: material above the talus slope (rise per cell
   distance `talus`) slides to the lowest neighbour. */
static void thermal(float *h, const uint8_t *lock, f64 step, f64 talus,
                    int iterations, f64 rate) {
  const f64 limit = talus * step;
  for (int it = 0; it < iterations; ++it) {
    for (int j = 1; j < N - 1; ++j) {
      for (int i = 1; i < N - 1; ++i) {
        const size_t idx = (size_t)j * N + i;
        if (lock && lock[idx]) {
          continue;
        }
        size_t low = idx;
        f64 drop = 0;
        const int off[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4; ++k) {
          const size_t n = (size_t)(j + off[k][1]) * N + i + off[k][0];
          const f64 dd = h[idx] - h[n];
          if (dd > drop) {
            drop = dd;
            low = n;
          }
        }
        if (drop > limit && (!lock || !lock[low])) {
          const f64 move = (drop - limit) * 0.5 * rate;
          h[idx] -= (float)move;
          h[low] += (float)move;
        }
      }
    }
  }
}

/* Peaks roll off from `knee` to at most 200 m above it. */
static f64 soft_cap(f64 h, f64 knee) {
  return h < knee ? h : knee + 200.0 * tanh((h - knee) / 200.0);
}

/* A 3x3 blur blended by `amount`, to settle erosion's pits. */
static void settle(float *h, f64 amount) {
  float *copy = malloc(sizeof(float) * N * N);
  memcpy(copy, h, sizeof(float) * N * N);
  for (int j = 1; j < N - 1; ++j) {
    for (int i = 1; i < N - 1; ++i) {
      f64 sum = 0;
      for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
          sum += copy[(size_t)(j + dz) * N + i + dx];
        }
      }
      const size_t k = (size_t)j * N + i;
      h[k] = (float)lerp(copy[k], sum / 9.0, amount);
    }
  }
  free(copy);
}

/* Droplets that leave the grid carry their sediment away, so the outlet of
   a valley draining off an edge keeps deepening; nothing ends lower than
   `depth` under the uneroded surface. */
static void erosion_floor(float *h, const float *before, f64 depth) {
  for (size_t k = 0; k < (size_t)N * N; ++k) {
    h[k] = (float)fmax(h[k], before[k] - depth);
  }
}

/* ---------------------------------------------------------- weights */

/* Layer weights (grass, rock, scree, snow) from height above the valley
   floor, slope (rise/run), the slope's facing and settled sediment. */
static void weights(f64 x, f64 z, f64 h, f64 slope, f64 nz, f64 sediment,
                    f64 floor_h, uint8_t out[4]) {
  const f64 n1 = fbm(x / 70.0, z / 70.0, 3, 201);
  const f64 n2 = fbm(x / 18.0, z / 18.0, 2, 207);
  const f64 rel = h - floor_h + 60.0 * n1;
  /* Rock where too steep for soil, from gentler slopes high up. */
  const f64 high = sstep(650.0, 950.0, rel);
  const f64 rock =
      sstep(0.95 - 0.35 * high + 0.1 * n2, 1.3 - 0.35 * high + 0.1 * n2, slope);
  /* Snow above a line lower on north faces, sliding off sheer rock. */
  const f64 line = 860.0 - 140.0 * fmax(0.0, -nz) + 30.0 * n2;
  const f64 snow =
      sstep(line, line + 80.0, rel) * (1.0 - sstep(1.0, 1.5, slope));
  /* Scree: talus slopes, sediment fans and the alpine zone. */
  f64 scree = sstep(0.7, 0.95, slope) * (0.35 + 0.3 * n1);
  scree += sstep(3.0, 10.0, sediment) * 0.6;
  scree += sstep(560.0, 780.0, rel) * 0.9;
  scree = fmin(1.0, scree);
  const f64 w_snow = snow;
  const f64 w_rock = rock * (1.0 - w_snow);
  const f64 w_scree = scree * (1.0 - w_snow - w_rock);
  const f64 w_grass = fmax(0.0, 1.0 - w_snow - w_rock - w_scree);
  const f64 total = w_grass + w_rock + w_scree + w_snow + 1e-9;
  out[0] = (uint8_t)lround(255.0 * w_grass / total);
  out[1] = (uint8_t)lround(255.0 * w_rock / total);
  out[2] = (uint8_t)lround(255.0 * w_scree / total);
  out[3] = (uint8_t)lround(255.0 * w_snow / total);
}

/* ------------------------------------------------------------- PNG */

static void png_chunk(FILE *f, const char *type, const uint8_t *data,
                      uint32_t len) {
  uint8_t be[4] = {len >> 24, len >> 16, len >> 8, len};
  fwrite(be, 1, 4, f);
  fwrite(type, 1, 4, f);
  if (len) {
    fwrite(data, 1, len, f);
  }
  uLong crc = crc32(0, (const Bytef *)type, 4);
  crc = crc32(crc, data, len);
  uint8_t c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
  fwrite(c, 1, 4, f);
}

/* `bits` 8 or 16, `color` 0 gray, 2 RGB, 6 RGBA. */
static void png_write(const char *path, int w, int hgt, int bits, int color,
                      const uint8_t *pixels) {
  const int channels = color == 0 ? 1 : color == 2 ? 3 : 4;
  const size_t row = (size_t)w * channels * (bits / 8);
  uint8_t *raw = malloc((row + 1) * hgt);
  for (int j = 0; j < hgt; ++j) {
    raw[j * (row + 1)] = 0;
    memcpy(raw + j * (row + 1) + 1, pixels + j * row, row);
  }
  uLongf out_len = compressBound((uLong)((row + 1) * hgt));
  uint8_t *out = malloc(out_len);
  compress2(out, &out_len, raw, (uLong)((row + 1) * hgt), 6);
  FILE *f = fopen(path, "wb");
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13] = {
      w >> 24, w >> 16,       w >> 8,         w, hgt >> 24, hgt >> 16, hgt >> 8,
      hgt,     (uint8_t)bits, (uint8_t)color, 0, 0,         0};
  png_chunk(f, "IHDR", ihdr, 13);
  png_chunk(f, "IDAT", out, (uint32_t)out_len);
  png_chunk(f, "IEND", NULL, 0);
  fclose(f);
  free(raw);
  free(out);
}

static void write_heights(const char *path, const float *h, f64 lo, f64 hi) {
  uint8_t *px = malloc((size_t)N * N * 2);
  for (size_t k = 0; k < (size_t)N * N; ++k) {
    f64 v = (h[k] - lo) / (hi - lo);
    const uint16_t q = (uint16_t)lround(fmin(1.0, fmax(0.0, v)) * 65535.0);
    px[k * 2] = q >> 8;
    px[k * 2 + 1] = q & 0xFF;
  }
  png_write(path, N, N, 16, 0, px);
  free(px);
}

/* Shaded relief tinted by the layer weights. */
static void write_preview(const char *path, const float *h, const uint8_t *w,
                          f64 step) {
  uint8_t *px = malloc((size_t)N * N * 3);
  const f64 colors[4][3] = {
      {86, 112, 52}, {118, 112, 104}, {150, 140, 122}, {236, 240, 246}};
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const int i0 = i > 0 ? i - 1 : i, i1 = i < N - 1 ? i + 1 : i;
      const int j0 = j > 0 ? j - 1 : j, j1 = j < N - 1 ? j + 1 : j;
      const f64 gx =
          (h[(size_t)j * N + i1] - h[(size_t)j * N + i0]) / ((i1 - i0) * step);
      const f64 gz =
          (h[(size_t)j1 * N + i] - h[(size_t)j0 * N + i]) / ((j1 - j0) * step);
      f64 nx = -gx, ny = 1, nz = -gz;
      const f64 nl = sqrt(nx * nx + ny * ny + nz * nz);
      nx /= nl, ny /= nl, nz /= nl;
      const f64 shade = fmax(0.15, nx * -0.5 + ny * 0.7 + nz * 0.5) * 1.05;
      f64 c[3] = {0, 0, 0};
      for (int l = 0; l < 4; ++l) {
        const f64 t = w[((size_t)j * N + i) * 4 + l] / 255.0;
        for (int k = 0; k < 3; ++k) {
          c[k] += colors[l][k] * t;
        }
      }
      /* North (+z) at the top of the image. */
      uint8_t *o = px + ((size_t)(N - 1 - j) * N + i) * 3;
      for (int k = 0; k < 3; ++k) {
        o[k] = (uint8_t)fmin(255.0, c[k] * shade);
      }
    }
  }
  png_write(path, N, N, 8, 2, px);
  free(px);
}

/* Weights for every sample of a grid. */
static void grid_weights(const Grid *g, uint8_t *w) {
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const int i0 = i > 0 ? i - 1 : i, i1 = i < N - 1 ? i + 1 : i;
      const int j0 = j > 0 ? j - 1 : j, j1 = j < N - 1 ? j + 1 : j;
      const f64 gx = (g->h[(size_t)j * N + i1] - g->h[(size_t)j * N + i0]) /
                     ((i1 - i0) * g->step);
      const f64 gz = (g->h[(size_t)j1 * N + i] - g->h[(size_t)j0 * N + i]) /
                     ((j1 - j0) * g->step);
      const f64 slope = sqrt(gx * gx + gz * gz);
      const f64 nz = -gz / sqrt(1 + slope * slope);
      const f64 x = g->x0 + i * g->step;
      const f64 z = g->z0 + j * g->step;
      const f64 sediment =
          g->deposit[(size_t)j * N + i] / (g->step * g->step) * 4.0;
      weights(x, z, g->h[(size_t)j * N + i], slope, nz, sediment,
              (isnan(g_valley_floor) ? g_min + 50.0 : g_valley_floor),
              w + ((size_t)j * N + i) * 4);
    }
  }
}

static int arg_box(int argc, char **argv, int i, Box *out) {
  if (i + 5 >= argc) {
    return 0;
  }
  *out = (Box){atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]),
               atof(argv[i + 4]), atof(argv[i + 5])};
  return 1;
}

int main(int argc, char **argv) {
  const char *dir = ".";
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!strcmp(a, "--out") && i + 1 < argc) {
      dir = argv[++i];
    } else if (!strcmp(a, "--center") && i + 2 < argc) {
      g_cx = atof(argv[++i]);
      g_cz = atof(argv[++i]);
    } else if (!strcmp(a, "--size") && i + 1 < argc) {
      g_size = atof(argv[++i]);
    } else if (!strcmp(a, "--min") && i + 1 < argc) {
      g_min = atof(argv[++i]);
    } else if (!strcmp(a, "--max") && i + 1 < argc) {
      g_max = atof(argv[++i]);
    } else if (!strcmp(a, "--back-size") && i + 1 < argc) {
      g_back_size = atof(argv[++i]);
    } else if (!strcmp(a, "--back-min") && i + 1 < argc) {
      g_back_min = atof(argv[++i]);
    } else if (!strcmp(a, "--back-max") && i + 1 < argc) {
      g_back_max = atof(argv[++i]);
    } else if (!strcmp(a, "--valley") && i + 1 < argc) {
      g_valley_x = atof(argv[++i]);
    } else if (!strcmp(a, "--valley-floor") && i + 1 < argc) {
      g_valley_floor = atof(argv[++i]);
    } else if (!strcmp(a, "--seed") && i + 1 < argc) {
      g_seed = (uint32_t)atoi(argv[++i]);
    } else if (!strcmp(a, "--floor") && g_floor_count < BOX_MAX &&
               arg_box(argc, argv, i, &g_floors[g_floor_count])) {
      g_floor_count++;
      i += 5;
    } else if (!strcmp(a, "--cap") && g_cap_count < BOX_MAX &&
               arg_box(argc, argv, i, &g_caps[g_cap_count])) {
      g_cap_count++;
      i += 5;
    } else {
      fprintf(stderr, "unknown or incomplete option %s (see the header)\n", a);
      return 2;
    }
  }
  if (!(g_size > 0.0) || !(g_max > g_min)) {
    fprintf(stderr, "--size must be positive and --max above --min\n");
    return 2;
  }
  if (g_back_size <= 0.0) {
    g_back_size = 8.0 * g_size;
  }
  if (isnan(g_valley_floor)) {
    g_valley_floor = g_min + 50.0;
  }
  const f64 main_step = g_size / (N - 1);
  const f64 back_step = g_back_size / (N - 1);
  const size_t count = (size_t)N * N;
  char path[1024];

  Grid back = {g_cx - g_back_size / 2, g_cz - g_back_size / 2, back_step,
               calloc(count, sizeof(float)), calloc(count, sizeof(float))};
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      back.h[(size_t)j * N + i] = (float)world_height(
          back.x0 + i * back.step, back.z0 + j * back.step, 0);
    }
  }
  float *before = malloc(count * sizeof(float));
  memcpy(before, back.h, count * sizeof(float));
  erode(back.h, back.deposit, NULL, back.step, 350000, 2, 1.0);
  thermal(back.h, NULL, back.step, 1.6, 6, 0.5);
  settle(back.h, 0.3);
  erosion_floor(back.h, before, 60.0);
  const f64 knee = g_max - 220.0;
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const f64 x = back.x0 + i * back.step;
      const f64 z = back.z0 + j * back.step;
      const size_t k = (size_t)j * N + i;
      const f64 ox = fmax(fabs(x - g_cx) - g_size / 2, 0.0);
      const f64 oz = fmax(fabs(z - g_cz) - g_size / 2, 0.0);
      /* Near the main terrain the backdrop's peaks keep under its maximum,
         relaxing over 2 km. */
      back.h[k] = (float)soft_cap(valley_carve(x, z, back.h[k]),
                                  knee + 0.9 * sqrt(ox * ox + oz * oz));
    }
  }

  Grid main_grid = {g_cx - g_size / 2, g_cz - g_size / 2, main_step,
                    calloc(count, sizeof(float)), calloc(count, sizeof(float))};
  float *base = calloc(count, sizeof(float));
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const f64 x = main_grid.x0 + i * main_grid.step;
      const f64 z = main_grid.z0 + j * main_grid.step;
      const f64 coarse = grid_cubic(&back, back.h, x, z);
      base[(size_t)j * N + i] = (float)coarse;
      main_grid.h[(size_t)j * N + i] =
          (float)(coarse + world_height(x, z, 1) - world_height(x, z, 0));
    }
  }
  memcpy(before, main_grid.h, count * sizeof(float));
  erode(main_grid.h, main_grid.deposit, NULL, main_grid.step, 700000, 3, 0.5);
  thermal(main_grid.h, NULL, main_grid.step, 2.2, 4, 0.5);
  settle(main_grid.h, 0.5);
  erosion_floor(main_grid.h, before, 12.0);
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const size_t k = (size_t)j * N + i;
      const f64 x = main_grid.x0 + i * main_grid.step;
      const f64 z = main_grid.z0 + j * main_grid.step;
      f64 h =
          constrain(x, z, soft_cap(valley_carve(x, z, main_grid.h[k]), knee));
      /* The last 96 m reach the backdrop's own surface. */
      const int edge = i < j ? i : j;
      const int edge2 = (N - 1 - i) < (N - 1 - j) ? (N - 1 - i) : (N - 1 - j);
      const f64 border = (edge < edge2 ? edge : edge2) * main_grid.step;
      h = lerp(base[k], h, sstep(0.0, 96.0, border));
      main_grid.h[k] = (float)fmin(g_max - 1.0, fmax(g_min + 1.0, h));
    }
  }
  /* The backdrop equals the main terrain at its border and lies at least
     8 m under it one backdrop sample inside, so the two never fight. */
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const f64 x = back.x0 + i * back.step;
      const f64 z = back.z0 + j * back.step;
      const f64 inside =
          fmin(g_size / 2 - fabs(x - g_cx), g_size / 2 - fabs(z - g_cz));
      const size_t k = (size_t)j * N + i;
      if (inside > 1e-6) {
        back.h[k] = (float)(grid_sample(&main_grid, main_grid.h, x, z) -
                            fmin(60.0, 4.0 + 0.25 * fmax(inside, back.step)));
      } else if (inside > -1e-6) {
        back.h[k] = (float)grid_sample(&main_grid, main_grid.h, x, z);
      }
      back.h[k] =
          (float)fmin(g_back_max - 1.0, fmax(g_back_min + 1.0, back.h[k]));
    }
  }

  uint8_t *main_w = malloc(count * 4);
  uint8_t *back_w = malloc(count * 4);
  grid_weights(&main_grid, main_w);
  grid_weights(&back, back_w);
  snprintf(path, sizeof(path), "%s/main_h.png", dir);
  write_heights(path, main_grid.h, g_min, g_max);
  snprintf(path, sizeof(path), "%s/main_w.png", dir);
  png_write(path, N, N, 8, 6, main_w);
  snprintf(path, sizeof(path), "%s/main_preview.png", dir);
  write_preview(path, main_grid.h, main_w, main_grid.step);
  snprintf(path, sizeof(path), "%s/back_h.png", dir);
  write_heights(path, back.h, g_back_min, g_back_max);
  snprintf(path, sizeof(path), "%s/back_w.png", dir);
  png_write(path, N, N, 8, 6, back_w);
  snprintf(path, sizeof(path), "%s/back_preview.png", dir);
  write_preview(path, back.h, back_w, back.step);
  snprintf(path, sizeof(path), "%s/main_h.f32", dir);
  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", path);
    return 1;
  }
  fwrite(main_grid.h, sizeof(float), count, f);
  fclose(f);
  printf("main %.0f m at %.3f m: stamp center [%g, %g, %g] size %g "
         "height_scale %g\nbackdrop %.0f m at %.3f m: stamp center [%g, %g, "
         "%g] size %g height_scale %g\n",
         g_size, main_step, g_cx, g_min, g_cz, g_size, g_max - g_min,
         g_back_size, back_step, g_cx, g_back_min, g_cz, g_back_size,
         g_back_max - g_back_min);
  return 0;
}

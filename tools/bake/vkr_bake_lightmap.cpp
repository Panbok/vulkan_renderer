#include "bake/vkr_bake_lightmap.h"

#include <ktx-software/external/astc-encoder/Source/astcenc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>
#include <system_error>
#include <thread>

namespace {

uint32_t round_up_to_block(float32_t texels) {
  const uint32_t whole = static_cast<uint32_t>(std::ceil(texels));
  return std::max(4u, (whole + 3u) & ~3u);
}

/* The instance's uniform world scale: the cube root of its linear part's
   absolute determinant. */
float32_t instance_scale(const Mat4 &world) {
  const float32_t *m = world.elements;
  const float64_t determinant =
      (float64_t)m[0] * ((float64_t)m[5] * m[10] - (float64_t)m[6] * m[9]) -
      (float64_t)m[4] * ((float64_t)m[1] * m[10] - (float64_t)m[2] * m[9]) +
      (float64_t)m[8] * ((float64_t)m[1] * m[6] - (float64_t)m[2] * m[5]);
  return static_cast<float32_t>(std::cbrt(std::fabs(determinant)));
}

float32_t edge(float32_t ax, float32_t ay, float32_t bx, float32_t by,
               float32_t px, float32_t py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

float64_t luminance(Vec3 value) {
  return 0.2126 * value.x + 0.7152 * value.y + 0.0722 * value.z;
}

} // namespace

bool vkr_bake_lightmap_pack(
    const std::vector<VkrBakeLightmapInstance> &instances, uint32_t page_size,
    float32_t texels_per_world_unit, VkrBakeLightmapLayout *out_layout) {
  if (!out_layout || page_size == 0u || page_size % 4u != 0u ||
      !std::isfinite(texels_per_world_unit) || texels_per_world_unit <= 0.0f) {
    return false;
  }
  try {
    VkrBakeLightmapLayout layout;
    layout.page_size = page_size;
    uint32_t instance_end = 0u;
    for (const VkrBakeLightmapInstance &instance : instances) {
      instance_end =
          std::max(instance_end, instance.source_instance_index + 1u);
    }
    layout.rect_by_instance.assign(instance_end, UINT32_MAX);

    for (const VkrBakeLightmapInstance &instance : instances) {
      if (instance.atlas_width == 0u || instance.atlas_height == 0u) {
        continue;
      }
      if (!(instance.texels_per_unit > 0.0f)) {
        return false;
      }
      const float32_t scale = instance_scale(instance.world) *
                              texels_per_world_unit / instance.texels_per_unit;
      if (!std::isfinite(scale) || scale <= 0.0f) {
        return false;
      }
      uint32_t width =
          round_up_to_block((float32_t)instance.atlas_width * scale);
      uint32_t height =
          round_up_to_block((float32_t)instance.atlas_height * scale);
      if (width > page_size || height > page_size) {
        const float32_t fit =
            (float32_t)page_size / (float32_t)std::max(width, height);
        width = std::max(4u, static_cast<uint32_t>(width * fit) & ~3u);
        height = std::max(4u, static_cast<uint32_t>(height * fit) & ~3u);
      }
      VkrBakeLightmapRect rect;
      rect.source_instance_index = instance.source_instance_index;
      rect.width = width;
      rect.height = height;
      layout.rects.push_back(rect);
    }

    /* Tallest first keeps shelves full; ties break on width, then instance,
       so the layout is deterministic. */
    std::vector<uint32_t> order(layout.rects.size());
    for (uint32_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const VkrBakeLightmapRect &ra = layout.rects[a];
      const VkrBakeLightmapRect &rb = layout.rects[b];
      if (ra.height != rb.height)
        return ra.height > rb.height;
      if (ra.width != rb.width)
        return ra.width > rb.width;
      return ra.source_instance_index < rb.source_instance_index;
    });
    uint32_t page = 0u;
    uint32_t shelf_y = 0u;
    uint32_t shelf_height = 0u;
    uint32_t x = 0u;
    for (uint32_t index : order) {
      VkrBakeLightmapRect &rect = layout.rects[index];
      if (x + rect.width > page_size) {
        shelf_y += shelf_height;
        shelf_height = 0u;
        x = 0u;
      }
      if (shelf_y + rect.height > page_size) {
        ++page;
        shelf_y = 0u;
        shelf_height = 0u;
        x = 0u;
      }
      rect.page = page;
      rect.x = x;
      rect.y = shelf_y;
      x += rect.width;
      shelf_height = std::max(shelf_height, rect.height);
      layout.rect_by_instance[rect.source_instance_index] = index;
    }
    layout.page_count = layout.rects.empty() ? 0u : page + 1u;
    *out_layout = std::move(layout);
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool vkr_bake_lightmap_pack_fitted(
    const std::vector<VkrBakeLightmapInstance> &instances, uint32_t page_size,
    float32_t texels_per_world_unit, VkrBakeLightmapLayout *out_layout) {
  VkrBakeLightmapLayout full;
  if (!vkr_bake_lightmap_pack(instances, page_size, texels_per_world_unit,
                              &full)) {
    return false;
  }
  if (full.page_count == 1u) {
    uint32_t largest = 0u;
    for (const VkrBakeLightmapRect &rect : full.rects) {
      largest = std::max(largest, std::max(rect.width, rect.height));
    }
    for (uint32_t size = 256u; size < page_size; size *= 2u) {
      if (size < largest) {
        continue;
      }
      VkrBakeLightmapLayout fitted;
      if (!vkr_bake_lightmap_pack(instances, size, texels_per_world_unit,
                                  &fitted)) {
        return false;
      }
      if (fitted.page_count == 1u) {
        *out_layout = std::move(fitted);
        return true;
      }
    }
  }
  *out_layout = std::move(full);
  return true;
}

bool vkr_bake_lightmap_rasterize_page(
    const VkrBakeTriangle *triangles, uint32_t triangle_count,
    const VkrBakeLightmapLayout &layout, uint32_t page,
    std::vector<VkrBakeLightmapTexel> *out_texels) {
  if ((!triangles && triangle_count) || !out_texels ||
      page >= layout.page_count) {
    return false;
  }
  try {
    const uint32_t size = layout.page_size;
    std::vector<uint8_t> claimed((size_t)size * size, 0u);
    std::vector<VkrBakeLightmapTexel> texels;
    for (uint32_t t = 0; t < triangle_count; ++t) {
      const VkrBakeTriangle &triangle = triangles[t];
      const uint32_t instance = triangle.source_instance_index;
      if (instance >= layout.rect_by_instance.size() ||
          layout.rect_by_instance[instance] == UINT32_MAX) {
        continue;
      }
      const VkrBakeLightmapRect &rect =
          layout.rects[layout.rect_by_instance[instance]];
      if (rect.page != page) {
        continue;
      }
      float32_t px[3];
      float32_t py[3];
      for (uint32_t c = 0; c < 3u; ++c) {
        px[c] = (float32_t)rect.x +
                triangle.vertex[c].lightmap_uv.x * (float32_t)rect.width;
        py[c] = (float32_t)rect.y +
                triangle.vertex[c].lightmap_uv.y * (float32_t)rect.height;
      }
      const float32_t area = edge(px[0], py[0], px[1], py[1], px[2], py[2]);
      if (!std::isfinite(area) || std::fabs(area) < 1.0e-8f) {
        continue;
      }
      const uint32_t min_x = (uint32_t)std::max(
          (float32_t)rect.x, std::floor(std::min({px[0], px[1], px[2]})));
      const uint32_t min_y = (uint32_t)std::max(
          (float32_t)rect.y, std::floor(std::min({py[0], py[1], py[2]})));
      const uint32_t max_x =
          (uint32_t)std::min((float32_t)(rect.x + rect.width - 1u),
                             std::ceil(std::max({px[0], px[1], px[2]})));
      const uint32_t max_y =
          (uint32_t)std::min((float32_t)(rect.y + rect.height - 1u),
                             std::ceil(std::max({py[0], py[1], py[2]})));
      const float32_t inverse_area = 1.0f / area;
      for (uint32_t y = min_y; y <= max_y; ++y) {
        for (uint32_t x = min_x; x <= max_x; ++x) {
          const float32_t cx = (float32_t)x + 0.5f;
          const float32_t cy = (float32_t)y + 0.5f;
          const float32_t w0 =
              edge(px[1], py[1], px[2], py[2], cx, cy) * inverse_area;
          const float32_t w1 =
              edge(px[2], py[2], px[0], py[0], cx, cy) * inverse_area;
          const float32_t w2 = 1.0f - w0 - w1;
          if (w0 < -1.0e-6f || w1 < -1.0e-6f || w2 < -1.0e-6f) {
            continue;
          }
          uint8_t &owner = claimed[(size_t)y * size + x];
          if (owner) {
            continue;
          }
          owner = 1u;
          VkrBakeLightmapTexel texel;
          texel.x = x;
          texel.y = y;
          texel.position =
              vec3_add(vec3_add(vec3_scale(triangle.vertex[0].position, w0),
                                vec3_scale(triangle.vertex[1].position, w1)),
                       vec3_scale(triangle.vertex[2].position, w2));
          texel.normal = vec3_normalize(
              vec3_add(vec3_add(vec3_scale(triangle.vertex[0].normal, w0),
                                vec3_scale(triangle.vertex[1].normal, w1)),
                       vec3_scale(triangle.vertex[2].normal, w2)));
          texel.triangle_index = t;
          texel.source_instance_index = instance;
          texels.push_back(texel);
        }
      }
    }
    std::sort(texels.begin(), texels.end(),
              [](const VkrBakeLightmapTexel &a, const VkrBakeLightmapTexel &b) {
                return a.y != b.y ? a.y < b.y : a.x < b.x;
              });
    *out_texels = std::move(texels);
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool vkr_bake_lightmap_neighbors(
    uint32_t page_size, const std::vector<VkrBakeLightmapTexel> &texels,
    float32_t max_distance, VkrBakeLightmapNeighbors *out_neighbors) {
  if (!out_neighbors || page_size == 0u || !std::isfinite(max_distance) ||
      max_distance <= 0.0f || texels.size() >= UINT32_MAX) {
    return false;
  }
  /* About 37 degrees. */
  constexpr float32_t kMinNormalCosine = 0.8f;
  try {
    const size_t size = page_size;
    std::vector<uint32_t> at(size * size, UINT32_MAX);
    for (size_t i = 0u; i < texels.size(); ++i) {
      at[(size_t)texels[i].y * size + texels[i].x] = (uint32_t)i;
    }
    out_neighbors->first.assign(texels.size() + 1u, 0u);
    out_neighbors->indices.clear();
    out_neighbors->indices.reserve(texels.size() * 8u);
    const float32_t max_distance_squared = max_distance * max_distance;
    for (size_t i = 0u; i < texels.size(); ++i) {
      const VkrBakeLightmapTexel &texel = texels[i];
      out_neighbors->first[i] = (uint32_t)out_neighbors->indices.size();
      for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
          const int64_t x = (int64_t)texel.x + dx;
          const int64_t y = (int64_t)texel.y + dy;
          if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= (int64_t)size ||
              y >= (int64_t)size) {
            continue;
          }
          const uint32_t j = at[(size_t)y * size + (size_t)x];
          if (j == UINT32_MAX) {
            continue;
          }
          const VkrBakeLightmapTexel &other = texels[j];
          const Vec3 delta = vec3_sub(other.position, texel.position);
          if (vec3_dot(delta, delta) > max_distance_squared ||
              vec3_dot(other.normal, texel.normal) < kMinNormalCosine) {
            continue;
          }
          out_neighbors->indices.push_back(j);
        }
      }
    }
    out_neighbors->first[texels.size()] =
        (uint32_t)out_neighbors->indices.size();
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

VkrBakeLightmapOutliers
vkr_bake_lightmap_reject_outliers(const VkrBakeLightmapNeighbors &neighbors,
                                  float32_t ratio, uint32_t min_neighbors,
                                  bool replace, std::vector<Vec3> *values) {
  VkrBakeLightmapOutliers result;
  if (!values || neighbors.first.size() != values->size() + 1u) {
    return result;
  }
  const std::vector<Vec3> source = *values;
  float64_t total = 0.0;
  float64_t rejected = 0.0;
  for (size_t i = 0u; i < source.size(); ++i) {
    const float64_t y = luminance(source[i]);
    total += y;
    const uint32_t begin = neighbors.first[i];
    const uint32_t end = neighbors.first[i + 1u];
    if (end - begin < min_neighbors) {
      continue;
    }
    float64_t brightest = 0.0;
    Vec3 sum = vec3_zero();
    for (uint32_t n = begin; n < end; ++n) {
      const Vec3 value = source[neighbors.indices[n]];
      brightest = std::max(brightest, luminance(value));
      sum = vec3_add(sum, value);
    }
    if (!(y > ratio * brightest)) {
      continue;
    }
    ++result.texels;
    rejected += y;
    if (replace) {
      (*values)[i] = vec3_scale(sum, 1.0f / (float32_t)(end - begin));
    }
  }
  result.energy_fraction = total > 0.0 ? rejected / total : 0.0;
  return result;
}

bool vkr_bake_lightmap_smooth(const VkrBakeLightmapNeighbors &neighbors,
                              const std::vector<VkrBakeLightmapTexel> &texels,
                              uint32_t passes, std::vector<Vec3> *values) {
  if (!values || values->size() != texels.size() ||
      neighbors.first.size() != texels.size() + 1u) {
    return false;
  }
  try {
    std::vector<Vec3> source;
    for (uint32_t pass = 0u; pass < passes; ++pass) {
      source = *values;
      for (size_t i = 0u; i < texels.size(); ++i) {
        Vec3 sum = vec3_scale(source[i], 4.0f);
        float32_t weight = 4.0f;
        for (uint32_t n = neighbors.first[i]; n < neighbors.first[i + 1u];
             ++n) {
          const uint32_t j = neighbors.indices[n];
          const bool edge =
              texels[j].x == texels[i].x || texels[j].y == texels[i].y;
          const float32_t w = edge ? 2.0f : 1.0f;
          sum = vec3_add(sum, vec3_scale(source[j], w));
          weight += w;
        }
        (*values)[i] = vec3_scale(sum, 1.0f / weight);
      }
    }
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool vkr_bake_lightmap_plan_fill(const VkrBakeLightmapNeighbors &neighbors,
                                 const std::vector<uint8_t> &valid,
                                 VkrBakeLightmapFill *out_fill) {
  if (!out_fill || neighbors.first.size() != valid.size() + 1u) {
    return false;
  }
  try {
    VkrBakeLightmapFill fill;
    /* The ring that gave each texel its value: zero for a valid texel. */
    std::vector<uint32_t> ring(valid.size(), UINT32_MAX);
    std::vector<uint32_t> pending;
    for (size_t i = 0u; i < valid.size(); ++i) {
      if (valid[i]) {
        ring[i] = 0u;
      } else {
        pending.push_back((uint32_t)i);
      }
    }
    fill.first.push_back(0u);
    std::vector<uint32_t> still_pending;
    for (uint32_t current = 1u; !pending.empty(); ++current) {
      still_pending.clear();
      for (uint32_t i : pending) {
        const size_t sources_before = fill.sources.size();
        for (uint32_t n = neighbors.first[i]; n < neighbors.first[i + 1u];
             ++n) {
          const uint32_t j = neighbors.indices[n];
          if (ring[j] < current) {
            fill.sources.push_back(j);
          }
        }
        if (fill.sources.size() == sources_before) {
          still_pending.push_back(i);
          continue;
        }
        ring[i] = current;
        fill.texels.push_back(i);
        fill.first.push_back((uint32_t)fill.sources.size());
      }
      if (still_pending.size() == pending.size()) {
        break;
      }
      pending.swap(still_pending);
    }
    fill.unfilled = pending.size();
    *out_fill = std::move(fill);
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

namespace {

template <typename Value, typename Add, typename Scale>
void apply_fill(const VkrBakeLightmapFill &fill, std::vector<Value> *values,
                Value zero, Add add, Scale scale) {
  for (size_t k = 0u; k < fill.texels.size(); ++k) {
    Value sum = zero;
    for (uint32_t n = fill.first[k]; n < fill.first[k + 1u]; ++n) {
      sum = add(sum, (*values)[fill.sources[n]]);
    }
    (*values)[fill.texels[k]] =
        scale(sum, 1.0f / (float32_t)(fill.first[k + 1u] - fill.first[k]));
  }
}

} // namespace

void vkr_bake_lightmap_apply_fill(const VkrBakeLightmapFill &fill,
                                  std::vector<Vec3> *values) {
  apply_fill(fill, values, vec3_zero(), vec3_add, vec3_scale);
}

void vkr_bake_lightmap_apply_fill(const VkrBakeLightmapFill &fill,
                                  std::vector<float32_t> *values) {
  apply_fill(
      fill, values, 0.0f, [](float32_t a, float32_t b) { return a + b; },
      [](float32_t a, float32_t s) { return a * s; });
}

namespace {

/* Normals closer than about 37 degrees face the same way, as
   vkr_bake_lightmap_neighbors counts them. */
constexpr float32_t kSameSurfaceCosine = 0.8f;
/* A tap's world distance may exceed the one its page offset implies by this
   factor plus kSeamSlackTexels texel spacings before it counts as across a
   seam. */
constexpr float32_t kSeamDistanceFactor = 1.5f;
constexpr float32_t kSeamSlackTexels = 0.5f;
/* B3-spline weights of the 5-tap a-trous kernel. */
constexpr float32_t kAtrousWeights[3] = {3.0f / 8.0f, 1.0f / 4.0f,
                                         1.0f / 16.0f};
constexpr float32_t kVarianceWeights[2] = {1.0f / 2.0f, 1.0f / 4.0f};

/* Runs work(begin, end) over [0, count) on `threads` workers. */
template <typename Work>
void parallel_for(size_t count, uint32_t threads, Work work) {
  const size_t workers =
      std::max<size_t>(1u, std::min<size_t>(threads, (count + 4095u) / 4096u));
  if (workers == 1u) {
    work((size_t)0u, count);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (size_t w = 0u; w < workers; ++w) {
    const size_t begin = count * w / workers;
    const size_t end = count * (w + 1u) / workers;
    pool.emplace_back([&work, begin, end] { work(begin, end); });
  }
  for (std::thread &thread : pool) {
    thread.join();
  }
}

struct DenoisePage {
  const std::vector<VkrBakeLightmapTexel> *texels;
  const std::vector<uint8_t> *valid;
  /* Texel index per page texel, UINT32_MAX where none. */
  std::vector<uint32_t> at;
  /* Each texel's world distance to its same-surface edge neighbors. */
  std::vector<float32_t> spacing;
  int64_t size;
};

/* The texel at page offset (dx, dy) * step from texel i that may contribute
   to it, or UINT32_MAX; *out_cosine is their normals' cosine and
   *out_plane its distance from texel i's tangent plane. */
uint32_t denoise_tap(const DenoisePage &page, uint32_t i, int32_t dx,
                     int32_t dy, int32_t step, float32_t *out_cosine,
                     float32_t *out_plane) {
  const VkrBakeLightmapTexel &center = (*page.texels)[i];
  const int64_t x = (int64_t)center.x + (int64_t)dx * step;
  const int64_t y = (int64_t)center.y + (int64_t)dy * step;
  if (x < 0 || y < 0 || x >= page.size || y >= page.size) {
    return UINT32_MAX;
  }
  const uint32_t j = page.at[(size_t)y * (size_t)page.size + (size_t)x];
  if (j == UINT32_MAX || (page.valid && !(*page.valid)[j])) {
    return UINT32_MAX;
  }
  const VkrBakeLightmapTexel &tap = (*page.texels)[j];
  if (tap.source_instance_index != center.source_instance_index) {
    return UINT32_MAX;
  }
  const float32_t cosine = vec3_dot(tap.normal, center.normal);
  if (cosine < kSameSurfaceCosine) {
    return UINT32_MAX;
  }
  const Vec3 delta = vec3_sub(tap.position, center.position);
  const float32_t offset =
      (float32_t)step * std::sqrt((float32_t)(dx * dx + dy * dy));
  const float32_t spacing = page.spacing[i];
  const float32_t limit =
      (kSeamDistanceFactor * offset + kSeamSlackTexels) * spacing;
  if (vec3_dot(delta, delta) > limit * limit) {
    return UINT32_MAX;
  }
  *out_cosine = cosine;
  *out_plane = std::fabs(vec3_dot(delta, center.normal));
  return j;
}

} // namespace

bool vkr_bake_lightmap_denoise(uint32_t page_size,
                               const std::vector<VkrBakeLightmapTexel> &texels,
                               const std::vector<uint8_t> *valid,
                               const std::vector<float32_t> *variance,
                               const VkrBakeLightmapDenoiseSettings &settings,
                               std::vector<Vec3> *values) {
  if (!values || values->size() != texels.size() || page_size == 0u ||
      texels.size() >= UINT32_MAX ||
      (valid && valid->size() != texels.size()) ||
      (variance && variance->size() != texels.size()) ||
      !std::isfinite(settings.texels_per_unit) ||
      !(settings.texels_per_unit > 0.0f) ||
      !std::isfinite(settings.luminance_sigma) ||
      !(settings.luminance_sigma > 0.0f) || settings.iterations > 16u) {
    return false;
  }
  const uint32_t threads =
      settings.threads ? settings.threads
                       : std::max(1u, std::thread::hardware_concurrency());
  try {
    const size_t count = texels.size();
    DenoisePage page;
    page.texels = &texels;
    page.valid = valid;
    page.size = page_size;
    page.at.assign((size_t)page_size * page_size, UINT32_MAX);
    for (size_t i = 0u; i < count; ++i) {
      page.at[(size_t)texels[i].y * page_size + texels[i].x] = (uint32_t)i;
    }

    /* A texel's spacing is the mean distance to its same-instance,
       same-facing edge neighbors no farther than twice the nearest, so a
       neighbor across a seam does not stretch it. */
    page.spacing.assign(count, 1.0f / settings.texels_per_unit);
    static const int32_t kEdges[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (size_t i = 0u; i < count; ++i) {
      const VkrBakeLightmapTexel &texel = texels[i];
      float32_t distances[4];
      uint32_t found = 0u;
      float32_t nearest = INFINITY;
      for (const int32_t *edge : kEdges) {
        const int64_t x = (int64_t)texel.x + edge[0];
        const int64_t y = (int64_t)texel.y + edge[1];
        if (x < 0 || y < 0 || x >= page.size || y >= page.size) {
          continue;
        }
        const uint32_t j = page.at[(size_t)y * page_size + (size_t)x];
        if (j == UINT32_MAX ||
            texels[j].source_instance_index != texel.source_instance_index ||
            vec3_dot(texels[j].normal, texel.normal) < kSameSurfaceCosine) {
          continue;
        }
        const float32_t distance =
            vec3_length(vec3_sub(texels[j].position, texel.position));
        if (distance > 0.0f && std::isfinite(distance)) {
          distances[found++] = distance;
          nearest = std::min(nearest, distance);
        }
      }
      float32_t sum = 0.0f;
      uint32_t used = 0u;
      for (uint32_t k = 0u; k < found; ++k) {
        if (distances[k] <= 2.0f * nearest) {
          sum += distances[k];
          ++used;
        }
      }
      if (used != 0u) {
        page.spacing[i] = sum / (float32_t)used;
      }
    }

    std::vector<float32_t> current_variance;
    if (variance) {
      current_variance = *variance;
    }
    std::vector<float32_t> filtered_variance(variance ? count : 0u);
    std::vector<float32_t> next_variance(variance ? count : 0u);
    std::vector<Vec3> source;
    for (uint32_t iteration = 0u; iteration < settings.iterations;
         ++iteration) {
      const int32_t step = 1 << iteration;
      source = *values;
      /* The center's variance for its luminance weight, smoothed over its
         3x3 same-surface neighbors so one sample's luck does not decide. */
      if (variance) {
        parallel_for(count, threads, [&](size_t begin, size_t end) {
          for (size_t i = begin; i < end; ++i) {
            float32_t sum = 0.0f;
            float32_t weight = 0.0f;
            for (int32_t dy = -1; dy <= 1; ++dy) {
              for (int32_t dx = -1; dx <= 1; ++dx) {
                float32_t cosine = 1.0f;
                float32_t plane = 0.0f;
                const uint32_t j = dx == 0 && dy == 0
                                       ? (uint32_t)i
                                       : denoise_tap(page, (uint32_t)i, dx, dy,
                                                     1, &cosine, &plane);
                if (j == UINT32_MAX) {
                  continue;
                }
                const float32_t w = kVarianceWeights[std::abs(dx)] *
                                    kVarianceWeights[std::abs(dy)];
                sum += w * current_variance[j];
                weight += w;
              }
            }
            filtered_variance[i] = sum / weight;
          }
        });
      }
      parallel_for(count, threads, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
          const float32_t center_luminance = (float32_t)luminance(source[i]);
          const float32_t spacing = page.spacing[i];
          Vec3 sum = vec3_zero();
          float32_t weight = 0.0f;
          float32_t variance_sum = 0.0f;
          for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
              float32_t cosine = 1.0f;
              float32_t plane = 0.0f;
              uint32_t j = UINT32_MAX;
              if (dx == 0 && dy == 0) {
                if (!valid || (*valid)[i]) {
                  j = (uint32_t)i;
                }
              } else {
                j = denoise_tap(page, (uint32_t)i, dx, dy, step, &cosine,
                                &plane);
              }
              if (j == UINT32_MAX) {
                continue;
              }
              float32_t w =
                  kAtrousWeights[std::abs(dx)] * kAtrousWeights[std::abs(dy)];
              if (j != i) {
                const float32_t c2 = cosine * cosine;
                const float32_t c4 = c2 * c2;
                const float32_t c8 = c4 * c4;
                w *= c8 * c8 * std::exp(-plane / spacing);
                if (variance) {
                  /* Both texels' noise widens the tolerance, so a tap that
                     caught a rare bright path counts at a texel that did
                     not; a one-sided tolerance darkens the result. */
                  const float32_t difference = std::fabs(
                      (float32_t)luminance(source[j]) - center_luminance);
                  const float32_t sigma = settings.luminance_sigma *
                                              std::sqrt(filtered_variance[i] +
                                                        filtered_variance[j]) +
                                          1.0e-10f;
                  w *= std::exp(-difference / sigma);
                }
              }
              sum = vec3_add(sum, vec3_scale(source[j], w));
              weight += w;
              if (variance) {
                variance_sum += w * w * current_variance[j];
              }
            }
          }
          if (weight > 0.0f) {
            (*values)[i] = vec3_scale(sum, 1.0f / weight);
            if (variance) {
              next_variance[i] = variance_sum / (weight * weight);
            }
          } else if (variance) {
            next_variance[i] = current_variance[i];
          }
        }
      });
      if (variance) {
        current_variance.swap(next_variance);
      }
    }
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  } catch (const std::system_error &) {
    return false;
  }
}

bool vkr_bake_lightmap_compose_page(
    const VkrBakeLightmapLayout &layout, uint32_t page,
    const std::vector<VkrBakeLightmapTexel> &texels,
    const std::vector<Vec3> &values, const std::vector<float32_t> *occlusion,
    uint32_t dilation_passes, std::vector<float32_t> *out_rgba) {
  if (!out_rgba || page >= layout.page_count ||
      texels.size() != values.size() ||
      (occlusion && occlusion->size() != texels.size())) {
    return false;
  }
  try {
    const size_t size = layout.page_size;
    out_rgba->assign(size * size * 4u, 0.0f);
    float32_t *rgba = out_rgba->data();
    for (size_t i = 0u; i < size * size; ++i) {
      rgba[4u * i + 3u] = 1.0f;
    }
    /* Zero is empty, one covered, and 2 + pass a texel filled in that pass,
       so a pass reads only texels filled before it. */
    std::vector<uint8_t> state(size * size, 0u);
    for (size_t i = 0u; i < texels.size(); ++i) {
      const size_t index = (size_t)texels[i].y * size + texels[i].x;
      rgba[4u * index + 0u] = values[i].x;
      rgba[4u * index + 1u] = values[i].y;
      rgba[4u * index + 2u] = values[i].z;
      rgba[4u * index + 3u] = occlusion ? (*occlusion)[i] : 1.0f;
      state[index] = 1u;
    }

    const uint32_t passes = std::min(dilation_passes, 250u);
    for (const VkrBakeLightmapRect &rect : layout.rects) {
      if (rect.page != page) {
        continue;
      }
      const uint32_t x_end = rect.x + rect.width;
      const uint32_t y_end = rect.y + rect.height;

      for (uint32_t pass = 0u; pass < passes; ++pass) {
        const uint8_t filled_before = (uint8_t)(2u + pass);
        bool filled_any = false;
        for (uint32_t y = rect.y; y < y_end; ++y) {
          for (uint32_t x = rect.x; x < x_end; ++x) {
            const size_t index = (size_t)y * size + x;
            if (state[index] != 0u) {
              continue;
            }
            float32_t sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            uint32_t count = 0u;
            for (int32_t dy = -1; dy <= 1; ++dy) {
              for (int32_t dx = -1; dx <= 1; ++dx) {
                const int64_t nx = (int64_t)x + dx;
                const int64_t ny = (int64_t)y + dy;
                if (nx < rect.x || ny < rect.y || nx >= x_end || ny >= y_end) {
                  continue;
                }
                const size_t neighbor = (size_t)ny * size + (size_t)nx;
                const uint8_t neighbor_state = state[neighbor];
                if (neighbor_state == 0u || neighbor_state >= filled_before) {
                  continue;
                }
                for (uint32_t c = 0u; c < 4u; ++c) {
                  sum[c] += rgba[4u * neighbor + c];
                }
                ++count;
              }
            }
            if (count == 0u) {
              continue;
            }
            const float32_t scale = 1.0f / (float32_t)count;
            for (uint32_t c = 0u; c < 4u; ++c) {
              rgba[4u * index + c] = sum[c] * scale;
            }
            state[index] = filled_before;
            filled_any = true;
          }
        }
        if (!filled_any) {
          break;
        }
      }

      float64_t mean[4] = {0.0, 0.0, 0.0, 0.0};
      uint64_t covered = 0u;
      for (uint32_t y = rect.y; y < y_end; ++y) {
        for (uint32_t x = rect.x; x < x_end; ++x) {
          const size_t index = (size_t)y * size + x;
          if (state[index] == 1u) {
            for (uint32_t c = 0u; c < 4u; ++c) {
              mean[c] += rgba[4u * index + c];
            }
            ++covered;
          }
        }
      }
      if (covered != 0u) {
        for (uint32_t c = 0u; c < 4u; ++c) {
          mean[c] /= (float64_t)covered;
        }
      } else {
        mean[3] = 1.0;
      }
      for (uint32_t y = rect.y; y < y_end; ++y) {
        for (uint32_t x = rect.x; x < x_end; ++x) {
          const size_t index = (size_t)y * size + x;
          if (state[index] == 0u) {
            for (uint32_t c = 0u; c < 4u; ++c) {
              rgba[4u * index + c] = (float32_t)mean[c];
            }
          }
        }
      }
    }
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

namespace {

/* Runs one astcenc compression or decompression over `threads` workers;
   astcenc splits the image's blocks between them. */
template <typename Work>
bool run_astc_workers(astcenc_context *context, uint32_t threads, Work work) {
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (uint32_t thread = 0u; thread < threads; ++thread) {
    workers.emplace_back([&, thread] {
      if (work(thread) != ASTCENC_SUCCESS) {
        failed.store(true);
      }
    });
  }
  for (std::thread &worker : workers) {
    worker.join();
  }
  return !failed.load();
}

} // namespace

bool vkr_bake_lightmap_encode_astc_hdr(const std::vector<float32_t> &rgba,
                                       uint32_t size, float32_t effort,
                                       uint32_t threads,
                                       std::vector<uint8_t> *out_blocks) {
  if (!out_blocks || size == 0u || size % 4u != 0u ||
      rgba.size() != (size_t)size * size * 4u || !(effort >= 0.0f) ||
      effort > 100.0f) {
    return false;
  }
  astcenc_config config;
  if (astcenc_config_init(ASTCENC_PRF_HDR_RGB_LDR_A, 4u, 4u, 1u, effort, 0u,
                          &config) != ASTCENC_SUCCESS) {
    return false;
  }
  const uint32_t workers = std::max(1u, threads);
  astcenc_context *context = nullptr;
  if (astcenc_context_alloc(&config, workers, &context) != ASTCENC_SUCCESS) {
    return false;
  }
  bool encoded = false;
  try {
    const size_t blocks = (size_t)(size / 4u) * (size / 4u);
    out_blocks->assign(blocks * 16u, 0u);
    void *slice = const_cast<float32_t *>(rgba.data());
    astcenc_image image = {size, size, 1u, ASTCENC_TYPE_F32, &slice};
    const astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G,
                                     ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    encoded = run_astc_workers(context, workers, [&](uint32_t thread) {
      return astcenc_compress_image(context, &image, &swizzle,
                                    out_blocks->data(), out_blocks->size(),
                                    thread);
    });
  } catch (const std::bad_alloc &) {
    encoded = false;
  }
  astcenc_context_free(context);
  return encoded;
}

bool vkr_bake_lightmap_decode_astc_hdr(const std::vector<uint8_t> &blocks,
                                       uint32_t size,
                                       std::vector<float32_t> *out_rgba) {
  if (!out_rgba || size == 0u || size % 4u != 0u ||
      blocks.size() != (size_t)(size / 4u) * (size / 4u) * 16u) {
    return false;
  }
  astcenc_config config;
  if (astcenc_config_init(ASTCENC_PRF_HDR_RGB_LDR_A, 4u, 4u, 1u,
                          ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY,
                          &config) != ASTCENC_SUCCESS) {
    return false;
  }
  astcenc_context *context = nullptr;
  if (astcenc_context_alloc(&config, 1u, &context) != ASTCENC_SUCCESS) {
    return false;
  }
  bool decoded = false;
  try {
    out_rgba->assign((size_t)size * size * 4u, 0.0f);
    void *slice = out_rgba->data();
    astcenc_image image = {size, size, 1u, ASTCENC_TYPE_F32, &slice};
    const astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G,
                                     ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    decoded = astcenc_decompress_image(context, blocks.data(), blocks.size(),
                                       &image, &swizzle, 0u) == ASTCENC_SUCCESS;
  } catch (const std::bad_alloc &) {
    decoded = false;
  }
  astcenc_context_free(context);
  return decoded;
}

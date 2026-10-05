#include "bake/vkr_bake_lightmap.h"

#include <ktx-software/external/astc-encoder/Source/astcenc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>
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

bool vkr_bake_lightmap_compose_page(
    const VkrBakeLightmapLayout &layout, uint32_t page,
    const std::vector<VkrBakeLightmapTexel> &texels,
    const std::vector<Vec3> &values, uint32_t dilation_passes,
    std::vector<float32_t> *out_rgba) {
  if (!out_rgba || page >= layout.page_count ||
      texels.size() != values.size()) {
    return false;
  }
  try {
    const size_t size = layout.page_size;
    out_rgba->assign(size * size * 4u, 0.0f);
    float32_t *rgba = out_rgba->data();
    /* Zero is empty, one covered, and 2 + pass a texel filled in that pass,
       so a pass reads only texels filled before it. */
    std::vector<uint8_t> state(size * size, 0u);
    for (size_t i = 0u; i < texels.size(); ++i) {
      const size_t index = (size_t)texels[i].y * size + texels[i].x;
      rgba[4u * index + 0u] = values[i].x;
      rgba[4u * index + 1u] = values[i].y;
      rgba[4u * index + 2u] = values[i].z;
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
            float32_t sum[3] = {0.0f, 0.0f, 0.0f};
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
                sum[0] += rgba[4u * neighbor + 0u];
                sum[1] += rgba[4u * neighbor + 1u];
                sum[2] += rgba[4u * neighbor + 2u];
                ++count;
              }
            }
            if (count == 0u) {
              continue;
            }
            const float32_t scale = 1.0f / (float32_t)count;
            rgba[4u * index + 0u] = sum[0] * scale;
            rgba[4u * index + 1u] = sum[1] * scale;
            rgba[4u * index + 2u] = sum[2] * scale;
            state[index] = filled_before;
            filled_any = true;
          }
        }
        if (!filled_any) {
          break;
        }
      }

      float64_t mean[3] = {0.0, 0.0, 0.0};
      uint64_t covered = 0u;
      for (uint32_t y = rect.y; y < y_end; ++y) {
        for (uint32_t x = rect.x; x < x_end; ++x) {
          const size_t index = (size_t)y * size + x;
          if (state[index] == 1u) {
            mean[0] += rgba[4u * index + 0u];
            mean[1] += rgba[4u * index + 1u];
            mean[2] += rgba[4u * index + 2u];
            ++covered;
          }
        }
      }
      if (covered != 0u) {
        mean[0] /= (float64_t)covered;
        mean[1] /= (float64_t)covered;
        mean[2] /= (float64_t)covered;
      }
      for (uint32_t y = rect.y; y < y_end; ++y) {
        for (uint32_t x = rect.x; x < x_end; ++x) {
          const size_t index = (size_t)y * size + x;
          if (state[index] == 0u) {
            rgba[4u * index + 0u] = (float32_t)mean[0];
            rgba[4u * index + 1u] = (float32_t)mean[1];
            rgba[4u * index + 2u] = (float32_t)mean[2];
          }
        }
      }
    }

    for (size_t i = 0u; i < size * size; ++i) {
      rgba[4u * i + 3u] = 1.0f;
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

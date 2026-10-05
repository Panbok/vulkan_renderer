#include "vkr_tool_entry.h"

extern "C" {
#include "assets/vkr_lightmap_set.h"
#include "core/logger.h"
#include "core/vkr_hash.h"
#include "memory/vkr_arena_allocator.h"
}
#include "bake/vkr_bake_atmosphere.h"
#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_integrator.h"
#include "bake/vkr_bake_lightmap.h"
#include "bake/vkr_bake_metal.h"
#include "bake/vkr_bake_scene.h"
#include "filesystem/vkr_filesystem_cpp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/*
 * Static-light lightmap baker (ADR-087, docs/proposals/tiled-pipeline.md).
 * It packs and rasterizes the scene's lightmapped instances. The CPU mode
 * gathers cosine-weighted samples per texel through the ADR-054 integrator and
 * reports throughput. `--gpu trace` measures Metal ray throughput; `--gpu
 * gather` bakes the layer irradiance on Metal and checks it against the CPU
 * integrator. `--output` bakes the scene's lightmap set (VKLM) on Metal and
 * `--manifest` writes the bake's source closure for `vkr_bakery bake
 * lightmap`; `--inspect` stops after the manifest.
 */

namespace {

enum class GpuMode : uint8_t {
  Off,
  Trace,
  Gather,
};

/* Which transport the CPU parity check compares; emission is always in. Sun
   is the directional lights and lamps the other lights, both without sky. */
enum class CheckTransport : uint8_t {
  All,
  Sky,
  Sun,
  Lamps,
};

struct Options {
  const char *scene = nullptr;
  const char *output = nullptr;
  const char *manifest = nullptr;
  bool inspect = false;
  uint32_t page_size = 4096u;
  float32_t texels_per_unit = 8.0f;
  uint32_t samples = 16u;
  uint32_t max_depth = 4u;
  uint32_t pages = UINT32_MAX;
  uint32_t threads = 0u;
  uint32_t seed = 1u;
  GpuMode gpu = GpuMode::Off;
  uint32_t check_texels = 4096u;
  uint32_t check_samples = 256u;
  CheckTransport check_transport = CheckTransport::All;
  /* astcenc effort for the layer pages, 0 (fastest) to 100. */
  float32_t astc_effort = 10.0f;
  uint32_t dilation_passes = 4u;
};

void usage() {
  std::fprintf(
      stderr,
      "Lightmap baker: --scene <scene.json> [--output <set.vklm>] "
      "[--manifest <path>] [--inspect] [--page-size <texels>] "
      "[--texels-per-unit <world density>] [--samples <n>] [--max-depth <n>] "
      "[--pages <n>] [--threads <n>] [--seed <n>] [--gpu <trace|gather>] "
      "[--check-texels <n>] [--check-samples <n>] "
      "[--check-transport <all|sky|sun|lamps>] [--astc-effort <0-100>] "
      "[--dilation <passes>]\n");
}

bool parse_u32(const char *text, uint32_t *out) {
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (!end || *end != '\0' || value > UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)value;
  return true;
}

bool parse(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const char *flag = argv[i];
    if (std::strcmp(flag, "--inspect") == 0) {
      options->inspect = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    const char *value = argv[++i];
    if (std::strcmp(flag, "--scene") == 0) {
      options->scene = value;
    } else if (std::strcmp(flag, "--output") == 0) {
      options->output = value;
    } else if (std::strcmp(flag, "--manifest") == 0) {
      options->manifest = value;
    } else if (std::strcmp(flag, "--page-size") == 0) {
      if (!parse_u32(value, &options->page_size)) {
        return false;
      }
    } else if (std::strcmp(flag, "--texels-per-unit") == 0) {
      options->texels_per_unit = std::strtof(value, nullptr);
    } else if (std::strcmp(flag, "--samples") == 0) {
      if (!parse_u32(value, &options->samples)) {
        return false;
      }
    } else if (std::strcmp(flag, "--max-depth") == 0) {
      if (!parse_u32(value, &options->max_depth)) {
        return false;
      }
    } else if (std::strcmp(flag, "--pages") == 0) {
      if (!parse_u32(value, &options->pages)) {
        return false;
      }
    } else if (std::strcmp(flag, "--threads") == 0) {
      if (!parse_u32(value, &options->threads)) {
        return false;
      }
    } else if (std::strcmp(flag, "--gpu") == 0) {
      if (std::strcmp(value, "trace") == 0) {
        options->gpu = GpuMode::Trace;
      } else if (std::strcmp(value, "gather") == 0) {
        options->gpu = GpuMode::Gather;
      } else {
        return false;
      }
    } else if (std::strcmp(flag, "--check-texels") == 0) {
      if (!parse_u32(value, &options->check_texels)) {
        return false;
      }
    } else if (std::strcmp(flag, "--astc-effort") == 0) {
      options->astc_effort = std::strtof(value, nullptr);
    } else if (std::strcmp(flag, "--dilation") == 0) {
      if (!parse_u32(value, &options->dilation_passes)) {
        return false;
      }
    } else if (std::strcmp(flag, "--check-transport") == 0) {
      if (std::strcmp(value, "all") == 0) {
        options->check_transport = CheckTransport::All;
      } else if (std::strcmp(value, "sky") == 0) {
        options->check_transport = CheckTransport::Sky;
      } else if (std::strcmp(value, "sun") == 0) {
        options->check_transport = CheckTransport::Sun;
      } else if (std::strcmp(value, "lamps") == 0) {
        options->check_transport = CheckTransport::Lamps;
      } else {
        return false;
      }
    } else if (std::strcmp(flag, "--check-samples") == 0) {
      if (!parse_u32(value, &options->check_samples)) {
        return false;
      }
    } else if (std::strcmp(flag, "--seed") == 0) {
      if (!parse_u32(value, &options->seed)) {
        return false;
      }
    } else {
      return false;
    }
  }
  if (options->inspect && (!options->manifest || options->output)) {
    return false;
  }
  if (options->output && options->gpu != GpuMode::Off) {
    return false;
  }
  return options->scene && options->samples > 0u &&
         options->astc_effort >= 0.0f && options->astc_effort <= 100.0f &&
         options->check_samples > 0u && options->max_depth > 0u &&
         options->max_depth <= VKR_BAKE_INTEGRATOR_MAX_DEPTH &&
         std::isfinite(options->texels_per_unit) &&
         options->texels_per_unit > 0.0f;
}

uint32_t mix_seed(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  return x ^ (x >> 16);
}

float32_t random_unit(uint32_t seed) {
  return (float32_t)(mix_seed(seed) >> 8) * (1.0f / 16777216.0f);
}

Vec3 scene_environment(void *scene, Vec3 direction) {
  return vkr_bake_scene_sample_environment((const VkrBakeScene *)scene,
                                           direction);
}

/* Cosine-weighted direction about `normal` from two uniform numbers. */
Vec3 cosine_direction(Vec3 normal, float32_t u1, float32_t u2) {
  const float32_t sign = normal.z >= 0.0f ? 1.0f : -1.0f;
  const float32_t a = -1.0f / (sign + normal.z);
  const float32_t b = normal.x * normal.y * a;
  const Vec3 tangent = vec3_new(1.0f + sign * normal.x * normal.x * a, sign * b,
                                -sign * normal.x);
  const Vec3 bitangent = vec3_new(b, sign + normal.y * normal.y * a, -normal.y);
  const float32_t radius = std::sqrt(u1);
  const float32_t phi = 6.28318530718f * u2;
  const float32_t x = radius * std::cos(phi);
  const float32_t y = radius * std::sin(phi);
  const float32_t z = std::sqrt(std::fmax(0.0f, 1.0f - u1));
  return vec3_normalize(
      vec3_add(vec3_add(vec3_scale(tangent, x), vec3_scale(bitangent, y)),
               vec3_scale(normal, z)));
}

double seconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

/* Texels whose GPU hit fractions the CPU BVH re-traces as a cross-check. */
constexpr uint32_t kCpuCheckTexels = 20000u;

/*
 * Traces the benchmark rays on the GPU and checks a prefix of each page
 * against the CPU BVH with the same seeds. Directions differ only by float
 * rounding between the two math libraries, so hit fractions agree except for
 * grazing rays.
 */
int run_gpu_benchmark(const Options &options, VkrBakeScene &scene,
                      const VkrBakeBvh &bvh,
                      const VkrBakeLightmapLayout &layout) {
  if (!vkr_bake_metal_available()) {
    std::fprintf(stderr, "Metal ray tracing is unavailable on this host\n");
    return 3;
  }
  const auto setup_start = std::chrono::steady_clock::now();
  VkrBakeMetalContext *gpu = vkr_bake_metal_create(scene);
  if (!gpu) {
    return 1;
  }
  std::printf("gpu_setup_s=%.2f\n", seconds_since(setup_start));
  const uint32_t page_end = std::min(options.pages, layout.page_count);
  uint64_t rays_total = 0u;
  double gpu_seconds_total = 0.0;
  double max_check_difference = 0.0;
  for (uint32_t page = 0u; page < page_end; ++page) {
    std::vector<VkrBakeLightmapTexel> texels;
    if (!vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          layout, page, &texels)) {
      vkr_bake_metal_destroy(gpu);
      return 1;
    }
    std::vector<float32_t> hit_fraction;
    double gpu_seconds = 0.0;
    if (!vkr_bake_metal_trace_benchmark(gpu, texels, options.samples,
                                        options.seed, &hit_fraction,
                                        &gpu_seconds)) {
      vkr_bake_metal_destroy(gpu);
      return 1;
    }
    const uint32_t check =
        (uint32_t)std::min<size_t>(texels.size(), kCpuCheckTexels);
    double gpu_sum = 0.0;
    double cpu_sum = 0.0;
    for (uint32_t t = 0u; t < check; ++t) {
      const VkrBakeLightmapTexel &texel = texels[t];
      const Vec3 origin =
          vec3_add(texel.position, vec3_scale(texel.normal, 1.0e-3f));
      const uint32_t texel_seed = mix_seed(options.seed ^ mix_seed(t));
      uint32_t hits = 0u;
      for (uint32_t s = 0u; s < options.samples; ++s) {
        const uint32_t seed = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
        VkrBakeRay ray = {};
        ray.origin = origin;
        ray.direction = cosine_direction(texel.normal, random_unit(seed),
                                         random_unit(seed ^ 0x68bc21ebu));
        ray.t_min = 0.0f;
        ray.t_max = 3.0e38f;
        VkrBakeHit hit = {};
        hits += vkr_bake_bvh_intersect_closest(&bvh, ray, &hit) ? 1u : 0u;
      }
      cpu_sum += (double)hits / options.samples;
      gpu_sum += hit_fraction[t];
    }
    const double difference =
        check ? std::fabs(gpu_sum - cpu_sum) / check : 0.0;
    max_check_difference = std::max(max_check_difference, difference);
    const uint64_t rays = (uint64_t)texels.size() * options.samples;
    rays_total += rays;
    gpu_seconds_total += gpu_seconds;
    std::printf("gpu page=%u texels=%zu gpu_s=%.3f rays_per_s=%.3g "
                "cpu_check_texels=%u mean_hit_gpu=%.4f mean_hit_cpu=%.4f\n",
                page, texels.size(), gpu_seconds, rays / gpu_seconds, check,
                check ? gpu_sum / check : 0.0, check ? cpu_sum / check : 0.0);
    std::fflush(stdout);
  }
  if (gpu_seconds_total > 0.0) {
    std::printf("gpu measured_pages=%u rays=%llu gpu_s=%.3f rays_per_s=%.3g "
                "max_mean_hit_difference=%.5f\n",
                page_end, (unsigned long long)rays_total, gpu_seconds_total,
                rays_total / gpu_seconds_total, max_check_difference);
  }
  vkr_bake_metal_destroy(gpu);
  return 0;
}

float64_t luminance(Vec3 value) {
  return 0.2126 * value.x + 0.7152 * value.y + 0.0722 * value.z;
}

/*
 * CPU reference irradiance for the parity layer: pi times the mean radiance
 * of cosine-weighted integrator paths, the GPU kernel's estimator. Also
 * returns each texel's luminance sample variance for the noise estimate.
 */
bool cpu_reference(const Options &options, const VkrBakeIntegrator &integrator,
                   const std::vector<VkrBakeLightmapTexel> &texels,
                   std::vector<Vec3> *out_irradiance,
                   std::vector<float64_t> *out_variance) {
  const uint32_t threads =
      options.threads ? options.threads
                      : std::max(1u, std::thread::hardware_concurrency());
  out_irradiance->assign(texels.size(), vec3_zero());
  out_variance->assign(texels.size(), 0.0);
  std::atomic<uint64_t> next{0u};
  std::atomic<bool> failed{false};
  auto worker = [&]() {
    for (;;) {
      const uint64_t t = next.fetch_add(1u);
      if (t >= texels.size() || failed.load()) {
        break;
      }
      const VkrBakeLightmapTexel &texel = texels[t];
      const Vec3 origin =
          vec3_add(texel.position, vec3_scale(texel.normal, 1.0e-3f));
      const uint32_t texel_seed =
          mix_seed((options.seed + 0x51ed27u) ^ mix_seed((uint32_t)t));
      Vec3 sum = vec3_zero();
      float64_t sum_y = 0.0;
      float64_t sum_y2 = 0.0;
      for (uint32_t s = 0u; s < options.check_samples; ++s) {
        const uint32_t seed = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
        const Vec3 direction = cosine_direction(
            texel.normal, random_unit(seed), random_unit(seed ^ 0x68bc21ebu));
        VkrBakeIntegratorResult result = {};
        VkrBakeIntegratorError error = {};
        if (!vkr_bake_integrator_trace(&integrator, origin, direction,
                                       ((uint64_t)seed << 32) | s, &result,
                                       &error)) {
          failed.store(true);
          return;
        }
        const Vec3 value = vec3_scale(result.radiance, 3.14159265f);
        sum = vec3_add(sum, value);
        const float64_t y = luminance(value);
        sum_y += y;
        sum_y2 += y * y;
      }
      const float64_t n = options.check_samples;
      (*out_irradiance)[t] = vec3_scale(sum, (float32_t)(1.0 / n));
      (*out_variance)[t] =
          std::max(0.0, sum_y2 / n - (sum_y / n) * (sum_y / n));
    }
  };
  std::vector<std::thread> pool;
  for (uint32_t i = 0u; i < threads; ++i) {
    pool.emplace_back(worker);
  }
  for (std::thread &thread : pool) {
    thread.join();
  }
  return !failed.load();
}

/*
 * Composes a layer's page, encodes it to ASTC 4x4 HDR and decodes it again,
 * reporting the time of each step and the encoding error at covered texels:
 * the RMS of the log2 luminance ratio and the mean relative luminance error,
 * over texels brighter than 1e-4.
 */
bool report_encoding(const Options &options,
                     const VkrBakeLightmapLayout &layout, uint32_t page,
                     const std::vector<VkrBakeLightmapTexel> &texels,
                     const std::vector<Vec3> &values, const char *name) {
  const uint32_t threads =
      options.threads ? options.threads
                      : std::max(1u, std::thread::hardware_concurrency());
  const auto compose_start = std::chrono::steady_clock::now();
  std::vector<float32_t> rgba;
  if (!vkr_bake_lightmap_compose_page(layout, page, texels, values,
                                      options.dilation_passes, &rgba)) {
    std::fprintf(stderr, "Composing page %u failed\n", page);
    return false;
  }
  const double compose_seconds = seconds_since(compose_start);
  const auto encode_start = std::chrono::steady_clock::now();
  std::vector<uint8_t> blocks;
  if (!vkr_bake_lightmap_encode_astc_hdr(
          rgba, layout.page_size, options.astc_effort, threads, &blocks)) {
    std::fprintf(stderr, "Encoding page %u failed\n", page);
    return false;
  }
  const double encode_seconds = seconds_since(encode_start);
  std::vector<float32_t> decoded;
  if (!vkr_bake_lightmap_decode_astc_hdr(blocks, layout.page_size, &decoded)) {
    std::fprintf(stderr, "Decoding page %u failed\n", page);
    return false;
  }
  float64_t log_squared = 0.0;
  float64_t relative = 0.0;
  uint64_t measured = 0u;
  for (const VkrBakeLightmapTexel &texel : texels) {
    const size_t index = (size_t)texel.y * layout.page_size + texel.x;
    const Vec3 source = vec3_new(rgba[4u * index], rgba[4u * index + 1u],
                                 rgba[4u * index + 2u]);
    const Vec3 result = vec3_new(decoded[4u * index], decoded[4u * index + 1u],
                                 decoded[4u * index + 2u]);
    const float64_t y_source = luminance(source);
    const float64_t y_result = luminance(result);
    if (!(y_source > 1.0e-4)) {
      continue;
    }
    const float64_t ratio = std::log2(std::max(y_result, 1.0e-6) / y_source);
    log_squared += ratio * ratio;
    relative += std::fabs(y_result - y_source) / y_source;
    ++measured;
  }
  std::printf("astc page=%u layer=%s effort=%g compose_s=%.2f encode_s=%.2f "
              "bytes=%zu measured_texels=%llu rms_log2_luminance=%.4f "
              "mean_relative_luminance=%.4f\n",
              page, name, options.astc_effort, compose_seconds, encode_seconds,
              blocks.size(), (unsigned long long)measured,
              measured ? std::sqrt(log_squared / measured) : 0.0,
              measured ? relative / measured : 0.0);
  std::fflush(stdout);
  return true;
}

/*
 * Bakes three layers on Metal per page: the parity layer (every light, the
 * sky and emission, no texel direct term: what one integrator path carries),
 * sun key 0 (directional lights and sky, bounce only) and lamp group 0 (the
 * other lights and emission, texel direct included). On page 0 it compares
 * the parity layer at evenly spaced texels against the CPU integrator.
 */
int run_gpu_gather(const Options &options, VkrBakeScene &scene,
                   const VkrBakeIntegrator &integrator,
                   const VkrBakeLightmapLayout &layout) {
  if (!vkr_bake_metal_available()) {
    std::fprintf(stderr, "Metal ray tracing is unavailable on this host\n");
    return 3;
  }
  const auto setup_start = std::chrono::steady_clock::now();
  VkrBakeMetalContext *gpu = vkr_bake_metal_create(scene);
  if (!gpu) {
    return 1;
  }
  std::printf("gpu_setup_s=%.2f\n", seconds_since(setup_start));

  const bool sky = scene.environment.enabled &&
                   scene.environment.kind != VkrBakeSceneEnvironmentKind::None;
  VkrBakeMetalLayer parity;
  VkrBakeMetalLayer sun_key;
  VkrBakeMetalLayer lamps;
  for (uint32_t i = 0u; i < scene.lights.size(); ++i) {
    parity.lights.push_back(i);
    if (scene.lights[i].kind == VkrBakeSceneLightKind::Directional) {
      sun_key.lights.push_back(i);
    } else {
      lamps.lights.push_back(i);
    }
  }
  parity.sky = sky;
  parity.emission = true;
  sun_key.sky = sky;
  lamps.emission = true;
  lamps.texel_direct = true;
  std::printf("layers sun_key_lights=%zu lamp_lights=%zu sky=%d\n",
              sun_key.lights.size(), lamps.lights.size(), sky ? 1 : 0);

  VkrBakeMetalGatherSettings settings;
  settings.samples = options.samples;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = options.max_depth >= 4u ? 4u : 0u;
  settings.seed = options.seed;

  struct NamedLayer {
    const char *name;
    const VkrBakeMetalLayer *layer;
    double seconds;
    uint64_t texels;
  };
  NamedLayer layers[] = {{"parity", &parity, 0.0, 0u},
                         {"sun_key0", &sun_key, 0.0, 0u},
                         {"lamps0", &lamps, 0.0, 0u}};
  const uint32_t page_end = std::min(options.pages, layout.page_count);
  for (uint32_t page = 0u; page < page_end; ++page) {
    std::vector<VkrBakeLightmapTexel> texels;
    if (!vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          layout, page, &texels)) {
      vkr_bake_metal_destroy(gpu);
      return 1;
    }
    for (NamedLayer &named : layers) {
      std::vector<Vec3> irradiance;
      double seconds = 0.0;
      if (!vkr_bake_metal_gather(gpu, texels, *named.layer, settings,
                                 &irradiance, &seconds)) {
        vkr_bake_metal_destroy(gpu);
        return 1;
      }
      float64_t mean = 0.0;
      uint64_t nonfinite = 0u;
      for (const Vec3 &value : irradiance) {
        const float64_t y = luminance(value);
        if (!std::isfinite(y)) {
          ++nonfinite;
          continue;
        }
        mean += y;
      }
      mean /= std::max<size_t>(texels.size(), 1u);
      named.seconds += seconds;
      named.texels += texels.size();
      std::printf("gpu page=%u layer=%s texels=%zu gpu_s=%.3f "
                  "paths_per_s=%.3g mean_luminance=%.5g nonfinite=%llu\n",
                  page, named.name, texels.size(), seconds,
                  (double)texels.size() * options.samples / seconds, mean,
                  (unsigned long long)nonfinite);
      std::fflush(stdout);
      if (!report_encoding(options, layout, page, texels, irradiance,
                           named.name)) {
        vkr_bake_metal_destroy(gpu);
        return 1;
      }
    }

    if (page != 0u || texels.empty() || options.check_texels == 0u) {
      continue;
    }
    const size_t check = std::min<size_t>(texels.size(), options.check_texels);
    std::vector<VkrBakeLightmapTexel> subset(check);
    for (size_t i = 0u; i < check; ++i) {
      subset[i] = texels[i * texels.size() / check];
    }
    VkrBakeMetalGatherSettings check_settings = settings;
    check_settings.samples = options.check_samples;
    /* The check layer and a CPU integrator restricted to the same transport. */
    VkrBakeMetalLayer check_layer = parity;
    VkrBakeIntegrator check_integrator = integrator;
    std::vector<VkrBakeSceneLight> check_lights;
    if (options.check_transport == CheckTransport::Sky) {
      check_layer.lights.clear();
      check_integrator.settings.scene.light_count = 0u;
    } else if (options.check_transport != CheckTransport::All) {
      const bool sun = options.check_transport == CheckTransport::Sun;
      check_layer = sun ? sun_key : lamps;
      check_layer.sky = false;
      check_layer.emission = true;
      check_layer.texel_direct = false;
      for (uint32_t index : check_layer.lights) {
        check_lights.push_back(scene.lights[index]);
      }
      check_integrator.settings.scene.lights = check_lights.data();
      check_integrator.settings.scene.light_count =
          (uint32_t)check_lights.size();
      check_integrator.settings.environment_radiance = nullptr;
    }
    std::vector<Vec3> gpu_values;
    double seconds = 0.0;
    if (!vkr_bake_metal_gather(gpu, subset, check_layer, check_settings,
                               &gpu_values, &seconds)) {
      vkr_bake_metal_destroy(gpu);
      return 1;
    }
    const auto cpu_start = std::chrono::steady_clock::now();
    std::vector<Vec3> cpu_values;
    std::vector<float64_t> variance;
    if (!cpu_reference(options, check_integrator, subset, &cpu_values,
                       &variance)) {
      std::fprintf(stderr, "CPU reference transport failed\n");
      vkr_bake_metal_destroy(gpu);
      return 1;
    }
    /* Means over the subset, the standard error of their difference (both
       estimators carry about the CPU's per-texel variance), and the per-texel
       relative RMS difference against the noise both sides contribute. */
    Vec3 cpu_mean = vec3_zero();
    Vec3 gpu_mean = vec3_zero();
    float64_t variance_sum = 0.0;
    float64_t squared_difference = 0.0;
    float64_t squared_noise = 0.0;
    for (size_t i = 0u; i < check; ++i) {
      cpu_mean = vec3_add(cpu_mean, cpu_values[i]);
      gpu_mean = vec3_add(gpu_mean, gpu_values[i]);
      variance_sum += variance[i];
      const float64_t d = luminance(gpu_values[i]) - luminance(cpu_values[i]);
      squared_difference += d * d;
      squared_noise += 2.0 * variance[i] / options.check_samples;
    }
    cpu_mean = vec3_scale(cpu_mean, 1.0f / check);
    gpu_mean = vec3_scale(gpu_mean, 1.0f / check);
    const float64_t standard_error =
        std::sqrt(2.0 * variance_sum / options.check_samples) / check;
    const float64_t cpu_y = luminance(cpu_mean);
    const float64_t gpu_y = luminance(gpu_mean);
    std::printf(
        "parity texels=%zu samples=%u cpu_s=%.2f cpu_mean=(%.5g %.5g %.5g) "
        "gpu_mean=(%.5g %.5g %.5g) luminance_cpu=%.5g luminance_gpu=%.5g "
        "relative_difference=%.4f z=%.2f texel_rms_over_noise=%.3f\n",
        check, options.check_samples, seconds_since(cpu_start), cpu_mean.x,
        cpu_mean.y, cpu_mean.z, gpu_mean.x, gpu_mean.y, gpu_mean.z, cpu_y,
        gpu_y, cpu_y > 0.0 ? (gpu_y - cpu_y) / cpu_y : 0.0,
        standard_error > 0.0 ? (gpu_y - cpu_y) / standard_error : 0.0,
        squared_noise > 0.0 ? std::sqrt(squared_difference / squared_noise)
                            : 0.0);
    std::fflush(stdout);

    if (options.check_transport != CheckTransport::All) {
      continue;
    }
    /* The layers partition the parity transport: sun key plus lamps without
       their texel direct term carry every light, the sky and emission once. */
    VkrBakeMetalLayer lamps_bounce = lamps;
    lamps_bounce.texel_direct = false;
    std::vector<Vec3> sun_values;
    std::vector<Vec3> lamp_values;
    if (!vkr_bake_metal_gather(gpu, subset, sun_key, check_settings,
                               &sun_values, &seconds) ||
        !vkr_bake_metal_gather(gpu, subset, lamps_bounce, check_settings,
                               &lamp_values, &seconds)) {
      vkr_bake_metal_destroy(gpu);
      return 1;
    }
    float64_t layer_sum = 0.0;
    for (size_t i = 0u; i < check; ++i) {
      layer_sum += luminance(sun_values[i]) + luminance(lamp_values[i]);
    }
    layer_sum /= check;
    std::printf(
        "partition sun_plus_lamps=%.5g parity_gpu=%.5g "
        "relative_difference=%.4f z=%.2f\n",
        layer_sum, gpu_y, gpu_y > 0.0 ? (layer_sum - gpu_y) / gpu_y : 0.0,
        standard_error > 0.0 ? (layer_sum - gpu_y) / standard_error : 0.0);
    std::fflush(stdout);
  }
  for (const NamedLayer &named : layers) {
    if (named.seconds > 0.0) {
      std::printf("gpu layer=%s pages=%u texels=%llu gpu_s=%.3f "
                  "paths_per_s=%.3g\n",
                  named.name, page_end, (unsigned long long)named.texels,
                  named.seconds,
                  (double)named.texels * options.samples / named.seconds);
    }
  }
  vkr_bake_metal_destroy(gpu);
  return 0;
}

std::string json_string(const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += (char)c;
    } else if (c < 0x20u) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else {
      result += (char)c;
    }
  }
  return result + "\"";
}

/* Writes `bytes` beside `path` and renames the file over it. */
bool write_atomic(const char *path, const std::string &bytes) {
  const std::filesystem::path target = vkr_filesystem_native_utf8_path(path);
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), (std::streamsize)bytes.size());
    if (!stream) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

/*
 * The bake's source closure and counts in the inspect-manifest shape that
 * `vkr_bakery bake` validates: dependencies, atmosphere provenance and the
 * counts that decide whether anything is lightmapped.
 */
bool write_manifest(const Options &options, const VkrBakeScene &scene,
                    const VkrBakeLightmapLayout &layout) {
  std::ostringstream manifest;
  manifest << "{\"version\":1,\"dependencies\":[";
  for (size_t i = 0u; i < scene.dependency_paths.size(); ++i) {
    if (i != 0u) {
      manifest << ',';
    }
    manifest << json_string(
        std::filesystem::weakly_canonical(scene.dependency_paths[i])
            .generic_string());
  }
  manifest << "],\"atmosphere\":{\"enabled\":"
           << (scene.atmosphere.enabled ? "true" : "false")
           << ",\"model_version\":" << VKR_BAKE_ATMOSPHERE_MODEL_VERSION
           << ",\"params_hash\":\"" << std::hex
           << vkr_bake_atmosphere_recipe_hash(&scene.atmosphere) << std::dec
           << "\",\"sh_deringing\":"
           << (scene.atmosphere.enabled ? scene.environment.sh_deringing : 0.0f)
           << "},\"triangles\":" << scene.triangles.size()
           << ",\"materials\":" << scene.materials.size()
           << ",\"lights\":" << scene.lights.size()
           << ",\"lightmap_instances\":" << layout.rects.size()
           << ",\"pages\":" << layout.page_count
           << ",\"page_size\":" << layout.page_size << "}\n";
  return write_atomic(options.manifest, manifest.str());
}

/* One baked layer: its record in the set and the transport that fills it. */
struct BakeLayer {
  VkrLightmapLayer record;
  VkrBakeMetalLayer transport;
};

/*
 * The layers this scene's lights make (ADR-087): sun key 0 holds the
 * directional lights' bounce and the sky, without the sun's direct term at the
 * texel, which the runtime adds; lamp group 0 holds the other lights' direct
 * and bounce light and surface emission. A scene without a directional light
 * has no sun key, and its sky joins lamp group 0 as static light.
 */
std::vector<BakeLayer> scene_layers(const VkrBakeScene &scene) {
  const bool sky = scene.environment.enabled &&
                   scene.environment.kind != VkrBakeSceneEnvironmentKind::None;
  BakeLayer sun = {};
  BakeLayer lamps = {};
  bool has_sun = false;
  for (uint32_t i = 0u; i < scene.lights.size(); ++i) {
    const VkrBakeSceneLight &light = scene.lights[i];
    if (light.kind != VkrBakeSceneLightKind::Directional) {
      lamps.transport.lights.push_back(i);
      continue;
    }
    sun.transport.lights.push_back(i);
    if (!has_sun) {
      has_sun = true;
      sun.record.sun_direction = vec3_normalize(
          vec3_new(-light.direction.x, -light.direction.y, -light.direction.z));
    }
  }
  std::vector<BakeLayer> layers;
  if (has_sun) {
    sun.record.kind = VKR_LIGHTMAP_LAYER_SUN_KEY;
    sun.record.index = 0u;
    sun.transport.sky = sky;
    layers.push_back(sun);
  }
  lamps.record.kind = VKR_LIGHTMAP_LAYER_LAMP_GROUP;
  lamps.record.index = 0u;
  lamps.transport.sky = sky && !has_sun;
  lamps.transport.emission = true;
  lamps.transport.texel_direct = true;
  layers.push_back(lamps);
  return layers;
}

/*
 * Bakes the scene's lightmap set into `options.output`. Pages are baked one
 * at a time and each layer image is encoded and appended as soon as it is
 * gathered; the header and tables, which carry the payload checksum, are
 * written last. The file appears under its name only when complete.
 */
int bake_set(const Options &options, VkrBakeScene &scene,
             const VkrBakeLightmapLayout &layout) {
  if (layout.rects.empty()) {
    std::fprintf(stderr, "The scene has no lightmapped instances\n");
    return 1;
  }
  if (!vkr_bake_metal_available()) {
    std::fprintf(stderr, "Lightmap bakes need Metal ray tracing, which this "
                         "host does not have\n");
    return 1;
  }

  const std::vector<BakeLayer> layers = scene_layers(scene);
  std::vector<VkrLightmapLayer> layer_records;
  for (const BakeLayer &layer : layers) {
    layer_records.push_back(layer.record);
  }
  std::vector<uint32_t> lightmap_by_source(layout.rect_by_instance.size(),
                                           UINT32_MAX);
  for (uint32_t i = 0u; i < scene.lightmap_instances.size(); ++i) {
    lightmap_by_source[scene.lightmap_instances[i].source_instance_index] = i;
  }
  std::vector<VkrLightmapInstance> instances;
  for (const VkrBakeLightmapRect &rect : layout.rects) {
    const VkrBakeLightmapInstance &source =
        scene
            .lightmap_instances[lightmap_by_source[rect.source_instance_index]];
    VkrLightmapInstance instance = {};
    if (source.has_document_id) {
      std::memcpy(instance.document_id, source.document_id.data(),
                  sizeof(instance.document_id));
    }
    instance.entity_index = source.entity_index;
    instance.instance_index = source.source_node_index;
    instance.page = rect.page;
    instance.x = rect.x;
    instance.y = rect.y;
    instance.width = rect.width;
    instance.height = rect.height;
    instances.push_back(instance);
  }
  std::sort(instances.begin(), instances.end(),
            [](const VkrLightmapInstance &a, const VkrLightmapInstance &b) {
              if (a.entity_index != b.entity_index) {
                return a.entity_index < b.entity_index;
              }
              return a.instance_index < b.instance_index;
            });

  VkrLightmapSet set = {};
  set.page_size = layout.page_size;
  set.page_count = layout.page_count;
  set.layer_count = (uint32_t)layer_records.size();
  set.instance_count = (uint32_t)instances.size();
  set.texels_per_unit = options.texels_per_unit;
  set.layers = layer_records.data();
  set.instances = instances.data();
  uint64_t payload_offset = 0u;
  uint64_t file_size = 0u;
  if (!vkr_lightmap_set_layout(&set, &payload_offset, &file_size)) {
    std::fprintf(stderr, "The lightmap set layout is invalid\n");
    return 1;
  }

  const std::filesystem::path target =
      vkr_filesystem_native_utf8_path(options.output);
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
  std::vector<uint8_t> prefix(payload_offset, 0u);
  stream.write((const char *)prefix.data(), (std::streamsize)prefix.size());
  if (!stream) {
    std::fprintf(stderr, "Cannot write %s\n", temporary.string().c_str());
    return 1;
  }

  const auto setup_start = std::chrono::steady_clock::now();
  VkrBakeMetalContext *gpu = vkr_bake_metal_create(scene);
  if (!gpu) {
    return 1;
  }
  std::printf("gpu_setup_s=%.2f layers=%zu\n", seconds_since(setup_start),
              layers.size());
  VkrBakeMetalGatherSettings settings;
  settings.samples = options.samples;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = options.max_depth >= 4u ? 4u : 0u;
  settings.seed = options.seed;
  const uint32_t threads =
      options.threads ? options.threads
                      : std::max(1u, std::thread::hardware_concurrency());

  uint32_t payload_crc = VKR_CRC32_INITIAL;
  bool ok = true;
  double gpu_seconds = 0.0;
  double encode_seconds = 0.0;
  std::vector<float32_t> rgba;
  std::vector<uint8_t> blocks;
  for (uint32_t page = 0u; ok && page < layout.page_count; ++page) {
    std::vector<VkrBakeLightmapTexel> texels;
    ok = vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          layout, page, &texels);
    for (uint32_t l = 0u; ok && l < layers.size(); ++l) {
      std::vector<Vec3> irradiance;
      double seconds = 0.0;
      ok = vkr_bake_metal_gather(gpu, texels, layers[l].transport, settings,
                                 &irradiance, &seconds);
      gpu_seconds += seconds;
      const auto encode_start = std::chrono::steady_clock::now();
      ok = ok &&
           vkr_bake_lightmap_compose_page(layout, page, texels, irradiance,
                                          options.dilation_passes, &rgba) &&
           vkr_bake_lightmap_encode_astc_hdr(
               rgba, layout.page_size, options.astc_effort, threads, &blocks);
      encode_seconds += seconds_since(encode_start);
      if (!ok) {
        break;
      }
      payload_crc =
          vkr_crc32_update(payload_crc, blocks.data(), (uint64_t)blocks.size());
      stream.write((const char *)blocks.data(), (std::streamsize)blocks.size());
      ok = (bool)stream;
      std::printf("baked page=%u/%u layer=%u/%zu texels=%zu gpu_s=%.2f\n",
                  page + 1u, layout.page_count, l + 1u, layers.size(),
                  texels.size(), seconds);
      std::fflush(stdout);
    }
  }
  vkr_bake_metal_destroy(gpu);

  ok = ok && vkr_lightmap_set_write_prefix(&set, ~payload_crc, prefix.data(),
                                           prefix.size());
  if (ok) {
    stream.seekp(0);
    stream.write((const char *)prefix.data(), (std::streamsize)prefix.size());
    stream.close();
    ok = !stream.fail();
  }
  std::error_code error;
  if (ok) {
    std::filesystem::rename(temporary, target, error);
    ok = !error;
  }
  if (!ok) {
    stream.close();
    std::filesystem::remove(temporary, error);
    std::fprintf(stderr, "Lightmap bake failed\n");
    return 1;
  }
  std::printf("saved=%s bytes=%llu pages=%u layers=%zu instances=%zu "
              "gpu_s=%.2f encode_s=%.2f\n",
              options.output, (unsigned long long)file_size, layout.page_count,
              layers.size(), instances.size(), gpu_seconds, encode_seconds);
  return 0;
}

int run(const Options &options, VkrAllocator *allocator, Arena *arena) {
  const auto load_start = std::chrono::steady_clock::now();
  VkrBakeScene scene(allocator);
  VkrBakeSceneError scene_error;
  if (!vkr_bake_scene_load(&scene, options.scene, &scene_error)) {
    std::fprintf(stderr, "Scene preparation failed (error %u): %s\n",
                 (unsigned)scene_error, options.scene);
    return 1;
  }
  if (scene.triangles.size() > VKR_BAKE_BVH_MAX_TRIANGLES) {
    return 1;
  }
  VkrBakeBvh bvh = {};
  if (!vkr_bake_bvh_build(
          {scene.triangles.data(), (uint32_t)scene.triangles.size()}, arena,
          &bvh)) {
    std::fprintf(stderr, "BVH construction failed\n");
    return 1;
  }
  const VkrBakeAabb bounds = bvh.nodes[0].bounds;
  std::printf("scene_load_bvh_s=%.2f triangles=%zu lights=%zu "
              "lightmap_instances=%zu bounds=(%g %g %g)-(%g %g %g)\n",
              seconds_since(load_start), scene.triangles.size(),
              scene.lights.size(), scene.lightmap_instances.size(),
              bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x,
              bounds.max.y, bounds.max.z);

  VkrBakeLightmapLayout layout;
  if (!vkr_bake_lightmap_pack_fitted(scene.lightmap_instances,
                                     options.page_size, options.texels_per_unit,
                                     &layout)) {
    std::fprintf(stderr, "Lightmap packing failed\n");
    return 1;
  }
  uint64_t rect_texels = 0u;
  for (const VkrBakeLightmapRect &rect : layout.rects) {
    rect_texels += (uint64_t)rect.width * rect.height;
  }
  std::printf("pages=%u page_size=%u rects=%zu rect_texels=%llu\n",
              layout.page_count, layout.page_size, layout.rects.size(),
              (unsigned long long)rect_texels);

  if (options.manifest && !write_manifest(options, scene, layout)) {
    std::fprintf(stderr, "Writing the bake manifest failed\n");
    return 1;
  }
  if (options.inspect) {
    return 0;
  }
  if (options.output) {
    return bake_set(options, scene, layout);
  }

  VkrBakeIntegratorSettings settings = {};
  settings.scene = {&bvh,
                    scene.texture_store,
                    scene.materials.data(),
                    (uint32_t)scene.materials.size(),
                    scene.lights.data(),
                    (uint32_t)scene.lights.size(),
                    scene.subsurface_profiles,
                    scene.subsurface_profile_count};
  settings.environment_radiance =
      scene.environment.enabled &&
              scene.environment.kind != VkrBakeSceneEnvironmentKind::None
          ? scene_environment
          : nullptr;
  settings.environment_user = &scene;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = options.max_depth >= 4u ? 4u : 0u;
  settings.max_transparent_layers = VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS;
  settings.ray_epsilon = 0.00001f;
  VkrBakeIntegrator integrator = {};
  VkrBakeIntegratorError integrator_error = {};
  if (!vkr_bake_integrator_init(&settings, &integrator, &integrator_error)) {
    std::fprintf(stderr, "Transport preparation failed: %u\n",
                 integrator_error);
    return 1;
  }

  if (options.gpu == GpuMode::Trace) {
    return run_gpu_benchmark(options, scene, bvh, layout);
  }
  if (options.gpu == GpuMode::Gather) {
    return run_gpu_gather(options, scene, integrator, layout);
  }

  const uint32_t threads =
      options.threads ? options.threads
                      : std::max(1u, std::thread::hardware_concurrency());
  const uint32_t page_end = std::min(options.pages, layout.page_count);
  uint64_t texels_total = 0u;
  uint64_t paths_total = 0u;
  uint64_t segments_total = 0u;
  double raster_seconds = 0.0;
  double trace_seconds = 0.0;
  for (uint32_t page = 0u; page < page_end; ++page) {
    const auto raster_start = std::chrono::steady_clock::now();
    std::vector<VkrBakeLightmapTexel> texels;
    if (!vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          layout, page, &texels)) {
      std::fprintf(stderr, "Rasterizing page %u failed\n", page);
      return 1;
    }
    raster_seconds += seconds_since(raster_start);

    const auto trace_start = std::chrono::steady_clock::now();
    std::atomic<uint64_t> next{0u};
    std::atomic<uint64_t> segments{0u};
    std::atomic<bool> failed{false};
    auto worker = [&]() {
      uint64_t local_segments = 0u;
      for (;;) {
        const uint64_t first = next.fetch_add(256u);
        if (first >= texels.size() || failed.load()) {
          break;
        }
        const uint64_t end = std::min<uint64_t>(first + 256u, texels.size());
        for (uint64_t t = first; t < end; ++t) {
          const VkrBakeLightmapTexel &texel = texels[t];
          const Vec3 origin =
              vec3_add(texel.position, vec3_scale(texel.normal, 1.0e-3f));
          const uint32_t texel_seed = mix_seed(
              options.seed ^ mix_seed(page * 0x9e3779b9u ^ (uint32_t)t));
          for (uint32_t s = 0u; s < options.samples; ++s) {
            const uint32_t seed = mix_seed(texel_seed ^ (s * 0x85ebca6bu));
            const Vec3 direction =
                cosine_direction(texel.normal, random_unit(seed),
                                 random_unit(seed ^ 0x68bc21ebu));
            VkrBakeIntegratorResult result = {};
            VkrBakeIntegratorError error = {};
            if (!vkr_bake_integrator_trace(&integrator, origin, direction,
                                           ((uint64_t)seed << 32) | s, &result,
                                           &error)) {
              failed.store(true);
              break;
            }
            local_segments += result.surface_depth + 1u;
          }
        }
      }
      segments.fetch_add(local_segments);
    };
    std::vector<std::thread> pool;
    for (uint32_t i = 0u; i < threads; ++i) {
      pool.emplace_back(worker);
    }
    for (std::thread &thread : pool) {
      thread.join();
    }
    if (failed.load()) {
      std::fprintf(stderr, "Transport failed on page %u\n", page);
      return 1;
    }
    const double page_seconds = seconds_since(trace_start);
    trace_seconds += page_seconds;
    texels_total += texels.size();
    paths_total += (uint64_t)texels.size() * options.samples;
    segments_total += segments.load();
    std::printf("page=%u texels=%zu trace_s=%.2f paths_per_s=%.3g\n", page,
                texels.size(), page_seconds,
                (double)texels.size() * options.samples / page_seconds);
    std::fflush(stdout);
  }
  if (trace_seconds > 0.0) {
    std::printf("measured_pages=%u texels=%llu paths=%llu segments=%llu "
                "raster_s=%.2f trace_s=%.2f threads=%u paths_per_s=%.3g "
                "segments_per_s=%.3g\n",
                page_end, (unsigned long long)texels_total,
                (unsigned long long)paths_total,
                (unsigned long long)segments_total, raster_seconds,
                trace_seconds, threads, (double)paths_total / trace_seconds,
                (double)segments_total / trace_seconds);
  }
  return 0;
}

} // namespace

VKR_TOOL_ENTRY(vkr_lightmap_baker_tool_main) {
  Options options;
  if (!parse(argc, argv, &options)) {
    usage();
    return 2;
  }
  Arena *arena = arena_create_internal(GB(8), MB(1), ARENA_DEFAULT_FLAGS);
  if (!arena) {
    return 1;
  }
  VkrAllocator allocator = {};
  allocator.ctx = arena;
  if (!vkr_allocator_arena(&allocator) || !log_init(arena)) {
    arena_destroy(arena);
    return 1;
  }
  int result = 1;
  try {
    result = run(options, &allocator, arena);
  } catch (const std::bad_alloc &) {
    std::fprintf(stderr, "Bake allocation failed\n");
  }
  log_shutdown();
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  return result;
}

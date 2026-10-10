#include "vkr_tool_entry.h"

extern "C" {
#include "assets/vkr_lightmap_set.h"
#include "core/logger.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"
#include "memory/vkr_arena_allocator.h"
}
#include "bake/vkr_bake_atmosphere.h"
#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_gpu.h"
#include "bake/vkr_bake_integrator.h"
#include "bake/vkr_bake_layers.h"
#include "bake/vkr_bake_lightmap.h"
#include "bake/vkr_bake_scene.h"
#include "filesystem/vkr_filesystem_cpp.h"
#include "vkr_ibl_math.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/*
 * Static-light lightmap baker (ADR-087, docs/proposals/tiled-pipeline.md).
 * It packs and rasterizes the scene's lightmapped instances. The CPU mode
 * gathers cosine-weighted samples per texel through the ADR-054 integrator and
 * reports throughput. `--gpu trace` measures GPU ray throughput (Metal ray
 * tracing on Apple hosts, Vulkan ray queries elsewhere); `--gpu gather`
 * bakes the layer irradiance on the GPU and checks it against the CPU
 * integrator, and counts each layer's outlier texels. `--output` bakes the
 * scene's lightmap set (VKLM) on the GPU, filling texels buried in solids and
 * denoising each layer's indirect light before encoding (ADR-088), and
 * `--manifest` writes the bake's source closure for `vkr_bakery bake lightmap`;
 * `--inspect` stops after the manifest. A set holds only what the host's
 * pipeline class samples (PipelineClass).
 */

namespace {

/* Outlier rejection (vkr_bake_lightmap_reject_outliers): a texel brighter
   than kOutlierRatio times its brightest same-surface neighbor, of at least
   kOutlierMinNeighbors within kOutlierNeighborTexels texels in world space.
   On Bistro's first page at 64 samples such texels are 1% of the lamp layer
   and carry 1.3% of its light. */
constexpr float32_t kOutlierRatio = 2.0f;
constexpr uint32_t kOutlierMinNeighbors = 3u;
constexpr float32_t kOutlierNeighborTexels = 4.0f;
/* Smoothing passes (vkr_bake_lightmap_smooth) over a published layer's
   direct light, or over all of it without denoising. */
constexpr uint32_t kSmoothPasses = 1u;
/* A-trous passes (vkr_bake_lightmap_denoise) over a published layer's
   indirect light: steps 1, 2, 4 and 8 texels. */
constexpr uint32_t kDenoiseIterations = 4u;
constexpr float32_t kDenoiseSigma = 4.0f;
/* Russian roulette starts after the second bounce. */
constexpr uint32_t kRussianRouletteDepth = 2u;
/* A texel whose first-bounce rays hit the back of one-sided surfaces more
   often than this lies inside a solid, such as floor under a wall, and takes
   its same-surface neighbors' light instead of its own. */
constexpr float32_t kBuriedBackfaceFraction = 0.5f;
/* Samples per texel of a lamp-direct gather (ADR-108): first hits that find
   buried texels and footprint points of rectangle lights. */
constexpr uint32_t kDirectSamples = 8u;
/* World distance within which a first bounce occludes for the ambient
   occlusion every tiled layer's alpha carries (ADR-088). */
constexpr float32_t kOcclusionRadius = 1.0f;

enum class GpuMode : uint8_t {
  Off,
  Trace,
  Gather,
};

/* Encoding of the lamp groups' desktop planes: RGB9E5 irradiance and RGBA8
   direction, or BC6H and BC7, a quarter of the size, from the Vulkan
   device's compute encoders (vkr_bake_gpu_encode_bc6h). Automatic is BC
   where the device has the encoders and uncompressed elsewhere. */
enum class DesktopEncoding : uint8_t {
  Automatic,
  Uncompressed,
  Bc,
};

/* The pipeline class a set is baked for (ADR-087). Each platform bakes,
   packages and tests on its own machine (owner decision 2026-10-09), so a
   set holds only what its class samples: the desktop class each lamp
   group's irradiance and incident direction in the desktop encoding, and no
   sun key; the tiled class every layer's ASTC 4x4 HDR irradiance, and no
   direction, which it does not sample yet. The host picks the class as the
   runtime picks its backend: Metal, the tiled class, on Apple hosts and
   Vulkan, the desktop class, elsewhere. */
enum class PipelineClass : uint8_t {
  Desktop,
  Tiled,
};

#if defined(__APPLE__)
constexpr PipelineClass kHostPipeline = PipelineClass::Tiled;
#else
constexpr PipelineClass kHostPipeline = PipelineClass::Desktop;
#endif

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
  /* Density of the tiled class's lamp-direct pages (ADR-108); zero bakes
     lamp direct light into the lamp groups' irradiance instead. */
  float32_t direct_texels_per_unit = 0.0f;
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
  DesktopEncoding desktop_encoding = DesktopEncoding::Automatic;
  PipelineClass pipeline = kHostPipeline;
  uint32_t dilation_passes = 4u;
  /* Outlier rejection ratio for published layers without denoising; zero
     keeps every texel. */
  float32_t outlier_ratio = kOutlierRatio;
  /* Smoothing passes over published layers after outlier rejection: the
     direct light when denoising, else all of it. */
  uint32_t smooth_passes = kSmoothPasses;
  /* Ambient occlusion radius in world units; zero writes alpha one. */
  float32_t ao_radius = kOcclusionRadius;
  /* Whether published layers denoise their indirect light apart from the
     direct light, and in how many a-trous passes. */
  bool denoise = true;
  uint32_t denoise_iterations = kDenoiseIterations;
  /* Standard deviations of luminance noise within which a denoise tap
     counts fully; zero weighs taps by geometry alone. */
  float32_t denoise_sigma = kDenoiseSigma;
  /* Largest luminance of one indirect sample; zero keeps every sample. */
  float32_t indirect_clamp = 0.0f;
  /* Russian roulette start depth; zero turns it off. */
  uint32_t rr_start_depth = kRussianRouletteDepth;
  /* Back-face fraction above which a texel is buried; zero keeps every
     texel's own light. */
  float32_t buried_backface = kBuriedBackfaceFraction;
  /* Diagnostic CSV of every published texel's light before encoding. */
  const char *dump_texels = nullptr;
};

void usage() {
  std::fprintf(
      stderr,
      "Lightmap baker: --scene <scene.json> [--output <set.vklm>] "
      "[--manifest <path>] [--inspect] [--page-size <texels>] "
      "[--texels-per-unit <world density>] "
      "[--direct-texels-per-unit <world density, 0 off>] [--samples <n>] "
      "[--max-depth <n>] "
      "[--pages <n>] [--threads <n>] [--seed <n>] [--gpu <trace|gather>] "
      "[--check-texels <n>] [--check-samples <n>] "
      "[--check-transport <all|sky|sun|lamps>] [--astc-effort <0-100>] "
      "[--desktop-encoding <uncompressed|bc>] [--pipeline <desktop|tiled>] "
      "[--dilation <passes>] [--outlier-ratio <ratio, 0 off>] "
      "[--smooth <passes>] [--ao-radius <world units, 0 off>] "
      "[--denoise <0|1>] [--denoise-iterations <n>] "
      "[--denoise-sigma <noise deviations, 0 geometry only>] "
      "[--indirect-clamp <luminance, 0 off>] [--rr-start <depth, 0 off>] "
      "[--buried-backface <fraction, 0 off>] [--dump-texels <csv>]\n");
}

/* Parses a finite non-negative float that fills `text`. */
bool parse_nonnegative(const char *text, float32_t *out) {
  char *end = nullptr;
  const float32_t value = std::strtof(text, &end);
  if (!end || end == text || *end != '\0' || !std::isfinite(value) ||
      value < 0.0f) {
    return false;
  }
  *out = value;
  return true;
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
    } else if (std::strcmp(flag, "--direct-texels-per-unit") == 0) {
      if (!parse_nonnegative(value, &options->direct_texels_per_unit)) {
        return false;
      }
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
    } else if (std::strcmp(flag, "--desktop-encoding") == 0) {
      if (std::strcmp(value, "uncompressed") == 0) {
        options->desktop_encoding = DesktopEncoding::Uncompressed;
      } else if (std::strcmp(value, "bc") == 0) {
        options->desktop_encoding = DesktopEncoding::Bc;
      } else {
        return false;
      }
    } else if (std::strcmp(flag, "--pipeline") == 0) {
      if (std::strcmp(value, "desktop") == 0) {
        options->pipeline = PipelineClass::Desktop;
      } else if (std::strcmp(value, "tiled") == 0) {
        options->pipeline = PipelineClass::Tiled;
      } else {
        return false;
      }
    } else if (std::strcmp(flag, "--dilation") == 0) {
      if (!parse_u32(value, &options->dilation_passes)) {
        return false;
      }
    } else if (std::strcmp(flag, "--smooth") == 0) {
      if (!parse_u32(value, &options->smooth_passes)) {
        return false;
      }
    } else if (std::strcmp(flag, "--ao-radius") == 0) {
      char *end = nullptr;
      options->ao_radius = std::strtof(value, &end);
      if (!end || *end != '\0' || !std::isfinite(options->ao_radius) ||
          options->ao_radius < 0.0f) {
        return false;
      }
    } else if (std::strcmp(flag, "--outlier-ratio") == 0) {
      char *end = nullptr;
      options->outlier_ratio = std::strtof(value, &end);
      if (!end || *end != '\0' || !std::isfinite(options->outlier_ratio) ||
          (options->outlier_ratio != 0.0f && options->outlier_ratio < 1.0f)) {
        return false;
      }
    } else if (std::strcmp(flag, "--denoise") == 0) {
      uint32_t denoise = 0u;
      if (!parse_u32(value, &denoise) || denoise > 1u) {
        return false;
      }
      options->denoise = denoise != 0u;
    } else if (std::strcmp(flag, "--denoise-iterations") == 0) {
      if (!parse_u32(value, &options->denoise_iterations) ||
          options->denoise_iterations > 8u) {
        return false;
      }
    } else if (std::strcmp(flag, "--denoise-sigma") == 0) {
      if (!parse_nonnegative(value, &options->denoise_sigma)) {
        return false;
      }
    } else if (std::strcmp(flag, "--indirect-clamp") == 0) {
      if (!parse_nonnegative(value, &options->indirect_clamp)) {
        return false;
      }
    } else if (std::strcmp(flag, "--rr-start") == 0) {
      if (!parse_u32(value, &options->rr_start_depth)) {
        return false;
      }
    } else if (std::strcmp(flag, "--buried-backface") == 0) {
      if (!parse_nonnegative(value, &options->buried_backface) ||
          options->buried_backface > 1.0f) {
        return false;
      }
    } else if (std::strcmp(flag, "--dump-texels") == 0) {
      options->dump_texels = value;
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
  /* A tiled set has no desktop planes to encode. */
  if (options->pipeline == PipelineClass::Tiled &&
      options->desktop_encoding != DesktopEncoding::Automatic) {
    return false;
  }
  return options->scene && options->samples > 0u &&
         options->astc_effort >= 0.0f && options->astc_effort <= 100.0f &&
         options->check_samples > 0u && options->max_depth > 0u &&
         options->max_depth <= VKR_BAKE_INTEGRATOR_MAX_DEPTH &&
         std::isfinite(options->texels_per_unit) &&
         options->texels_per_unit > 0.0f &&
         options->direct_texels_per_unit <= 1024.0f;
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
  if (!vkr_bake_gpu_available()) {
    std::fprintf(stderr, "GPU ray tracing is unavailable on this host\n");
    return 3;
  }
  const auto setup_start = std::chrono::steady_clock::now();
  VkrBakeGpuContext *gpu = vkr_bake_gpu_create(scene);
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
      vkr_bake_gpu_destroy(gpu);
      return 1;
    }
    std::vector<float32_t> hit_fraction;
    double gpu_seconds = 0.0;
    if (!vkr_bake_gpu_trace_benchmark(gpu, texels, options.samples,
                                      options.seed, &hit_fraction,
                                      &gpu_seconds)) {
      vkr_bake_gpu_destroy(gpu);
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
  vkr_bake_gpu_destroy(gpu);
  return 0;
}

float64_t luminance(Vec3 value) {
  return 0.2126 * value.x + 0.7152 * value.y + 0.0722 * value.z;
}

/* The Russian roulette start depth for the GPU gather and the CPU parity
   integrator alike; a start beyond the last bounce turns it off. */
uint32_t rr_start_depth(const Options &options) {
  return options.rr_start_depth <= options.max_depth ? options.rr_start_depth
                                                     : 0u;
}

/* A gathered layer's direct plus indirect light per texel. */
std::vector<Vec3> gathered_total(const VkrBakeGpuGatherResult &result) {
  std::vector<Vec3> total(result.indirect.size());
  for (size_t i = 0u; i < total.size(); ++i) {
    total[i] = vec3_add(result.direct[i], result.indirect[i]);
  }
  return total;
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
  if (!vkr_bake_lightmap_compose_page(layout, page, texels, values, nullptr,
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
 * Bakes three layers on the GPU per page: the parity layer (every light, the
 * sky and emission, no texel direct term: what one integrator path carries),
 * sun key 0 (directional lights and sky, bounce only) and lamp group 0 (the
 * other lights and emission, texel direct included). On page 0 it compares
 * the parity layer at evenly spaced texels against the CPU integrator.
 */
int run_gpu_gather(const Options &options, VkrBakeScene &scene,
                   const VkrBakeIntegrator &integrator,
                   const VkrBakeLightmapLayout &layout) {
  if (!vkr_bake_gpu_available()) {
    std::fprintf(stderr, "GPU ray tracing is unavailable on this host\n");
    return 3;
  }
  const auto setup_start = std::chrono::steady_clock::now();
  VkrBakeGpuContext *gpu = vkr_bake_gpu_create(scene);
  if (!gpu) {
    return 1;
  }
  std::printf("gpu_setup_s=%.2f\n", seconds_since(setup_start));

  const bool sky = scene.environment.enabled &&
                   scene.environment.kind != VkrBakeSceneEnvironmentKind::None;
  VkrBakeGpuLayer parity;
  VkrBakeGpuLayer sun_key;
  VkrBakeGpuLayer lamps;
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
  /* The lamp layer split: its lights alone, and surface emission alone. */
  VkrBakeGpuLayer lamp_lights = lamps;
  lamp_lights.emission = false;
  VkrBakeGpuLayer emission;
  emission.emission = true;
  std::printf("layers sun_key_lights=%zu lamp_lights=%zu sky=%d\n",
              sun_key.lights.size(), lamps.lights.size(), sky ? 1 : 0);

  VkrBakeGpuGatherSettings settings;
  settings.samples = options.samples;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = rr_start_depth(options);
  settings.seed = options.seed;

  struct NamedLayer {
    const char *name;
    const VkrBakeGpuLayer *layer;
    double seconds;
    uint64_t texels;
  };
  NamedLayer layers[] = {{"parity", &parity, 0.0, 0u},
                         {"sun_key0", &sun_key, 0.0, 0u},
                         {"lamps0", &lamps, 0.0, 0u},
                         {"lamp_lights0", &lamp_lights, 0.0, 0u},
                         {"emission0", &emission, 0.0, 0u}};
  const uint32_t page_end = std::min(options.pages, layout.page_count);
  for (uint32_t page = 0u; page < page_end; ++page) {
    std::vector<VkrBakeLightmapTexel> texels;
    if (!vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          layout, page, &texels)) {
      vkr_bake_gpu_destroy(gpu);
      return 1;
    }
    VkrBakeLightmapNeighbors neighbors;
    if (!vkr_bake_lightmap_neighbors(
            layout.page_size, texels,
            kOutlierNeighborTexels / options.texels_per_unit, &neighbors)) {
      vkr_bake_gpu_destroy(gpu);
      return 1;
    }
    for (NamedLayer &named : layers) {
      VkrBakeGpuGatherResult gathered;
      if (!vkr_bake_gpu_gather(gpu, texels, *named.layer, settings,
                               &gathered)) {
        vkr_bake_gpu_destroy(gpu);
        return 1;
      }
      std::vector<Vec3> irradiance = gathered_total(gathered);
      const double seconds = gathered.gpu_seconds;
      /* Counted, not replaced: the parity check compares raw transport. */
      const VkrBakeLightmapOutliers outliers =
          vkr_bake_lightmap_reject_outliers(
              neighbors,
              options.outlier_ratio > 0.0f ? options.outlier_ratio
                                           : kOutlierRatio,
              kOutlierMinNeighbors, false, &irradiance);
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
                  "paths_per_s=%.3g mean_luminance=%.5g nonfinite=%llu "
                  "outliers=%llu outlier_energy=%.4f\n",
                  page, named.name, texels.size(), seconds,
                  (double)texels.size() * options.samples / seconds, mean,
                  (unsigned long long)nonfinite,
                  (unsigned long long)outliers.texels,
                  outliers.energy_fraction);
      std::fflush(stdout);
      if (!report_encoding(options, layout, page, texels, irradiance,
                           named.name)) {
        vkr_bake_gpu_destroy(gpu);
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
    VkrBakeGpuGatherSettings check_settings = settings;
    check_settings.samples = options.check_samples;
    /* The check layer and a CPU integrator restricted to the same transport. */
    VkrBakeGpuLayer check_layer = parity;
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
    VkrBakeGpuGatherResult check_gathered;
    if (!vkr_bake_gpu_gather(gpu, subset, check_layer, check_settings,
                             &check_gathered)) {
      vkr_bake_gpu_destroy(gpu);
      return 1;
    }
    const std::vector<Vec3> gpu_values = gathered_total(check_gathered);
    const auto cpu_start = std::chrono::steady_clock::now();
    std::vector<Vec3> cpu_values;
    std::vector<float64_t> variance;
    if (!cpu_reference(options, check_integrator, subset, &cpu_values,
                       &variance)) {
      std::fprintf(stderr, "CPU reference transport failed\n");
      vkr_bake_gpu_destroy(gpu);
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
    VkrBakeGpuLayer lamps_bounce = lamps;
    lamps_bounce.texel_direct = false;
    VkrBakeGpuGatherResult sun_gathered;
    VkrBakeGpuGatherResult lamp_gathered;
    if (!vkr_bake_gpu_gather(gpu, subset, sun_key, check_settings,
                             &sun_gathered) ||
        !vkr_bake_gpu_gather(gpu, subset, lamps_bounce, check_settings,
                             &lamp_gathered)) {
      vkr_bake_gpu_destroy(gpu);
      return 1;
    }
    const std::vector<Vec3> sun_values = gathered_total(sun_gathered);
    const std::vector<Vec3> lamp_values = gathered_total(lamp_gathered);
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
  vkr_bake_gpu_destroy(gpu);
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
/* Whether a layer the host's pipeline class bakes holds any light: every
   layer on the tiled class, which keeps the full layer table, and on the
   desktop class a lamp group with static lights, emission the scene has, or
   the sky. The default group exists for emission alone, so without this a
   scene with neither lamps nor emission baked one black layer. */
bool host_bakes_layer(const VkrBakeScene &scene, const VkrBakeLayerPlan &plan,
                      bool desktop) {
  if (!desktop) {
    return true;
  }
  if (plan.record.kind != VKR_LIGHT_LAYER_LAMP_GROUP) {
    return false;
  }
  if (!plan.lights.empty() || plan.sky) {
    return true;
  }
  if (plan.emission) {
    for (const VkrBakeMaterial &material : scene.materials) {
      const Vec3 emission = material.emissive_factor;
      if (emission.x > 0.0f || emission.y > 0.0f || emission.z > 0.0f) {
        return true;
      }
    }
  }
  return false;
}

bool write_manifest(const Options &options, const VkrBakeScene &scene,
                    const VkrBakeLightmapLayout &layout) {
  /* The layers this host's pipeline class bakes: every layer on the tiled
     class, the lamp groups on the desktop class. */
  std::vector<Vec3> sun_keys;
  if (scene.atmosphere.enabled &&
      scene.atmosphere_light < scene.lights.size()) {
    sun_keys = vkr_bake_sun_key_directions(scene);
  }
  std::vector<VkrBakeLayerPlan> plans;
  std::string plan_error;
  if (!vkr_bake_plan_layers(scene, sun_keys, &plans, &plan_error)) {
    std::fprintf(stderr, "%s\n", plan_error.c_str());
    return false;
  }
  const bool desktop = options.pipeline == PipelineClass::Desktop;
  size_t baked_layers = 0u;
  for (const VkrBakeLayerPlan &plan : plans) {
    if (host_bakes_layer(scene, plan, desktop)) {
      ++baked_layers;
    }
  }
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
           << ",\"pipeline\":\"" << (desktop ? "desktop" : "tiled") << '"'
           << ",\"baked_layers\":" << baked_layers
           << ",\"pages\":" << layout.page_count
           << ",\"page_size\":" << layout.page_size << "}\n";
  return write_atomic(options.manifest, manifest.str());
}

/* One baked layer: its record in the set, the transport that fills it, and
   for a sun key the index of its atmosphere. */
struct BakeLayer {
  VkrLightLayer record;
  VkrBakeGpuLayer transport;
  int32_t sun_key = -1;
};

/* A planned layer as the GPU baker gathers it: lamp groups also hold their
   lights' direct term at the texel. */
BakeLayer gpu_layer(const VkrBakeLayerPlan &plan) {
  BakeLayer layer = {};
  layer.record = plan.record;
  layer.transport.lights = plan.lights;
  layer.transport.sky = plan.sky;
  layer.transport.emission = plan.emission;
  layer.transport.texel_direct = plan.record.kind == VKR_LIGHT_LAYER_LAMP_GROUP;
  layer.sun_key = plan.sun_key;
  return layer;
}

/*
 * The lightmapped instances a lamp of a lamp-direct layer may light: those
 * whose world bounds a point or spot lamp's range reaches, and every one
 * when a lamp has no range or is a rectangle.
 */
std::vector<VkrBakeLightmapInstance>
lamp_lit_instances(const VkrBakeScene &scene,
                   const std::vector<BakeLayer> &layers,
                   const std::vector<uint8_t> &lamp_direct) {
  std::vector<uint32_t> lights;
  bool unbounded = false;
  for (size_t l = 0u; l < layers.size(); ++l) {
    if (!lamp_direct[l]) {
      continue;
    }
    for (uint32_t light : layers[l].transport.lights) {
      const VkrBakeSceneLight &lamp = scene.lights[light];
      const bool bounded = (lamp.kind == VkrBakeSceneLightKind::Point ||
                            lamp.kind == VkrBakeSceneLightKind::Spot) &&
                           lamp.range > 0.0f;
      unbounded = unbounded || !bounded;
      lights.push_back(light);
    }
  }
  if (unbounded) {
    return scene.lightmap_instances;
  }
  uint32_t source_count = 0u;
  for (const VkrBakeLightmapInstance &instance : scene.lightmap_instances) {
    source_count = std::max(source_count, instance.source_instance_index + 1u);
  }
  std::vector<Vec3> box_min(source_count,
                            vec3_new(INFINITY, INFINITY, INFINITY));
  std::vector<Vec3> box_max(source_count,
                            vec3_new(-INFINITY, -INFINITY, -INFINITY));
  for (const VkrBakeTriangle &triangle : scene.triangles) {
    const uint32_t s = triangle.source_instance_index;
    if (s >= source_count) {
      continue;
    }
    for (const VkrBakeVertex &vertex : triangle.vertex) {
      box_min[s] = vec3_new(std::min(box_min[s].x, vertex.position.x),
                            std::min(box_min[s].y, vertex.position.y),
                            std::min(box_min[s].z, vertex.position.z));
      box_max[s] = vec3_new(std::max(box_max[s].x, vertex.position.x),
                            std::max(box_max[s].y, vertex.position.y),
                            std::max(box_max[s].z, vertex.position.z));
    }
  }
  std::vector<VkrBakeLightmapInstance> lit;
  for (const VkrBakeLightmapInstance &instance : scene.lightmap_instances) {
    const uint32_t s = instance.source_instance_index;
    for (uint32_t light : lights) {
      const VkrBakeSceneLight &lamp = scene.lights[light];
      const Vec3 p = lamp.position;
      const float32_t dx =
          std::max(std::max(box_min[s].x - p.x, 0.0f), p.x - box_max[s].x);
      const float32_t dy =
          std::max(std::max(box_min[s].y - p.y, 0.0f), p.y - box_max[s].y);
      const float32_t dz =
          std::max(std::max(box_min[s].z - p.z, 0.0f), p.z - box_max[s].z);
      if (dx * dx + dy * dy + dz * dz < lamp.range * lamp.range) {
        lit.push_back(instance);
        break;
      }
    }
  }
  return lit;
}

/*
 * Bakes and streams the lamp-direct pages (ADR-108): per page, each lamp
 * group's direct light at the texels of the lamp-direct layout, gathered
 * without indirect light. Buried texels, found by the first group's
 * first-hit back faces, take their neighbors' light; nothing is smoothed,
 * so shadow edges keep the footprint grid's and the emitter's own softness.
 */
bool bake_direct_pages(const Options &options, VkrBakeScene &scene,
                       VkrBakeGpuContext *gpu,
                       const std::vector<BakeLayer> &layers,
                       const std::vector<uint8_t> &lamp_direct,
                       const VkrBakeLightmapLayout &direct_layout,
                       uint32_t threads, std::ofstream *stream,
                       uint32_t *payload_crc, double *out_gpu_seconds) {
  (void)scene;
  VkrBakeGpuGatherSettings settings;
  /* Rectangle lights take one footprint point per sample. */
  settings.samples = kDirectSamples;
  settings.max_depth = 1u;
  settings.rr_start_depth = 0u;
  settings.seed = options.seed;
  settings.direct_only = true;
  std::vector<float32_t> rgba;
  std::vector<uint8_t> blocks;
  for (uint32_t page = 0u; page < direct_layout.page_count; ++page) {
    std::vector<VkrBakeLightmapTexel> texels;
    VkrBakeLightmapNeighbors neighbors;
    if (!vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          direct_layout, page, &texels) ||
        !vkr_bake_lightmap_neighbors(direct_layout.page_size, texels,
                                     kOutlierNeighborTexels /
                                         options.direct_texels_per_unit,
                                     &neighbors)) {
      return false;
    }
    std::vector<uint8_t> valid(texels.size(), 1u);
    VkrBakeLightmapFill fill;
    bool planned = false;
    for (size_t l = 0u; l < layers.size(); ++l) {
      if (!lamp_direct[l]) {
        continue;
      }
      VkrBakeGpuLayer transport = layers[l].transport;
      transport.texel_direct = true;
      transport.sky = false;
      transport.emission = false;
      settings.backface = !planned && options.buried_backface > 0.0f;
      VkrBakeGpuGatherResult gathered;
      if (!vkr_bake_gpu_gather(gpu, texels, transport, settings, &gathered)) {
        return false;
      }
      *out_gpu_seconds += gathered.gpu_seconds;
      if (!planned) {
        for (size_t i = 0u; settings.backface && i < texels.size(); ++i) {
          if (gathered.backface[i] > options.buried_backface) {
            valid[i] = 0u;
          }
        }
        if (!vkr_bake_lightmap_plan_fill(neighbors, valid, &fill)) {
          return false;
        }
        planned = true;
      }
      vkr_bake_lightmap_apply_fill(fill, &gathered.direct);
      if (!vkr_bake_lightmap_compose_page(direct_layout, page, texels,
                                          gathered.direct, nullptr,
                                          options.dilation_passes, &rgba) ||
          !vkr_bake_lightmap_encode_astc_hdr(rgba, direct_layout.page_size,
                                             options.astc_effort, threads,
                                             &blocks)) {
        return false;
      }
      *payload_crc = vkr_crc32_update(*payload_crc, blocks.data(),
                                      (uint64_t)blocks.size());
      stream->write((const char *)blocks.data(),
                    (std::streamsize)blocks.size());
      if (!*stream) {
        return false;
      }
      float64_t luminance_sum = 0.0;
      for (const Vec3 &value : gathered.direct) {
        luminance_sum += luminance(value);
      }
      std::printf("direct page=%u/%u lamp_group=%s texels=%zu gpu_s=%.2f "
                  "mean_luminance=%.5g\n",
                  page + 1u, direct_layout.page_count, layers[l].record.name,
                  texels.size(), gathered.gpu_seconds,
                  texels.empty() ? 0.0 : luminance_sum / texels.size());
      std::fflush(stdout);
    }
  }
  return true;
}

/*
 * Turns a gathered layer into the light its page publishes. Buried texels
 * take their same-surface neighbors' light first. With denoising, the a-trous
 * filter removes the indirect light's noise, while the direct light, exact
 * but point-sampled per texel, only gets the smoothing passes; then the two
 * are added. Its noise-scaled luminance weights spread a lone high-energy
 * path over its neighbors, so outlier rejection, which discarded 4.5% of the
 * synthetic room's floor indirect light without lowering its noise, does not
 * run. Without denoising, outlier rejection and smoothing work on the sum.
 * `gathered` keeps the processed direct and indirect light.
 */
bool finish_layer(const Options &options, uint32_t page_size,
                  const std::vector<VkrBakeLightmapTexel> &texels,
                  const VkrBakeLightmapNeighbors &neighbors,
                  const std::vector<uint8_t> &valid,
                  const VkrBakeLightmapFill &fill,
                  VkrBakeGpuGatherResult *gathered,
                  std::vector<Vec3> *out_irradiance,
                  VkrBakeLightmapOutliers *out_outliers) {
  vkr_bake_lightmap_apply_fill(fill, &gathered->direct);
  vkr_bake_lightmap_apply_fill(fill, &gathered->indirect);
  vkr_bake_lightmap_apply_fill(fill, &gathered->indirect_variance);
  if (!options.denoise) {
    *out_irradiance = gathered_total(*gathered);
    if (options.outlier_ratio > 0.0f) {
      *out_outliers = vkr_bake_lightmap_reject_outliers(
          neighbors, options.outlier_ratio, kOutlierMinNeighbors, true,
          out_irradiance);
    }
    return vkr_bake_lightmap_smooth(neighbors, texels, options.smooth_passes,
                                    out_irradiance);
  }
  VkrBakeLightmapDenoiseSettings denoise;
  denoise.iterations = options.denoise_iterations;
  denoise.texels_per_unit = options.texels_per_unit;
  denoise.threads = options.threads;
  if (options.denoise_sigma > 0.0f) {
    denoise.luminance_sigma = options.denoise_sigma;
  }
  if (!vkr_bake_lightmap_denoise(
          page_size, texels, &valid,
          options.denoise_sigma > 0.0f ? &gathered->indirect_variance : nullptr,
          denoise, &gathered->indirect) ||
      !vkr_bake_lightmap_smooth(neighbors, texels, options.smooth_passes,
                                &gathered->direct)) {
    return false;
  }
  *out_irradiance = gathered_total(*gathered);
  return true;
}

/* Appends a published layer's texels to the diagnostic CSV. */
bool dump_layer(std::ofstream *dump, uint32_t page, uint32_t layer,
                const std::vector<VkrBakeLightmapTexel> &texels,
                const std::vector<uint8_t> &valid,
                const VkrBakeGpuGatherResult &gathered,
                const std::vector<Vec3> &irradiance,
                const std::vector<Vec3> *directions) {
  char line[640];
  for (size_t i = 0u; i < texels.size(); ++i) {
    const VkrBakeLightmapTexel &texel = texels[i];
    const Vec3 direction = directions ? (*directions)[i] : vec3_zero();
    const Vec3 direct = gathered.direct[i];
    const Vec3 indirect = gathered.indirect[i];
    const Vec3 total = irradiance[i];
    const int length = std::snprintf(
        line, sizeof(line),
        "%u,%u,%u,%u,%.6g,%.6g,%.6g,%.4g,%.4g,%.4g,%u,%.6g,%.6g,%.6g,%.6g,"
        "%.6g,%.6g,%.6g,%.6g,%.6g,%.4g,%.4g,%.4g,%.4g\n",
        page, layer, texel.x, texel.y, texel.position.x, texel.position.y,
        texel.position.z, texel.normal.x, texel.normal.y, texel.normal.z,
        (unsigned)valid[i], direct.x, direct.y, direct.z, indirect.x,
        indirect.y, indirect.z, total.x, total.y, total.z, direction.x,
        direction.y, direction.z, vec3_length(direction));
    if (length <= 0 || (size_t)length >= sizeof(line)) {
      return false;
    }
    dump->write(line, length);
  }
  return (bool)*dump;
}

/* Packs a composed float RGBA page into little-endian RGB9E5 texels; alpha,
   the ambient visibility, stays in the ASTC plane only. */
void encode_rgb9e5(const std::vector<float32_t> &rgba,
                   std::vector<uint8_t> *out_bytes) {
  const size_t texels = rgba.size() / 4u;
  out_bytes->resize(texels * 4u);
  for (size_t i = 0u; i < texels; ++i) {
    vkr_store_le_u32(out_bytes->data() + i * 4u,
                     vkr_rgb9e5_from_float3(rgba[i * 4u], rgba[i * 4u + 1u],
                                            rgba[i * 4u + 2u]));
  }
}

/* One BC plane's encode time and its error against the page it encodes,
   measured on the GPU's decode of its blocks. */
struct BcReport {
  /* Wall time of the encode, upload and readback included, and the GPU
     time of its dispatches. */
  double encode_seconds = 0.0;
  double gpu_seconds = 0.0;
  /* BC6H: RMS over RGB in linear units, and relative to the page's RMS
     value. BC7: RMS over RGBA in UNORM units, and the largest error in
     1/255 steps. */
  float64_t rms = 0.0;
  float64_t relative_rms = 0.0;
  uint32_t max_steps = 0u;
};

/* Packs a composed float RGBA page into BC6H blocks on the GPU; alpha, the
   ambient visibility, is not encoded. */
bool encode_bc6h(VkrBakeGpuContext *gpu, const std::vector<float32_t> &rgba,
                 uint32_t page_size, std::vector<uint8_t> *out_blocks,
                 BcReport *out_report) {
  const auto start = std::chrono::steady_clock::now();
  if (!vkr_bake_gpu_encode_bc6h(gpu, rgba.data(), page_size, page_size,
                                out_blocks, &out_report->gpu_seconds)) {
    return false;
  }
  out_report->encode_seconds = seconds_since(start);
  std::vector<float32_t> decoded;
  if (!vkr_bake_gpu_decode_bc(gpu, VkrBakeGpuBcFormat::Bc6hUfloat,
                              out_blocks->data(), page_size, page_size,
                              &decoded)) {
    return false;
  }
  /* Against the values the encoder takes: negative and NaN as zero, the
     rest at most the largest half float. */
  float64_t error = 0.0;
  float64_t signal = 0.0;
  const size_t texels = rgba.size() / 4u;
  for (size_t i = 0u; i < texels; ++i) {
    for (size_t c = 0u; c < 3u; ++c) {
      const float32_t value = rgba[i * 4u + c];
      const float64_t source = value > 0.0f ? std::fmin(value, 65504.0f) : 0.0;
      const float64_t difference = (float64_t)decoded[i * 4u + c] - source;
      error += difference * difference;
      signal += source * source;
    }
  }
  out_report->rms = texels ? std::sqrt(error / (float64_t)(texels * 3u)) : 0.0;
  out_report->relative_rms = signal > 0.0 ? std::sqrt(error / signal) : 0.0;
  return true;
}

/* Packs an RGBA8 page into BC7 blocks on the GPU. */
bool encode_bc7(VkrBakeGpuContext *gpu, const std::vector<uint8_t> &rgba8,
                uint32_t page_size, std::vector<uint8_t> *out_blocks,
                BcReport *out_report) {
  const auto start = std::chrono::steady_clock::now();
  if (!vkr_bake_gpu_encode_bc7(gpu, rgba8.data(), page_size, page_size,
                               out_blocks, &out_report->gpu_seconds)) {
    return false;
  }
  out_report->encode_seconds = seconds_since(start);
  std::vector<float32_t> decoded;
  if (!vkr_bake_gpu_decode_bc(gpu, VkrBakeGpuBcFormat::Bc7, out_blocks->data(),
                              page_size, page_size, &decoded)) {
    return false;
  }
  float64_t error = 0.0;
  uint32_t max_steps = 0u;
  for (size_t i = 0u; i < rgba8.size(); ++i) {
    const float64_t value = decoded[i] * 255.0;
    const float64_t difference = value - (float64_t)rgba8[i];
    error += difference * difference;
    max_steps =
        std::max(max_steps, (uint32_t)std::lround(std::fabs(difference)));
  }
  out_report->rms =
      rgba8.empty() ? 0.0 : std::sqrt(error / (float64_t)rgba8.size()) / 255.0;
  out_report->max_steps = max_steps;
  return true;
}

/* Packs a composed float RGBA page in [0, 1] into RGBA8 texels, R, G, B, A
   in byte order. */
void encode_rgba8(const std::vector<float32_t> &rgba,
                  std::vector<uint8_t> *out_bytes) {
  out_bytes->resize(rgba.size());
  for (size_t i = 0u; i < rgba.size(); ++i) {
    const float32_t value = std::fmin(std::fmax(rgba[i], 0.0f), 1.0f);
    (*out_bytes)[i] = (uint8_t)std::lround(value * 255.0f);
  }
}

/*
 * A lamp layer's per-texel mean incident direction (desktop baked lamps
 * proposal): the gathered luminance-weighted direction sum over its luminance
 * sum, zero where nothing lit the texel. Buried texels take their neighbors'
 * directions and the direct light's smoothing passes run over the vectors,
 * whose length, the directionality, shrinks where neighbors disagree.
 * Returns the mean directionality.
 */
bool finish_directions(const Options &options,
                       const std::vector<VkrBakeLightmapTexel> &texels,
                       const VkrBakeLightmapNeighbors &neighbors,
                       const VkrBakeLightmapFill &fill,
                       const VkrBakeGpuGatherResult &gathered,
                       std::vector<Vec3> *out_directions,
                       float64_t *out_mean_directionality) {
  out_directions->assign(texels.size(), vec3_zero());
  for (size_t i = 0u; i < texels.size(); ++i) {
    const Vec4 sum = gathered.direction[i];
    if (sum.w > 1.0e-12f && std::isfinite(sum.w)) {
      (*out_directions)[i] =
          vec3_scale(vec3_new(sum.x, sum.y, sum.z), 1.0f / sum.w);
    }
  }
  vkr_bake_lightmap_apply_fill(fill, out_directions);
  if (!vkr_bake_lightmap_smooth(neighbors, texels, options.smooth_passes,
                                out_directions)) {
    return false;
  }
  float64_t sum = 0.0;
  for (const Vec3 &direction : *out_directions) {
    sum += std::fmin(vec3_length(direction), 1.0f);
  }
  *out_mean_directionality = texels.empty() ? 0.0 : sum / texels.size();
  return true;
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
  if (!vkr_bake_gpu_available()) {
    std::fprintf(stderr, "Lightmap bakes need GPU ray tracing, which this "
                         "host does not have\n");
    return 1;
  }

  /* Each sun key's atmosphere is built once and made current before the key's
     layer of every page. */
  std::vector<Vec3> sun_keys;
  std::vector<VkrBakeAtmosphere> key_atmospheres;
  /* The keys' directions decide which lights the lamp groups hold, so both
     classes plan with them; only the tiled class bakes the keys and needs
     their atmospheres. */
  const bool desktop = options.pipeline == PipelineClass::Desktop;
  if (scene.atmosphere.enabled &&
      scene.atmosphere_light < scene.lights.size()) {
    sun_keys = vkr_bake_sun_key_directions(scene);
  }
  if (!desktop && !sun_keys.empty()) {
    const auto atmosphere_start = std::chrono::steady_clock::now();
    key_atmospheres.resize(sun_keys.size());
    for (size_t k = 0u; k < sun_keys.size(); ++k) {
      if (!vkr_bake_scene_build_sun_atmosphere(&scene, sun_keys[k],
                                               &key_atmospheres[k])) {
        std::fprintf(stderr, "Building the atmosphere of sun key %zu failed\n",
                     k);
        return 1;
      }
    }
    std::printf("sun_keys=%zu atmosphere_s=%.2f\n", sun_keys.size(),
                seconds_since(atmosphere_start));
  }
  std::vector<VkrBakeLayerPlan> plans;
  std::string plan_error;
  if (!vkr_bake_plan_layers(scene, sun_keys, &plans, &plan_error)) {
    std::fprintf(stderr, "%s\n", plan_error.c_str());
    return 1;
  }
  std::vector<BakeLayer> layers;
  for (const VkrBakeLayerPlan &plan : plans) {
    if (host_bakes_layer(scene, plan, desktop)) {
      layers.push_back(gpu_layer(plan));
    }
  }
  if (layers.empty()) {
    std::fprintf(stderr,
                 "The scene has no static lamps or emission, so a desktop "
                 "lightmap set has nothing to hold\n");
    return 3;
  }
  std::vector<VkrLightLayer> layer_records;
  for (const BakeLayer &layer : layers) {
    layer_records.push_back(layer.record);
  }
  /* On the tiled class, a lamp group with lights takes its direct light on
     the lamp-direct pages and keeps bounce, sky and emission in its
     irradiance (ADR-108). */
  std::vector<uint8_t> lamp_direct(layers.size(), 0u);
  bool any_lamp_direct = false;
  for (size_t l = 0u;
       !desktop && options.direct_texels_per_unit > 0.0f && l < layers.size();
       ++l) {
    if (layers[l].record.kind == VKR_LIGHT_LAYER_LAMP_GROUP &&
        !layers[l].transport.lights.empty()) {
      lamp_direct[l] = 1u;
      layers[l].transport.texel_direct = false;
      any_lamp_direct = true;
    }
  }
  VkrBakeLightmapLayout direct_layout;
  if (any_lamp_direct &&
      !vkr_bake_lightmap_pack(lamp_lit_instances(scene, layers, lamp_direct),
                              layout.page_size, options.direct_texels_per_unit,
                              &direct_layout)) {
    std::fprintf(stderr, "Lamp-direct packing failed\n");
    return 1;
  }
  /* No lamp reaches a lightmapped instance: the lamp groups keep their
     direct light. */
  if (any_lamp_direct && direct_layout.page_count == 0u) {
    for (size_t l = 0u; l < layers.size(); ++l) {
      if (lamp_direct[l]) {
        lamp_direct[l] = 0u;
        layers[l].transport.texel_direct = true;
      }
    }
    any_lamp_direct = false;
  }
  std::vector<uint32_t> lightmap_by_source(layout.rect_by_instance.size(),
                                           UINT32_MAX);
  for (uint32_t i = 0u; i < scene.lightmap_instances.size(); ++i) {
    lightmap_by_source[scene.lightmap_instances[i].source_instance_index] = i;
  }
  /* Each set instance with its source instance index, in set order. */
  std::vector<std::pair<VkrLightmapInstance, uint32_t>> keyed;
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
    keyed.emplace_back(instance, rect.source_instance_index);
  }
  std::sort(keyed.begin(), keyed.end(),
            [](const std::pair<VkrLightmapInstance, uint32_t> &a,
               const std::pair<VkrLightmapInstance, uint32_t> &b) {
              if (a.first.entity_index != b.first.entity_index) {
                return a.first.entity_index < b.first.entity_index;
              }
              return a.first.instance_index < b.first.instance_index;
            });
  std::vector<VkrLightmapInstance> instances;
  std::vector<VkrLightmapDirectRect> direct_rects;
  for (const auto &entry : keyed) {
    instances.push_back(entry.first);
    /* An instance no lamp reaches has an empty lamp-direct rectangle. */
    const uint32_t direct_index =
        any_lamp_direct && entry.second < direct_layout.rect_by_instance.size()
            ? direct_layout.rect_by_instance[entry.second]
            : UINT32_MAX;
    if (any_lamp_direct && direct_index != UINT32_MAX) {
      const VkrBakeLightmapRect &rect = direct_layout.rects[direct_index];
      direct_rects.push_back(
          {rect.page, rect.x, rect.y, rect.width, rect.height});
    } else if (any_lamp_direct) {
      direct_rects.push_back({0u, 0u, 0u, 0u, 0u});
    }
  }
  if (any_lamp_direct) {
    uint64_t direct_texels = 0u;
    for (const VkrBakeLightmapRect &rect : direct_layout.rects) {
      direct_texels += (uint64_t)rect.width * rect.height;
    }
    std::printf("direct_pages=%u direct_texels_per_unit=%g "
                "direct_rect_texels=%llu\n",
                direct_layout.page_count, options.direct_texels_per_unit,
                (unsigned long long)direct_texels);
  }

  const auto setup_start = std::chrono::steady_clock::now();
  std::unique_ptr<VkrBakeGpuContext, decltype(&vkr_bake_gpu_destroy)> gpu(
      vkr_bake_gpu_create(scene), vkr_bake_gpu_destroy);
  if (!gpu) {
    return 1;
  }
  std::printf("gpu_setup_s=%.2f layers=%zu\n", seconds_since(setup_start),
              layers.size());
  bool desktop_bc = false;
  if (desktop) {
    const bool bc_available = vkr_bake_gpu_bc_available(gpu.get());
    if (options.desktop_encoding == DesktopEncoding::Bc && !bc_available) {
      std::fprintf(stderr,
                   "--desktop-encoding bc needs the Vulkan BC encoders, which "
                   "this host's GPU does not run; bake with "
                   "--desktop-encoding uncompressed\n");
      return 1;
    }
    desktop_bc = options.desktop_encoding == DesktopEncoding::Bc ||
                 (options.desktop_encoding == DesktopEncoding::Automatic &&
                  bc_available);
  }
  std::printf("pipeline=%s desktop_encoding=%s\n",
              desktop ? "desktop" : "tiled",
              desktop ? (desktop_bc ? "bc" : "uncompressed") : "none");

  /* The planes of the set's pipeline class (PipelineClass), each page in
     this order: on the tiled class every layer's ASTC 4x4 HDR irradiance;
     on the desktop class, whose layers are the lamp groups, each one's
     irradiance (RGB9E5 or BC6H) and incident direction (RGBA8 or BC7;
     ADR-104). */
  std::vector<VkrLightmapPlane> planes;
  for (uint32_t l = 0u; l < layer_records.size(); ++l) {
    if (!desktop) {
      planes.push_back(
          {l, VKR_LIGHTMAP_PLANE_IRRADIANCE, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR});
      if (lamp_direct[l]) {
        planes.push_back({l, VKR_LIGHTMAP_PLANE_LAMP_DIRECT,
                          VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR});
      }
      continue;
    }
    planes.push_back(
        {l, VKR_LIGHTMAP_PLANE_IRRADIANCE,
         desktop_bc ? VKR_LIGHTMAP_FORMAT_BC6H : VKR_LIGHTMAP_FORMAT_RGB9E5});
    planes.push_back(
        {l, VKR_LIGHTMAP_PLANE_DIRECTION,
         desktop_bc ? VKR_LIGHTMAP_FORMAT_BC7 : VKR_LIGHTMAP_FORMAT_RGBA8});
  }
  VkrLightmapSet set = {};
  set.page_size = layout.page_size;
  set.page_count = layout.page_count;
  set.layer_count = (uint32_t)layer_records.size();
  set.plane_count = (uint32_t)planes.size();
  set.instance_count = (uint32_t)instances.size();
  set.texels_per_unit = options.texels_per_unit;
  set.layers = layer_records.data();
  set.planes = planes.data();
  set.instances = instances.data();
  set.direct_page_count = any_lamp_direct ? direct_layout.page_count : 0u;
  set.direct_texels_per_unit = options.direct_texels_per_unit;
  set.direct_rects = direct_rects.data();
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

  VkrBakeGpuGatherSettings settings;
  settings.samples = options.samples;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = rr_start_depth(options);
  settings.seed = options.seed;
  settings.occlusion_radius = options.ao_radius;
  settings.indirect_clamp = options.indirect_clamp;
  const uint32_t threads =
      options.threads ? options.threads
                      : std::max(1u, std::thread::hardware_concurrency());
  std::ofstream dump;
  if (options.dump_texels) {
    dump.open(vkr_filesystem_native_utf8_path(options.dump_texels),
              std::ios::binary | std::ios::trunc);
    dump << "page,layer,x,y,px,py,pz,nx,ny,nz,valid,direct_r,direct_g,"
            "direct_b,indirect_r,indirect_g,indirect_b,r,g,b,dir_x,dir_y,"
            "dir_z,directionality\n";
  }

  uint32_t payload_crc = VKR_CRC32_INITIAL;
  bool ok = !options.dump_texels || (bool)dump;
  double gpu_seconds = 0.0;
  double encode_seconds = 0.0;
  double cleanup_seconds = 0.0;
  std::vector<float32_t> rgba;
  std::vector<uint8_t> blocks;
  for (uint32_t page = 0u; ok && page < layout.page_count; ++page) {
    std::vector<VkrBakeLightmapTexel> texels;
    VkrBakeLightmapNeighbors neighbors;
    ok = vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          layout, page, &texels) &&
         vkr_bake_lightmap_neighbors(
             layout.page_size, texels,
             kOutlierNeighborTexels / options.texels_per_unit, &neighbors);
    /* The page's ambient occlusion and buried texels come from its first
       layer's first-bounce rays; occlusion goes into every layer's alpha, so
       whichever layers a frame weighs carry it (ADR-088), and every layer
       fills the same buried texels. */
    std::vector<float32_t> occlusion;
    std::vector<uint8_t> valid(texels.size(), 1u);
    VkrBakeLightmapFill fill;
    /* Occlusion rides in the ASTC planes' alpha; desktop planes have
       none. */
    const bool bake_occlusion = !desktop && options.ao_radius > 0.0f;
    const bool find_buried = options.buried_backface > 0.0f;
    for (uint32_t l = 0u; ok && l < layers.size(); ++l) {
      if (layers[l].sun_key >= 0) {
        vkr_bake_scene_use_atmosphere(
            &scene, key_atmospheres[(size_t)layers[l].sun_key]);
        ok = vkr_bake_gpu_update_lighting(gpu.get(), scene);
        if (!ok) {
          break;
        }
      }
      settings.occlusion = bake_occlusion && l == 0u;
      settings.backface = find_buried && l == 0u;
      /* Every desktop layer is a lamp group with a direction plane. */
      settings.direction = desktop;
      VkrBakeGpuGatherResult gathered;
      ok = vkr_bake_gpu_gather(gpu.get(), texels, layers[l].transport, settings,
                               &gathered);
      gpu_seconds += gathered.gpu_seconds;
      if (ok && l == 0u) {
        uint64_t buried = 0u;
        for (size_t i = 0u; find_buried && i < texels.size(); ++i) {
          if (gathered.backface[i] > options.buried_backface) {
            valid[i] = 0u;
            ++buried;
          }
        }
        ok = vkr_bake_lightmap_plan_fill(neighbors, valid, &fill);
        if (ok && find_buried) {
          std::printf("buried page=%u/%u texels=%llu filled=%zu "
                      "unfilled=%llu\n",
                      page + 1u, layout.page_count, (unsigned long long)buried,
                      fill.texels.size(), (unsigned long long)fill.unfilled);
        }
      }
      if (ok && bake_occlusion && l == 0u) {
        occlusion = std::move(gathered.occlusion);
        vkr_bake_lightmap_apply_fill(fill, &occlusion);
        std::vector<Vec3> smoothed(occlusion.size());
        for (size_t i = 0u; i < occlusion.size(); ++i) {
          smoothed[i] = vec3_new(occlusion[i], occlusion[i], occlusion[i]);
        }
        ok = vkr_bake_lightmap_smooth(neighbors, texels, options.smooth_passes,
                                      &smoothed);
        for (size_t i = 0u; ok && i < occlusion.size(); ++i) {
          occlusion[i] = smoothed[i].x;
        }
      }
      const auto cleanup_start = std::chrono::steady_clock::now();
      std::vector<Vec3> irradiance;
      VkrBakeLightmapOutliers outliers = {};
      ok = ok && finish_layer(options, layout.page_size, texels, neighbors,
                              valid, fill, &gathered, &irradiance, &outliers);
      cleanup_seconds += seconds_since(cleanup_start);
      const auto encode_start = std::chrono::steady_clock::now();
      BcReport irradiance_bc;
      ok = ok &&
           vkr_bake_lightmap_compose_page(layout, page, texels, irradiance,
                                          bake_occlusion ? &occlusion : nullptr,
                                          options.dilation_passes, &rgba);
      if (ok && !desktop) {
        ok = vkr_bake_lightmap_encode_astc_hdr(
            rgba, layout.page_size, options.astc_effort, threads, &blocks);
      } else if (ok && desktop_bc) {
        ok = encode_bc6h(gpu.get(), rgba, layout.page_size, &blocks,
                         &irradiance_bc);
      } else if (ok) {
        encode_rgb9e5(rgba, &blocks);
      }
      encode_seconds += seconds_since(encode_start);
      if (!ok) {
        break;
      }
      payload_crc =
          vkr_crc32_update(payload_crc, blocks.data(), (uint64_t)blocks.size());
      stream.write((const char *)blocks.data(), (std::streamsize)blocks.size());
      std::vector<Vec3> directions;
      float64_t mean_directionality = 0.0;
      BcReport direction_bc;
      if (desktop) {
        const auto direction_start = std::chrono::steady_clock::now();
        std::vector<uint8_t> rgba8;
        ok = finish_directions(options, texels, neighbors, fill, gathered,
                               &directions, &mean_directionality) &&
             vkr_bake_lightmap_compose_direction_page(
                 layout, page, texels, directions, options.dilation_passes,
                 &rgba);
        if (ok) {
          encode_rgba8(rgba, &rgba8);
        }
        if (ok && desktop_bc) {
          ok = encode_bc7(gpu.get(), rgba8, layout.page_size, &blocks,
                          &direction_bc);
          rgba8.swap(blocks);
        }
        encode_seconds += seconds_since(direction_start);
        if (!ok) {
          break;
        }
        payload_crc =
            vkr_crc32_update(payload_crc, rgba8.data(), (uint64_t)rgba8.size());
        stream.write((const char *)rgba8.data(), (std::streamsize)rgba8.size());
      }
      ok = (bool)stream;
      if (ok && options.dump_texels) {
        ok = dump_layer(&dump, page, l, texels, valid, gathered, irradiance,
                        desktop ? &directions : nullptr);
      }
      float64_t luminance_sum = 0.0;
      for (const Vec3 &value : irradiance) {
        luminance_sum += luminance(value);
      }
      const VkrLightLayer &record = layers[l].record;
      std::printf(
          "baked page=%u/%u layer=%u/%zu %s=%s texels=%zu "
          "gpu_s=%.2f mean_luminance=%.5g outliers=%llu "
          "outlier_energy=%.4f\n",
          page + 1u, layout.page_count, l + 1u, layers.size(),
          record.kind == VKR_LIGHT_LAYER_SUN_KEY ? "sun_key" : "lamp_group",
          record.kind == VKR_LIGHT_LAYER_SUN_KEY
              ? std::to_string(record.index).c_str()
              : record.name,
          texels.size(), gathered.gpu_seconds,
          texels.empty() ? 0.0 : luminance_sum / texels.size(),
          (unsigned long long)outliers.texels, outliers.energy_fraction);
      if (desktop) {
        std::printf("direction page=%u/%u layer=%u/%zu lamp_group=%s "
                    "mean_directionality=%.4f\n",
                    page + 1u, layout.page_count, l + 1u, layers.size(),
                    record.name, mean_directionality);
      }
      if (desktop_bc) {
        std::printf("bc page=%u/%u layer=%u/%zu lamp_group=%s "
                    "bc6h_encode_s=%.2f bc6h_gpu_s=%.3f bc6h_rms=%.4g "
                    "bc6h_relative_rms=%.4f bc7_encode_s=%.2f "
                    "bc7_gpu_s=%.3f bc7_rms=%.4f bc7_max_steps=%u\n",
                    page + 1u, layout.page_count, l + 1u, layers.size(),
                    record.name, irradiance_bc.encode_seconds,
                    irradiance_bc.gpu_seconds, irradiance_bc.rms,
                    irradiance_bc.relative_rms, direction_bc.encode_seconds,
                    direction_bc.gpu_seconds, direction_bc.rms,
                    direction_bc.max_steps);
      }
      std::fflush(stdout);
    }
  }
  /* The lamp-direct pages follow every other page (ADR-108). */
  if (ok && any_lamp_direct) {
    const auto direct_start = std::chrono::steady_clock::now();
    double direct_gpu_seconds = 0.0;
    ok = bake_direct_pages(options, scene, gpu.get(), layers, lamp_direct,
                           direct_layout, threads, &stream, &payload_crc,
                           &direct_gpu_seconds);
    gpu_seconds += direct_gpu_seconds;
    std::printf("direct_s=%.2f direct_gpu_s=%.2f\n",
                seconds_since(direct_start), direct_gpu_seconds);
  }
  gpu.reset();

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
              "gpu_s=%.2f cleanup_s=%.2f encode_s=%.2f\n",
              options.output, (unsigned long long)file_size, layout.page_count,
              layers.size(), instances.size(), gpu_seconds, cleanup_seconds,
              encode_seconds);
  return 0;
}

int run(const Options &options, VkrAllocator *allocator, Arena *arena) {
  const auto load_start = std::chrono::steady_clock::now();
  VkrBakeScene scene(allocator);
  VkrBakeSceneError scene_error;
  if (!vkr_bake_scene_load(&scene, options.scene, &scene_error)) {
    std::fprintf(stderr, "Scene preparation failed (error %u): %s: %s\n",
                 (unsigned)scene_error, options.scene,
                 scene.diagnostic.c_str());
    return 1;
  }
  if (scene.triangles.size() > VKR_BAKE_BVH_MAX_TRIANGLES) {
    std::fprintf(stderr, "The scene has %zu triangles; a bake holds %u\n",
                 scene.triangles.size(), VKR_BAKE_BVH_MAX_TRIANGLES);
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
  settings.rr_start_depth = rr_start_depth(options);
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

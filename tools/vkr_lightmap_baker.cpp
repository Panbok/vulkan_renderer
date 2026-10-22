#include "vkr_tool_entry.h"

extern "C" {
#include "assets/vkr_lightmap_set.h"
#include "core/logger.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
}
#include "bake/vkr_bake_atmosphere.h"
#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_gpu.h"
#include "bake/vkr_bake_integrator.h"
#include "bake/vkr_bake_layers.h"
#include "bake/vkr_bake_lightmap.h"
#include "bake/vkr_bake_lightmap_inputs.h"
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
#include <map>
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
/* World distance within which an incremental bake relights indirect light
   around a changed instance or past a changed lamp's range
   (vkr_bake_lightmap_plan_incremental); see ADR-088 for the error it
   leaves. At least the occlusion radius, so ambient occlusion and buried
   texels always relight with the geometry that changes them. */
constexpr float32_t kIncrementalReach = 4.0f;

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
  /* The input index the bake writes for its set, and the previous set and
     its index an incremental bake builds on (vkr_bake_lightmap_inputs.h). */
  const char *index = nullptr;
  const char *previous_set = nullptr;
  const char *previous_index = nullptr;
  float32_t reach = kIncrementalReach;
  /* JSON report of how the set was produced. */
  const char *report = nullptr;
  /* Two sets whose matching instances' irradiance --compare measures. */
  const char *compare[2] = {nullptr, nullptr};
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
      "[--buried-backface <fraction, 0 off>] [--dump-texels <csv>] "
      "[--index <inputs.vkli>] [--previous-set <set.vklm> --previous-index "
      "<inputs.vkli>] [--reach <world units>] [--report <json>]\n"
      "Set comparison: --compare <a.vklm> <b.vklm>\n");
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
    if (std::strcmp(flag, "--compare") == 0) {
      if (i + 2 >= argc) {
        return false;
      }
      options->compare[0] = argv[++i];
      options->compare[1] = argv[++i];
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
    } else if (std::strcmp(flag, "--index") == 0) {
      options->index = value;
    } else if (std::strcmp(flag, "--previous-set") == 0) {
      options->previous_set = value;
    } else if (std::strcmp(flag, "--previous-index") == 0) {
      options->previous_index = value;
    } else if (std::strcmp(flag, "--reach") == 0) {
      if (!parse_nonnegative(value, &options->reach)) {
        return false;
      }
    } else if (std::strcmp(flag, "--report") == 0) {
      options->report = value;
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
  if (options->compare[0]) {
    return !options->scene && !options->output;
  }
  if (options->inspect && (!options->manifest || options->output)) {
    return false;
  }
  if (options->output && options->gpu != GpuMode::Off) {
    return false;
  }
  /* A previous set comes with its index, and both with an output. */
  if ((options->previous_set != nullptr) !=
          (options->previous_index != nullptr) ||
      (options->previous_set && !options->output)) {
    return false;
  }
  options->reach = std::max(options->reach, options->ao_radius);
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

float32_t random_unit(uint32_t seed) {
  return (float32_t)(vkr_bake_lightmap_mix_seed(seed) >> 8) *
         (1.0f / 16777216.0f);
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
      /* The GPU kernel's seed for the texel: the bake's seed mixed with the
         texel's seed key. */
      const uint32_t texel_seed =
          vkr_bake_lightmap_mix_seed(options.seed ^ texel.seed);
      uint32_t hits = 0u;
      for (uint32_t s = 0u; s < options.samples; ++s) {
        const uint32_t seed =
            vkr_bake_lightmap_mix_seed(texel_seed ^ (s * 0x85ebca6bu));
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
      /* Samples independent of the GPU's, from the same texel seed key. */
      const uint32_t texel_seed =
          vkr_bake_lightmap_mix_seed((options.seed + 0x51ed27u) ^ texel.seed);
      Vec3 sum = vec3_zero();
      float64_t sum_y = 0.0;
      float64_t sum_y2 = 0.0;
      for (uint32_t s = 0u; s < options.check_samples; ++s) {
        const uint32_t seed =
            vkr_bake_lightmap_mix_seed(texel_seed ^ (s * 0x85ebca6bu));
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

/* The manifest also names the host class's layer count and the digest of
   the bake's inputs (vkr_bake_lightmap_inputs.h), from which `vkr_bakery
   bake lightmap` tells whether a previous set is current. */
bool write_manifest(const Options &options, const VkrBakeScene &scene,
                    const VkrBakeLightmapLayout &layout, size_t baked_layers,
                    const std::string &content_digest) {
  const bool desktop = options.pipeline == PipelineClass::Desktop;
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
           << ",\"content_digest\":" << json_string(content_digest)
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
                 uint32_t width, uint32_t height,
                 std::vector<uint8_t> *out_blocks, BcReport *out_report) {
  const auto start = std::chrono::steady_clock::now();
  if (!vkr_bake_gpu_encode_bc6h(gpu, rgba.data(), width, height, out_blocks,
                                &out_report->gpu_seconds)) {
    return false;
  }
  out_report->encode_seconds = seconds_since(start);
  std::vector<float32_t> decoded;
  if (!vkr_bake_gpu_decode_bc(gpu, VkrBakeGpuBcFormat::Bc6hUfloat,
                              out_blocks->data(), width, height, &decoded)) {
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
                uint32_t width, uint32_t height,
                std::vector<uint8_t> *out_blocks, BcReport *out_report) {
  const auto start = std::chrono::steady_clock::now();
  if (!vkr_bake_gpu_encode_bc7(gpu, rgba8.data(), width, height, out_blocks,
                               &out_report->gpu_seconds)) {
    return false;
  }
  out_report->encode_seconds = seconds_since(start);
  std::vector<float32_t> decoded;
  if (!vkr_bake_gpu_decode_bc(gpu, VkrBakeGpuBcFormat::Bc7, out_blocks->data(),
                              width, height, &decoded)) {
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

/* Builds each sun key's atmosphere once, so it can be made current before
   the key's layer of every page. */
bool build_sun_key_atmospheres(
    VkrBakeScene *scene, const std::vector<Vec3> &sun_keys,
    std::vector<VkrBakeAtmosphere> *key_atmospheres) {
  const auto atmosphere_start = std::chrono::steady_clock::now();
  key_atmospheres->resize(sun_keys.size());
  for (size_t k = 0u; k < sun_keys.size(); ++k) {
    if (!vkr_bake_scene_build_sun_atmosphere(scene, sun_keys[k],
                                             &(*key_atmospheres)[k])) {
      std::fprintf(stderr, "Building the atmosphere of sun key %zu failed\n",
                   k);
      return false;
    }
  }
  std::printf("sun_keys=%zu atmosphere_s=%.2f\n", sun_keys.size(),
              seconds_since(atmosphere_start));
  return true;
}

/*
 * Marks the lamp groups whose direct light moves to the lamp-direct pages
 * and packs the lamp-direct layout. False when the packing fails.
 */
bool plan_lamp_direct(const Options &options, const VkrBakeScene &scene,
                      const VkrBakeLightmapLayout &layout, bool desktop,
                      std::vector<BakeLayer> *layers,
                      std::vector<uint8_t> *lamp_direct,
                      VkrBakeLightmapLayout *direct_layout,
                      bool *any_lamp_direct) {
  for (size_t l = 0u;
       !desktop && options.direct_texels_per_unit > 0.0f && l < layers->size();
       ++l) {
    if ((*layers)[l].record.kind == VKR_LIGHT_LAYER_LAMP_GROUP &&
        !(*layers)[l].transport.lights.empty()) {
      (*lamp_direct)[l] = 1u;
      (*layers)[l].transport.texel_direct = false;
      *any_lamp_direct = true;
    }
  }
  if (*any_lamp_direct &&
      !vkr_bake_lightmap_pack(lamp_lit_instances(scene, *layers, *lamp_direct),
                              layout.page_size, options.direct_texels_per_unit,
                              direct_layout)) {
    std::fprintf(stderr, "Lamp-direct packing failed\n");
    return false;
  }
  /* No lamp reaches a lightmapped instance: the lamp groups keep their
     direct light. */
  if (*any_lamp_direct && direct_layout->page_count == 0u) {
    for (size_t l = 0u; l < layers->size(); ++l) {
      if ((*lamp_direct)[l]) {
        (*lamp_direct)[l] = 0u;
        (*layers)[l].transport.texel_direct = true;
      }
    }
    *any_lamp_direct = false;
  }
  return true;
}

/*
 * The set's instances, sorted by entity and instance index, and with
 * lamp-direct pages each one's lamp-direct rectangle in the same order.
 */
void build_set_instances(const VkrBakeScene &scene,
                         const VkrBakeLightmapLayout &layout,
                         bool any_lamp_direct,
                         const VkrBakeLightmapLayout &direct_layout,
                         std::vector<VkrLightmapInstance> *instances,
                         std::vector<VkrLightmapDirectRect> *direct_rects) {
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
  for (const auto &entry : keyed) {
    instances->push_back(entry.first);
    /* An instance no lamp reaches has an empty lamp-direct rectangle. */
    const uint32_t direct_index =
        any_lamp_direct && entry.second < direct_layout.rect_by_instance.size()
            ? direct_layout.rect_by_instance[entry.second]
            : UINT32_MAX;
    if (any_lamp_direct && direct_index != UINT32_MAX) {
      const VkrBakeLightmapRect &rect = direct_layout.rects[direct_index];
      direct_rects->push_back(
          {rect.page, rect.x, rect.y, rect.width, rect.height});
    } else if (any_lamp_direct) {
      direct_rects->push_back({0u, 0u, 0u, 0u, 0u});
    }
  }
}

/* The planes of the set's pipeline class (PipelineClass), each page in
   this order: on the tiled class every layer's ASTC 4x4 HDR irradiance;
   on the desktop class, whose layers are the lamp groups, each one's
   irradiance (RGB9E5 or BC6H) and incident direction (RGBA8 or BC7;
   ADR-104). */
std::vector<VkrLightmapPlane>
set_planes(const std::vector<VkrLightLayer> &layer_records,
           const std::vector<uint8_t> &lamp_direct, bool desktop,
           bool desktop_bc) {
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
  return planes;
}

/*
 * The set an incremental bake builds on (ADR-088): the previous set, mapped
 * read-only for as long as the bake copies its tiles, its decoded tables
 * and the input index written beside it. `usable` is false, with `reason`,
 * when the bake must bake every instance instead.
 */
struct PreviousSet {
  FileMapping mapping = {};
  VkrLightmapSet set = {};
  VkrBakeLightmapInputs inputs;
  bool usable = false;
  std::string reason = "no previous set";

  PreviousSet() = default;
  PreviousSet(const PreviousSet &) = delete;
  PreviousSet &operator=(const PreviousSet &) = delete;

  ~PreviousSet() {
    if (mapping.data) {
      file_unmap(&mapping);
    }
  }
};

/* Where a current instance's tiles lie in the previous set; `found` false
   when that set has no rectangle of its size for it. */
struct PreviousRect {
  bool found = false;
  uint32_t page = 0u;
  uint32_t x = 0u;
  uint32_t y = 0u;
  bool direct_found = false;
  uint32_t direct_page = 0u;
  uint32_t direct_x = 0u;
  uint32_t direct_y = 0u;
};

/* What a set bake bakes: the sun keys, the host class's layers and their
   records, which lamp groups take lamp-direct pages, and those pages'
   layout. */
struct SetPlan {
  std::vector<Vec3> sun_keys;
  std::vector<BakeLayer> layers;
  std::vector<VkrLightLayer> records;
  std::vector<uint8_t> lamp_direct;
  bool any_lamp_direct = false;
  VkrBakeLightmapLayout direct_layout;
};

/* Plans the set's layers; returns the tool's exit status on failure (3 when
   the host's class has nothing to bake), zero on success. */
int plan_set(const Options &options, const VkrBakeScene &scene,
             const VkrBakeLightmapLayout &layout, SetPlan *out_plan) {
  const bool desktop = options.pipeline == PipelineClass::Desktop;
  /* The keys' directions decide which lights the lamp groups hold, so both
     classes plan with them; only the tiled class bakes the keys. */
  if (scene.atmosphere.enabled &&
      scene.atmosphere_light < scene.lights.size()) {
    out_plan->sun_keys = vkr_bake_sun_key_directions(scene);
  }
  std::vector<VkrBakeLayerPlan> plans;
  std::string plan_error;
  if (!vkr_bake_plan_layers(scene, out_plan->sun_keys, &plans, &plan_error)) {
    std::fprintf(stderr, "%s\n", plan_error.c_str());
    return 1;
  }
  for (const VkrBakeLayerPlan &plan : plans) {
    if (host_bakes_layer(scene, plan, desktop)) {
      out_plan->layers.push_back(gpu_layer(plan));
    }
  }
  if (out_plan->layers.empty()) {
    return 3;
  }
  for (const BakeLayer &layer : out_plan->layers) {
    out_plan->records.push_back(layer.record);
  }
  /* On the tiled class, a lamp group with lights takes its direct light on
     the lamp-direct pages and keeps bounce, sky and emission in its
     irradiance (ADR-108). */
  out_plan->lamp_direct.assign(out_plan->layers.size(), 0u);
  if (!plan_lamp_direct(options, scene, layout, desktop, &out_plan->layers,
                        &out_plan->lamp_direct, &out_plan->direct_layout,
                        &out_plan->any_lamp_direct)) {
    return 1;
  }
  return 0;
}

/* Every bake option that changes a set's bytes, in a fixed order, for its
   input index. */
std::string settings_text(const Options &options) {
  char text[768];
  std::snprintf(
      text, sizeof(text),
      "page_size=%u texels_per_unit=%.9g direct_texels_per_unit=%.9g "
      "samples=%u max_depth=%u seed=%u astc_effort=%.9g "
      "desktop_encoding=%u pipeline=%u dilation=%u outlier_ratio=%.9g "
      "smooth=%u ao_radius=%.9g denoise=%u denoise_iterations=%u "
      "denoise_sigma=%.9g indirect_clamp=%.9g rr_start=%u "
      "buried_backface=%.9g",
      options.page_size, options.texels_per_unit,
      options.direct_texels_per_unit, options.samples, options.max_depth,
      options.seed, options.astc_effort, (uint32_t)options.desktop_encoding,
      (uint32_t)options.pipeline, options.dilation_passes,
      options.outlier_ratio, options.smooth_passes, options.ao_radius,
      options.denoise ? 1u : 0u, options.denoise_iterations,
      options.denoise_sigma, options.indirect_clamp, options.rr_start_depth,
      options.buried_backface);
  return std::string(text);
}

/* The instances of the set's instance table by key: the document id when
   the entity has one, else its index, and the node. */
std::string set_instance_key(const VkrLightmapInstance &instance) {
  static const uint8_t zero[16] = {};
  std::string key;
  if (std::memcmp(instance.document_id, zero, sizeof(zero)) != 0) {
    key.assign("d");
    key.append(reinterpret_cast<const char *>(instance.document_id), 16u);
  } else {
    key.assign("i");
    key.append(reinterpret_cast<const char *>(&instance.entity_index), 4u);
  }
  key.append(reinterpret_cast<const char *>(&instance.instance_index), 4u);
  return key;
}

/*
 * Opens the previous set and its index, and when both describe one set of
 * this bake's settings, planes and page size, finds each current instance's
 * rectangles in it. Any doubt leaves `previous->usable` false.
 */
void open_previous_set(const Options &options, Arena *arena,
                       const VkrBakeLightmapInputs &inputs,
                       const VkrBakeLightmapLayout &layout,
                       const std::vector<VkrLightmapPlane> &planes,
                       const VkrBakeScene &scene, PreviousSet *previous,
                       std::vector<PreviousRect> *out_rects) {
  out_rects->assign(inputs.instances.size(), PreviousRect{});
  if (!options.previous_set || !options.previous_index) {
    return;
  }
  uint32_t index_crc = 0u;
  uint64_t index_bytes = 0u;
  if (!vkr_bake_lightmap_index_read(options.previous_index, &previous->inputs,
                                    &index_crc, &index_bytes)) {
    previous->reason = "the previous set's input index does not read";
    return;
  }
  if (previous->inputs.globals != inputs.globals) {
    previous->reason = "settings, sky, sun keys, directional lights or "
                       "layers changed";
    return;
  }
  const FilePath path = file_path_view(options.previous_set);
  if (file_map_readonly(&path, &previous->mapping) != FILE_ERROR_NONE ||
      previous->mapping.size != index_bytes ||
      !vkr_lightmap_set_decode(previous->mapping.data, previous->mapping.size,
                               arena, &previous->set)) {
    previous->reason = "the previous set does not map, decode or match its "
                       "index";
    return;
  }
  /* The payload checksum names the set the index was written with. */
  if (previous->set.payload_crc != index_crc) {
    previous->reason = "the previous set is not the one its index describes";
    return;
  }
  bool same_planes = previous->set.plane_count == planes.size() &&
                     previous->set.page_size == layout.page_size &&
                     previous->set.shape_charts_current;
  for (uint32_t p = 0u; same_planes && p < planes.size(); ++p) {
    const VkrLightmapPlane &a = previous->set.planes[p];
    same_planes = a.layer == planes[p].layer && a.kind == planes[p].kind &&
                  a.format == planes[p].format;
  }
  if (!same_planes ||
      previous->set.direct_texels_per_unit != options.direct_texels_per_unit) {
    previous->reason = "the previous set's planes or page size differ";
    return;
  }
  std::map<std::string, uint32_t> by_key;
  std::vector<std::string> duplicates;
  for (uint32_t i = 0u; i < previous->set.instance_count; ++i) {
    const std::string key = set_instance_key(previous->set.instances[i]);
    if (!by_key.emplace(key, i).second) {
      duplicates.push_back(key);
    }
  }
  for (const std::string &key : duplicates) {
    by_key.erase(key);
  }
  for (uint32_t i = 0u; i < inputs.instances.size(); ++i) {
    const VkrBakeLightmapInputInstance &instance = inputs.instances[i];
    if (instance.width == 0u) {
      continue;
    }
    VkrLightmapInstance probe = {};
    if (scene.instances[i].has_document_id) {
      std::memcpy(probe.document_id, scene.instances[i].document_id.data(),
                  16u);
    }
    probe.entity_index = scene.instances[i].entity_index;
    probe.instance_index = scene.instances[i].source_node_index;
    const auto found = by_key.find(set_instance_key(probe));
    if (found == by_key.end()) {
      continue;
    }
    const VkrLightmapInstance &old = previous->set.instances[found->second];
    PreviousRect &rect = (*out_rects)[i];
    rect.found = old.width == instance.width && old.height == instance.height;
    rect.page = old.page;
    rect.x = old.x;
    rect.y = old.y;
    if (previous->set.direct_page_count != 0u) {
      const VkrLightmapDirectRect &direct =
          previous->set.direct_rects[found->second];
      rect.direct_found = direct.width == instance.direct_width &&
                          direct.height == instance.direct_height &&
                          direct.width != 0u;
      rect.direct_page = direct.page;
      rect.direct_x = direct.x;
      rect.direct_y = direct.y;
    }
  }
  previous->usable = true;
  previous->reason.clear();
}

/* One plane's page as it is assembled: the encoded bytes, every tile the
   empty tile until a rectangle's tiles replace it. */
struct PageImage {
  VkrLightmapPlaneFormat format = VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR;
  uint32_t page_size = 0u;
  std::vector<uint8_t> bytes;
};

/* What every page of a set bake reads and accumulates: the payload stream
   and its checksum, the diagnostic dump, the timings and, for an
   incremental bake, the previous set and which instances each layer
   relights. */
struct SetPageBake {
  const Options *options = nullptr;
  VkrBakeScene *scene = nullptr;
  const VkrBakeLightmapLayout *layout = nullptr;
  VkrBakeGpuContext *gpu = nullptr;
  const std::vector<BakeLayer> *layers = nullptr;
  const std::vector<VkrLightmapPlane> *planes = nullptr;
  const std::vector<VkrBakeAtmosphere> *key_atmospheres = nullptr;
  const PreviousSet *previous = nullptr;
  const std::vector<PreviousRect> *previous_rects = nullptr;
  /* Null for a full bake: every instance relights in every layer. */
  const VkrBakeLightmapIncrementalPlan *plan = nullptr;
  bool desktop = false;
  bool desktop_bc = false;
  uint32_t threads = 0u;
  VkrBakeGpuGatherSettings settings;
  std::ofstream *stream = nullptr;
  std::ofstream *dump = nullptr;
  uint32_t payload_crc = VKR_CRC32_INITIAL;
  double gpu_seconds = 0.0;
  double encode_seconds = 0.0;
  double cleanup_seconds = 0.0;
  /* Texels gathered and rectangles relit or copied, over every plane. */
  uint64_t gathered_texels = 0u;
  uint64_t total_texels = 0u;
  uint64_t relit_rects = 0u;
  uint64_t copied_rects = 0u;
  /* The empty tile of each plane format, encoded once. */
  std::map<uint32_t, std::vector<uint8_t>> empty_tiles;
};

/* Whether layer `l` relights source instance `instance`. */
bool layer_relights(const SetPageBake &bake, size_t l, uint32_t instance) {
  return !bake.plan || bake.plan->layer_dirty[l][instance] != 0u;
}

bool direct_relights(const SetPageBake &bake, size_t l, uint32_t instance) {
  return !bake.plan || bake.plan->direct_dirty[l][instance] != 0u;
}

/* Encodes the RGBA floats of `strip`, `width` x `height` texels of whole
   tiles, in `format`. */
bool encode_strip(SetPageBake *bake, VkrLightmapPlaneFormat format,
                  const std::vector<float32_t> &strip, uint32_t width,
                  uint32_t height, std::vector<uint8_t> *out_encoded) {
  const Options &options = *bake->options;
  BcReport report;
  switch (format) {
  case VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR:
    return vkr_bake_lightmap_encode_astc_hdr_image(
        strip, width, height, options.astc_effort, bake->threads, out_encoded);
  case VKR_LIGHTMAP_FORMAT_BC6H:
    return encode_bc6h(bake->gpu, strip, width, height, out_encoded, &report);
  case VKR_LIGHTMAP_FORMAT_RGB9E5:
    encode_rgb9e5(strip, out_encoded);
    return true;
  case VKR_LIGHTMAP_FORMAT_RGBA8:
    encode_rgba8(strip, out_encoded);
    return true;
  case VKR_LIGHTMAP_FORMAT_BC7: {
    std::vector<uint8_t> rgba8;
    encode_rgba8(strip, &rgba8);
    return encode_bc7(bake->gpu, rgba8, width, height, out_encoded, &report);
  }
  default:
    return false;
  }
}

/* The encoded empty tile of a plane format: zero with alpha one outside an
   irradiance plane's rectangles, the neutral direction outside a direction
   plane's. */
bool empty_tile(SetPageBake *bake, VkrLightmapPlaneFormat format,
                bool direction, const std::vector<uint8_t> **out_tile) {
  const uint32_t key = (uint32_t)format | (direction ? 0x100u : 0u);
  const auto found = bake->empty_tiles.find(key);
  if (found != bake->empty_tiles.end()) {
    *out_tile = &found->second;
    return true;
  }
  std::vector<float32_t> texels(16u * 4u);
  for (uint32_t i = 0u; i < 16u; ++i) {
    texels[4u * i + 0u] = direction ? 0.5f : 0.0f;
    texels[4u * i + 1u] = direction ? 0.5f : 0.0f;
    texels[4u * i + 2u] = direction ? 0.5f : 0.0f;
    texels[4u * i + 3u] = direction ? 0.0f : 1.0f;
  }
  std::vector<uint8_t> encoded;
  if (!encode_strip(bake, format, texels, 4u, 4u, &encoded)) {
    return false;
  }
  std::vector<uint8_t> tile(vkr_bake_lightmap_tile_bytes(format));
  vkr_bake_lightmap_strip_take(encoded, format, 0u, 4u, tile.data());
  *out_tile = &bake->empty_tiles.emplace(key, std::move(tile)).first->second;
  return true;
}

/* One rectangle of a page plane: relit from composed RGBA floats, or
   copied from the previous set's page image of the same plane. */
struct PlaneRect {
  const VkrBakeLightmapRect *rect = nullptr;
  bool relit = false;
  std::vector<float32_t> rgba;
  const uint8_t *previous_page = nullptr;
  uint32_t previous_x = 0u;
  uint32_t previous_y = 0u;
};

/*
 * Assembles one plane's page from its rectangles, encoding every relit
 * rectangle's tiles in one strip, and appends it to the payload.
 */
bool write_plane_page(SetPageBake *bake, VkrLightmapPlaneFormat format,
                      bool direction, uint32_t page_size,
                      const std::vector<PlaneRect> &rects) {
  const auto encode_start = std::chrono::steady_clock::now();
  const std::vector<uint8_t> *empty = nullptr;
  if (!empty_tile(bake, format, direction, &empty)) {
    return false;
  }
  const uint32_t tile_bytes = vkr_bake_lightmap_tile_bytes(format);
  const uint32_t tiles_per_row = page_size / 4u;
  PageImage image;
  image.format = format;
  image.page_size = page_size;
  image.bytes.resize((size_t)tiles_per_row * tiles_per_row * tile_bytes);
  for (size_t t = 0u; t < (size_t)tiles_per_row * tiles_per_row; ++t) {
    std::memcpy(image.bytes.data() + t * tile_bytes, empty->data(), tile_bytes);
  }
  uint64_t relit_tiles = 0u;
  for (const PlaneRect &plane_rect : rects) {
    if (plane_rect.relit) {
      relit_tiles += (uint64_t)(plane_rect.rect->width / 4u) *
                     (plane_rect.rect->height / 4u);
    }
  }
  if (relit_tiles != 0u) {
    uint32_t strip_width = 0u;
    uint32_t strip_height = 0u;
    vkr_bake_lightmap_strip_size(relit_tiles, &strip_width, &strip_height);
    std::vector<float32_t> strip((size_t)strip_width * strip_height * 4u, 0.0f);
    uint64_t tile = 0u;
    for (const PlaneRect &plane_rect : rects) {
      if (!plane_rect.relit) {
        continue;
      }
      const uint32_t width = plane_rect.rect->width;
      for (uint32_t ty = 0u; ty < plane_rect.rect->height / 4u; ++ty) {
        for (uint32_t tx = 0u; tx < width / 4u; ++tx) {
          vkr_bake_lightmap_strip_put(
              plane_rect.rgba.data() + 4u * ((size_t)ty * 4u * width + tx * 4u),
              width, tile++, strip_width, &strip);
        }
      }
    }
    std::vector<uint8_t> encoded;
    if (!encode_strip(bake, format, strip, strip_width, strip_height,
                      &encoded)) {
      return false;
    }
    tile = 0u;
    std::vector<uint8_t> bytes(tile_bytes);
    for (const PlaneRect &plane_rect : rects) {
      if (!plane_rect.relit) {
        continue;
      }
      const VkrBakeLightmapRect &rect = *plane_rect.rect;
      for (uint32_t ty = 0u; ty < rect.height / 4u; ++ty) {
        for (uint32_t tx = 0u; tx < rect.width / 4u; ++tx) {
          vkr_bake_lightmap_strip_take(encoded, format, tile++, strip_width,
                                       bytes.data());
          vkr_bake_lightmap_page_put_tile(&image.bytes, page_size, format,
                                          rect.x / 4u + tx, rect.y / 4u + ty,
                                          bytes.data());
        }
      }
    }
  }
  std::vector<uint8_t> bytes(tile_bytes);
  for (const PlaneRect &plane_rect : rects) {
    if (plane_rect.relit) {
      bake->relit_rects += 1u;
      continue;
    }
    const VkrBakeLightmapRect &rect = *plane_rect.rect;
    for (uint32_t ty = 0u; ty < rect.height / 4u; ++ty) {
      for (uint32_t tx = 0u; tx < rect.width / 4u; ++tx) {
        vkr_bake_lightmap_page_get_tile(plane_rect.previous_page, page_size,
                                        format, plane_rect.previous_x / 4u + tx,
                                        plane_rect.previous_y / 4u + ty,
                                        bytes.data());
        vkr_bake_lightmap_page_put_tile(&image.bytes, page_size, format,
                                        rect.x / 4u + tx, rect.y / 4u + ty,
                                        bytes.data());
      }
    }
    bake->copied_rects += 1u;
  }
  bake->encode_seconds += seconds_since(encode_start);
  bake->payload_crc = vkr_crc32_update(bake->payload_crc, image.bytes.data(),
                                       (uint64_t)image.bytes.size());
  bake->stream->write((const char *)image.bytes.data(),
                      (std::streamsize)image.bytes.size());
  return (bool)*bake->stream;
}

/* The texels of `texels` whose source instance `keep` selects, with their
   indices in `texels`. */
template <typename Keep>
void select_texels(const std::vector<VkrBakeLightmapTexel> &texels, Keep keep,
                   std::vector<VkrBakeLightmapTexel> *out_texels,
                   std::vector<uint32_t> *out_indices) {
  out_texels->clear();
  out_indices->clear();
  for (uint32_t i = 0u; i < texels.size(); ++i) {
    if (keep(texels[i].source_instance_index)) {
      out_texels->push_back(texels[i]);
      out_indices->push_back(i);
    }
  }
}

/* Texels one gather or cleanup works on, with their same-surface neighbors,
   validity, fill and ambient occlusion (empty without). Neighbors and fill
   stay within an instance, so the texels of whole instances plan what their
   page would plan for them. */
struct TexelSet {
  std::vector<VkrBakeLightmapTexel> texels;
  /* Index of each texel in the set it was selected from. */
  std::vector<uint32_t> source_index;
  VkrBakeLightmapNeighbors neighbors;
  std::vector<uint8_t> valid;
  VkrBakeLightmapFill fill;
  std::vector<float32_t> occlusion;
};

/* The texels of `from` whose instance `relit` marks, with their validity
   and occlusion from `from` and their own neighbors and fill; all of
   `from` when every texel's instance is marked. */
bool subset_texels(const TexelSet &from, const std::vector<uint8_t> &relit,
                   uint32_t page_size, float32_t neighbor_distance,
                   TexelSet *out) {
  select_texels(
      from.texels, [&](uint32_t instance) { return relit[instance] != 0u; },
      &out->texels, &out->source_index);
  if (out->texels.size() == from.texels.size()) {
    out->neighbors = from.neighbors;
    out->valid = from.valid;
    out->fill = from.fill;
    out->occlusion = from.occlusion;
    return true;
  }
  out->valid.resize(out->texels.size());
  out->occlusion.resize(from.occlusion.empty() ? 0u : out->texels.size());
  for (size_t i = 0u; i < out->texels.size(); ++i) {
    out->valid[i] = from.valid[out->source_index[i]];
    if (!from.occlusion.empty()) {
      out->occlusion[i] = from.occlusion[out->source_index[i]];
    }
  }
  return vkr_bake_lightmap_neighbors(page_size, out->texels, neighbor_distance,
                                     &out->neighbors) &&
         vkr_bake_lightmap_plan_fill(out->neighbors, out->valid, &out->fill);
}

/*
 * The page's buried texels and their fill, and with `bake_occlusion` its
 * smoothed ambient occlusion, from its first layer's first-bounce rays.
 */
bool plan_page_fill(const Options &options, uint32_t page, uint32_t page_count,
                    const std::vector<VkrBakeLightmapTexel> &texels,
                    const VkrBakeLightmapNeighbors &neighbors,
                    bool bake_occlusion, bool find_buried,
                    VkrBakeGpuGatherResult *gathered,
                    std::vector<uint8_t> *valid, VkrBakeLightmapFill *fill,
                    std::vector<float32_t> *occlusion) {
  uint64_t buried = 0u;
  for (size_t i = 0u; find_buried && i < texels.size(); ++i) {
    if (gathered->backface[i] > options.buried_backface) {
      (*valid)[i] = 0u;
      ++buried;
    }
  }
  bool ok = vkr_bake_lightmap_plan_fill(neighbors, *valid, fill);
  if (ok && find_buried) {
    std::printf("buried page=%u/%u texels=%llu filled=%zu "
                "unfilled=%llu\n",
                page + 1u, page_count, (unsigned long long)buried,
                fill->texels.size(), (unsigned long long)fill->unfilled);
  }
  if (ok && bake_occlusion) {
    *occlusion = std::move(gathered->occlusion);
    vkr_bake_lightmap_apply_fill(*fill, occlusion);
    std::vector<Vec3> smoothed(occlusion->size());
    for (size_t i = 0u; i < occlusion->size(); ++i) {
      smoothed[i] = vec3_new((*occlusion)[i], (*occlusion)[i], (*occlusion)[i]);
    }
    ok = vkr_bake_lightmap_smooth(neighbors, texels, options.smooth_passes,
                                  &smoothed);
    for (size_t i = 0u; ok && i < occlusion->size(); ++i) {
      (*occlusion)[i] = smoothed[i].x;
    }
  }
  return ok;
}

/*
 * The page surface: the texels of every instance some layer relights, and
 * from the first layer's first-bounce rays their buried texels, fill and
 * smoothed ambient occlusion. The first layer's own gather supplies them
 * for the instances it relights, and a direct-only gather of the first
 * layer's transport, whose first hits match it, for the rest
 * (VkrBakeGpuGatherSettings::direct_only). `first_layer` takes the first
 * layer's texels and its gather.
 */
bool plan_page_surface(SetPageBake *bake, uint32_t page,
                       const std::vector<VkrBakeLightmapTexel> &all,
                       const std::vector<uint8_t> &relit_any,
                       TexelSet *out_surface, TexelSet *out_first,
                       VkrBakeGpuGatherResult *out_first_gathered) {
  const Options &options = *bake->options;
  const std::vector<BakeLayer> &layers = *bake->layers;
  const float32_t neighbor_distance =
      kOutlierNeighborTexels / options.texels_per_unit;
  const bool bake_occlusion = !bake->desktop && options.ao_radius > 0.0f;
  const bool find_buried = options.buried_backface > 0.0f;
  std::vector<uint32_t> unused;
  select_texels(
      all, [&](uint32_t instance) { return relit_any[instance] != 0u; },
      &out_surface->texels, &unused);
  out_surface->valid.assign(out_surface->texels.size(), 1u);
  if (!vkr_bake_lightmap_neighbors(bake->layout->page_size, out_surface->texels,
                                   neighbor_distance,
                                   &out_surface->neighbors)) {
    return false;
  }
  std::vector<uint8_t> first_relit(relit_any.size(), 0u);
  for (uint32_t i = 0u; i < relit_any.size(); ++i) {
    first_relit[i] = relit_any[i] && layer_relights(*bake, 0u, i) ? 1u : 0u;
  }
  select_texels(
      out_surface->texels,
      [&](uint32_t instance) { return first_relit[instance] != 0u; },
      &out_first->texels, &out_first->source_index);
  std::vector<float32_t> backface(out_surface->texels.size(), 0.0f);
  std::vector<float32_t> occlusion(out_surface->texels.size(), 1.0f);
  /* Only first hits need no atmosphere, which an incremental bake that
     relights no sun key never builds. */
  if (layers[0].sun_key >= 0 && !bake->key_atmospheres->empty()) {
    vkr_bake_scene_use_atmosphere(
        bake->scene, (*bake->key_atmospheres)[(size_t)layers[0].sun_key]);
    if (!vkr_bake_gpu_update_lighting(bake->gpu, *bake->scene)) {
      return false;
    }
  }
  VkrBakeGpuGatherSettings settings = bake->settings;
  settings.occlusion = bake_occlusion;
  settings.backface = find_buried;
  settings.direction = bake->desktop;
  if (!vkr_bake_gpu_gather(bake->gpu, out_first->texels, layers[0].transport,
                           settings, out_first_gathered)) {
    return false;
  }
  bake->gpu_seconds += out_first_gathered->gpu_seconds;
  for (size_t i = 0u; i < out_first->texels.size(); ++i) {
    const uint32_t s = out_first->source_index[i];
    if (find_buried) {
      backface[s] = out_first_gathered->backface[i];
    }
    if (bake_occlusion) {
      occlusion[s] = out_first_gathered->occlusion[i];
    }
  }
  if (out_first->texels.size() != out_surface->texels.size() &&
      (bake_occlusion || find_buried)) {
    std::vector<VkrBakeLightmapTexel> rest;
    std::vector<uint32_t> rest_index;
    select_texels(
        out_surface->texels,
        [&](uint32_t instance) { return first_relit[instance] == 0u; }, &rest,
        &rest_index);
    VkrBakeGpuGatherSettings first_hits = settings;
    first_hits.direct_only = true;
    first_hits.direction = false;
    VkrBakeGpuGatherResult hits;
    if (!vkr_bake_gpu_gather(bake->gpu, rest, layers[0].transport, first_hits,
                             &hits)) {
      return false;
    }
    bake->gpu_seconds += hits.gpu_seconds;
    for (size_t i = 0u; i < rest.size(); ++i) {
      if (find_buried) {
        backface[rest_index[i]] = hits.backface[i];
      }
      if (bake_occlusion) {
        occlusion[rest_index[i]] = hits.occlusion[i];
      }
    }
  }
  VkrBakeGpuGatherResult surface_rays;
  surface_rays.backface = std::move(backface);
  surface_rays.occlusion = std::move(occlusion);
  if (!plan_page_fill(
          options, page, bake->layout->page_count, out_surface->texels,
          out_surface->neighbors, bake_occlusion, find_buried, &surface_rays,
          &out_surface->valid, &out_surface->fill, &out_surface->occlusion)) {
    return false;
  }
  return subset_texels(*out_surface, first_relit, bake->layout->page_size,
                       neighbor_distance, out_first);
}

/* The rectangles of `page` in layout order with, for a rectangle the layer
   relights, its texels' indices in `texels`. */
void page_rects(const VkrBakeLightmapLayout &layout, uint32_t page,
                const std::vector<VkrBakeLightmapTexel> &texels,
                std::vector<uint32_t> *out_rects,
                std::vector<std::vector<uint32_t>> *out_members) {
  out_rects->clear();
  for (uint32_t r = 0u; r < layout.rects.size(); ++r) {
    if (layout.rects[r].page == page) {
      out_rects->push_back(r);
    }
  }
  out_members->assign(layout.rects.size(), {});
  for (uint32_t i = 0u; i < texels.size(); ++i) {
    const uint32_t instance = texels[i].source_instance_index;
    (*out_members)[layout.rect_by_instance[instance]].push_back(i);
  }
}

/* The page image a copied rectangle of plane `plane` reads in the previous
   set, on its page `previous_page` (a lamp-direct page for a lamp-direct
   plane). */
const uint8_t *previous_page_image(const SetPageBake &bake, uint32_t plane,
                                   uint32_t previous_page) {
  const VkrLightmapSet &set = bake.previous->set;
  return set.payload +
         vkr_lightmap_set_plane_offset(&set, previous_page, plane);
}

/* The plane rectangles of one layer's plane on `page`: relit rectangles
   composed from `values` (or directions), the others copied. */
bool plane_rects(const SetPageBake &bake, const VkrBakeLightmapLayout &layout,
                 uint32_t page, uint32_t plane, bool lamp_direct_page,
                 const TexelSet &texels, const std::vector<Vec3> &values,
                 const std::vector<float32_t> *occlusion, bool direction,
                 std::vector<PlaneRect> *out_rects) {
  std::vector<uint32_t> rects;
  std::vector<std::vector<uint32_t>> members;
  page_rects(layout, page, texels.texels, &rects, &members);
  std::vector<uint8_t> relit(layout.rects.size(), 0u);
  for (const VkrBakeLightmapTexel &texel : texels.texels) {
    relit[layout.rect_by_instance[texel.source_instance_index]] = 1u;
  }
  out_rects->clear();
  out_rects->resize(rects.size());
  for (size_t k = 0u; k < rects.size(); ++k) {
    const uint32_t r = rects[k];
    const VkrBakeLightmapRect &rect = layout.rects[r];
    PlaneRect &plane_rect = (*out_rects)[k];
    plane_rect.rect = &rect;
    const PreviousRect *previous =
        bake.previous_rects && bake.plan
            ? &(*bake.previous_rects)[rect.source_instance_index]
            : nullptr;
    const bool found = previous && (lamp_direct_page ? previous->direct_found
                                                     : previous->found);
    /* A rectangle no texel covers composes the same in every bake; one
       whose instance the plan relights but whose texels this set lacks is
       relit too. */
    plane_rect.relit = relit[r] != 0u || !found;
    if (!plane_rect.relit) {
      plane_rect.previous_page = previous_page_image(
          bake, plane,
          lamp_direct_page ? previous->direct_page : previous->page);
      plane_rect.previous_x =
          lamp_direct_page ? previous->direct_x : previous->x;
      plane_rect.previous_y =
          lamp_direct_page ? previous->direct_y : previous->y;
      continue;
    }
    if (!vkr_bake_lightmap_compose_rect(
            rect, texels.texels, members[r], values, occlusion,
            bake.options->dilation_passes, direction, &plane_rect.rgba)) {
      return false;
    }
  }
  return true;
}

/* Index of the plane of `layer` and `kind` in the set's plane table. */
uint32_t plane_index(const std::vector<VkrLightmapPlane> &planes,
                     uint32_t layer, VkrLightmapPlaneKind kind) {
  for (uint32_t p = 0u; p < planes.size(); ++p) {
    if (planes[p].layer == layer && planes[p].kind == kind) {
      return p;
    }
  }
  return UINT32_MAX;
}

/* Prints a baked layer's statistics, with its direction line on the
   desktop class. */
void print_layer_report(const SetPageBake &bake, uint32_t page, uint32_t l,
                        const TexelSet &texels,
                        const VkrBakeGpuGatherResult &gathered,
                        const std::vector<Vec3> &irradiance,
                        const VkrBakeLightmapOutliers &outliers,
                        float64_t mean_directionality) {
  const VkrBakeLightmapLayout &layout = *bake.layout;
  const std::vector<BakeLayer> &layers = *bake.layers;
  float64_t luminance_sum = 0.0;
  for (const Vec3 &value : irradiance) {
    luminance_sum += luminance(value);
  }
  const VkrLightLayer &record = layers[l].record;
  std::printf("baked page=%u/%u layer=%u/%zu %s=%s texels=%zu "
              "gpu_s=%.2f mean_luminance=%.5g outliers=%llu "
              "outlier_energy=%.4f\n",
              page + 1u, layout.page_count, l + 1u, layers.size(),
              record.kind == VKR_LIGHT_LAYER_SUN_KEY ? "sun_key" : "lamp_group",
              record.kind == VKR_LIGHT_LAYER_SUN_KEY
                  ? std::to_string(record.index).c_str()
                  : record.name,
              texels.texels.size(), gathered.gpu_seconds,
              texels.texels.empty() ? 0.0
                                    : luminance_sum / texels.texels.size(),
              (unsigned long long)outliers.texels, outliers.energy_fraction);
  if (bake.desktop) {
    std::printf("direction page=%u/%u layer=%u/%zu lamp_group=%s "
                "mean_directionality=%.4f\n",
                page + 1u, layout.page_count, l + 1u, layers.size(),
                record.name, mean_directionality);
  }
}

/*
 * Bakes one layer of a page: gathers the texels it relights (the first
 * layer's come gathered), cleans them up, and writes its planes with every
 * other rectangle copied from the previous set.
 */
bool bake_page_layer(SetPageBake *bake, uint32_t page, uint32_t l,
                     const TexelSet &surface, const TexelSet &first,
                     VkrBakeGpuGatherResult *first_gathered) {
  const Options &options = *bake->options;
  const VkrBakeLightmapLayout &layout = *bake->layout;
  const std::vector<BakeLayer> &layers = *bake->layers;
  const float32_t neighbor_distance =
      kOutlierNeighborTexels / options.texels_per_unit;
  TexelSet own;
  const TexelSet *texels = &first;
  VkrBakeGpuGatherResult gathered;
  VkrBakeGpuGatherResult *result = first_gathered;
  if (l != 0u) {
    std::vector<uint8_t> relit(bake->scene->instances.size(), 0u);
    for (uint32_t i = 0u; i < relit.size(); ++i) {
      relit[i] = layer_relights(*bake, l, i) ? 1u : 0u;
    }
    if (!subset_texels(surface, relit, layout.page_size, neighbor_distance,
                       &own)) {
      return false;
    }
    texels = &own;
    result = &gathered;
    if (!own.texels.empty()) {
      if (layers[l].sun_key >= 0) {
        vkr_bake_scene_use_atmosphere(
            bake->scene, (*bake->key_atmospheres)[(size_t)layers[l].sun_key]);
        if (!vkr_bake_gpu_update_lighting(bake->gpu, *bake->scene)) {
          return false;
        }
      }
      VkrBakeGpuGatherSettings settings = bake->settings;
      settings.direction = bake->desktop;
      if (!vkr_bake_gpu_gather(bake->gpu, own.texels, layers[l].transport,
                               settings, &gathered)) {
        return false;
      }
      bake->gpu_seconds += gathered.gpu_seconds;
    }
  }
  bake->gathered_texels += texels->texels.size();
  const auto cleanup_start = std::chrono::steady_clock::now();
  std::vector<Vec3> irradiance;
  VkrBakeLightmapOutliers outliers = {};
  if (!texels->texels.empty() &&
      !finish_layer(options, layout.page_size, texels->texels,
                    texels->neighbors, texels->valid, texels->fill, result,
                    &irradiance, &outliers)) {
    return false;
  }
  std::vector<Vec3> directions;
  float64_t mean_directionality = 0.0;
  if (bake->desktop && !texels->texels.empty() &&
      !finish_directions(options, texels->texels, texels->neighbors,
                         texels->fill, *result, &directions,
                         &mean_directionality)) {
    return false;
  }
  bake->cleanup_seconds += seconds_since(cleanup_start);
  const bool bake_occlusion = !bake->desktop && options.ao_radius > 0.0f;
  const uint32_t irradiance_plane =
      plane_index(*bake->planes, l, VKR_LIGHTMAP_PLANE_IRRADIANCE);
  std::vector<PlaneRect> rects;
  if (!plane_rects(*bake, layout, page, irradiance_plane, false, *texels,
                   irradiance, bake_occlusion ? &texels->occlusion : nullptr,
                   false, &rects) ||
      !write_plane_page(bake, (*bake->planes)[irradiance_plane].format, false,
                        layout.page_size, rects)) {
    return false;
  }
  if (bake->desktop) {
    const uint32_t direction_plane =
        plane_index(*bake->planes, l, VKR_LIGHTMAP_PLANE_DIRECTION);
    if (!plane_rects(*bake, layout, page, direction_plane, false, *texels,
                     directions, nullptr, true, &rects) ||
        !write_plane_page(bake, (*bake->planes)[direction_plane].format, true,
                          layout.page_size, rects)) {
      return false;
    }
  }
  if (options.dump_texels && !texels->texels.empty() &&
      !dump_layer(bake->dump, page, l, texels->texels, texels->valid, *result,
                  irradiance, bake->desktop ? &directions : nullptr)) {
    return false;
  }
  print_layer_report(*bake, page, l, *texels, *result, irradiance, outliers,
                     mean_directionality);
  std::fflush(stdout);
  return true;
}

/*
 * Bakes one page: rasterizes its texels, then gathers, finishes, encodes and
 * appends each layer's planes in set order. An incremental bake gathers
 * only the instances each layer relights and copies the others' tiles.
 */
bool bake_page(SetPageBake *bake, uint32_t page) {
  VkrBakeScene &scene = *bake->scene;
  const VkrBakeLightmapLayout &layout = *bake->layout;
  const std::vector<BakeLayer> &layers = *bake->layers;
  std::vector<VkrBakeLightmapTexel> all;
  if (!vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                        (uint32_t)scene.triangles.size(),
                                        layout, page, &all)) {
    return false;
  }
  bake->total_texels += (uint64_t)all.size() * layers.size();
  std::vector<uint8_t> relit_any(scene.instances.size(), 0u);
  for (uint32_t i = 0u; i < relit_any.size(); ++i) {
    for (size_t l = 0u; l < layers.size() && !relit_any[i]; ++l) {
      relit_any[i] = layer_relights(*bake, l, i) ? 1u : 0u;
    }
  }
  TexelSet surface;
  TexelSet first;
  VkrBakeGpuGatherResult first_gathered;
  bool any = false;
  for (const VkrBakeLightmapTexel &texel : all) {
    any = any || relit_any[texel.source_instance_index] != 0u;
  }
  if (any && !plan_page_surface(bake, page, all, relit_any, &surface, &first,
                                &first_gathered)) {
    return false;
  }
  for (uint32_t l = 0u; l < layers.size(); ++l) {
    if (!bake_page_layer(bake, page, l, surface, first, &first_gathered)) {
      return false;
    }
  }
  return true;
}

/*
 * Bakes and streams the lamp-direct pages (ADR-108): per page, each lamp
 * group's direct light at the texels of the lamp-direct layout, gathered
 * without indirect light. Buried texels, found by the first group's
 * first-hit back faces, take their neighbors' light; nothing is smoothed,
 * so shadow edges keep the footprint grid's and the emitter's own softness.
 * An incremental bake gathers, for every lamp-direct group, the instances
 * any of them relights, and copies the others' tiles.
 */
bool bake_direct_pages(SetPageBake *bake,
                       const std::vector<uint8_t> &lamp_direct,
                       const VkrBakeLightmapLayout &direct_layout) {
  const Options &options = *bake->options;
  VkrBakeScene &scene = *bake->scene;
  const std::vector<BakeLayer> &layers = *bake->layers;
  VkrBakeGpuGatherSettings settings;
  /* Rectangle lights take one footprint point per sample. */
  settings.samples = kDirectSamples;
  settings.max_depth = 1u;
  settings.rr_start_depth = 0u;
  settings.seed = options.seed;
  settings.direct_only = true;
  std::vector<uint8_t> relit(scene.instances.size(), 0u);
  for (uint32_t i = 0u; i < relit.size(); ++i) {
    for (size_t l = 0u; l < layers.size() && !relit[i]; ++l) {
      relit[i] = lamp_direct[l] && direct_relights(*bake, l, i) ? 1u : 0u;
    }
  }
  for (uint32_t page = 0u; page < direct_layout.page_count; ++page) {
    std::vector<VkrBakeLightmapTexel> all;
    if (!vkr_bake_lightmap_rasterize_page(scene.triangles.data(),
                                          (uint32_t)scene.triangles.size(),
                                          direct_layout, page, &all)) {
      return false;
    }
    TexelSet texels;
    select_texels(
        all, [&](uint32_t instance) { return relit[instance] != 0u; },
        &texels.texels, &texels.source_index);
    if (!vkr_bake_lightmap_neighbors(direct_layout.page_size, texels.texels,
                                     kOutlierNeighborTexels /
                                         options.direct_texels_per_unit,
                                     &texels.neighbors)) {
      return false;
    }
    texels.valid.assign(texels.texels.size(), 1u);
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
      if (!vkr_bake_gpu_gather(bake->gpu, texels.texels, transport, settings,
                               &gathered)) {
        return false;
      }
      bake->gpu_seconds += gathered.gpu_seconds;
      bake->gathered_texels += texels.texels.size();
      bake->total_texels += all.size();
      if (!planned) {
        for (size_t i = 0u; settings.backface && i < texels.texels.size();
             ++i) {
          if (gathered.backface[i] > options.buried_backface) {
            texels.valid[i] = 0u;
          }
        }
        if (!vkr_bake_lightmap_plan_fill(texels.neighbors, texels.valid,
                                         &texels.fill)) {
          return false;
        }
        planned = true;
      }
      vkr_bake_lightmap_apply_fill(texels.fill, &gathered.direct);
      const uint32_t plane = plane_index(*bake->planes, (uint32_t)l,
                                         VKR_LIGHTMAP_PLANE_LAMP_DIRECT);
      std::vector<PlaneRect> rects;
      if (!plane_rects(*bake, direct_layout, page, plane, true, texels,
                       gathered.direct, nullptr, false, &rects) ||
          !write_plane_page(bake, (*bake->planes)[plane].format, false,
                            direct_layout.page_size, rects)) {
        return false;
      }
      float64_t luminance_sum = 0.0;
      for (const Vec3 &value : gathered.direct) {
        luminance_sum += luminance(value);
      }
      std::printf("direct page=%u/%u lamp_group=%s texels=%zu gpu_s=%.2f "
                  "mean_luminance=%.5g\n",
                  page + 1u, direct_layout.page_count, layers[l].record.name,
                  texels.texels.size(), gathered.gpu_seconds,
                  texels.texels.empty() ? 0.0
                                        : luminance_sum / texels.texels.size());
      std::fflush(stdout);
    }
  }
  return true;
}

/*
 * Plans which instances each layer relights over the previous set. A sun
 * key's shadows run away from its key light, whose direction comes from the
 * key's atmosphere when `key_atmospheres` holds them, else from the key's
 * sun direction.
 */
bool plan_relight(const Options &options, const PreviousSet &previous,
                  const VkrBakeLightmapInputs &inputs, const SetPlan &plan,
                  const std::vector<PreviousRect> &previous_rects,
                  const std::vector<VkrBakeAtmosphere> *key_atmospheres,
                  VkrBakeLightmapIncrementalPlan *out_plan) {
  std::vector<uint8_t> has_rect(inputs.instances.size(), 0u);
  std::vector<uint8_t> has_direct_rect(inputs.instances.size(), 0u);
  for (size_t i = 0u; i < previous_rects.size(); ++i) {
    has_rect[i] = previous_rects[i].found ? 1u : 0u;
    has_direct_rect[i] = previous_rects[i].direct_found ? 1u : 0u;
  }
  std::vector<VkrBakeLightmapPlanLayer> layers(plan.layers.size());
  for (size_t l = 0u; l < plan.layers.size(); ++l) {
    const BakeLayer &layer = plan.layers[l];
    layers[l].kind = layer.record.kind;
    layers[l].lights = layer.transport.lights;
    layers[l].texel_direct = layer.transport.texel_direct;
    layers[l].lamp_direct = plan.lamp_direct[l] != 0u;
    layers[l].key_light_direction =
        key_atmospheres && layer.sun_key >= 0
            ? (*key_atmospheres)[(size_t)layer.sun_key].key_light_direction
            : layer.record.sun_direction;
  }
  if (!vkr_bake_lightmap_plan_incremental(previous.inputs, inputs, layers,
                                          has_rect, has_direct_rect,
                                          options.reach, out_plan)) {
    std::fprintf(stderr, "Planning the incremental bake failed\n");
    return false;
  }
  return true;
}

/* Writes the bake's incremental report: how the set was produced, from
   which previous set, and how much it gathered and copied. */
bool write_report(const Options &options, const SetPageBake &bake,
                  const PreviousSet &previous,
                  const VkrBakeLightmapIncrementalPlan *plan,
                  const std::vector<BakeLayer> &layers, double seconds) {
  if (!options.report) {
    return true;
  }
  std::ostringstream report;
  report << "{\"version\":1,\"kind\":\"" << (plan ? "incremental" : "full")
         << "\",\"full_reason\":"
         << json_string(plan ? std::string() : previous.reason)
         << ",\"reach\":" << options.reach
         << ",\"changed_instances\":" << (plan ? plan->changed_instances : 0u)
         << ",\"changed_lights\":" << (plan ? plan->changed_lights : 0u)
         << ",\"gathered_texels\":" << bake.gathered_texels
         << ",\"total_texels\":" << bake.total_texels
         << ",\"relit_rects\":" << bake.relit_rects
         << ",\"copied_rects\":" << bake.copied_rects << ",\"layers\":[";
  for (size_t l = 0u; plan && l < layers.size(); ++l) {
    uint32_t relit = 0u;
    uint32_t direct = 0u;
    for (size_t i = 0u; i < plan->layer_dirty[l].size(); ++i) {
      relit += plan->layer_dirty[l][i] ? 1u : 0u;
      direct += plan->direct_dirty[l][i] ? 1u : 0u;
    }
    report << (l ? "," : "") << "{\"relit_instances\":" << relit
           << ",\"relit_direct_instances\":" << direct << "}";
  }
  report << "],\"seconds\":" << seconds << "}\n";
  return write_atomic(options.report, report.str());
}

/*
 * Bakes the scene's lightmap set into `options.output`. Pages are baked one
 * at a time and each layer image is encoded and appended as soon as it is
 * gathered; the header and tables, which carry the payload checksum, are
 * written last. The file appears under its name only when complete. With a
 * previous set and its input index whose globals match, only what the
 * incremental plan relights is gathered; every other rectangle's tiles are
 * copied (vkr_bake_lightmap_inputs.h). `options.index` receives this set's
 * input index.
 */
int bake_set(const Options &options, VkrBakeScene &scene, Arena *arena,
             const VkrBakeLightmapLayout &layout, SetPlan &plan,
             const VkrBakeLightmapInputs &inputs) {
  const auto bake_start = std::chrono::steady_clock::now();
  if (layout.rects.empty()) {
    std::fprintf(stderr, "The scene has no lightmapped instances\n");
    return 1;
  }
  if (!vkr_bake_gpu_available()) {
    std::fprintf(stderr, "Lightmap bakes need GPU ray tracing, which this "
                         "host does not have\n");
    return 1;
  }
  const bool desktop = options.pipeline == PipelineClass::Desktop;
  std::vector<BakeLayer> &layers = plan.layers;
  std::vector<VkrLightmapInstance> instances;
  std::vector<VkrLightmapDirectRect> direct_rects;
  build_set_instances(scene, layout, plan.any_lamp_direct, plan.direct_layout,
                      &instances, &direct_rects);
  if (plan.any_lamp_direct) {
    uint64_t direct_texels = 0u;
    for (const VkrBakeLightmapRect &rect : plan.direct_layout.rects) {
      direct_texels += (uint64_t)rect.width * rect.height;
    }
    std::printf("direct_pages=%u direct_texels_per_unit=%g "
                "direct_rect_texels=%llu\n",
                plan.direct_layout.page_count, options.direct_texels_per_unit,
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
  std::vector<VkrLightmapPlane> planes =
      set_planes(plan.records, plan.lamp_direct, desktop, desktop_bc);

  /* The previous set, and which instances each layer relights. */
  PreviousSet previous;
  std::vector<PreviousRect> previous_rects;
  open_previous_set(options, arena, inputs, layout, planes, scene, &previous,
                    &previous_rects);
  VkrBakeLightmapIncrementalPlan incremental;
  const VkrBakeLightmapIncrementalPlan *relight = nullptr;
  if (previous.usable) {
    if (!plan_relight(options, previous, inputs, plan, previous_rects, nullptr,
                      &incremental)) {
      return 1;
    }
    relight = &incremental;
  }
  /* The sun keys relight only when an instance changed; then they need
     their atmospheres, and the plan their key lights' directions: the sun,
     or the moon while the sun is down. */
  const bool sun_keys_relight = !relight ||
                                incremental.changed_instances != 0u ||
                                incremental.changed_boxes != 0u;
  std::vector<VkrBakeAtmosphere> key_atmospheres;
  if (!desktop && !plan.sun_keys.empty() && sun_keys_relight) {
    if (!build_sun_key_atmospheres(&scene, plan.sun_keys, &key_atmospheres)) {
      return 1;
    }
  }
  if (relight && !key_atmospheres.empty() &&
      !plan_relight(options, previous, inputs, plan, previous_rects,
                    &key_atmospheres, &incremental)) {
    return 1;
  }
  if (relight) {
    std::printf("incremental base=%s changed_instances=%u changed_lights=%u "
                "reach=%g\n",
                options.previous_set, incremental.changed_instances,
                incremental.changed_lights, options.reach);
  } else {
    std::printf("full reason=\"%s\"\n", previous.reason.c_str());
  }

  VkrLightmapSet set = {};
  set.page_size = layout.page_size;
  set.page_count = layout.page_count;
  set.layer_count = (uint32_t)plan.records.size();
  set.plane_count = (uint32_t)planes.size();
  set.instance_count = (uint32_t)instances.size();
  set.texels_per_unit = options.texels_per_unit;
  set.layers = plan.records.data();
  set.planes = planes.data();
  set.instances = instances.data();
  set.direct_page_count =
      plan.any_lamp_direct ? plan.direct_layout.page_count : 0u;
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

  SetPageBake pages;
  pages.options = &options;
  pages.scene = &scene;
  pages.layout = &layout;
  pages.gpu = gpu.get();
  pages.layers = &layers;
  pages.planes = &planes;
  pages.key_atmospheres = &key_atmospheres;
  pages.previous = &previous;
  pages.previous_rects = &previous_rects;
  pages.plan = relight;
  pages.desktop = desktop;
  pages.desktop_bc = desktop_bc;
  pages.settings.samples = options.samples;
  pages.settings.max_depth = options.max_depth;
  pages.settings.rr_start_depth = rr_start_depth(options);
  pages.settings.seed = options.seed;
  pages.settings.occlusion_radius = options.ao_radius;
  pages.settings.indirect_clamp = options.indirect_clamp;
  pages.threads = options.threads
                      ? options.threads
                      : std::max(1u, std::thread::hardware_concurrency());
  std::ofstream dump;
  if (options.dump_texels) {
    dump.open(vkr_filesystem_native_utf8_path(options.dump_texels),
              std::ios::binary | std::ios::trunc);
    dump << "page,layer,x,y,px,py,pz,nx,ny,nz,valid,direct_r,direct_g,"
            "direct_b,indirect_r,indirect_g,indirect_b,r,g,b,dir_x,dir_y,"
            "dir_z,directionality\n";
  }
  pages.stream = &stream;
  pages.dump = &dump;

  bool ok = !options.dump_texels || (bool)dump;
  for (uint32_t page = 0u; ok && page < layout.page_count; ++page) {
    ok = bake_page(&pages, page);
  }
  /* The lamp-direct pages follow every other page (ADR-108). */
  if (ok && plan.any_lamp_direct) {
    const auto direct_start = std::chrono::steady_clock::now();
    const double gpu_before = pages.gpu_seconds;
    ok = bake_direct_pages(&pages, plan.lamp_direct, plan.direct_layout);
    std::printf("direct_s=%.2f direct_gpu_s=%.2f\n",
                seconds_since(direct_start), pages.gpu_seconds - gpu_before);
  }
  gpu.reset();

  ok = ok && vkr_lightmap_set_write_prefix(&set, ~pages.payload_crc,
                                           prefix.data(), prefix.size());
  if (ok) {
    stream.seekp(0);
    stream.write((const char *)prefix.data(), (std::streamsize)prefix.size());
    stream.close();
    ok = !stream.fail();
  }
  ok = ok && (!options.index ||
              vkr_bake_lightmap_index_write(options.index, inputs,
                                            ~pages.payload_crc, file_size));
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
  if (!write_report(options, pages, previous, relight, layers,
                    seconds_since(bake_start))) {
    std::fprintf(stderr, "Writing the bake report failed\n");
    return 1;
  }
  std::printf("saved=%s bytes=%llu pages=%u layers=%zu instances=%zu "
              "gpu_s=%.2f cleanup_s=%.2f encode_s=%.2f gathered_texels=%llu "
              "total_texels=%llu relit_rects=%llu copied_rects=%llu\n",
              options.output, (unsigned long long)file_size, layout.page_count,
              layers.size(), instances.size(), pages.gpu_seconds,
              pages.cleanup_seconds, pages.encode_seconds,
              (unsigned long long)pages.gathered_texels,
              (unsigned long long)pages.total_texels,
              (unsigned long long)pages.relit_rects,
              (unsigned long long)pages.copied_rects);
  return 0;
}

/* One set's tiles of an instance's rectangle on plane `plane`, as the
   blocks of a width x height image. */
std::vector<uint8_t> rect_blocks(const VkrLightmapSet &set, uint32_t plane,
                                 uint32_t page, uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height) {
  const uint8_t *image =
      set.payload + vkr_lightmap_set_plane_offset(&set, page, plane);
  std::vector<uint8_t> blocks((size_t)(width / 4u) * (height / 4u) * 16u);
  size_t at = 0u;
  for (uint32_t ty = 0u; ty < height / 4u; ++ty) {
    for (uint32_t tx = 0u; tx < width / 4u; ++tx) {
      vkr_bake_lightmap_page_get_tile(
          image, set.page_size, VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR, x / 4u + tx,
          y / 4u + ty, blocks.data() + at);
      at += 16u;
    }
  }
  return blocks;
}

/* The rectangle of instance `i` of `set` on a plane's pages. */
VkrLightmapDirectRect instance_rect(const VkrLightmapSet &set, uint32_t i,
                                    bool direct) {
  if (direct) {
    return set.direct_rects[i];
  }
  const VkrLightmapInstance &instance = set.instances[i];
  return {instance.page, instance.x, instance.y, instance.width,
          instance.height};
}

/* The mean relative luminance difference over one rectangle's texels of
   `b` against `a`, scaled by the rectangle's mean luminance in `a`; false
   when a rectangle does not decode. */
bool rect_difference(const std::vector<uint8_t> &a,
                     const std::vector<uint8_t> &b, uint32_t width,
                     uint32_t height, float64_t *out_sum) {
  std::vector<float32_t> da;
  std::vector<float32_t> db;
  if (!vkr_bake_lightmap_decode_astc_hdr_image(a, width, height, &da) ||
      !vkr_bake_lightmap_decode_astc_hdr_image(b, width, height, &db)) {
    return false;
  }
  const size_t count = (size_t)width * height;
  float64_t mean = 0.0;
  for (size_t t = 0u; t < count; ++t) {
    mean += luminance(vec3_new(da[4u * t], da[4u * t + 1u], da[4u * t + 2u]));
  }
  mean = std::max(mean / (float64_t)count, 1.0e-6);
  float64_t sum = 0.0;
  for (size_t t = 0u; t < count; ++t) {
    const float64_t la =
        luminance(vec3_new(da[4u * t], da[4u * t + 1u], da[4u * t + 2u]));
    const float64_t lb =
        luminance(vec3_new(db[4u * t], db[4u * t + 1u], db[4u * t + 2u]));
    sum += std::fabs(lb - la) / mean;
  }
  *out_sum = sum;
  return true;
}

/*
 * Compares two sets' ASTC HDR planes instance by instance (ADR-088
 * incremental bakes): for every plane both hold and every instance both
 * hold with one rectangle size, whether its tiles are identical, and over
 * its texels the relative luminance difference of `b` against `a`, scaled
 * by the instance's mean luminance in `a`. Prints one line per plane and
 * the worst instance.
 */
int compare_sets(const Options &options, Arena *arena) {
  FileMapping mappings[2] = {};
  VkrLightmapSet sets[2] = {};
  for (uint32_t k = 0u; k < 2u; ++k) {
    const FilePath path = file_path_view(options.compare[k]);
    if (file_map_readonly(&path, &mappings[k]) != FILE_ERROR_NONE ||
        !vkr_lightmap_set_decode(mappings[k].data, mappings[k].size, arena,
                                 &sets[k])) {
      std::fprintf(stderr, "Cannot read the set %s\n", options.compare[k]);
      for (uint32_t m = 0u; m <= k; ++m) {
        if (mappings[m].data) {
          file_unmap(&mappings[m]);
        }
      }
      return 1;
    }
  }
  std::map<std::string, uint32_t> b_by_key;
  for (uint32_t i = 0u; i < sets[1].instance_count; ++i) {
    b_by_key[set_instance_key(sets[1].instances[i])] = i;
  }
  int result = 0;
  float64_t worst = 0.0;
  for (uint32_t p = 0u; result == 0 && p < sets[0].plane_count; ++p) {
    const VkrLightmapPlane &plane = sets[0].planes[p];
    uint32_t q = UINT32_MAX;
    for (uint32_t c = 0u; c < sets[1].plane_count; ++c) {
      if (sets[1].planes[c].layer == plane.layer &&
          sets[1].planes[c].kind == plane.kind &&
          sets[1].planes[c].format == plane.format) {
        q = c;
      }
    }
    if (plane.format != VKR_LIGHTMAP_FORMAT_ASTC_4X4_HDR || q == UINT32_MAX) {
      continue;
    }
    const bool direct = plane.kind == VKR_LIGHTMAP_PLANE_LAMP_DIRECT;
    uint32_t compared = 0u;
    uint32_t identical = 0u;
    uint64_t texels = 0u;
    float64_t error_sum = 0.0;
    float64_t instance_worst = 0.0;
    for (uint32_t i = 0u; i < sets[0].instance_count; ++i) {
      const auto found = b_by_key.find(set_instance_key(sets[0].instances[i]));
      if (found == b_by_key.end()) {
        continue;
      }
      const VkrLightmapDirectRect ra = instance_rect(sets[0], i, direct);
      const VkrLightmapDirectRect rb =
          instance_rect(sets[1], found->second, direct);
      if (ra.width == 0u || ra.width != rb.width || ra.height != rb.height) {
        continue;
      }
      const std::vector<uint8_t> a =
          rect_blocks(sets[0], p, ra.page, ra.x, ra.y, ra.width, ra.height);
      const std::vector<uint8_t> b =
          rect_blocks(sets[1], q, rb.page, rb.x, rb.y, rb.width, rb.height);
      const size_t count = (size_t)ra.width * ra.height;
      compared += 1u;
      texels += count;
      if (a == b) {
        identical += 1u;
        continue;
      }
      float64_t sum = 0.0;
      if (!rect_difference(a, b, ra.width, ra.height, &sum)) {
        result = 1;
        break;
      }
      error_sum += sum;
      instance_worst = std::max(instance_worst, sum / (float64_t)count);
    }
    worst = std::max(worst, instance_worst);
    std::printf("compare plane=%u layer=%u kind=%s instances=%u identical=%u "
                "mean_relative_luminance_error=%.6f "
                "worst_instance_relative_error=%.6f\n",
                p, plane.layer, direct ? "lamp_direct" : "irradiance", compared,
                identical, texels ? error_sum / (float64_t)texels : 0.0,
                instance_worst);
  }
  std::printf("compare worst_instance_relative_error=%.6f\n", worst);
  file_unmap(&mappings[0]);
  file_unmap(&mappings[1]);
  return result;
}

int run(const Options &options, VkrAllocator *allocator, Arena *arena) {
  if (options.compare[0]) {
    return compare_sets(options, arena);
  }
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
  /* Bounds from the triangles, so inspections and GPU bakes, which never
     trace the CPU BVH, skip building it. */
  Vec3 bounds_min = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 bounds_max = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  for (const VkrBakeTriangle &triangle : scene.triangles) {
    for (const VkrBakeVertex &vertex : triangle.vertex) {
      bounds_min = vec3_new(std::min(bounds_min.x, vertex.position.x),
                            std::min(bounds_min.y, vertex.position.y),
                            std::min(bounds_min.z, vertex.position.z));
      bounds_max = vec3_new(std::max(bounds_max.x, vertex.position.x),
                            std::max(bounds_max.y, vertex.position.y),
                            std::max(bounds_max.z, vertex.position.z));
    }
  }
  std::printf("scene_load_s=%.2f triangles=%zu lights=%zu "
              "lightmap_instances=%zu bounds=(%g %g %g)-(%g %g %g)\n",
              seconds_since(load_start), scene.triangles.size(),
              scene.lights.size(), scene.lightmap_instances.size(),
              bounds_min.x, bounds_min.y, bounds_min.z, bounds_max.x,
              bounds_max.y, bounds_max.z);

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

  /* What the set holds and the digest of its inputs, which an inspection
     reports too. */
  SetPlan plan;
  const int planned = plan_set(options, scene, layout, &plan);
  if (planned == 1) {
    return 1;
  }
  VkrBakeLightmapInputs inputs;
  if (planned == 0) {
    const auto inputs_start = std::chrono::steady_clock::now();
    if (!vkr_bake_lightmap_inputs_build(
            scene, settings_text(options), plan.records, plan.sun_keys, layout,
            plan.direct_layout,
            options.threads ? options.threads
                            : std::max(1u, std::thread::hardware_concurrency()),
            &inputs)) {
      std::fprintf(stderr, "Describing the bake's inputs failed\n");
      return 1;
    }
    std::printf("inputs_s=%.2f content_digest=%s\n",
                seconds_since(inputs_start),
                vkr_bake_digest_hex(inputs.digest).c_str());
  }
  if (options.manifest &&
      !write_manifest(
          options, scene, layout, planned == 0 ? plan.layers.size() : 0u,
          planned == 0 ? vkr_bake_digest_hex(inputs.digest) : std::string())) {
    std::fprintf(stderr, "Writing the bake manifest failed\n");
    return 1;
  }
  if (options.inspect) {
    return 0;
  }
  if (options.output) {
    if (planned == 3) {
      std::fprintf(stderr,
                   "The scene has no static lamps or emission, so a desktop "
                   "lightmap set has nothing to hold\n");
      return 3;
    }
    return bake_set(options, scene, arena, layout, plan, inputs);
  }

  /* The CPU modes trace the CPU BVH; the GPU trace check compares with it. */
  const auto bvh_start = std::chrono::steady_clock::now();
  VkrBakeBvh bvh = {};
  if (!vkr_bake_bvh_build(
          {scene.triangles.data(), (uint32_t)scene.triangles.size()}, arena,
          &bvh)) {
    std::fprintf(stderr, "BVH construction failed\n");
    return 1;
  }
  std::printf("bvh_s=%.2f\n", seconds_since(bvh_start));

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
          const uint32_t texel_seed =
              vkr_bake_lightmap_mix_seed(options.seed ^ texel.seed);
          for (uint32_t s = 0u; s < options.samples; ++s) {
            const uint32_t seed =
                vkr_bake_lightmap_mix_seed(texel_seed ^ (s * 0x85ebca6bu));
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

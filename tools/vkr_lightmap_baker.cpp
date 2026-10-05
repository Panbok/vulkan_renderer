#include "vkr_tool_entry.h"

extern "C" {
#include "core/logger.h"
#include "memory/vkr_arena_allocator.h"
}
#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_integrator.h"
#include "bake/vkr_bake_lightmap.h"
#include "bake/vkr_bake_scene.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

/*
 * Static-light lightmap baker (ADR-087, docs/proposals/tiled-pipeline.md).
 * The current stage measures transport cost: it packs and rasterizes the
 * scene's lightmapped instances and gathers cosine-weighted samples per texel
 * through the ADR-054 integrator, reporting throughput. Layer output follows.
 */

namespace {

struct Options {
  const char *scene = nullptr;
  uint32_t page_size = 4096u;
  float32_t texels_per_unit = 8.0f;
  uint32_t samples = 16u;
  uint32_t max_depth = 4u;
  uint32_t pages = UINT32_MAX;
  uint32_t threads = 0u;
  uint32_t seed = 1u;
};

void usage() {
  std::fprintf(
      stderr,
      "Lightmap baker: --scene <scene.json> [--page-size <texels>] "
      "[--texels-per-unit <world density>] [--samples <n>] [--max-depth <n>] "
      "[--pages <n>] [--threads <n>] [--seed <n>]\n");
}

bool parse_u32(const char *text, uint32_t *out) {
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (!end || *end != '\0' || value > UINT32_MAX)
    return false;
  *out = (uint32_t)value;
  return true;
}

bool parse(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const char *flag = argv[i];
    if (i + 1 >= argc)
      return false;
    const char *value = argv[++i];
    if (std::strcmp(flag, "--scene") == 0) {
      options->scene = value;
    } else if (std::strcmp(flag, "--page-size") == 0) {
      if (!parse_u32(value, &options->page_size))
        return false;
    } else if (std::strcmp(flag, "--texels-per-unit") == 0) {
      options->texels_per_unit = std::strtof(value, nullptr);
    } else if (std::strcmp(flag, "--samples") == 0) {
      if (!parse_u32(value, &options->samples))
        return false;
    } else if (std::strcmp(flag, "--max-depth") == 0) {
      if (!parse_u32(value, &options->max_depth))
        return false;
    } else if (std::strcmp(flag, "--pages") == 0) {
      if (!parse_u32(value, &options->pages))
        return false;
    } else if (std::strcmp(flag, "--threads") == 0) {
      if (!parse_u32(value, &options->threads))
        return false;
    } else if (std::strcmp(flag, "--seed") == 0) {
      if (!parse_u32(value, &options->seed))
        return false;
    } else {
      return false;
    }
  }
  return options->scene && options->samples > 0u && options->max_depth > 0u &&
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

int run(const Options &options, VkrAllocator *allocator, Arena *arena) {
  const auto load_start = std::chrono::steady_clock::now();
  VkrBakeScene scene(allocator);
  VkrBakeSceneError scene_error;
  if (!vkr_bake_scene_load(&scene, options.scene, &scene_error)) {
    std::fprintf(stderr, "Scene preparation failed (error %u): %s\n",
                 (unsigned)scene_error, options.scene);
    return 1;
  }
  if (scene.triangles.size() > VKR_BAKE_BVH_MAX_TRIANGLES)
    return 1;
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
  if (!vkr_bake_lightmap_pack(scene.lightmap_instances, options.page_size,
                              options.texels_per_unit, &layout)) {
    std::fprintf(stderr, "Lightmap packing failed\n");
    return 1;
  }
  uint64_t rect_texels = 0u;
  for (const VkrBakeLightmapRect &rect : layout.rects)
    rect_texels += (uint64_t)rect.width * rect.height;
  std::printf("pages=%u page_size=%u rects=%zu rect_texels=%llu\n",
              layout.page_count, layout.page_size, layout.rects.size(),
              (unsigned long long)rect_texels);

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
        if (first >= texels.size() || failed.load())
          break;
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
    for (uint32_t i = 0u; i < threads; ++i)
      pool.emplace_back(worker);
    for (std::thread &thread : pool)
      thread.join();
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
  if (!arena)
    return 1;
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

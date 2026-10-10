#include "filesystem/vkr_filesystem_cpp.h"
#include "vkr_tool_entry.h"
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "assets/vkr_diffuse_volume.h"
#include "bake/vkr_bake_bricks.h"
#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_gpu.h"
#include "bake/vkr_bake_integrator.h"
#include "bake/vkr_bake_layers.h"
#include "bake/vkr_bake_scene.h"
#include "bake/vkr_bake_sh.h"

extern "C" {
#include "core/logger.h"
#include "core/vkr_byte_io.h"
#include "memory/vkr_arena_allocator.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace {
struct Options {
  const char *scene = nullptr;
  /* Without --bounds the bricks cover the geometry's bounds. */
  VkrBakeAabb bounds = {};
  bool explicit_bounds = false;
  float spacing = 1.0f;
  uint32_t levels = 3u;
  float margin = 1.0f;
  bool inspect = false;
  const char *output = nullptr;
  const char *manifest = nullptr;
  uint32_t face_size = 8, samples = 4, max_depth = 12, seed = 1,
           photons = 1000000;
  float photon_radius = 0.0f;
  uint32_t threads = 0; // Zero bakes probes on every hardware thread.
  /* Bake probes with the CPU integrator even when the GPU probe gather is
     available; the CPU path is the reference and adds caustic photons. */
  bool cpu = false;
  /* Instead of a volume, bake this many evenly spaced probes of every layer
     on both paths and compare them. */
  uint32_t gpu_parity = 0u;
};

bool number(const char *text, float *out) {
  char *end = nullptr;
  *out = std::strtof(text, &end);
  return end != text && *end == '\0' && std::isfinite(*out);
}
bool integer(const char *text, uint32_t *out) {
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == text || *end || value > UINT32_MAX || text[0] == '-')
    return false;
  *out = (uint32_t)value;
  return true;
}
void usage() {
  std::fprintf(stderr,
               "Usage: vkr_diffuse_baker --scene scene.json --inspect "
               "[--bounds minx miny minz maxx maxy maxz] [--spacing 1] "
               "[--levels 3] [--margin 1]\n"
               "Bake: --output file.vkdv [--manifest file.json] "
               "[--face-size 8] [--samples 4]\n"
               "      [--max-depth 12] [--seed 1] [--photons 1000000] "
               "[--photon-radius meters] [--threads 0]\n"
               "      [--cpu] probes on the CPU integrator, not the GPU\n"
               "Parity: --gpu-parity <probes> bakes that many probes of every "
               "layer on the CPU and the GPU and compares them\n");
}
bool parse(int argc, char **argv, Options *out) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
      out->scene = argv[++i];
    else if (std::strcmp(argv[i], "--inspect") == 0)
      out->inspect = true;
    else if (std::strcmp(argv[i], "--bounds") == 0 && i + 6 < argc) {
      float values[6];
      for (float &v : values)
        if (!number(argv[++i], &v))
          return false;
      out->bounds = {{values[0], values[1], values[2]},
                     {values[3], values[4], values[5]}};
      out->explicit_bounds = true;
    } else if (std::strcmp(argv[i], "--spacing") == 0 && i + 1 < argc) {
      if (!number(argv[++i], &out->spacing))
        return false;
    } else if (std::strcmp(argv[i], "--levels") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->levels))
        return false;
    } else if (std::strcmp(argv[i], "--margin") == 0 && i + 1 < argc) {
      if (!number(argv[++i], &out->margin))
        return false;
    } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc)
      out->output = argv[++i];
    else if (std::strcmp(argv[i], "--manifest") == 0 && i + 1 < argc)
      out->manifest = argv[++i];
    else if (std::strcmp(argv[i], "--face-size") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->face_size))
        return false;
    } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->samples))
        return false;
    } else if (std::strcmp(argv[i], "--max-depth") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->max_depth))
        return false;
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->seed))
        return false;
    } else if (std::strcmp(argv[i], "--photons") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->photons))
        return false;
    } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->threads))
        return false;
    } else if (std::strcmp(argv[i], "--cpu") == 0) {
      out->cpu = true;
    } else if (std::strcmp(argv[i], "--gpu-parity") == 0 && i + 1 < argc) {
      if (!integer(argv[++i], &out->gpu_parity) || out->gpu_parity == 0u) {
        return false;
      }
    } else if (std::strcmp(argv[i], "--photon-radius") == 0 && i + 1 < argc) {
      if (!number(argv[++i], &out->photon_radius) || out->photon_radius <= 0.0f)
        return false;
    } else
      return false;
  }
  return out->scene && (out->inspect || out->output || out->gpu_parity) &&
         !(out->gpu_parity && (out->cpu || out->inspect || out->output)) &&
         out->spacing >= 0.05f && out->spacing <= 100.0f && out->levels >= 1u &&
         out->levels <= VKR_DIFFUSE_VOLUME_MAX_LEVELS && out->margin >= 0.0f &&
         out->margin <= 4.0f && out->face_size > 0 && out->face_size <= 32 &&
         out->samples > 0 && out->samples <= 65536 && out->max_depth > 0 &&
         out->max_depth <= 64 && out->photons <= 16000000 &&
         (!out->output || fs::u8path(out->output).extension() == ".vkdv");
}

std::string json_string(const std::string &value) {
  std::string result = "\"";
  const char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += c;
    } else if (c < 32) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else
      result += c;
  }
  return result + "\"";
}

bool write_atomic(const char *path, const void *bytes, uint64_t size) {
  fs::path target = vkr_filesystem_native_utf8_path(path);
  fs::path temporary = target;
  temporary += ".tmp." +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
  std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
  if (!stream)
    return false;
  stream.write((const char *)bytes, (std::streamsize)size);
  stream.close();
  std::error_code error;
  if (stream.fail()) {
    fs::remove(temporary, error);
    return false;
  }
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    error = std::error_code((int)GetLastError(), std::system_category());
#else
  fs::rename(temporary, target, error);
#endif
  if (error) {
    fs::remove(temporary);
    return false;
  }
  return true;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

uint32_t mix_seed(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  return x ^ (x >> 16);
}
float random_unit(uint32_t seed) {
  return (float)(mix_seed(seed) >> 8) * (1.0f / 16777216.0f);
}
Vec3 scene_environment(void *scene, Vec3 direction) {
  return vkr_bake_scene_sample_environment((const VkrBakeScene *)scene,
                                           direction);
}

/* A probe's path seeds follow its lattice position and level, not its index,
   so they do not change when placement elsewhere does. */
uint32_t probe_seed(const VkrBakeBrickProbe &probe) {
  return mix_seed(probe.lattice[0] ^
                  mix_seed(probe.lattice[1] ^
                           mix_seed(probe.lattice[2] ^
                                    mix_seed(probe.level + 0x2545f491u))));
}

/* One layer's L1 SH of one probe; see vkr_bake_sh_project_l1. */
struct ProbeSh {
  float v[3][4];
};

/* Everything placement and visibility leave for the layer bakes. */
struct Volume {
  VkrBakeBrickLayout layout = {};
  std::vector<VkrBakeBrickVisibility> visibility;
  /* Probes a stored brick names and that are valid: the ones layers bake. */
  std::vector<uint8_t> baked;
};

/* The Russian roulette start depth of both probe transports. */
uint32_t rr_start_depth(const Options &options) {
  return options.max_depth >= 4u ? 4u : 0u;
}

/* Where a probe's paths start: its lattice position moved by visibility. */
Vec3 probe_origin(const Volume &volume, uint32_t probe) {
  return vec3_add(vkr_bake_bricks_probe_position(&volume.layout, probe),
                  volume.visibility[probe].offset);
}

/* A probe's path seed under the bake seed `seed`. */
uint32_t probe_path_seed(const Volume &volume, uint32_t seed, uint32_t probe) {
  return mix_seed(seed ^ probe_seed(volume.layout.probes[probe]));
}

/* Samples of a rectangle light's direct light at a probe. */
constexpr uint32_t kRectangleDirectSamples = 64u;

/*
 * Bakes one layer's L1 SH of every probe in `probes` into `sh[i]` with the
 * CPU integrator: the layer's lights, the sky when it holds it and surface
 * emission when it holds it, with caustic photons from the layer's analytic
 * lights.
 */
bool bake_layer_cpu(const Options &options, VkrBakeScene &scene,
                    VkrBakeBvh &bvh, const Volume &volume,
                    const VkrBakeLayerPlan &plan,
                    const std::vector<uint32_t> &probes, uint32_t seed,
                    Arena *arena, ProbeSh *sh) {
  std::vector<VkrBakeSceneLight> lights;
  for (uint32_t index : plan.lights) {
    lights.push_back(scene.lights[index]);
  }
  VkrBakeIntegratorSettings settings = {};
  settings.scene = {&bvh,
                    scene.texture_store,
                    scene.materials.data(),
                    (uint32_t)scene.materials.size(),
                    lights.data(),
                    (uint32_t)lights.size(),
                    scene.subsurface_profiles,
                    scene.subsurface_profile_count};
  settings.environment_radiance = plan.sky ? scene_environment : nullptr;
  settings.environment_user = &scene;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = rr_start_depth(options);
  settings.max_transparent_layers = VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS;
  settings.ray_epsilon = 0.00001f;
  settings.exclude_emission = plan.emission ? false_v : true_v;
  VkrBakeIntegrator integrator = {};
  VkrBakeIntegratorError error = {};
  if (!vkr_bake_integrator_init(&settings, &integrator, &error)) {
    std::fprintf(stderr, "Transport preparation failed: %u\n", error);
    return false;
  }
  VkrBakePhotonMap photons = {};
  bool analytic_lights = false;
  for (const auto &light : lights)
    analytic_lights |=
        light.enabled && (light.kind == VkrBakeSceneLightKind::Rectangle
                              ? light.radiance > 0.0f
                              : light.intensity > 0.0f);
  const Scratch photon_scope = scratch_create(arena);
  if (options.photons && analytic_lights) {
    photons.photon_capacity = options.photons;
    photons.photons = (VkrBakePhoton *)arena_alloc(
        arena, sizeof(VkrBakePhoton) * (uint64_t)options.photons,
        ARENA_MEMORY_TAG_BUFFER);
    photons.cell_offsets = (uint32_t *)arena_alloc(
        arena, sizeof(uint32_t) * (32u * 32u * 32u + 1u),
        ARENA_MEMORY_TAG_BUFFER);
    VkrBakePhotonSettings photon_settings = {};
    photon_settings.seed = seed;
    photon_settings.photon_count = options.photons;
    photon_settings.batch_count = options.photons;
    photon_settings.max_depth = options.max_depth;
    photon_settings.grid_dimensions[0] = photon_settings.grid_dimensions[1] =
        photon_settings.grid_dimensions[2] = 32u;
    photon_settings.radius = options.photon_radius > 0
                                 ? options.photon_radius
                                 : 0.25f * options.spacing;
    if (!photons.photons || !photons.cell_offsets ||
        !vkr_bake_integrator_photon_map_reset(&integrator, photon_settings,
                                              &photons, &error) ||
        !vkr_bake_integrator_emit_photons(&integrator, photon_settings,
                                          &photons, &error) ||
        !vkr_bake_integrator_build_photon_grid(&photons, &error)) {
      std::fprintf(stderr, "Photon transport failed: %u\n", error);
      scratch_destroy(photon_scope, ARENA_MEMORY_TAG_BUFFER);
      return false;
    }
    std::printf("photons_emitted=%u caustic_deposits=%u radius=%g\n",
                options.photons, photons.photon_count, photon_settings.radius);
    settings.photon_map = &photons;
    if (!vkr_bake_integrator_init(&settings, &integrator, &error)) {
      scratch_destroy(photon_scope, ARENA_MEMORY_TAG_BUFFER);
      return false;
    }
  }
  const float32_t sh_deringing = scene.environment.sh_deringing;
  // Every path's seed derives from (probe, pixel, sample) and the integrator,
  // scene, textures and photon map are read-only while tracing, so probes
  // bake on independent workers and each probe keeps its serial sample
  // order: the volume is byte-identical for any worker count.
  const uint32_t probe_count = (uint32_t)probes.size();
  std::atomic<uint32_t> next_probe{0u};
  std::atomic<bool> failed{false};
  std::mutex report_mutex;
  auto bake_probes = [&]() {
    std::vector<Vec3> radiance(6u * options.face_size * options.face_size);
    for (;;) {
      const uint32_t index = next_probe.fetch_add(1u);
      if (index >= probe_count || failed.load()) {
        return;
      }
      const uint32_t probe = probes[index];
      const Vec3 position = probe_origin(volume, probe);
      const uint32_t base_seed = probe_path_seed(volume, seed, probe);
      for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t y = 0; y < options.face_size; ++y)
          for (uint32_t x = 0; x < options.face_size; ++x) {
            const uint32_t pixel =
                (face * options.face_size + y) * options.face_size + x;
            double sum[3] = {};
            for (uint32_t sample = 0; sample < options.samples; ++sample) {
              const uint32_t path_seed =
                  mix_seed(base_seed ^ mix_seed(pixel + 256u) ^
                           mix_seed(sample + 65536u));
              Vec3 direction = vkr_bake_cube_direction(
                  face,
                  2.0f * (x + random_unit(path_seed)) / options.face_size -
                      1.0f,
                  2.0f * (y + random_unit(path_seed ^ 0x9e3779b9u)) /
                          options.face_size -
                      1.0f);
              VkrBakeIntegratorResult traced = {};
              VkrBakeIntegratorError trace_error = {};
              if (!vkr_bake_integrator_trace(&integrator, position, direction,
                                             path_seed, &traced,
                                             &trace_error)) {
                std::lock_guard<std::mutex> lock(report_mutex);
                if (!failed.exchange(true)) {
                  std::fprintf(stderr,
                               "Path failed: probe=%u pixel=%u sample=%u "
                               "error=%u\n",
                               probe, pixel, sample, trace_error);
                }
                return;
              }
              sum[0] += traced.radiance.x;
              sum[1] += traced.radiance.y;
              sum[2] += traced.radiance.z;
            }
            radiance[pixel] = {(float)(sum[0] / options.samples),
                               (float)(sum[1] / options.samples),
                               (float)(sum[2] / options.samples)};
          }
      if (!vkr_bake_sh_project_l1(radiance.data(), options.face_size,
                                  sh_deringing, sh[index].v)) {
        failed.store(true);
        return;
      }
    }
  };
  uint32_t worker_count = options.threads;
  if (!worker_count) {
    worker_count = std::max(1u, std::thread::hardware_concurrency());
  }
  worker_count = std::min(worker_count, std::max(1u, probe_count));
  std::vector<std::thread> workers;
  workers.reserve(worker_count - 1u);
  for (uint32_t i = 1u; i < worker_count; ++i) {
    workers.emplace_back(bake_probes);
  }
  bake_probes();
  for (std::thread &worker : workers) {
    worker.join();
  }
  scratch_destroy(photon_scope, ARENA_MEMORY_TAG_BUFFER);
  return !failed.load();
}

/*
 * A lamp group's direct light at every probe in `probes` as L1 SH into
 * `sh[i]` (vkr_bake_integrator_direct_l1), which no gathered path holds
 * because no ray hits a punctual light. Each probe draws from its own seed,
 * so the result does not depend on the worker count.
 */
bool bake_lamp_direct(const Options &options, VkrBakeScene &scene,
                      VkrBakeBvh &bvh, const Volume &volume,
                      const VkrBakeLayerPlan &plan,
                      const std::vector<uint32_t> &probes, uint32_t seed,
                      ProbeSh *sh) {
  std::vector<VkrBakeSceneLight> lights;
  for (uint32_t index : plan.lights) {
    lights.push_back(scene.lights[index]);
  }
  VkrBakeIntegratorSettings settings = {};
  settings.scene = {&bvh,
                    scene.texture_store,
                    scene.materials.data(),
                    (uint32_t)scene.materials.size(),
                    lights.data(),
                    (uint32_t)lights.size(),
                    scene.subsurface_profiles,
                    scene.subsurface_profile_count};
  settings.max_depth = options.max_depth;
  settings.max_transparent_layers = VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS;
  settings.ray_epsilon = 0.00001f;
  VkrBakeIntegrator integrator = {};
  VkrBakeIntegratorError error = {};
  if (!vkr_bake_integrator_init(&settings, &integrator, &error)) {
    std::fprintf(stderr, "Direct light preparation failed: %u\n", error);
    return false;
  }
  const uint32_t probe_count = (uint32_t)probes.size();
  std::atomic<uint32_t> next_probe{0u};
  std::atomic<bool> failed{false};
  auto bake_probes = [&]() {
    for (;;) {
      const uint32_t index = next_probe.fetch_add(1u);
      if (index >= probe_count || failed.load()) {
        return;
      }
      const uint32_t probe = probes[index];
      VkrBakeIntegratorError probe_error = {};
      if (!vkr_bake_integrator_direct_l1(
              &integrator, probe_origin(volume, probe),
              probe_path_seed(volume, seed, probe), kRectangleDirectSamples,
              sh[index].v, &probe_error)) {
        if (!failed.exchange(true)) {
          std::fprintf(stderr, "Direct light failed: probe=%u error=%u\n",
                       probe, probe_error);
        }
        return;
      }
    }
  };
  uint32_t worker_count = options.threads;
  if (!worker_count) {
    worker_count = std::max(1u, std::thread::hardware_concurrency());
  }
  worker_count = std::min(worker_count, std::max(1u, probe_count));
  std::vector<std::thread> workers;
  workers.reserve(worker_count - 1u);
  for (uint32_t i = 1u; i < worker_count; ++i) {
    workers.emplace_back(bake_probes);
  }
  bake_probes();
  for (std::thread &worker : workers) {
    worker.join();
  }
  return !failed.load();
}

/*
 * Bakes one layer's L1 SH of every probe in `probes` into `sh[i]` with the
 * GPU probe gather: the same layer sources as bake_layer_cpu through the
 * GPU's lightmap transport subset, without caustic photons. Adds the GPU
 * time to `in_out_gpu_seconds`.
 */
bool bake_layer_gpu(const Options &options, const VkrBakeScene &scene,
                    VkrBakeGpuContext *gpu, const Volume &volume,
                    const VkrBakeLayerPlan &plan,
                    const std::vector<uint32_t> &probes, uint32_t seed,
                    ProbeSh *sh, double *in_out_gpu_seconds) {
  std::vector<Vec3> positions(probes.size());
  std::vector<uint32_t> seeds(probes.size());
  for (size_t i = 0u; i < probes.size(); ++i) {
    positions[i] = probe_origin(volume, probes[i]);
    seeds[i] = probe_path_seed(volume, seed, probes[i]);
  }
  VkrBakeGpuLayer layer;
  layer.lights = plan.lights;
  layer.sky = plan.sky;
  layer.emission = plan.emission;
  VkrBakeGpuProbeSettings settings;
  settings.face_size = options.face_size;
  settings.samples = options.samples;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = rr_start_depth(options);
  settings.deringing = scene.environment.sh_deringing;
  std::vector<float32_t> values;
  double seconds = 0.0;
  if (!vkr_bake_gpu_gather_probes(gpu, positions, seeds, layer, settings,
                                  &values, &seconds)) {
    std::fprintf(stderr, "GPU probe gather failed\n");
    return false;
  }
  *in_out_gpu_seconds += seconds;
  for (size_t i = 0u; i < probes.size(); ++i) {
    std::memcpy(sh[i].v, values.data() + 12u * i, sizeof(sh[i].v));
  }
  return true;
}

/* The probes the layers bake: the valid probes a stored brick names. */
std::vector<uint32_t> baked_probes(const Volume &volume) {
  std::vector<uint32_t> probes;
  for (uint32_t p = 0u; p < volume.layout.probe_count; ++p) {
    if (volume.baked[p]) {
      probes.push_back(p);
    }
  }
  return probes;
}

/* The volume's light layers (ADR-088): in an atmosphere scene one per sun
   key, each with its key's atmosphere, then one per static light group. */
bool plan_volume_layers(VkrBakeScene &scene,
                        std::vector<VkrBakeLayerPlan> *plans,
                        std::vector<VkrBakeAtmosphere> *key_atmospheres) {
  std::vector<Vec3> sun_keys;
  if (scene.atmosphere.enabled &&
      scene.atmosphere_light < scene.lights.size()) {
    sun_keys = vkr_bake_sun_key_directions(scene);
    key_atmospheres->resize(sun_keys.size());
    for (size_t k = 0u; k < sun_keys.size(); ++k) {
      if (!vkr_bake_scene_build_sun_atmosphere(&scene, sun_keys[k],
                                               &(*key_atmospheres)[k])) {
        std::fprintf(stderr, "Building the atmosphere of sun key %zu failed\n",
                     k);
        return false;
      }
    }
  }
  std::string plan_error;
  if (!vkr_bake_plan_layers(scene, sun_keys, plans, &plan_error)) {
    std::fprintf(stderr, "%s\n", plan_error.c_str());
    return false;
  }
  if (plans->size() > VKR_DIFFUSE_VOLUME_MAX_LAYERS) {
    std::fprintf(stderr,
                 "The scene makes %zu light layers; a volume holds at "
                 "most %u\n",
                 plans->size(), VKR_DIFFUSE_VOLUME_MAX_LAYERS);
    return false;
  }
  return true;
}

using GpuContext =
    std::unique_ptr<VkrBakeGpuContext, void (*)(VkrBakeGpuContext *)>;

/* Makes a sun key's atmosphere current on the CPU scene and, when the GPU
   bakes, on its copy. */
bool use_layer_lighting(VkrBakeScene &scene, const VkrBakeLayerPlan &plan,
                        const std::vector<VkrBakeAtmosphere> &key_atmospheres,
                        VkrBakeGpuContext *gpu) {
  if (plan.sun_key < 0) {
    return true;
  }
  vkr_bake_scene_use_atmosphere(&scene, key_atmospheres[(size_t)plan.sun_key]);
  return !gpu || vkr_bake_gpu_update_lighting(gpu, scene);
}

/*
 * Shortens each channel's linear term to at most its constant term, so the
 * runtime's E/pi(n) = c + dot(l, n) is non-negative for every normal. A
 * lamp's direct band projects |l| = 2c, and bright light near a lamp can do
 * the same in a gathered band; behind the lamp that channel then goes
 * negative, the runtime clamps it to zero and only the other channels'
 * light, such as the sky's blue, survives. Composition sums bands with
 * non-negative weights, so bounding each band bounds every composition.
 */
void clamp_l1_nonnegative(ProbeSh *sh) {
  for (uint32_t c = 0u; c < 3u; ++c) {
    const float constant = std::max(sh->v[c][3], 0.0f);
    const float linear =
        std::sqrt(sh->v[c][0] * sh->v[c][0] + sh->v[c][1] * sh->v[c][1] +
                  sh->v[c][2] * sh->v[c][2]);
    if (linear > constant) {
      const float scale = constant / linear;
      for (uint32_t k = 0u; k < 3u; ++k) {
        sh->v[c][k] *= scale;
      }
    }
  }
}

/* Luminance of a probe's constant term of E/pi. */
double constant_luminance(const ProbeSh &sh) {
  return 0.2126 * sh.v[0][3] + 0.7152 * sh.v[1][3] + 0.0722 * sh.v[2][3];
}

/* The mean of `b - a` over the probes, its paired z-score and its RMS. */
struct PairedDifference {
  double mean = 0.0;
  double z = 0.0;
  double rms = 0.0;
};

PairedDifference paired_difference(const std::vector<ProbeSh> &a,
                                   const std::vector<ProbeSh> &b) {
  PairedDifference result;
  const size_t n = a.size();
  if (n == 0u) {
    return result;
  }
  double sum = 0.0;
  double squares = 0.0;
  for (size_t i = 0u; i < n; ++i) {
    const double d = constant_luminance(b[i]) - constant_luminance(a[i]);
    sum += d;
    squares += d * d;
  }
  result.mean = sum / n;
  result.rms = std::sqrt(squares / n);
  if (n > 1u) {
    const double variance =
        std::max(0.0, (squares - n * result.mean * result.mean) / (n - 1u));
    const double error = std::sqrt(variance / n);
    result.z = error > 0.0 ? result.mean / error : 0.0;
  }
  return result;
}

/*
 * Bakes `options.gpu_parity` evenly spaced baked probes of every layer on the
 * CPU with the bake seed and the next seed, and on the GPU with the bake
 * seed, and reports per layer the mean constant-term luminance of each side,
 * the GPU's paired difference from the CPU (relative, z-score, per-probe RMS)
 * and that RMS over the RMS between the two CPU seeds: near one when the GPU
 * differs from the CPU only as much as another CPU seed does.
 */
int run_gpu_parity(const Options &options, VkrBakeScene &scene, VkrBakeBvh &bvh,
                   const Volume &volume, Arena *arena) {
  if (!vkr_bake_gpu_probe_gather_available()) {
    std::fprintf(stderr, "The GPU probe gather is unavailable on this host\n");
    return 3;
  }
  std::vector<VkrBakeLayerPlan> plans;
  std::vector<VkrBakeAtmosphere> key_atmospheres;
  if (!plan_volume_layers(scene, &plans, &key_atmospheres)) {
    return 1;
  }
  const std::vector<uint32_t> all = baked_probes(volume);
  const size_t count = std::min<size_t>(all.size(), options.gpu_parity);
  std::vector<uint32_t> probes(count);
  for (size_t i = 0u; i < count; ++i) {
    probes[i] = all[i * all.size() / count];
  }
  GpuContext gpu(vkr_bake_gpu_create(scene), vkr_bake_gpu_destroy);
  if (!gpu) {
    std::fprintf(stderr, "GPU transport setup failed\n");
    return 1;
  }
  std::printf("parity probes=%zu of %zu samples=%u face_size=%u photons=%u\n",
              count, all.size(), options.samples, options.face_size,
              options.photons);
  std::vector<ProbeSh> cpu(count);
  std::vector<ProbeSh> cpu_next(count);
  std::vector<ProbeSh> gpu_sh(count);
  for (uint32_t l = 0u; l < plans.size(); ++l) {
    const VkrBakeLayerPlan &plan = plans[l];
    if (!use_layer_lighting(scene, plan, key_atmospheres, gpu.get())) {
      return 1;
    }
    const auto cpu_start = std::chrono::steady_clock::now();
    if (!bake_layer_cpu(options, scene, bvh, volume, plan, probes, options.seed,
                        arena, cpu.data()) ||
        !bake_layer_cpu(options, scene, bvh, volume, plan, probes,
                        options.seed + 1u, arena, cpu_next.data())) {
      return 1;
    }
    const double cpu_seconds = seconds_since(cpu_start) * 0.5;
    double gpu_seconds = 0.0;
    if (!bake_layer_gpu(options, scene, gpu.get(), volume, plan, probes,
                        options.seed, gpu_sh.data(), &gpu_seconds)) {
      return 1;
    }
    double cpu_mean = 0.0;
    double gpu_mean = 0.0;
    for (size_t i = 0u; i < count; ++i) {
      cpu_mean += constant_luminance(cpu[i]);
      gpu_mean += constant_luminance(gpu_sh[i]);
    }
    cpu_mean /= std::max<size_t>(count, 1u);
    gpu_mean /= std::max<size_t>(count, 1u);
    const PairedDifference gpu_difference = paired_difference(cpu, gpu_sh);
    const PairedDifference seed_difference = paired_difference(cpu, cpu_next);
    std::printf(
        "parity layer=%u/%zu %s=%s cpu_s=%.2f gpu_s=%.3f cpu_mean=%.5g "
        "gpu_mean=%.5g relative_difference=%.4f z=%.2f probe_rms=%.4g "
        "seed_rms=%.4g rms_over_seed=%.3f seed_z=%.2f\n",
        l + 1u, plans.size(),
        plan.record.kind == VKR_LIGHT_LAYER_SUN_KEY ? "sun_key" : "lamp_group",
        plan.record.kind == VKR_LIGHT_LAYER_SUN_KEY
            ? std::to_string(plan.record.index).c_str()
            : plan.record.name,
        cpu_seconds, gpu_seconds, cpu_mean, gpu_mean,
        cpu_mean != 0.0 ? (gpu_mean - cpu_mean) / cpu_mean : 0.0,
        gpu_difference.z, gpu_difference.rms, seed_difference.rms,
        seed_difference.rms > 0.0 ? gpu_difference.rms / seed_difference.rms
                                  : 0.0,
        seed_difference.z);
    std::fflush(stdout);
  }
  return 0;
}

uint32_t level_span(uint32_t level) {
  uint32_t span = 1u;
  for (uint32_t i = 0u; i < level; ++i) {
    span *= 3u;
  }
  return span;
}

/* A brick probe slot whose SH comes from a coarser neighbor: the weighted
   probes of that brick at the slot's position. */
struct FaceFill {
  uint32_t slot;
  uint32_t probes[8];
  float weights[8];
};

/*
 * Finds the slots of finer bricks that touch an entry of a coarser brick.
 * Such a slot takes the coarser brick's interpolation at its position, so
 * light is continuous where levels meet. Weights skip invalid probes and
 * are renormalized; a slot whose coarse corners are all invalid keeps its
 * own value.
 */
std::vector<FaceFill> face_fills(const Volume &volume) {
  const VkrBakeBrickLayout &layout = volume.layout;
  std::vector<FaceFill> fills;
  for (uint32_t b = 0u; b < layout.brick_count; ++b) {
    const VkrDiffuseVolumeBrick &brick = layout.bricks[b];
    for (uint32_t i = 0u; i < VKR_DIFFUSE_VOLUME_BRICK_PROBES; ++i) {
      const uint32_t slot = b * VKR_DIFFUSE_VOLUME_BRICK_PROBES + i;
      const VkrBakeBrickProbe &probe = layout.probes[layout.brick_probes[slot]];
      /* Entries whose closed box holds the probe: two per axis where it
         lies on an entry boundary. */
      uint32_t first[3];
      uint32_t last[3];
      for (uint32_t axis = 0u; axis < 3u; ++axis) {
        const uint32_t lattice = probe.lattice[axis];
        last[axis] = std::min(lattice / 3u, layout.dimensions[axis] - 1u);
        first[axis] =
            lattice % 3u == 0u && lattice > 0u ? lattice / 3u - 1u : last[axis];
      }
      uint32_t coarse = VKR_DIFFUSE_VOLUME_ENTRY_EMPTY;
      uint32_t coarse_level = brick.level;
      for (uint32_t z = first[2]; z <= last[2]; ++z)
        for (uint32_t y = first[1]; y <= last[1]; ++y)
          for (uint32_t x = first[0]; x <= last[0]; ++x) {
            const uint32_t entry =
                layout.entries[x + layout.dimensions[0] *
                                       (y + layout.dimensions[1] * z)];
            if (entry == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY) {
              continue;
            }
            const uint32_t level =
                entry >> VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT;
            if (level > coarse_level) {
              coarse_level = level;
              coarse = entry & VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK;
            }
          }
      if (coarse == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY) {
        continue;
      }
      const VkrDiffuseVolumeBrick &source = layout.bricks[coarse];
      const float step = (float)level_span(source.level);
      uint32_t base[3];
      float fraction[3];
      for (uint32_t axis = 0u; axis < 3u; ++axis) {
        const float local =
            ((float)probe.lattice[axis] - 3.0f * (float)source.entry[axis]) /
            step;
        const float clamped = std::min(std::max(local, 0.0f), 3.0f);
        base[axis] = std::min((uint32_t)clamped, 2u);
        fraction[axis] = clamped - (float)base[axis];
      }
      FaceFill fill = {};
      fill.slot = slot;
      float total = 0.0f;
      for (uint32_t corner = 0u; corner < 8u; ++corner) {
        const uint32_t c[3] = {base[0] + (corner & 1u),
                               base[1] + ((corner >> 1u) & 1u),
                               base[2] + ((corner >> 2u) & 1u)};
        float weight = 1.0f;
        for (uint32_t axis = 0u; axis < 3u; ++axis) {
          weight *=
              (corner >> axis) & 1u ? fraction[axis] : 1.0f - fraction[axis];
        }
        const uint32_t corner_probe =
            layout.brick_probes[coarse * VKR_DIFFUSE_VOLUME_BRICK_PROBES +
                                c[0] + 4u * (c[1] + 4u * c[2])];
        fill.probes[corner] = corner_probe;
        fill.weights[corner] = volume.baked[corner_probe] ? weight : 0.0f;
        total += fill.weights[corner];
      }
      if (total <= 1.0e-6f) {
        continue;
      }
      for (float &weight : fill.weights) {
        weight /= total;
      }
      fills.push_back(fill);
    }
  }
  return fills;
}

void store_half(uint8_t *dst, float value) {
  const uint16_t half = vkr_float32_to_float16(value);
  dst[0] = (uint8_t)(half & 0xffu);
  dst[1] = (uint8_t)(half >> 8u);
}

/*
 * Bakes the volume's light layers (ADR-088): in an atmosphere scene one per
 * sun key, each under its key's atmosphere, then one per static light group.
 * The runtime weights them by the current sun and light group factors.
 */
int bake_volume(const Options &options, VkrBakeScene &scene, VkrBakeBvh &bvh,
                const Volume &volume, Arena *arena) {
  std::vector<VkrBakeLayerPlan> plans;
  std::vector<VkrBakeAtmosphere> key_atmospheres;
  if (!plan_volume_layers(scene, &plans, &key_atmospheres)) {
    return 1;
  }
  /* The GPU probe gather bakes when the host has it, unless --cpu asks for
     the reference integrator. */
  const bool use_gpu = !options.cpu && vkr_bake_gpu_probe_gather_available();
  GpuContext gpu(nullptr, vkr_bake_gpu_destroy);
  if (use_gpu) {
    const auto setup_start = std::chrono::steady_clock::now();
    gpu.reset(vkr_bake_gpu_create(scene));
    if (!gpu) {
      std::fprintf(stderr, "GPU transport setup failed; --cpu bakes the "
                           "probes on the CPU\n");
      return 1;
    }
    std::printf("gpu_setup_s=%.2f\n", seconds_since(setup_start));
  }
  const std::vector<uint32_t> probes = baked_probes(volume);
  std::printf("probe_transport=%s baked_probes=%zu caustic_photons=%s\n",
              use_gpu ? "gpu" : "cpu", probes.size(),
              !use_gpu && options.photons ? "on" : "off");
  std::vector<ProbeSh> baked_sh(probes.size());
  const VkrBakeBrickLayout &layout = volume.layout;
  const uint32_t layer_count = (uint32_t)plans.size();
  const uint32_t slot_count =
      layout.brick_count * VKR_DIFFUSE_VOLUME_BRICK_PROBES;
  const std::vector<FaceFill> fills = face_fills(volume);
  std::vector<VkrLightLayer> layers;
  /* A lamp group's direct light follows the layers as its own band. */
  uint32_t lamp_direct_count = 0u;
  for (const VkrBakeLayerPlan &plan : plans) {
    if (plan.record.kind == VKR_LIGHT_LAYER_LAMP_GROUP) {
      ++lamp_direct_count;
    }
  }
  const uint32_t band_count = layer_count + lamp_direct_count;
  /* Brick-major SH of every band, band-major as the file stores it. */
  std::vector<ProbeSh> slot_sh((size_t)slot_count * band_count);
  std::vector<ProbeSh> probe_sh(layout.probe_count);
  /* Stores the baked probes' SH as band `band`, non-negative; face-fill
     slots interpolate their source probes. */
  auto store_band = [&](uint32_t band) {
    std::fill(probe_sh.begin(), probe_sh.end(), ProbeSh{});
    for (size_t i = 0u; i < probes.size(); ++i) {
      probe_sh[probes[i]] = baked_sh[i];
      clamp_l1_nonnegative(&probe_sh[probes[i]]);
    }
    ProbeSh *slots = slot_sh.data() + (size_t)band * slot_count;
    for (uint32_t slot = 0u; slot < slot_count; ++slot) {
      slots[slot] = probe_sh[layout.brick_probes[slot]];
    }
    for (const FaceFill &fill : fills) {
      ProbeSh value = {};
      for (uint32_t corner = 0u; corner < 8u; ++corner) {
        const ProbeSh &source = probe_sh[fill.probes[corner]];
        for (uint32_t c = 0u; c < 3u; ++c)
          for (uint32_t k = 0u; k < 4u; ++k)
            value.v[c][k] += fill.weights[corner] * source.v[c][k];
      }
      slots[fill.slot] = value;
    }
  };
  uint32_t direct_band = layer_count;
  for (uint32_t l = 0u; l < layer_count; ++l) {
    const VkrBakeLayerPlan &plan = plans[l];
    layers.push_back(plan.record);
    if (!use_layer_lighting(scene, plan, key_atmospheres, gpu.get())) {
      return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    double gpu_seconds = 0.0;
    const bool baked =
        use_gpu
            ? bake_layer_gpu(options, scene, gpu.get(), volume, plan, probes,
                             options.seed, baked_sh.data(), &gpu_seconds)
            : bake_layer_cpu(options, scene, bvh, volume, plan, probes,
                             options.seed, arena, baked_sh.data());
    if (!baked) {
      return 1;
    }
    store_band(l);
    std::printf("baked_layer=%u/%u %s=%s lights=%zu transport=%s seconds=%.2f "
                "gpu_s=%.2f\n",
                l + 1u, layer_count,
                plan.record.kind == VKR_LIGHT_LAYER_SUN_KEY ? "sun_key"
                                                            : "lamp_group",
                plan.record.kind == VKR_LIGHT_LAYER_SUN_KEY
                    ? std::to_string(plan.record.index).c_str()
                    : plan.record.name,
                plan.lights.size(), use_gpu ? "gpu" : "cpu",
                seconds_since(start), gpu_seconds);
    std::fflush(stdout);
    if (plan.record.kind == VKR_LIGHT_LAYER_LAMP_GROUP) {
      const auto direct_start = std::chrono::steady_clock::now();
      if (!bake_lamp_direct(options, scene, bvh, volume, plan, probes,
                            options.seed, baked_sh.data())) {
        return 1;
      }
      store_band(direct_band++);
      std::printf("baked_direct_band=%u lamp_group=%s lights=%zu "
                  "seconds=%.2f\n",
                  direct_band, plan.record.name, plan.lights.size(),
                  seconds_since(direct_start));
      std::fflush(stdout);
    }
  }

  /* The stored SH is divided by the smallest power of two that keeps it
     well inside the half range. */
  float largest = 0.0f;
  for (const ProbeSh &value : slot_sh)
    for (uint32_t c = 0u; c < 3u; ++c)
      for (uint32_t k = 0u; k < 4u; ++k)
        largest = std::max(largest, std::fabs(value.v[c][k]));
  float sh_scale = 1.0f;
  while (largest / sh_scale > 32768.0f) {
    sh_scale *= 2.0f;
  }

  std::vector<uint8_t> aux((size_t)slot_count * VKR_DIFFUSE_VOLUME_AUX_BYTES);
  std::vector<uint8_t> moments((size_t)slot_count *
                               VKR_DIFFUSE_VOLUME_MOMENT_BYTES);
  std::vector<uint8_t> sh((size_t)slot_count * band_count *
                          VKR_DIFFUSE_VOLUME_SH_BYTES);
  for (uint32_t slot = 0u; slot < slot_count; ++slot) {
    const uint32_t probe = layout.brick_probes[slot];
    const VkrBakeBrickVisibility &visibility = volume.visibility[probe];
    uint8_t *record = aux.data() + (size_t)slot * VKR_DIFFUSE_VOLUME_AUX_BYTES;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      store_half(record + axis * 2u, visibility.offset.elements[axis]);
    }
    store_half(record + 6u, volume.baked[probe] ? 1.0f : 0.0f);
    uint8_t *tile =
        moments.data() + (size_t)slot * VKR_DIFFUSE_VOLUME_MOMENT_BYTES;
    for (uint32_t i = 0u; i < VKR_DIFFUSE_VOLUME_MOMENT_TEXELS * 2u; ++i) {
      store_half(tile + i * 2u, visibility.moments[i]);
    }
  }
  for (size_t i = 0u; i < slot_sh.size(); ++i) {
    uint8_t *record = sh.data() + i * VKR_DIFFUSE_VOLUME_SH_BYTES;
    for (uint32_t c = 0u; c < 3u; ++c)
      for (uint32_t k = 0u; k < 4u; ++k)
        store_half(record + (c * 4u + k) * 2u, slot_sh[i].v[c][k] / sh_scale);
  }
  std::vector<uint8_t> entries((size_t)layout.entry_count * sizeof(uint32_t));
  for (uint32_t i = 0u; i < layout.entry_count; ++i) {
    vkr_store_le_u32(entries.data() + (size_t)i * sizeof(uint32_t),
                     layout.entries[i]);
  }

  VkrDiffuseVolume file = {};
  file.origin = layout.origin;
  file.spacing = layout.spacing;
  file.sh_scale = sh_scale;
  file.level_count = layout.level_count;
  file.dimensions[0] = layout.dimensions[0];
  file.dimensions[1] = layout.dimensions[1];
  file.dimensions[2] = layout.dimensions[2];
  file.entries = entries.data();
  file.entry_count = layout.entry_count;
  file.bricks = layout.bricks;
  file.brick_count = layout.brick_count;
  file.probe_count = slot_count;
  file.probe_aux = aux.data();
  file.moments = moments.data();
  file.layer_sh = sh.data();
  file.layers = layers.data();
  file.layer_count = layer_count;
  file.lamp_direct_count = lamp_direct_count;
  const uint8_t *bytes = nullptr;
  uint64_t size = 0;
  VkrDiffuseVolume reopened = {};
  // parse requires an output path unless the run only inspects.
  assert_log(options.output != nullptr, "A bake requires an output path");
  if (!vkr_diffuse_volume_encode(&file, arena, &bytes, &size)) {
    std::fprintf(stderr, "Volume encode failed\n");
    return 1;
  }
  if (!vkr_diffuse_volume_decode(bytes, size, arena, &reopened)) {
    std::fprintf(stderr, "The encoded volume does not decode\n");
    return 1;
  }
  if (!write_atomic(options.output, bytes, size)) {
    std::fprintf(stderr, "Volume write failed: %s\n", options.output);
    return 1;
  }
  std::printf("saved=%s bytes=%llu sh_scale=%g face_fills=%zu\n",
              options.output, (unsigned long long)size, sh_scale, fills.size());
  return 0;
}

/* Builds the BVH of a scene with geometry and places its bricks.
   `out_over_budget` reports a placement that exceeds the DVOL limits. */
bool place_bricks(const Options &options, VkrBakeScene &scene, Arena *arena,
                  VkrBakeBvh *bvh, std::vector<VkrBakeBrickMaterial> *materials,
                  VkrBakeBrickLayout *layout, bool *out_over_budget) {
  *out_over_budget = false;
  if (scene.triangles.size() > VKR_BAKE_BVH_MAX_TRIANGLES)
    return false;
  if (!vkr_bake_bvh_build(
          {scene.triangles.data(), (uint32_t)scene.triangles.size()}, arena,
          bvh)) {
    std::fprintf(
        stderr,
        "BVH construction rejected invalid geometry or exhausted memory\n");
    return false;
  }
  VkrBakeBrickDesc desc = {};
  desc.bounds = options.explicit_bounds ? options.bounds : bvh->nodes[0].bounds;
  desc.spacing = options.spacing;
  desc.level_count = options.levels;
  desc.margin_spans = options.margin;
  const Vec3 extent = vec3_sub(desc.bounds.max, desc.bounds.min);
  if (!std::isfinite(extent.x) || !std::isfinite(extent.y) ||
      !std::isfinite(extent.z) || extent.x <= 0.0f || extent.y <= 0.0f ||
      extent.z <= 0.0f) {
    std::fprintf(stderr,
                 "Diffuse-volume bounds need positive extent on all three "
                 "axes (extent: %g, %g, %g). %s\n",
                 extent.x, extent.y, extent.z,
                 options.explicit_bounds
                     ? "Correct --bounds so every minimum is below its maximum."
                     : "The scene geometry does not enclose a 3D volume.");
    return false;
  }
  /* Surfaces light passes, such as non-transmissive blending, neither place
     bricks nor block probe visibility. */
  materials->resize(scene.materials.size());
  for (size_t i = 0; i < scene.materials.size(); ++i) {
    const auto &material = scene.materials[i];
    (*materials)[i].blocks =
        material.alpha_mode != VKR_BAKE_MATERIAL_ALPHA_BLEND ||
        material.transmission_factor > 0.0f;
    (*materials)[i].double_sided = material.double_sided;
  }
  if (!vkr_bake_bricks_place(bvh, materials->data(),
                             (uint32_t)materials->size(), desc, arena,
                             layout)) {
    std::fprintf(stderr,
                 "Brick placement failed: the bounds need at most %u "
                 "indirection entries and %u bricks; raise --spacing or "
                 "lower --margin\n",
                 VKR_DIFFUSE_VOLUME_MAX_ENTRIES, VKR_DIFFUSE_VOLUME_MAX_BRICKS);
    /* The spacing, levels, margin and bounds were checked above, so only
       the limits remain. */
    *out_over_budget = true;
    return false;
  }
  return true;
}

void count_levels(const VkrBakeBrickLayout &layout, uint32_t out[3]) {
  out[0] = out[1] = out[2] = 0u;
  for (uint32_t b = 0u; b < layout.brick_count; ++b) {
    ++out[layout.bricks[b].level];
  }
}

/* Relocates the probes, measures their visibility and drops bricks with no
   valid probe. */
bool prepare_visibility(const Options &options, const VkrBakeBvh &bvh,
                        const std::vector<VkrBakeBrickMaterial> &materials,
                        Volume *volume) {
  const auto start = std::chrono::steady_clock::now();
  VkrBakeBrickLayout &layout = volume->layout;
  volume->visibility.resize(layout.probe_count);
  if (!vkr_bake_bricks_visibility(&bvh, materials.data(),
                                  (uint32_t)materials.size(), &layout,
                                  options.threads, volume->visibility.data())) {
    std::fprintf(stderr, "Probe visibility failed\n");
    return false;
  }
  const uint32_t placed_bricks = layout.brick_count;
  vkr_bake_bricks_drop_invalid(&layout, volume->visibility.data());
  volume->baked.assign(layout.probe_count, 0u);
  for (uint32_t slot = 0u;
       slot < layout.brick_count * VKR_DIFFUSE_VOLUME_BRICK_PROBES; ++slot) {
    const uint32_t probe = layout.brick_probes[slot];
    volume->baked[probe] = volume->visibility[probe].valid ? 1u : 0u;
  }
  uint32_t valid = 0u;
  uint32_t relocated = 0u;
  uint32_t baked = 0u;
  for (uint32_t p = 0u; p < layout.probe_count; ++p) {
    const VkrBakeBrickVisibility &visibility = volume->visibility[p];
    valid += visibility.valid ? 1u : 0u;
    relocated += vec3_dot(visibility.offset, visibility.offset) > 0.0f;
    baked += volume->baked[p];
  }
  uint32_t levels[3];
  count_levels(layout, levels);
  std::printf("visibility probes=%u valid=%u relocated=%u baked=%u "
              "bricks=%u/%u level_bricks=%u,%u,%u seconds=%.2f\n",
              layout.probe_count, valid, relocated, baked, layout.brick_count,
              placed_bricks, levels[0], levels[1], levels[2],
              seconds_since(start));
  std::fflush(stdout);
  if (!layout.brick_count) {
    std::fprintf(stderr, "Every probe lies inside geometry; no brick holds a "
                         "valid probe\n");
    return false;
  }
  return true;
}

int inspect_scene(const Options &options, VkrAllocator *allocator,
                  Arena *arena) {
  VkrBakeScene scene(allocator);
  VkrBakeSceneError error;
  if (!vkr_bake_scene_load(&scene, options.scene, &error)) {
    std::fprintf(stderr, "Scene preparation failed (error %u): %s: %s\n",
                 (unsigned)error, options.scene, scene.diagnostic.c_str());
    return 1;
  }
  // Resolve aliases before any output can replace a source used by the bake.
  for (const auto &dependency : scene.dependency_paths) {
    const fs::path source =
        fs::weakly_canonical(vkr_filesystem_native_utf8_path(dependency));
    if ((options.output && fs::weakly_canonical(options.output) == source) ||
        (options.manifest &&
         fs::weakly_canonical(options.manifest) == source)) {
      std::fprintf(stderr, "Output aliases a bake input: %s\n",
                   dependency.c_str());
      return 1;
    }
  }
  if (options.output && options.manifest &&
      fs::weakly_canonical(options.output) ==
          fs::weakly_canonical(options.manifest))
    return 1;
  /* A scene without geometry places no brick: the manifest reports zero
     bricks, and the bakery skips the volume. */
  const auto start = std::chrono::steady_clock::now();
  VkrBakeBvh bvh = {};
  Volume volume;
  std::vector<VkrBakeBrickMaterial> materials;
  bool over_budget = false;
  if (!scene.triangles.empty() &&
      !place_bricks(options, scene, arena, &bvh, &materials, &volume.layout,
                    &over_budget)) {
    return over_budget ? VKR_DIFFUSE_VOLUME_OVER_BUDGET_EXIT : 1;
  }
  const VkrBakeBrickLayout &layout = volume.layout;
  uint32_t levels[3];
  count_levels(layout, levels);
  std::printf("triangles=%zu materials=%zu lights=%zu textures=%u "
              "dependencies=%zu zero_area_triangles=%u\n",
              scene.triangles.size(), scene.materials.size(),
              scene.lights.size(),
              vkr_bake_texture_store_count(scene.texture_store),
              scene.dependency_paths.size(), scene.zero_area_triangle_count);
  std::printf("entries=%u dimensions=%u,%u,%u bricks=%u level_bricks=%u,%u,%u "
              "probes=%u slots=%u placement_seconds=%.2f\n",
              layout.entry_count, layout.dimensions[0], layout.dimensions[1],
              layout.dimensions[2], layout.brick_count, levels[0], levels[1],
              levels[2], layout.probe_count,
              layout.brick_count * VKR_DIFFUSE_VOLUME_BRICK_PROBES,
              seconds_since(start));
  if (options.manifest) {
    std::ostringstream manifest;
    manifest << "{\"version\":1,\"dependencies\":[";
    for (size_t i = 0; i < scene.dependency_paths.size(); ++i) {
      if (i)
        manifest << ',';
      manifest << json_string(
          fs::weakly_canonical(scene.dependency_paths[i]).generic_string());
    }
    manifest << "],\"atmosphere\":{\"enabled\":"
             << (scene.atmosphere.enabled ? "true" : "false")
             << ",\"model_version\":" << VKR_BAKE_ATMOSPHERE_MODEL_VERSION
             << ",\"params_hash\":\"" << std::hex
             << vkr_bake_atmosphere_recipe_hash(&scene.atmosphere) << std::dec
             << "\",\"sh_deringing\":"
             << (scene.atmosphere.enabled ? scene.environment.sh_deringing
                                          : 0.0f)
             << "},\"triangles\":" << scene.triangles.size()
             << ",\"materials\":" << scene.materials.size()
             << ",\"zero_area_triangles\":" << scene.zero_area_triangle_count
             << ",\"lights\":" << scene.lights.size()
             << ",\"bricks\":" << layout.brick_count
             << ",\"probes\":" << layout.probe_count
             << ",\"entries\":" << layout.entry_count << ",\"level_bricks\":["
             << levels[0] << ',' << levels[1] << ',' << levels[2]
             << "],\"spacing\":" << layout.spacing
             << ",\"levels\":" << layout.level_count << ",\"origin\":["
             << layout.origin.x << ',' << layout.origin.y << ','
             << layout.origin.z << "],\"dimensions\":[" << layout.dimensions[0]
             << ',' << layout.dimensions[1] << ',' << layout.dimensions[2]
             << "]}\n";
    const std::string value = manifest.str();
    if (!write_atomic(options.manifest, value.data(), value.size()))
      return 1;
  }
  if (options.inspect)
    return 0;
  if (!layout.brick_count) {
    std::fprintf(stderr, "The scene has no geometry to place probes near\n");
    return 1;
  }
  if (!prepare_visibility(options, bvh, materials, &volume)) {
    return 1;
  }
  if (options.gpu_parity) {
    return run_gpu_parity(options, scene, bvh, volume, arena);
  }
  return bake_volume(options, scene, bvh, volume, arena);
}
} // namespace

VKR_TOOL_ENTRY(vkr_diffuse_baker_tool_main) {
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
    result = inspect_scene(options, &allocator, arena);
  } catch (const std::bad_alloc &) {
    std::fprintf(stderr, "Bake allocation failed\n");
  } catch (const fs::filesystem_error &error) {
    std::fprintf(stderr, "Bake filesystem error: %s\n", error.what());
  }
  log_shutdown();
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  return result;
}

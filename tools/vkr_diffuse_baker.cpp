#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "assets/vkr_diffuse_volume.h"
#include "bake/vkr_bake_bvh.h"
#include "bake/vkr_bake_integrator.h"
#include "bake/vkr_bake_scene.h"
#include "bake/vkr_bake_sh.h"
#include "bake/vkr_bake_voxels.h"

extern "C" {
#include "core/logger.h"
#include "memory/vkr_arena_allocator.h"
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace {
struct Options {
  const char *scene = nullptr;
  VkrBakeVoxelGridDesc grid = {{}, {4, 4, 4}, 0.0f};
  bool explicit_bounds = false;
  bool inspect = false;
  const char *output = nullptr;
  const char *manifest = nullptr;
  uint32_t face_size = 16, samples = 64, max_depth = 12, seed = 1,
           photons = 1000000;
  float photon_radius = 0.0f;
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
  std::fprintf(stderr, "Usage: vkr_diffuse_baker --scene scene.json --inspect "
                       "[--bounds minx miny minz maxx maxy maxz] [--grid x y "
                       "z] [--voxel-size meters]\n"
                       "Bake: --output file.vkdv [--manifest file.json] "
                       "[--face-size 16] [--samples 64]\n"
                       "      [--max-depth 12] [--seed 1] [--photons 1000000] "
                       "[--photon-radius meters]\n");
}
bool parse(int argc, char **argv, Options *out) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
      out->scene = argv[++i];
    else if (std::strcmp(argv[i], "--inspect") == 0)
      out->inspect = true;
    else if (std::strcmp(argv[i], "--grid") == 0 && i + 3 < argc) {
      for (unsigned a = 0; a < 3; ++a)
        if (!integer(argv[++i], &out->grid.probe_dimensions[a]))
          return false;
    } else if (std::strcmp(argv[i], "--bounds") == 0 && i + 6 < argc) {
      float values[6];
      for (float &v : values)
        if (!number(argv[++i], &v))
          return false;
      out->grid.bounds = {{values[0], values[1], values[2]},
                          {values[3], values[4], values[5]}};
      out->explicit_bounds = true;
    } else if (std::strcmp(argv[i], "--voxel-size") == 0 && i + 1 < argc) {
      if (!number(argv[++i], &out->grid.voxel_size) ||
          out->grid.voxel_size <= 0.0f)
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
    } else if (std::strcmp(argv[i], "--photon-radius") == 0 && i + 1 < argc) {
      if (!number(argv[++i], &out->photon_radius) || out->photon_radius <= 0.0f)
        return false;
    } else
      return false;
  }
  const uint64_t probe_count = (uint64_t)out->grid.probe_dimensions[0] *
                               out->grid.probe_dimensions[1] *
                               out->grid.probe_dimensions[2];
  return out->scene && (out->inspect || out->output) && out->face_size > 0 &&
         out->face_size <= 32 && out->samples > 0 && out->samples <= 65536 &&
         out->max_depth > 0 && out->max_depth <= 64 &&
         out->photons <= 16000000 && out->grid.probe_dimensions[0] >= 2 &&
         out->grid.probe_dimensions[1] >= 2 &&
         out->grid.probe_dimensions[2] >= 2 &&
         out->grid.probe_dimensions[0] <= 256 &&
         out->grid.probe_dimensions[1] <= 256 &&
         out->grid.probe_dimensions[2] <= 256 && probe_count <= 256 &&
         (!out->output || fs::path(out->output).extension() == ".vkdv");
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
  fs::path target(path);
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
Vec3 legacy_environment(void *scene, Vec3 direction) {
  return vkr_bake_scene_sample_environment((const VkrBakeScene *)scene,
                                           direction);
}
Vec3 atmosphere_environment(void *scene, Vec3 direction) {
  return vkr_bake_atmosphere_sample(
      &static_cast<const VkrBakeScene *>(scene)->atmosphere, direction);
}

int bake_volume(const Options &options, VkrBakeScene &scene, VkrBakeBvh &bvh,
                const VkrBakeVoxelResult &rooms, Arena *arena) {
  VkrBakeIntegratorSettings settings = {};
  settings.scene = {&bvh,
                    scene.texture_store,
                    scene.materials.data(),
                    (uint32_t)scene.materials.size(),
                    scene.lights.data(),
                    (uint32_t)scene.lights.size(),
                    scene.subsurface_profiles, scene.subsurface_profile_count};
  settings.environment_radiance =
      scene.atmosphere.enabled
          ? atmosphere_environment
          : (scene.environment.enabled &&
                     scene.environment.kind != VkrBakeSceneEnvironmentKind::None
                 ? legacy_environment
                 : nullptr);
  settings.environment_user = &scene;
  settings.max_depth = options.max_depth;
  settings.rr_start_depth = options.max_depth >= 4u ? 4u : 0u;
  settings.max_transparent_layers = VKR_BAKE_INTEGRATOR_MAX_TRANSPARENT_LAYERS;
  settings.ray_epsilon = 0.00001f;
  VkrBakeIntegrator integrator = {};
  VkrBakeIntegratorError error = {};
  if (!vkr_bake_integrator_init(&settings, &integrator, &error)) {
    std::fprintf(stderr, "Transport preparation failed: %u\n", error);
    return 1;
  }
  VkrBakePhotonMap photons = {};
  bool analytic_lights = false;
  for (const auto &light : scene.lights)
    analytic_lights |=
        light.enabled && (light.kind == VkrBakeSceneLightKind::Rectangle
                              ? light.radiance > 0.0f
                              : light.intensity > 0.0f);
  if (options.photons && analytic_lights) {
    photons.photon_capacity = options.photons;
    photons.photons = (VkrBakePhoton *)arena_alloc(
        arena, sizeof(VkrBakePhoton) * (uint64_t)options.photons,
        ARENA_MEMORY_TAG_BUFFER);
    photons.cell_offsets = (uint32_t *)arena_alloc(
        arena, sizeof(uint32_t) * (32u * 32u * 32u + 1u),
        ARENA_MEMORY_TAG_BUFFER);
    VkrBakePhotonSettings photon_settings = {};
    photon_settings.seed = options.seed;
    photon_settings.photon_count = options.photons;
    photon_settings.batch_count = options.photons;
    photon_settings.max_depth = options.max_depth;
    photon_settings.grid_dimensions[0] = photon_settings.grid_dimensions[1] =
        photon_settings.grid_dimensions[2] = 32u;
    photon_settings.radius =
        options.photon_radius > 0
            ? options.photon_radius
            : 0.25f * std::fmin(rooms.spacing.x,
                                std::fmin(rooms.spacing.y, rooms.spacing.z));
    if (!photons.photons || !photons.cell_offsets ||
        !vkr_bake_integrator_photon_map_reset(&integrator, photon_settings,
                                              &photons, &error) ||
        !vkr_bake_integrator_emit_photons(&integrator, photon_settings,
                                          &photons, &error) ||
        !vkr_bake_integrator_build_photon_grid(&photons, &error)) {
      std::fprintf(stderr, "Photon transport failed: %u\n", error);
      return 1;
    }
    std::printf("photons_emitted=%u caustic_deposits=%u radius=%g\n",
                options.photons, photons.photon_count, photon_settings.radius);
    settings.photon_map = &photons;
    if (!vkr_bake_integrator_init(&settings, &integrator, &error))
      return 1;
  }
  std::vector<VkrDiffuseVolumeProbe> probes(rooms.probe_count);
  std::vector<Vec3> radiance(6u * options.face_size * options.face_size);
  const float32_t sh_deringing = scene.atmosphere.enabled
                                     ? scene.atmosphere.sh_deringing
                                     : scene.environment.sh_deringing;
  for (uint32_t probe = 0; probe < rooms.probe_count; ++probe) {
    probes[probe].region_id = rooms.probes[probe].region_id;
    if (!probes[probe].region_id)
      continue;
    for (uint32_t face = 0; face < 6; ++face)
      for (uint32_t y = 0; y < options.face_size; ++y)
        for (uint32_t x = 0; x < options.face_size; ++x) {
          const uint32_t pixel =
              (face * options.face_size + y) * options.face_size + x;
          double sum[3] = {};
          for (uint32_t sample = 0; sample < options.samples; ++sample) {
            uint32_t seed =
                mix_seed(options.seed ^ mix_seed(probe) ^
                         mix_seed(pixel + 256u) ^ mix_seed(sample + 65536u));
            Vec3 direction = vkr_bake_cube_direction(
                face, 2.0f * (x + random_unit(seed)) / options.face_size - 1.0f,
                2.0f * (y + random_unit(seed ^ 0x9e3779b9u)) /
                        options.face_size -
                    1.0f);
            VkrBakeIntegratorResult traced = {};
            if (!vkr_bake_integrator_trace(&integrator,
                                           rooms.probes[probe].position,
                                           direction, seed, &traced, &error)) {
              std::fprintf(
                  stderr, "Path failed: probe=%u pixel=%u sample=%u error=%u\n",
                  probe, pixel, sample, error);
              return 1;
            }
            sum[0] += traced.radiance.x;
            sum[1] += traced.radiance.y;
            sum[2] += traced.radiance.z;
          }
          radiance[pixel] = {(float)(sum[0] / options.samples),
                             (float)(sum[1] / options.samples),
                             (float)(sum[2] / options.samples)};
        }
    if (!vkr_bake_sh_project(radiance.data(), options.face_size, sh_deringing,
                             &probes[probe].sh))
      return 1;
    std::printf("baked_probe=%u/%u region=%u\n", probe + 1, rooms.probe_count,
                probes[probe].region_id);
    std::fflush(stdout);
  }
  VkrDiffuseVolume volume = {rooms.origin,
                             rooms.spacing,
                             {rooms.probe_dimensions[0],
                              rooms.probe_dimensions[1],
                              rooms.probe_dimensions[2]},
                             probes.data(),
                             rooms.probe_count,
                             rooms.cell_region_ids,
                             rooms.cell_count};
  const uint8_t *bytes = nullptr;
  uint64_t size = 0;
  VkrDiffuseVolume reopened = {};
  if (!vkr_diffuse_volume_encode(&volume, arena, &bytes, &size) ||
      !vkr_diffuse_volume_decode(bytes, size, arena, &reopened) ||
      !write_atomic(options.output, bytes, size)) {
    std::fprintf(stderr, "Volume encode/write failed\n");
    return 1;
  }
  std::printf("saved=%s bytes=%llu\n", options.output,
              (unsigned long long)size);
  return 0;
}

int inspect_scene(const Options &options, VkrAllocator *allocator,
                  Arena *arena) {
  VkrBakeScene scene(allocator);
  VkrBakeSceneError error;
  if (!vkr_bake_scene_load(&scene, options.scene, &error)) {
    std::fprintf(stderr, "Scene preparation failed (error %u): %s\n",
                 (unsigned)error, options.scene);
    return 1;
  }
  // Resolve aliases before any output can replace a source used by the bake.
  for (const auto &dependency : scene.dependency_paths) {
    const fs::path source = fs::weakly_canonical(dependency);
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
  if (scene.triangles.size() > VKR_BAKE_BVH_MAX_TRIANGLES)
    return 1;
  VkrBakeBvh bvh = {};
  if (!vkr_bake_bvh_build(
          {scene.triangles.data(), (uint32_t)scene.triangles.size()}, arena,
          &bvh)) {
    std::fprintf(
        stderr,
        "BVH construction rejected invalid geometry or exhausted memory\n");
    return 1;
  }
  VkrBakeVoxelGridDesc grid = options.grid;
  if (!options.explicit_bounds)
    grid.bounds = bvh.nodes[0].bounds;
  if (grid.voxel_size == 0.0f) {
    const Vec3 extent = vec3_sub(grid.bounds.max, grid.bounds.min);
    grid.voxel_size =
        std::fmin(extent.x / grid.probe_dimensions[0],
                  std::fmin(extent.y / grid.probe_dimensions[1],
                            extent.z / grid.probe_dimensions[2])) *
        VKR_BAKE_VOXEL_DEFAULT_SIZE_FRACTION;
  }
  std::unique_ptr<bool8_t[]> room_boundaries(
      new bool8_t[scene.materials.size()]);
  for (size_t i = 0; i < scene.materials.size(); ++i) {
    const auto &material = scene.materials[i];
    room_boundaries[i] = material.alpha_mode != VKR_BAKE_MATERIAL_ALPHA_BLEND ||
                         material.transmission_factor > 0.0f;
  }
  VkrBakeVoxelResult rooms = {};
  if (!vkr_bake_voxels_build(&bvh, room_boundaries.get(),
                             (uint32_t)scene.materials.size(), grid, arena,
                             &rooms)) {
    std::fprintf(stderr, "Room detection failed: bounds/grid must be finite, "
                         "at most 256 probes and 8M voxels\n");
    return 1;
  }
  uint32_t valid_probes = 0, valid_cells = 0, regions = 0;
  for (uint32_t i = 0; i < rooms.probe_count; ++i) {
    valid_probes += rooms.probes[i].region_id != 0;
    if (rooms.probes[i].region_id > regions)
      regions = rooms.probes[i].region_id;
  }
  for (uint32_t i = 0; i < rooms.cell_count; ++i)
    valid_cells += rooms.cell_region_ids[i] != 0;
  std::printf("triangles=%zu materials=%zu lights=%zu textures=%u "
              "dependencies=%zu zero_area_triangles=%u\n",
              scene.triangles.size(), scene.materials.size(),
              scene.lights.size(),
              vkr_bake_texture_store_count(scene.texture_store),
              scene.dependency_paths.size(), scene.zero_area_triangle_count);
  std::printf(
      "probes=%u valid_probes=%u cells=%u valid_cells=%u max_region_id=%u\n",
      rooms.probe_count, valid_probes, rooms.cell_count, valid_cells, regions);
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
             << "\",\"sh_deringing\":" << scene.atmosphere.sh_deringing
             << "},\"triangles\":" << scene.triangles.size()
             << ",\"materials\":" << scene.materials.size()
             << ",\"zero_area_triangles\":" << scene.zero_area_triangle_count
             << ",\"lights\":" << scene.lights.size()
             << ",\"probes\":" << rooms.probe_count
             << ",\"valid_probes\":" << valid_probes
             << ",\"cells\":" << rooms.cell_count
             << ",\"valid_cells\":" << valid_cells << ",\"spacing\":["
             << rooms.spacing.x << ',' << rooms.spacing.y << ','
             << rooms.spacing.z << "],\"origin\":[" << rooms.origin.x << ','
             << rooms.origin.y << ',' << rooms.origin.z << "],\"dimensions\":["
             << rooms.probe_dimensions[0] << ',' << rooms.probe_dimensions[1]
             << ',' << rooms.probe_dimensions[2] << "]}\n";
    const std::string value = manifest.str();
    if (!write_atomic(options.manifest, value.data(), value.size()))
      return 1;
  }
  if (options.inspect)
    return 0;
  if (!valid_cells) {
    std::fprintf(
        stderr,
        "No valid room cells; refine the grid or close actual geometry gaps\n");
    return 1;
  }
  return bake_volume(options, scene, bvh, rooms, arena);
}
} // namespace

int main(int argc, char **argv) {
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

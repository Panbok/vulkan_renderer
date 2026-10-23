/*
 * `vkr_bakery tool lighting-baker --handoff <dir>`: runs the lightmap bake
 * and then the diffuse-volume bake of one project job in one process, which
 * loads their scene once (ADR-088 Rebakes). `vkr_bakery bake lightmap` and
 * `bake diffuse`, given the same `--handoff` directory, each write
 * `<dir>/<kind>.args` instead of starting their baker: the kind's log on
 * the first line, then the baker's arguments from "tool", one per line.
 * Each baker's exit status goes to `<dir>/<kind>.code`.
 *
 * The lightmap bake runs first: the diffuse baker's BVH reorders the
 * scene's triangles, which a lightmap bake reads in load order. Between
 * the two the scene gets back the atmosphere the lightmap bake's sun keys
 * replace, so the diffuse baker reads the scene as loaded.
 */

#include "vkr_tool_entry.h"

extern "C" {
#include "core/logger.h"
#include "memory/vkr_arena_allocator.h"
}

#include "bake/vkr_bake_scene.h"
#include "filesystem/vkr_filesystem_cpp.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

extern "C" int vkr_lightmap_baker_tool_main(int argc, char **argv);
extern "C" int vkr_diffuse_baker_tool_main(int argc, char **argv);

namespace {

/* One handed-off bake: its log and its baker's arguments after "tool". */
struct HandedOffBake {
  std::string log;
  std::vector<std::string> arguments;
};

bool read_handoff(const std::string &path, HandedOffBake *out) {
  std::ifstream file(vkr_filesystem_native_utf8_path(path));
  if (!file) {
    return false;
  }
  std::string line;
  if (!std::getline(file, out->log) || out->log.empty()) {
    return false;
  }
  while (std::getline(file, line)) {
    out->arguments.push_back(line);
  }
  /* "tool", the baker's name and at least one option. */
  return out->arguments.size() > 2u && out->arguments[0] == "tool";
}

/* The value after `flag` in a baker's arguments, empty without one. */
std::string flag_value(const HandedOffBake &bake, const char *flag) {
  for (size_t i = 0u; i + 1u < bake.arguments.size(); ++i) {
    if (bake.arguments[i] == flag) {
      return bake.arguments[i + 1u];
    }
  }
  return std::string();
}

/* Writes `code` to `path` through a temporary file, so a reader sees all of
   it or nothing. */
bool write_code(const std::string &path, int code) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream file(vkr_filesystem_native_utf8_path(temporary),
                       std::ios::trunc);
    if (!(file << code << '\n')) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(vkr_filesystem_native_utf8_path(temporary),
                          vkr_filesystem_native_utf8_path(path), error);
  return !error;
}

/* Runs a baker with its output appended to its own log. */
int run_baker(const HandedOffBake &bake, int (*baker)(int, char **)) {
  std::fflush(stdout);
  std::fflush(stderr);
  if (!std::freopen(bake.log.c_str(), "a", stdout) ||
      !std::freopen(bake.log.c_str(), "a", stderr)) {
    return 1;
  }
  std::vector<char *> argv;
  for (size_t i = 1u; i < bake.arguments.size(); ++i) {
    argv.push_back(const_cast<char *>(bake.arguments[i].c_str()));
  }
  argv.push_back(nullptr);
  const int code = baker((int)argv.size() - 1, argv.data());
  std::fflush(stdout);
  std::fflush(stderr);
  return code;
}

} // namespace

VKR_TOOL_ENTRY(vkr_lighting_baker_tool_main) {
  if (argc != 3 || std::strcmp(argv[1], "--handoff") != 0) {
    std::fprintf(stderr, "usage: lighting-baker --handoff <dir>\n");
    return 2;
  }
  const std::string directory = argv[2];
  HandedOffBake lightmap;
  HandedOffBake diffuse;
  if (!read_handoff(directory + "/lightmap.args", &lightmap) ||
      !read_handoff(directory + "/diffuse.args", &diffuse) ||
      lightmap.arguments[1] != "lightmap-baker" ||
      diffuse.arguments[1] != "diffuse-baker") {
    std::fprintf(stderr,
                 "lighting-baker: %s holds no lightmap and diffuse "
                 "bake\n",
                 directory.c_str());
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
  int result = 0;
  try {
    /* The two bakes share a scene only when they read the same text with
       the same atmospheres; otherwise each loads its own as it would
       alone. */
    VkrBakeScene scene(&allocator);
    const std::string scene_path = flag_value(lightmap, "--scene");
    const std::string atmosphere_cache =
        flag_value(lightmap, "--atmosphere-cache");
    bool shared = !scene_path.empty() &&
                  scene_path == flag_value(diffuse, "--scene") &&
                  atmosphere_cache == flag_value(diffuse, "--atmosphere-cache");
    VkrBakeSceneError error = VkrBakeSceneError::None;
    if (shared) {
      scene.atmosphere_cache = atmosphere_cache;
      shared = vkr_bake_scene_load(&scene, scene_path.c_str(), &error);
    }
    log_shutdown();
    VkrBakeAtmosphere loaded_atmosphere;
    if (shared) {
      loaded_atmosphere = scene.atmosphere;
      vkr_bake_scene_share(&scene, scene_path.c_str());
    }
    const int lightmap_code = run_baker(lightmap, vkr_lightmap_baker_tool_main);
    if (shared) {
      vkr_bake_scene_use_atmosphere(&scene, loaded_atmosphere);
    }
    const int diffuse_code = run_baker(diffuse, vkr_diffuse_baker_tool_main);
    vkr_bake_scene_share(nullptr, nullptr);
    if (!write_code(directory + "/lightmap.code", lightmap_code) ||
        !write_code(directory + "/diffuse.code", diffuse_code)) {
      result = 1;
    }
  } catch (const std::bad_alloc &) {
    vkr_bake_scene_share(nullptr, nullptr);
    result = 1;
  }
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  return result;
}

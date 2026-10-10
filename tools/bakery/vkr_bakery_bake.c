#include "vkr_bakery_bake.h"

#include "assets/vkr_diffuse_volume.h"
#include "assets/vkr_lightmap_set.h"
#include "assets/vkr_material_graph.h"
#include "core/vkr_hash.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "level/vkr_brush.h"
#include "level/vkr_surface.h"
#include "math/mat.h"
#include "math/vkr_quat.h"
#include "meshoptimizer.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_buffer.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

/* `vkr_bakery bake diffuse`, `vkr_bakery bake probe` and `vkr_bakery bake
 * lightmap`: the scene bakes of ADR-054, ADR-019 and ADR-087. Diffuse and
 * probe bakes were tools/bake_diffuse_volume.py and
 * tools/bake_reflection_probe.py; their options, provenance sidecars,
 * evidence directories and exit codes keep those scripts' contracts, and the
 * lightmap bake follows the diffuse one. The bakers run as
 * `vkr_bakery tool <name>` children of this process. */

#define VKR_BAKE_VERSION 1
#define VKR_BAKE_DIAGNOSTIC_BYTES 4096u
#define VKR_BAKE_MAX_JSON_BYTES (256ull * 1024ull * 1024ull)
#define VKR_BAKE_PATH VKR_BAKERY_PATH_CAPACITY
#define VKR_BAKE_PROBE_CHANNEL "hdr_post_transmission"
#define VKR_BAKE_PROBE_CASE "local.probe_bake"
/* Boot warmup before the first face, then frames each later face renders
   after the previous capture; both exceed the readback latency. */
#define VKR_BAKE_PROBE_WARMUP_FRAMES 4
#define VKR_BAKE_PROBE_FACE_FRAMES 8u
#define VKR_BAKE_SCENE_MANIFEST_KIND "vkr.harness.scene-content-manifest"

typedef struct VkrBake {
  const VkrBakeryConfig *config;
  Arena *arena;
  char repo[VKR_BAKE_PATH];
  char error[4096];
  bool8_t failed;
  /* Exit status of the last baker vkr_bake_run_baker ran. */
  int32_t baker_code;
} VkrBake;

#define VKR_BAKE_TRY(expression)                                               \
  do {                                                                         \
    if (!(expression)) {                                                       \
      return false_v;                                                          \
    }                                                                          \
  } while (0)

vkr_internal bool8_t vkr_bake_fail(VkrBake *bake, const char *format, ...) {
  if (!bake->failed) {
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(bake->error, sizeof(bake->error), format, arguments);
    va_end(arguments);
  }
  bake->failed = true_v;
  return false_v;
}

vkr_internal const char *vkr_bake_printf(VkrBake *bake, const char *format,
                                         ...) {
  va_list arguments;
  va_start(arguments, format);
  va_list copy;
  va_copy(copy, arguments);
  const int length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  char *text =
      (char *)arena_alloc(bake->arena, (uint64_t)(length > 0 ? length : 0) + 1u,
                          ARENA_MEMORY_TAG_STRING);
  if (text) {
    (void)vsnprintf(text, (size_t)length + 1u, format, arguments);
  }
  va_end(arguments);
  return text ? text : "";
}

vkr_internal const char *vkr_bake_text(const VkrBakeryJson *object,
                                       const char *key) {
  const VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  return value && value->type == VKR_BAKERY_JSON_STRING
             ? (const char *)value->string.str
             : NULL;
}

vkr_internal bool8_t vkr_bake_number(const VkrBakeryJson *value,
                                     float64_t *out) {
  if (value && value->type == VKR_BAKERY_JSON_INT) {
    *out = (float64_t)value->integer;
    return true_v;
  }
  if (value && value->type == VKR_BAKERY_JSON_FLOAT) {
    *out = value->number;
    return true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_bake_int(const VkrBakeryJson *value, int64_t *out) {
  if (value && value->type == VKR_BAKERY_JSON_INT) {
    *out = value->integer;
    return true_v;
  }
  return false_v;
}

/* sha256:<hex> of a file. */
vkr_internal bool8_t vkr_bake_digest(VkrBake *bake, const char *path,
                                     char out[72]) {
  char hex[VKR_BAKERY_SHA256_HEX];
  if (!vkr_bakery_hash_file(path, hex, NULL)) {
    return vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'",
                         path);
  }
  (void)snprintf(out, 72, "sha256:%s", hex);
  return true_v;
}

vkr_internal VkrBakeryJson *vkr_bake_load(VkrBake *bake, const char *path,
                                          const char *label) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, VKR_BAKE_MAX_JSON_BYTES, &data, &length)) {
    vkr_bake_fail(bake, "%s: [Errno 2] No such file or directory: '%s'", label,
                  path);
    return NULL;
  }
  VkrBakeryJsonError error;
  VkrBakeryJson *value =
      vkr_bakery_json_parse(bake->arena, data, length, 256u, &error);
  free(data);
  if (!value) {
    vkr_bake_fail(bake, "%s: %s: line %u column %u", label, error.message,
                  error.line, error.column);
  }
  return value;
}

/* Recursive copy with object members in key order: json.dump(sort_keys). */
vkr_internal VkrBakeryJson *vkr_bake_sorted(Arena *arena,
                                            const VkrBakeryJson *value) {
  if (!value || value->type != VKR_BAKERY_JSON_OBJECT) {
    if (value && value->type == VKR_BAKERY_JSON_ARRAY) {
      VkrBakeryJson *copy = vkr_bakery_json_array(arena);
      for (const VkrBakeryJson *item = value->first; item; item = item->next) {
        vkr_bakery_json_append(copy, vkr_bake_sorted(arena, item));
      }
      return copy;
    }
    return vkr_bakery_json_clone(arena, value);
  }
  const VkrBakeryJson *fields[1024];
  uint32_t count = 0u;
  for (const VkrBakeryJson *field = value->first;
       field && count < ArrayCount(fields); field = field->next) {
    fields[count++] = field;
  }
  for (uint32_t i = 1u; i < count; ++i) {
    const VkrBakeryJson *current = fields[i];
    uint32_t j = i;
    while (j > 0u && strcmp((const char *)fields[j - 1u]->key.str,
                            (const char *)current->key.str) > 0) {
      fields[j] = fields[j - 1u];
      j -= 1u;
    }
    fields[j] = current;
  }
  VkrBakeryJson *copy = vkr_bakery_json_object(arena);
  for (uint32_t i = 0u; i < count; ++i) {
    vkr_bakery_json_set(arena, copy, (const char *)fields[i]->key.str,
                        vkr_bake_sorted(arena, fields[i]));
  }
  return copy;
}

vkr_internal bool8_t vkr_bake_write_json(VkrBake *bake, const char *path,
                                         const VkrBakeryJson *value,
                                         bool8_t sort_keys) {
  String8 text = {0};
  const VkrBakeryJson *ordered =
      sort_keys ? vkr_bake_sorted(bake->arena, value) : value;
  if (!vkr_bakery_json_write(bake->arena, ordered, VKR_BAKERY_JSON_PRETTY,
                             &text)) {
    return vkr_bake_fail(bake, "Out of range float values are not JSON "
                               "compliant");
  }
  char directory[VKR_BAKE_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  VkrBakeryBuffer buffer = {0};
  vkr_bakery_buffer_append(&buffer, text.str, text.length);
  vkr_bakery_buffer_append(&buffer, "\n", 1u);
  const bool8_t ok =
      !buffer.failed && vkr_bakery_make_directories(directory) &&
      vkr_bakery_write_file_atomic(path, buffer.data, buffer.length);
  vkr_bakery_buffer_free(&buffer);
  return ok ? true_v : vkr_bake_fail(bake, "Cannot write %s", path);
}

vkr_internal bool8_t vkr_bake_resolve(const char *path, char *out) {
  char absolute[VKR_BAKE_PATH];
  if (!vkr_bakery_path_absolute(path, absolute, sizeof(absolute))) {
    return false_v;
  }
#if defined(_WIN32)
  (void)snprintf(out, VKR_BAKE_PATH, "%s", absolute);
  return true_v;
#else
  if (realpath(absolute, out)) {
    return true_v;
  }
  /* Resolve the existing parent; a missing leaf keeps its name. */
  char parent[VKR_BAKE_PATH];
  char resolved_parent[VKR_BAKE_PATH];
  vkr_bakery_path_parent(parent, sizeof(parent), absolute);
  if (realpath(parent, resolved_parent)) {
    return vkr_bakery_path_join(out, VKR_BAKE_PATH, resolved_parent,
                                vkr_bakery_path_name(absolute));
  }
  (void)snprintf(out, VKR_BAKE_PATH, "%s", absolute);
  return true_v;
#endif
}

vkr_internal bool8_t vkr_bake_is_under(const char *path, const char *root) {
  const uint64_t length = strlen(root);
  return strncmp(path, root, length) == 0 &&
         (path[length] == 0 || path[length] == '/' ||
          (length && root[length - 1u] == '/'));
}

/* os.path.relpath over absolute, normalized paths. */
vkr_internal bool8_t vkr_bake_relpath(const char *path, const char *start,
                                      char *out, uint32_t capacity) {
  uint64_t common = 0u;
  for (uint64_t index = 0u;; ++index) {
    const char a = path[index];
    const char b = start[index];
    if ((a == '/' || a == 0) && (b == '/' || b == 0)) {
      common = index;
    }
    if (a != b || a == 0) {
      break;
    }
  }
  uint32_t length = 0u;
  out[0] = 0;
  for (const char *c = start + common; *c; ++c) {
    if (*c == '/' && c[1] != 0 && c[1] != '/') {
      const int written =
          snprintf(out + length, capacity - length, "%s..", length ? "/" : "");
      if (written < 0 || (uint32_t)written >= capacity - length) {
        return false_v;
      }
      length += (uint32_t)written;
    }
  }
  const char *rest = path + common;
  while (*rest == '/') {
    rest += 1;
  }
  if (*rest) {
    const int written = snprintf(out + length, capacity - length, "%s%s",
                                 length ? "/" : "", rest);
    if (written < 0 || (uint32_t)written >= capacity - length) {
      return false_v;
    }
    length += (uint32_t)written;
  }
  if (!length) {
    (void)snprintf(out, capacity, ".");
  }
  return true_v;
}

/* Unique hex name for evidence directories: time, address space layout and
   a per-process counter. */
vkr_internal void vkr_bake_hex_id(char out[33]) {
  static uint32_t counter = 0u;
  char uuid[VKR_BAKERY_SHA256_HEX];
  char seed[96];
  (void)snprintf(seed, sizeof(seed), "%lld-%.9f-%p-%u",
                 (long long)vkr_bakery_unix_seconds(),
                 vkr_bakery_monotonic_seconds(), (void *)out, counter++);
  vkr_bakery_hash_bytes(seed, strlen(seed), uuid);
  (void)snprintf(out, 33, "%.32s", uuid);
}

vkr_internal bool8_t vkr_bake_is_cancelled(void *context) {
  (void)context;
  return vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                              VKR_MEMORY_ORDER_RELAXED)
             ? true_v
             : false_v;
}

/* Runs a child with stdout and stderr in `log`; returns its exit code. */
vkr_internal bool8_t vkr_bake_run(
    VkrBake *bake, const char *executable, const char *const *arguments,
    uint32_t count, const char *working_directory, const char *log,
    uint32_t timeout_ms, const VkrPlatformEnvironmentVariable *environment,
    uint32_t environment_count, int32_t *out_code) {
  (void)vkr_bakery_remove_file(log);
  const VkrPlatformProcessConfig process = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = count,
      .working_directory = working_directory,
      .stdout_path = log,
      .stderr_path = log,
      .append_output = true_v,
      .environment = environment,
      .environment_count = environment_count,
      .timeout_ms = timeout_ms,
      .termination_grace_ms = 5000u,
      .terminate_process_tree = true_v,
      .hidden = true_v,
      .is_cancelled = vkr_bake_is_cancelled,
  };
  bool8_t timed_out = false_v;
  *out_code = -1;
  if (!vkr_platform_process_run(&process, out_code, &timed_out)) {
    return vkr_bake_fail(bake, "Cannot run %s", executable);
  }
  if (vkr_bake_is_cancelled(NULL)) {
    return vkr_bake_fail(bake, "Bake cancelled");
  }
  if (timed_out) {
    return vkr_bake_fail(bake, "%s timed out after %u s; see %s",
                         vkr_bakery_path_name(executable), timeout_ms / 1000u,
                         log);
  }
  return true_v;
}

// =============================================================================
// Diffuse volumes (ADR-054, sparse bricks)
// =============================================================================

typedef struct VkrBakeDiffuse {
  const char *scene;
  const char *workspace_root;
  const char *output;
  const char *manifest;
  bool8_t inspect;
  bool8_t check;
  bool8_t has_bounds;
  float64_t bounds[6];
  /* The finest probe spacing, the brick levels and how far from geometry
     the finest bricks reach, in brick spans. */
  float64_t spacing;
  int64_t levels;
  float64_t margin;
  int64_t face_size;
  int64_t samples;
  int64_t max_depth;
  int64_t seed;
  int64_t photons;
  bool8_t has_photon_radius;
  float64_t photon_radius;
  /* Probes bake on the CPU integrator even where the GPU probe gather is
     available. */
  bool8_t cpu;
} VkrBakeDiffuse;

typedef struct VkrBakeArguments {
  const char *items[64];
  uint32_t count;
} VkrBakeArguments;

vkr_internal void vkr_bake_push(VkrBakeArguments *arguments,
                                const char *value) {
  if (arguments->count < ArrayCount(arguments->items)) {
    arguments->items[arguments->count++] = value;
  }
}

vkr_internal bool8_t vkr_bake_existing_file(VkrBake *bake, const char *value,
                                            const char *label, char *out) {
  if (!value || !value[0]) {
    return vkr_bake_fail(bake, "%s is required", label);
  }
  if (!vkr_bake_resolve(value, out) ||
      !vkr_bakery_stat(out, &(VkrBakeryStat){0})) {
    return vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'",
                         value);
  }
  if (!vkr_bakery_is_file(out)) {
    return vkr_bake_fail(bake, "%s must name a regular file", label);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_validate_recipe(VkrBake *bake,
                                              const VkrBakeDiffuse *args) {
  if (args->has_bounds) {
    for (uint32_t i = 0u; i < 6u; ++i) {
      if (!isfinite(args->bounds[i])) {
        return vkr_bake_fail(bake, "--bounds must be finite");
      }
    }
    for (uint32_t i = 0u; i < 3u; ++i) {
      if (args->bounds[i] >= args->bounds[i + 3u]) {
        return vkr_bake_fail(bake, "--bounds requires min < max on every axis");
      }
    }
  }
  if (!isfinite(args->spacing) || args->spacing < 0.05 ||
      args->spacing > 100.0) {
    return vkr_bake_fail(bake, "--spacing must be from 0.05 through 100");
  }
  if (args->levels < 1 || args->levels > 3) {
    return vkr_bake_fail(bake, "--levels must be from 1 through 3");
  }
  if (!isfinite(args->margin) || args->margin < 0.0 || args->margin > 4.0) {
    return vkr_bake_fail(bake, "--margin must be from 0 through 4");
  }
  if (args->face_size < 1 || args->face_size > 32) {
    return vkr_bake_fail(bake, "--face-size must be from 1 through 32");
  }
  if (args->samples < 1 || args->samples > 65536 || args->max_depth < 1 ||
      args->max_depth > 64) {
    return vkr_bake_fail(bake, "--samples must be 1..65536 and --max-depth "
                               "must be 1..64");
  }
  if (args->photons < 0 || args->photons > 16000000) {
    return vkr_bake_fail(bake, "--photons must be from 0 through 16000000");
  }
  if (args->seed < 0 || args->seed > 0xffffffffll) {
    return vkr_bake_fail(bake, "--seed must be an unsigned 32-bit value");
  }
  if (args->has_photon_radius &&
      (!isfinite(args->photon_radius) || args->photon_radius <= 0.0)) {
    return vkr_bake_fail(bake, "--photon-radius must be finite and positive");
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_recipe_arguments(VkrBake *bake,
                                               const VkrBakeDiffuse *args,
                                               const char *scene,
                                               bool8_t photon_radius,
                                               VkrBakeArguments *out) {
  vkr_bake_push(out, "tool");
  vkr_bake_push(out, "diffuse-baker");
  vkr_bake_push(out, "--scene");
  vkr_bake_push(out, scene);
  if (args->has_bounds) {
    vkr_bake_push(out, "--bounds");
    for (uint32_t i = 0u; i < 6u; ++i) {
      vkr_bake_push(out, vkr_bake_printf(bake, "%.9g", args->bounds[i]));
    }
  }
  vkr_bake_push(out, "--spacing");
  vkr_bake_push(out, vkr_bake_printf(bake, "%.9g", args->spacing));
  vkr_bake_push(out, "--levels");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->levels));
  vkr_bake_push(out, "--margin");
  vkr_bake_push(out, vkr_bake_printf(bake, "%.9g", args->margin));
  vkr_bake_push(out, "--face-size");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->face_size));
  vkr_bake_push(out, "--samples");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->samples));
  vkr_bake_push(out, "--max-depth");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->max_depth));
  vkr_bake_push(out, "--seed");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->seed));
  vkr_bake_push(out, "--photons");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->photons));
  if (photon_radius && args->has_photon_radius) {
    vkr_bake_push(out, "--photon-radius");
    vkr_bake_push(out, vkr_bake_printf(bake, "%.9g", args->photon_radius));
  }
  if (args->cpu) {
    vkr_bake_push(out, "--cpu");
  }
  return true_v;
}

/* Path order of pathlib: components compare before separators. */
vkr_internal int vkr_bake_compare_paths(const void *lhs, const void *rhs) {
  const uint8_t *a = *(const uint8_t *const *)lhs;
  const uint8_t *b = *(const uint8_t *const *)rhs;
  for (;; ++a, ++b) {
    const uint32_t x = *a == '/' ? 1u : (*a ? (uint32_t)*a + 1u : 0u);
    const uint32_t y = *b == '/' ? 1u : (*b ? (uint32_t)*b + 1u : 0u);
    if (x != y) {
      return x < y ? -1 : 1;
    }
    if (!*a) {
      return 0;
    }
  }
}

vkr_internal bool8_t vkr_bake_dependency(VkrBake *bake,
                                         const VkrBakeryJson *value,
                                         const char *scene, char *out) {
  if (!value || value->type != VKR_BAKERY_JSON_STRING ||
      !value->string.length) {
    return vkr_bake_fail(bake,
                         "Bake manifest contains an invalid dependency path");
  }
  const char *raw = (const char *)value->string.str;
  char candidates[2][VKR_BAKE_PATH];
  uint32_t candidate_count = 0u;
  if (vkr_bakery_path_is_absolute(raw)) {
    (void)snprintf(candidates[candidate_count++], VKR_BAKE_PATH, "%s", raw);
  } else {
    char scene_directory[VKR_BAKE_PATH];
    vkr_bakery_path_parent(scene_directory, sizeof(scene_directory), scene);
    (void)vkr_bakery_path_join(candidates[candidate_count++], VKR_BAKE_PATH,
                               bake->repo, raw);
    (void)vkr_bakery_path_join(candidates[candidate_count++], VKR_BAKE_PATH,
                               scene_directory, raw);
  }
  char resolved[2][VKR_BAKE_PATH];
  uint32_t resolved_count = 0u;
  for (uint32_t i = 0u; i < candidate_count; ++i) {
    char path[VKR_BAKE_PATH];
    if (!vkr_bake_resolve(candidates[i], path) || !vkr_bakery_is_file(path)) {
      continue;
    }
    bool8_t seen = false_v;
    for (uint32_t j = 0u; j < resolved_count; ++j) {
      seen = seen || strcmp(resolved[j], path) == 0;
    }
    if (!seen) {
      (void)snprintf(resolved[resolved_count++], VKR_BAKE_PATH, "%s", path);
    }
  }
  if (resolved_count != 1u) {
    return vkr_bake_fail(
        bake, "Bake dependency is unavailable or ambiguous: %s", raw);
  }
  (void)snprintf(out, VKR_BAKE_PATH, "%s", resolved[0]);
  return true_v;
}

vkr_internal bool8_t
vkr_bake_valid_atmosphere(const VkrBakeryJson *atmosphere) {
  const VkrBakeryJson *enabled = vkr_bakery_json_get(atmosphere, "enabled");
  int64_t model = 0;
  float64_t deringing = 0.0;
  return atmosphere && atmosphere->type == VKR_BAKERY_JSON_OBJECT && enabled &&
         enabled->type == VKR_BAKERY_JSON_BOOL &&
         vkr_bake_int(vkr_bakery_json_get(atmosphere, "model_version"),
                      &model) &&
         vkr_bake_text(atmosphere, "params_hash") &&
         vkr_bake_number(vkr_bakery_json_get(atmosphere, "sh_deringing"),
                         &deringing) &&
         isfinite(deringing) && deringing >= 0.0;
}

/* What tells one scene bake's inspect manifest from another's: the name
   its messages use and the counts its baker reports. */
typedef struct VkrBakeKind {
  const char *label;
  const char *const *counts;
  uint32_t count_count;
} VkrBakeKind;

vkr_internal const char *const vkr_bake_diffuse_counts[] = {
    "triangles", "materials", "lights", "bricks", "probes", "entries"};
vkr_internal const VkrBakeKind vkr_bake_diffuse_kind = {
    "Diffuse-volume", vkr_bake_diffuse_counts,
    ArrayCount(vkr_bake_diffuse_counts)};

vkr_internal const char *const vkr_bake_lightmap_counts[] = {
    "triangles", "materials", "lights", "lightmap_instances", "pages"};
vkr_internal const VkrBakeKind vkr_bake_lightmap_kind = {
    "Lightmap", vkr_bake_lightmap_counts, ArrayCount(vkr_bake_lightmap_counts)};

/* Validates a baker manifest and records its dependency closure sorted by
   path as {path, bytes, sha256}. */
vkr_internal bool8_t vkr_bake_inspect_manifest(
    VkrBake *bake, const VkrBakeKind *kind, const char *path, const char *scene,
    VkrBakeryJson **out_manifest, VkrBakeryJson **out_records) {
  Arena *arena = bake->arena;
  VkrBakeryJson *manifest =
      vkr_bake_load(bake, path, "Baker did not write a valid inspect manifest");
  VKR_BAKE_TRY(manifest);
  int64_t version = 0;
  if (manifest->type != VKR_BAKERY_JSON_OBJECT ||
      !vkr_bake_int(vkr_bakery_json_get(manifest, "version"), &version) ||
      version != VKR_BAKE_VERSION) {
    return vkr_bake_fail(bake, "Unexpected %s inspect manifest version",
                         kind->label);
  }
  const VkrBakeryJson *dependencies =
      vkr_bakery_json_get(manifest, "dependencies");
  if (!dependencies || dependencies->type != VKR_BAKERY_JSON_ARRAY ||
      !dependencies->count) {
    return vkr_bake_fail(bake, "%s inspect manifest has no dependencies",
                         kind->label);
  }
  for (uint32_t i = 0u; i < kind->count_count; ++i) {
    int64_t value = 0;
    if (!vkr_bake_int(vkr_bakery_json_get(manifest, kind->counts[i]), &value) ||
        value < 0) {
      return vkr_bake_fail(bake, "%s inspect manifest has invalid bake counts",
                           kind->label);
    }
  }
  if (!vkr_bake_valid_atmosphere(vkr_bakery_json_get(manifest, "atmosphere"))) {
    return vkr_bake_fail(bake,
                         "%s inspect manifest has invalid atmosphere "
                         "provenance",
                         kind->label);
  }
  const char *paths[4096];
  uint32_t path_count = 0u;
  bool8_t has_scene = false_v;
  for (const VkrBakeryJson *value = dependencies->first; value;
       value = value->next) {
    char resolved[VKR_BAKE_PATH];
    VKR_BAKE_TRY(vkr_bake_dependency(bake, value, scene, resolved));
    bool8_t seen = false_v;
    for (uint32_t i = 0u; i < path_count && !seen; ++i) {
      seen = strcmp(paths[i], resolved) == 0;
    }
    if (!seen && path_count < ArrayCount(paths)) {
      paths[path_count++] = vkr_bake_printf(bake, "%s", resolved);
    }
    has_scene = has_scene || strcmp(resolved, scene) == 0;
  }
  if (!has_scene) {
    return vkr_bake_fail(bake, "%s inspect manifest does not include its scene",
                         kind->label);
  }
  qsort((void *)paths, path_count, sizeof(paths[0]), vkr_bake_compare_paths);
  VkrBakeryJson *records = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < path_count; ++i) {
    VkrBakeryStat info;
    char digest[72];
    if (!vkr_bakery_stat(paths[i], &info)) {
      return vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'",
                           paths[i]);
    }
    VKR_BAKE_TRY(vkr_bake_digest(bake, paths[i], digest));
    VkrBakeryJson *record = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, record, "path",
                        vkr_bakery_json_cstr(arena, paths[i]));
    vkr_bakery_json_set(arena, record, "bytes",
                        vkr_bakery_json_int(arena, (int64_t)info.size));
    vkr_bakery_json_set(arena, record, "sha256",
                        vkr_bakery_json_cstr(arena, digest));
    vkr_bakery_json_append(records, record);
  }
  *out_manifest = manifest;
  *out_records = records;
  return true_v;
}

vkr_internal bool8_t vkr_bake_protect(VkrBake *bake,
                                      const VkrBakeryJson *records,
                                      const char *const *targets,
                                      uint32_t target_count,
                                      const char *message) {
  for (uint32_t t = 0u; t < target_count; ++t) {
    if (!targets[t]) {
      continue;
    }
    char resolved[VKR_BAKE_PATH];
    (void)vkr_bake_resolve(targets[t], resolved);
    for (const VkrBakeryJson *record = records->first; record;
         record = record->next) {
      const char *path = vkr_bake_text(record, "path");
      if (path && strcmp(path, resolved) == 0) {
        return vkr_bake_fail(bake, "%s", message);
      }
    }
  }
  return true_v;
}

/* Decodes a DVOL file with the runtime's validator and records its shape and
   digest. */
vkr_internal VkrBakeryJson *vkr_bake_verify_dvol(VkrBake *bake,
                                                 const char *path) {
  Arena *arena = bake->arena;
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, 0u, &data, &length)) {
    vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'", path);
    return NULL;
  }
  VkrBakeryJson *result = NULL;
  VkrDiffuseVolume volume = {0};
  char digest[72];
  if (!vkr_diffuse_volume_decode(data, length, arena, &volume)) {
    vkr_bake_fail(bake, "DVOL output is invalid: %s", path);
    goto cleanup;
  }
  if (!vkr_bake_digest(bake, path, digest)) {
    goto cleanup;
  }
  result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "format",
                      vkr_bakery_json_cstr(arena, "DVOL"));
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_DIFFUSE_VOLUME_VERSION));
  vkr_bakery_json_set(arena, result, "bytes",
                      vkr_bakery_json_int(arena, (int64_t)length));
  vkr_bakery_json_set(arena, result, "sha256",
                      vkr_bakery_json_cstr(arena, digest));
  VkrBakeryJson *dimension_list = vkr_bakery_json_array(arena);
  VkrBakeryJson *origin_list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < 3u; ++i) {
    vkr_bakery_json_append(dimension_list,
                           vkr_bakery_json_int(arena, volume.dimensions[i]));
    vkr_bakery_json_append(
        origin_list,
        vkr_bakery_json_float(arena, (float64_t)volume.origin.elements[i]));
  }
  vkr_bakery_json_set(arena, result, "dimensions", dimension_list);
  vkr_bakery_json_set(arena, result, "origin", origin_list);
  vkr_bakery_json_set(arena, result, "spacing",
                      vkr_bakery_json_float(arena, (float64_t)volume.spacing));
  vkr_bakery_json_set(arena, result, "levels",
                      vkr_bakery_json_int(arena, volume.level_count));
  vkr_bakery_json_set(arena, result, "bricks",
                      vkr_bakery_json_int(arena, volume.brick_count));
  vkr_bakery_json_set(arena, result, "probes",
                      vkr_bakery_json_int(arena, volume.probe_count));
  vkr_bakery_json_set(arena, result, "layers",
                      vkr_bakery_json_int(arena, volume.layer_count));
cleanup:
  free(data);
  return result;
}

vkr_internal bool8_t vkr_bake_run_baker(VkrBake *bake,
                                        const VkrBakeArguments *arguments,
                                        const char *log) {
  int32_t code = -1;
  VKR_BAKE_TRY(vkr_bake_run(bake, bake->config->self_path, arguments->items,
                            arguments->count, bake->repo, log, 0u, NULL, 0u,
                            &code));
  bake->baker_code = code;
  if (code == 0) {
    return true_v;
  }
  /* Arguments start with "tool <name>". */
  const char *tool = arguments->count > 1u ? arguments->items[1] : "baker";
  uint8_t *output = NULL;
  uint64_t length = 0u;
  (void)vkr_bakery_read_file(log, 0u, &output, &length);
  const uint64_t tail =
      length > VKR_BAKE_DIAGNOSTIC_BYTES ? VKR_BAKE_DIAGNOSTIC_BYTES : length;
  const char *start = output ? (const char *)output + (length - tail) : "";
  uint64_t size = tail;
  while (size && (*start == ' ' || *start == '\n' || *start == '\r' ||
                  *start == '\t')) {
    start += 1;
    size -= 1u;
  }
  while (size && (start[size - 1u] == ' ' || start[size - 1u] == '\n' ||
                  start[size - 1u] == '\r' || start[size - 1u] == '\t')) {
    size -= 1u;
  }
  if (size) {
    vkr_bake_fail(bake, "%s failed (%d); log: %s; diagnostic: %s%.*s", tool,
                  code, log,
                  length > VKR_BAKE_DIAGNOSTIC_BYTES ? "\xE2\x80\xA6" : "",
                  (int)size, start);
  } else {
    vkr_bake_fail(bake, "%s failed (%d); log: %s", tool, code, log);
  }
  free(output);
  return false_v;
}

vkr_internal bool8_t vkr_bake_write_command(VkrBake *bake, const char *path,
                                            const VkrBakeArguments *arguments) {
  Arena *arena = bake->arena;
  VkrBakeryJson *command = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(command,
                         vkr_bakery_json_cstr(arena, bake->config->self_path));
  for (uint32_t i = 0u; i < arguments->count; ++i) {
    vkr_bakery_json_append(command,
                           vkr_bakery_json_cstr(arena, arguments->items[i]));
  }
  return vkr_bake_write_json(bake, path, command, false_v);
}

/* Runs a baker's recipe with --inspect, writing its manifest and evidence as
   `name` in the job directory. */
vkr_internal bool8_t vkr_bake_run_recipe_inspect(VkrBake *bake,
                                                 const VkrBakeArguments *recipe,
                                                 const char *job,
                                                 const char *name,
                                                 char *out_manifest) {
  (void)snprintf(out_manifest, VKR_BAKE_PATH, "%s/%s.manifest.json", job, name);
  VkrBakeArguments arguments = *recipe;
  vkr_bake_push(&arguments, "--inspect");
  vkr_bake_push(&arguments, "--manifest");
  vkr_bake_push(&arguments, out_manifest);
  VKR_BAKE_TRY(vkr_bake_write_command(
      bake, vkr_bake_printf(bake, "%s/%s.command.json", job, name),
      &arguments));
  return vkr_bake_run_baker(bake, &arguments,
                            vkr_bake_printf(bake, "%s/%s.log", job, name));
}

vkr_internal bool8_t vkr_bake_run_inspect(VkrBake *bake,
                                          const VkrBakeDiffuse *args,
                                          const char *scene, const char *job,
                                          const char *name,
                                          char *out_manifest) {
  VkrBakeArguments recipe = {0};
  VKR_BAKE_TRY(vkr_bake_recipe_arguments(bake, args, scene, false_v, &recipe));
  return vkr_bake_run_recipe_inspect(bake, &recipe, job, name, out_manifest);
}

vkr_internal bool8_t vkr_bake_manifest_spacing(VkrBake *bake,
                                               const VkrBakeryJson *manifest,
                                               const VkrBakeDiffuse *args,
                                               float64_t *out) {
  static const char *const keys[] = {"spacing", "probe_spacing"};
  for (uint32_t k = 0u; k < ArrayCount(keys); ++k) {
    const VkrBakeryJson *candidate = vkr_bakery_json_get(manifest, keys[k]);
    float64_t spacing[3];
    float64_t single = 0.0;
    if (vkr_bake_number(candidate, &single)) {
      spacing[0] = spacing[1] = spacing[2] = single;
    } else if (candidate && candidate->type == VKR_BAKERY_JSON_ARRAY &&
               candidate->count == 3u) {
      bool8_t numbers = true_v;
      uint32_t i = 0u;
      for (const VkrBakeryJson *item = candidate->first; item;
           item = item->next, ++i) {
        numbers = numbers && vkr_bake_number(item, &spacing[i]);
      }
      if (!numbers) {
        continue;
      }
    } else {
      continue;
    }
    if (isfinite(spacing[0]) && isfinite(spacing[1]) && isfinite(spacing[2]) &&
        spacing[0] > 0.0 && spacing[1] > 0.0 && spacing[2] > 0.0) {
      *out = fmin(spacing[0], fmin(spacing[1], spacing[2]));
      return true_v;
    }
  }
  return vkr_bake_fail(bake, "Inspect manifest lacks probe spacing for the "
                             "default photon radius");
}

vkr_internal bool8_t vkr_bake_job_directory(VkrBake *bake,
                                            const char *workspace_root,
                                            const char *prefix,
                                            const char *repo_leaf, char *out) {
  char id[33];
  vkr_bake_hex_id(id);
  if (workspace_root) {
    char root[VKR_BAKE_PATH];
    (void)vkr_bake_resolve(workspace_root, root);
    (void)snprintf(out, VKR_BAKE_PATH, "%s/jobs/%s%s", root, prefix, id);
  } else {
    (void)snprintf(out, VKR_BAKE_PATH, "%s/build/_artifacts/%s/%s", bake->repo,
                   repo_leaf, id);
  }
  if (!vkr_bakery_make_directories(out)) {
    return vkr_bake_fail(bake, "Cannot create %s", out);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_copy(VkrBake *bake, const char *source,
                                   const char *destination) {
  char directory[VKR_BAKE_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), destination);
  if (!vkr_bakery_make_directories(directory) ||
      !vkr_bakery_clone_or_copy(source, destination)) {
    return vkr_bake_fail(bake, "Cannot copy %s to %s", source, destination);
  }
  return true_v;
}

vkr_internal VkrBakeryJson *vkr_bake_recipe_record(VkrBake *bake,
                                                   const VkrBakeDiffuse *args,
                                                   float64_t photon_radius) {
  Arena *arena = bake->arena;
  VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
  if (args->has_bounds) {
    VkrBakeryJson *bounds = vkr_bakery_json_array(arena);
    for (uint32_t i = 0u; i < 6u; ++i) {
      vkr_bakery_json_append(bounds,
                             vkr_bakery_json_float(arena, args->bounds[i]));
    }
    vkr_bakery_json_set(arena, recipe, "bounds", bounds);
  } else {
    vkr_bakery_json_set(arena, recipe, "bounds", vkr_bakery_json_null(arena));
  }
  vkr_bakery_json_set(arena, recipe, "spacing",
                      vkr_bakery_json_float(arena, args->spacing));
  vkr_bakery_json_set(arena, recipe, "levels",
                      vkr_bakery_json_int(arena, args->levels));
  vkr_bakery_json_set(arena, recipe, "margin",
                      vkr_bakery_json_float(arena, args->margin));
  vkr_bakery_json_set(arena, recipe, "face_size",
                      vkr_bakery_json_int(arena, args->face_size));
  vkr_bakery_json_set(arena, recipe, "samples",
                      vkr_bakery_json_int(arena, args->samples));
  vkr_bakery_json_set(arena, recipe, "max_depth",
                      vkr_bakery_json_int(arena, args->max_depth));
  vkr_bakery_json_set(arena, recipe, "seed",
                      vkr_bakery_json_int(arena, args->seed));
  vkr_bakery_json_set(arena, recipe, "photons",
                      vkr_bakery_json_int(arena, args->photons));
  vkr_bakery_json_set(arena, recipe, "cpu",
                      vkr_bakery_json_bool(arena, args->cpu));
  vkr_bakery_json_set(arena, recipe, "photon_radius",
                      vkr_bakery_json_float(arena, photon_radius));
  return recipe;
}

/* A verified bake output and the provenance its sidecar records. */
typedef struct VkrBakePublication {
  const char *scene_source;
  const char *scene;
  VkrBakeryJson *recipe;
  VkrBakeryJson *dependencies;
  VkrBakeryJson *baked;
  /* The verifier's record of the output, with its sha256. */
  VkrBakeryJson *output_record;
  const char *temporary;
  const char *output;
  const char *sidecar;
  const char *bake_manifest;
  const char *manifest_destination;
  const char *job;
} VkrBakePublication;

/* Publishes a verified temporary output under its name, then writes the
   provenance sidecar, the manifest copy and the evidence record, and prints
   the status line. */
vkr_internal bool8_t vkr_bake_publish(VkrBake *bake,
                                      const VkrBakePublication *publication) {
  Arena *arena = bake->arena;
  char self_digest[72];
  VKR_BAKE_TRY(vkr_bake_digest(bake, bake->config->self_path, self_digest));
  VkrBakeryJson *metadata = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, metadata, "version",
                      vkr_bakery_json_int(arena, VKR_BAKE_VERSION));
  VkrBakeryJson *scene_record = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, scene_record, "source",
                      vkr_bakery_json_cstr(arena, publication->scene_source));
  vkr_bakery_json_set(arena, scene_record, "canonical",
                      vkr_bakery_json_cstr(arena, publication->scene));
  vkr_bakery_json_set(arena, metadata, "scene", scene_record);
  vkr_bakery_json_set(arena, metadata, "tool_sha256",
                      vkr_bakery_json_cstr(arena, self_digest));
  vkr_bakery_json_set(arena, metadata, "recipe", publication->recipe);
  vkr_bakery_json_set(arena, metadata, "dependencies",
                      publication->dependencies);
  vkr_bakery_json_set(arena, metadata, "baker_manifest", publication->baked);
  vkr_bakery_json_set(
      arena, metadata, "atmosphere",
      vkr_bakery_json_clone(
          arena, vkr_bakery_json_get(publication->baked, "atmosphere")));
  vkr_bakery_json_set(arena, metadata, "output", publication->output_record);
  const char *sha256 = vkr_bake_text(publication->output_record, "sha256");
  vkr_bakery_json_set(arena, metadata, "output_sha256",
                      vkr_bakery_json_cstr(arena, sha256));
  if (!vkr_bakery_rename(publication->temporary, publication->output, true_v)) {
    return vkr_bake_fail(bake, "Cannot publish %s", publication->output);
  }
  VKR_BAKE_TRY(
      vkr_bake_write_json(bake, publication->sidecar, metadata, true_v) &&
      vkr_bake_copy(bake, publication->bake_manifest,
                    publication->manifest_destination) &&
      vkr_bake_write_json(
          bake, vkr_bake_printf(bake, "%s/bake.json", publication->job),
          metadata, true_v));
  VkrBakeryJson *status = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, status, "status",
                      vkr_bakery_json_cstr(arena, "baked"));
  vkr_bakery_json_set(arena, status, "output",
                      vkr_bakery_json_cstr(arena, publication->output));
  vkr_bakery_json_set(arena, status, "sha256",
                      vkr_bakery_json_cstr(arena, sha256));
  vkr_bakery_json_set(arena, status, "evidence",
                      vkr_bakery_json_cstr(arena, publication->job));
  String8 text = {0};
  if (vkr_bakery_json_write(arena, status, VKR_BAKERY_JSON_COMPACT, &text)) {
    printf("%.*s\n", (int)text.length, (const char *)text.str);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_diffuse(VkrBake *bake, const VkrBakeDiffuse *args,
                                      const char *output, const char *sidecar,
                                      const char *manifest_destination,
                                      bool8_t *out_no_volume,
                                      bool8_t *out_too_large) {
  char scene[VKR_BAKE_PATH];
  char job[VKR_BAKE_PATH];
  char inspect_path[VKR_BAKE_PATH];
  VKR_BAKE_TRY(vkr_bake_validate_recipe(bake, args));
  VKR_BAKE_TRY(vkr_bake_existing_file(bake, args->scene, "--scene", scene));
  VKR_BAKE_TRY(vkr_bake_job_directory(bake, args->workspace_root, "diffuse_",
                                      "diffuse_volume_bake", job));
  if (!vkr_bake_run_inspect(bake, args, scene, job, "inspect", inspect_path)) {
    /* Bounds too large for the brick budget at this spacing leave the scene
       without a volume, as a scene without geometry does. */
    *out_too_large = bake->baker_code == VKR_DIFFUSE_VOLUME_OVER_BUDGET_EXIT;
    return false_v;
  }
  VkrBakeryJson *inspect = NULL;
  VkrBakeryJson *dependencies = NULL;
  VKR_BAKE_TRY(vkr_bake_inspect_manifest(bake, &vkr_bake_diffuse_kind,
                                         inspect_path, scene, &inspect,
                                         &dependencies));
  char resolved_targets[3][VKR_BAKE_PATH];
  const char *targets[] = {output, sidecar, manifest_destination};
  for (uint32_t i = 0u; i < 3u; ++i) {
    (void)vkr_bake_resolve(targets[i], resolved_targets[i]);
    for (uint32_t j = 0u; j < i; ++j) {
      if (strcmp(resolved_targets[i], resolved_targets[j]) == 0) {
        return vkr_bake_fail(bake, "Output, metadata, and manifest must have "
                                   "distinct paths");
      }
    }
  }
  VKR_BAKE_TRY(vkr_bake_protect(
      bake, dependencies, targets, ArrayCount(targets),
      "Output, metadata, or manifest would overwrite a bake source asset"));
  /* A scene without geometry places no brick; it keeps environment and
     reflection-probe diffuse lighting. */
  int64_t bricks = 0;
  (void)vkr_bake_int(vkr_bakery_json_get(inspect, "bricks"), &bricks);
  if (!bricks) {
    *out_no_volume = true_v;
    return vkr_bake_fail(bake,
                         "the scene has no geometry to place probes near, so "
                         "it keeps environment and reflection-probe diffuse "
                         "lighting. Inspection: %s",
                         job);
  }
  char output_directory[VKR_BAKE_PATH];
  vkr_bakery_path_parent(output_directory, sizeof(output_directory), output);
  if (!vkr_bakery_make_directories(output_directory)) {
    return vkr_bake_fail(bake, "Cannot create %s", output_directory);
  }
  char id[33];
  vkr_bake_hex_id(id);
  const char *temporary = vkr_bake_printf(bake, "%s.%.8s.vkdv", output, id);
  const char *bake_manifest =
      vkr_bake_printf(bake, "%s/bake.manifest.json", job);
  VkrBakeArguments arguments = {0};
  VKR_BAKE_TRY(
      vkr_bake_recipe_arguments(bake, args, scene, true_v, &arguments));
  vkr_bake_push(&arguments, "--output");
  vkr_bake_push(&arguments, temporary);
  vkr_bake_push(&arguments, "--manifest");
  vkr_bake_push(&arguments, bake_manifest);
  VKR_BAKE_TRY(vkr_bake_write_command(
      bake, vkr_bake_printf(bake, "%s/bake.command.json", job), &arguments));
  bool8_t ok = vkr_bake_run_baker(bake, &arguments,
                                  vkr_bake_printf(bake, "%s/bake.log", job));
  VkrBakeryJson *baked = NULL;
  VkrBakeryJson *after = NULL;
  VkrBakeryJson *dvol = NULL;
  ok = ok && vkr_bake_inspect_manifest(bake, &vkr_bake_diffuse_kind,
                                       bake_manifest, scene, &baked, &after);
  if (ok && !vkr_bakery_json_equal(dependencies, after)) {
    ok = vkr_bake_fail(bake, "Bake source closure changed while the volume was "
                             "prepared");
  }
  ok = ok && (dvol = vkr_bake_verify_dvol(bake, temporary)) != NULL;
  char final_inspect[VKR_BAKE_PATH];
  VkrBakeryJson *final_manifest = NULL;
  VkrBakeryJson *final_dependencies = NULL;
  ok = ok &&
       vkr_bake_run_inspect(bake, args, scene, job, "inspect_after",
                            final_inspect) &&
       vkr_bake_inspect_manifest(bake, &vkr_bake_diffuse_kind, final_inspect,
                                 scene, &final_manifest, &final_dependencies);
  if (ok && !vkr_bakery_json_equal(dependencies, final_dependencies)) {
    ok = vkr_bake_fail(bake, "Bake source closure changed before publication");
  }
  float64_t photon_radius = args->photon_radius;
  if (ok && !args->has_photon_radius) {
    float64_t spacing = 0.0;
    ok = vkr_bake_manifest_spacing(bake, baked, args, &spacing);
    photon_radius = spacing * 0.25;
  }
  if (ok) {
    const VkrBakePublication publication = {
        .scene_source = args->scene,
        .scene = scene,
        .recipe = vkr_bake_recipe_record(bake, args, photon_radius),
        .dependencies = dependencies,
        .baked = baked,
        .output_record = dvol,
        .temporary = temporary,
        .output = output,
        .sidecar = sidecar,
        .bake_manifest = bake_manifest,
        .manifest_destination = manifest_destination,
        .job = job,
    };
    ok = vkr_bake_publish(bake, &publication);
  }
  (void)vkr_bakery_remove_file(temporary);
  return ok;
}

vkr_internal bool8_t
vkr_bake_diffuse_inspect(VkrBake *bake, const VkrBakeDiffuse *args,
                         const char *manifest_destination) {
  Arena *arena = bake->arena;
  char scene[VKR_BAKE_PATH];
  char job[VKR_BAKE_PATH];
  char temporary[VKR_BAKE_PATH];
  VKR_BAKE_TRY(vkr_bake_validate_recipe(bake, args));
  VKR_BAKE_TRY(vkr_bake_existing_file(bake, args->scene, "--scene", scene));
  VKR_BAKE_TRY(vkr_bake_job_directory(bake, args->workspace_root, "diffuse_",
                                      "diffuse_volume_bake", job));
  VKR_BAKE_TRY(
      vkr_bake_run_inspect(bake, args, scene, job, "inspect", temporary));
  VkrBakeryJson *manifest = NULL;
  VkrBakeryJson *dependencies = NULL;
  VKR_BAKE_TRY(vkr_bake_inspect_manifest(bake, &vkr_bake_diffuse_kind,
                                         temporary, scene, &manifest,
                                         &dependencies));
  const char *targets[] = {manifest_destination};
  VKR_BAKE_TRY(vkr_bake_protect(
      bake, dependencies, targets, 1u,
      "Output, metadata, or manifest would overwrite a bake source asset"));
  VKR_BAKE_TRY(vkr_bake_copy(bake, temporary, manifest_destination));
  char self_digest[72];
  VKR_BAKE_TRY(vkr_bake_digest(bake, bake->config->self_path, self_digest));
  VkrBakeryJson *record = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, record, "version",
                      vkr_bakery_json_int(arena, VKR_BAKE_VERSION));
  vkr_bakery_json_set(arena, record, "manifest", manifest);
  vkr_bakery_json_set(arena, record, "dependencies", dependencies);
  vkr_bakery_json_set(arena, record, "tool_sha256",
                      vkr_bakery_json_cstr(arena, self_digest));
  VKR_BAKE_TRY(vkr_bake_write_json(
      bake, vkr_bake_printf(bake, "%s/inspect.json", job), record, true_v));
  printf("{\"status\":\"inspected\",\"manifest\":\"%s\",\"dependencies\":%u,"
         "\"evidence\":\"%s\"}\n",
         manifest_destination, dependencies->count, job);
  return true_v;
}

vkr_internal bool8_t vkr_bake_legacy_atmosphere(VkrBake *bake,
                                                const VkrBakeryJson *metadata,
                                                const VkrBakeryJson *records) {
  /* Old sidecars cannot establish transport for an enabled atmosphere. */
  const char *canonical =
      vkr_bake_text(vkr_bakery_json_get(metadata, "scene"), "canonical");
  char scene[VKR_BAKE_PATH];
  if (!canonical || !vkr_bake_resolve(canonical, scene) ||
      !vkr_bakery_is_file(scene)) {
    return true_v;
  }
  bool8_t listed = false_v;
  for (const VkrBakeryJson *record = records->first; record;
       record = record->next) {
    char path[VKR_BAKE_PATH];
    const char *value = vkr_bake_text(record, "path");
    listed = listed || (value && vkr_bake_resolve(value, path) &&
                        strcmp(path, scene) == 0);
  }
  if (!listed) {
    return true_v;
  }
  const bool8_t failed_before = bake->failed;
  VkrBakeryJson *root = vkr_bake_load(bake, scene, "scene");
  if (!root) {
    bake->failed = failed_before;
    return true_v;
  }
  const VkrBakeryJson *atmosphere = vkr_bakery_json_get(root, "atmosphere");
  const VkrBakeryJson *enabled = vkr_bakery_json_get(atmosphere, "enabled");
  return atmosphere && atmosphere->type == VKR_BAKERY_JSON_OBJECT &&
         !(enabled && enabled->type == VKR_BAKERY_JSON_BOOL &&
           !enabled->boolean);
}

typedef VkrBakeryJson *(*VkrBakeVerify)(VkrBake *bake, const char *path);

/* Whether a published output still matches its sidecar: the output digest,
   every dependency's size and digest, and the atmosphere provenance. */
vkr_internal bool8_t vkr_bake_current(VkrBake *bake, const char *output,
                                      const char *sidecar,
                                      VkrBakeVerify verify) {
  VkrBakeryJson *metadata = vkr_bake_load(bake, sidecar, "metadata");
  int64_t version = 0;
  if (!metadata ||
      !vkr_bake_int(vkr_bakery_json_get(metadata, "version"), &version) ||
      version != VKR_BAKE_VERSION) {
    return false_v;
  }
  VkrBakeryJson *verified = verify(bake, output);
  if (!verified ||
      !vkr_bakery_json_equal(vkr_bakery_json_get(verified, "sha256"),
                             vkr_bakery_json_get(metadata, "output_sha256"))) {
    return false_v;
  }
  const VkrBakeryJson *dependencies =
      vkr_bakery_json_get(metadata, "dependencies");
  if (!dependencies || dependencies->type != VKR_BAKERY_JSON_ARRAY ||
      !dependencies->count) {
    return false_v;
  }
  for (const VkrBakeryJson *record = dependencies->first; record;
       record = record->next) {
    const char *value = vkr_bake_text(record, "path");
    char path[VKR_BAKE_PATH];
    char digest[72];
    VkrBakeryStat info;
    int64_t bytes = -1;
    if (!value || !vkr_bake_resolve(value, path) || !vkr_bakery_is_file(path) ||
        !vkr_bakery_stat(path, &info) ||
        !vkr_bake_int(vkr_bakery_json_get(record, "bytes"), &bytes) ||
        bytes != (int64_t)info.size || !vkr_bake_digest(bake, path, digest) ||
        !vkr_bakery_json_is_string(vkr_bakery_json_get(record, "sha256"),
                                   digest)) {
      return false_v;
    }
  }
  const VkrBakeryJson *atmosphere = vkr_bakery_json_get(metadata, "atmosphere");
  if (!atmosphere || atmosphere->type == VKR_BAKERY_JSON_NULL) {
    if (vkr_bake_legacy_atmosphere(bake, metadata, dependencies)) {
      return false_v;
    }
  } else if (!vkr_bake_valid_atmosphere(atmosphere)) {
    return false_v;
  }
  const char *targets[] = {output, sidecar};
  return vkr_bake_protect(bake, dependencies, targets, 2u, "overwrite");
}

vkr_internal bool8_t vkr_bake_parse_numbers(char **argv, int argc, int *index,
                                            uint32_t count, float64_t *out) {
  for (uint32_t i = 0u; i < count; ++i) {
    if (*index + 1 >= argc) {
      return false_v;
    }
    char *end = NULL;
    *index += 1;
    out[i] = strtod(argv[*index], &end);
    if (!end || *end) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_parse_integer(char **argv, int argc, int *index,
                                            int64_t *out) {
  if (*index + 1 >= argc) {
    return false_v;
  }
  char *end = NULL;
  *index += 1;
  *out = strtoll(argv[*index], &end, 10);
  return end && !*end;
}

vkr_internal const char *vkr_bake_value(char **argv, int argc, int *index) {
  if (*index + 1 >= argc) {
    return NULL;
  }
  *index += 1;
  return argv[*index];
}

vkr_internal int vkr_bake_diffuse_main(VkrBake *bake, int argc, char **argv) {
  VkrBakeDiffuse args = {.spacing = 1.0,
                         .levels = 3,
                         .margin = 1.0,
                         .face_size = 8,
                         .samples = 4,
                         .max_depth = 12,
                         .seed = 1,
                         .photons = 1000000};
  for (int i = 1; i < argc; ++i) {
    const char *flag = argv[i];
    bool8_t ok = true_v;
    if (!strcmp(flag, "--scene")) {
      ok = (args.scene = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--workspace-root")) {
      ok = (args.workspace_root = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--output")) {
      ok = (args.output = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--manifest")) {
      ok = (args.manifest = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--baker")) {
      ok = vkr_bake_value(argv, argc, &i) !=
           NULL; /* The baker is this binary. */
    } else if (!strcmp(flag, "--inspect")) {
      args.inspect = true_v;
    } else if (!strcmp(flag, "--check")) {
      args.check = true_v;
    } else if (!strcmp(flag, "--bounds")) {
      ok = args.has_bounds =
          vkr_bake_parse_numbers(argv, argc, &i, 6u, args.bounds);
    } else if (!strcmp(flag, "--spacing")) {
      ok = vkr_bake_parse_numbers(argv, argc, &i, 1u, &args.spacing);
    } else if (!strcmp(flag, "--levels")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.levels);
    } else if (!strcmp(flag, "--margin")) {
      ok = vkr_bake_parse_numbers(argv, argc, &i, 1u, &args.margin);
    } else if (!strcmp(flag, "--face-size")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.face_size);
    } else if (!strcmp(flag, "--samples")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.samples);
    } else if (!strcmp(flag, "--max-depth")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.max_depth);
    } else if (!strcmp(flag, "--seed")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.seed);
    } else if (!strcmp(flag, "--photons")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.photons);
    } else if (!strcmp(flag, "--photon-radius")) {
      ok = args.has_photon_radius =
          vkr_bake_parse_numbers(argv, argc, &i, 1u, &args.photon_radius);
    } else if (!strcmp(flag, "--cpu")) {
      args.cpu = true_v;
    } else {
      ok = false_v;
    }
    if (!ok) {
      fprintf(stderr, "bake diffuse: invalid argument %s\n", flag);
      return 2;
    }
  }
  if (args.check) {
    if (!args.output) {
      fprintf(stderr, "bake diffuse: --output is required with --check\n");
      return 2;
    }
  } else if (!args.scene) {
    fprintf(stderr, "bake diffuse: --scene is required\n");
    return 2;
  } else if (args.inspect && args.output) {
    fprintf(stderr, "bake diffuse: --inspect does not write --output\n");
    return 2;
  } else if (args.inspect && !args.manifest) {
    fprintf(stderr, "bake diffuse: --manifest is required with --inspect\n");
    return 2;
  } else if (!args.inspect && !args.output) {
    fprintf(stderr, "bake diffuse: --output is required\n");
    return 2;
  }
  char output[VKR_BAKE_PATH] = {0};
  char sidecar[VKR_BAKE_PATH] = {0};
  if (args.output) {
    char extension[16];
    (void)vkr_bake_resolve(args.output, output);
    vkr_bakery_path_extension(output, extension, sizeof(extension));
    const uint64_t length = strlen(output);
    if (length < 5u || strcmp(output + length - 5u, ".vkdv") != 0) {
      fprintf(stderr, "Diffuse-volume bake failed: --output must name a .vkdv "
                      "file\n");
      return 1;
    }
    (void)snprintf(sidecar, sizeof(sidecar), "%s.bake.json", output);
  }
  if (args.check) {
    const bool8_t current =
        vkr_bake_current(bake, output, sidecar, vkr_bake_verify_dvol);
    printf("%s\n", current ? "current" : "stale");
    return current ? 0 : 1;
  }
  char manifest[VKR_BAKE_PATH];
  if (args.manifest) {
    (void)vkr_bake_resolve(args.manifest, manifest);
  } else {
    char joined[VKR_BAKE_PATH];
    (void)snprintf(joined, sizeof(joined), "%s.manifest.json", output);
    (void)vkr_bake_resolve(joined, manifest);
  }
  bool8_t no_volume = false_v;
  bool8_t too_large = false_v;
  const bool8_t ok = args.inspect
                         ? vkr_bake_diffuse_inspect(bake, &args, manifest)
                         : vkr_bake_diffuse(bake, &args, output, sidecar,
                                            manifest, &no_volume, &too_large);
  if (ok) {
    return 0;
  }
  if (no_volume || too_large) {
    fprintf(stderr, "Diffuse-volume bake skipped: %s\n", bake->error);
    return no_volume ? VKR_BAKERY_BAKE_NO_VOLUME
                     : VKR_BAKERY_BAKE_VOLUME_TOO_LARGE;
  }
  fprintf(stderr, "Diffuse-volume bake failed: %s\n", bake->error);
  return 1;
}

// =============================================================================
// Lightmaps (ADR-087)
// =============================================================================

typedef struct VkrBakeLightmap {
  const char *scene;
  const char *workspace_root;
  const char *output;
  const char *manifest;
  bool8_t inspect;
  bool8_t check;
  int64_t samples;
  int64_t max_depth;
  int64_t seed;
  int64_t page_size;
  float64_t texels_per_unit;
  /* The tiled class's lamp-direct pages' density (ADR-108); zero bakes lamp
     direct light into the lamp groups' irradiance. */
  float64_t direct_texels_per_unit;
  /* Indirect-light denoising (ADR-088): on or off, its a-trous passes, and
     the largest luminance one indirect sample keeps (zero keeps all). */
  int64_t denoise;
  int64_t denoise_iterations;
  float64_t indirect_clamp;
} VkrBakeLightmap;

vkr_internal bool8_t vkr_bake_lightmap_validate(VkrBake *bake,
                                                const VkrBakeLightmap *args) {
  if (args->samples < 1 || args->samples > 65536 || args->max_depth < 1 ||
      args->max_depth > 64) {
    return vkr_bake_fail(bake, "--samples must be 1..65536 and --max-depth "
                               "must be 1..64");
  }
  if (args->seed < 0 || args->seed > 0xffffffffll) {
    return vkr_bake_fail(bake, "--seed must be an unsigned 32-bit value");
  }
  if (args->page_size < 64 ||
      args->page_size > VKR_LIGHTMAP_SET_MAX_PAGE_SIZE ||
      args->page_size % 4 != 0) {
    return vkr_bake_fail(bake, "--page-size must be a multiple of 4 from 64 "
                               "through 8192");
  }
  if (!isfinite(args->texels_per_unit) || args->texels_per_unit <= 0.0 ||
      args->texels_per_unit > 1024.0) {
    return vkr_bake_fail(bake, "--texels-per-unit must be finite and in "
                               "(0, 1024]");
  }
  if (!isfinite(args->direct_texels_per_unit) ||
      args->direct_texels_per_unit < 0.0 ||
      args->direct_texels_per_unit > 1024.0) {
    return vkr_bake_fail(bake, "--direct-texels-per-unit must be finite and "
                               "in [0, 1024]");
  }
  if (args->denoise < 0 || args->denoise > 1 || args->denoise_iterations < 0 ||
      args->denoise_iterations > 8) {
    return vkr_bake_fail(bake, "--denoise must be 0 or 1 and "
                               "--denoise-iterations 0..8");
  }
  if (!isfinite(args->indirect_clamp) || args->indirect_clamp < 0.0) {
    return vkr_bake_fail(bake, "--indirect-clamp must be finite and not "
                               "negative");
  }
  return true_v;
}

vkr_internal void vkr_bake_lightmap_arguments(VkrBake *bake,
                                              const VkrBakeLightmap *args,
                                              const char *scene,
                                              VkrBakeArguments *out) {
  vkr_bake_push(out, "tool");
  vkr_bake_push(out, "lightmap-baker");
  vkr_bake_push(out, "--scene");
  vkr_bake_push(out, scene);
  vkr_bake_push(out, "--samples");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->samples));
  vkr_bake_push(out, "--max-depth");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->max_depth));
  vkr_bake_push(out, "--seed");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->seed));
  vkr_bake_push(out, "--page-size");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->page_size));
  vkr_bake_push(out, "--texels-per-unit");
  vkr_bake_push(out, vkr_bake_printf(bake, "%.9g", args->texels_per_unit));
  vkr_bake_push(out, "--direct-texels-per-unit");
  vkr_bake_push(out,
                vkr_bake_printf(bake, "%.9g", args->direct_texels_per_unit));
  vkr_bake_push(out, "--denoise");
  vkr_bake_push(out, vkr_bake_printf(bake, "%lld", (long long)args->denoise));
  vkr_bake_push(out, "--denoise-iterations");
  vkr_bake_push(
      out, vkr_bake_printf(bake, "%lld", (long long)args->denoise_iterations));
  vkr_bake_push(out, "--indirect-clamp");
  vkr_bake_push(out, vkr_bake_printf(bake, "%.9g", args->indirect_clamp));
}

vkr_internal VkrBakeryJson *
vkr_bake_lightmap_recipe(VkrBake *bake, const VkrBakeLightmap *args) {
  Arena *arena = bake->arena;
  VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, recipe, "samples",
                      vkr_bakery_json_int(arena, args->samples));
  vkr_bakery_json_set(arena, recipe, "max_depth",
                      vkr_bakery_json_int(arena, args->max_depth));
  vkr_bakery_json_set(arena, recipe, "seed",
                      vkr_bakery_json_int(arena, args->seed));
  vkr_bakery_json_set(arena, recipe, "page_size",
                      vkr_bakery_json_int(arena, args->page_size));
  vkr_bakery_json_set(arena, recipe, "texels_per_unit",
                      vkr_bakery_json_float(arena, args->texels_per_unit));
  vkr_bakery_json_set(
      arena, recipe, "direct_texels_per_unit",
      vkr_bakery_json_float(arena, args->direct_texels_per_unit));
  vkr_bakery_json_set(arena, recipe, "denoise",
                      vkr_bakery_json_int(arena, args->denoise));
  vkr_bakery_json_set(arena, recipe, "denoise_iterations",
                      vkr_bakery_json_int(arena, args->denoise_iterations));
  vkr_bakery_json_set(arena, recipe, "indirect_clamp",
                      vkr_bakery_json_float(arena, args->indirect_clamp));
  return recipe;
}

/* Decodes a VKLM file with the runtime's validator and records its shape and
   digest. */
vkr_internal VkrBakeryJson *vkr_bake_verify_vklm(VkrBake *bake,
                                                 const char *path) {
  Arena *arena = bake->arena;
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, 0u, &data, &length)) {
    vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'", path);
    return NULL;
  }
  VkrBakeryJson *result = NULL;
  VkrLightmapSet set = {0};
  char digest[72];
  if (!vkr_lightmap_set_decode(data, length, arena, &set)) {
    vkr_bake_fail(bake, "VKLM output is invalid: %s", path);
    goto cleanup;
  }
  if (!vkr_bake_digest(bake, path, digest)) {
    goto cleanup;
  }
  result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "format",
                      vkr_bakery_json_cstr(arena, "VKLM"));
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_LIGHTMAP_SET_VERSION));
  vkr_bakery_json_set(arena, result, "bytes",
                      vkr_bakery_json_int(arena, (int64_t)length));
  vkr_bakery_json_set(arena, result, "sha256",
                      vkr_bakery_json_cstr(arena, digest));
  vkr_bakery_json_set(arena, result, "page_size",
                      vkr_bakery_json_int(arena, set.page_size));
  vkr_bakery_json_set(arena, result, "pages",
                      vkr_bakery_json_int(arena, set.page_count));
  vkr_bakery_json_set(arena, result, "layers",
                      vkr_bakery_json_int(arena, set.layer_count));
  vkr_bakery_json_set(arena, result, "planes",
                      vkr_bakery_json_int(arena, set.plane_count));
  vkr_bakery_json_set(arena, result, "instances",
                      vkr_bakery_json_int(arena, set.instance_count));
cleanup:
  free(data);
  return result;
}

vkr_internal bool8_t vkr_bake_lightmap(VkrBake *bake,
                                       const VkrBakeLightmap *args,
                                       const char *output, const char *sidecar,
                                       const char *manifest_destination,
                                       bool8_t *out_nothing) {
  char scene[VKR_BAKE_PATH];
  char job[VKR_BAKE_PATH];
  char inspect_path[VKR_BAKE_PATH];
  VKR_BAKE_TRY(vkr_bake_lightmap_validate(bake, args));
  VKR_BAKE_TRY(vkr_bake_existing_file(bake, args->scene, "--scene", scene));
  VKR_BAKE_TRY(vkr_bake_job_directory(bake, args->workspace_root, "lightmap_",
                                      "lightmap_bake", job));
  VkrBakeArguments recipe = {0};
  vkr_bake_lightmap_arguments(bake, args, scene, &recipe);
  VKR_BAKE_TRY(
      vkr_bake_run_recipe_inspect(bake, &recipe, job, "inspect", inspect_path));
  VkrBakeryJson *inspect = NULL;
  VkrBakeryJson *dependencies = NULL;
  VKR_BAKE_TRY(vkr_bake_inspect_manifest(bake, &vkr_bake_lightmap_kind,
                                         inspect_path, scene, &inspect,
                                         &dependencies));
  const char *targets[] = {output, sidecar, manifest_destination};
  char resolved_targets[3][VKR_BAKE_PATH];
  for (uint32_t i = 0u; i < 3u; ++i) {
    (void)vkr_bake_resolve(targets[i], resolved_targets[i]);
    for (uint32_t j = 0u; j < i; ++j) {
      if (strcmp(resolved_targets[i], resolved_targets[j]) == 0) {
        return vkr_bake_fail(bake, "Output, metadata, and manifest must have "
                                   "distinct paths");
      }
    }
  }
  VKR_BAKE_TRY(vkr_bake_protect(
      bake, dependencies, targets, ArrayCount(targets),
      "Output, metadata, or manifest would overwrite a bake source asset"));
  int64_t instances = 0;
  (void)vkr_bake_int(vkr_bakery_json_get(inspect, "lightmap_instances"),
                     &instances);
  if (instances == 0) {
    *out_nothing = true_v;
    return vkr_bake_fail(bake,
                         "no scene model carries lightmap UVs; cook meshes "
                         "with lightmap_texels_per_unit. Inspection: %s",
                         job);
  }
  /* A desktop host bakes only lamp groups (ADR-104), so a scene without
     static lamps has nothing to bake there. */
  int64_t baked_layers = 1;
  (void)vkr_bake_int(vkr_bakery_json_get(inspect, "baked_layers"),
                     &baked_layers);
  if (baked_layers == 0) {
    *out_nothing = true_v;
    return vkr_bake_fail(bake,
                         "the scene has no static lamps or emission, and "
                         "this host's desktop pipeline bakes only their "
                         "light. "
                         "Inspection: %s",
                         job);
  }

  char output_directory[VKR_BAKE_PATH];
  vkr_bakery_path_parent(output_directory, sizeof(output_directory), output);
  if (!vkr_bakery_make_directories(output_directory)) {
    return vkr_bake_fail(bake, "Cannot create %s", output_directory);
  }
  char id[33];
  vkr_bake_hex_id(id);
  const char *temporary = vkr_bake_printf(bake, "%s.%.8s.vklm", output, id);
  const char *bake_manifest =
      vkr_bake_printf(bake, "%s/bake.manifest.json", job);
  VkrBakeArguments arguments = recipe;
  vkr_bake_push(&arguments, "--output");
  vkr_bake_push(&arguments, temporary);
  vkr_bake_push(&arguments, "--manifest");
  vkr_bake_push(&arguments, bake_manifest);
  VKR_BAKE_TRY(vkr_bake_write_command(
      bake, vkr_bake_printf(bake, "%s/bake.command.json", job), &arguments));
  bool8_t ok = vkr_bake_run_baker(bake, &arguments,
                                  vkr_bake_printf(bake, "%s/bake.log", job));
  VkrBakeryJson *baked = NULL;
  VkrBakeryJson *after = NULL;
  VkrBakeryJson *vklm = NULL;
  ok = ok && vkr_bake_inspect_manifest(bake, &vkr_bake_lightmap_kind,
                                       bake_manifest, scene, &baked, &after);
  if (ok && !vkr_bakery_json_equal(dependencies, after)) {
    ok = vkr_bake_fail(bake, "Bake source closure changed while the lightmaps "
                             "were prepared");
  }
  ok = ok && (vklm = vkr_bake_verify_vklm(bake, temporary)) != NULL;
  char final_inspect[VKR_BAKE_PATH];
  VkrBakeryJson *final_manifest = NULL;
  VkrBakeryJson *final_dependencies = NULL;
  ok = ok &&
       vkr_bake_run_recipe_inspect(bake, &recipe, job, "inspect_after",
                                   final_inspect) &&
       vkr_bake_inspect_manifest(bake, &vkr_bake_lightmap_kind, final_inspect,
                                 scene, &final_manifest, &final_dependencies);
  if (ok && !vkr_bakery_json_equal(dependencies, final_dependencies)) {
    ok = vkr_bake_fail(bake, "Bake source closure changed before publication");
  }
  if (ok) {
    const VkrBakePublication publication = {
        .scene_source = args->scene,
        .scene = scene,
        .recipe = vkr_bake_lightmap_recipe(bake, args),
        .dependencies = dependencies,
        .baked = baked,
        .output_record = vklm,
        .temporary = temporary,
        .output = output,
        .sidecar = sidecar,
        .bake_manifest = bake_manifest,
        .manifest_destination = manifest_destination,
        .job = job,
    };
    ok = vkr_bake_publish(bake, &publication);
  }
  (void)vkr_bakery_remove_file(temporary);
  return ok;
}

vkr_internal bool8_t
vkr_bake_lightmap_inspect(VkrBake *bake, const VkrBakeLightmap *args,
                          const char *manifest_destination) {
  char scene[VKR_BAKE_PATH];
  char job[VKR_BAKE_PATH];
  char temporary[VKR_BAKE_PATH];
  VKR_BAKE_TRY(vkr_bake_lightmap_validate(bake, args));
  VKR_BAKE_TRY(vkr_bake_existing_file(bake, args->scene, "--scene", scene));
  VKR_BAKE_TRY(vkr_bake_job_directory(bake, args->workspace_root, "lightmap_",
                                      "lightmap_bake", job));
  VkrBakeArguments recipe = {0};
  vkr_bake_lightmap_arguments(bake, args, scene, &recipe);
  VKR_BAKE_TRY(
      vkr_bake_run_recipe_inspect(bake, &recipe, job, "inspect", temporary));
  VkrBakeryJson *manifest = NULL;
  VkrBakeryJson *dependencies = NULL;
  VKR_BAKE_TRY(vkr_bake_inspect_manifest(bake, &vkr_bake_lightmap_kind,
                                         temporary, scene, &manifest,
                                         &dependencies));
  const char *targets[] = {manifest_destination};
  VKR_BAKE_TRY(vkr_bake_protect(
      bake, dependencies, targets, 1u,
      "Output, metadata, or manifest would overwrite a bake source asset"));
  VKR_BAKE_TRY(vkr_bake_copy(bake, temporary, manifest_destination));
  printf("{\"status\":\"inspected\",\"manifest\":\"%s\",\"dependencies\":%u,"
         "\"evidence\":\"%s\"}\n",
         manifest_destination, dependencies->count, job);
  return true_v;
}

vkr_internal int vkr_bake_lightmap_main(VkrBake *bake, int argc, char **argv) {
  VkrBakeLightmap args = {.samples = 64,
                          .max_depth = 4,
                          .seed = 1,
                          .page_size = 4096,
                          .texels_per_unit = 8.0,
                          .direct_texels_per_unit = 16.0,
                          .denoise = 1,
                          .denoise_iterations = 4,
                          .indirect_clamp = 0.0};
  for (int i = 1; i < argc; ++i) {
    const char *flag = argv[i];
    bool8_t ok = true_v;
    if (!strcmp(flag, "--scene")) {
      ok = (args.scene = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--workspace-root")) {
      ok = (args.workspace_root = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--output")) {
      ok = (args.output = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--manifest")) {
      ok = (args.manifest = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--inspect")) {
      args.inspect = true_v;
    } else if (!strcmp(flag, "--check")) {
      args.check = true_v;
    } else if (!strcmp(flag, "--samples")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.samples);
    } else if (!strcmp(flag, "--max-depth")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.max_depth);
    } else if (!strcmp(flag, "--seed")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.seed);
    } else if (!strcmp(flag, "--page-size")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.page_size);
    } else if (!strcmp(flag, "--texels-per-unit")) {
      ok = vkr_bake_parse_numbers(argv, argc, &i, 1u, &args.texels_per_unit);
    } else if (!strcmp(flag, "--direct-texels-per-unit")) {
      ok = vkr_bake_parse_numbers(argv, argc, &i, 1u,
                                  &args.direct_texels_per_unit);
    } else if (!strcmp(flag, "--denoise")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.denoise);
    } else if (!strcmp(flag, "--denoise-iterations")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.denoise_iterations);
    } else if (!strcmp(flag, "--indirect-clamp")) {
      ok = vkr_bake_parse_numbers(argv, argc, &i, 1u, &args.indirect_clamp);
    } else {
      ok = false_v;
    }
    if (!ok) {
      fprintf(stderr, "bake lightmap: invalid argument %s\n", flag);
      return 2;
    }
  }
  if (args.check) {
    if (!args.output) {
      fprintf(stderr, "bake lightmap: --output is required with --check\n");
      return 2;
    }
  } else if (!args.scene) {
    fprintf(stderr, "bake lightmap: --scene is required\n");
    return 2;
  } else if (args.inspect && args.output) {
    fprintf(stderr, "bake lightmap: --inspect does not write --output\n");
    return 2;
  } else if (args.inspect && !args.manifest) {
    fprintf(stderr, "bake lightmap: --manifest is required with --inspect\n");
    return 2;
  } else if (!args.inspect && !args.output) {
    fprintf(stderr, "bake lightmap: --output is required\n");
    return 2;
  }
  char output[VKR_BAKE_PATH] = {0};
  char sidecar[VKR_BAKE_PATH] = {0};
  if (args.output) {
    (void)vkr_bake_resolve(args.output, output);
    const uint64_t length = strlen(output);
    if (length < 5u || strcmp(output + length - 5u, ".vklm") != 0) {
      fprintf(stderr, "Lightmap bake failed: --output must name a .vklm "
                      "file\n");
      return 1;
    }
    (void)snprintf(sidecar, sizeof(sidecar), "%s.bake.json", output);
  }
  if (args.check) {
    const bool8_t current =
        vkr_bake_current(bake, output, sidecar, vkr_bake_verify_vklm);
    printf("%s\n", current ? "current" : "stale");
    return current ? 0 : 1;
  }
  char manifest[VKR_BAKE_PATH];
  if (args.manifest) {
    (void)vkr_bake_resolve(args.manifest, manifest);
  } else {
    char joined[VKR_BAKE_PATH];
    (void)snprintf(joined, sizeof(joined), "%s.manifest.json", output);
    (void)vkr_bake_resolve(joined, manifest);
  }
  bool8_t nothing = false_v;
  const bool8_t ok =
      args.inspect
          ? vkr_bake_lightmap_inspect(bake, &args, manifest)
          : vkr_bake_lightmap(bake, &args, output, sidecar, manifest, &nothing);
  if (ok) {
    return 0;
  }
  if (nothing) {
    fprintf(stderr, "Lightmap bake skipped: %s\n", bake->error);
    return VKR_BAKERY_BAKE_NO_LIGHTMAPPED_INSTANCES;
  }
  fprintf(stderr, "Lightmap bake failed: %s\n", bake->error);
  return 1;
}

// =============================================================================
// Reflection probes (ADR-019)
// =============================================================================

typedef struct VkrBakeProbe {
  const char *scene;
  const char *workspace_root;
  const char *profile;
  bool8_t has_position;
  float64_t position[3];
  int64_t size;
  float64_t near_plane;
  float64_t far_plane;
  const char *output;
  const char *harness;
  bool8_t check;
} VkrBakeProbe;

vkr_internal const char *const vkr_bake_faces[] = {"px", "nx", "py",
                                                   "ny", "pz", "nz"};

/* (root / relative).resolve() that stays under root. */
vkr_internal bool8_t vkr_bake_confined(VkrBake *bake, const char *root,
                                       const char *relative, const char *label,
                                       char *out) {
  if (!relative || !relative[0]) {
    return vkr_bake_fail(bake, "%s path is missing", label);
  }
  char joined[VKR_BAKE_PATH];
  char resolved_root[VKR_BAKE_PATH];
  if (vkr_bakery_path_is_absolute(relative)) {
    (void)snprintf(joined, sizeof(joined), "%s", relative);
  } else {
    (void)vkr_bakery_path_join(joined, sizeof(joined), root, relative);
  }
  (void)vkr_bake_resolve(joined, out);
  (void)vkr_bake_resolve(root, resolved_root);
  if (!vkr_bake_is_under(out, resolved_root)) {
    return vkr_bake_fail(bake, "%s path escapes its artifact root", label);
  }
  return true_v;
}

vkr_internal const char *vkr_bake_relative(VkrBake *bake, const char *root,
                                           const char *path) {
  char relative[VKR_BAKE_PATH];
  char resolved_root[VKR_BAKE_PATH];
  (void)vkr_bake_resolve(root, resolved_root);
  if (!vkr_bakery_path_relative(resolved_root, path, relative,
                                sizeof(relative))) {
    return path;
  }
  return vkr_bake_printf(bake, "%s", relative);
}

typedef struct VkrBakeManifestAsset {
  const char *path;
  /* What the aggregate folds: the byte digest (schema 1) or identity. */
  const char *sha256;
  uint64_t bytes;
} VkrBakeManifestAsset;

vkr_internal int vkr_bake_compare_assets(const void *lhs, const void *rhs) {
  return strcmp(((const VkrBakeManifestAsset *)lhs)->path,
                ((const VkrBakeManifestAsset *)rhs)->path);
}

/* Validates a harness scene-content manifest; collects its asset paths.
 * Schema 1 records each asset's byte digest and folds it, with its byte
 * count, into the aggregate. Schema 2 records a host-neutral identity
 * (ADR-051) that only the harness can derive, and folds the identity alone;
 * the bake then records each asset's byte digest beside it, so a stored
 * manifest still detects any later change to a scene input. */
vkr_internal bool8_t vkr_bake_scene_manifest(VkrBake *bake,
                                             const VkrBakeryJson *manifest,
                                             const char *scene,
                                             const char *root,
                                             VkrBakeryJson *out_paths) {
  Arena *arena = bake->arena;
  int64_t schema = 0;
  if (!manifest || manifest->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_bake_fail(bake, "Scene-content manifest is not an object");
  }
  if (!vkr_bake_int(vkr_bakery_json_get(manifest, "schema_version"), &schema) ||
      (schema != 1 && schema != 2) ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(manifest, "kind"),
                                 VKR_BAKE_SCENE_MANIFEST_KIND)) {
    return vkr_bake_fail(bake, "Unexpected scene-content manifest contract");
  }
  const bool8_t identities = schema == 2;
  const char *manifest_scene = vkr_bake_text(manifest, "scene");
  if (!manifest_scene || !manifest_scene[0]) {
    return vkr_bake_fail(bake, "Scene-content manifest is missing its scene");
  }
  if (scene &&
      strcmp(manifest_scene, vkr_bake_relative(bake, root, scene)) != 0) {
    return vkr_bake_fail(
        bake, "Scene-content manifest scene differs from the requested scene");
  }
  const char *aggregate = vkr_bake_text(manifest, "sha256");
  if (!aggregate || strncmp(aggregate, "sha256:", 7u) != 0) {
    return vkr_bake_fail(bake, "Scene-content manifest is missing its digest");
  }
  const VkrBakeryJson *assets = vkr_bakery_json_get(manifest, "assets");
  if (!assets || assets->type != VKR_BAKERY_JSON_ARRAY || !assets->count) {
    return vkr_bake_fail(bake, "Scene-content manifest has no assets");
  }
  VkrBakeManifestAsset *entries = (VkrBakeManifestAsset *)arena_alloc(
      arena, sizeof(VkrBakeManifestAsset) * assets->count,
      ARENA_MEMORY_TAG_ARRAY);
  if (!entries) {
    return vkr_bake_fail(bake, "Out of memory");
  }
  uint32_t count = 0u;
  for (VkrBakeryJson *asset = assets->first; asset; asset = asset->next) {
    const char *relative = vkr_bake_text(asset, "path");
    const char *identity = identities ? vkr_bake_text(asset, "identity") : NULL;
    if (asset->type != VKR_BAKERY_JSON_OBJECT || !relative ||
        (identities && !identity)) {
      return vkr_bake_fail(bake, "Scene-content manifest contains an invalid "
                                 "asset");
    }
    char path[VKR_BAKE_PATH];
    VKR_BAKE_TRY(vkr_bake_confined(bake, root, relative, "Scene asset", path));
    int64_t bytes = -1;
    if (vkr_bakery_json_get(out_paths, path) || !vkr_bakery_is_file(path) ||
        !vkr_bake_int(vkr_bakery_json_get(asset, "bytes"), &bytes)) {
      return vkr_bake_fail(bake, "Scene-content manifest contains an "
                                 "unavailable asset");
    }
    VkrBakeryStat info;
    char digest[72];
    const char *recorded = vkr_bake_text(asset, "sha256");
    if (bytes < 0 || !vkr_bakery_stat(path, &info) ||
        info.size != (uint64_t)bytes || !vkr_bake_digest(bake, path, digest) ||
        (recorded && strcmp(recorded, digest) != 0) ||
        (!recorded && !identities)) {
      return vkr_bake_fail(bake, "Scene input changed since the harness "
                                 "manifest was written");
    }
    if (!recorded) {
      recorded = vkr_bake_printf(bake, "%s", digest);
      vkr_bakery_json_set(arena, asset, "sha256",
                          vkr_bakery_json_cstr(arena, recorded));
    }
    vkr_bakery_json_set(arena, out_paths, path,
                        vkr_bakery_json_bool(arena, true_v));
    entries[count++] = (VkrBakeManifestAsset){
        relative, identities ? identity : recorded, (uint64_t)bytes};
  }
  qsort(entries, count, sizeof(entries[0]), vkr_bake_compare_assets);
  VkrSha256 state;
  vkr_sha256_init(&state);
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t length = (uint32_t)strlen(entries[i].path);
    const uint8_t length_bytes[4] = {(uint8_t)(length >> 24),
                                     (uint8_t)(length >> 16),
                                     (uint8_t)(length >> 8), (uint8_t)length};
    vkr_sha256_update(&state, length_bytes, sizeof(length_bytes));
    vkr_sha256_update(&state, entries[i].path, length);
    vkr_sha256_update(&state, entries[i].sha256, strlen(entries[i].sha256));
    /* Schema 2 leaves the byte count out: it differs between hosts. */
    if (!identities) {
      uint8_t size_bytes[8];
      for (uint32_t b = 0u; b < 8u; ++b) {
        size_bytes[b] = (uint8_t)(entries[i].bytes >> (56u - b * 8u));
      }
      vkr_sha256_update(&state, size_bytes, sizeof(size_bytes));
    }
  }
  uint8_t hash[32];
  vkr_sha256_final(&state, hash);
  char expected[72] = "sha256:";
  for (uint32_t i = 0u; i < 32u; ++i) {
    (void)snprintf(expected + 7u + i * 2u, 3u, "%02x", hash[i]);
  }
  if (strcmp(aggregate, expected) != 0) {
    return vkr_bake_fail(bake, "Scene-content manifest aggregate digest "
                               "mismatch");
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_protect_scene(VkrBake *bake, const char *output,
                                            const char *sidecar,
                                            const char *scene,
                                            const VkrBakeryJson *manifest,
                                            const char *root) {
  VkrBakeryJson *sources = vkr_bakery_json_object(bake->arena);
  VKR_BAKE_TRY(vkr_bake_scene_manifest(bake, manifest, scene, root, sources));
  const char *targets[] = {output, sidecar};
  for (uint32_t i = 0u; i < ArrayCount(targets); ++i) {
    if (strcmp(targets[i], scene) == 0 ||
        vkr_bakery_json_get(sources, targets[i])) {
      return vkr_bake_fail(bake, "Output or bake metadata would overwrite a "
                                 "scene source asset");
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_bake_report_fingerprints(VkrBake *bake,
                                                  const VkrBakeryJson *report,
                                                  const char *out[3]) {
  const VkrBakeryJson *comparison = vkr_bakery_json_get(report, "comparison");
  if (!comparison || comparison->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_bake_fail(bake, "Snapshot report is missing comparison "
                               "fingerprints");
  }
  static const char *const names[] = {
      "environment_fingerprint", "workload_fingerprint", "policy_fingerprint"};
  for (uint32_t i = 0u; i < 3u; ++i) {
    out[i] = vkr_bake_text(comparison, names[i]);
    if (!out[i] || strncmp(out[i], "sha256:", 7u) != 0) {
      return vkr_bake_fail(bake, "Snapshot report has invalid comparison "
                                 "fingerprints");
    }
  }
  return true_v;
}

vkr_internal VkrBakeryJson *
vkr_bake_report_provenance(VkrBake *bake, const VkrBakeryJson *report) {
  const VkrBakeryJson *provenance = vkr_bakery_json_get(report, "provenance");
  if (!provenance || provenance->type != VKR_BAKERY_JSON_OBJECT) {
    vkr_bake_fail(bake, "Snapshot report is missing provenance");
    return NULL;
  }
  static const char *const names[] = {
      "git_sha",       "dirty",         "binary_sha256", "gpu",
      "gpu_vendor_id", "gpu_device_id", "driver"};
  VkrBakeryJson *result = vkr_bakery_json_object(bake->arena);
  for (uint32_t i = 0u; i < ArrayCount(names); ++i) {
    const VkrBakeryJson *value = vkr_bakery_json_get(provenance, names[i]);
    if (!value) {
      vkr_bake_fail(bake, "Snapshot report provenance is incomplete");
      return NULL;
    }
    vkr_bakery_json_set(bake->arena, result, names[i],
                        vkr_bakery_json_clone(bake->arena, value));
  }
  return result;
}

vkr_internal bool8_t vkr_bake_check_child(VkrBake *bake, const char *run,
                                          const VkrBakeryJson *parent) {
  const VkrBakeryJson *children = vkr_bakery_json_get(parent, "auxiliary_runs");
  if (!children || children->type != VKR_BAKERY_JSON_ARRAY ||
      children->count != 1u) {
    return vkr_bake_fail(bake,
                         "Snapshot report must have exactly one child run");
  }
  const VkrBakeryJson *child = children->first;
  if (!vkr_bakery_json_is_string(vkr_bakery_json_get(child, "status"),
                                 "pass")) {
    return vkr_bake_fail(bake, "Snapshot child run did not pass");
  }
  const char *fingerprints[3];
  VKR_BAKE_TRY(vkr_bake_report_fingerprints(bake, parent, fingerprints));
  static const char *const names[] = {
      "environment_fingerprint", "workload_fingerprint", "policy_fingerprint"};
  for (uint32_t i = 0u; i < 3u; ++i) {
    if (!vkr_bakery_json_is_string(vkr_bakery_json_get(child, names[i]),
                                   fingerprints[i])) {
      return vkr_bake_fail(bake, "Snapshot child fingerprints do not match the "
                                 "parent");
    }
  }
  char child_path[VKR_BAKE_PATH];
  char digest[72];
  VKR_BAKE_TRY(vkr_bake_confined(bake, run, vkr_bake_text(child, "report"),
                                 "Snapshot child report", child_path));
  if (!vkr_bakery_is_file(child_path) ||
      !vkr_bake_digest(bake, child_path, digest) ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(child, "sha256"),
                                 digest)) {
    return vkr_bake_fail(bake, "Snapshot child report digest mismatch");
  }
  VkrBakeryJson *report =
      vkr_bake_load(bake, child_path, "Snapshot child report");
  VKR_BAKE_TRY(report);
  if (!vkr_bakery_json_is_string(vkr_bakery_json_get(report, "status"),
                                 "pass") ||
      !vkr_bakery_json_is_string(
          vkr_bakery_json_get(vkr_bakery_json_get(report, "case"), "id"),
          VKR_BAKE_PROBE_CASE)) {
    return vkr_bake_fail(bake, "Snapshot child report is not the requested "
                               "cubemap capture");
  }
  return true_v;
}

vkr_internal VkrBakeryJson *vkr_bake_capture(VkrBake *bake, const char *run,
                                             const VkrBakeryJson *report,
                                             const char *face,
                                             int64_t checkpoint, int64_t size,
                                             char *out_raw) {
  Arena *arena = bake->arena;
  const VkrBakeryJson *captures = vkr_bakery_json_get(report, "captures");
  const VkrBakeryJson *capture = NULL;
  uint32_t matches = 0u;
  for (const VkrBakeryJson *item =
           captures && captures->type == VKR_BAKERY_JSON_ARRAY ? captures->first
                                                               : NULL;
       item; item = item->next) {
    int64_t frame = -1;
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(item, "channel"),
                                  VKR_BAKE_PROBE_CHANNEL) &&
        vkr_bake_int(vkr_bakery_json_get(item, "checkpoint_frame"), &frame) &&
        frame == checkpoint) {
      capture = item;
      matches += 1u;
    }
  }
  if (matches != 1u) {
    vkr_bake_fail(bake,
                  "Snapshot report must contain one HDR post-transmission "
                  "capture for face %s",
                  face);
    return NULL;
  }
  /* The harness report vocabulary serializes linear capture color space as
     "none". */
  static const char *const text_fields[][2] = {
      {"canonical_encoding", "RGBA16_FLOAT_LE"},
      {"value_kind", "color"},
      {"color_space", "none"},
      {"origin", "top_left"}};
  static const char *const zero_fields[] = {"mip", "layer"};
  for (uint32_t i = 0u; i < ArrayCount(text_fields); ++i) {
    if (!vkr_bakery_json_is_string(
            vkr_bakery_json_get(capture, text_fields[i][0]),
            text_fields[i][1])) {
      vkr_bake_fail(bake, "Unexpected HDR capture contract");
      return NULL;
    }
  }
  for (uint32_t i = 0u; i < ArrayCount(zero_fields); ++i) {
    int64_t value = -1;
    if (!vkr_bake_int(vkr_bakery_json_get(capture, zero_fields[i]), &value) ||
        value != 0) {
      vkr_bake_fail(bake, "Unexpected HDR capture contract");
      return NULL;
    }
  }
  int64_t width = 0;
  int64_t height = 0;
  if (!vkr_bake_int(vkr_bakery_json_get(capture, "width"), &width) ||
      !vkr_bake_int(vkr_bakery_json_get(capture, "height"), &height) ||
      width != size || height != size) {
    vkr_bake_fail(bake, "Unexpected HDR capture contract");
    return NULL;
  }
  char metadata_path[VKR_BAKE_PATH];
  char digest[72];
  if (!vkr_bake_confined(bake, run, vkr_bake_text(capture, "data_path"),
                         "HDR capture", out_raw) ||
      !vkr_bake_confined(bake, run, vkr_bake_text(capture, "metadata_path"),
                         "HDR capture metadata", metadata_path)) {
    return NULL;
  }
  if (!vkr_bakery_is_file(out_raw) || !vkr_bakery_is_file(metadata_path) ||
      !vkr_bake_digest(bake, out_raw, digest) ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(capture, "data_sha256"),
                                 digest)) {
    vkr_bake_fail(bake, "HDR capture payload path/digest mismatch");
    return NULL;
  }
  if (!vkr_bake_digest(bake, metadata_path, digest) ||
      !vkr_bakery_json_is_string(
          vkr_bakery_json_get(capture, "metadata_sha256"), digest)) {
    vkr_bake_fail(bake, "HDR capture metadata digest mismatch");
    return NULL;
  }
  VkrBakeryJson *metadata =
      vkr_bake_load(bake, metadata_path, "HDR capture metadata");
  if (!metadata) {
    return NULL;
  }
  static const char *const fields[] = {"channel",
                                       "capture_version",
                                       "producer_resource",
                                       "value_kind",
                                       "color_space",
                                       "source_format",
                                       "canonical_encoding",
                                       "origin",
                                       "width",
                                       "height",
                                       "source_row_pitch",
                                       "mip",
                                       "layer",
                                       "source_frame_index",
                                       "submit_serial",
                                       "data_sha256"};
  int64_t schema = 0;
  bool8_t same =
      vkr_bake_int(vkr_bakery_json_get(metadata, "schema_version"), &schema) &&
      schema == 1;
  for (uint32_t i = 0u; same && i < ArrayCount(fields); ++i) {
    const VkrBakeryJson *lhs = vkr_bakery_json_get(metadata, fields[i]);
    const VkrBakeryJson *rhs = vkr_bakery_json_get(capture, fields[i]);
    same = (!lhs && !rhs) || (lhs && rhs && vkr_bakery_json_equal(lhs, rhs));
  }
  if (!same) {
    vkr_bake_fail(bake, "HDR capture metadata does not match its report row");
    return NULL;
  }
  /* Child metadata paths are child-root relative; the parent report rebases
     them. */
  char metadata_root[VKR_BAKE_PATH];
  char metadata_parent[VKR_BAKE_PATH];
  char payload[VKR_BAKE_PATH];
  vkr_bakery_path_parent(metadata_parent, sizeof(metadata_parent),
                         metadata_path);
  vkr_bakery_path_parent(metadata_root, sizeof(metadata_root), metadata_parent);
  if (!vkr_bake_confined(bake, metadata_root,
                         vkr_bake_text(metadata, "data_path"),
                         "Metadata payload", payload)) {
    return NULL;
  }
  if (strcmp(payload, out_raw) != 0) {
    vkr_bake_fail(bake, "HDR metadata payload differs from its report row");
    return NULL;
  }
  VkrBakeryJson *record = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, record, "face", vkr_bakery_json_cstr(arena, face));
  vkr_bakery_json_set(arena, record, "sha256",
                      vkr_bakery_json_clone(
                          arena, vkr_bakery_json_get(capture, "data_sha256")));
  vkr_bakery_json_set(
      arena, record, "metadata_sha256",
      vkr_bakery_json_clone(arena,
                            vkr_bakery_json_get(capture, "metadata_sha256")));
  return record;
}

vkr_internal VkrBakeryJson *vkr_bake_probe_case(VkrBake *bake,
                                                const VkrBakeProbe *args,
                                                const char *scene_relative) {
  static const char text[] =
      "{\"schema_version\": 1, \"suite\": \"local\", \"description\": \"Static "
      "HDR probe faces in one renderer session; global illumination retained, "
      "local probes disabled.\", \"seed\": 1, \"boot\": \"full\", "
      "\"target\": \"offscreen\", \"present\": \"none\", "
      "\"target_image_count\": 2, \"cache\": \"isolated_cold\", "
      "\"repetitions\": 1, \"repetition_timeout_ms\": 600000, "
      "\"asset_ready_timeout_ms\": 240000, \"renderer\": {\"editor\": "
      "false, \"skybox\": true, \"shadow_preset\": \"balanced\", "
      "\"shadow_cascades\": 4, \"taa_enabled\": false, \"tonemap_enabled\": "
      "false, \"fxaa_enabled\": false, \"exposure_mode\": \"manual\", "
      "\"manual_exposure\": 1, \"display_transform\": \"agx\", "
      "\"bloom_enabled\": false, \"gtao_enabled\": false, \"image_sharpness\": "
      "0, \"ibl_probe_limit\": 0, \"render_mode\": \"default\"}, "
      "\"assertions\": [{\"metric\": \"visibility.gbuffer.resolve_invalid\", "
      "\"stat\": \"max\", \"max\": 0}]}";
  Arena *arena = bake->arena;
  VkrBakeryJson *fixed = vkr_bakery_json_parse(arena, (const uint8_t *)text,
                                               sizeof(text) - 1u, 16u, NULL);
  if (!fixed) {
    return NULL;
  }
  VkrBakeryJson *document = vkr_bakery_json_object(arena);
  static const char *const head[] = {"schema_version", "suite", "description"};
  vkr_bakery_json_set(arena, document, "id",
                      vkr_bakery_json_cstr(arena, VKR_BAKE_PROBE_CASE));
  for (uint32_t i = 0u; i < ArrayCount(head); ++i) {
    vkr_bakery_json_set(
        arena, document, head[i],
        vkr_bakery_json_clone(arena, vkr_bakery_json_get(fixed, head[i])));
  }
  vkr_bakery_json_set(arena, document, "scene",
                      vkr_bakery_json_cstr(arena, scene_relative));
  VkrBakeryJson *resolution = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(resolution, vkr_bakery_json_int(arena, args->size));
  vkr_bakery_json_append(resolution, vkr_bakery_json_int(arena, args->size));
  vkr_bakery_json_set(arena, document, "resolution", resolution);
  static const char *const middle[] = {"seed",
                                       "boot",
                                       "target",
                                       "present",
                                       "target_image_count",
                                       "cache",
                                       "repetitions",
                                       "repetition_timeout_ms",
                                       "asset_ready_timeout_ms",
                                       "renderer",
                                       "assertions"};
  for (uint32_t i = 0u; i < ArrayCount(middle); ++i) {
    vkr_bakery_json_set(
        arena, document, middle[i],
        vkr_bakery_json_clone(arena, vkr_bakery_json_get(fixed, middle[i])));
  }
  vkr_bakery_json_set(arena, document, "fixed_delta",
                      vkr_bakery_json_float(arena, 1.0 / 60.0));
  /* Each face renders VKR_BAKE_PROBE_FACE_FRAMES frames after the previous
     face's capture completes; the first face follows the boot warmup. */
  VkrBakeryJson *frames = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, frames, "warmup",
                      vkr_bakery_json_int(arena, VKR_BAKE_PROBE_WARMUP_FRAMES));
  vkr_bakery_json_set(
      arena, frames, "measure",
      vkr_bakery_json_int(
          arena,
          VKR_BAKE_PROBE_FACE_FRAMES * (ArrayCount(vkr_bake_faces) - 1u) + 1));
  vkr_bakery_json_set(arena, document, "frames", frames);
  VkrBakeryJson *camera = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, camera, "mode",
                      vkr_bakery_json_cstr(arena, "cubemap_px"));
  VkrBakeryJson *position = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < 3u; ++i) {
    vkr_bakery_json_append(position,
                           vkr_bakery_json_float(arena, args->position[i]));
  }
  vkr_bakery_json_set(arena, camera, "position", position);
  vkr_bakery_json_set(arena, camera, "vertical_fov_degrees",
                      vkr_bakery_json_int(arena, 90));
  vkr_bakery_json_set(arena, camera, "near_plane",
                      vkr_bakery_json_float(arena, args->near_plane));
  vkr_bakery_json_set(arena, camera, "far_plane",
                      vkr_bakery_json_float(arena, args->far_plane));
  vkr_bakery_json_set(arena, document, "camera", camera);
  VkrBakeryJson *captures = vkr_bakery_json_array(arena);
  for (uint32_t f = 0u; f < ArrayCount(vkr_bake_faces); ++f) {
    VkrBakeryJson *capture = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, capture, "at_frame",
        vkr_bakery_json_int(arena, VKR_BAKE_PROBE_FACE_FRAMES * f));
    vkr_bakery_json_set(
        arena, capture, "camera_mode",
        vkr_bakery_json_cstr(
            arena, vkr_bake_printf(bake, "cubemap_%s", vkr_bake_faces[f])));
    VkrBakeryJson *channels = vkr_bakery_json_array(arena);
    vkr_bakery_json_append(channels,
                           vkr_bakery_json_cstr(arena, VKR_BAKE_PROBE_CHANNEL));
    vkr_bakery_json_set(arena, capture, "channels", channels);
    vkr_bakery_json_append(captures, capture);
  }
  vkr_bakery_json_set(arena, document, "captures", captures);
  vkr_bakery_json_set(arena, document, "capture_session",
                      vkr_bakery_json_cstr(arena, "single"));
  if (args->workspace_root) {
    vkr_bakery_json_set(arena, document, "asset_context",
                        vkr_bakery_json_cstr(arena, "managed_workspace"));
  }
  return document;
}

/* The last harness JSON line that names its report. */
vkr_internal VkrBakeryJson *vkr_bake_publication(VkrBake *bake,
                                                 const char *log) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(log, 0u, &data, &length)) {
    return NULL;
  }
  VkrBakeryJson *publication = NULL;
  const char *cursor = (const char *)data;
  const char *end = cursor + length;
  while (cursor < end) {
    const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
    const char *line_end = newline ? newline : end;
    const uint64_t line_length = (uint64_t)(line_end - cursor);
    if (line_length && cursor[0] == '{') {
      bool8_t names_report = false_v;
      for (const char *c = cursor; c + 8 <= line_end && !names_report; ++c) {
        names_report = MemCompare(c, "\"report\"", 8u) == 0;
      }
      if (names_report) {
        VkrBakeryJson *parsed = vkr_bakery_json_parse(
            bake->arena, (const uint8_t *)cursor, line_length, 64u, NULL);
        if (parsed) {
          publication = parsed;
        }
      }
    }
    cursor = line_end + 1;
  }
  free(data);
  return publication;
}

/* Renders all six faces in one harness snapshot; validates its report,
   child run and scene manifest, then collects each face's raw capture. */
vkr_internal bool8_t vkr_bake_capture_faces(
    VkrBake *bake, const VkrBakeProbe *args, const char *root,
    const char *scene, const char *job, const char *cases, const char *profile,
    const char *harness, VkrBakeryJson **scene_manifest,
    VkrBakeryJson *captures, char raws[6][VKR_BAKE_PATH], char *out_run) {
  Arena *arena = bake->arena;
  VkrBakeryJson *document =
      vkr_bake_probe_case(bake, args, vkr_bake_relative(bake, root, scene));
  if (!document) {
    return vkr_bake_fail(bake, "Out of memory");
  }
  const char *case_path = vkr_bake_printf(bake, "%s/probe.case.json", cases);
  VKR_BAKE_TRY(vkr_bake_write_json(bake, case_path, document, false_v));
  VKR_BAKE_TRY(vkr_bake_copy(bake, case_path,
                             vkr_bake_printf(bake, "%s/probe.case.json", job)));
  const char *arguments[8];
  uint32_t count = 0u;
  arguments[count++] = "snapshot";
  arguments[count++] = "--case";
  arguments[count++] = vkr_bake_relative(bake, root, case_path);
  arguments[count++] = "--profile";
  arguments[count++] = profile;
  if (args->workspace_root) {
    arguments[count++] = "--repo-root";
    arguments[count++] = root;
  }
  printf("Capturing six faces at %lldx%lld\n", (long long)args->size,
         (long long)args->size);
  fflush(stdout);
  /* Validation layers change timing and output; bakes run without them. */
  static const VkrPlatformEnvironmentVariable environment[] = {
      {"MTL_DEBUG_LAYER", NULL},
      {"MTL_SHADER_VALIDATION", NULL},
      {"VK_INSTANCE_LAYERS", NULL}};
  const char *log = vkr_bake_printf(bake, "%s/capture.log", job);
  int32_t code = -1;
  VKR_BAKE_TRY(vkr_bake_run(bake, harness, arguments, count, root, log, 660000u,
                            environment, ArrayCount(environment), &code));
  if (code) {
    return vkr_bake_fail(bake, "Probe capture failed (%d); see %s", code, job);
  }
  VkrBakeryJson *publication = vkr_bake_publication(bake, log);
  if (!publication) {
    return vkr_bake_fail(bake, "Probe capture produced no report; see %s", job);
  }
  char report_path[VKR_BAKE_PATH];
  char snapshot_root[VKR_BAKE_PATH];
  char resolved_snapshot[VKR_BAKE_PATH];
  char digest[72];
  VKR_BAKE_TRY(vkr_bake_confined(bake, root,
                                 vkr_bake_text(publication, "report"),
                                 "Snapshot report", report_path));
  (void)snprintf(snapshot_root, sizeof(snapshot_root),
                 "%s/build/_artifacts/snapshot", root);
  (void)vkr_bake_resolve(snapshot_root, resolved_snapshot);
  if (!vkr_bake_is_under(report_path, resolved_snapshot) ||
      !vkr_bakery_is_file(report_path)) {
    return vkr_bake_fail(
        bake, "Capture report is outside the snapshot artifact tree");
  }
  if (!vkr_bake_digest(bake, report_path, digest) ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(publication, "sha256"),
                                 digest)) {
    return vkr_bake_fail(bake, "Capture report digest mismatch");
  }
  VkrBakeryJson *report = vkr_bake_load(bake, report_path, "Snapshot report");
  VKR_BAKE_TRY(report);
  if (!vkr_bakery_json_is_string(vkr_bakery_json_get(report, "status"),
                                 "pass") ||
      !vkr_bakery_json_is_string(
          vkr_bakery_json_get(vkr_bakery_json_get(report, "case"), "id"),
          VKR_BAKE_PROBE_CASE)) {
    return vkr_bake_fail(bake, "Probe report did not pass for the requested "
                               "case");
  }
  const char *fingerprints[3];
  VKR_BAKE_TRY(vkr_bake_report_fingerprints(bake, report, fingerprints));
  VkrBakeryJson *provenance = vkr_bake_report_provenance(bake, report);
  VKR_BAKE_TRY(provenance);
  vkr_bakery_path_parent(out_run, VKR_BAKE_PATH, report_path);
  VkrBakeryJson *manifest = vkr_bake_load(
      bake, vkr_bake_printf(bake, "%s/scene-content-manifest.json", out_run),
      "Scene-content manifest");
  VKR_BAKE_TRY(manifest);
  VkrBakeryJson *paths = vkr_bakery_json_object(arena);
  VKR_BAKE_TRY(vkr_bake_scene_manifest(bake, manifest, scene, root, paths));
  *scene_manifest = manifest;
  VKR_BAKE_TRY(vkr_bake_check_child(bake, out_run, report));
  for (uint32_t f = 0u; f < ArrayCount(vkr_bake_faces); ++f) {
    VkrBakeryJson *capture = vkr_bake_capture(
        bake, out_run, report, vkr_bake_faces[f],
        (int64_t)VKR_BAKE_PROBE_FACE_FRAMES * f, args->size, raws[f]);
    VKR_BAKE_TRY(capture);
    vkr_bakery_json_set(arena, capture, "report_sha256",
                        vkr_bakery_json_clone(
                            arena, vkr_bakery_json_get(publication, "sha256")));
    vkr_bakery_json_set(arena, capture, "provenance",
                        vkr_bakery_json_clone(arena, provenance));
    vkr_bakery_json_append(captures, capture);
  }
  return vkr_bake_copy(bake, report_path,
                       vkr_bake_printf(bake, "%s/probe.report.json", job));
}

vkr_internal bool8_t vkr_bake_probe(VkrBake *bake, const VkrBakeProbe *args,
                                    const char *output, const char *sidecar) {
  Arena *arena = bake->arena;
  char root[VKR_BAKE_PATH];
  char scene[VKR_BAKE_PATH];
  if (args->workspace_root) {
    if (!vkr_bake_resolve(args->workspace_root, root) ||
        !vkr_bakery_is_directory(root)) {
      return vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'",
                           args->workspace_root);
    }
  } else {
    (void)snprintf(root, sizeof(root), "%s", bake->repo);
  }
  VKR_BAKE_TRY(vkr_bake_confined(bake, root, args->scene, "Scene", scene));
  char scenes_root[VKR_BAKE_PATH];
  (void)snprintf(scenes_root, sizeof(scenes_root), "%s/assets/scenes",
                 bake->repo);
  if (!vkr_bakery_is_file(scene) ||
      (!args->workspace_root && !vkr_bake_is_under(scene, scenes_root))) {
    return vkr_bake_fail(bake, "--scene must name an existing scene under "
                               "assets/scenes");
  }
  if (!args->has_position || !isfinite(args->position[0]) ||
      !isfinite(args->position[1]) || !isfinite(args->position[2])) {
    return vkr_bake_fail(bake, "--position requires three finite world "
                               "coordinates");
  }
  if (args->size < 1 || args->size > 2048 || (args->size & (args->size - 1))) {
    return vkr_bake_fail(bake, "--size must be a power of two from 1 to 2048");
  }
  if (!isfinite(args->near_plane) || !isfinite(args->far_plane) ||
      !(0.0 < args->near_plane && args->near_plane < args->far_plane)) {
    return vkr_bake_fail(bake, "Require finite 0 < near-plane < far-plane");
  }
  char harness[VKR_BAKE_PATH];
  if (args->harness) {
    (void)vkr_bake_resolve(args->harness, harness);
  } else {
    (void)snprintf(harness, sizeof(harness),
                   "%s/build_release/tools/vkr_harness", bake->repo);
  }
  if (!vkr_bakery_is_file(harness)) {
    return vkr_bake_fail(bake, "Missing vkr_harness; build with "
                               "build_release.sh/.bat first");
  }
  char id[33];
  vkr_bake_hex_id(id);
  char job[VKR_BAKE_PATH];
  char cases[VKR_BAKE_PATH];
  if (args->workspace_root) {
    (void)snprintf(job, sizeof(job), "%s/jobs/probe_bake_%s", root, id);
    (void)snprintf(cases, sizeof(cases), "%s/cases", job);
  } else {
    (void)snprintf(job, sizeof(job), "%s/build/_artifacts/probe_bake/%s",
                   bake->repo, id);
    (void)snprintf(cases, sizeof(cases), "%s/tools/cases/local/probe_bake_%s",
                   bake->repo, id);
  }
  if (!vkr_bakery_make_directories(cases) ||
      !vkr_bakery_make_directories(job)) {
    return vkr_bake_fail(bake, "Cannot create %s", job);
  }
  const char *profile = "tools/profiles/local-offscreen.json";
  bool8_t ok = true_v;
  if (args->workspace_root) {
    char source[VKR_BAKE_PATH];
    if (args->profile) {
      (void)vkr_bake_resolve(args->profile, source);
    } else {
      (void)snprintf(source, sizeof(source),
                     "%s/tools/profiles/local-offscreen.json", bake->repo);
    }
    const char *copied = vkr_bake_printf(bake, "%s/profile.json", job);
    ok = vkr_bake_copy(bake, source, copied);
    profile = vkr_bake_relative(bake, root, copied);
  }
  VkrBakeryJson *captures = vkr_bakery_json_array(arena);
  VkrBakeryJson *scene_manifest = NULL;
  char raws[6][VKR_BAKE_PATH];
  char run[VKR_BAKE_PATH] = {0};
  ok = ok &&
       vkr_bake_capture_faces(bake, args, root, scene, job, cases, profile,
                              harness, &scene_manifest, captures, raws, run);
  ok = ok && vkr_bake_protect_scene(bake, output, sidecar, scene,
                                    scene_manifest, root);
  if (ok) {
    char directory[VKR_BAKE_PATH];
    vkr_bakery_path_parent(directory, sizeof(directory), output);
    const char *arguments[32];
    uint32_t count = 0u;
    arguments[count++] = "tool";
    arguments[count++] = "hdr-cube";
    arguments[count++] = "--size";
    arguments[count++] = vkr_bake_printf(bake, "%lld", (long long)args->size);
    arguments[count++] = "--output";
    arguments[count++] = output;
    for (uint32_t f = 0u; f < ArrayCount(vkr_bake_faces); ++f) {
      arguments[count++] = "--face";
      arguments[count++] = raws[f];
    }
    int32_t code = -1;
    ok = vkr_bakery_make_directories(directory) &&
         vkr_bake_run(bake, bake->config->self_path, arguments, count, root,
                      vkr_bake_printf(bake, "%s/pack.log", job), 0u, NULL, 0u,
                      &code);
    if (ok && code != 0) {
      ok = vkr_bake_fail(bake,
                         "Command '%s tool hdr-cube' returned non-zero "
                         "exit status %d.",
                         bake->config->self_path, code);
    }
  }
  char output_digest[72];
  if (ok && vkr_bake_digest(bake, output, output_digest)) {
    VkrBakeryJson *metadata = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, metadata, "schema_version",
                        vkr_bakery_json_int(arena, 1));
    vkr_bakery_json_set(arena, metadata, "recipe_version",
                        vkr_bakery_json_int(arena, VKR_BAKE_VERSION));
    vkr_bakery_json_set(
        arena, metadata, "scene",
        vkr_bakery_json_cstr(arena, vkr_bake_relative(bake, root, scene)));
    VkrBakeryJson *position = vkr_bakery_json_array(arena);
    for (uint32_t i = 0u; i < 3u; ++i) {
      vkr_bakery_json_append(position,
                             vkr_bakery_json_float(arena, args->position[i]));
    }
    vkr_bakery_json_set(arena, metadata, "position", position);
    vkr_bakery_json_set(arena, metadata, "size",
                        vkr_bakery_json_int(arena, args->size));
    vkr_bakery_json_set(arena, metadata, "near_plane",
                        vkr_bakery_json_float(arena, args->near_plane));
    vkr_bakery_json_set(arena, metadata, "far_plane",
                        vkr_bakery_json_float(arena, args->far_plane));
    vkr_bakery_json_set(arena, metadata, "lighting",
                        vkr_bakery_json_cstr(arena,
                                             "direct + emissive + global "
                                             "environment; local probes "
                                             "disabled"));
    vkr_bakery_json_set(arena, metadata, "source_channel",
                        vkr_bakery_json_cstr(arena, VKR_BAKE_PROBE_CHANNEL));
    vkr_bakery_json_set(
        arena, metadata, "capture_to_cube",
        vkr_bakery_json_cstr(arena, "flip each face vertically once"));
    vkr_bakery_json_set(arena, metadata, "capture_session",
                        vkr_bakery_json_cstr(arena, "single"));
    vkr_bakery_json_set(arena, metadata, "scene_manifest", scene_manifest);
    vkr_bakery_json_set(arena, metadata, "output_sha256",
                        vkr_bakery_json_cstr(arena, output_digest));
    vkr_bakery_json_set(arena, metadata, "captures", captures);
    ok = vkr_bake_write_json(bake, sidecar, metadata, false_v) &&
         vkr_bake_write_json(bake, vkr_bake_printf(bake, "%s/bake.json", job),
                             metadata, false_v);
    if (ok) {
      (void)vkr_bakery_remove_tree(run);
      printf("{\"status\":\"baked\",\"output\":\"%s\",\"sha256\":\"%s\","
             "\"evidence\":\"%s\"}\n",
             output, output_digest, job);
    }
  } else {
    ok = false_v;
  }
  (void)vkr_bakery_remove_tree(cases);
  return ok;
}

vkr_internal bool8_t vkr_bake_probe_current(VkrBake *bake, const char *output,
                                            const char *sidecar,
                                            const char *root) {
  VkrBakeryJson *metadata = vkr_bake_load(bake, sidecar, "metadata");
  int64_t schema = 0;
  int64_t recipe = 0;
  char digest[72];
  if (!metadata ||
      !vkr_bake_int(vkr_bakery_json_get(metadata, "schema_version"), &schema) ||
      schema != 1 ||
      !vkr_bake_int(vkr_bakery_json_get(metadata, "recipe_version"), &recipe) ||
      recipe != VKR_BAKE_VERSION ||
      !vkr_bakery_json_is_string(
          vkr_bakery_json_get(metadata, "source_channel"),
          VKR_BAKE_PROBE_CHANNEL) ||
      !vkr_bake_digest(bake, output, digest) ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(metadata, "output_sha256"),
                                 digest)) {
    return false_v;
  }
  char scene[VKR_BAKE_PATH];
  char scenes_root[VKR_BAKE_PATH];
  (void)snprintf(scenes_root, sizeof(scenes_root), "%s/assets/scenes",
                 bake->repo);
  if (!vkr_bake_confined(bake, root, vkr_bake_text(metadata, "scene"),
                         "Bake scene", scene) ||
      !vkr_bakery_is_file(scene) ||
      (strcmp(root, bake->repo) == 0 &&
       !vkr_bake_is_under(scene, scenes_root)) ||
      !vkr_bake_protect_scene(bake, output, sidecar, scene,
                              vkr_bakery_json_get(metadata, "scene_manifest"),
                              root)) {
    return false_v;
  }
  const VkrBakeryJson *captures = vkr_bakery_json_get(metadata, "captures");
  if (!captures || captures->type != VKR_BAKERY_JSON_ARRAY ||
      captures->count != ArrayCount(vkr_bake_faces)) {
    return false_v;
  }
  const VkrBakeryJson *provenance =
      vkr_bakery_json_get(captures->first, "provenance");
  if (!provenance || provenance->type != VKR_BAKERY_JSON_OBJECT) {
    return false_v;
  }
  uint32_t f = 0u;
  for (const VkrBakeryJson *capture = captures->first; capture;
       capture = capture->next, ++f) {
    if (!vkr_bakery_json_is_string(vkr_bakery_json_get(capture, "face"),
                                   vkr_bake_faces[f]) ||
        !vkr_bake_text(capture, "sha256") ||
        !vkr_bake_text(capture, "report_sha256") ||
        !vkr_bake_text(capture, "metadata_sha256") ||
        !vkr_bakery_json_equal(vkr_bakery_json_get(capture, "provenance"),
                               provenance)) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal int vkr_bake_probe_main(VkrBake *bake, int argc, char **argv) {
  VkrBakeProbe args = {.size = 256, .near_plane = 0.1, .far_plane = 1000.0};
  for (int i = 1; i < argc; ++i) {
    const char *flag = argv[i];
    bool8_t ok = true_v;
    if (!strcmp(flag, "--scene")) {
      ok = (args.scene = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--workspace-root")) {
      ok = (args.workspace_root = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--profile")) {
      ok = (args.profile = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--position")) {
      ok = args.has_position =
          vkr_bake_parse_numbers(argv, argc, &i, 3u, args.position);
    } else if (!strcmp(flag, "--size")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.size);
    } else if (!strcmp(flag, "--near-plane")) {
      ok = vkr_bake_parse_numbers(argv, argc, &i, 1u, &args.near_plane);
    } else if (!strcmp(flag, "--far-plane")) {
      ok = vkr_bake_parse_numbers(argv, argc, &i, 1u, &args.far_plane);
    } else if (!strcmp(flag, "--output")) {
      ok = (args.output = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--harness")) {
      ok = (args.harness = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--packer")) {
      ok = vkr_bake_value(argv, argc, &i) !=
           NULL; /* The packer is this binary. */
    } else if (!strcmp(flag, "--check")) {
      args.check = true_v;
    } else {
      ok = false_v;
    }
    if (!ok) {
      fprintf(stderr, "bake probe: invalid argument %s\n", flag);
      return 2;
    }
  }
  if (!args.output) {
    fprintf(stderr, "bake probe: --output is required\n");
    return 2;
  }
  char output[VKR_BAKE_PATH];
  char sidecar[VKR_BAKE_PATH];
  (void)vkr_bake_resolve(args.output, output);
  const uint64_t length = strlen(output);
  if (length < 4u || strcmp(output + length - 4u, ".vkt") != 0) {
    fprintf(stderr, "Probe bake failed: --output must name a .vkt file\n");
    return 1;
  }
  (void)snprintf(sidecar, sizeof(sidecar), "%s.bake.json", output);
  if (args.check) {
    char root[VKR_BAKE_PATH];
    if (args.workspace_root) {
      (void)vkr_bake_resolve(args.workspace_root, root);
    } else {
      (void)snprintf(root, sizeof(root), "%s", bake->repo);
    }
    const bool8_t current = vkr_bake_probe_current(bake, output, sidecar, root);
    printf("%s\n", current ? "current" : "stale");
    return current ? 0 : 1;
  }
  if (!args.scene) {
    fprintf(stderr, "bake probe: --scene is required for baking\n");
    return 2;
  }
  if (!vkr_bake_probe(bake, &args, output, sidecar)) {
    fprintf(stderr, "Probe bake failed: %s\n", bake->error);
    return 1;
  }
  return 0;
}

vkr_internal int vkr_bake_proxies_main(VkrBake *bake, int argc, char **argv);

int vkr_bakery_bake_main(const VkrBakeryConfig *config, int argc, char **argv) {
  if (argc < 1 || (strcmp(argv[0], "diffuse") && strcmp(argv[0], "probe") &&
                   strcmp(argv[0], "lightmap") && strcmp(argv[0], "proxies"))) {
    fprintf(
        stderr,
        "usage: vkr_bakery bake diffuse|probe|lightmap|proxies [options]\n");
    return 2;
  }
  VkrBake bake = {.config = config};
  bake.arena = arena_create(MB(512), MB(1));
  if (!bake.arena) {
    fprintf(stderr, "Out of memory\n");
    return 1;
  }
  (void)vkr_bake_resolve(vkr_content_root(), bake.repo);
  vkr_bakery_install_cancel_signals();
  const int code =
      strcmp(argv[0], "diffuse") == 0 ? vkr_bake_diffuse_main(&bake, argc, argv)
      : strcmp(argv[0], "probe") == 0 ? vkr_bake_probe_main(&bake, argc, argv)
      : strcmp(argv[0], "lightmap") == 0
          ? vkr_bake_lightmap_main(&bake, argc, argv)
          : vkr_bake_proxies_main(&bake, argc, argv);
  fflush(stdout);
  arena_destroy(bake.arena);
  return code;
}

// =============================================================================
// World partition proxies (ADR-086)
// =============================================================================

/* Metres of shape a proxy may lose to simplification: well under a pixel
   past a cell's load radius. */
#define VKR_PROXY_ERROR_M 0.25f
/* Records one cell document may hold, as the editor bounds them. */
#define VKR_PROXY_RECORDS_MAX 1024u
#define VKR_PROXY_MATERIALS_MAX 64u

typedef struct VkrProxyRecord {
  int64_t id;
  int64_t parent;
  Mat4 local;
  const VkrBakeryJson *components;
} VkrProxyRecord;

typedef struct VkrProxyMaterial {
  const char *path;
  Vec2 uv_scale;
} VkrProxyMaterial;

/* The cell's visible brush triangles, three corners each, in cell space. */
typedef struct VkrProxyMesh {
  float32_t *positions;
  uint32_t *materials;
  uint32_t triangle_count;
  uint32_t triangle_capacity;
  VkrProxyMaterial material[VKR_PROXY_MATERIALS_MAX];
  uint32_t material_count;
} VkrProxyMesh;

vkr_internal bool8_t vkr_proxy_floats(const VkrBakeryJson *array,
                                      float32_t *out, uint32_t count) {
  if (!array || array->type != VKR_BAKERY_JSON_ARRAY || array->count != count) {
    return false_v;
  }
  uint32_t i = 0u;
  for (const VkrBakeryJson *item = array->first; item; item = item->next) {
    float64_t value = 0.0;
    if (!vkr_bake_number(item, &value)) {
      return false_v;
    }
    out[i++] = (float32_t)value;
  }
  return true_v;
}

vkr_internal const VkrProxyRecord *vkr_proxy_find(const VkrProxyRecord *records,
                                                  uint32_t count, int64_t id) {
  for (uint32_t i = 0u; i < count; ++i) {
    if (records[i].id == id) {
      return &records[i];
    }
  }
  return NULL;
}

/* A record's world matrix through its parents in the same document. */
vkr_internal Mat4 vkr_proxy_world(const VkrProxyRecord *records, uint32_t count,
                                  const VkrProxyRecord *record) {
  Mat4 world = record->local;
  for (uint32_t depth = 0u; depth < 64u && record->parent; ++depth) {
    record = vkr_proxy_find(records, count, record->parent);
    if (!record) {
      break;
    }
    world = mat4_mul(record->local, world);
  }
  return world;
}

/* Meters one texture repeat of material `path` covers on brush faces, as
   the runtime reads it: its own `world_size=`, else its graph's, else 1 m. */
vkr_internal Vec2 vkr_proxy_world_size(VkrBake *bake, const char *path);

/* The proxy material for `path`, added with `uv_scale` (counting repeats of
   the material's world size when `sized`) the first time a face shows
   it. */
vkr_internal uint32_t vkr_proxy_material(VkrBake *bake, VkrProxyMesh *mesh,
                                         const char *path, Vec2 uv_scale,
                                         bool8_t sized) {
  for (uint32_t i = 0u; i < mesh->material_count; ++i) {
    if (!strcmp(mesh->material[i].path, path)) {
      return i;
    }
  }
  if (mesh->material_count == VKR_PROXY_MATERIALS_MAX) {
    return mesh->material_count - 1u;
  }
  if (sized) {
    const Vec2 world_size = vkr_proxy_world_size(bake, path);
    uv_scale = vec2_new(uv_scale.x * world_size.x, uv_scale.y * world_size.y);
  }
  mesh->material[mesh->material_count] = (VkrProxyMaterial){
      .path = vkr_bake_printf(bake, "%s", path), .uv_scale = uv_scale};
  return mesh->material_count++;
}

vkr_internal bool8_t vkr_proxy_triangle(VkrBake *bake, VkrProxyMesh *mesh,
                                        Vec3 a, Vec3 b, Vec3 c,
                                        uint32_t material) {
  if (mesh->triangle_count == mesh->triangle_capacity) {
    const uint32_t capacity = Max(256u, mesh->triangle_capacity * 2u);
    float32_t *positions = arena_alloc(
        bake->arena, capacity * 9u * sizeof(float32_t), ARENA_MEMORY_TAG_ARRAY);
    uint32_t *materials = arena_alloc(bake->arena, capacity * sizeof(uint32_t),
                                      ARENA_MEMORY_TAG_ARRAY);
    if (!positions || !materials) {
      return vkr_bake_fail(bake, "Out of memory");
    }
    if (mesh->triangle_count) {
      MemCopy(positions, mesh->positions,
              mesh->triangle_count * 9u * sizeof(float32_t));
      MemCopy(materials, mesh->materials,
              mesh->triangle_count * sizeof(uint32_t));
    }
    mesh->positions = positions;
    mesh->materials = materials;
    mesh->triangle_capacity = capacity;
  }
  float32_t *at = mesh->positions + mesh->triangle_count * 9u;
  const Vec3 corners[3] = {a, b, c};
  for (uint32_t i = 0u; i < 3u; ++i) {
    at[i * 3u + 0u] = corners[i].x;
    at[i * 3u + 1u] = corners[i].y;
    at[i * 3u + 2u] = corners[i].z;
  }
  mesh->materials[mesh->triangle_count++] = material;
  return true_v;
}

/* The value of `key=` in `.mt` text, or an empty string. */
vkr_internal String8 vkr_proxy_definition_value(String8 text, const char *key) {
  const uint64_t key_length = strlen(key);
  uint64_t at = 0u;
  while (at < text.length) {
    uint64_t end = at;
    while (end < text.length && text.str[end] != '\n') {
      end++;
    }
    String8 line = {.str = text.str + at, .length = end - at};
    string8_trim(&line);
    if (line.length > key_length && line.str[key_length] == '=' &&
        MemCompare(line.str, key, key_length) == 0) {
      String8 value = {.str = line.str + key_length + 1u,
                       .length = line.length - key_length - 1u};
      string8_trim(&value);
      return value;
    }
    at = end + 1u;
  }
  return (String8){0};
}

vkr_internal Vec2 vkr_proxy_world_size(VkrBake *bake, const char *path) {
  Vec2 size = vec2_new(1.0f, 1.0f);
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(vkr_bake_printf(bake, "%s/%s", bake->repo, path),
                            0u, &data, &length)) {
    return size;
  }
  const String8 text = {.str = data, .length = length};
  const String8 value = vkr_proxy_definition_value(text, "world_size");
  const String8 graph = vkr_proxy_definition_value(text, "graph");
  float32_t uniform = 0.0f;
  Vec2 parsed = {0};
  if (value.length) {
    if (string8_to_vec2(&value, &parsed)) {
      size = parsed;
    } else if (string8_to_f32(&value, &uniform)) {
      size = vec2_new(uniform, uniform);
    }
  } else if (graph.length) {
    /* An instance's graph lies beside it with ./ or ../, else under the
       content root. */
    const char *slash = strrchr(path, '/');
    const bool8_t relative = graph.str[0] == '.';
    const char *graph_path =
        relative && slash ? vkr_bake_printf(bake, "%s/%.*s/%.*s", bake->repo,
                                            (int)(slash - path), path,
                                            (int)graph.length, graph.str)
                          : vkr_bake_printf(bake, "%s/%.*s", bake->repo,
                                            (int)graph.length, graph.str);
    uint8_t *json = NULL;
    uint64_t json_length = 0u;
    VkrMaterialGraph *material_graph = arena_alloc(
        bake->arena, sizeof(*material_graph), ARENA_MEMORY_TAG_STRUCT);
    if (material_graph &&
        vkr_bakery_read_file(graph_path, 0u, &json, &json_length) &&
        vkr_material_graph_read((String8){.str = json, .length = json_length},
                                material_graph, NULL, 0u) &&
        material_graph->settings.world_size.x > 0.0f) {
      size = material_graph->settings.world_size;
    }
    free(json);
  }
  free(data);
  return size.x >= 0.01f && size.y >= 0.01f ? size : vec2_new(1.0f, 1.0f);
}

/* Adds brush `brush`'s faces, built from its `brush_face` children, in
   cell space. Clip and trigger brushes draw nothing in a game. */
vkr_internal bool8_t vkr_proxy_brush(VkrBake *bake, VkrProxyMesh *mesh,
                                     const VkrProxyRecord *records,
                                     uint32_t count,
                                     const VkrProxyRecord *brush, Vec3 origin,
                                     const VkrSurfaceTheme *theme) {
  String8 role = {0};
  const VkrBakeryJson *settings =
      vkr_bakery_json_get(brush->components, "brush");
  if (vkr_bakery_json_get_string(settings, "role", &role) &&
      (vkr_string8_equals_cstr(&role, "clip") ||
       vkr_string8_equals_cstr(&role, "trigger"))) {
    return true_v;
  }
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  const VkrBakeryJson *faces[VKR_BRUSH_FACE_MAX];
  uint32_t face_count = 0u;
  for (uint32_t i = 0u; i < count && face_count < VKR_BRUSH_FACE_MAX; ++i) {
    const VkrBakeryJson *face =
        records[i].parent == brush->id
            ? vkr_bakery_json_get(records[i].components, "brush_face")
            : NULL;
    float32_t normal[3];
    float64_t distance = 0.0;
    if (face &&
        vkr_proxy_floats(vkr_bakery_json_get(face, "normal"), normal, 3u) &&
        vkr_bakery_json_get_number(face, "distance", &distance)) {
      planes[face_count] =
          (VkrBrushPlane){.normal = vec3_new(normal[0], normal[1], normal[2]),
                          .distance = (float32_t)distance};
      faces[face_count++] = face;
    }
  }
  VkrBrushGeometry *geometry =
      arena_alloc(bake->arena, sizeof(*geometry), ARENA_MEMORY_TAG_STRUCT);
  if (!geometry) {
    return vkr_bake_fail(bake, "Out of memory");
  }
  if (vkr_brush_build(planes, face_count, geometry, NULL) != VKR_BRUSH_OK) {
    /* The editor reports a broken brush; its proxy leaves it out. */
    return true_v;
  }
  const Mat4 world = vkr_proxy_world(records, count, brush);
  for (uint32_t f = 0u; f < geometry->face_count; ++f) {
    const VkrBrushPolygon polygon = geometry->polygons[f];
    if (polygon.count < 3u) {
      continue;
    }
    Vec3 corners[VKR_BRUSH_POLYGON_MAX];
    for (uint32_t i = 0u; i < polygon.count; ++i) {
      corners[i] = vec3_sub(
          mat4_mul_vec3(world, geometry->vertices[polygon.first + i]), origin);
    }
    /* The face's art-owned material or its tag's theme binding, at its
       world size, or its surface's greybox look with the greybox repeat,
       as the runtime picks it (vkr_scene_brush.c). */
    const Vec3 normal = vec3_normalize(vec3_cross(
        vec3_sub(corners[1], corners[0]), vec3_sub(corners[2], corners[0])));
    float32_t uv_scale[2] = {1.0f, 1.0f};
    (void)vkr_proxy_floats(vkr_bakery_json_get(faces[f], "uv_scale"), uv_scale,
                           2u);
    String8 text = {0};
    const char *material = "";
    if (vkr_bakery_json_get_string(faces[f], "material", &text) &&
        text.length) {
      material = vkr_bake_printf(bake, "%.*s", (int)text.length, text.str);
    }
    VkrSurface surface = VKR_SURFACE_NONE;
    VkrSurfaceMark mark = VKR_SURFACE_MARK_NONE;
    if (vkr_bakery_json_get_string(faces[f], "surface", &text)) {
      (void)vkr_surface_find(
          vkr_bake_printf(bake, "%.*s", (int)text.length, text.str), &surface);
    }
    if (vkr_bakery_json_get_string(faces[f], "mark", &text)) {
      (void)vkr_surface_mark_find(
          vkr_bake_printf(bake, "%.*s", (int)text.length, text.str), &mark);
    }
    if (vkr_surface_from_legacy_material(material, &surface, &mark)) {
      material = "";
    }
    const char *look = vkr_surface_face_material(surface, mark, material, theme,
                                                 NULL, normal.y, false_v);
    const bool8_t greybox =
        look != material &&
        look != vkr_surface_theme_material(theme, NULL, surface);
    if (greybox) {
      uv_scale[0] = VKR_SURFACE_GREYBOX_REPEAT;
      uv_scale[1] = VKR_SURFACE_GREYBOX_REPEAT;
    }
    const uint32_t index = vkr_proxy_material(
        bake, mesh, look, vec2_new(uv_scale[0], uv_scale[1]), !greybox);
    for (uint32_t i = 1u; i + 1u < polygon.count; ++i) {
      VKR_BAKE_TRY(vkr_proxy_triangle(bake, mesh, corners[0], corners[i],
                                      corners[i + 1u], index));
    }
  }
  return true_v;
}

/* The proxy's source: the triangles welded by position and material,
   simplified within VKR_PROXY_ERROR_M, then written flat-shaded with
   world-projected UVs as glTF, one primitive and material per brush
   material, its buffer in `bin`. */
vkr_internal bool8_t vkr_proxy_gltf(VkrBake *bake, const VkrProxyMesh *mesh,
                                    VkrBakeryBuffer *gltf,
                                    VkrBakeryBuffer *bin) {
  const uint32_t corner_count = mesh->triangle_count * 3u;
  /* Welding keys: position, then the material as a float. */
  float32_t *keyed =
      arena_alloc(bake->arena, corner_count * 4u * sizeof(float32_t),
                  ARENA_MEMORY_TAG_ARRAY);
  uint32_t *remap = arena_alloc(bake->arena, corner_count * sizeof(uint32_t),
                                ARENA_MEMORY_TAG_ARRAY);
  uint32_t *indices = arena_alloc(bake->arena, corner_count * sizeof(uint32_t),
                                  ARENA_MEMORY_TAG_ARRAY);
  uint32_t *simplified = arena_alloc(
      bake->arena, corner_count * sizeof(uint32_t), ARENA_MEMORY_TAG_ARRAY);
  /* Per kept corner: position, normal, UV. */
  float32_t *attributes =
      arena_alloc(bake->arena, corner_count * 8u * sizeof(float32_t),
                  ARENA_MEMORY_TAG_ARRAY);
  if (!keyed || !remap || !indices || !simplified || !attributes) {
    return vkr_bake_fail(bake, "Out of memory");
  }
  for (uint32_t i = 0u; i < corner_count; ++i) {
    MemCopy(keyed + i * 4u, mesh->positions + i * 3u, 3u * sizeof(float32_t));
    keyed[i * 4u + 3u] = (float32_t)mesh->materials[i / 3u];
  }
  const size_t unique = meshopt_generateVertexRemap(
      remap, NULL, corner_count, keyed, corner_count, 4u * sizeof(float32_t));
  float32_t *vertices = arena_alloc(
      bake->arena, unique * 4u * sizeof(float32_t), ARENA_MEMORY_TAG_ARRAY);
  if (!vertices) {
    return vkr_bake_fail(bake, "Out of memory");
  }
  meshopt_remapVertexBuffer(vertices, keyed, corner_count,
                            4u * sizeof(float32_t), remap);
  meshopt_remapIndexBuffer(indices, NULL, corner_count, remap);
  /* Material borders stay where they are; flat surfaces lose their extra
     corners and small details collapse. */
  const size_t kept =
      meshopt_simplify(simplified, indices, corner_count, vertices, unique,
                       4u * sizeof(float32_t), 0u, VKR_PROXY_ERROR_M,
                       meshopt_SimplifyErrorAbsolute, NULL);
  vkr_bakery_buffer_appendf(gltf,
                            "{\"asset\":{\"version\":\"2.0\",\"generator\":"
                            "\"vkr_bakery proxies\"},\"scene\":0,\"scenes\":"
                            "[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"proxy\","
                            "\"mesh\":0}],\"materials\":[");
  for (uint32_t m = 0u; m < mesh->material_count; ++m) {
    vkr_bakery_buffer_appendf(
        gltf,
        "%s{\"name\":\"m%u\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,"
        "\"roughnessFactor\":0.9}}",
        m ? "," : "", m);
  }
  VkrBakeryBuffer primitives = {0};
  VkrBakeryBuffer accessors = {0};
  VkrBakeryBuffer views = {0};
  uint32_t primitive_count = 0u;
  for (uint32_t m = 0u; m < mesh->material_count; ++m) {
    uint32_t count = 0u;
    Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
    Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
    for (size_t t = 0u; t + 3u <= kept; t += 3u) {
      const uint32_t *corner = simplified + t;
      if ((uint32_t)vertices[corner[0] * 4u + 3u] != m) {
        continue;
      }
      Vec3 p[3];
      for (uint32_t k = 0u; k < 3u; ++k) {
        p[k] = vec3_new(vertices[corner[k] * 4u], vertices[corner[k] * 4u + 1u],
                        vertices[corner[k] * 4u + 2u]);
      }
      const Vec3 normal =
          vec3_cross(vec3_sub(p[1], p[0]), vec3_sub(p[2], p[0]));
      if (vec3_length(normal) < 1.0e-8f) {
        continue;
      }
      const Vec3 unit = vec3_normalize(normal);
      for (uint32_t k = 0u; k < 3u; ++k) {
        const Vec2 uv = vkr_brush_uv(p[k], unit, vec2_zero(),
                                     mesh->material[m].uv_scale, 0.0f);
        float32_t *at = attributes + (count + k) * 8u;
        at[0] = p[k].x;
        at[1] = p[k].y;
        at[2] = p[k].z;
        at[3] = unit.x;
        at[4] = unit.y;
        at[5] = unit.z;
        at[6] = uv.x;
        at[7] = uv.y;
        lo = vec3_new(Min(lo.x, p[k].x), Min(lo.y, p[k].y), Min(lo.z, p[k].z));
        hi = vec3_new(Max(hi.x, p[k].x), Max(hi.y, p[k].y), Max(hi.z, p[k].z));
      }
      count += 3u;
    }
    if (!count) {
      continue;
    }
    /* One interleaved view per material: position, normal, UV. */
    const uint64_t offset = bin->length;
    vkr_bakery_buffer_append(bin, attributes,
                             (uint64_t)count * 8u * sizeof(float32_t));
    const uint32_t view = primitive_count;
    const uint32_t accessor = primitive_count * 3u;
    vkr_bakery_buffer_appendf(
        &views,
        "%s{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu,"
        "\"byteStride\":32,\"target\":34962}",
        view ? "," : "", (unsigned long long)offset,
        (unsigned long long)count * 32ull);
    vkr_bakery_buffer_appendf(
        &accessors,
        "%s{\"bufferView\":%u,\"byteOffset\":0,\"componentType\":5126,"
        "\"count\":%u,\"type\":\"VEC3\",\"min\":[%.4f,%.4f,%.4f],\"max\":"
        "[%.4f,%.4f,%.4f]},{\"bufferView\":%u,\"byteOffset\":12,"
        "\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},"
        "{\"bufferView\":%u,\"byteOffset\":24,\"componentType\":5126,"
        "\"count\":%u,\"type\":\"VEC2\"}",
        view ? "," : "", view, count, (float64_t)lo.x, (float64_t)lo.y,
        (float64_t)lo.z, (float64_t)hi.x, (float64_t)hi.y, (float64_t)hi.z,
        view, count, view, count);
    vkr_bakery_buffer_appendf(
        &primitives,
        "%s{\"attributes\":{\"POSITION\":%u,\"NORMAL\":%u,\"TEXCOORD_0\":%u},"
        "\"material\":%u}",
        primitive_count ? "," : "", accessor, accessor + 1u, accessor + 2u, m);
    primitive_count++;
  }
  vkr_bakery_buffer_appendf(gltf, "],\"meshes\":[{\"name\":\"proxy\","
                                  "\"primitives\":[");
  vkr_bakery_buffer_append(gltf, primitives.data, primitives.length);
  vkr_bakery_buffer_appendf(gltf, "]}],\"accessors\":[");
  vkr_bakery_buffer_append(gltf, accessors.data, accessors.length);
  vkr_bakery_buffer_appendf(gltf, "],\"bufferViews\":[");
  vkr_bakery_buffer_append(gltf, views.data, views.length);
  vkr_bakery_buffer_appendf(gltf,
                            "],\"buffers\":[{\"uri\":\"proxy.bin\","
                            "\"byteLength\":%llu}]}\n",
                            (unsigned long long)bin->length);
  const bool8_t ok = primitive_count && !gltf->failed && !bin->failed &&
                     !primitives.failed && !accessors.failed && !views.failed;
  vkr_bakery_buffer_free(&primitives);
  vkr_bakery_buffer_free(&accessors);
  vkr_bakery_buffer_free(&views);
  return ok || vkr_bake_fail(bake, "The proxy has no visible triangles");
}

/* Whether `path` already holds `bytes`. */
vkr_internal bool8_t vkr_proxy_same(const char *path,
                                    const VkrBakeryBuffer *bytes) {
  uint8_t *previous = NULL;
  uint64_t length = 0u;
  const bool8_t same =
      vkr_bakery_read_file(path, MB(256), &previous, &length) &&
      length == bytes->length &&
      MemCompare(previous, bytes->data, bytes->length) == 0;
  free(previous);
  return same;
}

/* Points each proxy material range at its brushes' material file. */
vkr_internal bool8_t vkr_proxy_bind(VkrBake *bake, const VkrProxyMesh *mesh,
                                    const char *output, const char *bundle) {
  Arena *arena = bake->arena;
  const char *remap_path = vkr_bake_printf(bake, "%s.remap.json", output);
  VkrBakeryJson *remap = vkr_bake_load(bake, remap_path, "Proxy material map");
  VKR_BAKE_TRY(remap);
  VkrBakeryJson *mappings = vkr_bakery_json_get(remap, "materials");
  if (!mappings || mappings->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_bake_fail(bake, "%s has no material ranges", remap_path);
  }
  VkrBakeryJson *bound = vkr_bakery_json_object(arena);
  for (const VkrBakeryJson *range = mappings->first; range;
       range = range->next) {
    /* The cooker names each OBJ material `<hash>_<order>.mt`. */
    const String8 key = range->key;
    uint64_t end = key.length;
    if (end > 3u && MemCompare(key.str + end - 3u, ".mt", 3u) == 0) {
      end -= 3u;
    }
    uint64_t start = end;
    while (start > 0u && key.str[start - 1u] >= '0' &&
           key.str[start - 1u] <= '9') {
      start--;
    }
    uint32_t m = 0u;
    for (uint64_t c = start; c < end && m < VKR_PROXY_MATERIALS_MAX; ++c) {
      m = m * 10u + (uint32_t)(key.str[c] - '0');
    }
    if (start == end || start == 0u || key.str[start - 1u] != '_' ||
        m >= mesh->material_count) {
      return vkr_bake_fail(bake, "%s names an unknown range", remap_path);
    }
    char absolute[VKR_BAKE_PATH];
    char relative[VKR_BAKE_PATH];
    if (!vkr_bakery_path_join(absolute, sizeof(absolute), bake->repo,
                              mesh->material[m].path) ||
        !vkr_bake_relpath(absolute, bundle, relative, sizeof(relative))) {
      return vkr_bake_fail(bake, "Path too long");
    }
    vkr_bakery_json_set(
        arena, bound, (const char *)range->key.str,
        vkr_bakery_json_cstr(arena, vkr_bake_printf(bake, "./%s", relative)));
  }
  vkr_bakery_json_set(arena, remap, "materials", bound);
  return vkr_bake_write_json(bake, remap_path, remap, false_v);
}

vkr_internal bool8_t vkr_proxy_exists(const char *path) {
  FILE *file = file_fopen(path, "rb");
  if (file) {
    fclose(file);
  }
  return file != NULL;
}

/* Builds or removes cell (x, z)'s proxy bundle; skips the cook when its
   source did not change. */
vkr_internal bool8_t vkr_proxy_cell(VkrBake *bake, const char *cells,
                                    float32_t cell_size, int64_t x, int64_t z,
                                    uint32_t *out_built) {
  const char *document = vkr_bake_printf(bake, "%s/%lld_%lld.json", cells,
                                         (long long)x, (long long)z);
  const char *bundle = vkr_bake_printf(bake, "%s/proxies/%lld_%lld", cells,
                                       (long long)x, (long long)z);
  VkrBakeryJson *root = vkr_bake_load(bake, document, "Cell document");
  VKR_BAKE_TRY(root);
  const VkrBakeryJson *created = vkr_bakery_json_get(root, "created");
  if (!created || created->type != VKR_BAKERY_JSON_ARRAY ||
      created->count > VKR_PROXY_RECORDS_MAX) {
    return vkr_bake_fail(bake, "%s: no created records", document);
  }
  VkrProxyRecord *records =
      arena_alloc(bake->arena, (created->count + 1u) * sizeof(*records),
                  ARENA_MEMORY_TAG_ARRAY);
  if (!records) {
    return vkr_bake_fail(bake, "Out of memory");
  }
  uint32_t count = 0u;
  for (const VkrBakeryJson *item = created->first; item; item = item->next) {
    VkrProxyRecord *record = &records[count++];
    float32_t position[3] = {0.0f, 0.0f, 0.0f};
    float32_t rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float32_t scale[3] = {1.0f, 1.0f, 1.0f};
    (void)vkr_proxy_floats(vkr_bakery_json_get(item, "position"), position, 3u);
    (void)vkr_proxy_floats(vkr_bakery_json_get(item, "rotation"), rotation, 4u);
    (void)vkr_proxy_floats(vkr_bakery_json_get(item, "scale"), scale, 3u);
    record->local = mat4_mul(
        mat4_mul(
            mat4_translate(vec3_new(position[0], position[1], position[2])),
            vkr_quat_to_mat4(
                (VkrQuat){rotation[0], rotation[1], rotation[2], rotation[3]})),
        mat4_scale(vec3_new(scale[0], scale[1], scale[2])));
    record->components = vkr_bakery_json_get(item, "components");
    (void)vkr_bakery_json_get_int(item, "id", &record->id);
    const VkrBakeryJson *parent = vkr_bakery_json_get(item, "parent");
    record->parent = 0;
    if (parent && parent->type == VKR_BAKERY_JSON_OBJECT) {
      (void)vkr_bakery_json_get_int(parent, "created", &record->parent);
    }
  }
  VkrProxyMesh mesh = {0};
  const Vec3 origin =
      vec3_new((float32_t)x * cell_size, 0.0f, (float32_t)z * cell_size);
  /* The scene's surface theme, the first one, as the runtime resolves a
     singleton; one that does not read binds nothing. */
  VkrSurfaceTheme *theme =
      arena_alloc(bake->arena, sizeof(*theme), ARENA_MEMORY_TAG_STRUCT);
  if (!theme) {
    return vkr_bake_fail(bake, "Out of memory");
  }
  MemZero(theme, sizeof(*theme));
  for (uint32_t i = 0u; i < count; ++i) {
    String8 theme_path = {0};
    if (!vkr_bakery_json_get_string(
            vkr_bakery_json_get(records[i].components, "surface_theme"),
            "theme", &theme_path) ||
        !theme_path.length) {
      continue;
    }
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (!vkr_bakery_read_file(vkr_bake_printf(bake, "%s/%.*s", bake->repo,
                                              (int)theme_path.length,
                                              theme_path.str),
                              0u, &data, &length) ||
        !vkr_surface_theme_read((String8){.str = data, .length = length}, theme,
                                NULL, 0u)) {
      MemZero(theme, sizeof(*theme));
    }
    free(data);
    break;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (vkr_bakery_json_get(records[i].components, "brush")) {
      VKR_BAKE_TRY(vkr_proxy_brush(bake, &mesh, records, count, &records[i],
                                   origin, theme));
    }
  }
  if (!mesh.triangle_count) {
    (void)vkr_bakery_remove_tree(bundle);
    return true_v;
  }
  VkrBakeryBuffer gltf = {0};
  VkrBakeryBuffer bin = {0};
  bool8_t ok = vkr_proxy_gltf(bake, &mesh, &gltf, &bin);
  const char *source = vkr_bake_printf(bake, "%s/proxy.gltf", bundle);
  const char *buffer = vkr_bake_printf(bake, "%s/proxy.bin", bundle);
  const char *output = vkr_bake_printf(bake, "%s/proxy.vkb", bundle);
  const bool8_t same = ok && vkr_proxy_same(source, &gltf) &&
                       vkr_proxy_same(buffer, &bin) && vkr_proxy_exists(output);
  if (ok && !same) {
    char import_id[33];
    vkr_bake_hex_id(import_id);
    const char *cook[] = {"tool",        "mesh",   "--input",       source,
                          "--output",    output,   "--bundle-root", bundle,
                          "--import-id", import_id};
    int32_t code = -1;
    ok = vkr_bakery_make_directories(bundle) &&
         vkr_bakery_write_file_atomic(buffer, bin.data, bin.length) &&
         vkr_bakery_write_file_atomic(source, gltf.data, gltf.length) &&
         vkr_bake_run(bake, bake->config->self_path, cook, ArrayCount(cook),
                      bundle, vkr_bake_printf(bake, "%s/cook.log", bundle),
                      600000u, NULL, 0u, &code);
    if (ok && code != 0) {
      ok = vkr_bake_fail(bake,
                         "The proxy of cell %lld,%lld did not cook; see "
                         "%s/cook.log",
                         (long long)x, (long long)z, bundle);
    }
    ok = ok && vkr_proxy_bind(bake, &mesh, output, bundle);
    *out_built += ok;
  }
  vkr_bakery_buffer_free(&gltf);
  vkr_bakery_buffer_free(&bin);
  return ok;
}

/* `bake proxies --scene <scene.json>`: a proxy per cell document of the
   scene's world partition, in `<cells>/proxies/<x>_<z>/proxy.vkb`. */
vkr_internal int vkr_bake_proxies_main(VkrBake *bake, int argc, char **argv) {
  const char *scene = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--scene")) {
      scene = vkr_bake_value(argv, argc, &i);
    } else {
      fprintf(stderr, "error: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (!scene) {
    fprintf(stderr, "usage: vkr_bakery bake proxies --scene <scene.json>\n");
    return 2;
  }
  char resolved[VKR_BAKE_PATH];
  if (!vkr_bake_resolve(scene, resolved)) {
    fprintf(stderr, "error: cannot resolve %s\n", scene);
    return 1;
  }
  uint64_t length = strlen(resolved);
  if (length > 5u && !strcmp(resolved + length - 5u, ".json")) {
    length -= 5u;
  }
  const char *cells =
      vkr_bake_printf(bake, "%.*s.cells", (int)length, resolved);
  VkrBakeryJson *index = vkr_bake_load(
      bake, vkr_bake_printf(bake, "%s/index.json", cells), "Cell index");
  float64_t cell_size = 0.0;
  const VkrBakeryJson *list =
      index ? vkr_bakery_json_get(index, "cells") : NULL;
  if (!index || !vkr_bakery_json_get_number(index, "cell_size", &cell_size) ||
      !(cell_size >= 1.0) || !list || list->type != VKR_BAKERY_JSON_ARRAY) {
    fprintf(stderr, "error: %s\n",
            bake->failed ? bake->error : "the cell index is unreadable");
    return 1;
  }
  uint32_t built = 0u;
  uint32_t cells_seen = 0u;
  bool8_t ok = true_v;
  for (const VkrBakeryJson *cell = list->first; ok && cell; cell = cell->next) {
    int64_t x = 0;
    int64_t z = 0;
    ok = cell->type == VKR_BAKERY_JSON_ARRAY && cell->count == 2u &&
         vkr_bake_int(cell->first, &x) && vkr_bake_int(cell->last, &z) &&
         vkr_proxy_cell(bake, cells, (float32_t)cell_size, x, z, &built);
    cells_seen++;
  }
  if (!ok) {
    fprintf(stderr, "error: %s\n", bake->error);
    return 1;
  }
  printf("proxies: %u cells, %u cooked\n", cells_seen, built);
  return 0;
}

// =============================================================================
// Material previews
// =============================================================================

/* Recipe 2: radius 1, 32 latitude/64 longitude intervals, neutral constant
 * ambient with two rectangular softboxes, 35 degree camera, manual exposure 1,
 * AgX, no temporal or post-process effects, rendered in an isolated harness.
 * The interactive renderer is never used; callers own the bounded thumbnail
 * cache. */
#define VKR_PREVIEW_RECIPE "material-sphere-v2"
#define VKR_PREVIEW_MAX_LOG_BYTES (4ull * 1024ull * 1024ull)
#define VKR_PREVIEW_TIMEOUT_MS 180000u
#define VKR_PREVIEW_LATITUDE 32u
#define VKR_PREVIEW_LONGITUDE 64u
#define VKR_PREVIEW_STUDIO_DISTANCE 6.0

/* Neutral studio: the mean of the former generated sky gradient as a
 * uniform ambient, and softboxes at its former panel directions, angular
 * extents and radiance. No active-scene sky, exposure or image is used. */
vkr_internal const float64_t vkr_preview_ambient = 0.24;

typedef struct VkrPreviewSoftbox {
  float64_t direction[3];
  float64_t extent_degrees[2];
  float64_t radiance;
} VkrPreviewSoftbox;

vkr_internal const VkrPreviewSoftbox vkr_preview_softboxes[] = {
    {{0.42, 0.15, 0.89}, {39.0, 73.0}, 1.2},
    {{0.51, 0.52, -0.69}, {67.0, 62.0}, 3.0},
};

/* Same UV/ring and triangle convention as the geometry sphere; degenerate
   pole triangles are skipped while normals and the UV seam remain. */
vkr_internal bool8_t vkr_preview_sphere(VkrBake *bake, const char *path) {
  VkrBakeryBuffer text = {0};
  vkr_bakery_buffer_append_cstr(
      &text, "mtllib sphere.mtl\no MaterialPreviewSphere\nusemtl "
             "preview_surface\n");
  const float64_t pi = 3.14159265358979323846;
  char line[160];
  for (uint32_t lat = 0u; lat <= VKR_PREVIEW_LATITUDE; ++lat) {
    const float64_t phi = pi * lat / VKR_PREVIEW_LATITUDE;
    for (uint32_t lon = 0u; lon <= VKR_PREVIEW_LONGITUDE; ++lon) {
      const float64_t theta = 2.0 * pi * lon / VKR_PREVIEW_LONGITUDE;
      const float64_t x = sin(phi) * cos(theta);
      const float64_t y = cos(phi);
      const float64_t z = sin(phi) * sin(theta);
      (void)snprintf(line, sizeof(line),
                     "v %.9g %.9g %.9g\nvn %.9g %.9g %.9g\n", x, y, z, x, y, z);
      vkr_bakery_buffer_append_cstr(&text, line);
      (void)snprintf(line, sizeof(line), "vt %.9g %.9g\n",
                     (float64_t)lon / VKR_PREVIEW_LONGITUDE,
                     1.0 - (float64_t)lat / VKR_PREVIEW_LATITUDE);
      vkr_bakery_buffer_append_cstr(&text, line);
    }
  }
  const uint32_t ring = VKR_PREVIEW_LONGITUDE + 1u;
  for (uint32_t lat = 0u; lat < VKR_PREVIEW_LATITUDE; ++lat) {
    for (uint32_t lon = 0u; lon < VKR_PREVIEW_LONGITUDE; ++lon) {
      const uint32_t a = lat * ring + lon + 1u;
      const uint32_t b = (lat + 1u) * ring + lon + 1u;
      if (lat != VKR_PREVIEW_LATITUDE - 1u) {
        (void)snprintf(line, sizeof(line), "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a,
                       a, a, b, b, b, b + 1u, b + 1u, b + 1u);
        vkr_bakery_buffer_append_cstr(&text, line);
      }
      if (lat != 0u) {
        (void)snprintf(line, sizeof(line), "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a,
                       a, a, b + 1u, b + 1u, b + 1u, a + 1u, a + 1u, a + 1u);
        vkr_bakery_buffer_append_cstr(&text, line);
      }
    }
  }
  static const char material[] = "newmtl preview_surface\nKd 0.5 0.5 0.5\n";
  char material_path[VKR_BAKE_PATH];
  (void)snprintf(material_path, sizeof(material_path), "%.*s.mtl",
                 (int)(strlen(path) - 4u), path);
  const bool8_t ok =
      !text.failed &&
      vkr_bakery_write_file_atomic(path, text.data, text.length) &&
      vkr_bakery_write_file_atomic(material_path, material,
                                   sizeof(material) - 1u);
  vkr_bakery_buffer_free(&text);
  return ok ? true_v : vkr_bake_fail(bake, "Cannot write %s", path);
}

vkr_internal void vkr_preview_normalize(float64_t v[3]) {
  const float64_t length = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  v[0] /= length;
  v[1] /= length;
  v[2] /= length;
}

vkr_internal void vkr_preview_cross(const float64_t a[3], const float64_t b[3],
                                    float64_t out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

/* [x, y, z, w] of the rotation whose matrix columns are x, y and z. */
vkr_internal void vkr_preview_quaternion(const float64_t x[3],
                                         const float64_t y[3],
                                         const float64_t z[3],
                                         float64_t out[4]) {
  const float64_t trace = x[0] + y[1] + z[2];
  if (trace > 0.0) {
    const float64_t scale = sqrt(trace + 1.0) * 2.0;
    out[0] = (y[2] - z[1]) / scale;
    out[1] = (z[0] - x[2]) / scale;
    out[2] = (x[1] - y[0]) / scale;
    out[3] = 0.25 * scale;
  } else if (x[0] > y[1] && x[0] > z[2]) {
    const float64_t scale = sqrt(1.0 + x[0] - y[1] - z[2]) * 2.0;
    out[0] = 0.25 * scale;
    out[1] = (y[0] + x[1]) / scale;
    out[2] = (z[0] + x[2]) / scale;
    out[3] = (y[2] - z[1]) / scale;
  } else if (y[1] > z[2]) {
    const float64_t scale = sqrt(1.0 + y[1] - x[0] - z[2]) * 2.0;
    out[0] = (y[0] + x[1]) / scale;
    out[1] = 0.25 * scale;
    out[2] = (z[1] + y[2]) / scale;
    out[3] = (z[0] - x[2]) / scale;
  } else {
    const float64_t scale = sqrt(1.0 + z[2] - x[0] - y[1]) * 2.0;
    out[0] = (z[0] + x[2]) / scale;
    out[1] = (z[1] + y[2]) / scale;
    out[2] = 0.25 * scale;
    out[3] = (x[1] - y[0]) / scale;
  }
}

vkr_internal VkrBakeryJson *
vkr_preview_vector(Arena *arena, const float64_t *values, uint32_t count) {
  VkrBakeryJson *array = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < count; ++i) {
    vkr_bakery_json_append(array, vkr_bakery_json_float(arena, values[i]));
  }
  return array;
}

vkr_internal VkrBakeryJson *vkr_preview_parse(VkrBake *bake, const char *text) {
  return vkr_bakery_json_parse(bake->arena, (const uint8_t *)text, strlen(text),
                               16u, NULL);
}

/* Rectangle lights emit along local -Z, so local +Z points at the panel. */
vkr_internal void vkr_preview_softbox_entities(VkrBake *bake,
                                               VkrBakeryJson *entities) {
  Arena *arena = bake->arena;
  for (uint32_t i = 0u; i < ArrayCount(vkr_preview_softboxes); ++i) {
    const VkrPreviewSoftbox *box = &vkr_preview_softboxes[i];
    float64_t forward[3] = {box->direction[0], box->direction[1],
                            box->direction[2]};
    vkr_preview_normalize(forward);
    const float64_t world_up[3] = {0.0, 1.0, 0.0};
    float64_t right[3];
    float64_t up[3];
    vkr_preview_cross(world_up, forward, right);
    vkr_preview_normalize(right);
    vkr_preview_cross(forward, right, up);
    float64_t rotation[4];
    vkr_preview_quaternion(right, up, forward, rotation);
    const float64_t position[3] = {forward[0] * VKR_PREVIEW_STUDIO_DISTANCE,
                                   forward[1] * VKR_PREVIEW_STUDIO_DISTANCE,
                                   forward[2] * VKR_PREVIEW_STUDIO_DISTANCE};
    const float64_t pi = 3.14159265358979323846;
    const float64_t size[2] = {
        2.0 * VKR_PREVIEW_STUDIO_DISTANCE *
            tan(box->extent_degrees[0] * pi / 180.0 * 0.5),
        2.0 * VKR_PREVIEW_STUDIO_DISTANCE *
            tan(box->extent_degrees[1] * pi / 180.0 * 0.5)};
    VkrBakeryJson *transform = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, transform, "pos",
                        vkr_preview_vector(arena, position, 3u));
    vkr_bakery_json_set(arena, transform, "rot",
                        vkr_preview_vector(arena, rotation, 4u));
    vkr_bakery_json_set(arena, transform, "scale",
                        vkr_preview_parse(bake, "[1, 1, 1]"));
    VkrBakeryJson *light = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, light, "color",
                        vkr_preview_parse(bake, "[1, 1, 1]"));
    vkr_bakery_json_set(arena, light, "radiance",
                        vkr_bakery_json_float(arena, box->radiance));
    vkr_bakery_json_set(arena, light, "size",
                        vkr_preview_vector(arena, size, 2u));
    VkrBakeryJson *entity = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, entity, "name",
        vkr_bakery_json_cstr(
            arena, vkr_bake_printf(bake, "Studio softbox %u", i + 1u)));
    vkr_bakery_json_set(arena, entity, "parent", vkr_bakery_json_null(arena));
    vkr_bakery_json_set(arena, entity, "transform", transform);
    vkr_bakery_json_set(arena, entity, "rectangle_light", light);
    vkr_bakery_json_append(entities, entity);
  }
}

vkr_internal VkrBakeryJson *
vkr_preview_case(VkrBake *bake, const char *scene_relative, int64_t size) {
  static const char text[] =
      "{\"schema_version\": 1, \"asset_context\": \"managed_workspace\", "
      "\"id\": \"local.material_preview\", \"suite\": \"local\", "
      "\"description\": \"" VKR_PREVIEW_RECIPE "\", \"scene\": \"\", \"seed\": "
      "1, \"resolution\": [0, 0], \"boot\": \"full\", \"target\": "
      "\"offscreen\", \"present\": \"none\", \"target_image_count\": 2, "
      "\"cache\": \"isolated_cold\", \"fixed_delta\": 0, \"repetitions\": 1, "
      "\"repetition_timeout_ms\": 120000, \"asset_ready_timeout_ms\": 90000, "
      "\"frames\": {\"warmup\": 4, \"measure\": 1}, \"renderer\": {\"editor\": "
      "false, \"skybox\": true, \"shadow_preset\": \"balanced\", "
      "\"shadow_cascades\": 4, \"taa_enabled\": false, \"tonemap_enabled\": "
      "true, \"fxaa_enabled\": true, \"exposure_mode\": \"manual\", "
      "\"manual_exposure\": 1, \"display_transform\": \"agx\", "
      "\"bloom_enabled\": false, \"gtao_enabled\": false, \"image_sharpness\": "
      "0, \"ibl_probe_limit\": 0, \"render_mode\": \"default\"}, \"camera\": "
      "{\"mode\": \"static\", \"position\": [0, 0, 4], \"yaw\": -90, "
      "\"pitch\": 0, \"vertical_fov_degrees\": 35, \"near_plane\": 0.1, "
      "\"far_plane\": 20}, \"captures\": [{\"at_frame\": 0, \"channels\": "
      "[\"final_color\"]}], \"assertions\": [{\"metric\": "
      "\"visibility.gbuffer.resolve_invalid\", \"stat\": \"max\", \"max\": "
      "0}]}";
  Arena *arena = bake->arena;
  VkrBakeryJson *document = vkr_preview_parse(bake, text);
  if (!document) {
    return NULL;
  }
  vkr_bakery_json_set(arena, document, "scene",
                      vkr_bakery_json_cstr(arena, scene_relative));
  VkrBakeryJson *resolution = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(resolution, vkr_bakery_json_int(arena, size));
  vkr_bakery_json_append(resolution, vkr_bakery_json_int(arena, size));
  vkr_bakery_json_set(arena, document, "resolution", resolution);
  vkr_bakery_json_set(arena, document, "fixed_delta",
                      vkr_bakery_json_float(arena, 1.0 / 60.0));
  return document;
}

vkr_internal const char vkr_preview_profile[] =
    "{\"schema_version\": 1, \"id\": \"local.material_preview\", "
    "\"authoritative\": false, \"dirty_policy\": \"allow\", \"environment\": "
    "{\"target\": \"offscreen\", \"required_present\": \"none\", "
    "\"require_actual_present\": false}, \"instrumentation\": "
    "{\"gpu_timing\": false, \"event_subjects\": false}, \"execution\": "
    "{\"minimum_repetitions\": 1, \"warmup_stability_window\": 2, "
    "\"warmup_max_drift_ratio\": 1, \"require_warmup_stability\": false, "
    "\"exclusive_gpu_lane\": false}, \"required_metrics\": []}";

/* Runs one preview child with a bounded log; the log names the failure. */
vkr_internal bool8_t vkr_preview_run(VkrBake *bake, const char *executable,
                                     const char *const *arguments,
                                     uint32_t count, const char *root,
                                     const char *log) {
  const char *backend = getenv("VKR_HARNESS_RENDERER_BACKEND");
  const VkrPlatformEnvironmentVariable environment[] = {
      {"MTL_DEBUG_LAYER", NULL},
      {"MTL_SHADER_VALIDATION", NULL},
      {"VK_INSTANCE_LAYERS", NULL},
      {"VKR_SCENE_PATH", NULL},
      {"VKR_AUTOLOAD_SCENE", NULL},
#if defined(__APPLE__)
      {"VKR_HARNESS_RENDERER_BACKEND", backend ? backend : "metal"},
#else
      {"VKR_HARNESS_RENDERER_BACKEND", backend ? backend : "vulkan"},
#endif
  };
  int32_t code = -1;
  VKR_BAKE_TRY(vkr_bake_run(bake, executable, arguments, count, root, log,
                            VKR_PREVIEW_TIMEOUT_MS, environment,
                            ArrayCount(environment), &code));
  VkrBakeryStat info;
  if (vkr_bakery_stat(log, &info) && info.size > VKR_PREVIEW_MAX_LOG_BYTES) {
    return vkr_bake_fail(
        bake, "Material preview exceeded its 4 MiB diagnostic limit");
  }
  if (code) {
    return vkr_bake_fail(bake, "%s failed (%d); see %s",
                         vkr_bakery_path_name(executable), code, log);
  }
  return true_v;
}

typedef struct VkrPreviewArgs {
  const char *input;
  const char *output;
  const char *workspace;
  const char *harness;
  int64_t size;
} VkrPreviewArgs;

vkr_internal bool8_t vkr_preview_bind_material(VkrBake *bake, const char *mesh,
                                               const char *material,
                                               const char *bundle) {
  Arena *arena = bake->arena;
  const char *remap_path = vkr_bake_printf(bake, "%s.remap.json", mesh);
  VkrBakeryJson *remap =
      vkr_bake_load(bake, remap_path, "Canonical sphere map");
  VKR_BAKE_TRY(remap);
  int64_t version = 0;
  VkrBakeryJson *mappings = vkr_bakery_json_get(remap, "materials");
  if (!vkr_bake_int(vkr_bakery_json_get(remap, "version"), &version) ||
      version != 1 || !mappings || mappings->type != VKR_BAKERY_JSON_OBJECT ||
      mappings->count != 1u) {
    return vkr_bake_fail(bake, "Canonical sphere must have exactly one "
                               "material range");
  }
  char relative[VKR_BAKE_PATH];
  if (!vkr_bake_relpath(material, bundle, relative, sizeof(relative))) {
    return vkr_bake_fail(bake, "Path too long");
  }
  VkrBakeryJson *bound = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(
      arena, bound, (const char *)mappings->first->key.str,
      vkr_bakery_json_cstr(arena, vkr_bake_printf(bake, "./%s", relative)));
  vkr_bakery_json_set(arena, remap, "materials", bound);
  return vkr_bake_write_json(bake, remap_path, remap, false_v);
}

vkr_internal bool8_t vkr_preview_publish(VkrBake *bake, const char *run_root,
                                         const VkrBakeryJson *report,
                                         int64_t size, const char *output) {
  const VkrBakeryJson *captures = vkr_bakery_json_get(report, "captures");
  const VkrBakeryJson *capture = NULL;
  uint32_t matches = 0u;
  for (const VkrBakeryJson *item = captures ? captures->first : NULL; item;
       item = item->next) {
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(item, "channel"),
                                  "final_color")) {
      capture = item;
      matches += 1u;
    }
  }
  if (matches != 1u) {
    return vkr_bake_fail(bake, "Material snapshot must contain one final-color "
                               "capture");
  }
  int64_t width = 0;
  int64_t height = 0;
  if (!vkr_bake_int(vkr_bakery_json_get(capture, "width"), &width) ||
      !vkr_bake_int(vkr_bakery_json_get(capture, "height"), &height) ||
      width != size || height != size) {
    return vkr_bake_fail(bake, "Material snapshot has unexpected dimensions");
  }
  char png[VKR_BAKE_PATH];
  char digest[72];
  VKR_BAKE_TRY(vkr_bake_confined(bake, run_root,
                                 vkr_bake_text(capture, "data_path"),
                                 "Preview image", png));
  if (!vkr_bake_digest(bake, png, digest) ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(capture, "data_sha256"),
                                 digest)) {
    return vkr_bake_fail(bake, "Material snapshot image digest mismatch");
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(png, 64ull * 1024ull * 1024ull, &data, &length)) {
    return vkr_bake_fail(bake, "Cannot read %s", png);
  }
  static const uint8_t signature[8] = {0x89, 'P',  'N',  'G',
                                       '\r', '\n', 0x1a, '\n'};
  const bool8_t valid =
      length >= 24u && MemCompare(data, signature, 8u) == 0 &&
      MemCompare(data + 12u, "IHDR", 4u) == 0 &&
      (((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
       ((uint32_t)data[18] << 8) | data[19]) == (uint32_t)size &&
      (((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
       ((uint32_t)data[22] << 8) | data[23]) == (uint32_t)size;
  char directory[VKR_BAKE_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), output);
  const bool8_t written = valid && vkr_bakery_make_directories(directory) &&
                          vkr_bakery_write_file_atomic(output, data, length);
  free(data);
  if (!valid) {
    return vkr_bake_fail(bake, "Material snapshot is not the requested "
                               "bounded PNG");
  }
  return written ? true_v : vkr_bake_fail(bake, "Cannot write %s", output);
}

vkr_internal bool8_t vkr_preview_render(VkrBake *bake,
                                        const VkrPreviewArgs *args) {
  Arena *arena = bake->arena;
  char workspace[VKR_BAKE_PATH];
  char material[VKR_BAKE_PATH];
  char output[VKR_BAKE_PATH];
  if (!vkr_bake_resolve(args->workspace, workspace) ||
      !vkr_bakery_is_directory(workspace)) {
    return vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'",
                         args->workspace);
  }
  VkrBakeryJson *marker = vkr_bake_load(
      bake, vkr_bake_printf(bake, "%s/workspace.json", workspace), "workspace");
  int64_t version = 0;
  if (!marker ||
      !vkr_bake_int(vkr_bakery_json_get(marker, "version"), &version) ||
      version != 1) {
    bake->failed = false_v;
    return vkr_bake_fail(bake, "Select a supported initialized workspace");
  }
  const uint64_t input_length = strlen(args->input);
  if (!vkr_bake_resolve(args->input, material) ||
      !vkr_bakery_is_file(material) ||
      !vkr_bake_is_under(material, workspace) || input_length < 3u ||
      strcmp(material + strlen(material) - 3u, ".mt") != 0) {
    return vkr_bake_fail(bake, "Material preview input must be a managed .mt "
                               "file");
  }
  char cache[VKR_BAKE_PATH];
  (void)snprintf(cache, sizeof(cache), "%s/cache/thumbnails", workspace);
  (void)vkr_bake_resolve(args->output, output);
  const uint64_t output_length = strlen(output);
  if (!vkr_bake_is_under(output, cache) || output_length < 4u ||
      strcmp(output + output_length - 4u, ".png") != 0) {
    return vkr_bake_fail(bake, "Preview output must remain in "
                               "workspace/cache/thumbnails");
  }
  char harness[VKR_BAKE_PATH];
  if (args->harness) {
    (void)vkr_bake_resolve(args->harness, harness);
  } else {
    char directory[VKR_BAKE_PATH];
    char tools[VKR_BAKE_PATH];
    vkr_bakery_path_parent(directory, sizeof(directory),
                           bake->config->self_path);
    vkr_bakery_path_parent(tools, sizeof(tools), directory);
    (void)snprintf(harness, sizeof(harness), "%s/vkr_harness", tools);
  }
  if (!vkr_bakery_is_file(harness)) {
    return vkr_bake_fail(bake, "Missing installed vkr_harness; supply its "
                               "executable path");
  }
  char id[33];
  vkr_bake_hex_id(id);
  const char *job =
      vkr_bake_printf(bake, "%s/jobs/material-preview-%.12s", workspace, id);
  if (!vkr_bakery_make_directories(job)) {
    return vkr_bake_fail(bake, "Cannot create %s", job);
  }
  char run_root[VKR_BAKE_PATH] = {0};
  const char *source = vkr_bake_printf(bake, "%s/sphere.obj", job);
  const char *bundle = vkr_bake_printf(bake, "%s/sphere", job);
  const char *mesh = vkr_bake_printf(bake, "%s/sphere.vkb", bundle);
  char import_id[33];
  vkr_bake_hex_id(import_id);
  const char *cook[] = {"tool",        "mesh",   "--input",       source,
                        "--output",    mesh,     "--bundle-root", bundle,
                        "--import-id", import_id};
  bool8_t ok =
      vkr_preview_sphere(bake, source) && vkr_bakery_make_directories(bundle) &&
      vkr_preview_run(bake, bake->config->self_path, cook, ArrayCount(cook),
                      workspace, vkr_bake_printf(bake, "%s/cook.log", job)) &&
      vkr_preview_bind_material(bake, mesh, material, bundle);
  const char *scene_path = vkr_bake_printf(bake, "%s/preview.scene.json", job);
  const char *case_path = vkr_bake_printf(bake, "%s/case.json", job);
  const char *profile_path = vkr_bake_printf(bake, "%s/profile.json", job);
  if (ok) {
    VkrBakeryJson *scene = vkr_preview_parse(
        bake, "{\"version\": 2, \"environment\": {\"enabled\": true, "
              "\"intensity\": 1, \"diffuse_intensity\": 1, "
              "\"specular_intensity\": 1}, \"reflection_probes\": [], "
              "\"entities\": [{\"name\": \"Canonical sphere\", \"parent\": "
              "null, \"transform\": {\"pos\": [0, 0, 0], \"rot\": [0, 0, 0, "
              "1], \"scale\": [1, 1, 1]}, \"mesh\": {\"path\": \"\", "
              "\"pipeline_domain\": \"world\"}}]}");
    const float64_t ambient[3] = {vkr_preview_ambient, vkr_preview_ambient,
                                  vkr_preview_ambient};
    vkr_bakery_json_set(arena, vkr_bakery_json_get(scene, "environment"),
                        "constant", vkr_preview_vector(arena, ambient, 3u));
    VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
    vkr_bakery_json_set(arena, vkr_bakery_json_get(entities->first, "mesh"),
                        "path", vkr_bakery_json_cstr(arena, mesh));
    vkr_preview_softbox_entities(bake, entities);
    VkrBakeryJson *document = vkr_preview_case(
        bake, vkr_bake_relative(bake, workspace, scene_path), args->size);
    ok = document && vkr_bake_write_json(bake, scene_path, scene, false_v) &&
         vkr_bake_write_json(bake, case_path, document, false_v) &&
         vkr_bake_write_json(bake, profile_path,
                             vkr_preview_parse(bake, vkr_preview_profile),
                             false_v);
  }
  const char *log = vkr_bake_printf(bake, "%s/render.log", job);
  if (ok) {
    const char *render[] = {"snapshot",
                            "--repo-root",
                            workspace,
                            "--case",
                            vkr_bake_relative(bake, workspace, case_path),
                            "--profile",
                            vkr_bake_relative(bake, workspace, profile_path)};
    ok = vkr_preview_run(bake, harness, render, ArrayCount(render), workspace,
                         log);
  }
  VkrBakeryJson *publication = ok ? vkr_bake_publication(bake, log) : NULL;
  if (ok && (!publication || !vkr_bake_text(publication, "sha256"))) {
    ok = vkr_bake_fail(bake, "Material snapshot did not publish a verified "
                             "report");
  }
  char report_path[VKR_BAKE_PATH];
  char digest[72];
  VkrBakeryJson *report = NULL;
  if (ok) {
    ok =
        vkr_bake_confined(bake, workspace, vkr_bake_text(publication, "report"),
                          "Preview report", report_path);
    if (ok && (!vkr_bake_digest(bake, report_path, digest) ||
               !vkr_bakery_json_is_string(
                   vkr_bakery_json_get(publication, "sha256"), digest))) {
      ok = vkr_bake_fail(bake, "Material snapshot report digest mismatch");
    }
    ok = ok &&
         (report = vkr_bake_load(bake, report_path, "Preview report")) != NULL;
  }
  if (ok) {
    vkr_bakery_path_parent(run_root, sizeof(run_root), report_path);
    if (!vkr_bakery_json_is_string(vkr_bakery_json_get(report, "status"),
                                   "pass") ||
        !vkr_bakery_json_is_string(
            vkr_bakery_json_get(vkr_bakery_json_get(report, "case"), "id"),
            "local.material_preview")) {
      ok = vkr_bake_fail(bake, "Material snapshot did not pass for the "
                               "requested preview");
    }
  }
  if (ok) {
    VkrBakeryJson *manifest = vkr_bake_load(
        bake, vkr_bake_printf(bake, "%s/scene-content-manifest.json", run_root),
        "Scene-content manifest");
    char resolved_scene[VKR_BAKE_PATH];
    (void)vkr_bake_resolve(scene_path, resolved_scene);
    ok = manifest &&
         vkr_bake_scene_manifest(bake, manifest, resolved_scene, workspace,
                                 vkr_bakery_json_object(arena)) &&
         vkr_preview_publish(bake, run_root, report, args->size, output);
  }
  if (ok) {
    char output_digest[72];
    (void)vkr_bake_digest(bake, output, output_digest);
    printf("{\"status\":\"ready\",\"recipe\":\"" VKR_PREVIEW_RECIPE
           "\",\"output\":\"%s\",\"sha256\":\"%s\"}\n",
           output, output_digest);
  }
  const bool8_t cancelled = vkr_bake_is_cancelled(NULL);
  if (ok || cancelled) {
    (void)vkr_bakery_remove_tree(job);
    char snapshots[VKR_BAKE_PATH];
    (void)snprintf(snapshots, sizeof(snapshots), "%s/build/_artifacts/snapshot",
                   workspace);
    if (run_root[0] && vkr_bake_is_under(run_root, snapshots)) {
      (void)vkr_bakery_remove_tree(run_root);
    }
  } else {
    fprintf(stderr, "Material preview diagnostic job: %s\n", job);
  }
  return ok;
}

typedef struct VkrPreviewEntry {
  int64_t mtime_ns;
  uint64_t size;
  char name[64];
} VkrPreviewEntry;

typedef struct VkrPreviewListing {
  VkrPreviewEntry *entries;
  uint32_t count;
  uint32_t capacity;
  uint32_t visited;
  const char *directory;
  bool8_t overflow;
} VkrPreviewListing;

/* [0-9a-f]{16}-(128|256).png, optionally followed by .log. */
vkr_internal bool8_t vkr_preview_cache_name(const char *name) {
  for (uint32_t i = 0u; i < 16u; ++i) {
    if (!((name[i] >= '0' && name[i] <= '9') ||
          (name[i] >= 'a' && name[i] <= 'f'))) {
      return false_v;
    }
  }
  const char *rest = name + 16;
  if (strncmp(rest, "-128.png", 8u) != 0 &&
      strncmp(rest, "-256.png", 8u) != 0) {
    return false_v;
  }
  return rest[8] == 0 || strcmp(rest + 8, ".log") == 0;
}

vkr_internal bool8_t vkr_preview_visit(void *context, const char *name,
                                       bool8_t is_directory) {
  VkrPreviewListing *listing = (VkrPreviewListing *)context;
  if (++listing->visited > 65536u) {
    listing->overflow = true_v;
    return false_v;
  }
  if (is_directory || strlen(name) >= 64u || !vkr_preview_cache_name(name)) {
    return true_v;
  }
  char path[VKR_BAKE_PATH];
  VkrBakeryStat info;
  if (!vkr_bakery_path_join(path, sizeof(path), listing->directory, name) ||
      !vkr_bakery_stat(path, &info) || info.is_directory) {
    return true_v;
  }
#if !defined(_WIN32)
  struct stat link_info;
  if (lstat(path, &link_info) != 0 || S_ISLNK(link_info.st_mode)) {
    return true_v;
  }
#endif
  if (listing->count == listing->capacity) {
    const uint32_t capacity = listing->capacity ? listing->capacity * 2u : 64u;
    VkrPreviewEntry *grown = (VkrPreviewEntry *)realloc(
        listing->entries, capacity * sizeof(VkrPreviewEntry));
    if (!grown) {
      listing->overflow = true_v;
      return false_v;
    }
    listing->entries = grown;
    listing->capacity = capacity;
  }
  VkrPreviewEntry *entry = &listing->entries[listing->count++];
  entry->mtime_ns = info.mtime_ns;
  entry->size = info.size;
  (void)snprintf(entry->name, sizeof(entry->name), "%s", name);
  return true_v;
}

vkr_internal int vkr_preview_compare(const void *lhs, const void *rhs) {
  const VkrPreviewEntry *a = (const VkrPreviewEntry *)lhs;
  const VkrPreviewEntry *b = (const VkrPreviewEntry *)rhs;
  if (a->mtime_ns != b->mtime_ns) {
    return a->mtime_ns < b->mtime_ns ? -1 : 1;
  }
  return strcmp(a->name, b->name);
}

/* Evicts the oldest completed thumbnails until the cache keeps 1 MiB free
   for the incoming bounded PNG and its short diagnostic. */
vkr_internal bool8_t vkr_preview_prune(VkrBake *bake, const char *value,
                                       uint64_t limit) {
  char directory[VKR_BAKE_PATH];
  char parent[VKR_BAKE_PATH];
  if (!vkr_bake_resolve(value, directory) ||
      !vkr_bakery_is_directory(directory)) {
    return vkr_bake_fail(bake, "[Errno 2] No such file or directory: '%s'",
                         value);
  }
  vkr_bakery_path_parent(parent, sizeof(parent), directory);
  if (strcmp(vkr_bakery_path_name(directory), "thumbnails") != 0 ||
      strcmp(vkr_bakery_path_name(parent), "cache") != 0) {
    return vkr_bake_fail(bake, "Expected a managed cache/thumbnails directory");
  }
  VkrPreviewListing listing = {.directory = directory};
  (void)vkr_bakery_list_directory(directory, vkr_preview_visit, &listing);
  if (listing.overflow) {
    free(listing.entries);
    return vkr_bake_fail(bake, "Thumbnail cache enumeration exceeds 65536 "
                               "entries");
  }
  uint64_t total = 0u;
  for (uint32_t i = 0u; i < listing.count; ++i) {
    total += listing.entries[i].size;
  }
  if (listing.count > 1u) {
    qsort(listing.entries, listing.count, sizeof(VkrPreviewEntry),
          vkr_preview_compare);
  }
  const uint64_t budget = limit - 1024u * 1024u;
  for (uint32_t i = 0u; i < listing.count && total > budget; ++i) {
    char path[VKR_BAKE_PATH];
    (void)vkr_bakery_path_join(path, sizeof(path), directory,
                               listing.entries[i].name);
    (void)vkr_bakery_remove_file(path);
    total -= listing.entries[i].size;
  }
  free(listing.entries);
  return true_v;
}

int vkr_bakery_preview_main(const VkrBakeryConfig *config, int argc,
                            char **argv) {
  if (argc < 1 || (strcmp(argv[0], "material") && strcmp(argv[0], "prune"))) {
    fprintf(stderr, "usage: vkr_bakery preview material|prune [options]\n");
    return 2;
  }
  VkrBake bake = {.config = config};
  bake.arena = arena_create(MB(256), MB(1));
  if (!bake.arena) {
    fprintf(stderr, "Out of memory\n");
    return 1;
  }
  (void)vkr_bake_resolve(vkr_content_root(), bake.repo);
  vkr_bakery_install_cancel_signals();
  VkrPreviewArgs args = {.size = 128};
  const char *directory = NULL;
  int64_t limit_mib = 512;
  int code = 0;
  for (int i = 1; i < argc && code == 0; ++i) {
    const char *flag = argv[i];
    bool8_t ok = true_v;
    if (!strcmp(flag, "--input")) {
      ok = (args.input = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--output")) {
      ok = (args.output = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--workspace")) {
      ok = (args.workspace = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--harness")) {
      ok = (args.harness = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--size")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &args.size) &&
           (args.size == 128 || args.size == 256);
    } else if (!strcmp(flag, "--directory")) {
      ok = (directory = vkr_bake_value(argv, argc, &i)) != NULL;
    } else if (!strcmp(flag, "--limit-mib")) {
      ok = vkr_bake_parse_integer(argv, argc, &i, &limit_mib) &&
           limit_mib >= 8 && limit_mib <= 4096;
    } else {
      ok = false_v;
    }
    if (!ok) {
      fprintf(stderr, "preview %s: invalid argument %s\n", argv[0], flag);
      code = 2;
    }
  }
  if (code == 0 && strcmp(argv[0], "prune") == 0) {
    if (!directory) {
      fprintf(stderr, "preview prune: --directory is required\n");
      code = 2;
    } else if (!vkr_preview_prune(&bake, directory,
                                  (uint64_t)limit_mib * 1024u * 1024u)) {
      fprintf(stderr, "Thumbnail cache prune failed: %s\n", bake.error);
      code = 1;
    }
  } else if (code == 0) {
    if (!args.input || !args.output || !args.workspace) {
      fprintf(stderr, "preview material: --input, --output and --workspace "
                      "are required\n");
      code = 2;
    } else if (!vkr_preview_render(&bake, &args)) {
      fprintf(stderr, "Material preview failed: %s\n", bake.error);
      code = 1;
    }
  }
  fflush(stdout);
  arena_destroy(bake.arena);
  return code;
}
